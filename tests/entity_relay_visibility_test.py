#!/usr/bin/env python3
"""Integration test: the transient entity relays over the real wire protocol.

Mobs and item drops got the tracked-viewer gate in the two slices before this
one (`docs/MOB_VISIBILITY.md`). Four kinds still broadcast their own way: a
payout's experience orbs, a primed TNT block, an arrow or a thrown item, and a
thrown potion (with the cloud a lingering one leaves). Each was spawned to every
connection built per recipient and synced to every connection, whatever the
distance. This test is the wire proof that they now use the same
`g_tracked_viewers` map: pairing when a relationship starts, Remove Entities
when it ends, and nothing in between to anyone outside the set.

Two clients on the committed flat capture, natural spawning off, the world's
weather cycle frozen and the survival clock packet-driven:

  * A summons a pig two blocks away and primes a TNT block beside it. A gets
    Add Entity + the fuse data; B, 70 blocks east, gets neither the spawn nor a
    position sync for that id. B walks to 25 blocks: the five-tick sweep pairs
    it (Add Entity + fuse again). B walks back out: exactly one Remove Entities.
  * A walks 14 blocks clear of the blast, which kills the pig. The orbs it
    leaves are 14 blocks from A — in tracking range, outside the eight-block
    pickup range — so A sees them and they stay. B, still 70 blocks away, sees
    no orb. B walks to 30 blocks: paired with the orb (Add Entity + its value).
    B walks back: the orb is removed from its stream.
  * With B far again, A fires a full-draw arrow and throws a splash potion. A
    sees each spawn (and the arrow's synched data / the throwable stack data)
    and every position sync for the ids; B's stream contains neither id.

Then the whole script runs again with `PLUSMC_ENTITY_VISIBILITY=0`: the far
player must see every one of those spawns and be told to remove nothing when it
walks away. Both runs read the per-kind shutdown lines (`shutdown: orb tracking:
…`, `tnt`, `projectile`, `potion`): the gated run must stay under 1.5 client
writes per payload (a two-player broadcast is 2.0) and the pre-slice run must be
at or above 1.8, with a viewer peak of zero on the arm that does not maintain
sets.

A third arm pins vanilla's *per-type* ranges (`world/entity_tracking_table.h`)
and the view-distance clamp: at 80 blocks a pig (160) is paired and an arrow
(64) is not, and at 150 blocks a pig is not either, because the 128-block view
distance clamps its range.

Run: entity_relay_visibility_test.py /path/to/plusmc
"""
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_client  # noqa: E402
from wire_client import make_frame, read_packet  # noqa: E402
from nbt_component import chat_text  # noqa: E402

PROTO = 777
HOST = "127.0.0.1"

# Clientbound packet ids.
CB_SPAWN_ENTITY = 0x01
CB_CONTAINER_SET_CONTENT = 0x12
CB_ENTITY_POSITION_SYNC = 0x23
CB_KEEP_ALIVE = 0x2D
CB_REMOVE_ENTITIES = 0x4E
CB_SET_ENTITY_DATA = 0x65
CB_SYSTEM_CHAT = 0x7C

# Serverbound packet ids.
SB_CHAT_COMMAND = 0x07
SB_FLYING = 0x21
SB_POSITION = 0x1E
SB_KEEP_ALIVE = 0x1C
SB_PLAYER_ACTION = 0x29
SB_SET_CARRIED_ITEM = 0x36
SB_USE_ITEM = 0x43
SB_USE_ITEM_ON = 0x42

# EntityType protocol ids (src/protocol/ids.h).
TYPE_PRIMED_TNT = 136
TYPE_XP_ORB = 50
TYPE_ARROW = 6
TYPE_SPLASH_POTION = 108
TYPE_PIG = 101

ACTION_RELEASE_USE_ITEM = 5
SLOT_MENU_HOTBAR_FIRST = 36
TICK_MS = 0.05

