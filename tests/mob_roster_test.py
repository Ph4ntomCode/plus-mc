#!/usr/bin/env python3
"""Integration test: the whole mob roster summoned over the real wire protocol.

`src/world/mob_table.h` is generated (tools/gen_mob_table.py) and unit-tested
(tests/mob_test.cpp): 90 rows carrying the game's own entity-type id, box,
health, speed, attack damage and flags. What no unit test can show is that the
*server* accepts each of those 90 names and puts the right entity on the wire —
that the protocol id in the table is the one the Add Entity packet carries and
that the mob arrives with the health the table gives it. That is this file's job.

The roster is read out of the header itself rather than hard-coded, so a table
that drifts from the engine fails here rather than silently passing:

  * the header's `kMobTypeCount` must equal the number of parsed rows (90), and
    the rows must be unique by name and by entity id,
  * for every row, `/summon <name>` answers `Summoned 1 <name>`, then an Add
    Entity (0x01) carrying that row's own entity_type_id, at the summoning ring
    (two blocks from the player, at the player's own y), followed by the health
    word — Set Entity Data (0x63), LivingEntity index 9, a FLOAT — equal to the
    row's max_health,
  * `/mobs kill` then removes exactly that entity, so each iteration starts from
    an empty world and one summon cannot leak into the next check.

A mixed batch (a monster, a retaliator and a drop-less passive) proves `/mobs`
tallies by name, and `/summon <nonsense>` proves the "known:" hint really
carries all 90 names — the hint walks the same kMobTypeCount the loop above
exercises one row at a time.

The world is the committed flat capture (tests/fixtures/r.0.0.mca) so every mob
has ground to stand on; natural spawning is off (`PLUSMC_MOB_SPAWNING=0`) so the
population under test is exactly what this test summoned; the clock is frozen
and the player is creative, so nothing burns, spawns or fights between a summon
and its kill. What this does *not* prove is AI: a Warden that can be summoned is
not a Warden that behaves. The flags and the unmodelled-goal list in the header
are the honest statement about that.

Run: mob_roster_test.py /path/to/plusmc
"""
import math
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

# LivingEntity.DATA_HEALTH_ID and EntityDataSerializers.FLOAT, the pair the
# spawn burst's health word is written with. Villagers additionally carry
# Villager.DATA_VILLAGER_DATA (index 19, a three-varint VILLAGER_DATA triple)
# and DATA_VILLAGER_DATA_FINALIZED (index 20, a BOOLEAN), which vanilla also
# writes at spawn.
MOB_HEALTH_INDEX = 9
FLOAT_SERIALIZER = 3
BOOLEAN_SERIALIZER = 8
VILLAGER_DATA_SERIALIZER = 18

TABLE = pathlib.Path(__file__).resolve().parent.parent / "src" / "world" / "mob_table.h"
# The flat capture's surface (the same y tests/spawn_wire_test.py stands on).
PLATFORM = (8.5, -60.0, 8.5)

# One generated row: name, display name, entity id, category, flags, fire
# immunity, width, height, baby width, baby height, max health, ... — only the
# leading columns this test needs are read.
ROW = re.compile(
    r'^\s*\{"(?P<name>minecraft:[a-z_]+)",\s*"(?P<display>[^"]+)",\s*(?P<id>\d+),\s*'
    r'kSpawn\w+,\s*[^,]+,\s*(?:true|false),\s*(?P<w>[0-9.]+)f,\s*(?P<h>[0-9.]+)f,\s*'
    r'(?P<bw>[0-9.]+)f,\s*(?P<bh>[0-9.]+)f,\s*(?P<health>[0-9.]+)f,')

ROSTER_SIZE = 90


def read_table():
    """Every mob row in the generated header, in the header's own order."""
    rows = []
    count = None
    text = TABLE.read_text()
    for line in text.splitlines():
        m = re.search(r"kMobTypeCount = (\d+);", line)
        if m:
            count = int(m.group(1))
        m = ROW.match(line)
        if m:
            rows.append({"name": m.group("name"), "display": m.group("display"),
                         "id": int(m.group("id")), "health": float(m.group("health"))})
    return rows, count


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
    # LpVec3 (src/protocol/lp_vec.h): a single 0 byte means a zero vector;
    # otherwise two marker bytes, a big-endian u32, and — when the marker's low
    # three bits say the scale did not fit — a scale varint on the end.
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
    """[varint eid] then [u8 index][varint serializer][value]... to 0xFF.

    A mob spawn's health word is a FLOAT; villagers also carry the two words
    above (the VILLAGER_DATA triple and the finalized BOOLEAN), so those
    serializers are decoded too. An index this decoder does not know is a
    failure rather than a silent skip.
    """
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
        elif serializer == 1:  # INT
            value, pos = rd_varint(body, pos)
            values[index] = value
        elif serializer == BOOLEAN_SERIALIZER:
            values[index] = body[pos] != 0
            pos += 1
        elif serializer == VILLAGER_DATA_SERIALIZER:
            etype, pos = rd_varint(body, pos)
            profession, pos = rd_varint(body, pos)
            level, pos = rd_varint(body, pos)
            values[index] = (etype, profession, level)
        else:
            raise AssertionError(f"entity data index {index}: serializer {serializer}")
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
    """The literal SystemChats (command replies)."""
    out = []
    for body in all_of(packets, CB_SYSTEM_CHAT):
        try:
            out.append(chat_text(body))
        except AssertionError:
            pass
    return " ".join(out)


