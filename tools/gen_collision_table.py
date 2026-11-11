#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates src/world/collision_table.h from the decompiled vanilla 26.2 block
# registry: the blocks whose `BlockState.blocksMotion()` is FALSE — the vanilla
# predicate `Heightmap.Types.MOTION_BLOCKING` is built from, and therefore the
# one that decides where a column's rain reaches.
#
# `BlockBehaviour.BlockStateBase.blocksMotion()` is
#
#     block != COBWEB && block != BAMBOO_SAPLING
#       && (forceSolidOn ? true
#           : forceSolidOff ? false
#           : <the collision shape covers 0.729 of the block's bounds>)
#
# (`calculateSolid`). The two flags are properties, so they are what this script
# reads; the shape term is code and is documented as the residual approximation
# in the generated header.
#
# Three registration forms have to be followed, because 26.2 registers most of
# its blocks through them:
#   * `Blocks.register(id, [factory,] properties)` — the plain form, and the
#     `registerStair` / `registerSlab` / `registerWall` / `registerLegacyStair`
#     convenience wrappers, which copy a base block's properties (and with them
#     both flags);
#   * `ColorCollection.registerBlocks(...)` / `WeatheringCopperCollection
#     .registerBlocks(...)` — one registration per dye colour / weathering
#     state, whose *names* come from the id collection in
#     `references/BlockItemIds.java` / `BlockIds.java`;
#   * `Blocks.<helper>(...)` property factories (`candleProperties`,
#     `shulkerBoxProperties`, ...), whose single-`return` bodies are inlined
#     before the flags are read.
#
# The block *names* come from the two id holders (`BlockItemId.create("name")`,
# `BlockItemId.create("block_name", "item_name")` — the first string is the
# block's). Every name is then cross-checked against the project's own registry
# (src/world/block_states.h): a block name the registry knows but this script
# never classified is a resolution bug, not a table entry, and it is fatal here.
#
# Regenerate with: python3 tools/gen_collision_table.py

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DECOMP = os.path.join(ROOT, "build", "_decomp", "net", "minecraft")
BLOCKS_JAVA = os.path.join(DECOMP, "world", "level", "block", "Blocks.java")
ID_HOLDERS = [
    os.path.join(DECOMP, "references", "BlockIds.java"),
    os.path.join(DECOMP, "references", "BlockItemIds.java"),
]
STATE_TABLE = os.path.join(ROOT, "src", "world", "block_states.h")
OUT = os.path.join(ROOT, "src", "world", "collision_table.h")
RESISTANCE_OUT = os.path.join(ROOT, "src", "world", "blast_resistance_table.h")
OCCLUSION_OUT = os.path.join(ROOT, "src", "world", "occlusion_table.h")

# `DyeColor` in `ColorCollection`'s own field order (white .. black), which is
# the order the colored id collections are built in.
DYE_COLORS = ["white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
              "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"]
# `WeatheringCopperCollection.PREFIXES`: the weathering states, then the waxed
# copies of them.
WEATHERING_PREFIXES = ["", "exposed_", "weathered_", "oxidized_"]
WAXED_PREFIXES = ["waxed_", "waxed_exposed_", "waxed_weathered_", "waxed_oxidized_"]

# `BlockStateBase.blocksMotion()` excludes these outright, whatever their
# properties say.
FORCED_NON_BLOCKING = {"minecraft:cobweb", "minecraft:bamboo_sapling"}

NAME_CONSTANT_RE = re.compile(r'public static final [\w<>]+ (\w+) = \w+\.create\("([a-z0-9_/]+)"')
REGISTRATION_RE = re.compile(r"public static final Block (\w+) = (Blocks\.\w+)\((\s*.*?)\);\s*$")
HELPER_RE = re.compile(
    r"private static BlockBehaviour\.Properties (\w+)\([^)]*\)\s*\{\s*return (.*?);\s*\}")


def split_args(args):
    """Split a Java argument list on its top-level commas.

    Only round braces are tracked: a lambda's `->` contains a `>`, so angle
    brackets cannot be counted, and no registration's *arguments* carry a
    generic with a top-level comma.
    """
    out, depth, start = [], 0, 0
    for i, ch in enumerate(args):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "," and depth == 0:
            out.append(args[start:i].strip())
            start = i + 1
    out.append(args[start:].strip())
    return [a for a in out if a]


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def load_names():
    """Constant -> block name, from `BlockItemId.create(...)` calls."""
    names = {}
    for path in ID_HOLDERS:
        for const, name in NAME_CONSTANT_RE.findall(read(path)):
            names[const] = "minecraft:" + name
    return names