# The flat capture's surface, as the item and mob visibility tests stand on it.
PLATFORM = (8.5, -60.0, 8.5)
# Beyond the shipped 48-block range and its 56-block drop line.
AWAY = (78.5, -60.0, 8.5)
# The TNT cell: two blocks north of A, inside the 4.5-block interaction reach,
# and close enough to the pig for the blast to be lethal.
TNT_CELL = (8, -60, 11)
# Where A waits out the blast: in tracking range of the orbs, outside their
# eight-block pickup range, and outside the explosion's own eight-block damage.
BLAST_CLEAR = (8.5, -60.0, -6.0)
# B's two sweep stops: inside the 48-block entry range, outside the blast (TNT)
# and outside the pickup range (orbs).
NEAR_TNT = (8.5, -60.0, 25.0)
NEAR_ORB = (8.5, -60.0, 30.0)
# The pig `/summon` puts on the ring east of A.
PIG_AT = (PLATFORM[0] + 2.0, PLATFORM[1], PLATFORM[2])

START = time.time()


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}", flush=True)


# --- decoding ---------------------------------------------------------------

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


def read_spawn(body):
    """Add Entity: [varint eid][uuid 16][varint type][f64 x y z][LpVec3]
    [u8 pitch][u8 yaw][u8 head yaw][varint data]."""
    eid, pos = rd_varint(body, 0)
    pos += 16
    etype, pos = rd_varint(body, pos)
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    pos += 24
    first = body[pos]
    if first == 0:
        pos += 1
    else:
        pos += 6
        if first & 0x4:
            _, pos = rd_varint(body, pos)
    pos += 3
    _, pos = rd_varint(body, pos)
    assert pos == len(body), f"spawn_entity: {len(body) - pos} trailing byte(s)"
    return {"entity": eid, "type": etype, "x": x, "y": y, "z": z}


def read_remove_entities(body):
    count, pos = rd_varint(body, 0)
    ids = []
    for _ in range(count):
        eid, pos = rd_varint(body, pos)
        ids.append(eid)
    return ids


def read_stack(buf, pos):
    count, pos = rd_varint(buf, pos)
    if count == 0:
        return None, pos
    item, pos = rd_varint(buf, pos)
    added, pos = rd_varint(buf, pos)
    removed, pos = rd_varint(buf, pos)
    assert added == 0 and removed == 0, (added, removed)
    return {"item": item, "count": count}, pos


def decode_content(body):
    """container_set_content: (window, state, [stacks], cursor)."""
    window, pos = rd_varint(body, 0)
    state, pos = rd_varint(body, pos)
    count, pos = rd_varint(body, pos)
    slots = []
    for _ in range(count):
        stack, pos = read_stack(body, pos)
        slots.append(stack)
    cursor, pos = read_stack(body, pos)
    assert pos == len(body), f"container_set_content: {len(body) - pos} trailing byte(s)"
    return window, state, slots, cursor


def data_for(body, wanted):
    eid, _ = rd_varint(body, 0)
    return eid == wanted


def sync_id(body):
    eid, _ = rd_varint(body, 0)
    return eid


def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def feedback(packets):
    out = []
    for body in all_of(packets, CB_SYSTEM_CHAT):
        try:
            out.append(chat_text(body))
        except AssertionError:
            pass
    return " ".join(out)


def spawns(packets):
    return [read_spawn(body) for body in all_of(packets, CB_SPAWN_ENTITY)]


def removes(packets):
    return [eid for body in all_of(packets, CB_REMOVE_ENTITIES)
            for eid in read_remove_entities(body)]


def spawn_of(packets, etype):
    return [s for s in spawns(packets) if s["type"] == etype]


# --- the client -------------------------------------------------------------

