#!/usr/bin/env python3
"""Integration test: the advancement store across a restart (Phase 4 milestone N).

A player's tree lives in the registry entry as long as their session does, and
the entry dies with the socket. World/advancement_save.h's file is what makes it
outlive both: a join adopts the player's rows into the fresh entry, a disconnect
copies the entry's rows back out, and each save writes the whole store to
`<world>/advancements.dat`. This test drives all three over the real wire
protocol, on a real world directory, with a real shutdown in between:

  * a join's own first tick earns the `minecraft:tick` recipe helper
    (`recipes/decorations/crafting_table`), and that first flush is the `reset`
    packet — the client's stale tree is dropped, everything the restored
    progress shows is added, and every row's criteria carry the instant they
    were obtained at;
  * `/give cobblestone 1` is the `inventory_changed` wire path and completes
    `story/mine_stone`, which reveals `story/root` with it;
  * a second player joins and earns only the tick row, then disconnects — so
    their session's own progress has to survive *their* logging off too;
  * the first player stays connected across the shutdown, so the save that ends
    the run reads a *live* entry rather than a stored one;
  * the second boot, on the same directory, hands each player back exactly their
    own rows: the first player's story branch and its obtained instants match
    what the first run sent, the second player's tree has no story node in it at
    all, and giving the same cobblestone again changes nothing (the criterion
    came back obtained, not re-granted).

The partial-progress shape of a row (an advancement whose criteria are open) is
pinned by tests/advancement_save_test.cpp, which round-trips a 1-of-8 trim and a
1-of-26 animal through the file; this test earns what the wire can earn cheaply
and checks the session's three paths end to end.

Run: advancement_persistence_test.py /path/to/plusmc
   or: PLUSMC_REPLAY=transcript.jsonl advancement_persistence_test.py  (no binary)
"""
import os
import pathlib
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import rd_nbt_translate  # noqa: E402
import wire_client  # noqa: E402
from wire_client import make_frame, read_packet  # noqa: E402

PROTO = 777
HOST = "127.0.0.1"

# Clientbound packet ids (src/protocol/packet_ids.generated.h).
CB_UPDATE_ADVANCEMENTS = 0x85
CB_KEEP_ALIVE = 0x2D

# Serverbound packet ids.
SB_CHAT_COMMAND = 0x07
SB_KEEP_ALIVE = 0x1C
SB_POSITION = 0x1E
SB_SET_CARRIED_ITEM = 0x36

# The one advancement the join's tick earns, and the two the /give does.
RECIPE_TABLE = "minecraft:recipes/decorations/crafting_table"
STORY_MINE_STONE = "minecraft:story/mine_stone"
STORY_ROOT = "minecraft:story/root"
# Everything the first run earned, so a rejoin has to hand all three back.
RESTORED_ROWS = (RECIPE_TABLE, STORY_MINE_STONE, STORY_ROOT)
# How long a rejoin is given to hand its rows back. A bound on a *failure*: an
# idle box answers in the first window, and a loaded one gets as many windows as
# it needs (see `flushes_until`).
RESTORE_TIMEOUT = 30.0

# Where a fresh spawn puts the player. The world is the void emitter (a fresh
# `--world` directory has no regions), so the player is pinned at spawn height
# before each command instead of being allowed to fall: the same trick
# mob_persistence_test.py uses to hold a bot in place.
FLOOR_Y = 63
PLAYER_X = 0.5
PLAYER_Z = 0.5

CB_DISPLAY_BACKGROUND = 1
CB_DISPLAY_TOAST = 2
CB_DISPLAY_HIDDEN = 4


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            return bytes(out)


