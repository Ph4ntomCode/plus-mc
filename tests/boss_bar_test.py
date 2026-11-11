#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4 — the boss bars over the real wire protocol.

Tier B's list names *raid/boss basics*. This test is the HUD's wire proof; the
wither, dragon and raid encounters themselves are `wither-fight`,
`dragon-fight` and `raid`.

  * a boss the world has never seen adds a UUID-keyed ``boss_event`` (0x09):
    operation 0, the title as a network-NBT component, the type's vanilla
    colour and the PROGRESS overlay (the wither starts at 1/300 health under
    its spawn shield; the dragon starts full);
  * the bar follows the *entity's* health, not a damage path: `/mobs hurt`
    lowers an ender dragon's health and operation 2 carries the new progress.
    Nothing on the hurt path knows the bar exists;
  * a client that joins while a bar exists is sent it in its join burst
    (``send_live_boss_bars``), so the HUD is not a function of who was online
    when the boss spawned;
  * ``/mobs kill`` discards the entity and operation 1 removes the bar.

The ender dragon is vanilla's other ``ServerBossEvent`` user, and it is checked
for its own title and PINK — those are per-type tables, not constants.

The world is generated (``--worldgen 42``) so the dragon has room to fly, and
the player is a spectator so the boss's hostility cannot move the stance the
summon ring is measured from.

Run: boss_bar_test.py /path/to/plusmc
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

# boss_event (ClientboundBossEventPacket) in the play state.
CB_BOSS_EVENT = 0x09
# Serverbound chat command: [string command] with no signature fields.
SB_CHAT_COMMAND = 0x07

# BossBarColor (clientbound enum order): PINK, BLUE, RED, GREEN, YELLOW,
# PURPLE, WHITE. Vanilla's WitherBoss builds PURPLE, EnderDragon PINK.
COLOR_PINK = 0
COLOR_PURPLE = 5
OVERLAY_PROGRESS = 0

# ClientboundBossEventPacket's own flag constants: the ADD's byte.
FLAG_DARKEN = 0x01
FLAG_MUSIC = 0x02
FLAG_FOG = 0x04

# minecraft:entity_type protocol ids (src/world/mob_table.h).
ET_ENDER_DRAGON = 44
ET_WITHER = 149


def step(msg):
    print(msg, flush=True)


def command(bot, text):
    assert text.startswith("/")
    bot.send(SB_CHAT_COMMAND, varint(len(text) - 1) + text[1:].encode())


def poll(bot, predicate, timeout=None):
    """Drain until `predicate(packets)` is truthy, or an explicit deadline passes.

    Positive events are the wait condition: a loaded full suite can delay the
    server tick that emits them, so CTest's overall timeout is the hang guard.
    """
    packets = []
    deadline = None if timeout is None else time.time() + timeout
    while True:
        packets += drain(bot, 0.1)
        if predicate(packets):
            return True, packets
        if deadline is not None and time.time() >= deadline:
            return False, packets


def parse_boss(body):
    """Decode one boss_event. The UUID comes first, then the operation, and the
    payload is exactly the fields that operation names."""
    assert len(body) >= 17, f"boss_event is too short for a uuid: {len(body)}"
    uuid = body[0:16]
    op, i = rd_varint(body, 16)
    out = {"uuid": uuid, "op": op}
    if op in (0, 3):  # ADD / UPDATE_NAME
        out["title"], i = rd_nbt_component(body, i)
    if op in (0, 2):  # ADD / UPDATE_PROGRESS
        out["progress"] = struct.unpack(">f", body[i:i + 4])[0]
        i += 4
    if op in (0, 4):  # ADD / UPDATE_STYLE
        out["color"], i = rd_varint(body, i)
        out["overlay"], i = rd_varint(body, i)
    if op in (0, 5):  # ADD / UPDATE_PROPERTIES
        out["flags"] = body[i]
        i += 1
    assert i == len(body), f"boss_event op {op}: {len(body) - i} trailing byte(s)"
    return out


def boss_events(packets):
    return [parse_boss(body) for pid, body in packets if pid == CB_BOSS_EVENT]


