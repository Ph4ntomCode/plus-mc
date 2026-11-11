#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates src/world/block_item_table.h: which block each *block item* places,
# from the decompiled vanilla 26.2 item registry.
#
# The item and the block it places usually share a name (`minecraft:oak_log`),
# which is why the placement path could get away with looking the item's own name
# up in the block registry — but not always. Seventeen items are `BlockItem`s for
# a block with a *different* name, and they are exactly the plantable ones:
#
#     Items.registerItem(BlockItemIds.WHEAT_CROP, Items.createBlockItemWithCustomItemName(Blocks.WHEAT))
#     // `BlockItemIds.WHEAT_CROP = BlockItemId.create("wheat", "wheat_seeds")`
#
# so `minecraft:wheat_seeds` places `minecraft:wheat`, `minecraft:carrot` places
# `minecraft:carrots`, `minecraft:pumpkin_seeds` places `minecraft:pumpkin_stem`,
# and so on. Without this table a player cannot plant a crop at all (the seed's
# name is not a block), and the item named `minecraft:wheat` — which *is* an item
# and *is* a block — would wrongly place a crop.
#
# Two registration shapes cover every block item in 26.2:
#   * `Items.registerBlock(<BlockItemId>, <Blocks.X>[, ...])` — the plain form,
#     whose item name comes from the `BlockItemId` constant (`create("name")` or
#     `create("block_name", "item_name")`);
#   * `Items.registerItem(<BlockItemId>[.item()], Items.createBlockItemWithCustomItemName(<Blocks.X>))`
#     — the fifteen plantables plus the two custom-named ones (`redstone` ->
#     `redstone_wire`, `string` -> `tripwire`).
#
# The block name of a `Blocks.X` constant comes from that constant's own
# registration id, and both names are cross-checked against the project's
# registries (src/world/item_registry.h, src/world/block_states.h).
#
# Regenerate with: python3 tools/gen_block_item_table.py

import os
import pathlib
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DECOMP = os.path.join(ROOT, "build", "_decomp", "net", "minecraft")
ITEMS_JAVA = os.path.join(DECOMP, "world", "item", "Items.java")
BLOCK_ITEM_IDS = os.path.join(DECOMP, "references", "BlockItemIds.java")
BLOCK_IDS = os.path.join(DECOMP, "references", "BlockIds.java")
ITEM_HEADER = os.path.join(ROOT, "src", "world", "item_registry.h")
STATE_TABLE = os.path.join(ROOT, "src", "world", "block_states.h")
OUT = os.path.join(ROOT, "src", "world", "block_item_table.h")

sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_collision_table as gct  # noqa: E402  (the registration walk and name holders)

# `BlockItemId.create("block_name")` / `create("block_name", "item_name")`.
BLOCK_ITEM_ID_RE = re.compile(
    r"public static final BlockItemId (\w+) = BlockItemId\.create\(\"([a-z0-9_/]+)\"(?:, \"([a-z0-9_/]+)\")?\)")
REGISTER_BLOCK_RE = re.compile(
    r"public static final Item (\w+) = Items\.registerBlock\((BlockItemIds\.\w+(?:\.item\(\))?), (Blocks\.\w+)")
CUSTOM_NAME_RE = re.compile(
    r"public static final Item (\w+) = Items\.registerItem\((BlockItemIds\.\w+(?:\.item\(\))?), "
    r"Items\.createBlockItemWithCustomItemName\((Blocks\.\w+)\)")
# The dyed and weathering families register one block item per name in bulk
# (`ColorCollection.registerBlockItems(BlockItemIds.WOOL, Blocks.WOOL, ...)`, the
# weathering twin with `registerItems`). Both id collections expand to the same
# names on the item and block side, so every expanded name is its own block item.
COLLECTION_ITEM_RE = re.compile(
    r"public static final (?:ColorCollection|WeatheringCopperCollection)<Item> (\w+) = "
    r"(?:ColorCollection|WeatheringCopperCollection)\.register(?:BlockItems|Items)"
    r"\((BlockItemIds\.\w+(?:\.item\(\))?)")


def block_item_ids():
    """BlockItemIds constant -> (block name, item name)."""
    out = {}
    for const, block, item in BLOCK_ITEM_ID_RE.findall(gct.read(BLOCK_ITEM_IDS)):
        out[const] = ("minecraft:" + block, "minecraft:" + (item or block))
    return out


def block_names():
    """The `Blocks.X` constant -> the block name it registers."""
    names = {}
    for line in gct.read(gct.BLOCKS_JAVA).splitlines():
        m = gct.REGISTRATION_RE.search(line.strip())
        if not m:
            continue
        const, call, args = m.group(1), m.group(2), gct.split_args(m.group(3))
        # Every `Blocks.register*` form takes the id first (`register`,
        # `registerSlab`, `registerStair`, `registerWall`, `registerLegacyStair`).
        if not call.startswith("Blocks.register") or not args:
            continue
        names[const] = args[0].split(".")[-1]  # the id constant it is named by
    return names