def rd_varint(buf, pos=0):
    val, shift = 0, 0
    while pos < len(buf):
        b = buf[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            return val, pos
    return None, pos


def rd_string(buf, pos=0):
    ln, pos = rd_varint(buf, pos)
    return buf[pos:pos + ln].decode("utf-8"), pos + ln


def rd_f32(buf, pos):
    return struct.unpack(">f", buf[pos:pos + 4])[0], pos + 4


def read_display(buf, pos):
    """DisplayInfo.serializeToNetwork: [Component title][Component description]
    [ItemStackTemplate icon][varint frame][u32 flags][utf background when bit 0].
    26.3's PositionedAdvancement carries the coordinates after the holder; the
    icon is the *template* codec — item id, then count — and the display always
    carries an empty component patch."""
    title, _, pos = rd_nbt_translate(buf, pos)
    description, _, pos = rd_nbt_translate(buf, pos)
    icon, pos = rd_varint(buf, pos)
    icon_count, pos = rd_varint(buf, pos)
    added, pos = rd_varint(buf, pos)
    removed, pos = rd_varint(buf, pos)
    assert added == 0 and removed == 0, f"DisplayInfo icon patch {added}/{removed}"
    frame, pos = rd_varint(buf, pos)
    flags = struct.unpack(">I", buf[pos:pos + 4])[0]
    pos += 4
    background = None
    if flags & CB_DISPLAY_BACKGROUND:
        background, pos = rd_string(buf, pos)
    return {
        "title": title,
        "description": description,
        "icon": icon,
        "icon_count": icon_count,
        "frame": frame,
        "background": background,
        "show_toast": bool(flags & CB_DISPLAY_TOAST),
        "hidden": bool(flags & CB_DISPLAY_HIDDEN),
    }, pos


def read_advancement(buf, pos):
    """Advancement.write: [bool + utf parent][bool + DisplayInfo]
    [varint requirement sets]{varint count}{utf criterion}[bool telemetry]. The
    criteria and the rewards are not on the wire — the client needs the tree's
    shape and the display, nothing else."""
    parent = None
    if buf[pos]:
        parent, pos = rd_string(buf, pos + 1)
    else:
        pos += 1
    display = None
    if buf[pos]:
        display, pos = read_display(buf, pos + 1)
    else:
        pos += 1
    set_count, pos = rd_varint(buf, pos)
    requirements = []
    for _ in range(set_count):
        size, pos = rd_varint(buf, pos)
        one = []
        for _ in range(size):
            name, pos = rd_string(buf, pos)
            one.append(name)
        requirements.append(one)
    telemetry = bool(buf[pos])
    pos += 1
    return {"parent": parent, "display": display, "requirements": requirements,
            "telemetry": telemetry}, pos


def read_progress(buf, pos):
    """`writeMap(criteria, writeUtf, CriterionProgress::serializeToNetwork)`:
    [varint count]{[utf name][bool obtained][i64 epoch ms when obtained]}."""
    count, pos = rd_varint(buf, pos)
    out = {}
    for _ in range(count):
        name, pos = rd_string(buf, pos)
        obtained = bool(buf[pos])
        pos += 1
        ms = None
        if obtained:
            ms = struct.unpack(">q", buf[pos:pos + 8])[0]
            pos += 8
        out[name] = (obtained, ms)
    return out, pos


def read_update_advancements(body):
    """update_advancements (0x85): [bool reset]
    [varint added]{[utf id][Advancement][f32 x][f32 y]}
    [varint removed]{[utf id]}
    [varint progress]{[utf id][AdvancementProgress]}[bool show].

    26.3's added entry is a PositionedAdvancement: the coordinates follow the
    AdvancementHolder instead of living in DisplayInfo. The decoder folds them
    back onto the display so callers can keep asking display["x"].

    The whole body must be consumed, so a field that moved or a length that is
    wrong fails here rather than leaving the rest of the test reading garbage.
    """
    reset = bool(body[0])
    pos = 1
    count, pos = rd_varint(body, pos)
    added = []
    for _ in range(count):
        adv_id, pos = rd_string(body, pos)
        advancement, pos = read_advancement(body, pos)
        x, pos = rd_f32(body, pos)
        y, pos = rd_f32(body, pos)
        advancement["x"], advancement["y"] = x, y
        if advancement["display"] is not None:
            advancement["display"]["x"], advancement["display"]["y"] = x, y
        added.append((adv_id, advancement))
    count, pos = rd_varint(body, pos)
    removed = []
    for _ in range(count):
        adv_id, pos = rd_string(body, pos)
        removed.append(adv_id)
    count, pos = rd_varint(body, pos)
    progress = []
    for _ in range(count):
        adv_id, pos = rd_string(body, pos)
        entry, pos = read_progress(body, pos)
        progress.append((adv_id, entry))
    show = bool(body[pos])
    pos += 1
    assert pos == len(body), (
        f"update_advancements: {len(body) - pos} trailing byte(s) of {len(body)}")
    return {"reset": reset, "added": added, "removed": removed,
            "progress": progress, "show": show}


class Bot:
    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(10)
        self.threshold = -1
        self.reader = wire_client.Reader(self.sock, self)
        # Everything the handshake read before a caller could install a drain.
        # The join's advancement flush is sent *before* the spawn chunk batch
        # (so it is not gated on terrain), and `join` has to walk past the batch
        # to reach the point a caller takes over — so a silent rejoin's restored
        # tree is in here, not in a later socket window.
        self.join_packets = []

    def send(self, pid, body=b""):
        self.reader.send(pid, body)

    def recv(self):
        return read_packet(self.sock, self.threshold)

    def start_reader(self):
        self.reader.start()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def join(self, ack_chunks=True):
        """The handshake, plus (unless `ack_chunks` is false) the chunk-batch ack.

        Stopping at the chunk-batch request without answering it leaves the
        client with *no* play packet sent at all: everything the handshake
        itself sends is config-state, and the play state is entered by the
        server. That is what makes a rejoin below a silent one — the point being
        that the tree a rejoining player gets back is the join's to hand over,
        not something a client has to earn by talking (the survival clock is
        packet-driven, so a client that says nothing never ticks).
        """
        hs = (varint(0) + varint(PROTO) + varint(len(HOST)) + HOST.encode()
              + struct.pack(">H", 0) + varint(2))
        self.sock.sendall(make_frame(hs, -1))
        self.send(0x00, varint(len(self.name)) + self.name.encode() + b"\x00" * 16)
        while True:
            pid, body = self.recv()
            if pid == 0x03:
                self.threshold, _ = rd_varint(body)
            elif pid == 0x02:
                self.send(0x03)
                break
            else:
                raise AssertionError(f"unexpected login packet 0x{pid:02x}")
        while True:
            pid, body = self.recv()
            if pid == 0x0F:
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
            elif pid == 0x03:
                self.send(0x03)
                break
        while True:
            pid, body = self.recv()
            self.join_packets.append((pid, body))
            if pid == 0x32:
                break
        for _ in range(400):
            pid, body = self.recv()
            self.join_packets.append((pid, body))
            if pid == CB_KEEP_ALIVE:
                self.send(SB_KEEP_ALIVE, body[:8])
            elif pid == 0x0B:
                if not ack_chunks:
                    break
                batch_size, _ = rd_varint(body)
                self.send(0x0B, varint(batch_size))
                break

    def move_to(self, x, y, z, ground=1):
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([ground]))


