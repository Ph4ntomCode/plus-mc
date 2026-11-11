#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/world/fire_table.h: FireBlock's per-block flammability table.

Flammability is *not* a datapack in 26.2 — `FireBlock.bootStrap()` fills two
Object2IntMaps in code, one `setFlammable(block, igniteOdds, burnOdds)` call per
block, and both of `FireBlock`'s readers go through them:

    getIgniteOdds(state) -> 0 when waterlogged, else igniteOdds[state.getBlock()]
    canBurn(state)       -> getIgniteOdds(state) > 0

`canBurn` is what decides whether a block *feeds* fire, and it is therefore what
`getStateForPlacement` (the connected-face properties), `canSurvive` (fire lives
next to or above something burnable) and the spread loop all ask. So the
authoritative source is that one method, transcribed here in its exact order.

Both columns have a caller: world/fire.cpp's `fire_tick` (the port of
`FireBlock.tick`) reads the ignite odds for the spread and the burn odds for the
six-neighbour burn-out that consumes what the fire is sitting on.

Usage:
    tools/gen_fire_table.py --java-dir build/_decomp/net/minecraft/world/level/block
    tools/gen_fire_table.py --tsv data/vanilla/26.2/fire_flammability.tsv
"""

import argparse
import pathlib
import re
import sys

_NS = "minecraft:"


def block_names(header_path):
    """Every block name in the generated default-state table.

    `src/world/default_block_states.h` is one entry per block (its default
    state), so it is the complete block-name list to check the Java constants
    against — a `Blocks.X` that does not lower-case into one of these is a
    generator bug, not a missing block, and must not be dropped silently.
    """
    text = header_path.read_text()
    body = text.split("kDefaultBlockStates[]", 1)[1].split("};", 1)[0]
    return {m for m in re.findall(r'\{"([^"]+)"', body)}


def parse_java(java_text):
    """`setFlammable(Blocks.X, ignite, burn)` calls, in source order."""
    rows = []
    for match in re.finditer(
            r"setFlammable\(\s*Blocks\.([A-Z0-9_]+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)",
            java_text):
        constant, ignite, burn = match.groups()
        rows.append((_NS + constant.lower(), int(ignite), int(burn)))
    return rows


def build(java_dir, blocks_header):
    java = (java_dir / "FireBlock.java").read_text()
    known = block_names(blocks_header)
    rows = parse_java(java)
    if not rows:
        raise SystemExit(f"no setFlammable calls found in {java_dir}/FireBlock.java")

    seen = {}
    unknown = []
    for name, ignite, burn in rows:
        if name not in known:
            unknown.append(name)
            continue
        # `setFlammable` writes into a map, so a repeated block overwrites.
        seen[name] = (ignite, burn)
    if unknown:
        raise SystemExit("blocks not in the generated block table: "
                         + ", ".join(sorted(unknown)))

    names = sorted(seen)
    return [(name, seen[name][0], seen[name][1]) for name in names]


def write_tsv(path, rows):
    lines = ["block\tignite_odds\tburn_odds"]
    lines += [f"{name}\t{ignite}\t{burn}" for name, ignite, burn in rows]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


def write_header(path, rows):
    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// clang-format off")
    out.append("//")
    out.append(f"// FireBlock's flammability table ({len(rows)} blocks, generated —")
    out.append("// do not hand-edit): block name -> `igniteOdds`, `burnOdds`.")
    out.append("//")
    out.append("// `FireBlock.bootStrap()` is code, not a datapack in this version, so the")
    out.append("// table is transcribed from that one method. `canBurn` — the ignite odds")
    out.append("// being non-zero — is what decides whether a block feeds fire, so it is what")
    out.append("// fire placement and fire survival ask (world/fire.cpp). Both columns are")
    out.append("// read: the ignite odds by the spread, the burn odds by the burn-out that")
    out.append("// consumes what a fire is sitting on (`fire_tick`).")
    out.append("//")
    out.append("// Regenerate with tools/gen_fire_table.py.")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("struct FireFlammability {")
    out.append("  const char* name; // \"minecraft:oak_planks\"")
    out.append("  int16_t ignite_odds;")
    out.append("  int16_t burn_odds;")
    out.append("};")
    out.append("")
    out.append("// Sorted by name, so fire_ignite_odds can binary-search it.")
    out.append("inline constexpr FireFlammability kFireFlammability[] = {")
    for name, ignite, burn in rows:
        out.append(f'    {{"{name}", {ignite}, {burn}}},')
    out.append("};")
    out.append(f"inline constexpr int kFireFlammabilityCount = {len(rows)};")
    out.append("")
    out.append("} // namespace mc::world")
    out.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(out))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--java-dir", type=pathlib.Path,
                        default=pathlib.Path(
                            "build/_decomp/net/minecraft/world/level/block"),
                        help="decompiled net/minecraft/world/level/block dir")
    parser.add_argument("--blocks", type=pathlib.Path,
                        default=pathlib.Path("src/world/default_block_states.h"),
                        help="the generated per-block default-state table")
    parser.add_argument("--tsv", type=pathlib.Path,
                        default=pathlib.Path(
                            "data/vanilla/26.2/fire_flammability.tsv"),
                        help="the flattened extraction")
    parser.add_argument("--header", type=pathlib.Path,
                        default=pathlib.Path("src/world/fire_table.h"))
    args = parser.parse_args()

    rows = build(args.java_dir, args.blocks)
    write_tsv(args.tsv, rows)
    write_header(args.header, rows)
    ignites = sorted({ignite for _, ignite, _ in rows})
    print(f"wrote {args.header}: {len(rows)} blocks, ignite odds {ignites}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