class Bot:
    """A client that always reads (the reader thread owns the socket, so a
    windowed drain cannot desync the stream)."""

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


def drain(bot, timeout=1.0):
    out = wire_client.drain(bot, timeout)
    with bot.lock:
        bot.seen += out
    return out


def pump(bot, timeout=2.0, stop=None, slice_s=0.05):
    """Drain in slices until `stop(packets)` says the reply is complete (or the
    timeout runs out), returning every packet this call collected."""
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


START = time.time()


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}", flush=True)


def wait_listening(proc, timeout=20):
    deadline = time.time() + timeout
    port = None
    while time.time() < deadline:
        line = proc.stdout.readline().decode(errors="replace")
        if line:
            sys.stderr.write(line)
            m = re.search(r"listening on.*:(\d+)", line)
            if m:
                port = int(m.group(1))
                break

    def pump_log():
        for raw in iter(proc.stdout.readline, b""):
            sys.stderr.write(raw.decode(errors="replace"))

    threading.Thread(target=pump_log, daemon=True).start()
    if port is None:
        raise RuntimeError("server did not start")
    for _ in range(30):
        try:
            s = __import__("socket").create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            break
        except OSError:
            time.sleep(0.1)
    return port


def check_table(rows, count):
    """The generated table's own invariants, before a single packet is sent."""
    assert rows, f"no mob rows parsed out of {TABLE}"
    assert count == len(rows) == ROSTER_SIZE, (
        f"kMobTypeCount {count} / {len(rows)} parsed rows, wanted {ROSTER_SIZE}")
    names = [r["name"] for r in rows]
    ids = [r["id"] for r in rows]
    assert len(set(names)) == len(names), "the table has duplicate names"
    assert len(set(ids)) == len(ids), "the table has duplicate entity ids"
    # The command's own hint is the table walk; the unknown-name check below
    # reads it off the wire, and these are the probes a human will look for.
    for probe in ("minecraft:allay", "minecraft:copper_golem", "minecraft:ender_dragon",
                  "minecraft:warden", "minecraft:camel_husk"):
        assert probe in names, f"the expansion is missing {probe}"


def summon(bot, row, at):
    """One `/summon <name>`: returns the Add Entity dict and its health word."""
    px, py, pz = at
    text = f"summon {row['name']}"
    bot.send(SB_CHAT_COMMAND, varint(len(text)) + text.encode())
    state = {"spawn": None, "health": None}

    def stop(fresh):
        if state["spawn"] is None:
            for body in all_of(fresh, CB_SPAWN_ENTITY):
                spawn = read_spawn(body)
                if spawn["type"] == row["id"]:
                    state["spawn"] = spawn
        if state["spawn"] is not None and state["health"] is None:
            for body in all_of(fresh, CB_SET_ENTITY_DATA):
                parsed = read_entity_data(body, state["spawn"]["entity"])
                if parsed and MOB_HEALTH_INDEX in parsed[1]:
                    state["health"] = parsed[1][MOB_HEALTH_INDEX]
        # The reply is part of the stop condition, as it is in `despawn`: the
        # entity packets and the command's own chat are separate frames, and a
        # 0.05 s slice ending between them used to return without the chat. The
        # server had sent it — a transcript of a guest run holds every
        # "Summoned 1 minecraft:..." line, including the one this assertion
        # reported as missing — so waiting for it is what the assertion means.
        return (state["spawn"] is not None and state["health"] is not None
                and f"Summoned 1 {row['name']}" in feedback(fresh))

    fresh = pump(bot, timeout=3.0, stop=stop)
    reply = feedback(fresh)
    assert f"Summoned 1 {row['name']}" in reply, f"{row['name']}: {reply!r}"
    spawn = state["spawn"]
    assert spawn is not None, (
        f"{row['name']}: no Add Entity with type {row['id']} "
        f"(saw {[read_spawn(b)['type'] for b in all_of(fresh, CB_SPAWN_ENTITY)]})")
    assert state["health"] is not None, f"{row['name']}: no health word"
    # The summoning ring: two blocks out, at the summoner's own y.
    distance = math.hypot(spawn["x"] - px, spawn["z"] - pz)
    assert abs(distance - 2.0) < 0.01, f"{row['name']}: ring distance {distance:.3f}"
    assert abs(spawn["y"] - py) < 0.01, f"{row['name']}: y {spawn['y']} != {py}"
    # The health the client is told is the table's own max_health, except the
    # wither: its encounter starts the spawn shield at 1 health (wither-fight),
    # so the spawn burst carries 1 even though the table's row is 300.
    expected_health = 1.0 if row["name"] == "minecraft:wither" else row["health"]
    assert abs(state["health"] - expected_health) < 1e-3, (
        f"{row['name']}: health word {state['health']} != expected {expected_health}")
    return spawn


