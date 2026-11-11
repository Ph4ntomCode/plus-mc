#!/usr/bin/env python3
"""Integration test: mob tracking over the real wire protocol.

A mob's packets used to go to every connection in the world, whatever the
distance: the tick's fan-out was `for each player: for each mob: send its
position`, and a summon answered Add Entity to everyone. Vanilla does not do
that — `ChunkMap.TrackedEntity` keeps a viewer set per entity, sends
`ServerEntity.sendPairingData` (Add Entity and its synched words) when a
relationship starts, Remove Entities when it ends, and nothing in between to
anyone outside the set. This test is the wire proof of the mob half of that
gate, and it runs the same script twice — once with the gate on (the shipped
behaviour) and once with `PLUSMC_MOB_VISIBILITY=0` (the pre-slice arm, every
mob to every player) — because a gate that is never compared with its absence
is a gate nobody has shown to be doing anything.

The world is the committed flat capture (`tests/fixtures/r.0.0.mca`), natural
spawning is off (so the population under test is exactly what this script
summons), the clock is frozen and both players are creative, so nothing burns,
spawns or fights between the steps. Two clients join:

  * A summons a pig; A *and* B (standing two blocks away) both get its Add
    Entity (type 100) and its health word, which is the pairing burst an
    in-range viewer has always got,
  * B walks 70 blocks east. One server tick later B gets exactly one Remove
    Entities carrying that mob's id — the relationship ended — and A, still
    beside the mob, does not,
  * standing still, B gets no second remove and no re-add: the sweep is
    idempotent, and the hysteresis keeps a viewer on the boundary from
    flapping,
  * B walks back and gets the pairing burst again — Add Entity, then Set
    Entity Data with the health word — because the relationship has to be
    re-established before the mob's deltas can be applied,
  * B walks away again and *stays* away; A summons a second pig. A sees it, B
    does not: a spawn is gated the same way a relay is,
  * in the `PLUSMC_MOB_VISIBILITY=0` arm the same steps produce the pre-slice
    stream — no Remove Entities when B walks out, no second Add Entity when it
    comes back — so the gated arm's packets are the gate's, not a coincidence
    of the scene.

The server's shutdown line is read from its log as well: the gated run must say
it built fewer client writes than payloads and report a peak of two viewers,
which is the load-independent statement of what the gate did (`docs/
MOB_VISIBILITY.md`).

Run: mob_visibility_test.py /path/to/plusmc
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

# LivingEntity.DATA_HEALTH_ID and EntityDataSerializers.FLOAT: the health word
# the pairing burst carries.
MOB_HEALTH_INDEX = 9
FLOAT_SERIALIZER = 3

# The flat capture's surface (the same y tests/mob_roster_test.py stands on).
PLATFORM = (8.5, -60.0, 8.5)
# Beyond the shipped 48-block range and its 56-block drop line, with room for the
# pig to wander while B is away.
AWAY = PLATFORM[0] + 70.0

PIG_TYPE = 101  # src/world/mob_table.h, "minecraft:pig"

START = time.time()


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}", flush=True)


# --- framing helpers (the same decoding the other wire tests use) ------------


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


def read_entity_data(body, wanted=None):
    """[varint eid] then [u8 index][varint serializer][value]... to 0xFF."""
    eid, pos = rd_varint(body, 0)
    if wanted is not None and eid != wanted:
        return None
    values = {}
    while True:
        index = body[pos]
        pos += 1
        if index == 0xFF:
            break
        serializer, pos = rd_varint(body, pos)
        if serializer == FLOAT_SERIALIZER:
            values[index] = struct.unpack(">f", body[pos:pos + 4])[0]
            pos += 4
        else:
            raise AssertionError(f"mob pairing data: unexpected serializer {serializer}")
    assert pos == len(body), f"set_entity_data: {len(body) - pos} trailing byte(s)"
    return eid, values


def read_remove_entities(body):
    """: [varint count][varint eid]..."""
    count, pos = rd_varint(body, 0)
    ids = []
    for _ in range(count):
        eid, pos = rd_varint(body, pos)
        ids.append(eid)
    assert pos == len(body), f"remove_entities: {len(body) - pos} trailing byte(s)"
    return ids


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
    """Walks a client to `x` with real Move Player Position packets, the way a
    client reports its own movement — which is what the sweep reads."""
    for _ in range(steps):
        bot.move_to(x, PLATFORM[1], PLATFORM[2])
        time.sleep(0.15)


def summon_pig(bot, timeout=3.0):
    """`/summon pig 1`, returning its Add Entity once it arrives."""
    bot.send(SB_CHAT_COMMAND, varint(len("summon pig 1")) + b"summon pig 1")
    state = {"spawn": None}

    def stop(fresh):
        for spawn in spawns(fresh):
            if spawn["type"] == PIG_TYPE:
                state["spawn"] = spawn
        return state["spawn"] is not None and "Summoned 1 minecraft:pig" in feedback(fresh)

    fresh = pump(bot, timeout=timeout, stop=stop)
    assert state["spawn"] is not None, (
        f"{bot.name}: /summon pig produced no Add Entity (feedback {feedback(fresh)!r})")
    assert "Summoned 1 minecraft:pig" in feedback(fresh), feedback(fresh)
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
    label = "gate on" if gated else "PLUSMC_MOB_VISIBILITY=0"
    step(f"== arm: {label} ==")
    tmp = tempfile.mkdtemp(prefix="plusmc-mobvis-")
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
        env["PLUSMC_MOB_VISIBILITY"] = "0"

    log = []
    proc = None
    try:
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "mob visibility",
             "--offline-mode", "--gamemode", "1", "--data", str(data_root),
             "--world", str(world), "--no-flat-world", "--view-distance", "3"],
            lambda p: wait_listening(p, log), env=env, text=True)
        step(f"server listening on {port}")

        a = Bot("MobWatchA", port)
        b = Bot("MobWatchB", port)
        wire_client.record(a, note=f"mob-visibility: {label} — the summoner")
        wire_client.record(b, note=f"mob-visibility: {label} — the viewer who walks")
        a.join()
        b.join()
        drain(a, 1.5)
        drain(b, 1.5)
        command(a, f"tp {PLATFORM[0]} {PLATFORM[1]} {PLATFORM[2]}")
        command(b, f"tp {PLATFORM[0] + 1.0} {PLATFORM[1]} {PLATFORM[2]}")
        drain(a, 0.4)
        drain(b, 0.4)

        # --- 1. a summon beside both: both are paired ------------------------
        step("-> A summons a pig between them; both must get the pairing burst")
        pig = summon_pig(a)
        eid = pig["entity"]
        pair = pump(b, timeout=2.0, stop=lambda f: any(
            s["entity"] == eid for s in spawns(f)))
        assert any(s["entity"] == eid and s["type"] == PIG_TYPE for s in spawns(pair)), (
            f"B never got Add Entity for the pig A summoned (saw {spawns(pair)})")
        health = [read_entity_data(body, eid) for body in all_of(pair, CB_SET_ENTITY_DATA)]
        assert any(values and MOB_HEALTH_INDEX in values[1] for values in health), (
            f"B got the pig's Add Entity without its health word: {health}")
        step(f"   OK both clients were paired with entity {eid} (type {PIG_TYPE})")

        # --- 2. B walks out: one remove, and only for B ----------------------
        step(f"-> B walks to x={AWAY:.1f}; A stays beside the pig")
        move(b, AWAY)
        if gated:
            gone = pump(b, timeout=2.0, stop=lambda f: eid in removes(f))
            assert eid in removes(gone), (
                f"B walked out of range but was never told to remove entity {eid} "
                f"(removes: {removes(gone)})")
            assert removes(gone).count(eid) == 1, (
                f"B got {removes(gone).count(eid)} removes for entity {eid}, wanted one")
        else:
            # B still gets Remove Entities for *A* here — the player gate is
            # already on and B has left A's range — so the arm's assertion is
            # about the mob's id only.
            gone = pump(b, timeout=1.2)
            assert eid not in removes(gone), (
                f"PLUSMC_MOB_VISIBILITY=0 removed the mob {eid} from a far viewer: "
                f"{removes(gone)}")
        # A is still beside the mob and must not have been told to drop it.
        still_there = pump(a, timeout=0.6)
        assert eid not in removes(still_there), (
            f"A, beside the mob, was told to remove entity {eid}: {removes(still_there)}")
        step("   OK B's remove stream carried no mob remove; A got none" if not gated
             else f"   OK B got exactly one Remove Entities for {eid}; A got none")

        # --- 3. standing still: no second remove, no re-add ------------------
        step("-> B stands still out of range: the sweep must be idempotent")
        quiet = pump(b, timeout=1.2)
        assert eid not in removes(quiet), f"B was told to remove {eid} twice: {removes(quiet)}"
        assert not any(s["entity"] == eid for s in spawns(quiet)), (
            f"B was re-paired with {eid} while still out of range")
        step("   OK no remove and no re-add")

        # --- 4. B walks back: the pairing burst again -----------------------
        step("-> B walks back; the relationship has to start again")
        move(b, PLATFORM[0] + 1.0)
        if gated:
            back = pump(b, timeout=2.5, stop=lambda f: any(
                s["entity"] == eid for s in spawns(f)))
            assert any(s["entity"] == eid for s in spawns(back)), (
                f"B came back inside the range but was never re-paired with {eid} "
                f"(spawns: {spawns(back)})")
            health_back = [read_entity_data(body, eid)
                           for body in all_of(back, CB_SET_ENTITY_DATA)]
            assert any(values and MOB_HEALTH_INDEX in values[1]
                       for values in health_back), (
                f"B's re-pairing carried no health word: {health_back}")
            step(f"   OK B was paired with {eid} again, health word included")
        else:
            # The pre-slice arm never ended the relationship, so there is no
            # second pairing burst for the server to send.
            back = pump(b, timeout=1.0)
            assert not any(s["entity"] == eid for s in spawns(back)), (
                f"PLUSMC_MOB_VISIBILITY=0 re-paired B with {eid} "
                f"(spawns: {spawns(back)})")
            step("   OK (pre-slice arm) no second pairing burst")

        # --- 5. a far summon reaches nobody (gated) / everyone (legacy) ------
        step("-> B walks away and stays away; A summons a second pig")
        move(b, AWAY)
        if gated:
            pump(b, timeout=2.0, stop=lambda f: eid in removes(f))
        second = summon_pig(a)
        step(f"   A summoned entity {second['entity']} at x={second['x']:.1f}")
        # A's own pairing for it was asserted inside summon_pig (the Add Entity
        # and the "Summoned 1 minecraft:pig" reply); what is left to read is
        # what the far-away viewer did or did not get.
        b_window = pump(b, timeout=1.5)
        if gated:
            assert not any(s["entity"] == second["entity"] for s in spawns(b_window)), (
                f"B, 70 blocks away, was told about the new pig {second['entity']}")
            step(f"   OK {second['entity']} reached A and not B")
        else:
            # The pre-slice arm: every spawn went to every connection.
            assert any(s["entity"] == second["entity"] for s in spawns(b_window)), (
                f"PLUSMC_MOB_VISIBILITY=0 did not reach B with {second['entity']}")
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
        report = [line for line in text.splitlines() if "shutdown: mob tracking:" in line]
        assert report, f"the server printed no mob tracking line:\n{text[-2000:]}"
        line = report[0]
        m = re.search(r"(\d+) payloads built, (\d+) client writes \(([\d.]+) per payload\), "
                      r"(\d+) pairings, peak (\d+) viewers", line)
        assert m, f"mob tracking line has an unexpected shape: {line!r}"
        payloads, writes, per, pairings, peak = (m.group(1), int(m.group(2)),
                                                 float(m.group(3)), int(m.group(4)),
                                                 int(m.group(5)))
        step(f"   shutdown: {line.split('shutdown: ', 1)[1]}")
        if gated:
            assert peak >= 2, f"a mob's viewer set never held both players (peak {peak})"
            # One per relationship: the first pig paired with A and with B, B's
            # re-entry started a third, and the second pig a fourth.
            assert pairings >= 4, (
                f"only {pairings} relationships started; the summon, the re-entry "
                f"and the second summon alone are four")
        else:
            assert peak == 0, (
                f"PLUSMC_MOB_VISIBILITY=0 still maintained viewer sets (peak {peak})")
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
    print("mob-visibility: both arms passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
