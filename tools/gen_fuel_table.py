#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/world/fuel_table.h: every furnace fuel and its burn time.

Fuel is *not* a datapack in 26.3 — `FuelValues.vanillaBurnTimes` builds the
table in code from items and item tags, all as multiples of a 200-tick base
unit, and `AbstractFurnaceBlockEntity.getBurnDuration` is a lookup in it. So the
authoritative source is that one Java method, transcribed here in its exact
order: the builder is an insertion-ordered map whose `put` *overwrites*, so a
later `add` wins over an earlier one, and the trailing `remove(ItemTags
.NON_FLAMMABLE_WOOD)` takes the crimson/warped wood (added by the LOGS, PLANKS
... tags a moment earlier) back out. Reordering these lines silently changes
burn times, which is why the order is preserved verbatim.

Tags are expanded to item-id sets with the same recursive resolver the recipe
generator uses: a tag may list another tag, and a missing tag resolves to
nothing rather than failing (vanilla's "nothing satisfies it").

Usage:
    gen_fuel_table.py TAG_DIR ITEM_HEADER OUT_HEADER
"""

import json
import os
import re
import sys

# FuelValues.vanillaBurnTimes(registries, features, 200): the base unit is 200
# ticks (one item's worth of smelting).
BASE = 200


def load_item_ids(header_path):
    """{name: item id} from the generated item table's kItems[] array."""
    text = open(header_path).read()
    body = text.split("kItems[]", 1)[1].split("};", 1)[0]
    pairs = re.findall(r'\{"([^"]+)", (\d+),', body)
    return {name: int(i) for name, i in pairs}


def load_tags(tag_dir):
    """Resolves every item tag to a set of item names.

    Tags live at the top level *and* in subdirectories, so the walk recurses.
    """
    raw = {}
    for root, _dirs, files in os.walk(tag_dir):
        for filename in sorted(files):
            if not filename.endswith(".json"):
                continue
            path = os.path.join(root, filename)
            rel = os.path.relpath(path, tag_dir)[:-5].replace(os.sep, "/")
            raw["minecraft:" + rel] = json.load(open(path)).get("values", [])

    resolved = {}
    in_progress = set()

    def resolve(tag):
        if tag in resolved:
            return resolved[tag]
        if tag in in_progress:
            raise ValueError(f"tag cycle through {tag}")
        if tag not in raw:
            print(f"warning: unknown tag {tag}", file=sys.stderr)
            return set()
        in_progress.add(tag)
        out = set()
        for entry in raw[tag]:
            if isinstance(entry, dict):
                # {"id": ..., "required": false} — only the id matters.
                entry = entry.get("id", "")
            if entry.startswith("#"):
                out |= resolve(entry[1:])
            else:
                out.add(entry)
        in_progress.discard(tag)
        resolved[tag] = out
        return out

    for tag in raw:
        resolve(tag)
    return resolved


# The FuelValues table, in source order. ("item", NAME) or ("tag", NAME).
def fuel_entries():
    u = BASE
    return [
        ("item", "minecraft:lava_bucket", u * 100),
        ("item", "minecraft:coal_block", u * 8 * 10),
        ("item", "minecraft:blaze_rod", u * 12),
        ("item", "minecraft:coal", u * 8),
        ("item", "minecraft:charcoal", u * 8),
        ("tag", "minecraft:logs", u * 3 // 2),
        ("tag", "minecraft:bamboo_blocks", u * 3 // 2),
        ("tag", "minecraft:planks", u * 3 // 2),
        ("item", "minecraft:bamboo_mosaic", u * 3 // 2),
        ("tag", "minecraft:wooden_stairs", u * 3 // 2),
        ("item", "minecraft:bamboo_mosaic_stairs", u * 3 // 2),
        ("tag", "minecraft:wooden_slabs", u * 3 // 4),
        ("item", "minecraft:bamboo_mosaic_slab", u * 3 // 4),
        ("tag", "minecraft:wooden_trapdoors", u * 3 // 2),
        ("tag", "minecraft:wooden_pressure_plates", u * 3 // 2),
        ("tag", "minecraft:wooden_shelves", u * 3 // 2),
        ("tag", "minecraft:wooden_fences", u * 3 // 2),
        ("tag", "minecraft:fence_gates", u * 3 // 2),
        ("item", "minecraft:note_block", u * 3 // 2),
        ("item", "minecraft:bookshelf", u * 3 // 2),
        ("item", "minecraft:chiseled_bookshelf", u * 3 // 2),
        ("item", "minecraft:lectern", u * 3 // 2),
        ("item", "minecraft:jukebox", u * 3 // 2),
        ("item", "minecraft:chest", u * 3 // 2),
        ("item", "minecraft:trapped_chest", u * 3 // 2),
        ("item", "minecraft:crafting_table", u * 3 // 2),
        ("item", "minecraft:daylight_detector", u * 3 // 2),
        ("tag", "minecraft:banners", u * 3 // 2),
        ("item", "minecraft:bow", u * 3 // 2),
        ("item", "minecraft:fishing_rod", u * 3 // 2),
        ("item", "minecraft:ladder", u * 3 // 2),
        ("tag", "minecraft:signs", u * 1),
        ("tag", "minecraft:hanging_signs", u * 4),
        ("item", "minecraft:wooden_shovel", u * 1),
        ("item", "minecraft:wooden_sword", u * 1),
        ("item", "minecraft:wooden_spear", u * 1),
        ("item", "minecraft:wooden_hoe", u * 1),
        ("item", "minecraft:wooden_axe", u * 1),
        ("item", "minecraft:wooden_pickaxe", u * 1),
        ("tag", "minecraft:wooden_doors", u * 1),
        ("tag", "minecraft:boats", u * 6),
        ("tag", "minecraft:wool", u // 2),
        ("tag", "minecraft:wooden_buttons", u // 2),
        ("item", "minecraft:stick", u // 2),
        ("tag", "minecraft:saplings", u // 2),
        ("item", "minecraft:bowl", u // 2),
        ("tag", "minecraft:wool_carpets", 1 + u // 3),
        ("item", "minecraft:dried_kelp_block", 1 + u * 20),
        ("item", "minecraft:crossbow", u * 3 // 2),
        ("item", "minecraft:bamboo", u // 4),
        ("item", "minecraft:dead_bush", u // 2),
        ("item", "minecraft:short_dry_grass", u // 2),
        ("item", "minecraft:tall_dry_grass", u // 2),
        ("item", "minecraft:scaffolding", u // 4),
        ("item", "minecraft:loom", u * 3 // 2),
        ("item", "minecraft:barrel", u * 3 // 2),
        ("item", "minecraft:cartography_table", u * 3 // 2),
        ("item", "minecraft:fletching_table", u * 3 // 2),
        ("item", "minecraft:smithing_table", u * 3 // 2),
        ("item", "minecraft:composter", u * 3 // 2),
        ("item", "minecraft:azalea", u // 2),
        ("item", "minecraft:flowering_azalea", u // 2),
        ("item", "minecraft:mangrove_roots", u * 3 // 2),
        ("item", "minecraft:leaf_litter", u // 2),
    ]


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    tag_dir = sys.argv[1]
    item_header = sys.argv[2]
    out_path = sys.argv[3]

    item_ids = load_item_ids(item_header)
    tags = load_tags(tag_dir)

    # Insertion-ordered overwrite semantics: later entries replace earlier ones.
    times = {}
    for kind, name, ticks in fuel_entries():
        if kind == "item":
            names = [name]
        else:
            names = sorted(tags.get(name, set()))
            if not names:
                print(f"warning: fuel tag {name} resolved to nothing",
                      file=sys.stderr)
        for n in names:
            times[n] = ticks

    # ...and the trailing remove(NON_FLAMMABLE_WOOD).
    for n in tags.get("minecraft:non_flammable_wood", set()):
        times.pop(n, None)

    unknown = sorted(n for n in times if n not in item_ids)
    for n in unknown:
        print(f"warning: fuel item {n} is not in the item table", file=sys.stderr)
        del times[n]

    rows = sorted((item_ids[n], t) for n, t in times.items())

    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("//")
    out.append(f"// Every 26.3 furnace fuel ({len(rows)}, generated — do not hand-edit):")
    out.append("// item id -> burn time in ticks. Transcribed from")
    out.append("// `FuelValues.vanillaBurnTimes(registries, features, 200)` — fuel is")
    out.append("// code, not a datapack in this version, so there is no vanilla JSON to")
    out.append("// read instead. Tags are expanded here, so the runtime only ever looks")
    out.append("// at a flat item-id table (world/cooking.cpp does the lookup).")
    out.append("//")
    out.append("// Regenerate with tools/gen_fuel_table.py TAG_DIR ITEM_HEADER OUT_HEADER.")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("// Sorted by item id, so fuel_burn_ticks can binary-search it.")
    out.append("struct FuelEntry {")
    out.append("  int32_t item;")
    out.append("  int32_t ticks;")
    out.append("};")
    out.append("")
    out.append("inline constexpr FuelEntry kFuels[] = {")
    for item, ticks in rows:
        out.append(f"    {{{item}, {ticks}}},")
    out.append("};")
    out.append(f"inline constexpr int kFuelCount = {len(rows)};")
    out.append("")
    out.append("} // namespace mc::world")
    out.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(out))
    print(f"wrote {out_path}: {len(rows)} fuels, "
          f"{min((t for _, t in rows), default=0)}..{max((t for _, t in rows), default=0)} ticks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