class Bot:
    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=30)
        self.threshold = -1
        self.position = None
        self.seen = []
        self.lock = threading.Lock()
        self.sequence = 0
        self.slots = []
        self.reader = wire_client.Reader(self.sock, self)

    def send(self, pid, body=b""):
        self.reader.send(pid, body)

    def recv(self):
        pid, body = read_packet(self.sock, self.threshold)
        if pid == 0x48:  # player_position
            self.position = struct.unpack(">ddd", body[0:24])
        return pid, body

    def start_reader(self):
        self.reader.start()

    def close(self):
        self.reader.stop()
        try:
            self.sock.close()
        except OSError:
            pass

    def join(self):
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
            pid, _ = self.recv()
            if pid == 0x32:
                break
        for _ in range(400):
            pid, body = self.recv()
            if pid == CB_KEEP_ALIVE:
                self.send(SB_KEEP_ALIVE, body[:8])
            elif pid == 0x0B:
                batch, _ = rd_varint(body)
                self.send(0x0B, varint(batch))
                break
        self.start_reader()
        return True

    def move_to(self, x, y, z, ground=1):
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([ground]))

    def click(self, x, y, z, face=1):
        packed = (((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12)
                  | (y & 0xFFF)) & 0xFFFFFFFFFFFFFFFF
        body = (varint(0) + struct.pack(">Q", packed) + varint(face)
                + struct.pack(">fff", 0.5, 0.5, 0.5) + bytes([0, 0])
                + varint(self.sequence))
        self.send(SB_USE_ITEM_ON, body)
        self.sequence += 1


def drain(bot, timeout=1.0):
    out = wire_client.drain(bot, timeout)
    with bot.lock:
        bot.seen += out
    return out


def pump(bot, timeout=2.0, stop=None, slice_s=0.05):
    fresh = []
    deadline = time.time() + timeout
    while True:
        fresh += drain(bot, slice_s)
        if stop is not None and stop(fresh):
            return fresh
        if time.time() >= deadline:
            return fresh


def command(bot, text, timeout=0.7):
    bot.send(SB_CHAT_COMMAND, varint(len(text)) + text.encode())
    return drain(bot, timeout)


def move_to(bot, xyz, settle=0.5):
    """Send the position, let the sweep run, and return what arrived — the
    pairing burst of a walk-in lands in this window, not in the one after it."""
    bot.move_to(*xyz)
    time.sleep(settle)
    return drain(bot, 0.25)


def walk_until(bot, xyz, done, settle=0.35, timeout=1.2):
    """Move, then collect until `done`; the sweep's five ticks are 250 ms, so
    this waits a quarter second at most for a transition and does not spend the
    fuse waiting for a packet it already has."""
    packets = move_to(bot, xyz, settle=settle)
    if not done(packets):
        packets += pump(bot, timeout=timeout, stop=done)
    return packets


def give(bot, item, count=1):
    """`/give`; returns the menu slot the stack landed in, from the resync."""
    before = list(bot.slots)
    packets = command(bot, f"give {bot.name} minecraft:{item} {count}")
    assert "Gave " in feedback(packets), f"{bot.name}: /give said {feedback(packets)!r}"
    contents = all_of(packets, CB_CONTAINER_SET_CONTENT)
    assert contents, "a /give must resync the inventory"
    _, _, after, _ = decode_content(contents[-1])
    bot.slots = after
    for i, stack in enumerate(after):
        if stack is not None and (i >= len(before) or before[i] != stack):
            return i
    raise AssertionError(f"{item} did not appear in the inventory: {after}")


def select(bot, menu_slot):
    bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", menu_slot - SLOT_MENU_HOTBAR_FIRST))
    drain(bot, 0.15)


def use_item(bot, yaw=0.0, pitch=0.0):
    bot.send(SB_USE_ITEM, varint(0) + varint(0) + struct.pack(">ff", yaw, pitch))


def release_use_item(bot):
    body = varint(ACTION_RELEASE_USE_ITEM) + struct.pack(">q", 0) + bytes([1]) + varint(0)
    bot.send(SB_PLAYER_ACTION, body)


def advance(bot, ticks):
    packets = []
    for _ in range(ticks):
        bot.send(SB_FLYING, bytes([1]))
        packets += drain(bot, 0.06)
    return packets


# --- the server -------------------------------------------------------------

def wait_listening(proc, log, timeout=30):
    deadline = time.time() + timeout
    port = None
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            break
        log.append(line)
        m = re.search(r"listening on.*:(\d+)", line)
        if m:
            port = int(m.group(1))
            break

    def pump_log():
        for raw in iter(proc.stdout.readline, ""):
            log.append(raw)

    threading.Thread(target=pump_log, daemon=True).start()
    if port is None:
        raise RuntimeError("server did not start")
    import socket
    for _ in range(30):
        try:
            s = socket.create_connection((HOST, port), timeout=1)
            s.close()
            break
        except OSError:
            time.sleep(0.1)
    return port


def parse_tracking(text, kind):
    m = re.search(rf"{kind} tracking: (\d+) payloads built, (\d+) client writes "
                  rf"\(([\d.]+) per payload\), (\d+) pairings, peak (\d+) viewers", text)
    if not m:
        return None
    return {"payloads": int(m.group(1)), "writes": int(m.group(2)),
            "per": float(m.group(3)), "pairings": int(m.group(4)),
            "peak": int(m.group(5))}


def run_arm(binary, data_root, fixtures, gated):
    label = "gate on" if gated else "PLUSMC_ENTITY_VISIBILITY=0"
    step(f"== arm: {label} ==")
    tmp = tempfile.mkdtemp(prefix="plusmc-relayvis-")
    world = pathlib.Path(tmp) / "world"
    region_dir = world / "dimensions" / "minecraft" / "overworld" / "region"
    region_dir.mkdir(parents=True)
    shutil.copyfile(fixtures / "r.0.0.mca", region_dir / "r.0.0.mca")

    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_MOB_SPAWNING"] = "0"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    if not gated:
        env["PLUSMC_ENTITY_VISIBILITY"] = "0"

    log = []
    proc = None
    try:
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "entity relays",
             "--offline-mode", "--gamemode", "1", "--data", str(data_root),
             "--world", str(world), "--no-flat-world", "--view-distance", "3"],
            lambda p: wait_listening(p, log), env=env, text=True)
        step(f"server listening on {port}")

        a = Bot("RelayWatchA", port)
        b = Bot("RelayWatchB", port)
        a.join()
        b.join()
        drain(a, 1.2)
        drain(b, 1.2)
        command(a, f"tp {PLATFORM[0]} {PLATFORM[1]} {PLATFORM[2]}")
        command(b, f"tp {AWAY[0]} {AWAY[1]} {AWAY[2]}")
        # `/tp` moves the session and the client, but the registry copy the
        # sweeps read is written by the movement packet; without this the far
        # client would still be tracked at its login position.
        a.move_to(*PLATFORM)
        b.move_to(*AWAY)
        drain(a, 0.4)
        drain(b, 0.4)

        # --- 1. TNT: the pairing, the fuse, the sweep, the despawn -----------
        step("-> A primes a TNT block; B is 70 blocks away")
        command(a, f"setblock {TNT_CELL[0]} {TNT_CELL[1]} {TNT_CELL[2]} minecraft:tnt")
        a.send(SB_CHAT_COMMAND, varint(len("summon pig 1")) + b"summon pig 1")
        drain(a, 0.4)
        flint = give(a, "flint_and_steel")
        select(a, flint)
        a.click(*TNT_CELL)
        lit = pump(a, timeout=2.0, stop=lambda f: bool(spawn_of(f, TYPE_PRIMED_TNT)))
        tnt = spawn_of(lit, TYPE_PRIMED_TNT)
        assert tnt, (f"A primed a TNT block and saw no Add Entity (saw types "
                     f"{[s['type'] for s in spawns(lit)]}, feedback {feedback(lit)!r})")
        tnt_eid = tnt[0]["entity"]
        assert any(data_for(body, tnt_eid) for body in all_of(lit, CB_SET_ENTITY_DATA)), \
            "the TNT spawn came without its fuse data"
        far = pump(b, timeout=0.8)
        if gated:
            assert not spawn_of(far, TYPE_PRIMED_TNT), (
                f"B, 70 blocks away, was told about the primed TNT: "
                f"{spawn_of(far, TYPE_PRIMED_TNT)}")
            assert tnt_eid not in [sync_id(body) for body in all_of(far, CB_ENTITY_POSITION_SYNC)], \
                "B, out of range, was sent a position sync for the TNT"
            step(f"   OK entity {tnt_eid} reached A only (fuse data included)")
        else:
            assert spawn_of(far, TYPE_PRIMED_TNT), (
                "PLUSMC_ENTITY_VISIBILITY=0 did not spawn the TNT for B")
            step("   OK (pre-slice arm) the priming reached B as well")

        # A has to be clear of the pig before the fuse runs out: at the platform
        # it is two blocks from the pig, inside the eight-block pickup range, so
        # the orbs would be collected before the test could read their pairing.
        step("-> A walks 14 blocks clear of the coming blast")
        move_to(a, BLAST_CLEAR, settle=0.3)

        step("-> B walks inside the range and back out; the sweep decides")
        pair = walk_until(b, NEAR_TNT, lambda f: bool(spawn_of(f, TYPE_PRIMED_TNT)))
        if gated:
            assert spawn_of(pair, TYPE_PRIMED_TNT), (
                "B walked inside 48 blocks of the TNT but was never paired")
            assert any(data_for(body, spawn_of(pair, TYPE_PRIMED_TNT)[0]["entity"])
                       for body in all_of(pair, CB_SET_ENTITY_DATA)), \
                "B's pairing with the TNT carried no fuse data"
            step("   OK the sweep paired B with the burning entity, fuse included")
        if gated:
            gone = walk_until(b, AWAY, lambda f: tnt_eid in removes(f))
            assert tnt_eid in removes(gone), (
                f"B walked out of range but was never told to remove the TNT "
                f"(removes {removes(gone)})")
            assert removes(gone).count(tnt_eid) == 1, (
                f"B got {removes(gone).count(tnt_eid)} removes for {tnt_eid}")
            step("   OK B was told to remove the TNT exactly once")
        else:
            # No sweep runs on this arm, so walking out is a no-op; the despawn
            # that follows is the blast's own broadcast, which is expected here
            # and is why this step asserts nothing about removes.
            move_to(b, AWAY, settle=0.35)
            step("   OK (pre-slice arm) no sweep and no walk-out transition")

        # --- 2. the pig dies in the blast; its orbs stay on the ground -------
        step("-> the blast kills the pig and leaves orbs")
        deadline = time.time() + 9.0
        while not spawn_of(a.seen, TYPE_XP_ORB) and time.time() < deadline:
            pump(a, timeout=0.25)
        orbs = spawn_of(a.seen, TYPE_XP_ORB)
        assert orbs, ("the TNT blast killed no pig: A saw no experience orb "
                      f"(A's spawn types {[s['type'] for s in spawns(a.seen)]})")
        orb_eid = orbs[0]["entity"]
        assert any(data_for(body, orb_eid) for body in all_of(a.seen, CB_SET_ENTITY_DATA)), \
            "the orb's Add Entity came without its value"
        assert abs(orbs[0]["x"] - PIG_AT[0]) < 4.0, (
            f"the orb is at x={orbs[0]['x']:.1f}, not near the pig at {PIG_AT[0]:.1f}")
        if gated:
            assert not spawn_of(b.seen, TYPE_XP_ORB), (
                f"B, 70 blocks away, was told about the orbs: {spawn_of(b.seen, TYPE_XP_ORB)}")
            step(f"   OK orb {orb_eid} reached A only")
        else:
            wait = time.time() + 2.0
            while not spawn_of(b.seen, TYPE_XP_ORB) and time.time() < wait:
                pump(b, timeout=0.2)
            assert spawn_of(b.seen, TYPE_XP_ORB), (
                "PLUSMC_ENTITY_VISIBILITY=0 did not reach B with the orbs")
            step("   OK (pre-slice arm) the orbs reached B as well")

        step("-> B walks inside the orb's range and back out")
        orb_pair = walk_until(b, NEAR_ORB, lambda f: bool(spawn_of(f, TYPE_XP_ORB)))
        if gated:
            assert spawn_of(orb_pair, TYPE_XP_ORB), (
                "B walked inside 48 blocks of the orb but was never paired")
            step("   OK the sweep paired B with the orb")
        if gated:
            orb_gone = walk_until(b, AWAY, lambda f: orb_eid in removes(f))
            assert orb_eid in removes(orb_gone), (
                f"B walked out of range but was never told to remove orb {orb_eid} "
                f"(removes {removes(orb_gone)})")
            step("   OK B was told to remove the orb once")
        else:
            move_to(b, AWAY, settle=0.35)
            step("   OK (pre-slice arm) no orb remove on the way out")

        # --- 3. a projectile while B is away ---------------------------------
        step("-> A fires a full-draw arrow; B is far")
        bow_slot = give(a, "bow")
        give(a, "arrow", 8)
        select(a, bow_slot)
        use_item(a, yaw=0.0, pitch=0.0)
        drain(a, 0.2)
        shot = advance(a, 20)
        release_use_item(a)
        shot += pump(a, timeout=2.0, stop=lambda f: bool(spawn_of(f, TYPE_ARROW)))
        arrow = spawn_of(shot, TYPE_ARROW)
        assert arrow, (f"A released a full draw and no arrow spawned (saw types "
                      f"{[s['type'] for s in spawns(shot)]})")
        arrow_eid = arrow[0]["entity"]
        if not any(data_for(body, arrow_eid) for body in all_of(shot, CB_SET_ENTITY_DATA)):
            # The pairing burst is two writes back to back; a stop-on-spawn
            # window can close between them, so give the data its own slice
            # before calling it missing (seen once, as a flake).
            shot += pump(a, timeout=0.5,
                         stop=lambda f: any(data_for(body, arrow_eid)
                                            for body in all_of(f, CB_SET_ENTITY_DATA)))
        assert any(data_for(body, arrow_eid) for body in all_of(shot, CB_SET_ENTITY_DATA)), \
            "the arrow's Add Entity came without its in-ground/crit data"
        far = pump(b, timeout=0.8)
        if gated:
            assert not spawn_of(far, TYPE_ARROW), (
                f"B, 70 blocks away, was told about the arrow: {spawn_of(far, TYPE_ARROW)}")
            assert arrow_eid not in [sync_id(body) for body in all_of(far, CB_ENTITY_POSITION_SYNC)], \
                "B, out of range, was sent an arrow position sync"
            step(f"   OK arrow {arrow_eid} reached A only")
        else:
            assert spawn_of(far, TYPE_ARROW), (
                "PLUSMC_ENTITY_VISIBILITY=0 did not reach B with the arrow")
            step("   OK (pre-slice arm) the arrow reached B as well")

        # --- 4. a thrown potion while B is away ------------------------------
        step("-> A throws a splash potion; B is far")
        potion_slot = give(a, "splash_potion")
        select(a, potion_slot)
        use_item(a, yaw=0.0, pitch=-20.0)
        thrown = pump(a, timeout=2.0, stop=lambda f: bool(spawn_of(f, TYPE_SPLASH_POTION)))
        potion = spawn_of(thrown, TYPE_SPLASH_POTION)
        assert potion, (f"A threw a splash potion and no bottle spawned (saw types "
                        f"{[s['type'] for s in spawns(thrown)]})")
        potion_eid = potion[0]["entity"]
        if not any(data_for(body, potion_eid) for body in all_of(thrown, CB_SET_ENTITY_DATA)):
            thrown += pump(a, timeout=0.5,
                           stop=lambda f: any(data_for(body, potion_eid)
                                              for body in all_of(f, CB_SET_ENTITY_DATA)))
        assert any(data_for(body, potion_eid) for body in all_of(thrown, CB_SET_ENTITY_DATA)), \
            "the bottle's Add Entity came without its throwable stack data"
        far = pump(b, timeout=0.8)
        if gated:
            assert not spawn_of(far, TYPE_SPLASH_POTION), (
                f"B, 70 blocks away, was told about the bottle: "
                f"{spawn_of(far, TYPE_SPLASH_POTION)}")
            assert potion_eid not in [sync_id(body) for body in all_of(far, CB_ENTITY_POSITION_SYNC)], \
                "B, out of range, was sent a bottle position sync"
            step(f"   OK bottle {potion_eid} reached A only")
        else:
            assert spawn_of(far, TYPE_SPLASH_POTION), (
                "PLUSMC_ENTITY_VISIBILITY=0 did not reach B with the bottle")
            step("   OK (pre-slice arm) the bottle reached B as well")

        # --- 5. the shutdown lines -------------------------------------------
        a.close()
        b.close()
        time.sleep(0.2)
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
        proc = None
        time.sleep(0.3)
        text = "".join(log)
        reports = {}
        for kind in ("orb", "tnt", "projectile", "potion"):
            line = parse_tracking(text, kind)
            assert line is not None, (
                f"the server printed no `{kind} tracking:` line:\n{text[-2000:]}")
            reports[kind] = line
            step(f"   shutdown: {kind}: {line['payloads']} payloads, {line['writes']} "
                 f"writes ({line['per']:.2f}), {line['pairings']} pairings, "
                 f"peak {line['peak']}")
        if gated:
            for kind, line in reports.items():
                assert line["per"] < 1.5, (
                    f"{kind}: {line['per']:.2f} client writes per payload — the "
                    f"fan-out is not writing to the tracked viewers alone")
            assert reports["tnt"]["pairings"] >= 2, (
                f"only {reports['tnt']['pairings']} TNT relationships; the spawn and "
                f"B's walk-in are two")
            assert reports["orb"]["pairings"] >= 2, (
                f"only {reports['orb']['pairings']} orb relationships; the spawn and "
                f"B's walk-in are two")
            assert all(line["peak"] <= 2 for line in reports.values()), (
                f"a viewer set held more than the two players: {reports}")
        else:
            for kind, line in reports.items():
                assert line["per"] >= 1.8, (
                    f"{kind}: {line['per']:.2f} client writes per payload — the "
                    f"pre-slice arm must broadcast to both players")
                assert line["peak"] == 0, (
                    f"PLUSMC_ENTITY_VISIBILITY=0 still maintained {kind} viewer sets "
                    f"(peak {line['peak']})")
        step(f"== arm passed: {label} ==")
        return True
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


