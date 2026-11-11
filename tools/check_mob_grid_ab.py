#!/usr/bin/env python3
"""The `pen` scene's grid-vs-scan A/B, on two binaries.

`MobGrid` answers `BreedGoal.getFreePartner` and `FollowParentGoal.canUse` from
8-block buckets once the population reaches its switch, and from a population
scan below it. `tests/mob_digest_test.cpp`'s `pen` scene (120 animals) is the
only corpus past the switch, so its digest is the one that can tell the two
paths apart; the rest of the corpus is the control that says switching arms did
not move anything else.

The scan arm is a *build*: CMake compiles `src/world/mob.cpp` a second time with
`PLUSMC_MOB_GRID_OFF`, so `MobGrid` never builds in that binary and the scan
answers instead. The grid is a production tick path, so the arm is a
compile-time define rather than a variable a deployed server could carry:

    ctest -R mob-grid-ab
    python3 tools/check_mob_grid_ab.py build/plusmc-mob-digest-test \\
                                        build/plusmc-mob-digest-test-scan

It fails when a per-scene digest or the aggregate differs between the arms, and
also when the binaries do not report different arms -- that comparison would be
vacuous, and a define that stopped being honoured is exactly how this check
would rot into always passing.

The scan binary is not part of the default build (CMake marks it
EXCLUDE_FROM_ALL, so an ordinary build compiles `mob.cpp` once). A missing
binary exits 77, CTest's skip code, with the target to build: an input that has
not been built yet is not a pass.

Usage: check_mob_grid_ab.py GRID_BINARY SCAN_BINARY
"""

import os
import re
import subprocess
import sys

SKIP = 77

SCENE = re.compile(r"^\s*(\S+)\s+.*\sdigest\s([0-9a-f]{16})\s")
AGGREGATE = re.compile(r"^mob-digest: aggregate\s+([0-9a-f]{16})$")
ARM = re.compile(r"^mob-digest: the pen scene's 120 animals will answer from the (.+)$")

GRID_ARM = "grid"
SCAN_ARM = "population scan"


def run(binary):
    """The scene digests, the aggregate, and the arm the binary said it took."""
    proc = subprocess.run([binary, "--print"], capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"check_mob_grid_ab: {binary} --print exited {proc.returncode}\n{proc.stderr}")
    scenes, aggregate, arm = {}, None, None
    for line in proc.stdout.splitlines():
        match = SCENE.match(line)
        if match:
            scenes[match.group(1)] = match.group(2)
            continue
        match = AGGREGATE.match(line)
        if match:
            aggregate = match.group(1)
            continue
        match = ARM.match(line)
        if match:
            arm = match.group(1)
    if not scenes or aggregate is None or arm is None:
        sys.exit("check_mob_grid_ab: could not read the digest test's output "
                 f"(scenes {sorted(scenes)}, aggregate {aggregate}, arm {arm!r}); "
                 "its --print format changed")
    return scenes, aggregate, arm


def main():
    if len(sys.argv) != 3:
        print("Usage: check_mob_grid_ab.py GRID_BINARY SCAN_BINARY", file=sys.stderr)
        return 2
    grid_binary, scan_binary = sys.argv[1], sys.argv[2]
    missing = [path for path in (grid_binary, scan_binary)
               if not os.path.isfile(path)]
    if missing:
        print(f"mob-grid-ab: not built yet: {', '.join(missing)}", file=sys.stderr)
        print("The scan arm is excluded from the default build (one extra "
              "compilation of mob.cpp). To run the A/B:\n"
              "  cmake --build <build dir> --target plusmc-mob-digest-test-scan\n"
              "  ctest -R mob-grid-ab\n"
              "Skipping: a binary that has not been built is a missing input, "
              "not an equivalence that holds.", file=sys.stderr)
        return SKIP
    grid = run(grid_binary)
    scan = run(scan_binary)

    arms = (grid[2], scan[2])
    if arms != (GRID_ARM, SCAN_ARM):
        sys.exit(f"check_mob_grid_ab: the two binaries did not report different arms "
                 f"({grid_binary}: {arms[0]!r}, {scan_binary}: {arms[1]!r}); expected "
                 f"{GRID_ARM!r} then {SCAN_ARM!r}. The scan binary is built with "
                 f"PLUSMC_MOB_GRID_OFF; if it reports the grid, the define stopped "
                 f"being honoured and the comparison would be vacuous.")
    if list(grid[0]) != list(scan[0]):
        sys.exit(f"check_mob_grid_ab: the two binaries ticked different scenes "
                 f"({list(grid[0])} against {list(scan[0])})")
    if "pen" not in grid[0]:
        sys.exit("check_mob_grid_ab: the corpus has no `pen` scene, the only one "
                 f"past the population switch (scenes: {list(grid[0])})")

    width = max([len(name) for name in grid[0]] + [len("aggregate")])
    print("mob-digest A/B: the grid against the population scan")
    print(f"  grid  {grid_binary}")
    print(f"  scan  {scan_binary}  (PLUSMC_MOB_GRID_OFF)")
    print(f"  {'scene'.ljust(width)}  {'grid':16}  {'scan':16}  result")
    changed = []
    for name, digest in grid[0].items():
        same = digest == scan[0][name]
        if not same:
            changed.append(name)
        print(f"  {name.ljust(width)}  {digest:16}  {scan[0][name]:16}  "
              f"{'same' if same else 'DIFFERENT'}")
    aggregate_same = grid[1] == scan[1]
    if not aggregate_same:
        changed.append("aggregate")
    print(f"  {'aggregate'.ljust(width)}  {grid[1]:16}  {scan[1]:16}  "
          f"{'same' if aggregate_same else 'DIFFERENT'}")
    print()

    if changed:
        print(f"FAIL: {len(changed)} scene(s) answer differently from the buckets "
              f"than from the population scan: {', '.join(changed)}")
        print("The grid's candidate set, its tie-break or its one-block widen moved "
              "an answer. See docs/PHASE5.md, \"Where the switch sits\".")
        return 1

    print(f"pen: 120 animals answered from the {GRID_ARM} in one arm and the "
          f"{SCAN_ARM} in the other;")
    print(f"all {len(grid[0])} scenes and the aggregate agree ({grid[1]}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
