#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates src/world/random_tick_table.h from the decompiled vanilla 26.2 block
# registry: every block that is *randomly ticking*, and which family of
# behaviour its `randomTick` belongs to.
#
# `ServerLevel.tickChunk`'s block half only samples a chunk *section* whose
# `LevelChunkSection.isRandomlyTicking()` is true, and only ticks a sampled
# position whose `BlockState.isRandomlyTicking()` is true. Both are per-block
# answers, so both need the full list of randomly-ticking blocks — including the
# ones this engine does not simulate, or the pass would sample sections vanilla
# never samples and leave the rest unsampled (a crowd of copper blocks would
# make a wheat field grow slower).
#
# A block is randomly ticking when either
#   * its registration sets the `randomTicks()` property flag, which is what
#     `BlockBehaviour.isRandomlyTicking(state)` returns by default, or
#   * its class overrides `isRandomlyTicking(state)` with a state-dependent
#     answer (`CropBlock`: not at its last age; `LeavesBlock`: distance 7 and
#     not persistent; ...).
#
# Each block is therefore classified by its registration's *factory* class and
# its properties. `kind` is the family:
#   * `kCrop`   — the `CropBlock` family, whose tick this engine implements
#                 (`CropBlock.randomTick`), with its own `getMaxAge()`;
#   * `kLeaves` — the `LeavesBlock` family, implemented here too;
#   * `kOther`  — randomly ticking, but with no `randomTick` in this engine. The
#                 pass still counts these (so the sampling is faithful) and then
#                 ticks nothing, which is the same shape as the mob table's
#                 unmodelled spawn rules.
#
# The class hierarchy is read out of the decompiled sources (`extends` chains),
# and the max age out of the class's own `MAX_AGE` constant — so `beetroots`
# (3) and `torchflower_crop` (1) carry their real last age rather than the
# seven a bare `CropBlock` has. Every name is cross-checked against the
# project's registry (src/world/block_states.h).
#
# Regenerate with: python3 tools/gen_random_tick_table.py

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DECOMP = os.path.join(ROOT, "build", "_decomp", "net", "minecraft")
BLOCKS_JAVA = os.path.join(DECOMP, "world", "level", "block", "Blocks.java")
BLOCK_DIR = os.path.join(DECOMP, "world", "level", "block")
STATE_TABLE = os.path.join(ROOT, "src", "world", "block_states.h")
OUT = os.path.join(ROOT, "src", "world", "random_tick_table.h")

sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_collision_table as gct  # noqa: E402  (the registration walk is shared)

CLASS_RE = re.compile(r"(?:public|abstract|final|\s)*class (\w+)")
EXTENDS_RE = re.compile(r"class \w+\s+extends ([\w.]+)")
MAX_AGE_RE = re.compile(r"MAX_AGE\s*=\s*(\d+)")
IMPLEMENTS_RE = re.compile(r"class \w+\s+extends [\w.]+\s+implements (.*?)\{")


def class_parents():
    """class name -> its parent's simple name, from the decompiled sources."""
    parents = {}
    for fname in os.listdir(BLOCK_DIR):
        if not fname.endswith(".java"):
            continue
        source = gct.read(os.path.join(BLOCK_DIR, fname))
        cls = CLASS_RE.search(source)
        if not cls:
            continue
        ext = EXTENDS_RE.search(source)
        parents[cls.group(1)] = ext.group(1).split(".")[-1] if ext else None
    return parents


def reaches(parents, cls, ancestor):
    """Is `ancestor` `cls` or one of its ancestors?"""
    seen = set()
    while cls and cls not in seen:
        if cls == ancestor:
            return True
        seen.add(cls)
        cls = parents.get(cls)
    return False