def load_collections():
    """Constant -> the block names its collection registers.

    `ColorCollection.createSimpleColored(base)` is one per dye colour;
    `WeatheringCopperCollection.createSimpleCopper(base)` is the four weathering
    states plus their four waxed copies; the one hand-written copper family
    (`COPPER_BLOCK`) names its states through a `ByState` literal, because its
    weathering names are not a simple prefix of one base name
    (`copper_block` / `exposed_copper` / ...).
    """
    collections = {}
    for path in ID_HOLDERS:
        source = read(path)
        for base in re.findall(
                r'ColorCollection<[^;]*?> (\w+) = \w+\.createSimpleColored\("(\w+)"\)', source):
            collections[base[0]] = ["%s_%s" % (color, base[1]) for color in DYE_COLORS]
        for base in re.findall(
                r'WeatheringCopperCollection<[^;]*?> (\w+) = \w+\.createSimpleCopper\("(\w+)"\)',
                source):
            collections[base[0]] = ([prefix + base[1] for prefix in WEATHERING_PREFIXES] +
                                    [prefix + base[1] for prefix in WAXED_PREFIXES])
        # `prefixWithState(same(ByState("copper_block", "copper", "copper", "copper")))`.
        states = {}
        for const, *state_names in re.findall(
                r'ByState<String> (\w+) = new WeatheringCopperCollection\.ByState<String>'
                r'\("([\w]+)", "([\w]+)", "([\w]+)", "([\w]+)"\)', source):
            states[const] = state_names
        for const, state in re.findall(
                r'WeatheringCopperCollection<[^;]*?> (\w+) = WeatheringCopperCollection\.prefixWithState'
                r'\(WeatheringCopperCollection\.same\((\w+)\)\)', source):
            if state not in states:
                continue
            names = states[state]
            collections[const] = ([prefix + name for prefix, name in zip(WEATHERING_PREFIXES, names)] +
                                  [prefix + name for prefix, name in zip(WAXED_PREFIXES, names)])
    return collections


def load_helper_bodies(source):
    """Property factory name -> its single-`return` body."""
    return {name: body for name, body in HELPER_RE.findall(source)}


def inline_helpers(expr, helpers, depth=0):
    """Replace `Blocks.<helper>(...)` calls with the helper's own body.

    The helpers are only ever called as `Blocks.name(...)`, and their arguments
    never nest more than one level deep (`Blocks.candleProperties(color ==
    DyeColor.WHITE ? MapColor.WOOL : color.getMapColor())`), so one level of
    parens in the call pattern is enough.
    """
    if depth > 8:
        return expr
    changed = False
    for name, body in helpers.items():
        if "Blocks.%s(" % name in expr:
            expr = re.sub(r"Blocks\.%s\([^()]*(?:\([^()]*\)[^()]*)*\)" % re.escape(name), body,
                          expr)
            changed = True
    return inline_helpers(expr, helpers, depth + 1) if changed else expr


def flags(expr, helpers, raw, depth=0):
    """(forceSolidOn, forceSolidOff, noCollision, noOcclusion) for a property
    expression.

    Property factories are inlined and a `ofLegacyCopy`/`ofFullCopy` base is
    followed into its own registration, because the copy inherits all four
    (`Properties.ofLegacyCopy` copies `hasCollision`, `forceSolidOn`,
    `forceSolidOff` and `canOcclude` — and `noOcclusion()` is what clears the
    last of those).

    `noOcclusion` is not what `blocksMotion()` reads; it is what
    `isSolidRender()` reads, the other half of the light engine's dampening
    (`light.h`): glass and ice are full cubes that let light through.
    """
    resolved = inline_helpers(expr, helpers)
    solid_on = "forceSolidOn()" in resolved
    solid_off = "forceSolidOff()" in resolved
    no_collision = "noCollision()" in resolved
    no_occlusion = "noOcclusion()" in resolved
    base = re.match(r"(?:BlockBehaviour\.)?Properties\.of(?:Legacy|Full)Copy\(([\w.]+)\)", resolved)
    base_const = base.group(1).split(".")[-1] if base else None
    if base_const and base_const in raw and depth < 8 and base_const != expr:
        base_on, base_off, base_nc, base_no = flags(raw[base_const], helpers, raw, depth + 1)
        solid_on = solid_on or base_on
        solid_off = solid_off or base_off
        no_collision = no_collision or base_nc
        no_occlusion = no_occlusion or base_no
    return solid_on, solid_off, no_collision, no_occlusion


