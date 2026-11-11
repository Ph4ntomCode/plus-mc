#!/usr/bin/env python3
"""How often the birth pair strolls out of the eight-block partner range.

`BreedGoal.canUse` looks eight blocks around a cow in love for a partner, and an
in-love animal that finds none has **no** movement goal at all: the breed goal
outranks the stroll, so `breed_step`'s early return stands it still while its
600 love ticks count down. `breeding-wire` therefore measures the pair's gap
before it feeds them and replaces a pair that has strolled apart
(`SCENE_BREED_GAP`). This harness measures the quantity that check is calibrated
against — the scene's fragility, measured rather than inferred from a long test
that failed once in ten runs.

Each trial is a fresh `/summon cow 2` in the same walled corridor, the same
free-strolling window the test's three gate steps take (about five seconds), and
then the pair's gap off `/mobs list`. A trial inside the range is fed — from
arm's length, `/tp`-ing to each cow first, because a right click past
`handleInteract`'s three blocks is refused for range and that is a different
question — and watched for its birth:

  * an in-range pair that does not breed fails the run: that is the tick's
    business, and it is exactly what `breeding-wire` asserts every run;
  * an out-of-range pair is counted and printed, not failed: it is the scene's
    fragility, the number a taller gate window or a faster stroll would move.
    Nothing was wrong with the tick when those pairs failed to breed. A pair
    inside the eight blocks but past `SCENE_BREED_GAP` (7.0) is counted as "at
    the edge" and not fed at all — that is the repair threshold `breeding-wire`
    itself uses, so a pair the test would have re-summoned does not make this
    harness blame the tick.

A trial that cannot be measured at all is reported too: `/mobs list` not carrying
both cows means one left the platform, and it is neither an in-range birth nor an
out-of-range pair.

**The corridor is a closed box, and this harness asserts it.** The two-block rim
is not climbable — a jump lifts feet about 0.9 of a block and the rim's top is
2.0 above the floor — so a stroll cannot put a cow on it, and the unit test
`test_pen_rim_is_not_climbable` walks six cows against the same rim for
forty-eight seconds to say so in one process. The other way onto the rim used to
be a summon whose ring landed a cow in the rim column: `/summon`'s unstuck pass
lifted the buried box a block at a time and stood the animal on the rim's top,
one stroll from the outside. `mob_place` now nudges a blocked summon to the
nearest supported free cell on its own level, so the same summon lands on the
corridor floor — and this harness proves it before the trials start by summoning
from a stance whose ring lands one cow *inside the rim column*. Every listing is
also checked for a cow off the floor: such a cow
is a containment failure of the scene (`leaks`), reported and failed, never
folded into the out-of-range count. Every trial begins with a `/tp` back to the
corridor's centre, so the player is never standing where the previous feed left
them when the next pair is placed (and the gap is sampled from one place).

Usage: breeding_scene_soak.py /path/to/plusmc [--trials 10] [--window 5.4]
"""

import argparse
import os
import statistics
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import breeding_test as bt  # noqa: E402
import wire_client  # noqa: E402

# `getFreePartner`'s inflated scan box: `forNonCombat().range(8.0)`.
BREED_RANGE = 8.0
# The gap the pair is actually fed at: `breeding-wire`'s own `SCENE_BREED_GAP`.
# A pair between 7 and 8 blocks is technically inside the breed range, but the
# two feeds and the teleports between them take a moment, and the second cow is
# still strolling until it is fed — a 7.3-block gap can be a 8.1-block gap by
# the time the breed goal looks. The wire test keeps a block of margin for that
# and re-summons outside it; this harness reports those pairs as "at the edge"
# rather than feeding them and calling the resulting non-birth the tick's fault.
FEED_GAP = getattr(bt, "SCENE_BREED_GAP", 7.0)
# The world clock, not the player's: a quiet birth is a couple of seconds, and
# this bound only carries a *loaded* box — the same number and reasoning as the
# test's own `WORLD_EVENT_TIMEOUT`, because a box shared with four other builds
# advances far fewer world ticks per wall second and a tight bound turns that
# into a red suite rather than a measurement.
BIRTH_TIMEOUT = bt.WORLD_EVENT_TIMEOUT
# How long the pair strolls unchecked before the gap is read. `breeding-wire`'s
# gate steps (the 26-block-up right click, the stone, the refused second
# feeding, each with its own drain) take about this long between the summon and
# the point where its own gap check runs.
WINDOW = 5.4
START = time.time()


def say(text):
    print(f"[{time.time() - START:6.2f}s] {text}", flush=True)


