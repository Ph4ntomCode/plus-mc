#!/usr/bin/env python3
"""Integration test: natural mob spawning over the wire (Phase 4 milestone C).

`NaturalSpawner` (world/mob_spawn.{h,cpp}) is unit-tested without a server
(tests/mob_spawn_test.cpp); what this file proves is the *driver* — the
`ServerChunkCache.tickChunks` half in server/session.cpp — against a real world
and a real client.

The world is the committed capture (tests/fixtures/r.0.0.mca: chunk (0,0) of the
seed-42 flat world), so the platform has vanilla's own terrain, light and *biome*
(the chunk's `minecraft:plains`, whose monster list is nine entries — see
data/vanilla/26.2/biome_spawns.tsv). That matters twice over: a chunk this server
generated from nothing carries `minecraft:the_void`, which lists no mobs at all,
and a monster needs a biome that names it.

Four observations, each of which can only come from the pass running:

  1. **Daylight spawns nothing on the platform.** The bot stands 25 blocks west
     of chunk (0,0) — outside the 24-block `MIN_SPAWN_DISTANCE`, inside the
     128-block candidate radius. At world time 0 (`sky_darken` 0) the open
     surface is `raw_brightness` 15, and `Monster.isDarkEnoughToSpawn` refuses
     it however the draws fall.
  2. **Midnight spawns monsters on it.** `/time set midnight` moves only the
     clock: `sky_darken` 11 leaves the surface at `raw_brightness` 4, inside the
     dimension's `monster_spawn_light_level` of 0..7. Every spawn that lands is a
     monster (zombie or spider — the only createable entries of the biome's
     monster list), on the platform's first free cell (y = -60), inside the
     chunk, and at least 24 blocks from the player.
  3. **Daylight stops it again.** The same clock back at noon, and no *new*
     spawn appears — the gate, not a one-off.
  4. **The 24-block rule.** With the bot standing *on* the platform every cell
     is inside 24 blocks, and midnight spawns nothing there.

`/mobs` and `/mobs kill` bracket it: the population the pass created is what the
command reports, and its removal is a `remove_entities` for exactly the ids the
spawn packets named.

Run: spawn_wire_test.py /path/to/plusmc [/path/to/tests/fixtures]
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
CB_KEEP_ALIVE = 0x2D
CB_PLAYER_POSITION = 0x49
CB_SYSTEM_CHAT = 0x7C

# Serverbound packet ids.
SB_CHAT_COMMAND = 0x07
SB_POSITION = 0x1E
SB_KEEP_ALIVE = 0x1C

# Entity type ids (src/world/mob_table.h): the two monsters `minecraft:plains`
# lists that this engine can create.
ET_SPIDER = 127
ET_ZOMBIE = 154
MONSTERS = (ET_SPIDER, ET_ZOMBIE)

# The flat capture's surface: four solid layers ending in grass at y = -61, so
# the first free cell — and the only y a monster can spawn at — is y = -60.
SURFACE_Y = -60
# The bot watches chunk (0, 0) from 25 blocks west: beyond the 24-block minimum
# (which is measured in x/z too) and well inside the 128-block candidate radius.
WATCH_POSITION = (-25.5, float(SURFACE_Y), 8.5)
# ... and stands on the platform for the distance-gate control.
ON_PLATFORM = (8.5, float(SURFACE_Y), 8.5)

# How long a negative control watches for a spawn it must not see, and how long
# the positive one waits for one. The pass runs once per world tick (50 ms), and
# a midnight attempt on this platform lands roughly one spawn every few ticks,
# so the positive budget is generous only because the machine may be loaded.
NEGATIVE_SECONDS = 4.0
POSITIVE_SECONDS = 30.0


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
    # LpVec3 (src/protocol/lp_vec.h): a single 0 byte is the zero vector;
    # otherwise two marker bytes, a big-endian u32, and — when the marker's low
    # three bits say the scale did not fit — a scale varint on the end. A flat
    # six bytes mis-frames the pitch/yaw/data that follow it.
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
    """remove_entities: [varint count][varint eid]..."""
    count, pos = rd_varint(body, 0)
    out = []
    for _ in range(count):
        eid, pos = rd_varint(body, pos)
        out.append(eid)
    assert pos == len(body), f"remove_entities: {len(body) - pos} trailing byte(s)"
    return out


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

    def move_to(self, x, y, z, ground=1):
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([ground]))


def drain(bot, timeout=1.0):
    """Wait `timeout` seconds and hand back what the reader collected, appending
    it to the session log so a later observation can search past its own setup."""
    out = wire_client.drain(bot, timeout)
    with bot.lock:
        bot.seen += out
    return out


def command(bot, text, timeout=0.6):
    """Send a chat command and return its window's packets."""
    bot.send(SB_CHAT_COMMAND, varint(len(text)) + text.encode())
    return drain(bot, timeout)


