#!/usr/bin/env python3
"""Integration test: the block-update relay's audience is the chunk's viewers.

Every block write in the server — a command, a break, a fluid tick, a redstone
edge, a fire spreading, a blast's cells — ends in `broadcast_block_update`, and
that packet is the largest clientbound population this engine has. Its audience
is not a distance in blocks: it is the set of clients that *hold the chunk*
(`player_views_chunk`, the same square `stream_chunks` sends and prunes on),
because a client that was never sent the chunk has nothing to apply the update
to. That rule replaced a fixed-range broadcast which sent updates to clients
hundreds of blocks away, and until this test it had no wire pin of its own —
the entity relays have three (`mob-visibility`, `item-visibility`,
`entity-relay-visibility`) and the block relay had none.

Two clients on the committed flat capture, view distance 3 (a 7x7-chunk square,
48 blocks from the centre), natural spawning off, the day cycle frozen:

  * A and B stand in chunk (0, 0). A's `/setblock` in that chunk reaches both:
    the baseline that says the relay works at all.
  * B walks to chunk (5, 0), 80 blocks east. The next `/setblock` in chunk
    (0, 0) — air, so the relight has something to say — reaches A and **not**
    B; the same command's `light_update` for the chunk reaches A and not B
    either. The same cell, the same command, so the only thing that changed is
    whether B holds the chunk.
  * B walks back into chunk (0, 0). The next update reaches it again, which is
    what tells "withheld because it left the view" from "withheld because the
    relay broke".

The test reads `block_update` (0x08) and `light_update` (0x30), and asserts the
*cell* (and the chunk) the command named: a write in one chunk must not be
inferred from whatever else the stream carried.

Run: block_update_views_test.py /path/to/plusmc
"""
import os
import pathlib
import shutil
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import tnt_wire_test as tnt  # noqa: E402  (Bot, drain, chat commands, decoders)
import wire_client  # noqa: E402

