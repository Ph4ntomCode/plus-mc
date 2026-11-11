#!/usr/bin/env python3
"""Integration test: a blast crater beside a pool fills with water.

fluid_resume_test.py proves the *dig* path of the neighbour fan-out; this
proves the *explosion* path — the same `notify_block_neighbours` call, made
from the blast's destroy loop (`ServerExplosion` destroys through
`Level.setBlock(..., 3)`, session.cpp). A blast that opens a cell next to a
pool owes that pool a scheduled tick, and the water must run into the hole.

The harness is tnt_wire_test.py's, imported rather than copied: the fuse, the
prime (`flint and steel` on a real `use_item_on`), the flat fixture world and
its grass floor are all that test's machinery, and the transcript/replay
claims live over there.

The fixture, at the flat world's y=-60 cell row:

  * a one-cell cup — walls west/north/south, a *dirt* target cell east — with
    the water source placed last, so nothing has scheduled it and the settle
    window below can prove the pool is at rest;
  * dirt for the target because the blast's rays die on resistance: stone at
    that range would shrug the ray off, dirt does not (vanilla values, the
    table in world/blast_resistance_table.h). Water's 100 resistance means the
    source cell itself is never marked by a ray, let alone destroyed — the
    pool survives its own blast, as it does in vanilla;
  * a TNT block one cell east of the target, primed with flint and steel.

What each observation can only be explained by:

  1. the settle window: nothing writes the target before the blast — the pool
     is at rest, so any later fill came from the blast;
  2. the prime: the TNT cell goes to air and an entity spawns with an 80-tick
     fuse (tnt_wire_test's own contract);
  3. after the fuse: the target broadcasts air (the blast's destroy loop),
     then a water state — ordered, air first — which is the fan-out having
     scheduled the source and the source's spread reaching the opened cell.

Standing one block from the blast in creative: no damage, as tnt_wire_test
asserts for every creative blast.

Run: explosion_fluid_test.py /path/to/plusmc
"""
import os, shutil, sys, tempfile, time
import wire_client
from tnt_wire_test import (
    Bot, Session, creative_item, drain, step, wait_listening,
    read_block_update, read_spawn, rd_varint,
    CB_BLOCK_UPDATE, CB_SPAWN_ENTITY,
    ON_Y, SLOT_TNT, ITEM_TNT, SLOT_FLINT, ITEM_FLINT_AND_STEEL,
    TICK_SCALE, FUSE_SECONDS,
    TYPE_PRIMED_TNT, HOST,
)

# The scene: source cup and its blast, one chunk-corner of its own (the other
# tnt_wire scenes sit at z=4 and z=18..20; this is z=10..13).
CRATER_SOURCE = (5, ON_Y, 12)
CRATER_WALLS = [(4, ON_Y, 12), (5, ON_Y, 11), (5, ON_Y, 13)]
CRATER_TARGET = (6, ON_Y, 12)      # the dirt cell the blast must open
CRATER_TNT = (7, ON_Y, 12)
# Far from every scene and out of any fluid's ~8-cell reach of the cup: the
# level ids are captured here before any water exists in the world.
CRATER_SCRATCH = (1, ON_Y, 1)
CRATER_STAND = (8.5, float(ON_Y), 12.0)