def drain(bot, timeout=1.5):
    return wire_client.drain(bot, timeout)


def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def send_command(bot, cmd_minus_slash):
    bot.send(SB_CHAT_COMMAND, varint(len(cmd_minus_slash)) + cmd_minus_slash.encode())


def tick(bot, times=1):
    """Advance the survival clock by `times` packets (one tick each) — the run
    has `PLUSMC_SURVIVAL_CLOCK=packet`, so a packet *is* a game tick."""
    for _ in range(times):
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 0))


def flushes(bot, timeout=1.5):
    """Every update_advancements one window carries, decoded."""
    return [read_update_advancements(body)
            for body in all_of(drain(bot, timeout), CB_UPDATE_ADVANCEMENTS)]


def added_rows(updates):
    """Every advancement id the `added` lists of `updates` name."""
    out = set()
    for update in updates:
        out.update(dict(update["added"]))
    return out


def join_updates(bot):
    """The update_advancements the handshake already read (a restored tree)."""
    return [read_update_advancements(body)
            for body in all_of(bot.join_packets, CB_UPDATE_ADVANCEMENTS)]


def flushes_until(bot, done, timeout=RESTORE_TIMEOUT, seen=()):
    """Every update_advancements the flushes carry, until `done(updates)` holds.

    One `flushes` window is not a contract: the rows of a single join are the
    server's own flushes, and a loaded box can put them in more than one window
    (the recipe rows and the progress rows are not one packet). A step that
    waits for rows therefore waits for the *rows* — and the bound above is a
    bound on a failure, not the success path.

    `seen` is what the caller already read (a join's own packets, which landed
    before a socket window could open). Without it a wait whose answer is in
    `seen` would spin out the whole timeout, and on this test's rejoin that spin
    is longer than the server's read timeout: the socket is closed under the
    client while it waits for rows it already has.
    """
    updates = list(seen)
    deadline = time.time() + timeout
    while time.time() < deadline and not done(updates):
        updates += flushes(bot)
    return updates


