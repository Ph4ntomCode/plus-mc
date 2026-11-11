#!/usr/bin/env python3
"""Integration test: the live redstone wire graph attenuates like vanilla.

The bug this pins: the session's post-write redstone pass asked each of a wire's
six neighbours for its *source* signal and counted another wire's own POWER
among them, so a wire fed its power straight back to its neighbour. Every cell
of a run therefore read 15 and a lamp at any distance lit at full strength.
Vanilla keeps the two apart — `DefaultRedstoneWireEvaluator.calculateTargetStrength`
is `max(blockSignal, incomingWireSignal)` where `getBlockSignal` asks
`Level.getBestNeighborSignal` with `RedStoneWireBlock.shouldSignal` off (so a
wire contributes nothing) and `getIncomingWireSignal` is `max(neighbour wire
POWER) - 1`.

The scene is one run in chunk (0, 0), the all-air stage the fluid tests use: a
redstone block, then a wire at every step of an L, then two lamps — one beside
the first wire (lit) and one past the fifteenth step (dark). What each
observation can only be explained by:

  1. the control: with every wire placed and no source, the whole run sits at
     power 0 — a wire is not its own source, so nothing powers itself;
  2. the source: `/setblock` of the redstone block settles the graph through the
     command's own neighbour pass (`SetBlockCommand.setBlock`'s
     `updateNeighboursOnBlockSet`), so the run is read from the source's
     post-write fan-out and not from a later tick;
  3. the exact ladder: wire i counts 15, 14, 13 ... down the run, one per step,
     which only the decrement of the wire-to-wire term can produce;
  4. the cut-off: the sixteenth step is 0, and the lamp beside it stays dark —
     the assertion that separates attenuation from "everything is 15";
  5. the consumer: the lamp next to the first wire is *lit*, so a wire hands its
     own power to a consumer undiminished (`ownSignal`) while dimming it for the
     next wire.

Every power level's state id is captured from the server itself, by
`/setblock`ing that exact state on a scratch cell first, so the ladder is
asserted on ids, not on names.

Run: redstone_wire_test.py /path/to/plusmc
"""
import os, shutil, subprocess, sys, tempfile
import wire_client
from fluid_resume_test import (
    Bot, block_updates, drain, observe_until, setblock, step, wait_listening,
    HOST, TICK_SCALE,
)

Y = 100
# The run: a redstone block at the origin, thirteen wires east along z = 0, then
# the turn north up x = 13. Wire step i (1-based) sits at a distance of i from
# the source, so its vanilla power is 16 - i: 15, 14, ... 3 along the row, then
# 2, 1, 0 up the turn.
SOURCE = (0, Y, 0)
ROW = [(i, Y, 0) for i in range(1, 14)]
TURN = [(13, Y, 1), (13, Y, 2), (13, Y, 3)]
NEAR_LAMP = (1, Y, 1)          # beside the first wire: must light
FAR_LAMP = (13, Y, 4)          # past the fifteenth step: must stay dark

# Scratch cells for the state-id table, isolated from each other and from the
# run (a wire beside a wire would settle to a common value).
SCRATCH_WIRES = [((2 * i) % 16, Y, 8 if i < 8 else 10) for i in range(16)]
SCRATCH_LIT_LAMP = (2, Y, 12)
SCRATCH_DARK_LAMP = (4, Y, 12)

# In the chunk the run lives in, standing clear of every cell it writes.
BOT_POSITION = (7.5, float(Y) + 4.0, 7.5)


def wire_id(bot, cell, power):
    """The server's own id for `redstone_wire[power=<power>]`.

    `/setblock` broadcasts the state it was handed before the post-write pass
    re-derives it, so the first update at the scratch cell is the explicit
    state's id — whatever the pass writes after it.
    """
    return setblock(bot, cell, f"minecraft:redstone_wire[power={power}]")[0]


def last_states(bot, mark):
    """The most recent state each cell was written to, since `mark`."""
    seen = {}
    for at, state in block_updates(bot.seen[mark:]):
        seen[at] = state
    return seen


def settled_to(bot, mark, expected):
    """A wait predicate: every named cell has reached its expected state."""
    def predicate(_updates):
        latest = last_states(bot, mark)
        return all(latest.get(cell) == state for cell, state in expected.items())
    return predicate


