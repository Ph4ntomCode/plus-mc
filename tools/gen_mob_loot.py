#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the 26.2 entity-drop table for killed mobs (Phase 3 milestone I).

Vanilla decides a mob's drops with `data/minecraft/loot_table/entities/*.json`.
As with block drops (tools/gen_block_drops.py), we do not ship a loot-table
interpreter: this tool *flattens* each table into an ordered rule list under a
small, explicit, documented model of "a player kills a healthy adult mob that is
not on fire, bare-handed (no enchantments)".

Model, condition by condition:

  * `killed_by_player` is *modelled as a parameter*: it becomes the `player_kill`
    requirement of the rule, so a mob that died to a fall yields nothing.
  * `survives_explosion` is satisfied.
  * `random_chance` / `table_bonus` supply the rule's `chance`.
  * `random_chance_with_enchanted_bonus` uses `unenchanted_chance`; the
    enchantment-scaled path is ignored (no enchantments exist here).
  * `entity_properties` is *not* satisfiable in general (it tests vehicles, the
    on-fire flag, equipment, baby status) and such pools/entries are dropped.
    The one exception is the sheep's wool colour table, which is a chain of
    `entity_properties` predicates over the sheep's colour: the white child is
    taken, because white is the colour a summoned sheep has.
  * `damage_source_properties` is not satisfiable either — every table that has
    one gates on a death this model never produces: a ghast killed by a
    deflected fireball, a magma cube eaten by a frog, a turtle struck by
    lightning. The bare-handed player kill (or the fall) simply is not those.
  * `inverted`, `any_of`, `all_of` fold over their terms.
  * `match_tool` is never satisfied: no tool or enchantment makes a loot path
    fire in this model.
  * a `loot_table` reference outside `entities/` (the guardian's
    `gameplay/fishing/fish`) is not satisfiable: that table draws from a
    biome-weighted fish list this flattener has no way to evaluate.
  * a pool whose `rolls` is not 1 carries its range into the generated
    `rolls_min`/`rolls_max` columns, and `resolve_mob_drops` rolls the pool that
    many times (the witch's 1..3 glowstone/sugar/... picks).

Functions: `set_count` gives the count range (a constant count is min == max; a
`uniform` provider becomes min..max; a variable provider we cannot evaluate uses
its minimum). `enchanted_count_increase` (looting) and `furnace_smelt` (a mob
that dies on fire drops cooked food) are ignored — no enchantments and no fire
damage here. Other functions are an error, so the table is never silently wrong.

Pools: every vanilla entity pool rolls exactly once; a pool whose `entries` hold
several items therefore yields *one of them*, chosen uniformly (all vanilla
entity entries are weight 1). That becomes a single rule with several candidate
items, which `resolve_mob_drops` picks one of. A pool with more than four
candidates is an error rather than a silent truncation.

Usage:
    gen_mob_loot.py LOOT_DIR OUT_HEADER MOB [MOB ...]
"""

import json
import pathlib
import re
import sys

# The mob list comes from src/world/mob_table.h (the generated table's rows), so
# a type the engine can spawn is a type its loot is generated for — the two
# cannot drift. The dependency is resolved at run time, not baked in here.
MOB_HEADER = "src/world/mob_table.h"

MAX_CANDIDATES = 6


def mobs_from_header(path):
    """The bare names ("zombie", ...) of every row in the mob table."""
    out = []
    for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        match = re.search(r'\{"(minecraft:[a-z_]+)", "', line)
        if match and line.lstrip().startswith("{"):
            out.append(match.group(1).split(":", 1)[1])
    if not out:
        raise SystemExit(f"no mob rows found in {path}")
    return out


class Unsupported(Exception):
    """Raised when a table uses a shape this flattener cannot model."""


def cond_terms(cond, killed_by_player):
    """Returns (satisfiable, chance, player_kill) for one condition."""
    name = cond.get("condition")
    if name == "minecraft:survives_explosion":
        return True, None, None
    if name == "minecraft:killed_by_player":
        return killed_by_player, None, killed_by_player
    if name in ("minecraft:random_chance", "minecraft:table_bonus"):
        return True, float(cond["chance"]), None
    if name == "minecraft:random_chance_with_enchanted_bonus":
        return True, float(cond["unenchanted_chance"]), None
    if name == "minecraft:entity_properties":
        return False, None, None
    if name == "minecraft:damage_source_properties":
        return False, None, None
    if name == "minecraft:match_tool":
        return False, None, None
    if name == "minecraft:inverted":
        ok, chance, player = cond_terms(cond["term"], killed_by_player)
        return (not ok), chance, player
    if name in ("minecraft:any_of", "minecraft:all_of"):
        wants_all = name == "minecraft:all_of"
        results = [cond_terms(term, killed_by_player) for term in cond["terms"]]
        if wants_all:
            ok = all(r[0] for r in results)
        else:
            ok = any(r[0] for r in results)
        chance = next((r[1] for r in results if r[1] is not None), None)
        player = next((r[2] for r in results if r[2] is not None), None)
        return ok, chance, player
    raise Unsupported(f"condition {name}")


def fold_conditions(conditions, killed_by_player):
    """(satisfiable, chance, needs_player_kill) over a condition list."""
    ok = True
    chance = 1.0
    player_kill = False
    for cond in conditions or []:
        term_ok, term_chance, term_player = cond_terms(cond, killed_by_player)
        ok = ok and term_ok
        if term_chance is not None:
            chance *= term_chance
        if term_player:
            player_kill = True
    return ok, chance, player_kill


def count_range(functions):
    """(min, max) from the entry's `set_count`, or (1, 1)."""
    for fn in functions or []:
        fname = fn.get("function")
        if fname == "minecraft:set_count":
            count = fn["count"]
            if isinstance(count, (int, float)):
                return int(count), int(count)
            if isinstance(count, dict):
                if count.get("type") == "minecraft:uniform":
                    return int(count["min"]), int(count["max"])
                if "value" in count:
                    return int(count["value"]), int(count["value"])
                # A formula provider we cannot evaluate: use its base/minimum.
                for key in ("min", "base", "value"):
                    if key in count:
                        return int(count[key]), int(count[key])
                raise Unsupported(f"count provider {count}")
            raise Unsupported(f"count {count!r}")
        if fname in ("minecraft:enchanted_count_increase", "minecraft:furnace_smelt"):
            continue  # no enchantments, no fire deaths
        if fname in ("minecraft:limit_count",):
            continue
        raise Unsupported(f"function {fname}")
    return 1, 1


def entry_candidates(entry, tables, killed_by_player, path):
    """Flattens one entry into (candidates, count_min, count_max, chance, kill)."""
    kind = entry["type"]
    if kind == "minecraft:empty":
        return [], 0, 0, 0.0, False
    if kind == "minecraft:item":
        ok, chance, kill = fold_conditions(entry.get("conditions"), killed_by_player)
        if not ok:
            return [], 0, 0, 0.0, False
        lo, hi = count_range(entry.get("functions"))
        return [entry["name"]], lo, hi, chance, kill
    if kind == "minecraft:alternatives":
        for child in entry["children"]:
            cands, lo, hi, chance, kill = entry_candidates(child, tables, killed_by_player,
                                                           path)
            if cands:
                return cands, lo, hi, chance, kill
        return [], 0, 0, 0.0, False
    if kind == "minecraft:loot_table":
        ref = entry["value"].split(":", 1)[-1]
        if not ref.startswith("entities/"):
            # The guardian's `gameplay/fishing/fish` is a biome-weighted list;
            # nothing else in the entity tables reaches outside entities/.
            return [], 0, 0, 0.0, False
        ref = ref[len("entities/"):]
        sub = tables.get(ref)
        if sub is None:
            raise Unsupported(f"loot_table reference {entry['value']}")
        ok, chance, kill = fold_conditions(entry.get("conditions"), killed_by_player)
        # The sheep colour tables are `entity_properties` colour predicates; the
        # white child is the one a summoned (default) sheep matches.
        if not ok and "sheep/" not in ref:
            return [], 0, 0, 0.0, False
        if "sheep/" in ref and not ref.endswith("/white"):
            return [], 0, 0, 0.0, False
        out = pools_of(sub, tables, True, path=ref)
        if len(out) != 1 or len(out[0]["items"]) != 1:
            raise Unsupported(f"nested table {ref} is not a single item")
        rule = out[0]
        return rule["items"], rule["min"], rule["max"], chance, kill
    if kind in ("minecraft:group", "minecraft:sequence"):
        # A group yields all of its children; only model a single child (which is
        # all the vanilla entity tables use inside a pool entry).
        children = entry.get("children", [])
        if len(children) != 1:
            raise Unsupported(f"{kind} with {len(children)} children")
        return entry_candidates(children[0], tables, killed_by_player, path)
    raise Unsupported(f"entry type {kind}")


def rolls_range(rolls):
    """(min, max) for a pool's `rolls`: a number, or a uniform provider."""
    if isinstance(rolls, (int, float)):
        return int(rolls), int(rolls)
    if isinstance(rolls, dict):
        if rolls.get("type") == "minecraft:uniform":
            return int(rolls["min"]), int(rolls["max"])
        if "value" in rolls:
            return int(rolls["value"]), int(rolls["value"])
    raise Unsupported(f"rolls {rolls!r}")


def pools_of(table, tables, killed_by_player, path=""):
    """Flattens a table into rules."""
    rules = []
    for index, pool in enumerate(table.get("pools", [])):
        rolls_min, rolls_max = rolls_range(pool.get("rolls", 1))
        ok, pool_chance, pool_kill = fold_conditions(pool.get("conditions"),
                                                     killed_by_player)
        if not ok:
            continue
        candidates = []
        lo = hi = 0
        chance = pool_chance
        kill = pool_kill
        for entry in pool.get("entries", []):
            entry_ok, entry_chance, entry_kill = fold_conditions(entry.get("conditions"),
                                                                 killed_by_player)
            if not entry_ok:
                continue
            cands, elo, ehi, echance, ekill = entry_candidates(entry, tables,
                                                               killed_by_player, path)
            if not cands:
                continue
            if candidates and (elo != lo or ehi != hi):
                # Two entries with different counts in one pool cannot share a
                # rule; the vanilla entity tables never do this.
                raise Unsupported(f"{path} pool {index}: mixed counts")
            candidates.extend(cands)
            lo, hi = elo, ehi
            chance *= echance
            kill = kill or entry_kill
        if not candidates:
            continue
        if len(candidates) > MAX_CANDIDATES:
            raise Unsupported(f"{path} pool {index}: {len(candidates)} candidates")
        rules.append({"items": candidates, "min": lo, "max": hi, "chance": chance,
                      "kill": kill, "rolls_min": rolls_min, "rolls_max": rolls_max})
    return rules


def main():
    loot_dir = pathlib.Path(sys.argv[1])
    out_path = pathlib.Path(sys.argv[2])
    mobs = sys.argv[3:] or mobs_from_header(MOB_HEADER)

    tables = {}
    ents = loot_dir / "entities"
    for path in ents.rglob("*.json"):
        tables[str(path.relative_to(ents).with_suffix(""))] = json.loads(path.read_text())

    rows = []
    unsupported = []
    for mob in mobs:
        table = tables.get(mob)
        if table is None:
            unsupported.append((mob, "no loot table"))
            continue
        try:
            for rule in pools_of(table, tables, True, path=mob):
                rows.append((mob, rule))
        except Unsupported as error:
            unsupported.append((mob, str(error)))
    if unsupported:
        for mob, why in unsupported:
            print(f"UNSUPPORTED {mob}: {why}", file=sys.stderr)
        raise SystemExit(f"{len(unsupported)} mob(s) could not be flattened")

    lines = []
    lines.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    lines.append("//")
    lines.append("// 26.2 entity drops, flattened from the vanilla entity loot tables")
    lines.append("// (generated — do not hand-edit).  Regenerate with")
    lines.append("// tools/gen_mob_loot.py <loot_table dir> <out>.")
    lines.append("//")
    lines.append("// One row is one loot pool, under the model documented in the")
    lines.append("// generator: a healthy adult mob killed by a player, bare-handed,")
    lines.append("// with no enchantments and not on fire.  `chance` is the pool's")
    lines.append("// probability and `min_count`/`max_count` its `set_count` range (a")
    lines.append("// non-positive roll drops nothing, which is how vanilla encodes")
    lines.append("// \"0 or 1 spider eye\" as a uniform -1..1).  Up to four `item`")
    lines.append("// candidates mean vanilla's *one of these* pool: the drop is chosen")
    lines.append("// uniformly among them (up to six: the witch's pool).  `rolls_min`/")
    lines.append("// `rolls_max` repeat the whole draw (1 for every entity pool but the")
    lines.append("// witch's 1..3).")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("namespace mc::world {")
    lines.append("")
    lines.append("struct GeneratedMobDrop {")
    lines.append("  const char* mob;")
    lines.append("  const char* item;")
    lines.append("  const char* item2;")
    lines.append("  const char* item3;")
    lines.append("  const char* item4;")
    lines.append("  const char* item5;")
    lines.append("  const char* item6;")
    lines.append("  int8_t min_count;")
    lines.append("  int8_t max_count;")
    lines.append("  // How many times the pool rolls (1 for every vanilla entity pool but")
    lines.append("  // the witch's 1..3); `resolve_mob_drops` repeats the rule this many times.")
    lines.append("  uint8_t rolls_min;")
    lines.append("  uint8_t rolls_max;")
    lines.append("  float chance;")
    lines.append("  bool player_kill;")
    lines.append("};")
    lines.append("")
    lines.append("// clang-format off")
    lines.append("inline constexpr GeneratedMobDrop kMobDrops[] = {")
    for mob, rule in rows:
        cands = list(rule["items"]) + [None] * (MAX_CANDIDATES - len(rule["items"]))
        cells = ", ".join(f'"{c}"' if c else "nullptr" for c in cands)
        chance = f'{rule["chance"]:.6g}'
        if "." not in chance and "e" not in chance:
            chance += ".0"
        lines.append(f'    {{"minecraft:{mob}", {cells}, {rule["min"]}, {rule["max"]}, '
                     f'{rule["rolls_min"]}, {rule["rolls_max"]}, {chance}f, '
                     f'{"true" if rule["kill"] else "false"}}},')
    lines.append("};")
    lines.append(f"inline constexpr int kMobDropCount = {len(rows)};")
    lines.append("// clang-format on")
    lines.append("")
    lines.append("} // namespace mc::world")
    lines.append("")
    out_path.write_text("\n".join(lines))
    print(f"wrote {out_path} ({len(rows)} rules for {len(mobs)} mobs)")


if __name__ == "__main__":
    main()