def monster_spawns(packets):
    out = []
    for body in all_of(packets, CB_SPAWN_ENTITY):
        spawn = read_spawn(body)
        if spawn["type"] in MONSTERS:
            out.append(spawn)
    return out


def watch_for_spawn(bot, seconds, mark):
    """Drain in slices until a monster spawn arrives, returning every monster
    spawn seen since `mark` (which may be more than one)."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        drain(bot, 0.4)
        found = monster_spawns(bot.seen[mark:])
        if found:
            return found
    return []


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

    def pump():
        for raw in iter(proc.stdout.readline, b""):
            sys.stderr.write(raw.decode(errors="replace"))

    threading.Thread(target=pump, daemon=True).start()
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


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc [/path/to/tests/fixtures]",
              file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    fixtures = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else \
        pathlib.Path(__file__).resolve().parent / "fixtures"
    data_root = pathlib.Path(__file__).resolve().parent / ".." / "data" / "vanilla"

    tmp = tempfile.mkdtemp(prefix="plusmc-spawn-")
    world = pathlib.Path(tmp) / "world"
    region_dir = world / "dimensions" / "minecraft" / "overworld" / "region"
    region_dir.mkdir(parents=True)
    shutil.copyfile(fixtures / "r.0.0.mca", region_dir / "r.0.0.mca")

    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    # The world clock is frozen at 0 (dawn, `sky_darken` 0), and the two
    # observations move it with `/time set` — so the *only* thing that changes
    # between them is the light the spawn gate reads.
    env["PLUSMC_DAY_CYCLE"] = "0"
    # The natural spawn pass is on for this test, explicitly (it is on by
    # default; a test that wants no spawn traffic turns it off).
    env["PLUSMC_MOB_SPAWNING"] = "1"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "spawn test",
             "--offline-mode", "--gamemode", "1", "--data", str(data_root),
             "--world", str(world), "--no-flat-world", "--view-distance", "3"],
            wait_listening, env=env)
        step(f"server listening on {port}")

        bot = Bot("SpawnBot", port)
        wire_client.record(bot, note="spawn-wire: the natural spawn pass over a flat plains chunk")
        bot.join()
        drain(bot, 1.5)
        step("  joined (creative: nothing here can be attacked by what spawns)")

        # The platform is vanilla's own plains chunk; the bot watches it from 25
        # blocks west, which is the near edge of the band a spawn is allowed in.
        command(bot, f"tp {WATCH_POSITION[0]} {WATCH_POSITION[1]} {WATCH_POSITION[2]}")
        drain(bot, 0.6)

        # --- 1. daylight -------------------------------------------------------
        step("-> the open platform at world time 0 (raw brightness 15)")
        mark = len(bot.seen)
        spawns = watch_for_spawn(bot, NEGATIVE_SECONDS, mark)
        assert not spawns, f"a monster spawned in daylight: {spawns}"
        step(f"  OK no monster spawned in {NEGATIVE_SECONDS}s of daylight "
             "(an animal may: `Animal.isBrightEnoughToSpawn` ignores the clock)")

        # --- 2. midnight -------------------------------------------------------
        step("-> /time set midnight (raw brightness 4, inside the 0..7 spawn level)")
        packets = command(bot, "time set midnight")
        assert "Set time to 18000" in feedback(packets), feedback(packets)
        mark = len(bot.seen)
        spawns = watch_for_spawn(bot, POSITIVE_SECONDS, mark)
        assert spawns, f"no monster spawned in {POSITIVE_SECONDS}s of midnight"
        for s in spawns:
            assert s["type"] in MONSTERS, s
            assert abs(s["y"] - SURFACE_Y) < 1e-9, f"spawned off the platform: {s}"
            assert 0.0 <= s["x"] <= 16.0 and 0.0 <= s["z"] <= 16.0, \
                f"spawned outside the watched chunk: {s}"
            dist = ((s["x"] - WATCH_POSITION[0]) ** 2 + (s["z"] - WATCH_POSITION[2]) ** 2) ** 0.5
            assert dist > 24.0, f"spawned inside the 24-block minimum: {s} ({dist:.1f})"
        names = sorted({"zombie" if s["type"] == ET_ZOMBIE else "spider" for s in spawns})
        step(f"  OK {len(spawns)} monster(s) spawned on the platform, {', '.join(names)}")
        step(f"     first at ({spawns[0]['x']:.1f}, {spawns[0]['y']:.1f}, {spawns[0]['z']:.1f})")

        # --- 3. back to daylight ----------------------------------------------
        step("-> /time set noon: the same platform, the same player, only the clock")
        packets = command(bot, "time set noon")
        assert "Set time to 6000" in feedback(packets), feedback(packets)
        mark = len(bot.seen)
        spawns = watch_for_spawn(bot, NEGATIVE_SECONDS, mark)
        assert not spawns, f"the light gate stopped closing at noon: {spawns}"
        step("  OK daylight closed the gate again")

        # --- 4. the population, and the 24-block rule --------------------------
        # The tally the command prints is the registry the pass has been filling,
        # and it is checked against the spawn packets this session actually saw:
        # `/mobs kill`'s removal has to be a subset of them.
        spawned_ids = set()
        for body in all_of(bot.seen, CB_SPAWN_ENTITY):
            spawned_ids.add(read_spawn(body)["entity"])
        assert spawned_ids, "no entity was ever spawned"

        packets = command(bot, "mobs", timeout=1.0)
        assert "minecraft:" in feedback(packets), feedback(packets)
        step(f"  /mobs reports: {feedback(packets).strip()[:100]}")

        step("-> /mobs kill, then midnight with the player standing on the platform")
        packets = command(bot, "mobs kill", timeout=1.0)
        removed = []
        for body in all_of(packets, CB_REMOVE_ENTITIES):
            removed.extend(read_remove_entities(body))
        assert removed, "/mobs kill removed nothing"
        unknown = [eid for eid in removed if eid not in spawned_ids]
        assert not unknown, f"/mobs kill removed entities this session never saw: {unknown}"
        step(f"  OK /mobs kill despawned {len(removed)} entity(ies), every id one the wire "
             "named a spawn for")
        # (The registry is *not* asserted empty afterwards: the pass runs every
        # tick, and at midnight the 400-tick persistent pass is active too, so a
        # fresh animal can arrive inside the command's own drain.)

        command(bot, "time set midnight")
        command(bot, f"tp {ON_PLATFORM[0]} {ON_PLATFORM[1]} {ON_PLATFORM[2]}")
        drain(bot, 0.6)
        mark = len(bot.seen)
        spawns = watch_for_spawn(bot, NEGATIVE_SECONDS, mark)
        assert not spawns, f"a monster spawned within 24 blocks of the player: {spawns}"
        step(f"  OK nothing spawned with the player on the platform "
             f"({NEGATIVE_SECONDS}s of midnight)")

        bot.close()
        print("spawn: all steps passed")
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