def setblock(session, cell, block, timeout=0.6):
    """`/setblock` through the session and return the state id it broadcast."""
    x, y, z = cell
    packets = session.command(f"setblock {x} {y} {z} {block}", timeout=timeout)
    ids = [state for at, state in (read_block_update(b) for p, b in packets
                                   if p == CB_BLOCK_UPDATE) if at == cell]
    assert ids, f"/setblock {cell} {block} broadcast nothing"
    return ids[0]


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    fixtures = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
    world = tempfile.mkdtemp(prefix="plusmc-explosion-fluid-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_TICK_SCALE"] = str(TICK_SCALE)
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    env["PLUSMC_CHUNKS_PER_TICK"] = "1000"

    proc = None
    bot = None
    try:
        if not wire_client.replaying():
            region_dir = os.path.join(world, "dimensions", "minecraft", "overworld", "region")
            os.makedirs(region_dir, exist_ok=True)
            shutil.copyfile(os.path.join(fixtures, "r.0.0.mca"),
                            os.path.join(region_dir, "r.0.0.mca"))
            step(f"world dir: {world} (flat fixture: chunk (0,0) is plains)")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "explosion fluid test",
             "--offline-mode", "--gamemode", "1", "--data", data_root, "--world", world],
            wait_listening, env=env)

        bot = Bot("CraterBot", port)
        join_packets = bot.join()
        bot.start_reader()
        join_packets += drain(bot, 1.0)
        session = Session(bot)
        attrs = [b for p, b in join_packets if p == 0x86]  # update_attributes
        assert attrs, "the join must push the attribute set (which carries the entity id)"
        bot.entity_id, _ = rd_varint(attrs[-1], 0)

        # --- 1. the pool: cup walls, target, and every water level's id -----

        creative_item(bot, SLOT_TNT, ITEM_TNT, 64)
        creative_item(bot, SLOT_FLINT, ITEM_FLINT_AND_STEEL, 1)
        session.move_to(CRATER_STAND)
        session.place([CRATER_TNT], SLOT_TNT)
        step(f"  OK the TNT block landed at {CRATER_TNT}")

        dirt_id = setblock(session, CRATER_TARGET, "minecraft:dirt")
        for cell in CRATER_WALLS:
            assert setblock(session, cell, "minecraft:dirt") == dirt_id, \
                "every wall is the same dirt state"
        # Every level a spread can deliver (the source's first spread writes
        # level=1; the post-blast hole dance can arrive as any level or as the
        # source state itself), captured before any water exists in the world.
        fluid_ids = set()
        for level in range(1, 9):
            fluid_ids.add(setblock(session, CRATER_SCRATCH,
                                   f"minecraft:water[level={level}]"))
        source_id = setblock(session, CRATER_SCRATCH, "minecraft:water")
        fluid_ids.add(source_id)
        assert dirt_id not in fluid_ids and 0 not in fluid_ids

        # The source goes in last: nothing scheduled before it can schedule
        # the cells around it in a way that reaches *it*, so the pool settles
        # at rest — the control for "the fill below came from the blast".
        setblock(session, CRATER_SOURCE, "minecraft:water")
        step("  cup built: source at (5,-60,12), dirt target at (6,-60,12), at rest")

        control_mark = len(session.seen)
        time.sleep(2.0)
        session.observe(drain(bot, 1.0))
        control = [state for at, state in
                   (read_block_update(b) for p, b in session.seen[control_mark:]
                    if p == CB_BLOCK_UPDATE)
                   if at in (CRATER_TARGET, CRATER_SOURCE)]
        assert not control, f"the pool is not at rest before the blast: {control}"
        step("  OK nothing writes the target or the source while the blast is pending")

        # --- 2. prime the TNT: the block goes to air, the fuse starts -------

        lit = session.light(CRATER_TNT[0], CRATER_TNT[1], CRATER_TNT[2])
        spawns = [s for s in (read_spawn(b) for p, b in lit if p == CB_SPAWN_ENTITY)
                  if s["type"] == TYPE_PRIMED_TNT]
        assert spawns, "flint and steel must prime the block"
        tnt_eid = spawns[0]["entity"]
        assert (CRATER_TNT, 0) in [read_block_update(b) for p, b in lit
                                   if p == CB_BLOCK_UPDATE], \
            "the primed block must be taken away"
        step(f"  OK entity {tnt_eid} primed, its block -> air")

        blast_mark = len(session.seen)
        session.watch(FUSE_SECONDS, lambda: session.despawned(tnt_eid),
                      slice_seconds=0.05)
        assert session.despawned(tnt_eid), "the fuse must run out and the blast resolve"
        step("  OK the fuse ran out; watching the crater cell")

        # --- 3. the crater: air from the blast, then water ------------------

        session.watch(10.0,
                      lambda: any(state in fluid_ids for state in
                                  [s for at, s in
                                   (read_block_update(b) for p, b in session.seen[blast_mark:]
                                    if p == CB_BLOCK_UPDATE)
                                   if at == CRATER_TARGET]),
                      slice_seconds=0.2)
        target_states = [state for at, state in
                         (read_block_update(b) for p, b in session.seen[blast_mark:]
                          if p == CB_BLOCK_UPDATE)
                         if at == CRATER_TARGET]
        assert target_states, "the blast never touched the target cell (no air broadcast)"
        assert 0 in target_states, (
            f"the blast must open the target cell first: {target_states}")
        filled = [s for s in target_states if s in fluid_ids]
        assert filled, (
            f"the water never flowed into the blast crater: states at "
            f"{CRATER_TARGET} = {target_states} (fluid ids {sorted(fluid_ids)})")
        assert target_states.index(0) < target_states.index(filled[0]), (
            f"fill must follow the blast's air, not precede it: {target_states}")
        assert not session.damage_events(bot.entity_id), \
            "a creative player cannot be hurt by a blast"
        step(f"  OK blast opened {CRATER_TARGET} (air) and the water filled it "
             f"(state {filled[0]})")

        bot.close()
    finally:
        if proc is not None:
            proc.terminate()
            proc.wait(timeout=5)
    print("\nPASS: explosion fluid test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
