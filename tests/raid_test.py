#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4 — raids over the real wire protocol.

The engine has no village/POI model, so this walks the four parts §3.1's
*raid/boss basics* names at the scope that exists:

  * killing a pillager leaves `raid_omen` on its killer (every pillager is the
    captain; there is no ominous-banner model);
  * coming within 32 blocks of a villager consumes the omen and starts a raid;
  * three waves spawn around that villager — two pillagers, then pillagers and
    a vindicator, then a ravager and pillagers — and clearing each advances the
    raid's own `boss_event` bar;
  * clearing the last wave removes the bar and applies
    `hero_of_the_village`.

The survival clock is packet-driven (`PLUSMC_SURVIVAL_CLOCK=packet`): each
position packet is one tick, which makes the 40-tick wave check deterministic.

Run: raid_test.py /path/to/plusmc
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

HOST = "127.0.0.1"

CB_ADD_ENTITY = 0x01
CB_BOSS_EVENT = 0x09
CB_UPDATE_MOB_EFFECT = 0x87
CB_UPDATE_ATTRIBUTES = 0x86
SB_CHAT_COMMAND = 0x07
SB_PLAYER_POSITION = 0x1E

ET_PILLAGER = 104
ET_RAVAGER = 112
ET_VINDICATOR = 144
ET_VILLAGER = 143
COLOR_RED = 2

SPAWN = (-12.0, 64.0, -16.0)


def step(msg):
    print(msg, flush=True)


def command(bot, text):
    assert text.startswith("/")
    bot.send(SB_CHAT_COMMAND, varint(len(text) - 1) + text[1:].encode())


def pump(bot):
    """One packet-driven survival tick at the spawn point."""
    bot.send(SB_PLAYER_POSITION, struct.pack(">ddd", *SPAWN) + b"\x01")


def poll(bot, predicate, timeout=20.0, seen=None):
    """Pump ticks until `predicate` matches the packets accumulated so far.

    `seen` carries a previous stage's drain forward: the villager's Add Entity,
    the raid bar and wave one can all arrive in one batch, and a fresh list
    would miss the bar that the villager poll already consumed.
    """
    packets = seen if seen is not None else []
    if predicate(packets):
        return True, packets
    deadline = time.time() + timeout
    while True:
        pump(bot)
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


def effect_ids(packets):
    out = []
    for pid, body in packets:
        if pid != CB_UPDATE_MOB_EFFECT:
            continue
        _eid, i = rd_varint(body, 0)
        effect, i = rd_varint(body, i)
        out.append(effect)
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
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    bot = None
    try:
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "raid test",
             "--offline-mode", "--gamemode", "0", "--data", data_root,
             "--worldgen", "42"], wait_listening, env=env)
        step(f"server listening on {port}")

        bot = Bot("RaidBot", port)
        wire_client.record(bot, note="raid: omen, village trigger, waves, hero")
        bot.join()
        bot.start_reader()
        # The generated world streams its spawn chunks before the attribute
        # packet; on a busy host that can outlast fifteen seconds.
        ok, packets = poll(bot, lambda p: any(pid == CB_UPDATE_ATTRIBUTES for pid, _ in p),
                           timeout=45.0)
        assert ok, "the join must send update_attributes"
        step("joined")

        # --- 1. a pillager's death leaves the omen --------------------------
        command(bot, "/summon minecraft:pillager 1")
        ok, packets = poll(bot, lambda p: spawns_of(p, ET_PILLAGER))
        assert ok, "the pillager must Add Entity"
        pillager = spawns_of(packets, ET_PILLAGER)[0][0]
        command(bot, f"/mobs hurt {pillager} 100")
        ok, packets = poll(bot, lambda p: effect_ids(p), timeout=15.0)
        assert ok, "killing a pillager must apply raid_omen to the killer"
        omen_id = effect_ids(packets)[0]
        step(f"omen: effect {omen_id} applied by the pillager's death")

        # --- 2. a villager consumes the omen and starts the raid -----------
        # The bar, the villager and the first wave can land in one drain; keep
        # the whole batch rather than waiting for a second bar that never comes.
        command(bot, "/summon minecraft:villager 1")
        ok, packets = poll(
            bot,
            lambda p: spawns_of(p, ET_VILLAGER)
            and any(e["op"] == 0 and e["title"] == "Raid" and e["color"] == COLOR_RED
                    for e in boss_events(p))
            and len(spawns_of(p, ET_PILLAGER)) >= 2,
            timeout=20.0)
        assert ok, (f"a villager and the omen must add the raid bar and its first wave: "
                    f"{boss_events(packets)}, pillagers={spawns_of(packets, ET_PILLAGER)}")
        raid = next(e for e in boss_events(packets)
                    if e["op"] == 0 and e["title"] == "Raid")
        step(f"raid: bar {raid['uuid'].hex()} added, RED; wave 1: two pillagers")

        # --- 4. wave two: pillagers and a vindicator -----------------------
        # A fresh list per wave: the previous wave's progress packet is not
        # this wave's progress.
        seen = []
        command(bot, "/mobs kill")
        ok, seen = poll(bot, lambda p: any(
            e["op"] == 2 and e["uuid"] == raid["uuid"] and e["progress"] > 0.3
            for e in boss_events(p)) and spawns_of(p, ET_VINDICATOR), timeout=20.0, seen=seen)
        assert ok, "clearing wave one must advance the bar and spawn a vindicator"
        step("wave 2: the bar advanced and a vindicator Add Entity")

        # --- 5. wave three: a ravager --------------------------------------
        seen = []
        command(bot, "/mobs kill")
        ok, seen = poll(bot, lambda p: any(
            e["op"] == 2 and e["uuid"] == raid["uuid"] and e["progress"] > 0.6
            for e in boss_events(p)) and spawns_of(p, ET_RAVAGER), timeout=20.0, seen=seen)
        assert ok, "clearing wave two must advance the bar and spawn a ravager"
        step("wave 3: the bar advanced and a ravager Add Entity")

        # --- 6. clearing the last wave ends the raid and grants the hero ----
        seen = []
        command(bot, "/mobs kill")
        ok, seen = poll(bot, lambda p: any(
            e["op"] == 1 and e["uuid"] == raid["uuid"] for e in boss_events(p))
            and any(effect != omen_id for effect in effect_ids(p)), timeout=20.0, seen=seen)
        assert ok, "clearing the last wave must remove the bar and apply a new effect"
        heroes = [effect for effect in effect_ids(seen) if effect != omen_id]
        step(f"hero: bar removed, effect {heroes[0]} applied (not the omen {omen_id})")

        command(bot, "/mobs kill")
        drain(bot, 0.5)
        step("OK raid: omen, village trigger, three waves and hero of the village")
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
