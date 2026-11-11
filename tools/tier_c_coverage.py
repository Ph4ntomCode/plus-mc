#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase 4 — the Tier-C coverage ledger.

`docs/PLAN.md` §3.1 defines the tiers and §11 makes Phase 4's definition of
done *"Tier B green, Tier C ≥ 90% measured"*. Tier A and Tier B each have a
gate (`ctest -R tier-a-gate`, and the `tier-b-gate` beside this ledger); Tier C
had no measurement at all, so "≥ 90%" was an unbacked claim. This is that
measurement.

Tier C is *"every mob behavior, structure, biome, worldgen feature, enchant,
advancement, vanilla bug-for-bug behavior our Parity Suite can express."* Most
of that sentence is not enumerable from data; what **is** enumerable is the
content registries the sentence names. So the ledger measures exactly those,
each against one thing:

  * **total** — how many entries the 26.2 data pack registers, read from the
    pack itself (its `worldgen/**/*.json` files) or from the synchronized
    registry the server sends the client (`clientconfig/registries/*.txt`);
  * **carried** — how many of those vanilla ids actually name something this
    engine implements, read from *the engine artifact that would carry them*
    (a generated table, or the running code that consumes the registry).

The second column is the whole point: it is a *measurement*, not a claim. Every
dimension is measured by intersecting the pack's ids with the ids the artifact
actually holds, so a table that silently shrank would move the number. The
feature, carver and structure dimensions read the generated registry tables
(`worldgen_feature_table.h`, `worldgen_carver_table.h`,
`worldgen_structure_table.h`, from tools/gen_worldgen_registry_table.py); the
living-entity dimension reads `living_entity_table.h` against the game's own
`entity_types.tsv`. A dimension whose artifact were missing would measure 0 and
say so on its own row rather than hiding the gap inside an average. The row's
note then distinguishes a registry carried *and applied* (the ore, scattered-ore,
block-column, monster-room, freeze-top-layer, seagrass and fallen-tree features)
from one carried but not yet applied (the structure pieces beyond the shipwreck
kinds, which are applied).

There is no canonical way to fold named categories into one percentage, so the
ledger reports two and refuses to pretend either is *the* number:

  * **item-weighted (micro)** — every vanilla entry counts once, so the large
    registries (advancements, features) dominate. This is the number that
    answers "if a client walks the pack, what fraction works."
  * **category-weighted (macro)** — every named category counts once, so a
    whole missing stage (features, structures, carvers) is not diluted by the
    thousands of advancements that do work. This is the number that answers
    "how much of the *kind* of thing Tier C lists is here."

Both are printed. The gate this feeds (`--check`, run by `ctest -R
tier-coverage`) does not assert a threshold — the phase is not at 90% yet, and
a permanently red gate would be the same lie in the other direction. It asserts
that the ledger is *well-formed* (every dimension has a positive total, no
dimension carries more than it has, and the two aggregates are in [0, 1]), so a
broken measurement fails loudly instead of reporting a wrong number.

Usage:

    python3 tools/tier_c_coverage.py                # human table
    python3 tools/tier_c_coverage.py --json         # machine-readable
    python3 tools/tier_c_coverage.py --check        # table + invariants (ctest)