STRENGTH_RE = re.compile(r"strength\(([-0-9.eEfF]+)(?:,\s*([-0-9.eEfF]+))?\)")
RESISTANCE_RE = re.compile(r"explosionResistance\(([-0-9.eEfF]+)\)")


def blast_resistance(expr, helpers, raw, depth=0):
    """`BlockBehaviour.getExplosionResistance` for a property expression.

    `Properties.of()` starts at 0; `strength(destroyTime, resistance)` sets both
    and `strength(t)` is `strength(t, t)`; `explosionResistance(x)` sets the one.
    A later call in the chain wins, so the last match is the answer. A
    `ofLegacyCopy`/`ofFullCopy` base that the expression does not override
    carries its own resistance over (the copy covers that field).
    """
    resolved = inline_helpers(expr, helpers)
    matches = list(STRENGTH_RE.finditer(resolved)) + list(RESISTANCE_RE.finditer(resolved))
    if matches:
        last = max(matches, key=lambda m: m.start())
        # `strength(destroyTime, resistance)` carries the resistance second;
        # the one-argument form is `strength(t, t)` and `explosionResistance(x)`
        # has only the one number.
        value = last.group(2) if last.re is STRENGTH_RE and last.group(2) is not None else last.group(1)
        return float(value.rstrip("fF"))
    base = re.match(r"(?:BlockBehaviour\.)?Properties\.of(?:Legacy|Full)Copy\(([\w.]+)\)", resolved)
    base_const = base.group(1).split(".")[-1] if base else None
    if base_const and base_const in raw and depth < 8 and base_const != expr:
        return blast_resistance(raw[base_const], helpers, raw, depth + 1)
    return 0.0


def blocks_motion(solid_on, solid_off, no_collision):
    """`BlockStateBase.calculateSolid` for the two property terms."""
    if solid_on:
        return True
    if solid_off:
        return False
    if no_collision:
        return False
    return True  # the collision shape term, assumed large (see the header)