def note_progress(update, into):
    """Merge one packet's progress rows into `into` ({id: {criterion: (got, ms)}})."""
    for adv_id, criteria in update["progress"]:
        into.setdefault(adv_id, {}).update(criteria)
    return into


def obtained_only(progress):
    """The obtained criteria of a merged progress map, with their instants: what
    a save has to carry, without the open entries the visibility walk creates."""
    out = {}
    for adv_id, criteria in progress.items():
        got = {name: ms for name, (obtained, ms) in criteria.items() if obtained}
        if got:
            out[adv_id] = got
    return out


def pin(bot):
    for _ in range(3):
        bot.move_to(PLAYER_X, float(FLOOR_Y + 1), PLAYER_Z)
        time.sleep(0.05)


def earn_join_tick(bot, who):
    """The first flush a join produces: the `minecraft:tick` recipe helper.

    The tree is earned into existence, not pushed at login, and the clock is
    packet-driven — so the join's own play packets may already have ticked it,
    or the next packet may be the one that does. Take the flush from wherever
    it lands.
    """
    updates = flushes(bot)
    if not updates:
        tick(bot, 1)
        updates = flushes(bot)
    assert len(updates) == 1, (
        f"{who}: one flush and nothing else is earned at join, got {len(updates)}")
    first = updates[0]
    assert first["reset"], f"{who}: the first packet with anything to say resets the tree"
    assert set(dict(first["added"])) == {RECIPE_TABLE, "minecraft:recipes/root"}, (
        f"{who}: the tick completes exactly one advancement: {sorted(dict(first['added']))}")
    merged = {}
    for update in updates:
        note_progress(update, merged)
    return merged


def earn_cobblestone(bot, who):
    """`/give cobblestone 1` -> `story/mine_stone`, revealing `story/root`."""
    send_command(bot, f"give {who} cobblestone 1")
    tick(bot, 1)
    updates = flushes(bot)
    added = {}
    merged = {}
    for update in updates:
        added.update(dict(update["added"]))
        note_progress(update, merged)
    assert STORY_MINE_STONE in added, f"{who}: the story advancement must be added: {added}"
    assert STORY_ROOT in added, f"{who}: the done descendant reveals the root: {added}"
    assert all(not update["removed"] for update in updates), "nothing leaves the tree"
    return merged


def wait_listening(proc, timeout=20, before_port=()):
    """The advertised port, from the server's log read up to that line.

    `before_port` names a line the server has to have logged *already*, before
    it advertises a port. The save files are read before the listener starts
    (`load_mob_population` / `load_advancement_state` both document that they
    "must run before the listener starts"), and this test is what that contract
    protects: a connection accepted in the window before the store lands is
    served by a join whose `adopt_saved_advancements` finds no row, so the
    rejoining player's restored tree is silently missing and no amount of
    waiting on the wire brings it back. The assertion is about the log order and
    nothing else, so it is the same on an idle box and a loaded one: the store's
    line comes first, or the listener was open first.
    """
    deadline = time.time() + timeout
    port = None
    seen = []
    while time.time() < deadline:
        line = proc.stdout.readline().decode(errors="replace")
        if line:
            sys.stderr.write(line)
            m = re.search(r"listening on.*:(\d+)", line)
            if m:
                port = int(m.group(1))
                break
            seen.append(line)
    if port is None:
        raise RuntimeError("server did not start")
    for wanted in before_port:
        if not any(wanted in line for line in seen):
            proc.kill()
            proc.wait()
            raise AssertionError(
                f"the server advertised port {port} before {wanted!r}: it read "
                f"its save files too late, so a client that joins now adopts "
                f"nothing\n  what it logged first: {seen}")
    return port