def class_max_age(cls, parents):
    """`getMaxAge()` for a `CropBlock` family member.

    `MAX_AGE` on the class itself wins (`BeetrootBlock` 3, `TorchflowerCropBlock`
    1); otherwise the family's own seven (`CropBlock.getMaxAge`).
    """
    seen = set()
    while cls and cls not in seen:
        seen.add(cls)
        path = os.path.join(BLOCK_DIR, cls + ".java")
        if os.path.exists(path):
            m = MAX_AGE_RE.search(gct.read(path))
            if m:
                return int(m.group(1))
        cls = parents.get(cls)
    return 7


def family(class_name, parents):
    """(kind, max_age) for a registration's factory class."""
    if reaches(parents, class_name, "CropBlock"):
        return "kRandomTickCrop", class_max_age(class_name, parents)
    if reaches(parents, class_name, "LeavesBlock"):
        return "kRandomTickLeaves", 0
    return "kRandomTickOther", 0


def registration_factories(source):
    """block constant -> (factory class, properties expression).

    The two `Blocks.register` forms (`register(id, properties)` and
    `register(id, factory, properties)`) and the collection registrations, all
    of which the collision generator already parses; only the factory class is
    new here.
    """
    out = {}
    for line in source.splitlines():
        m = gct.REGISTRATION_RE.search(line.strip())
        if not m:
            continue
        const, call, args = m.group(1), m.group(2), gct.split_args(m.group(3))
        if call != "Blocks.register" or len(args) not in (2, 3):
            continue
        factory = ""
        if len(args) == 3:
            # `CropBlock::new`, `(Properties p) -> new TintedParticleLeavesBlock(...)`.
            new_match = re.search(r"new (\w+)\(", args[1])
            if new_match:
                factory = new_match.group(1)
            elif "::new" in args[1]:
                factory = args[1].split("::new")[0].split(".")[-1]
        out[const] = (factory, args[-1], args[0])
    return out