def main():
    names = load_names()
    collections = load_collections()
    source = read(BLOCKS_JAVA)
    helpers = load_helper_bodies(source)

    # ---- the plain registrations --------------------------------------------
    raw = {}        # block constant -> property expression
    ids_of = {}     # block constant -> the id constant its registration names it by
    unparsed = []   # registration lines this script could not read
    for line in source.splitlines():
        m = REGISTRATION_RE.search(line.strip())
        if not m:
            continue
        const, call, args = m.group(1), m.group(2), split_args(m.group(3))
        if call == "Blocks.register":
            if len(args) == 2:          # register(id, properties)
                raw[const], ids_of[const] = args[1], args[0]
            elif len(args) == 3:        # register(id, factory, properties)
                raw[const], ids_of[const] = args[2], args[0]
            else:
                unparsed.append((const, call, args))
        elif call in ("Blocks.registerLegacyStair", "Blocks.registerStair", "Blocks.registerSlab",
                      "Blocks.registerWall", "Blocks.registerLegacyWall") and len(args) == 2:
            # `registerSlab(id, base)`: the id, then the block the properties copy.
            raw[const] = "Properties.ofLegacyCopy(%s)" % args[1].split(".")[-1]
            ids_of[const] = args[0]
        else:
            unparsed.append((const, call, args))

    # ---- the collection registrations ---------------------------------------
    # One registration per name, all with the collection's own properties: the
    # last argument of `ColorCollection.registerBlocks` (and of the weathering
    # one, which puts its extra factory before it).
    collection_names = {}   # name -> (constant, expression)
    for call in re.findall(r"public static final (?:ColorCollection|WeatheringCopperCollection)<Block>"
                           r" (\w+) = (?:ColorCollection|WeatheringCopperCollection)\.registerBlocks"
                           r"\(([^;]*)\);", source, re.DOTALL):
        const, args = call[0], split_args(call[1])
        ids, props = args[0].split(".")[-1], args[-1]
        if ids not in collections:
            unparsed.append((const, "collection", args))
            continue
        for block_name in collections[ids]:
            collection_names["minecraft:" + block_name] = (const, props)

    if unparsed:
        print("UNPARSED registrations (%d):" % len(unparsed), file=sys.stderr)
        for entry in unparsed[:10]:
            print("   ", entry, file=sys.stderr)
        sys.exit(1)

    # ---- classify ------------------------------------------------------------
    classified = {}    # name -> bool (blocksMotion)
    resistance = {}    # name -> float (blast resistance)
    occludes = {}      # name -> bool (canOcclude: light and isSolidRender)
    for const, props in raw.items():
        # The block's own name comes from its registration's id argument, not
        # from the constant: `REDSTONE_WIRE = register(BlockItemIds.REDSTONE_DUST,
        # ...)` is `minecraft:redstone_wire`, and `WHEAT = register(
        # BlockItemIds.WHEAT_CROP, ...)` is `minecraft:wheat`.
        id_const = ids_of[const].split(".")[-1]
        if id_const not in names:
            print("NO ID for %s (%s)" % (const, ids_of[const]), file=sys.stderr)
            sys.exit(1)
        solid_on, solid_off, no_collision, no_occlusion = flags(props, helpers, raw)
        classified[names[id_const]] = blocks_motion(solid_on, solid_off, no_collision)
        occludes[names[id_const]] = not no_occlusion
        resistance[names[id_const]] = blast_resistance(props, helpers, raw)
    for block_name, (const, props) in collection_names.items():
        solid_on, solid_off, no_collision, no_occlusion = flags(props, helpers, raw)
        if not blocks_motion(solid_on, solid_off, no_collision):
            print("COLLECTION %s (%s) is non-blocking: its names must be expanded here, "
                  "not assumed" % (const, block_name), file=sys.stderr)
            sys.exit(1)
        classified[block_name] = True
        occludes[block_name] = not no_occlusion
        resistance[block_name] = blast_resistance(props, helpers, raw)
    for name in FORCED_NON_BLOCKING:
        classified[name] = False

    # ---- cross-check against the project's own registry ----------------------
    registry_names = set(re.findall(r'\{"([a-z0-9_]+:[a-z0-9_/]+)",', read(STATE_TABLE)))
    missed = sorted(registry_names - set(classified))
    bogus = sorted(set(classified) - registry_names)
    if bogus:
        print("NOT IN THE PROJECT REGISTRY (%d): %s" % (len(bogus), bogus[:20]), file=sys.stderr)
        sys.exit(1)
    if missed:
        print("BLOCKS NOT CLASSIFIED (%d): %s" % (len(missed), missed[:20]), file=sys.stderr)
        sys.exit(1)

    non_motion = sorted(name for name, moves in classified.items() if not moves)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("""// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// The blocks whose `BlockState.blocksMotion()` is FALSE (%d blocks, generated —
// do not hand-edit): the vanilla predicate `Heightmap.Types.MOTION_BLOCKING` is
// built from, and therefore the one that decides where a column's rain reaches.
//
// `BlockBehaviour.BlockStateBase.blocksMotion()` is
//
//     block != cobweb && block != bamboo_sapling
//       && (forceSolidOn ? true
//           : forceSolidOff ? false
//           : <the collision shape covers 0.729 of the block's bounds>)
//
// The two flags are properties, so they are what this table carries — through
// the copy chains of `ofLegacyCopy`/`ofFullCopy`, the colored and weathering
// collections, and the property factories in `Blocks`. The shape term is code,
// not data, so a block that sets neither flag is assumed to collide fully. The
// blocks that assumption can get wrong are the ones with a small collision
// shape and no `forceSolidOff` — a carpet, a candle, a pressure plate, a lily
// pad — where vanilla would let rain through and this table does not.
//
// Everything the weather needs is exact: fire, souls fire, torches, flowers,
// grass and the rest of the no-collision shapes are listed here, and they are
// what a rained-out fire or a filling cauldron stands on.
//
// Regenerate with tools/gen_collision_table.py.
#pragma once

#include <cstdint>

namespace mc::world {

// Sorted by name, so block_blocks_motion (world/chunk.h) can binary-search it.
inline constexpr const char* kNonMotionBlocking[] = {
""" % len(non_motion))
        for name in non_motion:
            f.write('    "%s",\n' % name)
        f.write("};\n")
        f.write("inline constexpr int kNonMotionBlockingCount = %d;\n" % len(non_motion))
        f.write("\n} // namespace mc::world\n")

    print("wrote %s: %d of %d blocks are non-blocking" %
          (os.path.relpath(OUT, ROOT), len(non_motion), len(classified)))

    # ---- the blast resistance table -----------------------------------------
    # Every block that resists an explosion at all, sorted by name (absence
    # means 0, which is what a fresh `Properties.of()` carries and what most
    # blocks — dirt, wood, glass — keep). `ServerExplosion` reads it per block
    # the ray marches through.
    resisting = sorted((name, value) for name, value in resistance.items() if value != 0.0)
    with open(RESISTANCE_OUT, "w", encoding="utf-8") as f:
        f.write("""// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// The blast resistance of every block that has one (%d of %d, generated — do
// not hand-edit): `BlockBehaviour.getExplosionResistance` as 26.2 registers it,
// which is the second argument of `Blocks`' `strength(destroyTime,
// explosionResistance)` (or the single-argument `strength(t)`, which is
// `strength(t, t)`). A block that is not listed resists nothing.
//
// `ServerExplosion.calculateExplodedPositions` subtracts
// `(resistance + 0.3) * 0.3` from a ray's remaining power for every cell it
// enters, so this table is what keeps a blast out of obsidian (1200) while
// going straight through dirt (0).
//
// Regenerate with tools/gen_collision_table.py.
#pragma once

namespace mc::world {

struct BlockBlastResistance {
  const char* name;
  float resistance;
};

// Sorted by name, so block_blast_resistance (world/tnt.h) can binary-search it.
inline constexpr BlockBlastResistance kBlockBlastResistance[] = {
""" % (len(resisting), len(classified)))
        for name, value in resisting:
            f.write('    {"%s", %sf},\n' % (name, repr(value)))
        f.write("};\n")
        f.write("inline constexpr int kBlockBlastResistanceCount = %d;\n" % len(resisting))
        f.write("\n} // namespace mc::world\n")

    print("wrote %s: %d of %d blocks resist" %
          (os.path.relpath(RESISTANCE_OUT, ROOT), len(resisting), len(classified)))

    # ---- the occlusion table -------------------------------------------------
    # The blocks registered `noOcclusion()` — `canOcclude` false, which is one
    # half of `isSolidRender()` (`= isShapeFullBlock(occlusionShape)`, and the
    # occlusion shape is empty when a block does not occlude). The light engine
    # reads it together with `blocksMotion()`: a full cube that does not occlude
    # (glass, ice, a shulker box without its lid) passes light where a full cube
    # that does (stone) stops it. Sorted by name, so `block_no_occlusion`
    # (world/light.cpp) can binary-search it.
    transparent = sorted(name for name, occludes_it in occludes.items() if not occludes_it)
    with open(OCCLUSION_OUT, "w", encoding="utf-8") as f:
        f.write("""// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// The blocks whose `BlockState.canOcclude()` is FALSE (%d of %d, generated — do
// not hand-edit): the ones registered `Properties...noOcclusion()`, through the
// copy chains (`ofLegacyCopy` carries `canOcclude`), the colored and weathering
// collections, and the property factories.
//
// `isSolidRender()` is `isShapeFullBlock(occlusionShape)`, and the occlusion
// shape is empty when a block cannot occlude — so this list is one half of it.
// The light engine's other half is the generated `blocksMotion()` table
// (collision_table.h): `light_is_opaque` is "blocks motion and does occlude".
//
// Regenerate with tools/gen_collision_table.py.
#pragma once

namespace mc::world {

// Sorted by name.
inline constexpr const char* kNoOcclusion[] = {
""" % (len(transparent), len(classified)))
        for name in transparent:
            f.write('    "%s",\n' % name)
        f.write("};\n")
        f.write("inline constexpr int kNoOcclusionCount = %d;\n" % len(transparent))
        f.write("\n} // namespace mc::world\n")

    print("wrote %s: %d of %d blocks do not occlude" %
          (os.path.relpath(OCCLUSION_OUT, ROOT), len(transparent), len(classified)))


if __name__ == "__main__":
    main()
