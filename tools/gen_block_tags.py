#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates src/world/block_tags.h from the vanilla 26.2 block tag data pack
# (`data/vanilla/26.2/data/minecraft/tags/block/*.json`): the name lists of the
# tags the block-tick loop asks about.
#
# A tag is a list of block names and of *other tags* (`"#minecraft:logs_that_burn"`),
# so the resolution here is the recursive one `TagLoader` performs: a tag is the
# union of its own entries and, transitively, of every tag it names. The output is
# the flattened per-block answer — a bitmask per block — because that is what a
# lookup needs at tick time (one binary search over `kBlockTags`).
#
# Only the tags the engine actually asks about are emitted; adding one is a line
# in TAGS below. Every resolved name is cross-checked against the project's own
# registry (src/world/block_states.h), so a tag that names a block this engine
# does not know is a data mismatch that fails the generation rather than a
# silently missing entry.
#
# Regenerate with: python3 tools/gen_block_tags.py

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAG_DIR = os.path.join(ROOT, "data", "vanilla", "26.2", "data", "minecraft", "tags", "block")
STATE_TABLE = os.path.join(ROOT, "src", "world", "block_states.h")
OUT = os.path.join(ROOT, "src", "world", "block_tag_table.h")

# The tags this engine reads, in bit order. The comment is the vanilla constant
# whose question the tag answers.
TAGS = [
    # `BlockTags.GROWS_CROPS`: the ground `CropBlock.getGrowthSpeed` draws its
    # speed from (farmland), which is the question the crop's random tick asks.
    # `mayPlaceOn`'s `SUPPORTS_CROPS` is a different tag with the same single
    # entry in 26.2, and this engine has no `canSurvive`, so it is not read.
    ("grows_crops", "GROWS_CROPS", "what a crop grows fastest on (farmland)"),
    # `BlockTags.PREVENTS_NEARBY_LEAF_DECAY`: what `LeavesBlock
    # .getOptionalDistanceAt` reports as distance 0. The tag itself is just
    # `["#minecraft:logs"]`, but reading the tag vanilla's code names is what
    # keeps this table's provenance checkable.
    ("prevents_nearby_leaf_decay", "PREVENTS_NEARBY_LEAF_DECAY",
     "the blocks whose presence keeps a leaf at distance 0"),
    # `BlockTags.ANIMALS_SPAWNABLE_ON` (Phase 4 milestone C): the ground an
    # animal's spawn position has to be standing on — `getBlockState(pos.below)
    # .is(BlockTags.ANIMALS_SPAWNABLE_ON)` is the first half of
    # `Animal.checkAnimalSpawnRules`. This is the *animal* list, not the general
    # `isValidSpawn` question (a monster's ground is any block with a sturdy top
    # face), which is why it is a tag and not a shape test. In 26.2 the tag is
    # `["minecraft:grass_block"]` alone — not the `#minecraft:dirt` family the
    # wiki's older notes describe — which is why the generated mask names one
    # block.
    ("animals_spawnable_on", "ANIMALS_SPAWNABLE_ON",
     "the ground an animal may spawn on (26.2: grass_block)"),
    # `BlockTags.PREVENT_MOB_SPAWNING_INSIDE` (Phase 4 milestone C): the blocks
    # `NaturalSpawner.isValidEmptySpawnBlock` refuses to spawn a mob *inside* —
    # the rails, so a minecart track cannot have a zombie standing on it.
    ("prevent_mob_spawning_inside", "PREVENT_MOB_SPAWNING_INSIDE",
     "a block a mob may never spawn inside (rails)"),
]


def read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def resolve_tag(name, cache, stack):
    """The flattened block names of `minecraft:<name>`, tags expanded."""
    if name in cache:
        return cache[name]
    if name in stack:
        print("TAG CYCLE: %s" % " -> ".join(stack + [name]), file=sys.stderr)
        sys.exit(1)
    path = os.path.join(TAG_DIR, name + ".json")
    if not os.path.exists(path):
        print("MISSING TAG FILE: %s" % path, file=sys.stderr)
        sys.exit(1)
    values = read_json(path).get("values", [])
    stack.append(name)
    out = set()
    for value in values:
        if isinstance(value, dict):
            # `{"id": ..., "required": false}` — the long form, which only ever
            # wraps a plain name or a tag reference.
            value = value.get("id", "")
        if value.startswith("#"):
            out |= resolve_tag(value[1:].split(":")[-1], cache, stack)
        else:
            out.add(value if ":" in value else "minecraft:" + value)
    stack.pop()
    cache[name] = out
    return out


def main():
    cache = {}
    per_block = {}  # name -> bitmask
    for bit, (tag, constant, _) in enumerate(TAGS):
        names = resolve_tag(tag, cache, [])
        if not names:
            print("EMPTY TAG: %s" % tag, file=sys.stderr)
            sys.exit(1)
        for name in names:
            per_block[name] = per_block.get(name, 0) | (1 << bit)
        print("%s (#minecraft:%s): %d blocks" % (constant, tag, len(names)))

    # Cross-check against the project's registry, exactly like the collision
    # table does: a name only this script knows is a data mismatch.
    registry = set(re.findall(r'\{"([a-z0-9_]+:[a-z0-9_/]+)",', open(STATE_TABLE, encoding="utf-8").read()))
    unknown = sorted(set(per_block) - registry)
    if unknown:
        print("NOT IN THE PROJECT REGISTRY (%d): %s" % (len(unknown), unknown[:20]), file=sys.stderr)
        sys.exit(1)

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("""// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// The block tags the block-tick loop asks about (%d blocks, generated — do not
// hand-edit), flattened out of the vanilla 26.2 tag data pack by
// tools/gen_block_tags.py. A tag is the union of its entries and of every tag
// it names (`#minecraft:prevents_nearby_leaf_decay` is `#minecraft:logs`, which
// is `#logs_that_burn` plus the two stem tags), so what is stored here is the
// *resolved* membership: one bitmask per block, because that is what a
// tick-time lookup wants.
//
#pragma once

#include <cstdint>

namespace mc::world {
""" % len(per_block))
        for bit, (tag, constant, comment) in enumerate(TAGS):
            f.write("\n// `BlockTags.%s`: %s.\nconstexpr uint8_t kBlockTag%s = 1u << %d;\n"
                    % (constant, comment, "".join(p.capitalize() for p in constant.split("_")), bit))
        f.write("""
struct GeneratedBlockTag {
  const char* name;
  uint8_t tags;
};

// Sorted by name, so block_has_tag (world/block_tags.h) can binary-search it.
// A block with no tags at all is absent.
inline constexpr GeneratedBlockTag kBlockTags[] = {
""")
        for name in sorted(per_block):
            f.write('    {"%s", %d},\n' % (name, per_block[name]))
        f.write("};\n")
        f.write("inline constexpr int kBlockTagCount = %d;\n" % len(per_block))
        f.write("\n} // namespace mc::world\n")

    print("wrote %s: %d blocks carry a tag" % (os.path.relpath(OUT, ROOT), len(per_block)))


if __name__ == "__main__":
    main()
