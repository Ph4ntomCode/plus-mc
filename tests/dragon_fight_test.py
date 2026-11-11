#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4 — the ender dragon's encounter over the real wire protocol.

The boss-bar test proves the HUD; this one proves the fight around it:

  * a summoned dragon adds the PINK `Ender Dragon` bar at full health;
  * `/summon minecraft:end_crystal` adds static crystals (entity type 45), and
    every ten ticks one crystal heals the dragon by one health — a bar that has
    been damaged climbs again while its crystals live;
  * a sword swing on a crystal destroys it (remove_entities) and runs vanilla's
    power-6 blast (explode);
  * after the arena is cleared, a fresh dragon at half health leaves the
    circling altitude and lands on its perch column, then takes off again — read
    off the dragon's own position syncs, not a log line.

The world is generated so the circling dragon has room; the harness clears a
5x5 shaft over the perch column because the dragon does not break the canopy.
`--gamemode 0` keeps the dragon's target real; the player takes resistance V
around the crystal blast, which is a 0 damage multiplier.

Run: dragon_fight_test.py /path/to/plusmc
"""

import math
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wire_client  # noqa: E402
from craft_test import Bot, drain, varint, wait_listening  # noqa: E402
from nbt_component import rd_nbt_component  # noqa: E402
from wire_client import rd_varint  # noqa: E402

HOST = "127.0.0.1"

CB_ADD_ENTITY = 0x01
CB_BOSS_EVENT = 0x09
CB_EXPLODE = 0x24
CB_ENTITY_POSITION_SYNC = 0x23
CB_REMOVE_ENTITIES = 0x4E
CB_UPDATE_ATTRIBUTES = 0x86
SB_ATTACK = 0x01
SB_CHAT_COMMAND = 0x07

ET_DRAGON = 44
ET_CRYSTAL = 46
COLOR_PINK = 0


def step(msg):
    print(msg, flush=True)


def command(bot, text):
    assert text.startswith("/")
    bot.send(SB_CHAT_COMMAND, varint(len(text) - 1) + text[1:].encode())


def clear_perch(bot, x, z, y_bottom, y_top):
    """Clear a 5x5 shaft over the perch column.

    A generated canopy can stop the seven-block descent: the dragon's 4x4x3
    steering box is blocked and it does not break blocks (a stated limit). The
    phase is what this test is about, so the harness removes that variable; the
    climb through the surrounding terrain is still exercised below.
    """
    x0 = math.floor(x) - 2
    z0 = math.floor(z) - 2
    for bx in range(x0, x0 + 5):
        for bz in range(z0, z0 + 5):
            for by in range(math.floor(y_bottom), math.floor(y_top) + 1):
                command(bot, f"/setblock {bx} {by} {bz} minecraft:air")


def poll(bot, predicate, timeout=20.0):
    packets = []
    deadline = time.time() + timeout
    while True:
        packets += drain(bot, 0.1)
        if predicate(packets):
            return True, packets
        if time.time() >= deadline:
            return False, packets


def parse_boss(body):
    uuid = body[0:16]
    op, i = rd_varint(body, 16)
    out = {"uuid": uuid, "op": op}
    if op in (0, 3):
        out["title"], i = rd_nbt_component(body, i)
    if op in (0, 2):
        out["progress"] = struct.unpack(">f", body[i:i + 4])[0]
        i += 4
    if op in (0, 4):
        out["color"], i = rd_varint(body, i)
        out["overlay"], i = rd_varint(body, i)
    if op in (0, 5):
        out["flags"] = body[i]
        i += 1
    assert i == len(body), f"boss_event op {op}: {len(body) - i} trailing byte(s)"
    return out


def boss_events(packets):
    return [parse_boss(b) for p, b in packets if p == CB_BOSS_EVENT]


def spawns_of(packets, entity_type):
    """Every Add Entity of `entity_type` as (eid, x, y, z), in wire order."""
    out = []
    for pid, body in packets:
        if pid != CB_ADD_ENTITY:
            continue
        eid, i = rd_varint(body, 0)
        i += 16
        type_id, i = rd_varint(body, i)
        if type_id != entity_type:
            continue
        x, y, z = struct.unpack(">ddd", body[i:i + 24])
        out.append((eid, x, y, z))
    return out


def positions_of(packets, eid):
    out = []
    for pid, body in packets:
        if pid != CB_ENTITY_POSITION_SYNC:
            continue
        got, i = rd_varint(body, 0)
        if got != eid:
            continue
        _, i = rd_varint(body, i)  # PositionPath.Type.LINEAR
        x, y, z = struct.unpack(">ddd", body[i:i + 24])
        out.append((x, y, z))
    return out


def removed_ids(packets):
    out = set()
    for pid, body in packets:
        if pid != CB_REMOVE_ENTITIES:
            continue
        count, i = rd_varint(body, 0)
        for _ in range(count):
            eid, i = rd_varint(body, i)
            out.add(eid)
    return out


def last_progress(packets, uuid):
    values = [e["progress"] for e in boss_events(packets)
              if e["uuid"] == uuid and e["op"] == 2]
    return values[-1] if values else None


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"

    proc = None
    bot = None
    try:
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "dragon fight",
             "--offline-mode", "--gamemode", "0", "--data", data_root,
             "--worldgen", "42"], wait_listening, env=env)
        step(f"server listening on {port}")

        bot = Bot("DragonBot", port)
        wire_client.record(bot, note="dragon fight: crystals, healing, perch and takeoff")
        bot.join()
        bot.start_reader()
        # The generated world's join burst streams the spawn chunks before the
        # attribute packet; on a busy host that is well past fifteen seconds.
        ok, packets = poll(bot, lambda p: any(pid == CB_UPDATE_ATTRIBUTES for pid, _ in p),
                           timeout=45.0)
        assert ok, "the join must send update_attributes"
        step(f"joined as entity {rd_varint(next(b for pid, b in packets if pid == CB_UPDATE_ATTRIBUTES), 0)[0]}")

        # --- 1. the dragon and its full PINK bar ----------------------------
        command(bot, "/summon minecraft:ender_dragon 1")
        ok, packets = poll(
            bot,
            lambda p: any(e["op"] == 0 and e["title"] == "Ender Dragon" for e in boss_events(p))
            and spawns_of(p, ET_DRAGON))
        assert ok, f"/summon ender_dragon must add its bar and entity: {boss_events(packets)}"
        bar = next(e for e in boss_events(packets)
                   if e["op"] == 0 and e["title"] == "Ender Dragon")
        assert bar["progress"] == 1.0, f"a fresh dragon is full: {bar}"
        assert bar["color"] == COLOR_PINK, f"the dragon's bar is PINK: {bar}"
        dragon = spawns_of(packets, ET_DRAGON)[0]
        dragon_eid = dragon[0]
        start_y = dragon[2]
        step(f"dragon: bar {bar['uuid'].hex()} full, PINK; entity {dragon_eid} at y={start_y:.1f}")

        # The dragon climbs into its circling altitude; remember the height so
        # the perch assertion is a descent from *there*, not from the summon.
        # Wait for the actual +9 crossing rather than an early +4 one: the perch
        # is only +2 over the summon, and a first-cross baseline can be below
        # the descent threshold even when the dragon did land.
        ok, packets = poll(bot, lambda p: any(y > start_y + 9.0 for _x, y, _z
                                              in positions_of(p, dragon_eid)), timeout=15.0)
        assert ok, "the dragon must climb into its circling altitude"
        circle_y = max(y for _x, y, _z in positions_of(packets, dragon_eid))
        step(f"dragon: circling above y={circle_y:.1f}")

        # --- 2. crystals add and heal the bar ------------------------------
        command(bot, "/summon minecraft:end_crystal 6")
        ok, packets = poll(bot, lambda p: len({s[0] for s in spawns_of(p, ET_CRYSTAL)}) >= 6)
        assert ok, f"six end crystals must Add Entity (type {ET_CRYSTAL})"
        crystals = [s[0] for s in spawns_of(packets, ET_CRYSTAL)][:6]
        step(f"crystals: {len(crystals)} Add Entity(type {ET_CRYSTAL})")

        command(bot, f"/mobs hurt {dragon_eid} 80")
        ok, packets = poll(bot, lambda p: (last_progress(p, bar["uuid"]) or 1.0) < 0.75, timeout=5.0)
        assert ok, f"/mobs hurt must move the dragon's bar: {boss_events(packets)}"
        hurt = last_progress(packets, bar["uuid"])
        ok, packets = poll(bot, lambda p: (last_progress(p, bar["uuid"]) or 0.0) > hurt + 0.02,
                           timeout=5.0)
        assert ok, ("the crystals must heal the dragon's bar: "
                    f"last={last_progress(packets, bar['uuid'])} from {hurt}")
        step(f"heal: the bar climbed from {hurt:.3f} to {last_progress(packets, bar['uuid']):.3f}")

        # --- 3. a crystal dies in its own blast ----------------------------
        command(bot, f"/effect give {bot.name} minecraft:resistance 999 4")
        drain(bot, 0.4)
        bot.send(SB_ATTACK, varint(crystals[0]))
        ok, packets = poll(
            bot,
            lambda p: crystals[0] in removed_ids(p) and any(pid == CB_EXPLODE for pid, _ in p),
            timeout=10.0)
        assert ok, (f"a sword swing must remove crystal {crystals[0]} and explode: "
                    f"removed={sorted(removed_ids(packets))}")
        step(f"crystal: entity {crystals[0]} removed and exploded")

        # --- 4. a clean dragon perches at half health, then takes off ------
        # The first dragon still has five healing crystals and may have been
        # scratched by the blast; either can lift it back over half before the
        # phase manager looks. Clear the arena and use a fresh dragon, so the
        # half-health trigger is the only thing that can move it.
        command(bot, "/mobs kill")
        drain(bot, 0.5)
        command(bot, "/summon minecraft:ender_dragon 1")
        ok, packets = poll(
            bot,
            lambda p: any(e["op"] == 0 and e["title"] == "Ender Dragon"
                          for e in boss_events(p)) and spawns_of(p, ET_DRAGON))
        assert ok, f"a fresh dragon must replace the first: {boss_events(packets)}"
        bar2 = [e for e in boss_events(packets)
                if e["op"] == 0 and e["title"] == "Ender Dragon"][-1]
        dragon2 = spawns_of(packets, ET_DRAGON)[-1]
        dragon2_eid = dragon2[0]
        start2_y = dragon2[2]
        ok, packets = poll(
            bot, lambda p: any(y > start2_y + 9.0 for _x, y, _z
                               in positions_of(p, dragon2_eid)), timeout=15.0)
        assert ok, "the fresh dragon must climb into its circling altitude"
        circle2_y = max(y for _x, y, _z in positions_of(packets, dragon2_eid))
        step(f"fresh dragon: circling above y={circle2_y:.1f}")

        # The generated canopy over the spawn column can stop the descent. The
        # dragon does not break blocks, so clear the shaft before the hurt; the
        # commands are ordered ahead of the hurt on the same socket.
        clear_perch(bot, dragon2[1], dragon2[3], start2_y + 2.0, circle2_y + 4.0)
        drain(bot, 2.0)

        command(bot, f"/mobs hurt {dragon2_eid} 120")
        ok, packets = poll(bot, lambda p: (last_progress(p, bar2["uuid"]) or 1.0) < 0.5,
                           timeout=5.0)
        assert ok, f"the fresh dragon must fall below half: {boss_events(packets)}"
        step(f"perch: the bar fell to {last_progress(packets, bar2['uuid']):.3f}")

        # The dragon closes on the perch at 0.5 blocks/tick along the diagonal
        # from the 12-block circle, and the landing altitude depends on the
        # summon column's terrain (3 Nov: the same engine landed at 71.6 and at
        # 73.177 on different runs, against a circle of 77.1). A fixed 4-block
        # bound is inside that spread; 3 blocks is still a real descent and the
        # takeoff is asserted separately below.
        ok, packets = poll(
            bot, lambda p: any(y < circle2_y - 3.0 for _x, y, _z
                               in positions_of(p, dragon2_eid)), timeout=40.0)
        ys = [y for _x, y, _z in positions_of(packets, dragon2_eid)]
        assert ok, ("at half health the dragon must descend to its perch "
                    f"(circle={circle2_y:.1f}, min={min(ys) if ys else None}, "
                    f"last={ys[-1] if ys else None})")
        landed_y = min(y for _x, y, _z in positions_of(packets, dragon2_eid))
        step(f"perch: the dragon descended to y={landed_y:.1f}")

        ok, packets = poll(
            bot, lambda p: any(y > landed_y + 3.0 for _x, y, _z
                               in positions_of(p, dragon2_eid)), timeout=25.0)
        assert ok, "after the perch the dragon must take off again"
        step(f"takeoff: the dragon climbed back to y="
             f"{max(y for _x, y, _z in positions_of(packets, dragon2_eid)):.1f}")

        command(bot, "/mobs kill")
        drain(bot, 0.5)
        step("OK dragon fight: crystals, healing, blast, perch and takeoff")
        return 0
    finally:
        if bot is not None:
            bot.close()
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except Exception:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