def gen_collections():
    """The id collections (`BlockItemIds.WOOL`, ...) -> the names they expand to."""
    return gct.load_collections()


def block_item_pairs():
    """Item name -> the block it places, for every block item in `Items.java`.

    The single source of truth for both this table and the item table's
    `kItemIsBlock` flag (tools/gen_item_table.py calls it), so the two can never
    disagree about which items are placeable.
    """
    ids = block_item_ids()
    const_names = block_names()
    names = gct.load_names()  # id constant -> block name
    source = gct.read(ITEMS_JAVA)

    pairs = {}
    for item_const, id_const, block_const in REGISTER_BLOCK_RE.findall(source) + CUSTOM_NAME_RE.findall(source):
        id_name = id_const.split(".")[1]  # `BlockItemIds.X` or `BlockItemIds.X.item()`
        if id_name not in ids:
            print("NO ID for %s (%s)" % (item_const, id_const), file=sys.stderr)
            sys.exit(1)
        block_id_const = const_names.get(block_const.split(".")[-1])
        if block_id_const is None or block_id_const not in names:
            print("NO BLOCK NAME for %s (%s)" % (item_const, block_const), file=sys.stderr)
            sys.exit(1)
        block_name = names[block_id_const]
        item_name = ids[id_name][1]
        pairs[item_name] = block_name

    # ---- the collection registrations ---------------------------------------
    collections = gen_collections()
    for _const, id_const in COLLECTION_ITEM_RE.findall(source):
        id_name = id_const.split(".")[1]
        if id_name not in collections:
            print("NO ID COLLECTION for %s" % id_const, file=sys.stderr)
            sys.exit(1)
        for name in collections[id_name]:
            pairs.setdefault("minecraft:" + name, "minecraft:" + name)
    return pairs


def pairs_from_tsv(path: pathlib.Path):
    """The game's own `item\tblock` dump, used when no decompile is present."""
    pairs = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("item\t"):
            continue
        item, block = line.split("\t", 1)
        pairs[item] = block
    return pairs


def main():
    tsv = None
    if len(sys.argv) == 3 and sys.argv[1] == "--tsv":
        tsv = pathlib.Path(sys.argv[2])
    pairs = pairs_from_tsv(tsv) if tsv is not None else block_item_pairs()
    if not pairs:
        print("NO BLOCK ITEMS PARSED", file=sys.stderr)
        sys.exit(1)

    # ---- cross-check against the project's registries ------------------------
    items = set(re.findall(r'\{"([a-z0-9_]+:[a-z0-9_/]+)",', gct.read(ITEM_HEADER)))
    blocks = set(re.findall(r'\{"([a-z0-9_]+:[a-z0-9_/]+)",', gct.read(STATE_TABLE)))
    unknown_items = sorted(set(pairs) - items)
    unknown_blocks = sorted(set(pairs.values()) - blocks)
    if unknown_items:
        print("ITEMS NOT IN THE PROJECT REGISTRY (%d): %s" % (len(unknown_items), unknown_items[:20]),
              file=sys.stderr)
        sys.exit(1)
    if unknown_blocks:
        print("BLOCKS NOT IN THE PROJECT REGISTRY (%d): %s" % (len(unknown_blocks), unknown_blocks[:20]),
              file=sys.stderr)
        sys.exit(1)

    # The interesting half: the items whose block has a different name.
    renamed = sorted((item, block) for item, block in pairs.items() if item != block)
    print("block items: %d; of those renamed: %d" % (len(pairs), len(renamed)))
    for item, block in renamed:
        print("   %s -> %s" % (item, block))

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("""// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// Every 26.3 item that places a block (%d of them, generated — do not
// hand-edit), and the block it places. The pairs come from the game's own
// BlockItem registry (item -> block); tools/gen_block_item_table.py also
// accepts a decompiled Items.java as its other source.
//
// Most entries map an item to the block of the same name; the %d that do not are
// the plantables (`minecraft:wheat_seeds` -> `minecraft:wheat`,
// `minecraft:carrot` -> `minecraft:carrots`) and two custom-named ones
// (`minecraft:redstone` -> `minecraft:redstone_wire`, `minecraft:string` ->
// `minecraft:tripwire`). The table is what the placement path resolves a held
// item through (world/block_items.h), and it is also what marks an item as a
// block item at all — the item whose *name* happens to match a block
// (`minecraft:wheat`, the harvested crop) is not one.
#pragma once

#include <cstdint>

namespace mc::world {

struct GeneratedBlockItem {
  const char* item;
  const char* block;
};

// Sorted by item name, so block_item_block (world/block_items.h) can
// binary-search it.
inline constexpr GeneratedBlockItem kBlockItems[] = {
""" % (len(pairs), len(renamed)))
        for item in sorted(pairs):
            f.write('    {"%s", "%s"},\n' % (item, pairs[item]))
        f.write("};\n")
        f.write("inline constexpr int kBlockItemCount = %d;\n" % len(pairs))
        f.write("\n} // namespace mc::world\n")

    print("wrote %s" % os.path.relpath(OUT, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
