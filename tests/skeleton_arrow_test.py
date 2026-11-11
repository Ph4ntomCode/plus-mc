#!/usr/bin/env python3
"""Integration test: a skeleton's bow over the wire (Phase 4 follow-up).

The mob roster carried the skeleton family for three phases with their signature
ability explicitly listed as *not modelled*: `mob_table.h` said so of `skeleton`,
`stray`, `bogged`, `parched` and `wither_skeleton`. They walked, were hit, died
and dropped loot like every other monster, and never once drew a bow.

`RangedBowAttackGoal` is now the mob tick's sixth goal (`world/mob.cpp`): inside
fifteen blocks, with the target visible, a skeleton family mob stands still,
faces the player and looses an arrow every twenty ticks. The arrow is not a new
entity path — it is the same `Projectile` a player's bow spawns, with the mob as
its owner, so this test drives the whole shared chain:

  1. **A pig does not shoot.** A non-archer summoned at the same distance for the
     same time produces no arrow at all: the control that separates "a projectile
     spawned" from "the ranged goal ran".
  2. **A skeleton shoots.** `/summon skeleton 1` puts it two blocks east of the
     bot; the next target scan sees the player and the first arrow is an Add
     Entity of type 6 (`minecraft:arrow`) from the skeleton's own eye.
  3. **The arrow flies.** Its position syncs advance, which only a ticking body
     produces.
  4. **It hurts.** The queued player hit lands on the bot's packet clock
     (`tick` sends one `flying` per game tick) and `set_health` drops by the
     arrow's launch-speed-scaled damage (its base 2.0 times `|v|`, with no
     bow enchantments) — the same `minecraft:arrow` damage a player's shot
     deals. The drop is read from the hit's own packet: the point-blank arrow
     of step 2 arrives before the teleport, so the bot is not at full health
     here and the run's earlier damage must not be billed to this arrow.

Run: skeleton_arrow_test.py /path/to/plusmc
"""
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

CB_SPAWN_ENTITY = 0x01
CB_SET_ENTITY_DATA = 0x65
CB_ENTITY_POSITION_SYNC = 0x23
CB_SET_HEALTH = 0x6A
CB_KEEP_ALIVE = 0x2D

SB_CHAT_COMMAND = 0x07
SB_KEEP_ALIVE = 0x1C
SB_POSITION = 0x1E
SB_FLYING = 0x21

# minecraft:entity type protocol ids (src/protocol/ids.h).
ET_ARROW = 6
ET_PIG = 101
ET_SKELETON = 118

# ItemEntity/arrow's spawn is an ordinary Add Entity, so the test decodes it the
# same way the item test does.
STAND_Y = 64.0
# The 9x9 floor the summon ring needs, plus a corridor north (+z) so the bot can
# step eleven blocks out and watch an arrow fly: the ring drops every mob two
# blocks from the summoner, where an arrow reaches the player on its very first
# tick and there is no flight to see.
PLATFORM = ([(x, 63, z) for x in range(-4, 5) for z in range(-4, 5)] +  # 9x9
            [(x, 63, z) for x in range(-2, 3) for z in range(5, 13)])    # corridor


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def rd_varint(buf, pos=0):
    value = 0
    shift = 0
    while True:
        b = buf[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        if not (b & 0x80):
            return value, pos
        shift += 7


def read_spawn(body):
    """Add Entity: [varint eid][uuid 16][varint type][f64 x y z][LpVec3]..."""
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
    data, pos = rd_varint(body, pos)
    assert pos == len(body), f"spawn_entity: {len(body) - pos} trailing byte(s)"
    return {"entity": eid, "type": etype, "x": x, "y": y, "z": z, "data": data}


def read_position_sync(body):
    """26.3: [varint eid][varint PositionPath type = LINEAR 0][f64 x y z]
    [f32 yRot][f32 xRot][bool onGround]."""
    eid, pos = rd_varint(body, 0)
    path_type, pos = rd_varint(body, pos)
    assert path_type == 0, f"position_sync path type {path_type}"
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    pos += 24 + 8
    ground = body[pos]
    return {"entity": eid, "x": x, "y": y, "z": z, "ground": bool(ground)}


def read_set_health(body):
    health = struct.unpack(">f", body[0:4])[0]
    return round(health, 3)


class Bot:
    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(10)
        self.threshold = -1
        self.reader = wire_client.Reader(self.sock, self)
        self.entity_id = None

    def send(self, pid, body=b""):
        self.reader.send(pid, body)

    def recv(self):
        return read_packet(self.sock, self.threshold)

    def start_reader(self):
        self.reader.start()

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
                    ln, i = rd_varint(body, i)
                    ns, i = body[i:i + ln].decode(), i + ln
                    ln, i = rd_varint(body, i)
                    pk, i = body[i:i + ln].decode(), i + ln
                    ln, i = rd_varint(body, i)
                    ver, i = body[i:i + ln].decode(), i + ln
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

    def move_to(self, x, y, z):
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([1]))

    def tick(self, count=1):
        for _ in range(count):
            self.send(SB_FLYING, bytes([1]))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# Every `set_health` the bot receives, recorded as the *drop* it produced. A hit