def main():
    if wire_client.replaying():
        binary = ""
    elif len(sys.argv) == 2:
        binary = os.path.realpath(sys.argv[1])
    else:
        print(f"usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-redstone-wire-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_TICK_SCALE"] = str(TICK_SCALE)
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    bot = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "redstone wire test",
             "--offline-mode", "--gamemode", "1", "--data", data_root, "--world", world],
            wait_listening, env=env)

        bot = Bot("WireBot", port)
        wire_client.record(bot, note="redstone-wire: a live run attenuates 15, 14, "
                                     "13 ... and stops at the fifteenth step")
        bot.join()
        bot.move_to(*BOT_POSITION)
        drain(bot, 1.0)
        step("  OK joined (creative, standing above the run)")

        # --- 1. the id table, from the server's own broadcasts ---------------

        table = [wire_id(bot, cell, power)
                 for power, cell in enumerate(SCRATCH_WIRES)]
        assert len(set(table)) == 16, f"power levels share state ids: {table}"
        lit_lamp_id = setblock(bot, SCRATCH_LIT_LAMP, "minecraft:redstone_lamp[lit=true]")[0]
        dark_lamp_id = setblock(bot, SCRATCH_DARK_LAMP, "minecraft:redstone_lamp[lit=false]")[0]
        assert lit_lamp_id != dark_lamp_id, "lit and dark lamps share a state id"
        step(f"  OK id table captured (power 0 = {table[0]}, power 15 = {table[15]})")

        # --- 2. the control: wires with no source sit at 0 -------------------

        mark = len(bot.seen)
        for cell in ROW + TURN:
            assert setblock(bot, cell, "minecraft:redstone_wire[power=0]")[0] == table[0]
        setblock(bot, NEAR_LAMP, "minecraft:redstone_lamp[lit=false]")
        setblock(bot, FAR_LAMP, "minecraft:redstone_lamp[lit=false]")
        drain(bot, 2.0)

        # Nothing powered itself: every update at a run cell in the whole window
        # is power 0, and both lamps are dark.
        run_updates = [(at, state) for at, state in block_updates(bot.seen[mark:])
                       if at in ROW + TURN]
        assert len(run_updates) >= len(ROW) + len(TURN), (
            f"the run's own placements are missing: {run_updates}")
        assert all(state == table[0] for _at, state in run_updates), (
            f"an unsourced wire powered itself: "
            f"{[(at, state) for at, state in run_updates if state != table[0]]}")
        settled = last_states(bot, mark)
        for lamp, name in ((NEAR_LAMP, "near"), (FAR_LAMP, "far")):
            assert settled.get(lamp) == dark_lamp_id, f"the {name} lamp lit with no source"
        # The state each cell held before the source existed: a cell the source
        # never reaches is never rewritten, so this is what it still reads.
        control = settled
        step("  OK control: every wire at 0 and both lamps dark while unsourced")

        # --- 3. the source: the command's neighbour pass settles the run -----

        # The fifteen steps the source reaches: powers 15, 14, ... 1. Each is a
        # real rewrite, so each is a broadcast to wait for.
        powered = {}
        for step_index, cell in enumerate(ROW):
            powered[cell] = table[15 - step_index]
        for turn_index, cell in enumerate(TURN[:2]):
            powered[cell] = table[2 - turn_index]
        powered[NEAR_LAMP] = lit_lamp_id
        # Past the reach: the sixteenth step and the lamp beside it keep the state
        # the control left them in, so there is no update to wait on — they are
        # asserted as never *powered* below.

        mark = len(bot.seen)
        setblock(bot, SOURCE, "minecraft:redstone_block")
        found, _updates = observe_until(bot, settled_to(bot, mark, powered), mark, seconds=20.0)
        assert found, (
            "the run never settled to the vanilla ladder; latest states: "
            f"{last_states(bot, mark)}")
        step("  OK source placed -> the run settled through the command's neighbour pass")

        # --- 4. the ladder, cell by cell -------------------------------------

        settled = last_states(bot, mark)
        ladder = []
        for step_index, cell in enumerate(ROW):
            expected_power = 15 - step_index
            actual = settled.get(cell, control.get(cell))
            assert actual == table[expected_power], (
                f"wire step {step_index + 1} at {cell} reads {actual}, "
                f"not power {expected_power} ({table[expected_power]})")
            ladder.append(expected_power)
        for turn_index, cell in enumerate(TURN):
            expected_power = 2 - turn_index
            actual = settled.get(cell, control.get(cell))
            assert actual == table[expected_power], (
                f"the turn at {cell} reads {actual}, not power "
                f"{expected_power} ({table[expected_power]})")
            ladder.append(expected_power)
        assert ladder == list(range(15, -1, -1)), ladder
        step(f"  OK every step of the run: {ladder}")

        # --- 5. the cut-off and the two consumers ---------------------------

        # Nothing beyond the fifteenth step was ever written: the run's reach is
        # bounded, which is the whole difference between attenuation and every
        # cell reading 15.
        beyond = [at for at, _state in block_updates(bot.seen[mark:])
                  if at in (TURN[2], FAR_LAMP)]
        assert not beyond, f"cells past the fifteenth step were rewritten: {beyond}"
        assert settled.get(NEAR_LAMP, control.get(NEAR_LAMP)) == lit_lamp_id, (
            "the lamp beside the first wire is not lit: "
            f"{settled.get(NEAR_LAMP, control.get(NEAR_LAMP))}")
        far = settled.get(FAR_LAMP, control.get(FAR_LAMP))
        assert far == dark_lamp_id, (
            f"the lamp past the fifteenth step is lit ({far}) — the run saturated "
            "instead of attenuating")
        step("  OK near lamp lit, far lamp dark (the run's reach ends at 15)")

        # --- 6. cutting the source relaxes the whole run back ----------------

        # Every cell that carried power must fall back to 0 and the lit lamp must
        # go out; the cell that was already 0 has nothing to write.
        relaxed = {cell: table[0] for cell in ROW + TURN[:2]}
        relaxed[NEAR_LAMP] = dark_lamp_id
        mark = len(bot.seen)
        setblock(bot, SOURCE, "minecraft:air")
        found, _updates = observe_until(bot, settled_to(bot, mark, relaxed), mark,
                                        seconds=20.0)
        assert found, (
            f"the run never relaxed after the source was cut: {last_states(bot, mark)}")
        settled = last_states(bot, mark)
        for cell in ROW + TURN:
            actual = settled.get(cell, control.get(cell))
            assert actual == table[0], f"the run did not relax at {cell}: {actual}"
        near = settled.get(NEAR_LAMP, lit_lamp_id)
        assert near == dark_lamp_id, f"the near lamp stayed lit: {near}"
        step("  OK source removed -> the run relaxed back to 0 and the lamp went out")

        bot.close()
        print("redstone wire: all steps passed")
        return 0
    finally:
        if bot is not None:
            try:
                bot.close()
            except Exception:
                pass
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:  # pragma: no cover
                proc.kill()
        shutil.rmtree(world, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
