#!/usr/bin/env python3
"""Integration test: block place/break (Milestone C).

Two bots join a flat world. Bot1 places a stone block at (0, 4, 1) and
verifies the server acks the sequence. Bot2 verifies it receives the
block update. Bot1 then breaks the block and both verify the update.
"""
import os, socket, struct, sys, time
import wire_client
from wire_client import make_frame, read_packet, recv_exact

# ── helpers ──────────────────────────────────────────────────────────────────

PROTO = 777
HOST = "127.0.0.1"

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F; n >>= 7
        if n: b |= 0x80
        out.append(b)
        if not n: return bytes(out)

def rd_varint(buf, pos=0):
    val, shift = 0, 0
    while pos < len(buf):
        b = buf[pos]; pos += 1
        val |= (b & 0x7F) << shift; shift += 7
        if not (b & 0x80): return val, pos
    return None, pos

def rd_string(buf, pos=0):
    ln, pos = rd_varint(buf, pos)
    return buf[pos:pos+ln].decode("utf-8"), pos+ln

def recv_varint(sock):
    result, shift = 0, 0
    for _ in range(5):
        b = recv_exact(sock, 1)[0]
        result |= (b & 0x7F) << shift; shift += 7
        if not (b & 0x80): return result
    raise ValueError("VarInt too long")

def position(x, y, z):
    # Vanilla protocol: x=bits[63:38], z=bits[37:12], y=bits[11:0]
    return wire_client.pack_block_pos(x, y, z)

def parse_position(raw):
    v = struct.unpack(">q", raw)[0]
    x = v >> 38
    y = v & 0xFFF
    z = (v >> 12) & 0x3FFFFFF
    if x >= 0x2000000: x -= 0x4000000
    if y >= 0x800: y -= 0x1000  # 12-bit sign extend (matches server)
    if z >= 0x2000000: z -= 0x4000000
    return x, y, z

# ── client ───────────────────────────────────────────────────────────────────

class Bot:
    """A minimal Minecraft client that performs the full offline join."""

    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(10)
        self.threshold = -1
        # The read side goes to a thread at the end of the join (`start_reader`):
        # a drain window that reads the socket directly can lose a packet it has
        # half-taken when the window closes, and every packet after it is then
        # garbage. See tests/wire_client.py.
        self.reader = wire_client.Reader(self.sock, self)

    def send(self, pkt_id, body=b""):
        self.reader.send(pkt_id, body)

    def recv(self):
        return read_packet(self.sock, self.threshold)

    def start_reader(self):
        self.reader.start()

    def close(self):
        try: self.sock.close()
        except OSError: pass

    def move_to(self, x, y, z, ground=1):
        """ServerboundMovePlayerPos (0x1e): feet x/y/z doubles + on-ground flag.

        A real client sends this every tick, and the server measures its
        interaction ranges from where it says it is (Player.isWithinBlockInteraction
        Range), so a bot that wants to place or dig has to stand there first.
        """
        self.send(0x1e, struct.pack(">ddd", x, y, z) + bytes([ground]))

    def join(self):
        """Run handshake + login + configuration. Blocks until play phase."""
        # Handshake
        hs = (varint(0) + varint(PROTO) + varint(len(HOST)) + HOST.encode()
              + struct.pack(">H", 0) + varint(2))
        self.sock.sendall(make_frame(hs, -1))

        # Login Start: [name string][uuid 16 bytes]
        self.send(0x00, varint(len(self.name)) + self.name.encode() + b"\x00" * 16)

        # --- login phase: read Set Compression + Login Success ---
        while True:
            pid, body = self.recv()
            if pid == 0x03:  # Set Compression
                self.threshold, _ = rd_varint(body)
            elif pid == 0x02:  # Login Success
                self.send(0x03)  # Login Acknowledged
                break
            else:
                raise AssertionError(f"unexpected login packet 0x{pid:02x}")

        # --- configuration phase ---
        while True:
            pid, body = self.recv()
            if pid == 0x0F:  # Select Known Packs → echo back the packs
                npacks, i = rd_varint(body)
                packs = []
                for _ in range(npacks):
                    ns, i = rd_string(body, i)
                    pk, i = rd_string(body, i)
                    ver, i = rd_string(body, i)
                    packs.append((ns, pk, ver))
                out = varint(len(packs))
                for ns, pk, ver in packs:
                    out += (varint(len(ns)) + ns.encode() + varint(len(pk)) + pk.encode()
                            + varint(len(ver)) + ver.encode())
                self.send(0x07, out)
            elif pid == 0x03:  # Finish Configuration → ack
                self.send(0x03)
                break
            # all other config packets (brand, feature flags, registry, tags) — ignored

        # --- play phase: read Join Game, then drain spawn chunks ---
        while True:
            pid, body = self.recv()
            if pid == 0x32:  # Join Game
                break

        # Drain post-join stream: keep-alives, chunks, chunk batch, etc.
        batch_started = False
        for _ in range(400):
            pid, body = self.recv()
            if pid == 0x2D:  # Keep Alive — must respond or get kicked
                self.send(0x1C, body[:8])
            elif pid == 0x0C:  # Chunk Batch Start
                batch_started = True
            elif pid == 0x2E:  # Chunk Data
                pass  # accept
            elif pid == 0x0B:  # Chunk Batch Finished — done
                batch_size, _ = rd_varint(body)
                self.send(0x0B, varint(batch_size))  # Chunk Batch Received
                break

        # The join's own reads are done; from here the reader thread collects.
        self.start_reader()
        return True


