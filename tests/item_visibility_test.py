#!/usr/bin/env python3
"""Integration test: dropped-item tracking over the real wire protocol.

Item drops are the largest population of entities a busy world carries — every
broken block, every mob's loot, every stack a player throws away — and until
this slice each one was spawned to every connection and every position/stack
change was written to every connection, built per recipient, whatever the
distance. Vanilla's `ChunkMap.TrackedEntity` does not do that: it keeps a viewer
set per entity, sends `ServerEntity.sendPairingData` (Add Entity and the item's
synched stack data) when a relationship starts, Remove Entities when it ends, and
nothing in between to anyone outside the set. This test is the wire proof of the
item half of that gate, run twice — with the gate on (shipped) and with
`PLUSMC_ENTITY_VISIBILITY=0` (the pre-slice arm, every item to every player), so
the gated packets are shown to be the gate's and not the scene's.

Two clients on the committed flat capture, natural spawning off:

  * A is given a stone and drops it (`ServerboundPlayerActionPacket` DROP_ITEM
    on the held slot). A and B, one block apart, both get its Add Entity (entity
    type 71) and the stack-data packet that follows,
  * A steps three blocks back so the drop is outside its pickup range and stays
    on the ground for the rest of the test,
  * B walks 70 blocks east: after the item sweep's quarter-second it gets
    exactly one Remove Entities carrying that item's id, and A — still in range
    — gets none,
  * B stands still: no second remove and no re-add,
  * B walks back: Add Entity and the stack data again,
  * B leaves again and stays away; A drops a second stone. A sees it, B does not,
  * the `PLUSMC_ENTITY_VISIBILITY=0` arm runs the same script and must produce
    the pre-slice stream: no remove on the way out, no second pairing on the way
    back, and the far drop reaching B as well.

Both runs read the server's shutdown line (`shutdown: item tracking: …`): the
gated run must report a peak of two viewers, at least four relationships and
fewer than 1.5 client writes per payload (a two-player broadcast is 2.0), the
pre-slice run a peak of zero and at least 1.8 writes per payload.

Run: item_visibility_test.py /path/to/plusmc
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
CB_REMOVE_ENTITIES = 0x4E
CB_SET_ENTITY_DATA = 0x65
CB_KEEP_ALIVE = 0x2D
CB_PLAYER_POSITION = 0x49
CB_SYSTEM_CHAT = 0x7C

# Serverbound packet ids.
SB_CHAT_COMMAND = 0x07
SB_POSITION = 0x1E
SB_KEEP_ALIVE = 0x1C
SB_PLAYER_ACTION = 0x29
SB_SET_CARRIED_ITEM = 0x36

# EntityType.ITEM's protocol id (src/protocol/ids.h, kItemEntityTypeId).
ITEM_TYPE = 72
# ServerboundPlayerActionPacket$Action.DROP_ITEM.
ACTION_DROP_ITEM = 3
# ServerboundPlayerActionPacket$Action.DROP_ALL_ITEMS.
ACTION_DROP_ALL_ITEMS = 4

# The flat capture's surface (the same y tests/mob_roster_test.py stands on).
PLATFORM = (8.5, -60.0, 8.5)
# Beyond the shipped 48-block range and its 56-block drop line.
AWAY = PLATFORM[0] + 70.0
# How far A steps back after a drop: outside the one-block pickup reach, so the
# stack stays on the ground for the whole test.
BACK = PLATFORM[2] - 3.0
# Where B stands when it comes back: inside the 48-block tracking range but
# outside the one-block pickup reach of the drop, so the test reads the pairing
# burst rather than collecting the item on the way in.
NEAR = PLATFORM[0] + 4.0

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
    """: [varint count][varint eid]..."""
    count, pos = rd_varint(body, 0)
    ids = []
    for _ in range(count):
        eid, pos = rd_varint(body, pos)
        ids.append(eid)
    assert pos == len(body), f"remove_entities: {len(body) - pos} trailing byte(s)"
    return ids


def data_for(body, wanted):
    """Whether this Set Entity Data packet is for `wanted` (the item stack word
    itself is a variable-length serializer this test does not decode: what the
    pairing contract says is that the word follows the Add Entity)."""
    eid, _ = rd_varint(body, 0)
    return eid == wanted


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


# --- the client --------------------------------------------------------------


class Bot:
    """A client that always reads (wire_client.Reader owns the socket)."""

    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=30)
        self.threshold = -1
        self.position = None
        self.seen = []
        self.lock = threading.Lock()
        self.sequence = 0
        self.reader = wire_client.Reader(self.sock, self)

    def send(self, pid, body=b""):
        self.reader.send(pid, body)

    def recv(self):
        pid, body = read_packet(self.sock, self.threshold)
        if pid == CB_PLAYER_POSITION:
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

    def drop_held(self):
        """ServerboundPlayerActionPacket DROP_ITEM on the held slot."""
        self.send(SB_PLAYER_ACTION,
                  varint(ACTION_DROP_ITEM) + struct.pack(">q", 0) + b"\x00"
                  + varint(self.sequence))
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


def command(bot, text, timeout=0.6):
    bot.send(SB_CHAT_COMMAND, varint(len(text)) + text.encode())
    return drain(bot, timeout)


def move(bot, x, steps=3):
    for _ in range(steps):
        bot.move_to(x, PLATFORM[1], PLATFORM[2])
        time.sleep(0.15)


def give_and_hold(bot, item="minecraft:stone", count=1):
    bot.send(SB_CHAT_COMMAND, varint(len(f"give {bot.name} {item} {count}"))
             + f"give {bot.name} {item} {count}".encode())
    reply = pump(bot, timeout=2.0, stop=lambda f: "Gave" in feedback(f))
    assert "Gave" in feedback(reply), f"{bot.name}: /give said {feedback(reply)!r}"
    # The held slot, whichever slot /give filled: slot 0 on a fresh creative
    # inventory, which is also what a client's own join sets.
    bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 0))
    drain(bot, 0.2)


def drop_and_wait(bot, timeout=3.0):
    """Drops the held stack and returns its Add Entity once it arrives."""
    state = {"spawn": None}
    bot.drop_held()

    def stop(fresh):
        for spawn in spawns(fresh):
            if spawn["type"] == ITEM_TYPE:
                state["spawn"] = spawn
        return state["spawn"] is not None

    fresh = pump(bot, timeout=timeout, stop=stop)
    assert state["spawn"] is not None, (
        f"{bot.name}: the dropped stack produced no Add Entity "
        f"(type 71; saw {[s['type'] for s in spawns(fresh)]})")
    return state["spawn"]


# --- the server --------------------------------------------------------------


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


def run_arm(binary, data_root, fixtures, gated):
    label = "gate on" if gated else "PLUSMC_ENTITY_VISIBILITY=0"
    step(f"== arm: {label} ==")
    tmp = tempfile.mkdtemp(prefix="plusmc-itemvis-")
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
            [binary, "--host", HOST, "--port", "0", "--motd", "item visibility",
             "--offline-mode", "--gamemode", "1", "--data", str(data_root),
             "--world", str(world), "--no-flat-world", "--view-distance", "3"],
            lambda p: wait_listening(p, log), env=env, text=True)
        step(f"server listening on {port}")

        a = Bot("ItemWatchA", port)
        b = Bot("ItemWatchB", port)
        wire_client.record(a, note=f"item-visibility: {label} — the dropper")
        wire_client.record(b, note=f"item-visibility: {label} — the viewer who walks")
        a.join()
        b.join()
        drain(a, 1.5)
        drain(b, 1.5)
        command(a, f"tp {PLATFORM[0]} {PLATFORM[1]} {PLATFORM[2]}")
        command(b, f"tp {PLATFORM[0] + 1.0} {PLATFORM[1]} {PLATFORM[2]}")
        drain(a, 0.4)
        drain(b, 0.4)

        # --- 1. a drop beside both: both are paired --------------------------
        step("-> A drops a stone between them; both must get the pairing burst")
        give_and_hold(a)
        first = drop_and_wait(a)
        eid = first["entity"]
        pair = pump(b, timeout=2.0, stop=lambda f: any(
            s["entity"] == eid for s in spawns(f)))
        assert any(s["entity"] == eid and s["type"] == ITEM_TYPE for s in spawns(pair)), (
            f"B never got Add Entity for the item A dropped (saw {spawns(pair)})")
        assert any(data_for(body, eid) for body in all_of(pair, CB_SET_ENTITY_DATA)), (
            f"B got the item's Add Entity without its stack data")
        step(f"   OK both clients were paired with item {eid} (type {ITEM_TYPE})")

        # --- 2. A steps back: the drop stays on the ground -------------------
        step("-> A steps three blocks back, out of the one-block pickup reach")
        a.move_to(PLATFORM[0], PLATFORM[1], BACK)
        time.sleep(0.3)
        drain(a, 0.5)

        # --- 3. B walks out: one remove, and only for B ----------------------
        step(f"-> B walks to x={AWAY:.1f}; A stays beside the item")
        move(b, AWAY)
        if gated:
            gone = pump(b, timeout=2.0, stop=lambda f: eid in removes(f))
            assert eid in removes(gone), (
                f"B walked out of range but was never told to remove item {eid} "
                f"(removes: {removes(gone)})")
            assert removes(gone).count(eid) == 1, (
                f"B got {removes(gone).count(eid)} removes for item {eid}, wanted one")
        else:
            # B still gets Remove Entities for *A* here (the player gate), so the
            # arm's assertion is about the item's id only.
            gone = pump(b, timeout=1.2)
            assert eid not in removes(gone), (
                f"PLUSMC_ENTITY_VISIBILITY=0 removed item {eid} from a far viewer: "
                f"{removes(gone)}")
        still_there = pump(a, timeout=0.6)
        assert eid not in removes(still_there), (
            f"A, beside the item, was told to remove it: {removes(still_there)}")
        step("   OK B's remove stream carried no item remove; A got none" if not gated
             else f"   OK B got exactly one Remove Entities for {eid}; A got none")

        # --- 4. standing still: no second remove, no re-add ------------------
        step("-> B stands still out of range: the sweep must be idempotent")
        quiet = pump(b, timeout=1.2)
        assert eid not in removes(quiet), f"B was told to remove {eid} twice: {removes(quiet)}"
        assert not any(s["entity"] == eid for s in spawns(quiet)), (
            f"B was re-paired with {eid} while still out of range")
        step("   OK no remove and no re-add")

        # --- 5. B walks back: the pairing burst again ------------------------
        step("-> B walks back; the relationship has to start again")
        move(b, NEAR)
        if gated:
            back = pump(b, timeout=3.0, stop=lambda f: any(
                s["entity"] == eid for s in spawns(f)))
            assert any(s["entity"] == eid for s in spawns(back)), (
                f"B came back inside the range but was never re-paired with {eid} "
                f"(spawns: {spawns(back)})")
            assert any(data_for(body, eid) for body in all_of(back, CB_SET_ENTITY_DATA)), (
                "B's re-pairing carried no stack data")
            step(f"   OK B was paired with {eid} again, stack data included")
        else:
            back = pump(b, timeout=1.0)
            assert not any(s["entity"] == eid for s in spawns(back)), (
                f"PLUSMC_ENTITY_VISIBILITY=0 re-paired B with {eid} "
                f"(spawns: {spawns(back)})")
            step("   OK (pre-slice arm) no second pairing burst")

        # --- 6. a drop made while B is away reaches only A -------------------
        step("-> B walks away and stays away; A drops a second stone")
        move(b, AWAY)
        if gated:
            pump(b, timeout=2.0, stop=lambda f: eid in removes(f))
        give_and_hold(a)
        second = drop_and_wait(a)
        step(f"   A dropped item {second['entity']}")
        b_window = pump(b, timeout=1.5)
        if gated:
            assert not any(s["entity"] == second["entity"] for s in spawns(b_window)), (
                f"B, 70 blocks away, was told about item {second['entity']}")
            step(f"   OK {second['entity']} reached A and not B")
        else:
            assert any(s["entity"] == second["entity"] for s in spawns(b_window)), (
                f"PLUSMC_ENTITY_VISIBILITY=0 did not reach B with {second['entity']}")
            step(f"   OK (pre-slice arm) {second['entity']} reached B as well")

        a.close()
        b.close()
        time.sleep(0.2)
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
        proc = None
        time.sleep(0.3)  # the log pump reads the last lines after the exit
        text = "".join(log)
        report = [line for line in text.splitlines() if "shutdown: item tracking:" in line]
        assert report, f"the server printed no item tracking line:\n{text[-2000:]}"
        line = report[0]
        m = re.search(r"(\d+) payloads built, (\d+) client writes \(([\d.]+) per payload\), "
                      r"(\d+) pairings, peak (\d+) viewers", line)
        assert m, f"item tracking line has an unexpected shape: {line!r}"
        payloads, writes, per, pairings, peak = (m.group(1), int(m.group(2)),
                                                 float(m.group(3)), int(m.group(4)),
                                                 int(m.group(5)))
        step(f"   shutdown: {line.split('shutdown: ', 1)[1]}")
        if gated:
            assert peak >= 2, f"an item's viewer set never held both players (peak {peak})"
            assert pairings >= 4, (
                f"only {pairings} relationships started; the summon, the re-entry and "
                f"the second drop alone are four")
            # The gate's whole point: a payload reaches its viewers, not the
            # population. Two players would be 2.0 under the pre-slice fan-out.
            assert per < 1.5, (
                f"{per:.2f} client writes per payload: the item fan-out is not "
                f"writing to the tracked viewers alone")
        else:
            assert per >= 1.8, (
                f"{per:.2f} client writes per payload: the pre-slice arm must "
                f"broadcast the drop to both players")
            assert peak == 0, (
                f"PLUSMC_ENTITY_VISIBILITY=0 still maintained viewer sets (peak {peak})")
        step(f"   OK {payloads} payloads / {writes} writes ({per:.2f} per payload), "
             f"{pairings} pairings, peak {peak}")
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
    print("item-visibility: both arms passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