def main():
    names = gct.load_names()
    collections = gct.load_collections()
    source = gct.read(BLOCKS_JAVA)
    helpers = gct.load_helper_bodies(source)
    parents = class_parents()

    # ---- the plain registrations -------------------------------------------
    plain = registration_factories(source)
    if not plain:
        print("NO REGISTRATIONS PARSED", file=sys.stderr)
        sys.exit(1)

    # ---- the collection registrations --------------------------------------
    collection_props = {}  # constant -> the properties expression
    for const, args in re.findall(
            r"public static final (?:ColorCollection|WeatheringCopperCollection)<Block>"
            r" (\w+) = (?:ColorCollection|WeatheringCopperCollection)\.registerBlocks\(([^;]*)\);",
            source, re.DOTALL):
        collection_props[const] = gct.split_args(args)[-1]

    # ---- classify -----------------------------------------------------------
    kinds = {}    # name -> (kind, max_age)
    unclassified = []
    for const, (factory, props, ids) in plain.items():
        id_const = ids.split(".")[-1]
        if id_const not in names:
            print("NO ID for %s (%s)" % (const, ids), file=sys.stderr)
            sys.exit(1)
        name = names[id_const]
        resolved = gct.inline_helpers(props, helpers)
        ticking = "randomTicks()" in resolved
        has_factory = bool(factory)
        if not has_factory and not ticking:
            continue
        if has_factory:
            kind, max_age = family(factory, parents)
            if kind == "kRandomTickOther" and not ticking:
                # A factory class that neither overrides `isRandomlyTicking`
                # nor asks for random ticks is not a randomly-ticking block:
                # `Blocks.register(id, factory, properties)` passes properties
                # that may set neither.
                if not overrides_random_tick(factory, parents):
                    continue
        else:
            kind, max_age = "kRandomTickOther", 0
        kinds[name] = (kind, max_age)
    for const, props in collection_props.items():
        # Copper families: the weathering classes override `isRandomlyTicking`.
        names_of = []
        for holder in (const,):
            if holder in collections:
                names_of = ["minecraft:" + n for n in collections[holder]]
        if not names_of:
            unclassified.append(const)
            continue
        for name in names_of:
            kinds.setdefault(name, ("kRandomTickOther", 0))

    if unclassified:
        print("UNRESOLVED COLLECTIONS: %s" % unclassified, file=sys.stderr)
        sys.exit(1)

    # ---- cross-check against the project's own registry ---------------------
    registry = set(re.findall(r'\{"([a-z0-9_]+:[a-z0-9_/]+)",', gct.read(STATE_TABLE)))
    unknown = sorted(set(kinds) - registry)
    if unknown:
        print("NOT IN THE PROJECT REGISTRY (%d): %s" % (len(unknown), unknown[:20]), file=sys.stderr)
        sys.exit(1)

    by_kind = {}
    for name, (kind, _) in kinds.items():
        by_kind[kind] = by_kind.get(kind, 0) + 1
    print("%d randomly ticking blocks: %s" % (len(kinds), sorted(by_kind.items())))

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("""// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// Every randomly-ticking block of the 26.2 registry (%d of them, generated — do
// not hand-edit) and the family its `randomTick` belongs to, from
// tools/gen_random_tick_table.py.
//
// `ServerLevel.tickChunk` asks both questions this table answers: whether a
// chunk *section* holds a randomly-ticking block (a per-cell count, which
// world/chunk.h maintains) and whether a sampled *position* is randomly ticking
// right now (`BlockState.isRandomlyTicking`, which is state-dependent for the
// families below).
//
//   * `kCrop`   — the `CropBlock` family (`CropBlock.randomTick`, and the
//                 `getMaxAge` its age property ends at): this engine grows
//                 these (world/crop.h). `max_age` is the block's own last age,
//                 so `beetroots` (3) and `torchflower_crop` (1) are not the
//                 seven a plain wheat crop has;
//   * `kLeaves` — the `LeavesBlock` family (`LeavesBlock.randomTick`): this
//                 engine decays and drops these (world/leaves.h);
//   * `kOther`  — randomly ticking with no `randomTick` here: bamboo, the
//                 weathering coppers, nether wart, redstone ore, the growing
//                 plants, flower pots, ... They still count towards the
//                 section's tick count, so a field of them does not make the
//                 rest of the chunk sample more often, and a sampled position
//                 ticks nothing. A handful of these answer
//                 `isRandomlyTicking` per *state* (nether wart below age 3, an
//                 unlit redstone ore, a potted flower); this table reports
//                 their whole block as ticking, which can only cost a sample.
#pragma once

#include <cstdint>

namespace mc::world {

enum RandomTickKind : uint8_t {
  kRandomTickNone = 0,
  kRandomTickCrop,
  kRandomTickLeaves,
  kRandomTickOther,
};

struct GeneratedRandomTickBlock {
  const char* name;
  uint8_t kind;
  // `getMaxAge()` for `kRandomTickCrop`, 0 otherwise.
  uint8_t max_age;
};

// Sorted by name, so random_tick_kind (world/random_tick.h) can binary-search
// it; a block that is absent never random-ticks.
inline constexpr GeneratedRandomTickBlock kRandomTickBlocks[] = {
""" % len(kinds))
        for name in sorted(kinds):
            kind, max_age = kinds[name]
            f.write('    {"%s", %s, %d},\n' % (name, kind, max_age))
        f.write("};\n")
        f.write("inline constexpr int kRandomTickBlockCount = %d;\n" % len(kinds))
        f.write("\n} // namespace mc::world\n")

    print("wrote %s" % os.path.relpath(OUT, ROOT))


def overrides_random_tick(class_name, parents):
    """Does `class_name` (or an ancestor) declare `isRandomlyTicking`?

    `BlockBehaviour.isRandomlyTicking` is the only implementation that reads the
    property flag, and it lives in the block-state package, not in the block
    directory, so a class found here is an override.
    """
    seen = set()
    while class_name and class_name not in seen:
        seen.add(class_name)
        path = os.path.join(BLOCK_DIR, class_name + ".java")
        if os.path.exists(path):
            body = gct.read(path)
            if "boolean isRandomlyTicking" in body:
                return True
        class_name = parents.get(class_name)
    return False


if __name__ == "__main__":
    main()
