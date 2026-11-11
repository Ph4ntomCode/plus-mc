#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the 26.2 block-drop table for break-a-block (Milestone F).

Vanilla decides block drops with loot tables plus code-side tool rules. We
cannot ship a full loot-table interpreter yet, so this tool *flattens* every
`data/minecraft/loot_table/blocks/*.json` into an ordered rule list under a
small, explicit, documented model of "a player breaks a block bare-handed
(no enchantments, no shears)":

  * A table is a list of pools; every pool rolls exactly once (all vanilla
    block tables use `rolls: 1`). Each pool therefore yields at most one item.
  * A pool contributes only under its conditions. Conditions we *satisfy*:
    `survives_explosion`; `block_state_property` (kept as a runtime
    requirement on the block's actual state, see below); `inverted` when its
    term is not satisfiable (every `inverted` in the dataset wraps a tool
    check); `table_bonus` / `random_chance` (chance-based — satisfiable, but
    flagged `CHANCE` and only honoured for the block's first pool, so a
    secondary "5% sapling" pool never becomes a guaranteed drop).
    Conditions we do *not* satisfy: `match_tool` (we never model a tool or an
    enchantment as satisfying a loot path — the code-side pickaxe rule below
    covers "requires a tool"), `entity_properties`, `location_check`.
  * Within a pool, `alternatives` pick the first satisfiable child that has no
    chance condition; if every candidate is chance-based the first one wins.
    This is what makes gravel drop gravel (the flint path is chance-based)
    while a sapling-only pool still yields its sapling.
  * `block_state_property` becomes a runtime requirement (`requires` in the
    header): the rule only applies when the broken block's actual state has
    that property value (e.g. `age=7` for mature wheat, `type=double` for a
    double slab). The state comes from the world model at break time.
  * `set_count` gives the stack size; a conditional `set_count` (slab double)
    is emitted as an `alt_count` guarded by its own property requirement.
    Variable counts (`{min,max}`, `apply_bonus` formulae we cannot evaluate)
    use the minimum.

Tool rules come from two sources:

  * The `needs_{stone,iron,diamond}_tool` block tags give the *tier* (wooden /
    stone / iron / diamond pickaxe or better).
  * Whether a tool is needed *at all* is the code-side
    `requiresCorrectToolForDrops` flag, which is not in any data file; pass
    the list produced by tools/gen_tool_requirements.py (a Java dump of
    `BuiltInRegistries.BLOCK`) as the optional TOOL_LIST argument. Without it
    only the tagged blocks are gated, which under-reports badly (stone,
    cobblestone, stairs, ... would drop bare-handed).

A block that requires a tool but is not `mineable/pickaxe` needs something we
cannot model (shovel/shears/sword): it is emitted at tier 5 -> min_tier 6, a
level no pickaxe reaches, i.e. it never drops under this model.

Usage:
    gen_block_drops.py LOOT_DIR TAGS_DIR REPORT_BLOCKS_JSON OUT_HEADER [TOOL_LIST]
"""

import json
import pathlib
import sys

# min_tier emitted for a block that needs a tool we cannot model; no pickaxe
# reaches it (held_pickaxe_tier maxes out at 5).
TIER_UNMODELED_TOOL = 5

# Condition names we treat as satisfied (see the module docstring).
BENIGN = {"minecraft:survives_explosion"}
CHANCE = {"minecraft:table_bonus", "minecraft:random_chance"}
TOOLISH = {"minecraft:match_tool", "minecraft:entity_properties", "minecraft:location_check"}
# Tags -> the pickaxe tier that can legitimately harvest the block.
TIER_TAGS = [("needs_stone_tool", 1), ("needs_iron_tool", 2), ("needs_diamond_tool", 3)]

FLAG_CHANCE = 1 << 0


class Unsupported(Exception):
    """Raised when a table uses a shape this flattener cannot model."""


def cond_terms(cond):
    """Splits a condition into (satisfiable, chance, property_requirements)."""
    name = cond.get("condition")
    if name in BENIGN:
        return True, False, []
    if name in CHANCE:
        return True, True, []
    if name == "minecraft:block_state_property":
        reqs = [(k, str(v)) for k, v in cond.get("properties", {}).items()]
        if not reqs:
            return True, False, []
        return True, False, reqs
    if name == "minecraft:any_of":
        ok_any = False
        chance_any = False
        reqs = []
        for term in cond.get("terms", []):
            ok, ch, r = cond_terms(term)
            if ok:
                ok_any = True
                chance_any = chance_any or ch
                reqs.extend(r)
        return ok_any, chance_any, (reqs if ok_any else [])
    if name == "minecraft:inverted":
        inner_ok, _, _ = cond_terms(cond["term"])
        # We satisfy "not <tool check>" because we never model a tool as
        # satisfying a loot path. Inverting anything we *do* satisfy (a
        # property requirement) is not modellable without a full interpreter.
        return (not inner_ok), False, []
    if name in TOOLISH:
        return False, False, []
    raise Unsupported(f"unknown condition {name}")


def eval_conditions(conds):
    ok = True
    chance = False
    reqs = []
    for cond in conds:
        c_ok, c_chance, c_reqs = cond_terms(cond)
        if not c_ok:
            ok = False
        chance = chance or c_chance
        reqs.extend(c_reqs)
    return ok, chance, reqs


def cond_probability(cond):
    """The probability a *chance* condition passes at fortune 0 (else 1.0).

    `table_bonus` carries one probability per fortune level and the first is the
    unenchanted one, which is what the model assumes everywhere else;
    `random_chance` carries its one. `any_of` takes the best of its terms (the
    chance sits on one branch of it, as `leaves`' sapling pool does).
    """
    name = cond.get("condition")
    if name == "minecraft:table_bonus":
        chances = cond.get("chances", [])
        return float(chances[0]) if chances else 1.0
    if name == "minecraft:random_chance":
        return float(cond.get("chance", 1.0))
    if name == "minecraft:any_of":
        values = [cond_probability(term) for term in cond.get("terms", [])]
        return max(values) if values else 1.0
    return 1.0


def eval_probability(conds):
    """The product of a condition list's chance terms (1.0 when there are none)."""
    probability = 1.0
    for cond in conds:
        probability *= cond_probability(cond)
    return probability


def set_count(functions):
    """(base_count, alt_count, alt_requirements) from a `set_count` function."""
    base = 1
    alt = 0
    alt_reqs = []
    for fn in functions or []:
        if fn.get("function") != "minecraft:set_count":
            continue
        ok, _, reqs = eval_conditions(fn.get("conditions", []))
        if not ok:
            continue  # a count we cannot reach (e.g. needs silk touch)
        count = fn.get("count", 1)
        if isinstance(count, dict):
            if count.get("min") is not None and count.get("max") is not None and \
                    float(count["min"]) == float(count["max"]):
                value = int(count["min"])
            else:
                value = int(count.get("min", 1))
        else:
            value = int(count)
        if reqs:
            alt, alt_reqs = value, reqs
        else:
            base = value
    return base, alt, alt_reqs


def flatten(block, table):
    """Returns the block's ordered list of drop rules (dicts).

    Within a pool the rules are ordered deterministic-first: a rule whose
    requirements are constant is preferred over a chance-based one, and among
    equals the vanilla child order is preserved. The runtime walks a pool's
    rules in order and takes the first whose requirements match, so a
    property-conditional alternative (wheat `age=7` -> wheat, else seeds)
    still resolves correctly at break time.
    """
    rules = []
    pools = table.get("pools", [])
    sequence = 0
    for pool_index, pool in enumerate(pools):
        if float(pool.get("rolls", 1)) != 1.0:
            raise Unsupported("pool rolls != 1")
        pool_ok, pool_chance, pool_reqs = eval_conditions(pool.get("conditions", []))
        if not pool_ok:
            continue
        pool_probability = eval_probability(pool.get("conditions", []))
        certain, chancy = [], []
        for entry, chance, reqs in candidates(pool.get("entries", []), pool_chance,
                                              pool_probability):
            item = entry.get("name")
            if not item or item == "minecraft:air":
                continue
            base, alt, alt_reqs = set_count(entry.get("functions"))
            rule = {
                "block": block,
                "item": item,
                "count": max(base, 1),
                "alt_count": alt,
                "pool": pool_index,
                # The chance this rule fires when its pool rolls: 1.0 for a
                # guaranteed drop, the table's first (unenchanted) probability
                # for a chance one.
                "chance": chance,
                "flags": FLAG_CHANCE if chance < 1.0 else 0,
                "reqs": pool_reqs + reqs,
                "alt_reqs": alt_reqs,
            }
            (chancy if chance < 1.0 else certain).append(rule)
        for rule in certain + chancy:
            rule["seq"] = sequence
            sequence += 1
            rules.append(rule)
    return rules


def candidates(entries, inherited_chance, inherited_probability=1.0):
    """Expands an entry list into ordered (item_entry, probability, reqs) candidates.

    Vanilla's `alternatives` take the first child whose conditions hold, so a
    dynamic (property-based) child cannot be decided at generation time: emit
    every satisfiable child in order and let the runtime requirement check pick
    the right one.

    The probability is carried down the nesting (`inherited_probability`), so a
    chance condition on a group applies to the items inside it.
    """
    out = []
    for entry in entries:
        etype = entry.get("type")
        ok, _chance, reqs = eval_conditions(entry.get("conditions", []))
        if not ok:
            continue
        probability = eval_probability(entry.get("conditions", [])) * inherited_probability
        if etype == "minecraft:alternatives":
            for sub, sub_chance, sub_reqs in candidates(entry.get("children", []), False,
                                                        probability):
                out.append((sub, sub_chance, reqs + sub_reqs))
            continue
        if etype == "minecraft:group" or etype == "minecraft:sequence":
            for sub, sub_chance, sub_reqs in candidates(entry.get("children", []), False,
                                                        probability):
                out.append((sub, sub_chance, reqs + sub_reqs))
            continue
        if etype != "minecraft:item":
            raise Unsupported(f"entry type {etype}")
        out.append((entry, probability, reqs))
    return out


def expand_tag(tags_dir, name, seen=None):
    """Recursively expands a block tag (which may reference other block tags)."""
    seen = set() if seen is None else seen
    path = tags_dir / f"{name}.json"
    if not path.exists():
        return set()
    out = set()
    for value in json.load(open(path))["values"]:
        if value.startswith("#"):
            ref = value[1:].split(":")[-1]
            if ref in seen:
                continue
            seen.add(ref)
            out |= expand_tag(tags_dir, ref, seen)
        else:
            out.add(value)
    return out


def collect_tag_blocks(tags_dir, requires_tool, pickaxe_blocks):
    """{block: min pickaxe tier} from the needs_*_tool tags + the tool dump.

    Needs tags: 1 = wooden, 2 = stone, 3 = iron (emitted +1). Blocks that
    require a tool but carry no tier are tier 0 (any pickaxe) when they are
    mineable/pickaxe, else the unmodeled-tool sentinel.
    """
    tiers = {}
    for tag, tier in TIER_TAGS:
        for name in expand_tag(tags_dir, tag):
            tiers[name] = max(tiers.get(name, 0), tier)
    for block in requires_tool:
        if block in pickaxe_blocks:
            tiers[block] = tiers.get(block, 0)
        else:
            tiers[block] = max(tiers.get(block, 0), TIER_UNMODELED_TOOL)
    return tiers


def main():
    if len(sys.argv) not in (5, 6):
        print(__doc__)
        return 2
    loot_dir = pathlib.Path(sys.argv[1])
    tags_dir = pathlib.Path(sys.argv[2])
    blocks_json = pathlib.Path(sys.argv[3])
    out_path = pathlib.Path(sys.argv[4])

    # The code-side requiresCorrectToolForDrops dump (optional but strongly
    # recommended: without it only the tagged blocks are gated).
    requires_tool = set()
    if len(sys.argv) == 6:
        tool_list = pathlib.Path(sys.argv[5])
        if not tool_list.exists():
            print(f"tool list not found: {tool_list} (run gen_tool_requirements.py)",
                  file=sys.stderr)
            return 1
        requires_tool = {l.strip() for l in tool_list.read_text().splitlines() if l.strip()}
    else:
        print("warning: no TOOL_LIST given; only needs_*_tool tags gate drops "
              "(stone/stairs/concrete will drop bare-handed)", file=sys.stderr)

    pickaxe_blocks = expand_tag(tags_dir, "mineable/pickaxe")
    tiers = collect_tag_blocks(tags_dir, requires_tool, pickaxe_blocks)

    # Sanity: every tier-tagged needs_* block must be pickaxe-mined, so the
    # tier rule can be expressed purely as "held item is a pickaxe >= tier N".
    non_pickaxe_tagged = [b for b in tiers
                          if tiers[b] < TIER_UNMODELED_TOOL and b not in pickaxe_blocks]
    if non_pickaxe_tagged:
        print(f"needs_* blocks not mineable/pickaxe: {non_pickaxe_tagged[:10]}", file=sys.stderr)
        return 1
    unmodeled = sorted(b for b, t in tiers.items() if t >= TIER_UNMODELED_TOOL)
    if unmodeled:
        print(f"requires a non-pickaxe tool (never drops here): {len(unmodeled)}")

    rules = []
    skipped = []
    for path in sorted(loot_dir.glob("*.json")):
        block = f"minecraft:{path.stem}"
        table = json.load(open(path))
        try:
            rules.extend(flatten(block, table))
        except Unsupported as exc:
            skipped.append((path.stem, str(exc)))

    report_blocks = set(json.load(open(blocks_json)).keys())
    tabled = {r["block"] for r in rules}
    missing = sorted(report_blocks - {f"minecraft:{s}" for s, _ in skipped} - tabled)
    known = sorted({r["block"] for r in rules} | {f"minecraft:{s}" for s, _ in skipped})

    # Emit, sorted by (block, pool, order) so the runtime can binary-search the
    # block's range and then walk the rules in vanilla pool order.
    rules.sort(key=lambda r: (r["block"], r["pool"], r["seq"]))

    def prop_str(reqs):
        return ",".join(f"{k}={v}" for k, v in reqs)

    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// clang-format off",
        "//",
        "// Block drops for the 26.2 canonical version, flattened from the vanilla",
        "// loot tables by tools/gen_block_drops.py (regenerate, don't hand-edit).",
        "//",
        "// Model: a player breaks a block with no tool enchantments and no shears.",
        "// Each rule belongs to a loot pool (one roll per pool, so one drop per",
        "// pool); a rule applies when every property in `requires_state` matches the",
        "// broken block's actual state. CHANCE rules are only honoured for the",
        "// block's first pool, so secondary chance pools (sapling/apple/stick",
        "// from leaves) never turn into guaranteed drops.",
        "//",
        "// `chance` is that rule's own probability (1.0 for a guaranteed drop, a",
        "// `table_bonus`'s first entry -- fortune 0 -- for a chance one), which the",
        "// rolled resolver (world/loot.h's resolve_block_drops_rolled) honours for",
        "// *every* pool. The deterministic resolver above deliberately does not:",
        "// it exists for the break path's guaranteed drops.",
        "//",
        "// `alt_count`/`alt_requires` handle a conditional stack size (the double",
        "// slab); when `alt_requires` matches the state the count is `alt_count`,",
        "// otherwise `count`.",
        "//",
        "// min_pickaxe_tier is 1 (any pickaxe) for blocks that require a tool but",
        "// carry no tier, 2/3/4 from the vanilla needs_{stone,iron,diamond}_tool",
        "// tags, and 6 for blocks whose required tool we cannot model",
        "// (shovel/shears/sword) -- nothing reaches 6, so they never drop here.",
        "// The requirement itself is the code-side requiresCorrectToolForDrops",
        "// flag, dumped by tools/gen_tool_requirements.py.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mc::world {",
        "",
        "inline constexpr uint8_t kDropFlagChance = 0x01;",
        "",
        "struct GeneratedBlockDrop {",
        "  const char* block;         // broken block, \"minecraft:oak_log\"",
        "  const char* item;          // dropped item, \"minecraft:oak_log\"",
        "  const char* requires_state;  // \"age=7\" (empty = always applies)",
        "  const char* alt_requires;    // property gate for alt_count (empty = none)",
        "  int16_t count;             // stack size",
        "  int16_t alt_count;         // stack size when alt_requires matches (0 = none)",
        "  int16_t pool;              // vanilla loot pool index",
        "  uint8_t flags;             // kDropFlagChance for chance-based rules",
        "  float chance;              // probability when the rule's pool rolls",
        "};",
        "",
        f"inline constexpr GeneratedBlockDrop kBlockDrops[] = {{",
    ]
    for r in rules:
        reqs = prop_str(r["reqs"])
        alt = prop_str(r["alt_reqs"])
        lines.append(
            f'    {{"{r["block"]}", "{r["item"]}", "{reqs}", "{alt}", '
            f'{r["count"]}, {r["alt_count"]}, {r["pool"]}, {r["flags"]}, '
            f'{r["chance"]!r}f}},')
    lines += [
        "};",
        f"inline constexpr int kBlockDropCount = {len(rules)};",
        "",
        "// Blocks that only drop with the correct pickaxe (vanilla",
        "// needs_{stone,iron,diamond}_tool block tags).",
        "struct GeneratedBlockToolRequirement {",
        "  const char* block;  // \"minecraft:iron_ore\"",
        "  int8_t min_tier;    // 1 = wooden/golden, 2 = stone, 3 = iron, 4 = diamond+",
        "};",
        "",
        "inline constexpr GeneratedBlockToolRequirement kBlockToolRequirements[] = {",
    ]
    for block, tier in sorted(tiers.items()):
        lines.append(f'    {{"{block}", {tier + 1}}},')
    lines += [
        "};",
        f"inline constexpr int kBlockToolRequirementCount = {len(tiers)};",
        "",
        "} // namespace mc::world",
        "",
    ]
    out_path.write_text("\n".join(lines))

    print(f"blocks with tables: {len(known)} of {len(report_blocks)} in blocks.json")
    print(f"rules: {len(rules)}; tool-gated blocks: {len(tiers)}")
    if skipped:
        print(f"skipped (unsupported shape): {len(skipped)}")
        for name, why in skipped[:10]:
            print(f"  {name}: {why}")
    if missing:
        print(f"blocks with no loot table and no drop: {len(missing)}")
        print("  " + ", ".join(missing[:20]))
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