def entity_ids(packets, entity_type):
    """Every Add Entity of `entity_type`, in wire order."""
    out = []
    for pid, body in packets:
        if pid != 0x01:  # add_entity
            continue
        eid, i = rd_varint(body, 0)
        i += 16
        got, i = rd_varint(body, i)
        if got == entity_type:
            out.append(eid)
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
    # Generated terrain, because the fall-damage step needs a floor.

    proc = None
    bots = []
    try:
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "boss bar test",
             "--offline-mode", "--gamemode", "3", "--data", data_root,
             "--worldgen", "42"], wait_listening, env=env)
        step(f"server listening on {port}")

        bot = Bot("BossBot", port)
        bots.append(bot)
        wire_client.record(bot, note="boss bar: add, replay, progress and removal")
        bot.join()
        bot.start_reader()
        packets = drain(bot, 1.2)
        assert not boss_events(packets), (
            f"an empty world must not add a boss bar: {boss_events(packets)}")
        step("join: no boss bar on an empty world")

        # --- 1. a summoned wither adds its bar -------------------------------
        command(bot, "/summon minecraft:wither 1")
        ok, packets = poll(bot, lambda p: any(
            e["op"] == 0 and e["title"] == "Wither" for e in boss_events(p)))
        assert ok, f"/summon wither must add its boss bar; packets: {boss_events(packets)}"
        adds = [e for e in boss_events(packets) if e["op"] == 0]
        wither = next(e for e in adds if e["title"] == "Wither")
        # The spawn shield starts the bar at 1/300 and fills it over 220 ticks,
        # so a fresh wither is *not* full (see wither_fight_test.py). A server
        # that caught up a few ticks before the bar's first update can be a few
        # percent in; it is still nowhere near the full bar a dragon starts at.
        assert wither["progress"] < 0.05, (
            f"a fresh wither's bar starts near empty, not full: {wither}")
        assert wither["color"] == COLOR_PURPLE, f"the wither bar is PURPLE: {wither}"
        assert wither["overlay"] == OVERLAY_PROGRESS, f"the wither bar is PROGRESS: {wither}"
        # `WitherBoss` builds its event with `setDarkenScreen(true)`, so the ADD's
        # byte is DARKEN and nothing else; a bar that sent 0 would draw the same
        # title over a world that never dims.
        assert wither.get("flags", 0) == FLAG_DARKEN, (
            f"the wither bar darkens the screen: {wither}")
        step(f"wither: bar {wither['uuid'].hex()} added near empty, PURPLE, DARKEN")

        # --- 2. a late joiner is replayed the bar ----------------------------
        watcher = Bot("BarWatcher", port)
        bots.append(watcher)
        watcher.join()
        watcher.start_reader()
        replay = drain(watcher, 1.5)
        assert any(e["op"] == 0 and e["uuid"] == wither["uuid"] for e in boss_events(replay)), (
            f"a joining client must be sent the live bar: {boss_events(replay)}")
        step("join replay: the second client was sent the existing bar")

        # --- 3. the dragon is the second boss type, with its own title/colour -
        command(bot, "/summon minecraft:ender_dragon 1")
        ok, packets = poll(bot, lambda p: any(
            e["op"] == 0 and e["title"] == "Ender Dragon" for e in boss_events(p)))
        assert ok, f"/summon ender_dragon must add its boss bar: {boss_events(packets)}"
        dragon = next(e for e in boss_events(packets)
                      if e["op"] == 0 and e["title"] == "Ender Dragon")
        assert dragon["color"] == COLOR_PINK, f"the dragon bar is PINK: {dragon}"
        assert dragon["progress"] == 1.0, f"a fresh dragon's bar must be full: {dragon}"
        # `EnderDragonFight` sets the music and the fog and never the darkening,
        # so the dragon's byte is exactly MUSIC | FOG — a different bar from the
        # wither's, which is the point of a per-type table.
        assert dragon.get("flags", 0) == FLAG_MUSIC | FLAG_FOG, (
            f"the dragon bar plays music and draws fog, and does not darken: {dragon}")
        step(f"dragon: bar {dragon['uuid'].hex()} added, title Ender Dragon, PINK, MUSIC|FOG")

        # --- 4. the bar follows the entity's death --------------------------
        command(bot, "/mobs kill")
        ok, packets = poll(bot, lambda p: {e["uuid"] for e in boss_events(p) if e["op"] == 1}
                           >= {wither["uuid"], dragon["uuid"]})
        assert ok, (f"/mobs kill must remove both bars; removes: "
                    f"{[e for e in boss_events(packets) if e['op'] == 1]}")
        step("removal: /mobs kill sent operation 1 for both bars")

        # --- 5. a health change moves the bar's progress --------------------
        # The dragon now flies (dragon-fight), so a thirty-block summon no
        # longer becomes a fall. `/mobs hurt` moves the same health field every
        # damage path writes; the bar's diff is what turns that into operation 2.
        command(bot, "/summon minecraft:ender_dragon 1")
        ok, packets = poll(
            bot,
            lambda p: any(e["op"] == 0 and e["title"] == "Ender Dragon"
                          for e in boss_events(p)) and entity_ids(p, ET_ENDER_DRAGON))
        assert ok, f"the hurt dragon must add its bar: {boss_events(packets)}"
        hurt = next(e for e in boss_events(packets)
                    if e["op"] == 0 and e["title"] == "Ender Dragon")
        dragon_eid = entity_ids(packets, ET_ENDER_DRAGON)[0]
        command(bot, f"/mobs hurt {dragon_eid} 20")
        ok, packets = poll(bot, lambda p: any(
            e["op"] == 2 and e["uuid"] == hurt["uuid"] and e["progress"] < 0.99
            for e in boss_events(p)))
        assert ok, (f"/mobs hurt must move the bar below full: "
                    f"{[e for e in boss_events(packets) if e['uuid'] == hurt['uuid']]}")
        progress = next(e["progress"] for e in boss_events(packets)
                        if e["op"] == 2 and e["uuid"] == hurt["uuid"])
        assert 0.5 < progress < 0.99, f"20 damage is not a near-empty bar: {progress}"
        step(f"progress: /mobs hurt moved the bar to {progress:.4f} of full")

        command(bot, "/mobs kill")
        drain(bot, 0.5)
        step("OK boss bars: add, replay, progress and removal all on the wire")
        return 0
    finally:
        for bot in bots:
            bot.close()
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except Exception:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