def despawn(bot, eid, row):
    """`/mobs kill`: the one entity of this iteration, removed at once."""
    bot.send(SB_CHAT_COMMAND, varint(len("mobs kill")) + b"mobs kill")

    def stop(fresh):
        removed = any(eid in read_remove_entities(b)
                      for b in all_of(fresh, CB_REMOVE_ENTITIES))
        return removed and "Removed" in feedback(fresh)

    fresh = pump(bot, timeout=3.0, stop=stop)
    removed = [e for b in all_of(fresh, CB_REMOVE_ENTITIES)
               for e in read_remove_entities(b)]
    assert removed == [eid], f"{row['name']}: /mobs kill removed {removed}, wanted [{eid}]"
    assert "Removed 1 entities" in feedback(fresh), feedback(fresh)


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    fixtures = pathlib.Path(__file__).resolve().parent / "fixtures"
    data_root = pathlib.Path(__file__).resolve().parent / ".." / "data" / "vanilla"

    rows, count = read_table()
    check_table(rows, count)
    step(f"table: {len(rows)} mobs parsed from src/world/mob_table.h")

    tmp = tempfile.mkdtemp(prefix="plusmc-roster-")
    world = pathlib.Path(tmp) / "world"
    region_dir = world / "dimensions" / "minecraft" / "overworld" / "region"
    region_dir.mkdir(parents=True)
    shutil.copyfile(fixtures / "r.0.0.mca", region_dir / "r.0.0.mca")

    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    # The world clock is frozen and natural spawning is off: the population this
    # test reads is exactly what it summoned, so a stray animal can neither add a
    # spawn packet nor survive a `/mobs kill` with it.
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_MOB_SPAWNING"] = "0"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "roster test",
             "--offline-mode", "--gamemode", "1", "--data", str(data_root),
             "--world", str(world), "--no-flat-world", "--view-distance", "3"],
            wait_listening, env=env)
        step(f"server listening on {port}")

        bot = Bot("RosterBot", port)
        wire_client.record(bot, note="mob-roster: every mob summoned, pinged and despawned")
        bot.join()
        drain(bot, 1.5)
        # The teleport target is where every summon ring will be centred; the
        # server replies to `/tp` with feedback, not another player_position, so
        # the known target is the honest source for this (the ring assertions in
        # summon() are what would catch a drifted player).
        command(bot, f"tp {PLATFORM[0]} {PLATFORM[1]} {PLATFORM[2]}")
        drain(bot, 0.4)
        at = PLATFORM
        step(f"  joined creative at ({at[0]:.1f}, {at[1]:.1f}, {at[2]:.1f})")

        for i, row in enumerate(rows):
            spawn = summon(bot, row, at)
            despawn(bot, spawn["entity"], row)
            step(f"  [{i + 1:2d}/{len(rows)}] {row['name']:<32} id {row['id']:>3} "
                 f"health {row['health']:.0f} -> entity {spawn['entity']}")

        # --- /mobs tallies by name -------------------------------------------
        step("-> a mixed batch: a monster, a retaliator and a drop-less passive")
        batch = [r for r in rows
                 if r["name"] in ("minecraft:zombie", "minecraft:wolf", "minecraft:allay")]
        assert len(batch) == 3, [r["name"] for r in batch]
        live = [summon(bot, row, at) for row in batch]
        reply = feedback(command(bot, "mobs", timeout=1.0))
        for row in batch:
            assert f"{row['name']} x1" in reply, f"{row['name']} missing from {reply!r}"
        reply = feedback(command(bot, "mobs kill", timeout=1.0))
        assert "Removed 3 entities" in reply, reply
        assert len({s["entity"] for s in live}) == 3, live
        step(f"  OK /mobs reported {', '.join(r['name'] for r in batch)} and removed all 3")

        # --- the unknown-name hint is the whole table -------------------------
        step("-> /summon <nonsense>: the 'known:' hint must carry all 90 names")
        reply = feedback(command(bot, "summon definitely_not_a_mob", timeout=1.0))
        assert "Unknown entity" in reply, reply
        missing = [row["name"] for row in rows if row["name"] not in reply]
        assert not missing, f"the hint is missing {missing[:5]} (+{len(missing) - 5})"
        step("  OK every table name is in the hint")

        reply = feedback(command(bot, "mobs", timeout=1.0))
        assert "No mobs are loaded" in reply, reply
        step("  OK the world is empty again")

        bot.close()
        print("mob-roster: all steps passed")
        return 0
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