def run_range_arm(binary, data_root, fixtures):
    """The third arm: vanilla's per-type `clientTrackingRange`, not one range.

    Vanilla tracks a pig at `10 * 16 = 160` blocks and an arrow at `4 * 16 = 64`
    (`ChunkMap.addEntity`; the numbers are `world/entity_tracking_table.h`), then
    clamps either to the viewer's view distance (`TrackedEntity.updatePlayer`).
    With the view distance at 8 chunks (128 blocks) the rules separate. B stands
    80 blocks from A; at that one distance:

      * the pig B summons (around B) is paired with A too — a pig's own range is
        160 blocks,
      * the arrow A fires away from B is *not* sent to B — an arrow's range is 64
        blocks — so the difference between the two is the type, not the distance,
      * with A moved 150 blocks off, a second pig is not paired with A even though
        150 < its 160, because the viewer's view distance clamps tracking to 128.

    Before per-type ranges every kind used one 48-block range, so A would have
    seen neither the pig nor the arrow at 80 blocks.
    """
    step("== arm: per-type ranges (view distance 8) ==")
    tmp = tempfile.mkdtemp(prefix="plusmc-ranges-")
    world = pathlib.Path(tmp) / "world"
    region_dir = world / "dimensions" / "minecraft" / "overworld" / "region"
    region_dir.mkdir(parents=True)
    shutil.copyfile(fixtures / "r.0.0.mca", region_dir / "r.0.0.mca")

    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_MOB_SPAWNING"] = "0"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    log = []
    proc = None
    try:
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "entity ranges",
             "--offline-mode", "--gamemode", "1", "--data", str(data_root),
             "--world", str(world), "--no-flat-world", "--view-distance", "8"],
            lambda p: wait_listening(p, log), env=env, text=True)
        step(f"server listening on {port} (view distance 8 = 128 blocks)")

        a = Bot("RangeWatchA", port)
        b = Bot("RangeWatchB", port)
        a.join()
        b.join()
        command(a, f"tp {PLATFORM[0]} {PLATFORM[1]} {PLATFORM[2]}")
        command(b, f"tp 88.5 {PLATFORM[1]} {PLATFORM[2]}")
        a.move_to(*PLATFORM)
        b.move_to(88.5, PLATFORM[1], PLATFORM[2])
        drain(a, 1.2)  # the join's chunk batch
        drain(b, 1.2)

        # --- a pig at 80 blocks: 160-block type range -----------------------
        step("-> B summons a pig 80 blocks from A (160-block type range)")
        # B is two blocks from the pig, so its pairing lands inside the command's
        # own drain window; A's arrives later. Keep both windows.
        pig_b = command(b, "summon pig")
        pig_a = pump(a, 1.5, stop=lambda f: bool(spawn_of(f, TYPE_PIG)))
        assert spawn_of(pig_a, TYPE_PIG), (
            "A did not get B's pig 80 blocks away; a pig's own range is 160 blocks")
        pig_b += pump(b, 1.0, stop=lambda f: bool(spawn_of(f, TYPE_PIG)))
        assert spawn_of(pig_b, TYPE_PIG), "B did not get its own summoned pig"
        step("   OK both paired with the pig")

        # --- an arrow at the same 80 blocks: 64-block type range ------------
        step("-> A fires an arrow at the same 80 blocks (64-block type range)")
        bow_slot = give(a, "bow")
        give(a, "arrow", 8)
        select(a, bow_slot)
        use_item(a, yaw=0.0, pitch=0.0)  # +z, away from B
        drain(a, 0.2)
        shot = advance(a, 20)
        release_use_item(a)
        shot += pump(a, timeout=2.0, stop=lambda f: bool(spawn_of(f, TYPE_ARROW)))
        assert spawn_of(shot, TYPE_ARROW), (
            "A released a full draw and no arrow spawned (saw types "
            f"{[s['type'] for s in spawns(shot)]})")
        arrow_far = pump(b, 0.8)
        assert not spawn_of(arrow_far, TYPE_ARROW), (
            "B was paired with an arrow 80 blocks away; its range is 64, not the "
            "pig's 160 — the relay is not reading the type's own range")
        step("   OK the arrow reached A only")

        # --- the view-distance clamp ---------------------------------------
        step("-> A moves 150 blocks off; a pig there is inside 160 but outside the 128 clamp")
        command(a, "tp 238.5 -60 8.5")
        a.move_to(238.5, PLATFORM[1], PLATFORM[2])
        drain(a, 0.6)
        command(b, "summon pig")
        fresh = pump(a, 1.5)
        assert not spawn_of(fresh, TYPE_PIG), (
            "A was paired with a pig 150 blocks away: that is inside the pig's own "
            "160, but the viewer's view distance clamps tracking to 128")
        step("   OK the view-distance clamp kept it out")

        a.close()
        b.close()
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
        proc = None
        step("== arm passed: per-type ranges ==")
        return True
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    fixtures = pathlib.Path(__file__).resolve().parent / "fixtures"
    data_root = pathlib.Path(__file__).resolve().parent / ".." / "data" / "vanilla"

    run_arm(binary, data_root, fixtures, gated=True)
    run_arm(binary, data_root, fixtures, gated=False)
    run_range_arm(binary, data_root, fixtures)
    print("entity-relay-visibility: all three arms passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