# is one packet, so the step from the previous value is one arrow's damage;
# natural regeneration between hits only lifts the previous value, so it cannot
# shorten a drop. Step 4 reads this instead of comparing the last reading with
# 20: the point-blank arrow in step 2 lands on its very first tick, before the
# teleport, so the bot has already been hurt when step 4 starts.
HITS = []
last_health = 20.0


def drain(bot, timeout=0.5):
    global last_health
    packets = wire_client.drain_quiet(bot, quiet=0.2, limit=timeout)
    for pid, body in packets:
        if pid == CB_SET_HEALTH:
            health = read_set_health(body)
            if health < last_health:
                HITS.append(last_health - health)
            last_health = health
    return packets


def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def send_chat_command(bot, command):
    bot.send(SB_CHAT_COMMAND, varint(len(command)) + command.encode())


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
            socket.create_connection((HOST, port), timeout=1).close()
            break
        except OSError:
            time.sleep(0.1)
    return port


START = time.time()


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}", flush=True)


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1])
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-skeleton-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--offline-mode", "--gamemode", "0",
             "--data", data_root, "--world", world, "--no-flat-world"], wait_listening, env=env)
        step(f"server listening on {port}")

        bot = Bot("ArrowBot", port)
        bot.join()
        packets = drain(bot, 1.0)
        for body in all_of(packets, 0x86):  # update_attributes
            bot.entity_id, _ = rd_varint(body, 0)
        assert bot.entity_id is not None, "the join must send update_attributes"
        bot.move_to(0.5, STAND_Y, 0.5)
        time.sleep(0.2)

        # A 9x9 stone floor: the test world is empty, and a summon rings two
        # blocks out from the bot, so the skeleton and the pig both need ground.
        for cell in PLATFORM:
            send_chat_command(bot, f"setblock {cell[0]} {cell[1]} {cell[2]} stone")
        drain(bot, 3.0)
        send_chat_command(bot, "tp 0.5 64.0 0.5")
        drain(bot, 0.4)

        # --- 1. the control: a pig at the same distance never shoots ----------
        step("-> /summon pig 1 (the negative control)")
        send_chat_command(bot, "summon pig 1")
        packets = drain(bot, 1.0)
        pig = next((read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY)), None)
        assert pig is not None and pig["type"] == ET_PIG, pig
        for _ in range(45):
            bot.tick(2)
            packets = drain(bot, 0.15)
            arrows = [s for s in (read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY))
                      if s["type"] == ET_ARROW]
            assert not arrows, f"a pig shot an arrow: {arrows}"
        step("  OK 45 player ticks and no arrow from a non-archer")

        send_chat_command(bot, "mobs kill")
        drain(bot, 0.5)

        # --- 2. a skeleton shoots an arrow ------------------------------------
        step("-> /summon skeleton 1")
        send_chat_command(bot, "summon skeleton 1")
        packets = drain(bot, 1.0)
        skeleton = next((read_spawn(b) for b in all_of(packets, CB_SPAWN_ENTITY)
                         if read_spawn(b)["type"] == ET_SKELETON), None)
        assert skeleton is not None, (
            f"no skeleton spawn: {[read_spawn(b)['type'] for b in all_of(packets, CB_SPAWN_ENTITY)]}")
        assert abs(skeleton["x"] - 2.5) < 0.01 and abs(skeleton["z"] - 0.5) < 0.01, skeleton
        step(f"  OK skeleton {skeleton['entity']} at ({skeleton['x']:.2f}, {skeleton['z']:.2f})")

        arrow = None
        seen = []
        deadline = time.time() + 8.0
        while time.time() < deadline and arrow is None:
            bot.tick(2)
            batch = drain(bot, 0.2)
            seen += batch
            arrow = next((s for s in (read_spawn(b) for b in all_of(batch, CB_SPAWN_ENTITY))
                          if s["type"] == ET_ARROW), None)
        assert arrow is not None, (
            "the skeleton never loosed an arrow; spawn types seen: "
            f"{sorted({s['type'] for s in (read_spawn(b) for b in all_of(seen, CB_SPAWN_ENTITY))})}")
        # `AbstractArrow` spawns at the shooter's x/z and eye height - 0.1; the
        # skeleton's eye is `1.0 + 1.99 * 0.85` here.
        assert abs(arrow["x"] - skeleton["x"]) < 0.01 and abs(arrow["z"] - skeleton["z"]) < 0.01, arrow
        assert abs(arrow["y"] - (STAND_Y + 1.99 * 0.85 - 0.1)) < 0.05, arrow
        step(f"  OK arrow {arrow['entity']} spawned at ({arrow['x']:.2f}, {arrow['y']:.2f}, "
             f"{arrow['z']:.2f}) from the skeleton's eye")

        # Step out of its reach: the ring drops every mob two blocks from the
        # summoner, and from there the arrow reaches the player on its first tick
        # — spawned and already landed, with no flight to observe. Eleven blocks
        # is still inside the bow's fifteen, so the same skeleton keeps firing,
        # and the next arrow is the one the flight step watches.
        send_chat_command(bot, "tp 0.5 64.0 11.5")
        drain(bot, 0.4)

        # --- 3. it flies ------------------------------------------------------
        moves = []
        next_arrow = None
        deadline = time.time() + 8.0
        while time.time() < deadline and (next_arrow is None or len(moves) < 2):
            bot.tick(2)
            batch = drain(bot, 0.2)
            if next_arrow is None:
                next_arrow = next((s for s in (read_spawn(b) for b in all_of(batch, CB_SPAWN_ENTITY))
                                   if s["type"] == ET_ARROW), None)
            # A sync from the first, short-range arrow is not this step's: it was
            # loosed at a player who no longer stands there, and matching on the
            # id is what keeps the two arrows apart in a mixed batch.
            moves += [read_position_sync(b) for b in all_of(batch, CB_ENTITY_POSITION_SYNC)
                      if next_arrow is not None
                      and read_position_sync(b)["entity"] == next_arrow["entity"]]
        assert next_arrow is not None, "the skeleton stopped firing once the player stepped back"
        assert len(moves) >= 2, f"the arrow never moved: {moves}"
        # `AbstractArrow` spawns at the shooter's x/z and eye height - 0.1; the
        # skeleton's eye is `1.0 + 1.99 * 0.85` here.
        assert abs(next_arrow["x"] - skeleton["x"]) < 0.01 and \
            abs(next_arrow["z"] - skeleton["z"]) < 0.01, next_arrow
        assert abs(next_arrow["y"] - (STAND_Y + 1.99 * 0.85 - 0.1)) < 0.05, next_arrow
        step(f"  OK a second arrow flew through {len(moves)} position syncs")

        # --- 4. it hurts ------------------------------------------------------
        # One arrow, not the drop since the join: the bot took the point-blank
        # arrow in step 2 and has been regenerating since, so `20 - health`
        # would bill this arrow for every hit the run ever took. Waiting for the
        # next entry in HITS is the hit this step is about.
        mark = len(HITS)
        deadline = time.time() + 8.0
        while time.time() < deadline and len(HITS) == mark:
            bot.tick(2)
            drain(bot, 0.2)
        assert len(HITS) > mark, "the skeleton's arrow never landed (no set_health)"
        took = HITS[mark]
        assert 1.0 <= took <= 8.0, (
            f"a skeleton arrow deals its launch-speed-scaled 4ish, took {took}")
        step(f"  OK the arrow hit for {took:.1f} (health now {last_health:.1f}, "
             f"{len(HITS)} hits so far)")

        bot.close()
        print("skeleton arrow: all steps passed")
        return 0
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        shutil.rmtree(world, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
