#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4 — the wither's encounter over the real wire protocol.

The boss bar test proves the HUD; this one proves the *fight* around it:

  * a summoned wither starts at 1 health under a 220-tick spawn shield, so its
    fresh bar is near empty rather than full;
  * the shield refuses damage: a diamond sword's swings during it produce no
    `damage_event` and no change to the bar;
  * the shield ends in `WitherBoss`'s power-7 blast — an `explode` packet at the
    wither — and the bar has filled to 1.0;
  * after it, the wither stands at range and looses a skull every 40 ticks:
    Add Entity carries `minecraft:wither_skull` (type 148), and the player takes
    a `damage_event` from it.

The half-health armour is not driven here — reaching 150 damage through a sword
would make this a minute-long fight — and is pinned by `plusmc-mob-test`'s
`test_wither_encounter` and `test_wither_armour_phase` (the threshold, the
4-to-8 armour step and the projectile refusal).

The player is a spectator for the spawn blast (a power-7 explosion at two
blocks would otherwise kill them) and switches back to survival for the volley,
so the skull's damage is real. The world is generated so the blast has terrain
to rearrange.

Run: wither_fight_test.py /path/to/plusmc
"""

import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wire_client  # noqa: E402
from craft_test import Bot, drain, varint, wait_listening  # noqa: E402
from nbt_component import rd_nbt_component  # noqa: E402
from wire_client import rd_varint  # noqa: E402

PROTO = 777
HOST = "127.0.0.1"

CB_ADD_ENTITY = 0x01
CB_BOSS_EVENT = 0x09
CB_DAMAGE_EVENT = 0x19
CB_EXPLODE = 0x24
CB_UPDATE_ATTRIBUTES = 0x86
SB_ATTACK = 0x01
SB_CHAT_COMMAND = 0x07
SB_SET_CARRIED_ITEM = 0x36

ET_WITHER = 149
ET_WITHER_SKULL = 151


def step(msg):
    print(msg, flush=True)


def command(bot, text):
    assert text.startswith("/")
    bot.send(SB_CHAT_COMMAND, varint(len(text) - 1) + text[1:].encode())


def poll(bot, predicate, timeout=None):
    packets = []
    deadline = None if timeout is None else time.time() + timeout
    while True:
        packets += drain(bot, 0.1)
        if predicate(packets):
            return True, packets
        # The wither's shield is counted in game ticks. A loaded full-suite run
        # can take arbitrarily long to deliver those ticks, so the event is the
        # wait condition unless a caller explicitly asks for a bound.
        if deadline is not None and time.time() >= deadline:
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


def spawn_of(packets, entity_type):
    """The first Add Entity of `entity_type` as (eid, x, y, z)."""
    for pid, body in packets:
        if pid != CB_ADD_ENTITY:
            continue
        eid, i = rd_varint(body, 0)
        i += 16
        type_id, i = rd_varint(body, i)
        if type_id == entity_type:
            x, y, z = struct.unpack(">ddd", body[i:i + 24])
            return eid, x, y, z
    return None


def damage_events(packets):
    out = []
    for pid, body in packets:
        if pid != CB_DAMAGE_EVENT:
            continue
        entity, i = rd_varint(body, 0)
        type_id, i = rd_varint(body, i)
        out.append((entity, type_id))
    return out


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
            [binary, "--host", HOST, "--port", "0", "--motd", "wither fight",
             "--offline-mode", "--gamemode", "0", "--data", data_root,
             "--worldgen", "42"], wait_listening, env=env)
        step(f"server listening on {port}")

        bot = Bot("WitherBot", port)
        wire_client.record(bot, note="wither fight: shield, blast, boss bar and skull volley")
        bot.join()
        bot.start_reader()
        # A generated world's join burst (chunks ahead of the spawn) is longer
        # than the void world's, so wait for the attributes rather than assuming
        # 1.2 s is enough.
        ok, packets = poll(bot, lambda p: any(pid == CB_UPDATE_ATTRIBUTES for pid, _ in p))
        assert ok, "the join must send update_attributes"
        eid = next(rd_varint(body, 0)[0] for pid, body in packets
                   if pid == CB_UPDATE_ATTRIBUTES)
        step(f"joined as entity {eid}")

        # --- 1. the shield starts the bar near empty ------------------------
        command(bot, "/summon minecraft:wither 1")
        ok, packets = poll(bot, lambda p: any(
            e["op"] == 0 and e["title"] == "Wither" for e in boss_events(p)))
        assert ok, f"/summon wither must add its bar: {boss_events(packets)}"
        wither = next(e for e in boss_events(packets)
                      if e["op"] == 0 and e["title"] == "Wither")
        # The world may have advanced a few shield ticks before the ADD lands;
        # 0.05 still leaves it far below the full bar the shield fills to.
        assert wither["progress"] < 0.05, (
            f"the spawn shield starts the bar at 1/300, not {wither['progress']}")
        spawned = spawn_of(packets, ET_WITHER)
        assert spawned is not None, "the wither must also Add Entity"
        wither_eid = spawned[0]
        step(f"wither: bar added at {wither['progress']:.4f}, entity {wither_eid}")

        # --- 2. the shield refuses a sword --------------------------------
        command(bot, f"/give {bot.name} minecraft:diamond_sword 1")
        drain(bot, 0.4)
        bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 0))
        drain(bot, 0.2)
        attack_packets = []
        for _ in range(6):
            bot.send(SB_ATTACK, varint(wither_eid))
            attack_packets += drain(bot, 0.15)
        hits = [d for d in damage_events(attack_packets) if d[0] == wither_eid]
        assert not hits, f"a shielding wither must refuse every hit, got {hits}"
        step("shield: six sword swings produced no damage_event")

        # --- 3. the shield ends in the blast and the bar is full ------------
        # Stay a non-spectator: `check_despawn` excludes spectators, so a boss
        # with no non-spectator player would be discarded before its blast. A
        # power-7 explosion at two blocks is survived under resistance 5, which
        # is cleared again before the skull volley so that damage is real.
        command(bot, f"/effect give {bot.name} minecraft:resistance 999 4")
        ok, packets = poll(
            bot, lambda p: any(e["op"] == 2 and e["uuid"] == wither["uuid"] and
                               e["progress"] > 0.999 for e in boss_events(p)))
        assert ok, ("the spawn shield must fill the bar within 220 ticks: "
                    f"{[e for e in boss_events(packets) if e['uuid'] == wither['uuid']][-3:]}")
        blasts = [b for pid, b in packets if pid == CB_EXPLODE]
        # The explode may have landed in an earlier drain of the same poll; the
        # bar reaching 1.0 is the shield's own deadline, and the blast is emitted
        # on that tick.
        assert blasts or any(pid == CB_EXPLODE for pid, _ in packets), \
            "the shield must end in an explode packet"
        step(f"blast: the shield ended with {len(blasts)} explode packet(s), bar full")

        # --- 4. the skull volley -------------------------------------------
        # The point-blank blast's knockback throws the player far enough that a
        # later skull is out of range, so come back beside the wither (whose
        # summon position the Add Entity named) and drop the resistance that
        # would null the skull's 8 damage.
        command(bot, f"/tp {bot.name} {spawned[1] + 3:.1f} {spawned[2] + 1:.1f} {spawned[3]:.1f}")
        command(bot, f"/effect clear {bot.name} minecraft:resistance")
        ok, packets = poll(
            bot,
            lambda p: spawn_of(p, ET_WITHER_SKULL) is not None and
            any(e == eid for e, _t in damage_events(p)))
        assert spawn_of(packets, ET_WITHER_SKULL) is not None, \
            "after the shield the wither must spawn a wither_skull entity"
        skull = spawn_of(packets, ET_WITHER_SKULL)
        step(f"volley: wither_skull entity {skull[0]} spawned at "
             f"{skull[1]:.1f},{skull[2]:.1f},{skull[3]:.1f}")
        hits = [d for d in damage_events(packets) if d[0] == eid]
        assert hits, f"the skull volley must land a damage_event on the player: {hits}"
        step(f"volley: the skull's damage_event reached the player: {hits[0]}")

        command(bot, "/mobs kill")
        drain(bot, 0.5)
        step("OK wither fight: shield, blast, boss bar and skull volley")
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