START = time.time()


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}", flush=True)


def boot(binary, data_root, world, env, motd, before_port=()):
    return wire_client.start_server(
        [binary, "--host", HOST, "--port", "0", "--motd", motd, "--offline-mode",
         "--gamemode", "1", "--data", data_root, "--world", world, "--no-flat-world"],
        lambda proc: wait_listening(proc, before_port=before_port), env=env)


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}  (no binary needed)",
              file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-adv-persist-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    env["PLUSMC_MOB_SPAWNING"] = "0"
    # A short autosave, so the store's file appears *during* the first run; the
    # shutdown save is the one the second boot reads.
    env["PLUSMC_AUTOSAVE_SECS"] = "1"

    proc = None
    proc2 = None
    try:
        step(f"world dir: {world}")
        proc, port = boot(binary, data_root, world, env, "advancement persistence 1")
        alice = Bot("AdvSaveA", port)
        wire_client.record(
            bot=alice,
            note="advancement persistence: the tick row, the story branch, and the "
                 "instants a restart has to hand back")
        alice.join()
        alice.start_reader()
        pin(alice)
        step("-> AdvSaveA joins: the tick-earned recipe helper is the first flush")
        alice_before = earn_join_tick(alice, "AdvSaveA")
        step(f"  OK added {RECIPE_TABLE}")
        step("-> /give AdvSaveA cobblestone 1: the inventory_changed trigger")
        alice_before.update(earn_cobblestone(alice, "AdvSaveA"))
        step(f"  OK story branch: {STORY_MINE_STONE} + {STORY_ROOT}")

        # The second player: their own row, and their own session to lose it
        # with. They are not part of the recorded transcript (a transcript is
        # one client), so a replay prints what it did not re-examine.
        bob_before = None
        if not wire_client.skip_server_check(
                "AdvSaveB's join, tick row and disconnect: the replay transcript "
                "holds AdvSaveA's connection only"):
            bob = Bot("AdvSaveB", port)
            bob.join()
            bob.start_reader()
            pin(bob)
            step("-> AdvSaveB joins and earns only the tick row")
            bob_before = earn_join_tick(bob, "AdvSaveB")
            assert set(obtained_only(bob_before)) == {RECIPE_TABLE}, (
                f"AdvSaveB must have the tick row and nothing else: "
                f"{sorted(obtained_only(bob_before))}")
            step("-> AdvSaveB disconnects: the entry's state has to outlive the session")
            bob.close()

        # From here on it is the server's own side: the file the autosave leaves
        # behind, the shutdown save, and the second boot that reads them. A
        # replay has no server to write anything, so it says what it skipped.
        if wire_client.skip_server_check(
                "the restart: the autosave's file, the shutdown save, and the tree "
                "read back from the second boot"):
            print("\nPASS: advancement persistence (phase 1 replayed: earn and disconnect)")
            return 0

        # The file appears without a shutdown: the autosave rides the same
        # cadence as the chunks and the population. (It may already be there —
        # it was written while the players were earning.)
        saved = pathlib.Path(world) / "advancements.dat"
        deadline = time.time() + 15.0
        while not (saved.exists() and saved.stat().st_size > 0) and time.time() < deadline:
            time.sleep(0.2)
        assert saved.exists() and saved.stat().st_size > 0, f"no store file: {saved}"
        step(f"  OK {saved.name} written by the autosave: {saved.stat().st_size} bytes")

        # AdvSaveA stays connected, so the shutdown save reads a *live* entry.
        step("-> SIGTERM with AdvSaveA still online: the shutdown save")
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            print("FAIL: server did not exit", file=sys.stderr)
            return 1
        proc = None
        assert saved.stat().st_size > 0, f"the shutdown save left no file: {saved}"
        step(f"  OK {saved.name}: {saved.stat().st_size} bytes")

        # --- the second boot: each player gets their own rows back -------------
        step("-> second boot, same world dir")
        # The store has to be in before the port is advertised: this boot's
        # rejoin is the one that adopts it, and a store that lands afterwards
        # restores nothing (see `wait_listening`).
        proc2, port2 = boot(binary, data_root, world, env, "advancement persistence 2",
                            before_port=("advancements: loaded",))
        alice2 = Bot("AdvSaveA", port2)
        # The rejoin is *silent*: the handshake's own config packets are all this
        # client ever sends (not even the chunk-batch ack), so the rows below
        # can only have come from the join itself. Under a flush that waited for
        # the player's first survival tick this window would stay empty and the
        # wait would run out — which is exactly what it asserts against.
        alice2.join(ack_chunks=False)
        # Drain the socket from here: the join stops at the chunk-batch request
        # without acking it, and the batch that follows is the join's long pole,
        # so a client that sits on an unread socket while the rows are waited for
        # can fill it and stall the server's own writes. The reader never sends
        # a play packet (it only answers keep-alives), so the rejoin is still
        # silent in the sense this step asserts.
        alice2.start_reader()
        step("-> AdvSaveA rejoins silently: the restored tree is the join's own")
        # The flush is the join's own and lands inside `join` (before the batch
        # the handshake walks past), so the packets that walk swallowed are the
        # first window this step reads.
        # The join's own packets are the first window this step reads, and they
        # are handed to the wait as `seen`: the flush is the join's, so waiting
        # for a *later* window can only cost the rest of the timeout (and on a
        # silent rejoin, more than the server's read timeout).
        updates = flushes_until(alice2, lambda u: set(RESTORED_ROWS) <= added_rows(u),
                                seen=join_updates(alice2))
        assert updates, "the restored tree is the first thing the flush has to say"
        rejoin = updates[0]
        assert rejoin["reset"], "a rejoining client's stale tree must be reset"
        # The rows may arrive over more than one flush (above); the assertions
        # about the *first* packet are the ones a stale rejoining client sees
        # first, and stay exactly as they were.
        added = added_rows(updates)
        for wanted in RESTORED_ROWS:
            assert wanted in added, f"{wanted} must come back visible: {sorted(added)}"
        assert not rejoin["removed"], f"nothing is removed on the first packet: {rejoin}"
        alice_after = {}
        for update in updates:
            note_progress(update, alice_after)
        # The instants are the first run's, so the rows came from the file and
        # not from a re-grant at the reload's own clock.
        assert obtained_only(alice_after) == obtained_only(alice_before), (
            f"the restored rows must be the saved ones:\n"
            f"  before {obtained_only(alice_before)}\n  after  {obtained_only(alice_after)}")
        step(f"  OK restored {sorted(obtained_only(alice_after))} with the same instants")
        # Only now does the client send anything of its own: the tree above was
        # read off the wire with no play packet from this client at all.
        pin(alice2)

        step("-> the same /give again: nothing is re-granted")
        send_command(alice2, "give AdvSaveA cobblestone 1")
        tick(alice2, 2)
        assert flushes(alice2) == [], "an obtained criterion must not fire again"

        step("-> AdvSaveB rejoins: their own row, and no trace of AdvSaveA's")
        bob2 = Bot("AdvSaveB", port2)
        bob2.join()
        bob2.start_reader()
        pin(bob2)
        def bob_rows(seen):
            merged = {}
            for update in seen:
                note_progress(update, merged)
            return RECIPE_TABLE in obtained_only(merged)

        updates = flushes_until(bob2, bob_rows, seen=join_updates(bob2))
        bob_after = {}
        for update in updates:
            note_progress(update, bob_after)
        bob_after = obtained_only(bob_after)
        assert RECIPE_TABLE in bob_after, f"AdvSaveB's own row must come back: {bob_after}"
        assert not any(key.startswith("minecraft:story/") for key in bob_after), (
            f"another player's progress must not leak into AdvSaveB's tree: {bob_after}")
        if bob_before is not None:
            assert bob_after == obtained_only(bob_before), (
                f"AdvSaveB's rows must be the saved ones: "
                f"{obtained_only(bob_before)} vs {bob_after}")
        step(f"  OK AdvSaveB: {sorted(bob_after)}")

        alice2.close()
        bob2.close()
        print("\nPASS: advancement persistence test (the tree and its instants across a "
              "restart)")
        return 0
    finally:
        for p in (proc, proc2):
            if p is not None:
                p.kill()
                try:
                    p.wait(timeout=5)
                except Exception:
                    pass
        shutil.rmtree(world, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