def drain_packets(bot, timeout=1.0):
    """Every packet the reader queued in the next `timeout` seconds."""
    return wire_client.drain(bot, timeout)


def find_packet(packets, target_pid):
    for pid, body in packets:
        if pid == target_pid:
            return body
    return None


# ── main test ────────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")

    # Start the server
    import subprocess, pathlib, re
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "1200"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "4000"
    # The survival tick counts packets — one recognized packet is one tick, and
    # idle wall time contributes none — so no step below can be paced by how
    # long a drain happened to run. This file asserts block states and menu
    # contents, never a survival-tick duration (the bot is creative, so nothing
    # is burning, starving or on cooldown); pinning the clock is what keeps that
    # true rather than accidental.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    def wait_listening(proc, timeout=20):
        """The port from the server's own "listening on host:port" line."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = proc.stdout.readline().decode(errors="replace")
            if line:
                sys.stderr.write(line)
                m = re.search(r"listening on.*:(\d+)", line)
                if m:
                    return int(m.group(1))
        print("FAIL: server did not start", file=sys.stderr)
        return None

    proc, port = wire_client.start_server(
        [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "block test",
         "--offline-mode", "--gamemode", "1", "--data", data_root],
        wait_listening, env=env)
    if port is None:
        proc.kill(); proc.wait(); return 1

    # Wait for port to be reachable (nothing to reach when replaying).
    if not wire_client.replaying():
        for _ in range(20):
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=1)
                s.close(); break
            except OSError:
                time.sleep(0.1)

    try:
        # Connect bot1
        print("Connecting bot1...")
        bot1 = Bot("BlockBot1", port)
        wire_client.record(bot1, note="block-interact: place, break, reach and cursors")
        bot1.join()
        print(f"  bot1 joined (threshold={bot1.threshold})")

        # Drain initial packets (chunks, etc.)
        drain_packets(bot1, timeout=1.0)

        # Stand beside the blocks this test places and digs. The server's reach
        # check is Player.isWithinBlockInteractionRange(pos, 1.0) — 5.5 blocks
        # from the eye to the block's box — so "wherever the bot happens to be"
        # no longer works: the placements below are at (0, 4, 1) and (0, 4, 3),
        # and the world under them is void, so the bot stands on the y = 5 level
        # beside them, exactly as a client that had walked over and built would.
        bot1.move_to(0.5, 5.0, 1.5)
        time.sleep(0.2)

        # Bot1: put stone in hotbar slot 0 via the creative inventory.
        # ServerboundSetCreativeModeSlot (0x38) is [slot i16][ItemStack]
        # where the slot is a *player menu* slot: vanilla accepts 1..45, and
        # menu slot 36 is hotbar slot 0. The stack is
        # [count VarInt][item id VarInt][DataComponentPatch] — an empty patch is
        # two zero varints (nothing added, nothing removed).
        creative_pkt = (struct.pack(">h", 36) + varint(64) + varint(1) + b"\x00\x00")
        bot1.send(0x39, creative_pkt)
        time.sleep(0.2)

        # Bot1: place stone at (0, 4, 1)
        # ServerboundUseItemOn (0x42):
        #   [hand VarInt=0][position i64][face VarInt=1(up)][cursorX f32]
        #   [cursorY f32][cursorZ f32][inside bool][worldBorder bool][sequence VarInt]
        place_pkt = (varint(0) + position(0, 4, 1) + varint(1)
                     + struct.pack(">f", 0.5) + struct.pack(">f", 0.0) + struct.pack(">f", 0.5)
                     + bytes([0]) + bytes([0]) + varint(1))
        bot1.send(0x42, place_pkt)

        # Bot1: read responses
        packets1 = drain_packets(bot1, timeout=2.0)
        block_update = find_packet(packets1, 0x08)
        block_ack = find_packet(packets1, 0x04)

        if block_update:
            bx, by, bz = parse_position(block_update[:8])
            state_id, _ = rd_varint(block_update, 8)
            print(f"  bot1 got block_update: ({bx},{by},{bz}) state={state_id}")
            assert bx == 0 and bz == 1, f"Wrong x/z: ({bx},{by},{bz})"
            assert state_id != 0, f"Block should be stone (id!=0), got {state_id}"
            placed_y = by  # remember for break test
        else:
            print("  FAIL: bot1 did not receive block_update after place")
            print(f"  packets received: {[(hex(p), len(b)) for p,b in packets1]}")
            return 1

        if block_ack:
            ack_seq, _ = rd_varint(block_ack, 0)
            print(f"  bot1 got block_changed_ack: seq={ack_seq}")
            assert ack_seq == 1, f"Wrong sequence: {ack_seq}"
        else:
            print("  FAIL: bot1 did not receive block_changed_ack")
            return 1

        # ...and a cell at a *negative* coordinate keeps its sign on the wire.
        # BlockPos packs x | z | y into one i64 and z sits in the middle 26 bits:
        # a reader that masks z (`& 0x3FFFFFF`) without putting the sign back
        # reads -1 as 67108863, a cell sixty-seven million blocks away — while
        # the packet itself is right, because the server's own `decode_block_pos`
        # sign-extends x, z and y. This step is the wire-level check for that:
        # the reader below must agree with the shared codec, cell for cell.
        print("-> a placement at a negative coordinate keeps its sign on the wire")
        place_neg = (varint(0) + position(-1, 4, -1) + varint(1)
                     + struct.pack(">f", 0.5) + struct.pack(">f", 0.0) + struct.pack(">f", 0.5)
                     + bytes([0]) + bytes([0]) + varint(2))
        bot1.send(0x42, place_neg)
        packets_neg = drain_packets(bot1, timeout=1.5)
        neg = find_packet(packets_neg, 0x08)
        assert neg is not None, ("a placement at (-1, 4, -1) must answer with a block "
                                 f"update: {[(hex(p), len(b)) for p, b in packets_neg]}")
        raw = struct.unpack(">q", neg[:8])[0]
        nx, ny, nz = parse_position(neg[:8])
        state_neg, _ = rd_varint(neg, 8)
        print(f"  packed {raw:#018x} -> ({nx},{ny},{nz}) state={state_neg}, and the "
              f"shared codec says {wire_client.unpack_block_pos(raw)}")
        assert (nx, nz) == (-1, -1), (
            f"the update must name the clicked cell with its sign: got ({nx},{ny},{nz}); "
            f"a masked read is ({raw >> 38},{raw & 0xFFF},{(raw >> 12) & 0x3FFFFFF})")
        assert parse_position(neg[:8]) == wire_client.unpack_block_pos(raw), (
            f"this file's decoder and the shared one must agree: "
            f"{parse_position(neg[:8])} vs {wire_client.unpack_block_pos(raw)}")
        assert state_neg != 0, "the negative-coordinate placement is not stone"
        print("  OK negative x and z survive the wire and both decoders")

        # The block comes from the *selected* hotbar slot: put dirt in hotbar
        # slot 1 (menu slot 37), select it with set_carried_item, and place
        # again. Before Phase 3 the server kept a single global "held item" and
        # ignored the selection entirely, so this would still place stone.
        print("-> hotbar slot 1 (dirt) must override slot 0 (stone)")
        # stone in slot 0 first, so slot 0 is definitely not empty
        bot1.send(0x39, struct.pack(">h", 36) + varint(64) + varint(1) + b"\x00\x00")
        # dirt (item id 55) in slot 1
        bot1.send(0x39, struct.pack(">h", 37) + varint(64) + varint(55) + b"\x00\x00")
        time.sleep(0.2)
        # ServerboundSetCarriedItem is 0x36 in 777 (0x2e is recipe_book_change_
        # settings — the id shifts because the play protocol interleaves the
        # common/cookie/ping packets; see src/protocol/packet_table.cpp).
        bot1.send(0x36, struct.pack(">h", 1))
        time.sleep(0.2)
        place2 = (varint(0) + position(0, 4, 3) + varint(1)
                  + struct.pack(">f", 0.5) + struct.pack(">f", 0.0) + struct.pack(">f", 0.5)
                  + bytes([0]) + bytes([0]) + varint(3))
        bot1.send(0x42, place2)
        packets_slot = drain_packets(bot1, timeout=2.0)
        upd = find_packet(packets_slot, 0x08)
        assert upd, ("placing from the selected slot produced no block update: "
                     f"{[(hex(p), len(b)) for p, b in packets_slot]}")
        sx, sy, sz = parse_position(upd[:8])
        state2, _ = rd_varint(upd, 8)
        # minecraft:stone is state id 1; dirt places as id 10 (see
        # src/world/block_states.h). Any nonzero id that is not stone proves the
        # selection was honoured.
        assert (sx, sz) == (0, 3), f"wrong placement position: ({sx},{sy},{sz})"
        assert state2 != 1, f"slot 1 should place dirt, not stone (state {state2})"
        print(f"  OK: slot 1 placed state {state2} (a different block than slot 0's stone)")
        # Put the selection back so the rest of the test is unchanged.
        bot1.send(0x36, struct.pack(">h", 0))
        time.sleep(0.2)

        # --- the interaction ranges (the serverbound packet audit) ----------
        # A placement 40 blocks from the eye must change nothing. The server
        # still acks the sequence — vanilla acks before it checks the range
        # — so the client's pending prediction is answered rather than left to
        # hang; what it must not do is place the block.
        print("-> a placement out of reach must be refused")
        far_place = (varint(0) + position(40, 4, 1) + varint(1)
                     + struct.pack(">f", 0.5) + struct.pack(">f", 0.0) + struct.pack(">f", 0.5)
                     + bytes([0]) + bytes([0]) + varint(7))
        bot1.send(0x42, far_place)
        packets_far = drain_packets(bot1, timeout=1.5)
        assert not find_packet(packets_far, 0x08), (
            "a placement 40 blocks away must not land: "
            f"{[(hex(p), len(b)) for p, b in packets_far]}")
        far_ack = find_packet(packets_far, 0x04)
        assert far_ack is not None, "a refused placement must still be acked"
        far_seq, _ = rd_varint(far_ack, 0)
        assert far_seq == 7, f"the ack must carry the refused sequence, got {far_seq}"
        print("  OK out-of-reach placement refused (no block update, sequence acked)")

        # ...and the cursor check: a packet that names a block right beside the
        # bot but carries a hit location 40 blocks away from it is refused too.
        # That is what stops a client naming the block it wishes it had clicked.
        print("-> a placement whose cursor is off the block must be refused")
        bad_cursor = (varint(0) + position(0, 4, 2) + varint(1)
                      + struct.pack(">f", 0.5) + struct.pack(">f", 0.0) + struct.pack(">f", 40.0)
                      + bytes([0]) + bytes([0]) + varint(8))
        bot1.send(0x42, bad_cursor)
        packets_cursor = drain_packets(bot1, timeout=1.5)
        assert not find_packet(packets_cursor, 0x08), (
            "a cursor 40 blocks off the block must be refused: "
            f"{[(hex(p), len(b)) for p, b in packets_cursor]}")
        assert find_packet(packets_cursor, 0x04) is not None, "a refused placement must be acked"
        print("  OK off-block cursor refused")

        # Connect bot2
        print("Connecting bot2...")
        bot2 = Bot("BlockBot2", port)
        wire_client.record(bot2, note="block-interact: the second client of the session")
        bot2.join()
        print(f"  bot2 joined (threshold={bot2.threshold})")
        drain_packets(bot2, timeout=1.0)

        # bot2 is the one that digs the block, so it has to be in reach of it too.
        bot2.move_to(0.5, 5.0, 1.5)
        time.sleep(0.2)

        # A dig 40 blocks away is refused the same way (ServerPlayerGameMode
        # .handleBlockBreakAction's first line), and its sequence is acked.
        print("-> a dig out of reach must be refused")
        far_dig = varint(0) + position(40, 4, 1) + varint(0) + varint(9)
        bot2.send(0x29, far_dig)
        packets_far_dig = drain_packets(bot2, timeout=1.5)
        assert not find_packet(packets_far_dig, 0x08), (
            "a dig 40 blocks away must not land: "
            f"{[(hex(p), len(b)) for p, b in packets_far_dig]}")
        assert find_packet(packets_far_dig, 0x04) is not None, "a refused dig must be acked"
        print("  OK out-of-reach dig refused")

        # Bot2: break the placed block — block_dig STOP_DESTROY_BLOCK
        # ServerboundPlayerAction (0x29): [action VarInt=2][position i64][face VarInt=0][sequence VarInt]
        break_pkt = varint(2) + position(0, placed_y, 1) + varint(0) + varint(2)
        bot2.send(0x29, break_pkt)

        # Bot2: read responses
        packets2 = drain_packets(bot2, timeout=2.0)
        block_update_b = find_packet(packets2, 0x08)
        block_ack_b = find_packet(packets2, 0x04)

        if block_update_b:
            bx, by, bz = parse_position(block_update_b[:8])
            state_id, _ = rd_varint(block_update_b, 8)
            print(f"  bot2 got block_update (break): ({bx},{by},{bz}) state={state_id}")
            assert bx == 0 and bz == 1, f"Wrong x/z: ({bx},{by},{bz})"
            assert state_id == 0, f"Block should be air (id=0), got {state_id}"
        else:
            print("  FAIL: bot2 did not receive block_update after break")
            print(f"  packets received: {[(hex(p), len(b)) for p,b in packets2]}")
            return 1

        if block_ack_b:
            ack_seq, _ = rd_varint(block_ack_b, 0)
            print(f"  bot2 got block_changed_ack (break): seq={ack_seq}")
            assert ack_seq == 2, f"Wrong sequence: {ack_seq}"
        else:
            print("  FAIL: bot2 did not receive block_changed_ack after break")
            return 1

        # Bot1 should also receive the block update from bot2's break
        packets1b = drain_packets(bot1, timeout=2.0)
        block_update_b1 = find_packet(packets1b, 0x08)
        if block_update_b1:
            bx, by, bz = parse_position(block_update_b1[:8])
            state_id, _ = rd_varint(block_update_b1, 8)
            print(f"  bot1 got block_update (break): ({bx},{by},{bz}) state={state_id}")
            assert bx == 0 and bz == 1
            assert state_id == 0, f"Block should be air, got {state_id}"
        else:
            print("  WARN: bot1 did not receive block_update after bot2 break")

        bot1.close()
        bot2.close()
    finally:
        proc.terminate()
        proc.wait(timeout=5)

    print("\nPASS: block place/break test")
    return 0

if __name__ == "__main__":
    sys.exit(main())