"""

import argparse
import json
import pathlib
import re
import sys

# The pack the rest of this repo is pinned to (`data/vanilla/<version>`), and
# the version whose `clientconfig/registries` the wire tables derive from.
PACK_VERSION = "26.2"

# Every `"minecraft:..."` literal in a generated header. Used to read the ids a
# table actually holds back out of it.
QUOTED_ID = re.compile(r'"(minecraft:[a-z0-9_/\.]+)"')

# A mob-roster row: `{ "minecraft:cow", ... }` at the start of a table line.
MOB_ROW = re.compile(r'\{\s*"(minecraft:[a-z0-9_]+)"')


def read_text(path):
    return pathlib.Path(path).read_text(encoding="utf-8", errors="ignore")


def registry_ids(repo, name):
    """The ids of a synchronized registry the server sends the client, one per
    line, in wire order (`clientconfig/registries/<name>.txt`)."""
    path = (repo / "data" / "vanilla" / PACK_VERSION / "clientconfig"
            / "registries" / f"{name}.txt")
    return [line.strip() for line in read_text(path).splitlines() if line.strip()]


def pack_ids(pack, subdir, suffix=".json"):
    """`minecraft:<relative-path-without-suffix>` for every file under a data
    pack registry directory."""
    base = pack / subdir
    out = set()
    for path in base.rglob(f"*{suffix}"):
        rel = path.relative_to(base)
        out.add("minecraft:" + "/".join(rel.with_suffix("").parts))
    return out


def header_ids(repo, relpath):
    """Every distinct `minecraft:` id named in a generated header."""
    return set(QUOTED_ID.findall(read_text(repo / relpath)))


def artifact_ids(repo, pattern):
    """Every distinct `minecraft:` id named across the headers matched by a
    repo-relative glob (one artifact may be split over several files)."""
    out = set()
    for path in repo.glob(pattern):
        out |= set(QUOTED_ID.findall(read_text(path)))
    return out


def mob_roster_rows(repo):
    """The number of rows in the generated mob table."""
    return len(MOB_ROW.findall(read_text(repo / "src" / "world" / "mob_table.h")))


def measure(repo, pack_version=PACK_VERSION):
    """Return the ledger: one dict per measured dimension plus the aggregates.

    Each dimension is `{key, label, total, carried, method, note}`; `total` and
    `carried` are integers, and `method` says in one line where each came from
    so a reader can re-derive it by hand.
    """
    repo = pathlib.Path(repo).resolve()
    pack = repo / "data" / "vanilla" / pack_version / "data" / "minecraft"
    rows = []

    def add(key, label, total, carried, method, note=""):
        rows.append({"key": key, "label": label, "total": total,
                     "carried": carried, "method": method, "note": note})

    # --- biomes -------------------------------------------------------------
    # The synchronized biome registry is the full 26.2 set (overworld, nether
    # and end). The engine carries every one: `biome_table.h` is the climate
    # row per biome (`worldgen/biome/*.json`), while the generator's own
    # parameter list is the overworld multi-noise preset (`worldgen_biome_*`).
    biomes = set(registry_ids(repo, "worldgen_biome"))
    carried_biomes = biomes & header_ids(repo, "src/world/biome_table.h")
    add("biomes", "biomes", len(biomes), len(carried_biomes),
        "clientconfig/registries/worldgen_biome.txt vs biome_table.h",
        "biome_table.h carries the full biome registry (climate per biome)")

    # --- the worldgen feature/carver/structure registries --------------------
    # Each dimension's `carried` is the intersection of the pack's ids with the
    # ids its generated table actually holds, the way the generated-table
    # dimensions below are measured: a table that silently shrank would move
    # the number. `worldgen_feature_table.h` carries both the configured and
    # the placed registries, so each is measured against it; the carver and
    # structure registries have their own table.
    for key, subdir, label, artifact, note in [
        ("configured_features", "worldgen/configured_feature",
         "configured features", "src/world/worldgen_feature*.h",
         "the registry is carried; ore, scattered ore, block column, monster room,"
         " freeze top layer, seagrass, fallen tree, simple block, block pile, disk,"
         " spring, tree and the three selector kinds are applied (the rest"
         " carried-only)"),
        ("placed_features", "worldgen/placed_feature",
         "placed features", "src/world/worldgen_feature*.h",
         "the registry is carried; every placement modifier the applied kinds"
         " use runs"),
        ("carvers", "worldgen/configured_carver",
         "configured carvers", "src/world/worldgen_carver*.h",
         "the registry is carried and the cave/canyon overworld kinds carve"
         " (the nether kind is table-carried only)"),
        ("structures", "worldgen/structure",
         "structures", "src/world/worldgen_structure*.h",
         "the registry is carried; pieces are not built yet"),
        ("structure_sets", "worldgen/structure_set",
         "structure sets", "src/world/worldgen_structure*.h",
         "the registry is carried; pieces are not built yet"),
    ]:
        total_ids = pack_ids(pack, subdir)
        carried = len(total_ids & artifact_ids(repo, artifact))
        add(key, label, len(total_ids), carried,
            f"data pack {subdir}/**/*.json vs {'/'.join(artifact.split('/')[1:])}",
            note)

    # --- enchantments --------------------------------------------------------
    enchants = set(registry_ids(repo, "enchantment"))
    carried_enchants = enchants & header_ids(repo, "src/world/enchantment_table.h")
    add("enchantments", "enchantments", len(enchants), len(carried_enchants),
        "clientconfig/registries/enchantment.txt vs enchantment_table.h")

    # --- advancements --------------------------------------------------------
    advancements = pack_ids(pack, "advancement")
    carried_advancements = advancements & header_ids(repo, "src/world/advancement_table.h")
    add("advancements", "advancements", len(advancements),
        len(carried_advancements),
        "data pack advancement/**/*.json vs advancement_table.h")

    # --- mobs ----------------------------------------------------------------
    # The living-entity registry is the full 93 types (`entity_types.tsv`);
    # `living_entity_table.h` carries all of them, while `mob_table.h` is the
    # 90-row subset WITH AI the engine simulates. The player, armour stand and
    # mannequin are living but not mobs, and the table names them so the ledger
    # can tell "carried but not simulated" from "missing".
    tsv = read_text(repo / "data" / "vanilla" / PACK_VERSION / "entity_types.tsv")
    living = {
        fields[1] for fields in (line.split("\t") for line in tsv.splitlines()[1:])
        if len(fields) > 1 and fields[1].startswith("minecraft:")}
    carried_mobs = living & header_ids(repo, "src/world/living_entity_table.h")
    add("mobs", "mob types", len(living), len(carried_mobs),
        "entity_types.tsv vs living_entity_table.h",
        f"mob_table.h simulates {mob_roster_rows(repo)} of them (AI roster)")

    total = sum(r["total"] for r in rows)
    carried = sum(r["carried"] for r in rows)
    macro = (sum(r["carried"] / r["total"] for r in rows) / len(rows)) if rows else 0.0
    return {
        "pack_version": pack_version,
        "repo": str(repo),
        "dimensions": rows,
        "item_weighted": {"carried": carried, "total": total,
                          "fraction": (carried / total) if total else 0.0},
        "category_weighted": {"categories": len(rows), "fraction": macro},
    }


def render(ledger):
    """The human table. Widths are fixed so repeated runs line up."""
    out = [f"Tier-C coverage ledger — pack {ledger['pack_version']}", ""]
    out.append(f"  {'dimension':<22}{'carried':>8} /{'total':>7}   {'':>6}"
               f"  method")
    out.append("  " + "-" * 94)
    for r in ledger["dimensions"]:
        frac = (r["carried"] / r["total"]) if r["total"] else 0.0
        out.append(f"  {r['label']:<22}{r['carried']:>8} /{r['total']:>7}"
                   f"   {frac * 100:>5.1f}%  {r['method']}")
        if r["note"]:
            out.append(f"  {'':<22}{'':>8} {'':>7}   {'':>6}  ({r['note']})")
    micro = ledger["item_weighted"]
    macro = ledger["category_weighted"]
    out.append("  " + "-" * 94)
    out.append(f"  {'item-weighted (micro)':<22}{micro['carried']:>8} /"
               f"{micro['total']:>7}   {micro['fraction'] * 100:>5.1f}%"
               "  every vanilla entry counts once")
    out.append(f"  {'category-weighted (macro)':<22}{'':>8}  {'':>7}   "
               f"{macro['fraction'] * 100:>5.1f}%  each named category counts once"
               f" ({macro['categories']})")
    return "\n".join(out)


def check(ledger):
    """The invariants `ctest -R tier-coverage` enforces. Returns a list of
    failure strings (empty when well-formed)."""
    problems = []
    for r in ledger["dimensions"]:
        if r["total"] <= 0:
            problems.append(f"{r['key']}: total is {r['total']}, expected > 0")
        if not (0 <= r["carried"] <= r["total"]):
            problems.append(f"{r['key']}: carried {r['carried']} outside "
                            f"[0, {r['total']}]")
    for name in ("item_weighted", "category_weighted"):
        frac = ledger[name]["fraction"]
        if not (0.0 <= frac <= 1.0):
            problems.append(f"{name}: fraction {frac} outside [0, 1]")
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", default=str(pathlib.Path(__file__).resolve().parent.parent),
                        help="repository root (default: the tool's parent tree)")
    parser.add_argument("--pack", default=PACK_VERSION,
                        help=f"data-pack version under data/vanilla (default: {PACK_VERSION})")
    parser.add_argument("--json", action="store_true",
                        help="print the ledger as JSON instead of the table")
    parser.add_argument("--check", action="store_true",
                        help="assert the ledger is well-formed (exit 1 otherwise)")
    args = parser.parse_args(argv)

    ledger = measure(args.repo, args.pack)
    if args.json:
        print(json.dumps(ledger, indent=2, sort_keys=True))
    else:
        print(render(ledger))

    if args.check:
        problems = check(ledger)
        if problems:
            print("\nFAIL: the ledger is malformed:", file=sys.stderr)
            for problem in problems:
                print(f"  - {problem}", file=sys.stderr)
            return 1
        micro = ledger["item_weighted"]
        macro = ledger["category_weighted"]
        print(f"\nPASS: the Tier-C ledger is well-formed — item-weighted "
              f"{micro['fraction'] * 100:.1f}%, category-weighted "
              f"{macro['fraction'] * 100:.1f}% "
              f"(threshold is the phase gate, not this test)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