HOST = tnt.HOST
# Clientbound light_update (0x30): chunk x, chunk z, then the four bitsets. It
# rides the same player_views_chunk rule as the block update, so the far client
# must not get it either.
CB_LIGHT_UPDATE = 0x31
VIEW_DISTANCE = 3
# The flat capture's surface, and the cell every command writes.
A_AT = (8.5, -60.0, 8.5)
B_AT = (11.5, -60.0, 8.5)  # the same chunk (0, 0)
CELL = (8, -60, 8)
# Five chunks east of the cell: outside the 3-chunk view square, so the client
# has been told to forget chunk (0, 0) and must not be sent its updates.
FAR = (88.5, -60.0, 8.5)


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    fixtures = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
    world = tempfile.mkdtemp(prefix="plusmc-blockview-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_MOB_SPAWNING"] = "0"
    # The join's chunk burst is not what this test measures; one batch a tick
    # keeps B's re-entry from being read as a slow relay.
    env["PLUSMC_CHUNKS_PER_TICK"] = "1000"

    proc = None
    a = b = None
    try:
        if not wire_client.replaying():
            region_dir = os.path.join(world, "dimensions", "minecraft", "overworld", "region")
            os.makedirs(region_dir, exist_ok=True)
            shutil.copyfile(os.path.join(fixtures, "r.0.0.mca"),
                            os.path.join(region_dir, "r.0.0.mca"))
            tnt.step(f"world dir: {world} (flat fixture, view distance {VIEW_DISTANCE})")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "block-view test",
             "--offline-mode", "--gamemode", "1", "--view-distance", str(VIEW_DISTANCE),
             "--data", data_root, "--world", world],
            tnt.wait_listening, env=env)

        a = tnt.Bot("ViewA", port)
        wire_client.record(a, note="block-update views: the player who stays")
        a.join()
        a.start_reader()
        a.move_to(*A_AT)
        tnt.drain(a, 0.6)

        b = tnt.Bot("ViewB", port)
        wire_client.record(b, note="block-update views: the player who leaves the chunk")
        b.join()
        b.start_reader()
        b.move_to(*B_AT)
        tnt.drain(b, 0.6)

        def updates_at(packets, cell):
            """The state ids `block_update` carried for one cell, in order."""
            return [state for at, state in
                    (tnt.read_block_update(body) for pid, body in packets
                     if pid == tnt.CB_BLOCK_UPDATE)
                    if at == cell]

        def setblock(block):
            tnt.step(f"-> /setblock {CELL[0]} {CELL[1]} {CELL[2]} {block}")
            tnt.send_chat_command(a, f"setblock {CELL[0]} {CELL[1]} {CELL[2]} {block}")
            # A's reply and both clients' updates: the command runs on the
            # session thread, so one second is ample for the broadcast (the
            # relight is budgeted at two chunks a tick).
            return tnt.drain(a, 1.0), tnt.drain(b, 1.0)

        def lit_chunks(packets):
            """The (cx, cz) every light_update in a window names."""
            out = set()
            for pid, body in packets:
                if pid != CB_LIGHT_UPDATE:
                    continue
                cx, at = tnt.rd_varint(body, 0)
                cz, _ = tnt.rd_varint(body, at)
                out.add((cx, cz))
            return out

        # --- 1. both hold the chunk ------------------------------------------
        tnt.step("-> both clients stand in chunk (0, 0): an update reaches both")
        a_seen, b_seen = setblock("stone")
        a_states = updates_at(a_seen, CELL)
        b_states = updates_at(b_seen, CELL)
        assert a_states, (f"A, whose chunk the cell is in, never got the update: "
                          f"{[hex(p) for p, _ in a_seen]}")
        assert b_states, (f"B holds the same chunk, so it must get the update too: "
                          f"{[hex(p) for p, _ in b_seen]}")
        assert a_states[-1] != 0 and b_states[-1] != 0, \
            f"stone is not air: A {a_states}, B {b_states}"
        tnt.step(f"  OK both saw cell {CELL} change to state {a_states[-1]}")

        # --- 2. B leaves the view --------------------------------------------
        tnt.step(f"-> B walks to {FAR[0]:.1f} (chunk 5, outside the 3-chunk square)")
        b.move_to(*FAR)
        tnt.drain(b, 1.2)
        # Air rather than another solid block: the cell had stone, so taking it
        # away moves the sky light as well, which is what makes the relight send
        # a light_update for the chunk. Both fan-outs ride player_views_chunk.
        a_seen, b_seen = setblock("air")
        a_states = updates_at(a_seen, CELL)
        b_states = updates_at(b_seen, CELL)
        assert a_states, "A still holds the chunk and must keep getting its updates"
        assert a_states[-1] == 0, f"the cell was set to air: A got {a_states}"
        assert not b_states, (
            f"B no longer holds chunk (0, 0) but was sent its update ({b_states}); "
            f"the audience is not the chunk's viewers. B's ids: "
            f"{[hex(p) for p, _ in b_seen]}")
        assert (0, 0) in lit_chunks(a_seen), (
            "the relight's light_update for chunk (0, 0) must reach A, the client "
            f"that holds it: {sorted(lit_chunks(a_seen))}")
        assert (0, 0) not in lit_chunks(b_seen), (
            "B does not hold chunk (0, 0) but was sent its light_update: "
            f"{sorted(lit_chunks(b_seen))}")
        tnt.step("  OK A got the block update and the chunk's light update; B, five "
                 "chunks away, got neither")

        # --- 3. B comes back --------------------------------------------------
        tnt.step(f"-> B walks back to {B_AT[0]:.1f}: the chunk and its updates return")
        b.move_to(*B_AT)
        tnt.drain(b, 1.5)
        a_seen, b_seen = setblock("sand")
        b_states = updates_at(b_seen, CELL)
        assert b_states, (
            "B re-entered chunk (0, 0) and did not get the update: either the "
            "chunk was not re-sent or the relay is not following sent_chunks")
        tnt.step(f"  OK B saw state {b_states[-1]} after re-entering the chunk")
        print("\nPASS: block-update views")
        return 0
    finally:
        for bot in (a, b):
            if bot is not None:
                bot.close()
        if proc is not None:
            proc.terminate()
            proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