def feed_at(bot, entity_id, position):
    """`/tp` to the animal's own cell, then the right click.

    The pair has strolled for `WINDOW` seconds, so one of them can be past the
    three blocks `handleInteract` checks; a feed refused for range would look
    like a failed birth and answer a question this harness is not asking. The
    teleport and the interact are two packets on one ordered stream, so the
    server has moved the player before it sees the click.
    """
    bt.send_chat_command(bot, f"tp {position[0]:.2f} {position[1]:.2f} {position[2]:.2f}")
    bt.interact(bot, entity_id)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("--trials", type=int, default=10,
                    help="summon-and-wait trials (default 10; one wheat stack feeds 32)")
    ap.add_argument("--window", type=float, default=WINDOW,
                    help="seconds the pair strolls before the gap is read (default 5.4)")
    args = ap.parse_args()

    binary = os.path.realpath(args.binary)
    if not os.path.exists(binary):
        print(f"breeding_scene_soak: no binary at {binary}", file=sys.stderr)
        return 2
    if args.trials < 1 or args.window < 0.0:
        print("breeding_scene_soak: --trials must be at least 1, --window at least 0",
              file=sys.stderr)
        return 2

    data_root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-scene-soak-")
    env = dict(os.environ)
    env.update({
        "PLUSMC_KEEPALIVE_MS": "60000",
        "PLUSMC_KEEPALIVE_TIMEOUT_MS": "300000",
        "PLUSMC_DAY_CYCLE": "0",
        # The test's own environment: one-chunk tick regions (the pair straddles
        # an x=0 cell boundary), the player's clock from its packets, and no
        # natural spawner adding bodies to the scene.
        "PLUSMC_TICK_REGIONS": "1",
        "PLUSMC_SURVIVAL_CLOCK": "packet",
        "PLUSMC_MOB_SPAWNING": "0",
    })

    proc = None
    try:
        proc, port = wire_client.start_server(
            [binary, "--host", bt.HOST, "--port", "0", "--motd", "breeding scene soak",
             "--offline-mode", "--gamemode", "1", "--data", data_root, "--world", world,
             "--no-flat-world"], bt.wait_listening, env=env)
        say(f"server listening on {port}")
        bot = bt.Bot("SceneSoakBot", port)
        bot.join()
        # The read side goes to the reader thread: every window below is a quiet
        # `drain`, and the keep-alives have to keep being answered (the server's
        # own receive timeout is thirty seconds).
        bot.start_reader()
        bt.drain(bot, 1.2)

        for _ in range(3):
            bot.move_to(bt.PLAYER_X, 64.0, bt.PLAYER_Z)
            time.sleep(0.05)
        placed = bt.build_floor(bot)
        say(f"floor: {placed} blocks placed (the walled corridor breeding-wire runs in)")
        for _ in range(2):
            bot.move_to(bt.PLAYER_X, 64.0, bt.PLAYER_Z)
            time.sleep(0.05)
        # The wheat goes into hotbar 0 while the player is still creative:
        # `handleSetCreativeModeSlot` refuses a survival client outright, and
        # the feeding itself is the survival path the test uses.
        bt.set_creative_slot(bot, 36, bt.WHEAT_STACK, bt.ITEM_WHEAT)
        time.sleep(0.2)
        bt.drain(bot, 0.3)
        bt.send_chat_command(bot, "gamemode survival")
        bt.drain(bot, 0.8)

        # --- containment, proved before anything is measured ---------------
        # `/summon`'s ring is two blocks out along x at the player's own z, so a
        # summoner one cell in from the corridor's end puts one cow in the rim
        # column (x = 12.5, whose box overlaps the two-block wall) and the other
        # on floor cell 8. Both have to come back on the corridor's floor:
        # `mob_place` nudges a blocked summon to the nearest supported free cell
        # on its own level instead of lifting it onto the rim's top. A cow at
        # y = 66 here is the old failure — one stroll from the outside.
        bt.send_chat_command(bot, f"tp {bt.CORRIDOR_INTERIOR_X - 0.5} 64.0 {bt.PLAYER_Z}")
        bt.drain(bot, 0.6)
        bt.send_chat_command(bot, "summon cow 2")
        packets = bt.drain(bot, 1.5)
        edge_cows = [bt.read_spawn(b) for b in bt.all_of(packets, bt.CB_SPAWN_ENTITY)]
        edge_cows = [c for c in edge_cows if c["type"] == bt.ET_COW]
        assert len(edge_cows) == 2, f"the edge summon placed {len(edge_cows)} cows"
        for cow in edge_cows:
            bt.assert_inside_corridor(cow, where="the edge summon")
        say("containment: an edge summon landed both cows on the floor at "
            + ", ".join(f"x={c['x']:.2f}" for c in edge_cows))
        bt.send_chat_command(bot, "mobs kill")
        bt.drain(bot, 0.5)
        bt.send_chat_command(bot, f"tp {bt.PLAYER_X} 64.0 {bt.PLAYER_Z}")
        bt.drain(bot, 0.5)

        say(f"{args.trials} trials, {args.window:.1f} s of strolling before the gap is read")

        gaps = []
        out_of_range = 0
        at_the_edge = 0
        lost = 0
        fed = 0
        born = 0
        misses = []
        leaks = []  # (trial, eid, y): a cow not standing on the corridor's floor
        for trial in range(1, args.trials + 1):
            # Every trial starts from the same spot. The previous feed teleports
            # the player to whichever animal it fed last, and `/summon`'s ring is
            # two blocks out at the player's own z. `/tp` back to the corridor's
            # own centre first (`summon_cow_pair` does the same for every caller):
            # the ring then always lands on interior cells, and the gap samples
            # are taken from one place.
            bt.send_chat_command(bot, f"tp {bt.PLAYER_X} 64.0 {bt.PLAYER_Z}")
            bt.drain(bot, 0.4)
            # `/mobs kill` is the server's own reset: the previous trial's pair
            # (and anything it dropped) leaves before the fresh summon, so every
            # trial starts from the same four-block ring around the player.
            bt.send_chat_command(bot, "mobs kill")
            bt.drain(bot, 0.4)
            cow_a, cow_b = bt.summon_cow_pair(bot)
            time.sleep(args.window)
            bt.send_chat_command(bot, "mobs list")
            packets = bt.drain(bot, 1.2)
            text = bt.feedback(packets)
            listing = bt.mob_listing(text)
            # Containment, checked on every listing: every mob in the corridor
            # stands on its floor. Feet at 65 or 66 mean a cow is in the rim or
            # on its top, which nothing in this scene should produce any more —
            # the edge-summon check above proves a hostile placement is nudged
            # onto the floor, and the rim is two blocks against a 0.9 jump.
            floor = bt.FLOOR_Y + 1.0
            for eid, pos in listing.items():
                if abs(pos[1] - floor) > 1e-6:
                    leaks.append((trial, eid, pos))
            gap = bt.mob_gap(listing, cow_a["entity"], cow_b["entity"])
            if gap is None:
                # Neither answer is about the eight-block range: the listing is
                # missing a cow (it walked off the platform, which is one more
                # thing the scene does) or carried no chat reply at all.
                lost += 1
                say(f"trial {trial:2d}: no pair to measure: "
                    f"{text.strip() or '(no chat reply)'} "
                    f"packet ids {sorted({pid for pid, _ in packets})}")
                continue
            gaps.append(gap)
            if gap > BREED_RANGE:
                out_of_range += 1
                say(f"trial {trial:2d}: gap {gap:5.2f}  OUTSIDE the "
                    f"{BREED_RANGE:.0f}-block range (not fed)")
                continue
            if gap > FEED_GAP:
                # Inside the eight-block scan but without the block of margin
                # the feeds take; `breeding-wire` re-summons here. Counted as
                # the scene's edge, not fed, so a non-birth is not attributed to
                # the tick.
                at_the_edge += 1
                say(f"trial {trial:2d}: gap {gap:5.2f}  at the edge "
                    f"({FEED_GAP:.1f} < gap <= {BREED_RANGE:.0f}, not fed)")
                continue
            feed_at(bot, cow_a["entity"], listing[cow_a["entity"]])
            feed_at(bot, cow_b["entity"], listing[cow_b["entity"]])
            bt.drain(bot, 0.3)
            started = time.time()
            _, child, *_ = bt.wait_for_cow_birth(
                bot, {cow_a["entity"], cow_b["entity"]}, {}, timeout=BIRTH_TIMEOUT)
            fed += 1
            if child is None:
                misses.append(trial)
                say(f"trial {trial:2d}: gap {gap:5.2f}  in range, NO birth in "
                    f"{BIRTH_TIMEOUT:.0f}s")
            else:
                born += 1
                say(f"trial {trial:2d}: gap {gap:5.2f}  in range, birth after "
                    f"{time.time() - started:.1f}s")

        print()
        print(f"breeding_scene_soak: {len(gaps)}/{args.trials} trials measured, "
              f"{args.window:.1f} s of strolling each")
        if gaps:
            print(f"  gap: min {min(gaps):.2f}  median {statistics.median(gaps):.2f}  "
                  f"max {max(gaps):.2f}  (the partner range is {BREED_RANGE:.0f})")
            print(f"  out of range: {out_of_range}/{len(gaps)} "
                  f"({100.0 * out_of_range / len(gaps):.1f}%)")
            print(f"  at the edge: {at_the_edge}/{len(gaps)} (inside "
                  f"{BREED_RANGE:.0f} but past the {FEED_GAP:.1f} the feeds leave "
                  f"margin for)")
        else:
            print("  gap: no trial produced both cows in /mobs list")
        print(f"  births: {born}/{fed} fed pairs")
        # Both containment counters are printed even when they are zero: "no
        # cow left the floor" is the result this harness exists to show, and a
        # line that only appears on failure hides it.
        print(f"  lost: {lost}/{args.trials} trials (no pair to measure: a cow "
              f"left the platform)")
        print(f"  leaks: {len(leaks)} cow(s) off the corridor floor")
        if leaks:
            print(f"  leak detail: {leaks[:4]}", file=sys.stderr)
        print("  the out-of-range and edge counts are the scene's fragility "
              "(reported); a fed pair that does not breed is the tick's "
              "(failed); a cow off the floor is the scene's (failed).")
        if misses:
            print(f"breeding_scene_soak: FAIL — trial(s) {misses}", file=sys.stderr)
            return 1
        if leaks:
            print(f"breeding_scene_soak: FAIL — {len(leaks)} containment leak(s)",
                  file=sys.stderr)
            return 1
        return 0
    finally:
        if proc is not None:
            proc.terminate()
            proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
