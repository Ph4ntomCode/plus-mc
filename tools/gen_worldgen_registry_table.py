#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the worldgen feature/carver/structure registry headers.

The generator's noise, biome and surface stages reproduce a vanilla dump on
their own; the stages *after* them are registry-driven in vanilla. This tool is
the reading half of those registries: it walks the 26.2 data pack's own
`worldgen/**/*.json` and the structure templates under `structure/`, and emits
five headers —

  * `worldgen_feature_table.h` — the 226 configured features plus the 16 inline
    definitions hoisted out of the selectors (their `type` and, for the kinds the
    stage applies, the parsed config), the 262 placed features with their
    flattened placement modifiers, and every biome's own decoration lists;
  * `worldgen_carver_table.h` — the configured carvers, and each biome's own
    `carvers` list;
  * `worldgen_structure_table.h` — the structures, and the structure sets with
    their weighted members;
  * `worldgen_fossil_table.h` — the two fossil features' configs and the 16
    `structure/fossil/*.nbt` templates they place, resolved to block lists;
  * `worldgen_structure_piece_table.h` — the 20 `structure/shipwreck/*.nbt`
    templates with their material palettes and `ShipwreckPieces`' two template
    lists, plus the three `structure/igloo/*.nbt` templates with their own
    pivots and `IglooPieces` order, the village templates and pools, and the
    eleven `structure/pillager_outpost/*.nbt` templates and its three pools.
    Each `structure_block` data marker's `metadata` is carried too, so a chest
    can be given the loot table vanilla names for it.
  * `worldgen_loot_table.h` — every structure container table (`chests`,
    `dispensers`, `archaeology`, `pots`, `spawners`), flattened into pools,
    weighted entries and the functions whose item components this engine
    models.

Every id is a bare `"minecraft:..."` literal and every table is sorted by name,
so the runtime lookups are binary searches and a generated header is diffable.

Usage:
    gen_worldgen_registry_table.py [--pack DIR] [--feature-header FILE]
        [--carver-header FILE] [--structure-header FILE] [--fossil-header FILE]
        [--piece-header FILE] [--loot-header FILE]
"""

import argparse
import gzip
import json
import pathlib
import struct
import sys

_PACK = "data/vanilla/26.2/data/minecraft/worldgen"
_U32_MAX = 4294967295


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def ids_of(base, suffix=".json"):
    """(id, path) for every JSON under a registry directory, sorted by id."""
    out = []
    for path in base.rglob(f"*{suffix}"):
        rel = path.relative_to(base)
        out.append(("minecraft:" + "/".join(rel.with_suffix("").parts), path))
    out.sort()
    return out


def cstr(text):
    return '"' + str(text).replace("\\", "\\\\").replace('"', '\\"') + '"'


def flit(value):
    """A C++ float literal (Python's `g` format omits the decimal point)."""
    text = f"{float(value):.6g}"
    if not any(c in text for c in ".eE"):
        text += ".0"
    return text + "f"


def state_of(spec):
    """A block spec (`{"Name": ..., "Properties": {...}}`) as a WgState row."""
    name = spec.get("Name", "minecraft:air")
    props = spec.get("Properties") or {}
    text = ",".join(f"{k}={props[k]}" for k in sorted(props))
    return (name, text)


def parse_anchor(value):
    """A `VerticalAnchor` as (kind, value): 0 absolute, 1 above_bottom, 2 below_top."""
    if "absolute" in value:
        return (0, int(value["absolute"]))
    if "above_bottom" in value:
        return (1, int(value["above_bottom"]))
    if "below_top" in value:
        return (2, int(value["below_top"]))
    return (0, 0)

#-- - block tags, resolved for `tag_match` predicates -- -- -- -- -- -- -- -- -- -- -- --

class EnchantmentTags:
    """The pack's enchantment tags, resolved transitively to enchantment names."""

    def __init__(self, minecraft_dir):
        self._values = {}
        base = minecraft_dir / "tags" / "enchantment"
        for name, path in ids_of(base):
            self._values[name] = load(path).get("values", [])

    def resolve(self, tag):
        if tag not in self._values:
            raise ValueError(f"unknown enchantment tag #{tag}")
        seen = set()
        out = []

        def walk(entry):
            if isinstance(entry, str) and entry.startswith("#"):
                nested = entry[1:]
                if nested in seen:
                    return
                seen.add(nested)
                if nested not in self._values:
                    raise ValueError(f"unknown enchantment tag {nested}")
                for value in self._values[nested]:
                    walk(value)
            elif isinstance(entry, str):
                if entry not in out:
                    out.append(entry)
            elif isinstance(entry, dict):
                if entry.get("required", True) is False:
                    return
                value = entry.get("id")
                if not isinstance(value, str):
                    raise ValueError(f"unrecognized enchantment tag entry {entry!r}")
                walk(value)
            else:
                raise ValueError(f"unrecognized enchantment tag entry {entry!r}")

        for value in self._values[tag]:
            walk(value)
        return out


class BlockTags:
    """The pack's block tags, resolved transitively to concrete block names."""

    def __init__(self, minecraft_dir):
        self._values = {}
        base = minecraft_dir / "tags" / "block"
        for name, path in ids_of(base):
            self._values[name] = load(path).get("values", [])

    def resolve(self, tag):
        seen = set()
        out = []

        def walk(entry):
            if isinstance(entry, str):
                # A tag entry is written `#minecraft:name`; a plain entry is a
                # concrete block. This matters for `overworld_carver_replaceables`,
                # whose whole content is nested tags.
                if entry.startswith("#"):
                    walk_tag(entry[1:])
                else:
                    out.append(entry)
                return
            name = entry.get("id")
            if name is None:
                return
            if entry.get("required") is False and name.lstrip("#") not in self._values:
                return
            if name.startswith("#"):
                walk_tag(name[1:])
            else:
                out.append(name)

        def walk_tag(name):
            if name in seen:
                return
            seen.add(name)
            for entry in self._values.get(name, []):
                walk(entry)

        walk_tag(tag)
        return sorted(set(out))


class BiomeTags:
    """The pack's biome tags, resolved transitively to concrete biome names.

    A structure's `biomes` and a concentric-ring placement's `preferred_biomes`
    are `HolderSet<Biome>`s; the pack writes both as tags, and the tags nest
    (`#minecraft:has_structure/stronghold` is `#minecraft:is_overworld`). The
    runtime compares biome names, so the generator resolves the reference here
    and refuses a tag the pack does not define rather than emitting an empty
    set (which would silently make every start fail its biome test).
    """

    def __init__(self, minecraft_dir):
        self._values = {}
        base = minecraft_dir / "tags" / "worldgen" / "biome"
        for name, path in ids_of(base):
            self._values[name] = load(path).get("values", [])

    def resolve(self, tag):
        if tag not in self._values:
            raise ValueError(f"unknown biome tag #{tag}")
        seen = set()
        out = []

        def walk(entry):
            if isinstance(entry, str):
                if entry.startswith("#"):
                    walk_tag(entry[1:])
                else:
                    out.append(entry)
                return
            name = entry.get("id")
            if name is None:
                return
            if entry.get("required") is False and name.lstrip("#") not in self._values:
                return
            if name.startswith("#"):
                walk_tag(name[1:])
            else:
                out.append(name)

        def walk_tag(name):
            if name in seen:
                return
            if name not in self._values:
                raise ValueError(f"unknown biome tag #{name} (nested)")
            seen.add(name)
            for entry in self._values[name]:
                walk(entry)

        walk_tag(tag)
        return sorted(set(out))


def biome_set(value, tags, where):
    """A `HolderSet<Biome>` reference as (provenance, sorted concrete names)."""
    if isinstance(value, str):
        if value.startswith("#"):
            names = tags.resolve(value[1:])
            if not names:
                raise ValueError(f"{where}: biome tag {value} resolves to no biomes")
            return value, names
        return value, [value]
    if isinstance(value, list):
        names = []
        for entry in value:
            if not isinstance(entry, str):
                raise ValueError(f"{where}: biome entry {entry!r} is not a name")
            if entry.startswith("#"):
                names.extend(tags.resolve(entry[1:]))
            else:
                names.append(entry)
        if not names:
            raise ValueError(f"{where}: biome list resolves to no biomes")
        return "", sorted(set(names))
    raise ValueError(f"{where}: biomes is neither a tag nor a list: {value!r}")


def property_suffix(props):
    if not props:
        return ""
    return ",".join(f"{k}={props[k]}" for k in sorted(props))


def state_text(name, props):
    suffix = property_suffix(props)
    return name + (f"[{suffix}]" if suffix else "")


def _float_literal(value):
    # C++ needs `1.0f`, not `1f`; `:.9g` drops the fraction on whole floats.
    text = f"{float(value):.9g}"
    if "." not in text and "e" not in text and "E" not in text:
        text += ".0"
    return text + "f"


def name_list(value):
    """A `blocks`/`fluids` list serialised as a `|` list. The pack lets the
    field be a bare string for a single entry, which is *not* a list of
    characters."""
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    return "|".join(str(entry) for entry in value)


def feature_ref(ref):
    """A selector child's feature reference. Inline placeholder objects
    (`{
  "feature" : "minecraft:x", "placement" : []}`) unwrap to the id; an inline
    configured-feature definition has no registry id and returns None."""
    if isinstance(ref, str):
        return ref
    if isinstance(ref, dict):
        inner = ref.get("feature")
        if isinstance(inner, str):
            return inner
    return None

# --- configured features ----------------------------------------------------

_KIND_BY_TYPE = {
    "minecraft:ore": "kOre",
    "minecraft:simple_block": "kSimpleBlock",
    "minecraft:block_pile": "kBlockPile",
    "minecraft:disk": "kDisk",
    "minecraft:spring_feature": "kSpring",
    "minecraft:random_selector": "kRandomSelector",
    "minecraft:simple_random_selector": "kSimpleRandomSelector",
    "minecraft:random_boolean_selector": "kRandomBooleanSelector",
    "minecraft:tree": "kTree",
    "minecraft:block_column": "kBlockColumn",
    "minecraft:scattered_ore": "kScatteredOre",
    "minecraft:monster_room": "kMonsterRoom",
    "minecraft:freeze_top_layer": "kFreezeTopLayer",
    "minecraft:seagrass": "kSeagrass",
    "minecraft:fallen_tree": "kFallenTree",
    "minecraft:vines": "kVines",
    "minecraft:multiface_growth": "kMultifaceGrowth",
    # The remaining kinds, each with a flat `extra_config_of` row.
    "minecraft:block_blob": "kBlockBlob",
    "minecraft:desert_well": "kDesertWell",
    "minecraft:glowstone_blob": "kGlowstoneBlob",
    "minecraft:blue_ice": "kBlueIce",
    "minecraft:bonus_chest": "kBonusChest",
    "minecraft:end_platform": "kEndPlatform",
    "minecraft:end_island": "kEndIsland",
    "minecraft:end_gateway": "kEndGateway",
    "minecraft:end_spike": "kEndSpike",
    "minecraft:chorus_plant": "kChorusPlant",
    "minecraft:kelp": "kKelp",
    "minecraft:sea_pickle": "kSeaPickle",
    "minecraft:basalt_pillar": "kBasaltPillar",
    "minecraft:basalt_columns": "kBasaltColumns",
    "minecraft:netherrack_replace_blobs": "kReplaceBlobs",
    "minecraft:delta_feature": "kDelta",
    "minecraft:nether_forest_vegetation": "kNetherForestVegetation",
    "minecraft:twisting_vines": "kTwistingVines",
    "minecraft:weeping_vines": "kWeepingVines",
    "minecraft:bamboo": "kBamboo",
    "minecraft:iceberg": "kIceberg",
    "minecraft:lake": "kLake",
    "minecraft:vegetation_patch": "kVegetationPatch",
    "minecraft:waterlogged_vegetation_patch": "kVegetationPatch",
    "minecraft:sculk_patch": "kSculkPatch",
    "minecraft:root_system": "kRootSystem",
    "minecraft:underwater_magma": "kUnderwaterMagma",
    "minecraft:speleothem": "kSpeleothem",
    "minecraft:speleothem_cluster": "kSpeleothemCluster",
    "minecraft:large_dripstone": "kLargeDripstone",
    "minecraft:huge_brown_mushroom": "kHugeMushroom",
    "minecraft:huge_red_mushroom": "kHugeMushroom",
    "minecraft:huge_fungus": "kHugeFungus",
    "minecraft:coral_tree": "kCoral",
    "minecraft:coral_claw": "kCoral",
    "minecraft:coral_mushroom": "kCoral",
    "minecraft:sequence": "kSequence",
    "minecraft:weighted_random_selector": "kWeightedSelector",
    "minecraft:spike": "kSpike",
    "minecraft:geode": "kGeode",
    "minecraft:void_start_platform": "kVoidStartPlatform",
    "minecraft:fossil": "kFossil",
}

# The kinds whose configs share the flat `extra_config_of` row. `fossil` is
# among them, but its row is empty: the template refs and processor integrities
# live in `worldgen_fossil_table.h`, which this tool emits from the `.nbt`
# templates themselves. Every other configured feature kind in the 26.2 pack is
# mapped above and applied by the runtime.
_EXTRA_KINDS = frozenset([
    "kBlockBlob", "kDesertWell", "kGlowstoneBlob", "kBlueIce", "kBonusChest",
    "kEndPlatform", "kEndIsland", "kEndGateway", "kEndSpike", "kChorusPlant",
    "kKelp", "kSeaPickle", "kBasaltPillar", "kBasaltColumns", "kReplaceBlobs",
    "kDelta", "kNetherForestVegetation", "kTwistingVines", "kWeepingVines",
    "kBamboo", "kIceberg", "kLake", "kVegetationPatch", "kSculkPatch",
    "kRootSystem", "kUnderwaterMagma", "kSpeleothem", "kSpeleothemCluster",
    "kLargeDripstone", "kHugeMushroom", "kHugeFungus", "kCoral", "kSequence",
    "kWeightedSelector", "kVoidStartPlatform", "kSpike", "kGeode",
])

# `Direction`'s own ordinal order (down, up, north, south, west, east), which is
# what a block column's `direction` resolves to.
_DIRECTION_KIND = {
    "down": 0,
    "up": 1,
    "north": 2,
    "south": 3,
    "west": 4,
    "east": 5,
}

# The `IntProvider` kinds the runtime samples. A provider outside this set (a
# trapezoid, a clamped one, ...) makes the whole config unparsed, and the
# runtime counts that as a kind it cannot apply rather than guessing.
_INT_PROVIDER_KIND = {
    "minecraft:constant": "kConstant",
    "minecraft:uniform": "kUniform",
    "minecraft:biased_to_bottom": "kBiasedToBottom",
    "minecraft:weighted_list": "kWeightedList",
}

# Trunk placers the runtime models. Anything else folds to straight (the height
# formula is shared), which the header names.
_TRUNK_KIND = {
    "minecraft:straight_trunk_placer": 0,
    "minecraft:fancy_trunk_placer": 1,
    "minecraft:dark_oak_trunk_placer": 2,
    "minecraft:mega_jungle_trunk_placer": 3,
    "minecraft:giant_trunk_placer": 4,
}

# Foliage placers the runtime models. Anything else folds to a blob.
_FOLIAGE_KIND = {
    "minecraft:blob_foliage_placer": 0,
    "minecraft:fancy_foliage_placer": 1,
    "minecraft:pine_foliage_placer": 2,
    "minecraft:spruce_foliage_placer": 3,
    "minecraft:acacia_foliage_placer": 4,
    "minecraft:dark_oak_foliage_placer": 5,
    "minecraft:jungle_foliage_placer": 6,
    "minecraft:cherry_foliage_placer": 7,
    "minecraft:bush_foliage_placer": 8,
    "minecraft:mega_pine_foliage_placer": 9,
    "minecraft:random_spread_foliage_placer": 0,
}


def _as_int(value, default=0):
    """An int or an `IntProvider` reduced to its upper bound."""
    if isinstance(value, bool):
        return default
    if isinstance(value, int):
        return value
    if isinstance(value, dict):
        for key in ("max_inclusive", "max", "value"):
            if isinstance(value.get(key), int):
                return value[key]
    return default


def tree_config_of(config):
    """A `tree` config's trunk/foliage providers and placers, flattened. The
    runtime's tree placer reads exactly these fields."""
    trunk = _simple_provider_state(config.get("trunk_provider"))
    foliage = _simple_provider_state(config.get("foliage_provider"))
    tp = config.get("trunk_placer", {}) or {
    }
    fp = config.get("foliage_placer", {}) or {}
    return {
      "trunk" : trunk,
      "foliage" : foliage,
      "trunk_kind" : _TRUNK_KIND.get(tp.get("type"), 0),
      "base_height" : _as_int(tp.get("base_height", tp.get("height", 4)), 4),
      "height_rand_a" : _as_int(tp.get("height_rand_a"), 0),
      "height_rand_b" : _as_int(tp.get("height_rand_b"), 0),
      "foliage_kind" : _FOLIAGE_KIND.get(fp.get("type"), 0),
      "radius" : _as_int(fp.get("radius"), 2),
      "foliage_height" : _as_int(fp.get("height"), 3),
      "offset" : _as_int(fp.get("offset"), 0),
    }

def _child_ref(ref, add_inline=None):
    """A selector child's `feature`: a placed-feature id, a configured-feature
    id, an inline `{"feature": <configured>, "placement": [...]}`, or an inline
    configured-feature definition. Returns (name, is_placed) or None when the
    reference cannot be named."""
    if isinstance(ref, dict):
        inner = ref.get("feature")
        if isinstance(inner, str):
            return (inner, False)
        if "type" in ref and add_inline is not None:
            name = add_inline(ref)
            if name is not None:
                return (name, False)
        return None
    if isinstance(ref, str):
        return (ref, True)  # resolved against the two registries by the caller
    return None


def selector_config_of(config, kind_code, add_inline=None):
    """A selector config -> (kind, children, default). Children are
    (feature, chance, is_placed); the boolean selector's two children share
    `chance` 0.5 and its kind code is 2 (0 random_selector,
    1 simple_random_selector, 2 random_boolean_selector)."""
    if kind_code == 2:
        true_ref = _child_ref(config.get("feature_true"), add_inline)
        false_ref = _child_ref(config.get("feature_false"), add_inline)
        children = []
        for ref in (true_ref, false_ref):
            if ref is not None:
                children.append((ref[0], 0.5, ref[1]))
        return (2, children, None)
    entries = config.get("features", []) or []
    children = []
    for entry in entries:
        if isinstance(entry, dict):
            ref = _child_ref(entry.get("feature"), add_inline)
            chance = float(entry["chance"]) if "chance" in entry else 1.0
        else:
            ref = _child_ref(entry, add_inline)
            chance = 1.0
        if ref is not None:
            children.append((ref[0], chance, ref[1]))
    default = _child_ref(config["default"], add_inline) if "default" in config else None
    return (kind_code, children, default)


def simple_state_of(config):
    """The state of a `simple_state_provider`, else None (an unparsed kind)."""
    provider = config.get("to_place") if isinstance(config, dict) else None
    if not isinstance(provider, dict):
        provider = config if isinstance(config, dict) else None
    if not isinstance(provider, dict) or provider.get("type") != "minecraft:simple_state_provider":
        return None
    return state_of(provider.get("state", {}))


def _simple_provider_state(provider):
    """The state a `BlockStateProvider` names, when it is a plain simple one
    (a rule-based provider is represented by its fallback)."""
    if not isinstance(provider, dict):
        return None
    if provider.get("type") == "minecraft:rule_based_state_provider":
        return _simple_provider_state(provider.get("fallback"))
    if provider.get("type") != "minecraft:simple_state_provider":
        return None
    return state_of(provider.get("state", {}))


def block_pile_state_of(config):
    if not isinstance(config, dict):
        return None
    return _simple_provider_state(config.get("state_provider"))


def _block_predicate_names(pred, tags):
    """A `matching_block_tag`/`matching_blocks` predicate reduced to its `|`
    name list, or "" when the predicate is not a plain membership test."""
    if not isinstance(pred, dict):
        return ""
    ptype = pred.get("type")
    if ptype == "minecraft:matching_block_tag":
        return "|".join(sorted(tags.resolve(pred.get("tag", ""))))
    if ptype == "minecraft:matching_blocks":
        blocks = pred.get("blocks") or ([pred["block"]] if "block" in pred else [])
        return name_list(blocks)
    return ""


def multiface_config_of(config, tags):
    """A `multiface_growth` config: the growth block (whose default state is
    what placement starts from), the three face flags the valid-direction list
    is built from, the search range, the spreading chance, and the blocks the
    growth may sit against.

    `can_be_placed_on` is a `HolderSet<Block>`, so an entry may be a tag; the
    tag is resolved here and the result serialised as one `|` name list.
    """
    if not isinstance(config, dict):
        return None
    block = config.get("block")
    if not isinstance(block, str):
        return None
    entries = config.get("can_be_placed_on") or []
    if isinstance(entries, str):
        entries = [entries]
    names = []
    for entry in entries:
        if not isinstance(entry, str):
            continue
        if entry.startswith("#"):
            names.extend(tags.resolve(entry[1:]))
        else:
            names.append(entry)
    return {
        "state": (block, ""),
        "search_range": int(config.get("search_range", 10)),
        "can_place_on_floor": int(bool(config.get("can_place_on_floor", False))),
        "can_place_on_ceiling": int(bool(config.get("can_place_on_ceiling", False))),
        "can_place_on_wall": int(bool(config.get("can_place_on_wall", False))),
        "chance": float(config.get("chance_of_spreading", 0.5)),
        "can_be_placed_on": "|".join(sorted(set(names))),
    }


def disk_config_of(config, tags):
    if not isinstance(config, dict):
        return None
    state = _simple_provider_state(config.get("state_provider"))
    if state is None:
        return None
    radius = config.get("radius", {})
    if isinstance(radius, dict):
        radius_min = int(radius.get("min_inclusive", radius.get("value", 0)))
        radius_max = int(radius.get("max_inclusive", radius_min))
    elif isinstance(radius, int):
        radius_min = radius_max = radius
    else:
        radius_min = radius_max = 0
    half_height = int(config.get("half_height", 0))
    target = _block_predicate_names(config.get("target"), tags)
    return (state, radius_min, radius_max, half_height, target)


def spring_config_of(config, tags):
    """A `spring_feature` config: (state, valid-block name list, requires block
    below, rock count, hole count)."""
    state = state_of(config.get("state", {}))
    valid = config.get("valid_blocks", [])
    if isinstance(valid, str):
        names = sorted(tags.resolve(valid[1:])) if valid.startswith("#") else [valid]
    else:
        names = list(valid)
    return (state, "|".join(sorted(names)),
            int(bool(config.get("requires_block_below", True))),
            int(config.get("rock_count", 4)),
            int(config.get("hole_count", 1)))


def ore_config_of(config):
    targets = []
    for target in config.get("targets", []):
        predicate = target.get("target", {})
        ptype = predicate.get("predicate_type")
        if ptype == "minecraft:tag_match":
            ref = ""  # filled by the caller, which knows the tag resolver
        elif ptype == "minecraft:block_match":
            # A direct block predicate carries its block id, not a tag: without
            # this every `block_match` ore target (the Nether's netherrack ores
            # among them) compiled to an empty membership list and could never
            # replace anything.
            ref = predicate.get("block", "")
        else:
            ref = ""
        targets.append({
            "state": state_of(target.get("state", {})),
            "predicate": "kBlocks",
            "ref": ref,
            "tag": predicate.get("tag"),
        })
    return {
        "size": int(config.get("size", 0)),
        "discard": float(config.get("discard_chance_on_air_exposure", 0.0)),
        "targets": targets,
    }


class IntProviders:
    """A flat `IntProvider` table for a block column's layer heights: each row is
    (kind, lo, hi, offset, count), a weighted list's children are its own
    entries, and each entry names a provider index and a weight."""

    def __init__(self):
        self.rows = []
        self.entries = []

    def add(self, value):
        """An `IntProvider` (a bare int is a constant) as a row index, or None
        when the runtime has no sampler for it."""
        if isinstance(value, bool):
            return None
        if isinstance(value, int):
            kind, lo, hi = "kConstant", value, value
        elif isinstance(value, dict):
            kind = _INT_PROVIDER_KIND.get(value.get("type"))
            if kind is None:
                return None
            if kind == "kConstant":
                lo = hi = int(value.get("value", 0))
            elif kind == "kWeightedList":
                offset = len(self.entries)
                for entry in value.get("distribution", []):
                    child = self.add(entry.get("data"))
                    if child is None:
                        return None
                    self.entries.append((child, int(entry.get("weight", 1))))
                count = len(self.entries) - offset
                if count == 0:
                    return None
                self.rows.append((kind, 0, 0, offset, count))
                return len(self.rows) - 1
            else:
                lo = int(value.get("min_inclusive", 0))
                hi = int(value.get("max_inclusive", lo))
        else:
            return None
        self.rows.append((kind, lo, hi, 0, 0))
        return len(self.rows) - 1


class StateProviders:
    """A flat `BlockStateProvider` table for a block column's layers. A row is
    (kind, state, offset, count, source, values, property): a simple provider is
    its state, a weighted one an offset/count into the weighted state list, and
    a `randomized_int_state_provider` its source row, the IntProvider its int
    property is drawn from, and that property's name."""

    def __init__(self):
        self.rows = []
        self.entries = []

    def add(self, provider, ints):
        if not isinstance(provider, dict):
            return None
        ptype = provider.get("type")
        if ptype == "minecraft:simple_state_provider":
            self.rows.append(("kSimple", state_of(provider.get("state", {})),
                              0, 0, 0, 0, ""))
            return len(self.rows) - 1
        if ptype == "minecraft:weighted_state_provider":
            offset = len(self.entries)
            for entry in provider.get("entries", []):
                data = entry.get("data")
                if not isinstance(data, dict):
                    return None
                self.entries.append((state_of(data), int(entry.get("weight", 1))))
            count = len(self.entries) - offset
            if count == 0:
                return None
            self.rows.append(("kWeighted", ("minecraft:air", ""), offset, count,
                              0, 0, ""))
            return len(self.rows) - 1
        if ptype == "minecraft:randomized_int_state_provider":
            source = self.add(provider.get("source"), ints)
            values = ints.add(provider.get("values"))
            property_name = provider.get("property")
            if source is None or values is None or not isinstance(property_name, str):
                return None
            self.rows.append(("kRandomizedInt", ("minecraft:air", ""), 0, 0,
                              source, values, property_name))
            return len(self.rows) - 1
        return None


def column_config_of(config, providers, ints):
    """A `block_column` config -> (layers, direction, allowed placement,
    prioritize tip), or None when any part of it is not modellable. The allowed
    placement is still the pack's own predicate object; the caller turns it
    into a predicate-table index once that table has the placement modifiers."""
    if not isinstance(config, dict):
        return None
    direction = _DIRECTION_KIND.get(config.get("direction"))
    if direction is None:
        return None
    layers = []
    for layer in config.get("layers", []):
        if not isinstance(layer, dict):
            return None
        provider = providers.add(layer.get("provider"), ints)
        height = ints.add(layer.get("height"))
        if provider is None or height is None:
            return None
        layers.append((provider, height))
    if not layers:
        return None
    return {
        "layers": layers,
        "direction": direction,
        "allowed": config.get("allowed_placement", {}),
        "prioritize_tip": int(bool(config.get("prioritize_tip", False))),
    }


def seagrass_config_of(config):
    """`SeagrassFeature`'s config is a single probability (`ProbabilityFeatureConfiguration`)."""
    value = config.get("probability", 0.0)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return float(value)


def fallen_tree_config_of(config, providers, ints):
    """A `fallen_tree` config: the trunk and log-length providers, the stump
    decorator (`trunk_vine` at most once -- vanilla runs each list entry once,
    and the pack's own five configs use it zero or one time) and the log
    decorator (`attached_to_logs` at most once, with the single `up` direction
    all five use). Anything else -- a second decorator, another direction list,
    a provider the state-provider table cannot model -- returns None, so the
    whole config is counted as an unapplied kind rather than approximated."""
    if not isinstance(config, dict):
        return None
    trunk = providers.add(config.get("trunk_provider"), ints)
    length = ints.add(config.get("log_length"))
    if trunk is None or length is None:
        return None
    stump_vine = 0
    for decorator in config.get("stump_decorators", []) or []:
        if not isinstance(decorator, dict) or decorator.get("type") != "minecraft:trunk_vine":
            return None
        stump_vine += 1
        if stump_vine > 1:
            return None
    log_provider = None
    probability = 0.0
    for decorator in config.get("log_decorators", []) or []:
        if not isinstance(decorator, dict) or decorator.get("type") != "minecraft:attached_to_logs":
            return None
        if log_provider is not None:
            return None
        directions = decorator.get("directions") or []
        if list(directions) != ["up"]:
            return None
        log_provider = providers.add(decorator.get("block_provider"), ints)
        chance = decorator.get("probability")
        if log_provider is None or isinstance(chance, bool) or not isinstance(chance, (int, float)):
            return None
        probability = float(chance)
    return {
        "trunk": trunk,
        "length": length,
        "stump_vine": stump_vine,
        "log_provider": _U32_MAX if log_provider is None else log_provider,
        "probability": probability,
    }


# --- the remaining configured-feature kinds ---------------------------------
#
# Every kind the runtime applies has its own flat row; these are the ones whose
# configs fit the shared shape below. `a`/`b`/`c` are the three states the kind
# reads (in the order its algorithm names them), `ref`/`ref2` are `|` name
# lists, `child`/`child2` are placed-feature references (the kinds that recurse
# into another placed feature), and the ints/floats go into the shared pools in
# the order the runtime indexes them (named in worldgen_feature.cpp at each
# use, and in docs/PHASE4.md).


def _tag_or_names(value, tags):
    """A tag reference (`"#minecraft:x"`) resolved, or a name list as-is."""
    if value is None:
        return ""
    if isinstance(value, dict):
        # A `BlockPredicate` object (e.g. huge fungus' `replaceable_blocks`).
        # Iterating it as a list used to serialise its keys ("type|blocks"),
        # which made every replaceable test fail.
        return _block_predicate_names(value, tags)
    if isinstance(value, str):
        if value.startswith("#"):
            return "|".join(sorted(tags.resolve(value[1:])))
        return value
    return name_list(value)


def _provider_state(value):
    """A `BlockStateProvider` reduced to a simple state, or None.

    A weighted provider has no single state. The runtime's flat config row can
    carry one, so it carries the dominant entry; the per-placement weighted
    draw is a separate, smaller parity gap (the crimson-forest vegetation row
    is roots/fungus/fungus at 87/11/1).
    """
    if isinstance(value, dict):
        if value.get("type") == "minecraft:simple_state_provider":
            return state_of(value.get("state", {}))
        if value.get("type") == "minecraft:weighted_state_provider":
            entries = value.get("entries") or []
            if entries:
                best = max(entries, key=lambda e: int(e.get("weight", 0)))
                return state_of(best.get("data", {}))
    return _simple_provider_state(value)


def _ref_name(value):
    """A placed-feature reference: a plain id, or `{\"feature\": id}`."""
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        return feature_ref(value) or ""
    return ""


def _int_pair_provider(value):
    """An `IntProvider` as (lo, hi); a constant is (v, v)."""
    if isinstance(value, int):
        return (value, value)
    if isinstance(value, dict):
        if "value" in value and isinstance(value["value"], int):
            return (value["value"], value["value"])
        lo = value.get("min_inclusive", value.get("min", 0))
        hi = value.get("max_inclusive", value.get("max", lo))
        return (int(lo), int(hi))
    return (0, 0)


def _float_pair_provider(value):
    if isinstance(value, (int, float)):
        return (float(value), float(value))
    if isinstance(value, dict):
        if "value" in value:
            return (float(value["value"]), float(value["value"]))
        lo = value.get("min_inclusive", value.get("min", 0.0))
        hi = value.get("max_inclusive", value.get("max", lo))
        return (float(lo), float(hi))
    return (0.0, 0.0)


def extra_config_of(ftype, config, tags):
    """The flat config for a kind outside the older per-kind tables."""
    if not isinstance(config, dict):
        config = {}
    row = {"a": None, "b": None, "c": None, "d": None, "e": None,
           "ref": "", "ref2": "", "child": "", "child2": "",
           "ints": [], "floats": []}
    ints = row["ints"]
    floats = row["floats"]

    if ftype == "minecraft:block_blob":
        row["a"] = _provider_state(config.get("state"))
        row["ref"] = _tag_or_names(_block_predicate_names(config.get("can_place_on"), tags), tags)
    elif ftype == "minecraft:iceberg":
        row["a"] = _provider_state(config.get("state"))
    elif ftype == "minecraft:lake":
        row["a"] = _provider_state(config.get("fluid"))
        row["b"] = _provider_state(config.get("barrier"))
    elif ftype == "minecraft:delta_feature":
        row["a"] = _provider_state(config.get("contents"))
        row["b"] = _provider_state(config.get("rim"))
        ints.extend(_int_pair_provider(config.get("size")))
        ints.extend(_int_pair_provider(config.get("rim_size")))
    elif ftype == "minecraft:netherrack_replace_blobs":
        row["a"] = state_of(config.get("target_state", {}))
        row["b"] = state_of(config.get("state", {}))
        ints.extend(_int_pair_provider(config.get("radius")))
    elif ftype == "minecraft:sea_pickle":
        ints.extend(_int_pair_provider(config.get("count")))
    elif ftype == "minecraft:basalt_columns":
        ints.extend(_int_pair_provider(config.get("height")))
        ints.extend(_int_pair_provider(config.get("reach")))
    elif ftype == "minecraft:bamboo":
        floats.append(float(config.get("probability", 0.0)))
    elif ftype == "minecraft:nether_forest_vegetation":
        row["a"] = _provider_state(config.get("state_provider"))
        ints.extend([int(config.get("spread_width", 0)), int(config.get("spread_height", 0))])
    elif ftype == "minecraft:twisting_vines":
        ints.extend([int(config.get("spread_width", 0)), int(config.get("spread_height", 0)),
                     int(config.get("max_height", 0))])
    elif ftype in ("minecraft:vegetation_patch", "minecraft:waterlogged_vegetation_patch"):
        row["a"] = _provider_state(config.get("ground_state"))
        row["ref"] = _tag_or_names(config.get("replaceable"), tags)
        row["child"] = _ref_name(config.get("vegetation_feature"))
        ints.extend(_int_pair_provider(config.get("xz_radius")))
        ints.extend(_int_pair_provider(config.get("depth")))
        ints.append(int(config.get("vertical_range", 0)))
        ints.append(1 if config.get("surface") == "ceiling" else 0)
        ints.append(1 if ftype == "minecraft:waterlogged_vegetation_patch" else 0)
        floats.append(float(config.get("extra_edge_column_chance", 0.0)))
        floats.append(float(config.get("extra_bottom_block_chance", 0.0)))
        floats.append(float(config.get("vegetation_chance", 0.0)))
    elif ftype == "minecraft:sculk_patch":
        ints.extend([
            int(config.get("charge_count", 0)), int(config.get("amount_per_charge", 0)),
            int(config.get("spread_rounds", 0)), int(config.get("growth_rounds", 0)),
            int(config.get("spread_attempts", 0)),
        ])
        ints.extend(_int_pair_provider(config.get("extra_rare_growths")))
        floats.append(float(config.get("catalyst_chance", 0.0)))
    elif ftype == "minecraft:root_system":
        row["child"] = _ref_name(config.get("tree_feature"))
        row["a"] = _provider_state(config.get("root_state_provider"))
        row["b"] = _provider_state(config.get("hanging_root_state_provider"))
        row["ref"] = _tag_or_names(config.get("root_replaceable"), tags)
        ints.extend([
            int(config.get("root_radius", 0)), int(config.get("root_placement_attempts", 0)),
            int(config.get("hanging_root_radius", 0)),
            int(config.get("hanging_roots_vertical_span", 0)),
            int(config.get("hanging_root_placement_attempts", 0)),
            int(config.get("required_vertical_space_for_tree", 0)),
            int(config.get("allowed_vertical_water_for_tree", 0)),
            int(config.get("level_test_distance", 0)), int(config.get("max_level_deviation", 0)),
            int(config.get("root_column_max_height", 0)),
        ])
    elif ftype == "minecraft:underwater_magma":
        ints.extend([int(config.get("floor_search_range", 0)),
                     int(config.get("placement_radius_around_floor", 0))])
        floats.append(float(config.get("placement_probability_per_valid_position", 0.0)))
    elif ftype == "minecraft:speleothem":
        # `base_block`/`pointed_block` are already full block-state objects
        # (`{"Name": ..., "Properties": ...}`), not names to wrap: wrapping one
        # made the row's name the *string* of the object, which no block table
        # knows, so any column a speleothem touched could not be encoded.
        row["a"] = state_of(config.get("base_block") or {})
        row["b"] = state_of(config.get("pointed_block") or {})
        row["ref"] = _tag_or_names(config.get("replaceable_blocks"), tags)
        floats.extend([float(config.get("chance_of_taller_generation", 0.0)),
                       float(config.get("chance_of_directional_spread", 0.0)),
                       float(config.get("chance_of_spread_radius2", 0.0)),
                       float(config.get("chance_of_spread_radius3", 0.0))])
    elif ftype == "minecraft:speleothem_cluster":
        row["a"] = state_of(config.get("base_block") or {})
        row["b"] = state_of(config.get("pointed_block") or {})
        row["ref"] = _tag_or_names(config.get("replaceable_blocks"), tags)
        ints.extend(_int_pair_provider(config.get("height")))
        ints.extend(_int_pair_provider(config.get("radius")))
        ints.extend(_int_pair_provider(config.get("speleothem_block_layer_thickness")))
        ints.extend(_int_pair_provider(config.get("floor_to_ceiling_search_range")))
        ints.append(int(config.get("max_stalagmite_stalactite_height_diff", 0)))
        floats.extend(_float_pair_provider(config.get("wetness")))
        floats.extend(_float_pair_provider(config.get("density")))
        floats.append(float(config.get("chance_of_speleothem_at_max_distance_from_center", 0.0)))
        floats.append(float(config.get("max_distance_from_edge_affecting_chance_of_speleothem", 0.0)))
        floats.append(float(config.get("max_distance_from_center_affecting_height_bias", 0.0)))
        floats.append(float(config.get("height_deviation", 0.0)))
    elif ftype == "minecraft:large_dripstone":
        row["ref"] = _tag_or_names(config.get("replaceable_blocks"), tags)
        ints.extend(_int_pair_provider(config.get("column_radius")))
        ints.extend([int(config.get("floor_to_ceiling_search_range", 0)),
                     int(config.get("min_radius_for_wind", 0))])
        floats.extend(_float_pair_provider(config.get("height_scale")))
        floats.extend(_float_pair_provider(config.get("stalactite_bluntness")))
        floats.extend(_float_pair_provider(config.get("stalagmite_bluntness")))
        floats.extend(_float_pair_provider(config.get("wind_speed")))
        floats.append(float(config.get("max_column_radius_to_cave_height_ratio", 0.0)))
        floats.append(float(config.get("min_bluntness_for_wind", 0.0)))
    elif ftype in ("minecraft:huge_brown_mushroom", "minecraft:huge_red_mushroom"):
        row["a"] = _provider_state(config.get("stem_provider"))
        row["b"] = _provider_state(config.get("cap_provider"))
        row["ref"] = _tag_or_names(_block_predicate_names(config.get("can_place_on"), tags), tags)
        ints.append(int(config.get("foliage_radius", 0)))
        ints.append(1 if ftype == "minecraft:huge_brown_mushroom" else 0)
    elif ftype == "minecraft:huge_fungus":
        # 26.2 names the stem as a plain `stem_state` block state; older packs
        # used a `stem_provider`. Reading only the latter made every fungus
        # stem air and left the feature with nothing to place.
        stem = config.get("stem_state")
        if isinstance(stem, dict) and "Name" in stem:
            row["a"] = state_of(stem)
        else:
            row["a"] = _provider_state(stem or config.get("stem_provider"))
        row["b"] = state_of(config.get("hat_state", {}))
        row["c"] = state_of(config.get("decor_state", {}))
        # `replaceable_blocks` is a `BlockPredicate`, not a bare tag/name list;
        # passing the dict to `_tag_or_names` serialised its keys ("type|blocks")
        # and every hat/brim attempt then saw an empty replaceable set.
        row["ref"] = _tag_or_names(_block_predicate_names(config.get("replaceable_blocks"), tags), tags)
        row["ref2"] = state_of(config.get("valid_base_block", {}))[0]
        ints.append(1 if config.get("planted") else 0)
    elif ftype in ("minecraft:coral_tree", "minecraft:coral_claw", "minecraft:coral_mushroom"):
        row["a"] = state_of({"Name": config.get("block", "minecraft:air")})
        ints.append({"minecraft:coral_tree": 0, "minecraft:coral_claw": 1,
                     "minecraft:coral_mushroom": 2}[ftype])
    elif ftype == "minecraft:sequence":
        children = [_ref_name(ref) for ref in config.get("features", [])]
        row["child"] = children[0] if children else ""
        row["child2"] = "|".join(children)
    elif ftype == "minecraft:weighted_random_selector":
        children = []
        for entry in config.get("features", []):
            name = _ref_name(entry.get("data") if isinstance(entry, dict) else entry)
            weight = int(entry.get("weight", 1)) if isinstance(entry, dict) else 1
            if name:
                children.append(f"{name},{weight}")
        row["child2"] = "|".join(children)
    elif ftype == "minecraft:geode":
        # `GeodeConfiguration`: the five layer/state providers, the two name
        # lists its writes are gated by, and the inner placements (each one a
        # state, encoded as `name;props` in the `|`-joined `child2`). Its
        # ints/floats are the provider bounds, the two gen offsets, the layer
        # thicknesses and the crack settings, in that order (the runtime's own
        # slot comments name each).
        blocks = config.get("blocks", {}) or {}
        row["a"] = _provider_state(blocks.get("filling_provider"))
        row["b"] = _provider_state(blocks.get("inner_layer_provider"))
        row["c"] = _provider_state(blocks.get("alternate_inner_layer_provider"))
        row["d"] = _provider_state(blocks.get("middle_layer_provider"))
        row["e"] = _provider_state(blocks.get("outer_layer_provider"))
        row["ref"] = _tag_or_names(blocks.get("cannot_replace"), tags)
        row["ref2"] = _tag_or_names(blocks.get("invalid_blocks"), tags)
        placements = []
        for state in blocks.get("inner_placements", []):
            name, props = state_of(state)
            placements.append(f"{name};{props}")
        row["child2"] = "|".join(placements)
        # The pack omits most of these and relies on the codec's defaults, so
        # each fallback is the codec's own (GeodeConfiguration's optionalFieldOf
        # values), not zero.
        ints.extend(_int_pair_provider(config.get("outer_wall_distance",
                                                 {"min_inclusive": 4, "max_inclusive": 5})))
        ints.extend(_int_pair_provider(config.get("distribution_points",
                                                 {"min_inclusive": 3, "max_inclusive": 4})))
        ints.extend(_int_pair_provider(config.get("point_offset",
                                                 {"min_inclusive": 1, "max_inclusive": 2})))
        ints.append(int(config.get("min_gen_offset", -16)))
        ints.append(int(config.get("max_gen_offset", 16)))
        ints.append(int(config.get("invalid_blocks_threshold", 0)))
        layers = config.get("layers", {}) or {}
        crack = config.get("crack", {}) or {}
        floats.extend([
            float(config.get("noise_multiplier", 0.05)),
            float(config.get("use_potential_placements_chance", 0.35)),
            float(config.get("use_alternate_layer0_chance", 0.0)),
            1.0 if config.get("placements_require_layer0_alternate", True) else 0.0,
            float(layers.get("filling", 1.7)),
            float(layers.get("inner_layer", 2.2)),
            float(layers.get("middle_layer", 3.2)),
            float(layers.get("outer_layer", 4.2)),
            float(crack.get("generate_crack_chance", 1.0)),
            float(crack.get("base_crack_size", 2.0)),
            float(crack.get("crack_point_offset", 2)),
        ])
    elif ftype == "minecraft:spike":
        # `SpikeConfiguration`: the packed-ice state, the block the spike may
        # start on, and the blocks (a tag here) it may write over.
        row["a"] = state_of(config.get("state", {}))
        row["ref"] = _tag_or_names(_block_predicate_names(config.get("can_place_on"), tags), tags)
        row["ref2"] = _tag_or_names(_block_predicate_names(config.get("can_replace"), tags), tags)
    return row


def collect_configured(pack, tags, preds):
    """The configured-feature rows plus the per-kind config tables.

    Returns (rows, ore_targets, ore_configs, simple_configs, pile_configs,
    disk_configs, tree_configs, spring_configs, selector_rows,
    selector_children, column_configs, column_layers, state_providers,
    int_providers, seagrass_configs), where each row is
    (name, type, kind, config_index).
    """
    base = pack / "configured_feature"
    rows = []
    fallen_tree_configs = []
    fallen_raw = []
    ore_targets = []
    ore_configs = []
    simple_configs = []
    pile_configs = []
    disk_configs = []
    tree_configs = []
    spring_configs = []
    selector_rows = []
    selector_children = []
    column_configs = []
    column_layers = []
    seagrass_configs = []
    multiface_configs = []
    extra_configs = []
    fossil_configs = 0  # `kFossil` indexes `kWgFossilConfigs`, not the flat row
    state_providers = StateProviders()
    int_providers = IntProviders()

    def register(name, ftype, config):
        nonlocal fossil_configs
        # No silent fallback: a pack feature type the runtime does not apply is
        # an unmodelled kind, and the honest answer is a failed generation, not
        # a row the declaration stage counts and never dispatches.
        kind = _KIND_BY_TYPE.get(ftype)
        if kind is None:
            raise ValueError(
                f"configured feature {name!r}: type {ftype!r} has no runtime kind; "
                "add it to the kind table and worldgen_feature.cpp instead of "
                "letting the row fall through to kOther")
        index = _U32_MAX
        if kind == "kOre" or kind == "kScatteredOre":
            parsed = ore_config_of(config)
            index = len(ore_configs)
            target_offset = len(ore_targets)
            for target in parsed["targets"]:
                ore_targets.append({
                    "state": target["state"],
                    "predicate": target["predicate"],
                    "ref": target["ref"],
                    "tag": target["tag"],
                })
            ore_configs.append({
                "size": parsed["size"],
                "discard": parsed["discard"],
                "offset": target_offset,
                "count": len(parsed["targets"]),
            })
        elif kind == "kSimpleBlock":
            state = simple_state_of(config)
            if state is not None:
                index = len(simple_configs)
                simple_configs.append(state)
        elif kind == "kBlockPile":
            state = block_pile_state_of(config)
            if state is not None:
                index = len(pile_configs)
                pile_configs.append(state)
        elif kind == "kDisk":
            disk = disk_config_of(config, tags)
            if disk is not None:
                index = len(disk_configs)
                disk_configs.append(disk)
        elif kind == "kSpring":
            index = len(spring_configs)
            spring_configs.append(spring_config_of(config, tags))
        elif kind == "kSeagrass":
            probability = seagrass_config_of(config)
            if probability is not None:
                index = len(seagrass_configs)
                seagrass_configs.append(probability)
        elif kind == "kFallenTree":
            # Registered after the loop: its providers join the shared tables
            # *after* the block columns' own rows, so those keep their indices
            # and the generated header stays diffable.
            fallen_raw.append((name, config))
        elif kind == "kBlockColumn":
            column = column_config_of(config, state_providers, int_providers)
            if column is not None:
                offset = len(column_layers)
                column_layers.extend(column["layers"])
                index = len(column_configs)
                column_configs.append({
                    "offset": offset,
                    "count": len(column["layers"]),
                    "direction": column["direction"],
                    "prioritize_tip": column["prioritize_tip"],
                    "allowed": column["allowed"],
                })
        elif kind == "kMultifaceGrowth":
            growth = multiface_config_of(config, tags)
            if growth is not None:
                index = len(multiface_configs)
                multiface_configs.append(growth)
        elif kind == "kFossil":
            # The fossil config tables live in `worldgen_fossil_table.h`; the
            # feature row's index is the fossil config index (the same sorted
            # order the emitter walks).
            index = fossil_configs
            fossil_configs += 1
        elif kind in _EXTRA_KINDS:
            index = len(extra_configs)
            extra_configs.append(extra_config_of(ftype, config, tags))
        elif kind == "kTree":
            tree = tree_config_of(config)
            if tree["trunk"] is not None and tree["foliage"] is not None:
                index = len(tree_configs)
                tree_configs.append(tree)
        elif kind in ("kRandomSelector", "kSimpleRandomSelector", "kRandomBooleanSelector"):
            kind_code = {"kRandomSelector": 0, "kSimpleRandomSelector": 1,
                         "kRandomBooleanSelector": 2}[kind]

            def add_inline(defn):
                if not isinstance(defn, dict) or "type" not in defn:
                    return None
                inline_name = f"minecraft:inline/{len(rows)}"
                register(inline_name, defn.get("type", ""), defn.get("config", {}))
                return inline_name

            skind, children, default = selector_config_of(config, kind_code, add_inline)
            offset = len(selector_children)
            for feature, chance, is_placed in children:
                selector_children.append((feature, chance, is_placed))
            index = len(selector_rows)
            selector_rows.append((skind, offset, len(children),
                                  default[0] if default else ""))
        rows.append((name, ftype, kind, index))
        return index

    for name, path in ids_of(base):
        doc = load(path)
        register(name, doc.get("type", ""), doc.get("config", {}))
    for name, config in fallen_raw:
        fallen = fallen_tree_config_of(config, state_providers, int_providers)
        if fallen is None:
            continue
        index = len(fallen_tree_configs)
        fallen_tree_configs.append(fallen)
        for i, row in enumerate(rows):
            if row[0] == name:
                rows[i] = (row[0], row[1], row[2], index)
                break
    rows.sort(key=lambda row: row[0])
    return (rows, ore_targets, ore_configs, simple_configs, pile_configs,
            disk_configs, tree_configs, spring_configs, selector_rows,
            selector_children, column_configs, column_layers, state_providers,
            int_providers, seagrass_configs, fallen_tree_configs, multiface_configs,
            extra_configs)

# --- placements -------------------------------------------------------------

def _int_provider(value):
    """An `IntProvider` reduced to its upper bound (the value a spread uses)."""
    if isinstance(value, int):
        return value
    if isinstance(value, dict):
        if "max_inclusive" in value:
            return int(value["max_inclusive"])
        if "max" in value:
            return int(value["max"])
        if isinstance(value.get("value"), int):
            return int(value["value"])
    return 0


def placement_of(modifier, add_predicate=None):
    """A `PlacementModifier` as (kind, [i0..i5]); unknown kinds become kOther."""
    mtype = modifier.get("type", "")
    if mtype == "minecraft:surface_water_depth_filter":
        return ("kSurfaceWaterDepthFilter", [int(modifier.get("max_water_depth", 0)),
                                            0, 0, 0, 0, 0])
    if mtype == "minecraft:block_predicate_filter" and add_predicate is not None:
        index = add_predicate(modifier.get("predicate", {}))
        return ("kBlockPredicateFilter", [index, 0, 0, 0, 0, 0])
    if mtype == "minecraft:noise_threshold_count":
        return ("kNoiseThresholdCount", [int(modifier.get("above_noise", 0)),
                                         int(modifier.get("below_noise", 0)),
                                         int(round(float(modifier.get("noise_level", 0.0)) * 1000)),
                                         0, 0, 0])
    if mtype == "minecraft:count_on_every_layer":
        count = modifier.get("count", 1)
        if isinstance(count, int):
            return ("kCountOnEveryLayer", [count, count, 0, 0, 0, 0])
        return ("kCountOnEveryLayer", [_int_provider(count), _int_provider(count), 0, 0, 0, 0])
    if mtype == "minecraft:count":
        count = modifier.get("count", 0)
        if isinstance(count, int):
            return ("kCount", [count, count, 0, 0, 0, 0])
        if isinstance(count, dict):
            if "min_inclusive" in count and "max_inclusive" in count:
                return ("kCount", [int(count["min_inclusive"]), int(count["max_inclusive"]),
                                   0, 0, 0, 0])
            if count.get("type") == "minecraft:weighted_list":
                data = [int(entry.get("data", 0)) for entry in count.get("distribution", [])]
                if data:
                    return ("kCount", [min(data), max(data), 0, 0, 0, 0])
        return ("kOther", [0] * 6)
    if mtype == "minecraft:rarity_filter":
        return ("kRarityFilter", [int(modifier.get("chance", 0)), 0, 0, 0, 0, 0])
    if mtype == "minecraft:in_square":
        return ("kInSquare", [0] * 6)
    if mtype == "minecraft:height_range":
        height = modifier.get("height", {})
        htype = height.get("type")
        if htype == "minecraft:constant":
            kind, value = parse_anchor(height.get("value", {}))
            return ("kHeightRange", [0, kind, value, 0, 0, 0])
        if htype == "minecraft:trapezoid" or "min_inclusive" in height:
            code = 2 if htype == "minecraft:trapezoid" else 1
            lo_kind, lo = parse_anchor(height.get("min_inclusive", {}))
            hi_kind, hi = parse_anchor(height.get("max_inclusive", {}))
            plateau = int(height.get("plateau", 0)) if htype == "minecraft:trapezoid" else 0
            return ("kHeightRange", [code, lo_kind, lo, hi_kind, hi, plateau])
        return ("kOther", [0] * 6)
    if mtype == "minecraft:heightmap":
# The engine's own `HeightmapType` values; the `_WG` forms reuse the
# live map the generation stage settles on.
        ordinal = {"WORLD_SURFACE_WG": 4, "WORLD_SURFACE": 1, "OCEAN_FLOOR_WG": 3,
                   "OCEAN_FLOOR": 3, "MOTION_BLOCKING": 4,
                   "MOTION_BLOCKING_NO_LEAVES": 5}.get(modifier.get("heightmap"), 0)
        return ("kHeightmap", [ordinal, 0, 0, 0, 0, 0])
    if mtype == "minecraft:biome":
        return ("kBiome", [0] * 6)
    if mtype == "minecraft:surface_relative_threshold_filter":
        ordinal = {"WORLD_SURFACE_WG": 4, "WORLD_SURFACE": 1, "OCEAN_FLOOR_WG": 3,
                   "OCEAN_FLOOR": 3, "MOTION_BLOCKING": 4,
                   "MOTION_BLOCKING_NO_LEAVES": 5}.get(modifier.get("heightmap"), 0)
        low = modifier.get("min_inclusive")
        high = modifier.get("max_inclusive")
        return ("kSurfaceRelativeThresholdFilter",
                [ordinal, int(low) if low is not None else 0, int(low is not None),
                 int(high) if high is not None else 0, int(high is not None), 0])
    if mtype == "minecraft:random_offset":
        return ("kRandomOffset", [_int_provider(modifier.get("xz_spread", 0)),
                                  _int_provider(modifier.get("y_spread", 0)), 0, 0, 0, 0])
    return ("kOther", [0] * 6)


class Predicates:
    """A flat predicate table: each row is (kind, a, b, ref); containers hold a
    range of child indices in `children`, `would_survive` holds a state index.
    Kind codes: 0 always, 1 blocks, 2 all_of, 3 any_of, 4 not, 5 would_survive,
    6 solid, 7 has_sturdy_face, 8 replaceable, 9 fluids."""

    def __init__(self, tags):
        self.tags = tags
        self.rows = []
        self.children = []
        self.states = []

    def add(self, pred):
        ptype = pred.get("type")
        offset = pred.get("offset") or [0, 0, 0]
        offset = tuple(int(v) if isinstance(v, int) else 0 for v in offset)[:3]
        if len(offset) < 3:
            offset = offset + (0,) * (3 - len(offset))
        if ptype == "minecraft:matching_block_tag":
            return self._emit(1, 0, 0, name_list(self.tags.resolve(pred.get("tag", ""))), offset)
        if ptype == "minecraft:matching_blocks":
            blocks = pred.get("blocks") or ([pred["block"]] if "block" in pred else [])
            return self._emit(1, 0, 0, name_list(blocks), offset)
        if ptype in ("minecraft:all_of", "minecraft:any_of"):
            index = len(self.rows)
            self.rows.append(None)  # reserve before recursing
            child_offset = len(self.children)
            self.children.extend(self.add(p) for p in pred.get("predicates", []))
            self.rows[index] = (2 if ptype.endswith("all_of") else 3, child_offset,
                                len(self.children) - child_offset, "", offset[0],
                                offset[1], offset[2])
            return index
        if ptype == "minecraft:not":
            child = self.add(pred.get("predicate", {}))
            return self._emit(4, child, 0, "", offset)
        if ptype == "minecraft:would_survive":
            state_index = len(self.states)
            self.states.append(state_of(pred.get("state", {})))
            return self._emit(5, state_index, 0, "", offset)
        if ptype == "minecraft:solid":
            return self._emit(6, 0, 0, "", offset)
        if ptype == "minecraft:has_sturdy_face":
            return self._emit(7, 0, 0, "", offset)
        if ptype == "minecraft:replaceable":
            return self._emit(8, 0, 0, "", offset)
        if ptype == "minecraft:matching_fluids":
            fluids = pred.get("fluids") or ([pred["fluid"]] if "fluid" in pred else [])
            return self._emit(9, 0, 0, name_list(fluids), offset)
        return self._emit(0, 0, 0, "", offset)

    def _emit(self, kind, a, b, ref, offset=(0, 0, 0)):
        self.rows.append((kind, a, b, ref, offset[0], offset[1], offset[2]))
        return len(self.rows) - 1


def collect_placed(pack, preds):
    base = pack / "placed_feature"
    rows = []
    placements = []
    for name, path in ids_of(base):
        doc = load(path)
        feature = doc.get("feature")
        target = feature if isinstance(feature, str) else name
        offset = len(placements)
        for modifier in doc.get("placement", []):
            kind, params = placement_of(modifier, preds.add)
            placements.append((kind, params))
        rows.append((name, target, offset, len(doc.get("placement", []))))
    return rows, placements


def collect_biome_features(pack):
    base = pack / "biome"
    entries = []
    biomes = []
    for name, path in ids_of(base):
        features = load(path).get("features", [])
        offset = len(entries)
        count = 0
        for step, step_list in enumerate(features):
            for feature in step_list:
                entries.append((step, feature))
                count += 1
        biomes.append((name, offset, count))
    return entries, biomes


def collect_carvers(pack):
    base = pack / "configured_carver"
    rows = []
    for name, path in ids_of(base):
        doc = load(path)
        rows.append((name, doc.get("type", ""), doc.get("config", {})))
    return rows


# A `FloatProvider` as (kind, min, max, plateau). The three shapes the carver
# configs use; `constant` keeps its value in `min` and ignores the rest.
def parse_float_provider(value):
    if isinstance(value, (int, float)):
        return ("kConstant", float(value), 0.0, 0.0)
    kind = value.get("type")
    if kind == "minecraft:constant":
        return ("kConstant", float(value["value"]), 0.0, 0.0)
    if kind == "minecraft:uniform":
        return ("kUniform", float(value["min_inclusive"]), float(value["max_exclusive"]), 0.0)
    if kind == "minecraft:trapezoid":
        return ("kTrapezoid", float(value["min"]), float(value["max"]),
                float(value["plateau"]))
    raise ValueError(f"unparsed carver float provider: {value}")


def parse_height_provider(value):
    """A `HeightProvider` as ((kind, value) min, (kind, value) max).

    Every carver config in the pack is a `uniform` over two anchors; a
    constant height folds onto a zero-width uniform so the runtime samples one
    shape (`Mth.randomBetweenInclusive` with `min == max` is `min`).
    """
    if "min_inclusive" in value:
        return (parse_anchor(value["min_inclusive"]), parse_anchor(value["max_inclusive"]))
    if value.get("type") == "minecraft:constant":
        anchor = parse_anchor(value["value"])
        return (anchor, anchor)
    anchor = parse_anchor(value)
    return (anchor, anchor)


def collect_biome_carvers(pack):
    base = pack / "biome"
    rows = []
    for name, path in ids_of(base):
        carvers = load(path).get("carvers", [])
        if isinstance(carvers, str):
            carvers = [carvers]
        for carver in carvers:
            rows.append((name, carver))
    return rows


def collect_structures(pack, biome_tags):
    base = pack / "structure"
    out = []
    for name, path in ids_of(base):
        doc = load(path)
        biomes, names = biome_set(doc.get("biomes"), biome_tags, f"structure {name!r}")
        out.append((name, doc.get("type", ""), biomes, names))
    return out


def collect_structure_sets(pack, biome_tags):
    """The structure sets, their placement configs and their weighted members.

    The placement half is what `RandomSpreadStructurePlacement` needs to name a
    candidate chunk; the pieces themselves are still the structure slice. Every
    field is read rather than defaulted silently: an unknown placement type or a
    spacing/separation pair the runtime could not use fails the generation.
    """
    base = pack / "structure_set"
    placements = []
    sets = []
    entries = []
    for name, path in ids_of(base):
        doc = load(path)
        placement = doc.get("placement", {})
        ptype = placement.get("type", "")
        row = {
            "type": ptype,
            "salt": int(placement.get("salt", 0)),
            "spacing": 0,
            "separation": 0,
            "spread_type": 0,
            "frequency": 1.0,
            "frequency_reduction": "default",
            "exclusion_set": "",
            "exclusion_chunks": 0,
            "locate": [0, 0, 0],
            "distance": 0,
            "spread": 0,
            "count": 0,
            "preferred_biomes": "",
            "preferred_biome_names": [],
        }
        if ptype == "minecraft:random_spread":
            spacing = int(placement["spacing"])
            separation = int(placement["separation"])
            if not (1 <= spacing <= 4096 and 0 <= separation < spacing):
                raise ValueError(
                    f"structure set {name!r}: spacing {spacing} / separation "
                    f"{separation} is not a usable random spread")
            spread_type = placement.get("spread_type", "linear")
            if spread_type not in ("linear", "triangular"):
                raise ValueError(f"structure set {name!r}: unknown spread_type {spread_type!r}")
            frequency = float(placement.get("frequency", 1.0))
            if not 0.0 < frequency <= 1.0:
                raise ValueError(f"structure set {name!r}: frequency {frequency} outside (0, 1]")
            reduction = placement.get("frequency_reduction_method", "default")
            # The runtime's gate has four reducers and falls back to `default`
            # for a name it does not know; refusing here is what keeps that
            # fallback from turning a new pack's method into a silent draw.
            if reduction not in ("default", "legacy_type_1", "legacy_type_2",
                                 "legacy_type_3"):
                raise ValueError(
                    f"structure set {name!r}: unknown frequency_reduction_method {reduction!r}")
            zone = placement.get("exclusion_zone", {}) or {}
            locate = placement.get("locate_offset", [0, 0, 0])
            if len(locate) != 3:
                raise ValueError(f"structure set {name!r}: locate_offset is not three ints")
            row.update(spacing=spacing, separation=separation,
                       spread_type=0 if spread_type == "linear" else 1,
                       frequency=frequency,
                       frequency_reduction=reduction,
                       exclusion_set=zone.get("other_set", ""),
                       exclusion_chunks=int(zone.get("chunk_count", 0)),
                       locate=[int(v) for v in locate])
        elif ptype == "minecraft:concentric_rings":
            distance = int(placement["distance"])
            spread = int(placement["spread"])
            count = int(placement["count"])
            # The codec's own ranges, plus the pair that would make the ring
            # walk divide by zero or never advance. The pack's row is
            # (32, 3, 128); a future pack that breaks this fails here rather
            # than hanging the generator.
            if not (0 <= distance <= 1023 and 0 <= spread <= 1023 and 1 <= count <= 4095):
                raise ValueError(
                    f"structure set {name!r}: ring distance/spread/count "
                    f"{distance}/{spread}/{count} outside the codec's ranges")
            if count > 0 and spread == 0:
                raise ValueError(f"structure set {name!r}: ring count with zero spread")
            preferred, preferred_names = biome_set(
                placement.get("preferred_biomes"), biome_tags,
                f"structure set {name!r} preferred_biomes")
            row.update(distance=distance, spread=spread, count=count,
                       preferred_biomes=preferred,
                       preferred_biome_names=preferred_names)
        else:
            raise ValueError(f"structure set {name!r}: unknown placement type {ptype!r}")
        placements.append(row)
        offset = len(entries)
        members = doc.get("structures", [])
        for entry in members:
            entries.append((entry.get("structure", ""), int(entry.get("weight", 1))))
        sets.append((name, len(placements) - 1, offset, len(members)))

    # The exclusion zone names another set, and the runtime resolves that name
    # and scans its starts; a name that is not a set, a count outside the
    # codec's own 1..16, or a chain that loops back on itself has no port and
    # fails the generation rather than becoming a scan that finds nothing (or
    # recurses forever).
    rows_by_set = {name: placements[placement] for name, placement, _, _ in sets}
    for name, row in rows_by_set.items():
        other = row["exclusion_set"]
        if not other:
            continue
        if other not in rows_by_set:
            raise ValueError(
                f"structure set {name!r}: exclusion_zone names {other!r}, "
                f"which is not a structure set")
        if not 1 <= row["exclusion_chunks"] <= 16:
            raise ValueError(
                f"structure set {name!r}: exclusion_zone chunk_count "
                f"{row['exclusion_chunks']} outside the codec's 1..16")
    for start in rows_by_set:
        seen = {start}
        other = rows_by_set[start]["exclusion_set"]
        while other:
            if other in seen:
                raise ValueError(f"structure set {start!r}: exclusion_zone chain loops")
            seen.add(other)
            other = rows_by_set[other]["exclusion_set"]
    return placements, sets, entries

# --- emit -------------------------------------------------------------------

def emit_feature_table(pack, tags):
    preds = Predicates(tags)
    (configured, ore_targets, ore_configs, simple_cfg, pile_cfg, disk_cfg,
     tree_cfg, spring_cfg, selector_rows, selector_children, column_cfg,
     column_layers, state_providers, int_providers, seagrass_cfg,
     fallen_tree_cfg, multiface_cfg, extra_cfg) = collect_configured(pack, tags, preds)
    placed, placements = collect_placed(pack, preds)
    # The block columns' own placement predicates join the table *after* the
    # placement modifiers' rows, so every existing row keeps its index.
    for cfg in column_cfg:
        cfg["allowed"] = preds.add(cfg["allowed"])
    biome_entries, biomes = collect_biome_features(pack)

# The soils a plant may survive on, as a name list: the pack's own
# `#minecraft:dirt` and `#minecraft:sand` tags plus the few blocks vanilla's
# `canSurvive` special-cases. `would_survive` reads it (an approximation,
# named in docs/PHASE4.md).
    soil = set(tags.resolve("minecraft:dirt")) | set(tags.resolve("minecraft:sand"))
    soil |= {"minecraft:grass_block", "minecraft:farmland", "minecraft:moss_block",
             "minecraft:pale_moss_block", "minecraft:mud",
             "minecraft:muddy_mangrove_roots", "minecraft:rooted_dirt"}
    soil_list = "|".join(sorted(soil))

    for target in ore_targets:
        if target["tag"] is not None:
            target["ref"] = "|".join(tags.resolve(target["tag"]))

    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// Generated by tools/gen_worldgen_registry_table.py — do not edit by hand.")
    out.append("//")
    out.append("// The overworld's feature registries, read from the 26.2 pack: "
               f"{len(configured)} configured")
    n_inline = sum(1 for row in configured if row[0].startswith("minecraft:inline/"))
    out.append(f"// features ({n_inline} of them inline definitions hoisted out of selectors) and")
    out.append(f"// {len(placed)} placed features, with the parsed configs of the")
    out.append("// kinds the runtime applies and each biome's own decoration lists.")
    out.append("// clang-format off")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("struct WgState {")
    out.append("  const char* name;")
    out.append('  const char* properties; // canonical sorted "k=v,k=v" (empty when none)')
    out.append("};")
    out.append("")
    out.append("enum class WgFeatureKind : uint8_t {")
    # `kOther` stays last: it is the one kind the runtime never applies, and
    # every kind added before it leaves the existing ordinals untouched (the
    # runtime's `applied_by_kind` indexes into this order).
    kinds = ("kOre", "kScatteredOre", "kSimpleBlock", "kBlockPile", "kDisk", "kSpring",
             "kBlockColumn", "kMonsterRoom", "kFreezeTopLayer", "kSeagrass",
             "kRandomSelector", "kSimpleRandomSelector", "kRandomBooleanSelector", "kTree",
             "kFallenTree", "kVines", "kMultifaceGrowth",
             "kBlockBlob", "kDesertWell", "kGlowstoneBlob", "kBlueIce", "kBonusChest",
             "kEndPlatform", "kEndIsland", "kEndGateway", "kEndSpike", "kChorusPlant",
             "kKelp", "kSeaPickle", "kBasaltPillar", "kBasaltColumns", "kReplaceBlobs",
             "kDelta", "kNetherForestVegetation", "kTwistingVines", "kWeepingVines",
             "kBamboo", "kIceberg", "kLake", "kVegetationPatch", "kSculkPatch",
             "kRootSystem", "kUnderwaterMagma", "kSpeleothem", "kSpeleothemCluster",
             "kLargeDripstone", "kHugeMushroom", "kHugeFungus", "kCoral", "kSequence",
             "kWeightedSelector", "kVoidStartPlatform", "kSpike", "kGeode", "kFossil",
             "kOther")
    for kind in kinds:
        out.append(f"  {kind},")
    out.append("};")
    out.append(f"inline constexpr uint32_t kWgFeatureKindCount = {len(kinds)};")
    out.append("")
    out.append("struct WgConfiguredFeature {")
    out.append("  const char* name;")
    out.append("  const char* type;")
    out.append("  WgFeatureKind kind;")
    out.append("  uint32_t config; // index into the kind's own config table (or UINT32_MAX)")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgConfiguredFeature kWgConfiguredFeatures[{len(configured)}] = {{")
    for name, ftype, kind, index in configured:
        out.append(f"    {{{cstr(name)}, {cstr(ftype)}, WgFeatureKind::{kind}, {index}}},")
    out.append("};")
    out.append("")
    out.append("enum class WgPredicateKind : uint8_t { kAlways = 0, kBlocks = 2 };")
    out.append("struct WgOreTarget { WgState state; WgPredicateKind predicate; const char* ref; };")
    out.append(f"inline constexpr WgOreTarget kWgOreTargets[{len(ore_targets)}] = {{")
    for target in ore_targets:
        state = target["state"]
        out.append(f"    {{{{{cstr(state[0])}, {cstr(state[1])}}}, WgPredicateKind::{target['predicate']}, "
                   f"{cstr(target['ref'])}}},")
    out.append("};")
    out.append("struct WgOreConfig { uint32_t size; float discard_on_air; uint32_t target_offset; "
               "uint32_t target_count; };")
    out.append(f"inline constexpr WgOreConfig kWgOreConfigs[{len(ore_configs)}] = {{")
    for cfg in ore_configs:
        out.append(f"    {{{cfg['size']}, {cfg['discard']}f, {cfg['offset']}, {cfg['count']}}},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgState kWgSimpleBlockConfigs[{len(simple_cfg)}] = {{")
    for name, props in simple_cfg:
        out.append(f"    {{{cstr(name)}, {cstr(props)}}},")
    out.append("};")
    out.append(f"inline constexpr WgState kWgBlockPileConfigs[{len(pile_cfg)}] = {{")
    for name, props in pile_cfg:
        out.append(f"    {{{cstr(name)}, {cstr(props)}}},")
    out.append("};")
    out.append("struct WgDiskConfig { WgState state; const char* target;"
               " uint16_t radius_min; uint16_t radius_max; uint8_t half_height; };")
    out.append(f"inline constexpr WgDiskConfig kWgDiskConfigs[{len(disk_cfg)}] = {{")
    for (name, props), rmin, rmax, half, target in disk_cfg:
        out.append(f"    {{{{{cstr(name)}, {cstr(props)}}}, {cstr(target)}, {rmin}, {rmax}, {half}}},")
    out.append("};")
    out.append("struct WgSpringConfig { WgState state; const char* valid_blocks;"
               " uint8_t requires_block_below; uint8_t rock_count; uint8_t hole_count; };")
    out.append(f"inline constexpr WgSpringConfig kWgSpringConfigs[{len(spring_cfg)}] = {{")
    for (name, props), valid, req, rock, hole in spring_cfg:
        out.append(f"    {{{{{cstr(name)}, {cstr(props)}}}, {cstr(valid)}, {req}, {rock}, {hole}}},")
    out.append("};")
    out.append("// `MultifaceGrowthFeature`'s config: the growth block's state, the search")
    out.append("// range, the three face flags `validDirections` is built from (UP for the")
    out.append("// ceiling, DOWN for the floor, the four horizontals for walls, in that")
    out.append("// build order), the spreading chance, and the blocks the growth may sit")
    out.append("// against as a `|` name list.")
    out.append("struct WgMultifaceGrowthConfig {")
    out.append("  WgState state;")
    out.append("  uint32_t search_range;")
    out.append("  const char* can_be_placed_on;")
    out.append("  float chance_of_spreading;")
    out.append("  uint8_t can_place_on_floor;")
    out.append("  uint8_t can_place_on_ceiling;")
    out.append("  uint8_t can_place_on_wall;")
    out.append("};")
    out.append(f"inline constexpr WgMultifaceGrowthConfig "
               f"kWgMultifaceGrowthConfigs[{len(multiface_cfg)}] = {{")
    for cfg in multiface_cfg:
        name, props = cfg["state"]
        out.append(f"    {{{{{cstr(name)}, {cstr(props)}}}, {cfg['search_range']}, "
                   f"{cstr(cfg['can_be_placed_on'])}, {cfg['chance']}f, "
                   f"{cfg['can_place_on_floor']}, {cfg['can_place_on_ceiling']}, "
                   f"{cfg['can_place_on_wall']}}},")
    out.append("};")
    out.append("")
    out.append("// `SeagrassFeature`'s config (`ProbabilityFeatureConfiguration`): the one")
    out.append("// probability drawn per candidate cell that holds water, which picks the")
    out.append("// tall two-block plant over the one-block one.")
    out.append("struct WgSeagrassConfig { float probability; };")
    out.append(f"inline constexpr WgSeagrassConfig kWgSeagrassConfigs[{len(seagrass_cfg)}] = {{")
    for probability in seagrass_cfg:
        out.append(f"    {{{probability}f}},")
    out.append("};")
    out.append("")
    out.append("// `FallenTreeFeature`'s config: the trunk's and the log decorator's state")
    out.append("// providers (into `kWgStateProviders`), the log-length provider (into")
    out.append("// `kWgIntProviders`), the stump decorator (0 or 1 `trunk_vine`) and the")
    out.append("// log decorator's own probability for its single `up` direction.")
    out.append("struct WgFallenTreeConfig {")
    out.append("  uint32_t trunk;        // state-provider row for the logs")
    out.append("  uint32_t log_length;   // int-provider row, sampled then reduced by 2")
    out.append("  uint32_t log_block;    // state-provider row for `attached_to_logs` (UINT32_MAX when none)")
    out.append("  float probability;     // that decorator's own draw")
    out.append("  uint8_t stump_vine;    // `trunk_vine` on the stump (0 or 1)")
    out.append("};")
    out.append(f"inline constexpr WgFallenTreeConfig kWgFallenTreeConfigs[{len(fallen_tree_cfg)}] = {{")
    for cfg in fallen_tree_cfg:
        out.append(f"    {{{cfg['trunk']}, {cfg['length']}, {cfg['log_provider']}, "
                   f"{cfg['probability']}f, {cfg['stump_vine']}}},")
    out.append("};")
    out.append("")
    int_entries = int_providers.entries or [(0, 0)]
    int_rows = int_providers.rows or [("kConstant", 0, 0, 0, 0)]
    state_entries = state_providers.entries or [(('minecraft:air', ''), 0)]
    state_rows = state_providers.rows or [("kSimple", ('minecraft:air', ''), 0, 0, 0, 0, "")]
    layer_rows = column_layers or [(0, 0)]
    column_rows = column_cfg or [{"offset": 0, "count": 0, "direction": 1,
                                  "prioritize_tip": 0, "allowed": 0}]
    out.append("// A block column's layer heights (`IntProvider`s) and its block-state")
    out.append("// providers, flattened. A weighted list points at its own entries:")
    out.append("// `kWgWeightedInts` names a provider row per entry, `kWgWeightedStates`")
    out.append("// a state per entry, both with the entry's weight.")
    out.append("enum class WgIntProviderKind : uint8_t { kConstant, kUniform,")
    out.append("                                        kBiasedToBottom, kWeightedList };")
    out.append("struct WgIntProvider { WgIntProviderKind kind; int32_t lo; int32_t hi;")
    out.append("                        uint32_t offset; uint32_t count; };")
    out.append("struct WgWeightedInt { uint32_t provider; int32_t weight; };")
    out.append(f"inline constexpr WgWeightedInt kWgWeightedInts[{len(int_entries)}] = {{")
    for provider, weight in int_entries:
        out.append(f"    {{{provider}, {weight}}},")
    out.append("};")
    out.append(f"inline constexpr WgIntProvider kWgIntProviders[{len(int_rows)}] = {{")
    for kind, lo, hi, offset, count in int_rows:
        out.append(f"    {{WgIntProviderKind::{kind}, {lo}, {hi}, {offset}, {count}}},")
    out.append("};")
    out.append("")
    out.append("// A layer's `BlockStateProvider`: kind 0 is the one state it names, kind 1")
    out.append("// a weighted list (`offset`/`count` into `kWgWeightedStates`), kind 2 a")
    out.append("// `randomized_int_state_provider` (`source` sampled first, then `values`)")
    out.append("// written into `property`).")
    out.append("enum class WgStateProviderKind : uint8_t { kSimple, kWeighted,")
    out.append("                                        kRandomizedInt };")
    out.append("struct WgWeightedState { WgState state; int32_t weight; };")
    out.append("struct WgStateProvider { WgStateProviderKind kind; WgState state;")
    out.append("                        uint32_t offset; uint32_t count; uint32_t source;")
    out.append("                        uint32_t values; const char* property; };")
    out.append(f"inline constexpr WgWeightedState kWgWeightedStates[{len(state_entries)}] = {{")
    for (name, props), weight in state_entries:
        out.append(f"    {{{{{cstr(name)}, {cstr(props)}}}, {weight}}},")
    out.append("};")
    out.append(f"inline constexpr WgStateProvider kWgStateProviders[{len(state_rows)}] = {{")
    for kind, (name, props), offset, count, source, values, prop in state_rows:
        out.append(f"    {{WgStateProviderKind::{kind}, {{{cstr(name)}, {cstr(props)}}}, "
                   f"{offset}, {count}, {source}, {values}, {cstr(prop)}}},")
    out.append("};")
    out.append("")
    out.append("// `BlockColumnFeature`: the layers in order, the direction they stack along")
    out.append("// (Direction's own ordinal), the predicate the column is truncated by, and")
    out.append("// whether the tip is what survives truncation.")
    out.append("struct WgColumnLayer { uint32_t provider; uint32_t height; };")
    out.append("struct WgColumnConfig { uint32_t layer_offset; uint32_t layer_count;")
    out.append("                        uint8_t direction; uint8_t prioritize_tip;")
    out.append("                        uint32_t allowed_placement; };")
    out.append(f"inline constexpr WgColumnLayer kWgColumnLayers[{len(layer_rows)}] = {{")
    for provider, height in layer_rows:
        out.append(f"    {{{provider}, {height}}},")
    out.append("};")
    out.append(f"inline constexpr WgColumnConfig kWgColumnConfigs[{len(column_rows)}] = {{")
    for cfg in column_rows:
        out.append(f"    {{{cfg['offset']}, {cfg['count']}, {cfg['direction']}, "
                   f"{cfg['prioritize_tip']}, {cfg['allowed']}}},")
    out.append("};")
    out.append("")
# --- the flat extra configs --------------------------------------------------
    extra_ints = []
    extra_floats = []
    extra_rows = []
    for cfg in extra_cfg:
        int_offset = len(extra_ints)
        extra_ints.extend(cfg["ints"])
        float_offset = len(extra_floats)
        extra_floats.extend(cfg["floats"])
        extra_rows.append((cfg, int_offset, float_offset))
    out.append("// One flat config row per kind outside the older per-kind tables: five")
    out.append("// states in the order the kind's algorithm names them (most kinds use the")
    out.append("// first three; the geode's layer stack needs all five), two `|` name lists,")
    out.append("// two recursed placed-feature references, and its ints/floats pooled out of")
    out.append("// line (each kind's header in worldgen_feature.cpp names what each slot is).")
    out.append("struct WgExtraConfig {")
    out.append("  WgState a;\n  WgState b;\n  WgState c;\n  WgState d;\n  WgState e;")
    out.append("  const char* ref;\n  const char* ref2;")
    out.append("  const char* child;\n  const char* child2;")
    out.append("  uint32_t int_offset;\n  uint32_t int_count;")
    out.append("  uint32_t float_offset;\n  uint32_t float_count;")
    out.append("};")
    int_default = extra_ints or [0]
    float_default = extra_floats or [0.0]
    out.append(f"inline constexpr int32_t kWgExtraInts[{len(int_default)}] = {{")
    out.append("    " + ", ".join(str(v) for v in int_default) + ",")
    out.append("};")
    out.append(f"inline constexpr float kWgExtraFloats[{len(float_default)}] = {{")
    out.append("    " + ", ".join(f"{v}f" for v in float_default) + ",")
    out.append("};")
    out.append(f"inline constexpr WgExtraConfig kWgExtraConfigs[{max(len(extra_rows), 1)}] = {{")
    if extra_rows:
        for cfg, int_offset, float_offset in extra_rows:
            a = cfg["a"] or ("minecraft:air", "")
            b = cfg["b"] or ("minecraft:air", "")
            c = cfg["c"] or ("minecraft:air", "")
            d = cfg["d"] or ("minecraft:air", "")
            e = cfg["e"] or ("minecraft:air", "")
            out.append(
                f"    {{{{{cstr(a[0])}, {cstr(a[1])}}}, {{{cstr(b[0])}, {cstr(b[1])}}},"
                f" {{{cstr(c[0])}, {cstr(c[1])}}}, {{{cstr(d[0])}, {cstr(d[1])}}},"
                f" {{{cstr(e[0])}, {cstr(e[1])}}},"
                f" {cstr(cfg['ref'])}, {cstr(cfg['ref2'])},"
                f" {cstr(cfg['child'])}, {cstr(cfg['child2'])},"
                f" {int_offset}, {len(cfg['ints'])}, {float_offset}, {len(cfg['floats'])}}},")
    else:
        out.append("    {{\"\", \"\"}, {\"\", \"\"}, {\"\", \"\"}, {\"\", \"\"}, {\"\", \"\"}, "
                   "\"\", \"\", \"\", \"\", 0, 0, 0, 0},")
    out.append("};")
    out.append("")
# --- trees ------------------------------------------------------------------
    out.append("// A tree's trunk and foliage: the two providers and the placers' own")
    out.append("// parameters. The runtime's tree placer (world/worldgen_feature.cpp)")
    out.append("// reads exactly these; the foliage shapes are modelled from the decoded")
    out.append("// placers and are a plausible approximation, not a byte-for-byte match")
    out.append("// (the decoration random is this port's own; see the stage header).")
    out.append("struct WgTreeConfig {")
    out.append("  WgState trunk;")
    out.append("  WgState foliage;")
    out.append("  uint8_t trunk_kind;\n  uint8_t foliage_kind;")
    out.append("  uint8_t base_height;\n  uint8_t height_rand_a;\n  uint8_t height_rand_b;")
    out.append("  uint8_t foliage_radius;\n  uint8_t foliage_height;\n  uint8_t foliage_offset;")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgTreeConfig kWgTreeConfigs[{len(tree_cfg)}] = {{")
    for tree in tree_cfg:
        trunk = tree["trunk"]
        foliage = tree["foliage"]
        out.append(
            f"    {{{{{cstr(trunk[0])}, {cstr(trunk[1])}}},"
            f" {{{cstr(foliage[0])}, {cstr(foliage[1])}}},"
            f" {tree['trunk_kind']}, {tree['foliage_kind']}, {tree['base_height']},"
            f" {tree['height_rand_a']}, {tree['height_rand_b']}, {tree['radius']},"
            f" {tree['foliage_height']}, {tree['offset']}}},")
    out.append("};")
    out.append("")
# --- selectors --------------------------------------------------------------
    out.append("// random_selector / simple_random_selector / random_boolean_selector:")
    out.append("// a child list (`chance` is a probability for the weighted selector, a")
    out.append("// weight for the simple one, and ignored for the boolean one) plus the")
    out.append("// weighted selector's default feature.")
    placed_names = {name for name, _t, _o, _c in placed}
    configured_names = {name for name, _t, _k, _i in configured}

    def child_kind(name, is_placed):
        if not is_placed:
            return 1  # an inline placed feature -> its configured feature
        if name in placed_names:
            return 0
        if name in configured_names:
            return 1
        return 0

    out.append("// `kind`: 0 a placed feature (run its own placement first), "
               "1 a configured feature placed directly.")
    out.append("struct WgSelectorChild { const char* feature; float chance; uint8_t kind; };")
    out.append("struct WgSelector {")
    out.append("  uint8_t kind; // 0 random_selector, 1 simple_random_selector, 2 boolean")
    out.append("  uint32_t offset;\n  uint32_t count;")
    out.append("  const char* default_feature;")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgSelectorChild kWgSelectorChildren[{len(selector_children)}] = {{")
    for feature, chance, is_placed in selector_children:
        out.append(f"    {{{cstr(feature)}, {chance}f, {child_kind(feature, is_placed)}}},")
    out.append("};")
    out.append(f"inline constexpr WgSelector kWgSelectors[{len(selector_rows)}] = {{")
    for skind, offset, count, default in selector_rows:
        out.append(f"    {{{skind}, {offset}, {count}, {cstr(default)}}},")
    out.append("};")
    out.append("")
# --- block predicates -------------------------------------------------------
    out.append("// Block predicates (`block_predicate_filter`), flattened. kind: 0 always,")
    out.append("// 1 blocks (`ref` is a `|` list), 2 all_of, 3 any_of, 4 not, 5 would_survive")
    out.append("// (`a` is a state index), 6 solid, 7 has_sturdy_face, 8 replaceable, 9 fluids.")
    out.append("// Every row carries the predicate's own `offset`, which the runtime applies")
    out.append("// before testing the position (containers included, as vanilla's")
    out.append("// `BlockPredicate.of` does).")
    out.append("struct WgPredicate {")
    out.append("  uint8_t kind;\n  uint32_t a;\n  uint32_t b;\n  const char* ref;")
    out.append("  int8_t ox;\n  int8_t oy;\n  int8_t oz;")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgPredicate kWgPredicates[{len(preds.rows)}] = {{")
    for kind, a, b, ref, ox, oy, oz in preds.rows:
        out.append(f"    {{{kind}, {a}, {b}, {cstr(ref)}, {ox}, {oy}, {oz}}},")
    out.append("};")
    if preds.children:
        out.append(f"inline constexpr uint32_t kWgPredicateChildren[{len(preds.children)}] = {{")
        out.append("    " + ", ".join(str(v) for v in preds.children) + ",")
        out.append("};")
    else:
        out.append("inline constexpr uint32_t kWgPredicateChildren[1] = {0};")
    out.append(f"inline constexpr WgState kWgPredicateStates[{max(1, len(preds.states))}] = {{")
    if preds.states:
        for name, props in preds.states:
            out.append(f"    {{{cstr(name)}, {cstr(props)}}},")
    else:
        out.append('    {"minecraft:air", ""},')
    out.append("};")
    out.append("")
    out.append("// The soils a `would_survive` plant may sit on (the pack's dirt/sand tags")
    out.append("// plus vanilla's special cases), as a `|` list.")
    out.append(f"inline constexpr const char* kWgPlantSoil = {cstr(soil_list)};")
    out.append("")
    out.append("enum class WgPlacementKind : uint8_t {")
    for kind in ("kCount", "kRarityFilter", "kInSquare", "kHeightRange", "kHeightmap",
                 "kBiome", "kRandomOffset", "kSurfaceWaterDepthFilter",
                 "kBlockPredicateFilter", "kNoiseThresholdCount", "kCountOnEveryLayer",
                 "kSurfaceRelativeThresholdFilter", "kOther"):
        out.append(f"  {kind},")
    out.append("};")
    out.append("")
    out.append("struct WgPlacement {")
    out.append("  WgPlacementKind kind;")
    out.append("  int32_t i0, i1, i2, i3, i4, i5;")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgPlacement kWgPlacements[{len(placements)}] = {{")
    for kind, params in placements:
        values = ", ".join(str(v) for v in params)
        out.append(f"    {{WgPlacementKind::{kind}, {values}}},")
    out.append("};")
    out.append("")
    out.append("struct WgPlacedFeature {")
    out.append("  const char* name;")
    out.append("  const char* feature; // the configured feature it places")
    out.append("  uint32_t placement_offset;")
    out.append("  uint32_t placement_count;")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgPlacedFeature kWgPlacedFeatures[{len(placed)}] = {{")
    for name, target, offset, count in placed:
        out.append(f"    {{{cstr(name)}, {cstr(target)}, {offset}, {count}}},")
    out.append("};")
    out.append("")
    out.append("// Each biome's decoration lists, flattened as (generation step, placed")
    out.append("// feature) pairs. `GenerationStep.Decoration` order is the pack's own array")
    out.append("// order (raw generation 0 .. top layer 10), which is also the order the")
    out.append("// runtime applies them.")
    out.append("struct WgBiomeFeatureEntry { uint32_t step; const char* placed_feature; };")
    out.append("struct WgBiomeFeatures { const char* biome; uint32_t offset; uint32_t count; };")
    out.append("")
    out.append(f"inline constexpr WgBiomeFeatureEntry kWgBiomeFeatureEntries[{len(biome_entries)}] = {{")
    for step, feature in biome_entries:
        out.append(f"    {{{step}, {cstr(feature)}}},")
    out.append("};")
    out.append(f"inline constexpr WgBiomeFeatures kWgBiomeFeatures[{len(biomes)}] = {{")
    for name, offset, count in biomes:
        out.append(f"    {{{cstr(name)}, {offset}, {count}}},")
    out.append("};")
    out.append("")
    out.append("}  // namespace mc::world")
    out.append("// clang-format on")
    return "\n".join(out) + "\n"


def emit_carver_table(pack, tags):
    carvers = collect_carvers(pack)
    biome_carvers = collect_biome_carvers(pack)

    # The `replaceable` block sets, deduped: every overworld carver points at
    # `#minecraft:overworld_carver_replaceables`, and the nether pair at its own
    # tag. The runtime turns the names into a code mask once at startup.
    replaceables = {}
    for _, _, cfg in carvers:
        ref = cfg.get("replaceable", "")
        if isinstance(ref, str):
            names = tuple(tags.resolve(ref.lstrip("#")))
        else:
            names = tuple(sorted(ref))
        replaceables.setdefault(names, len(replaceables))
    replaceable_rows = sorted(replaceables, key=lambda names: replaceables[names])
    replaceable_offset = {names: sum(len(n) for n in replaceable_rows[:index])
                          for index, names in enumerate(replaceable_rows)}

    def anchor_name(anchor):
        return ("kAbsolute", "kAboveBottom", "kBelowTop")[anchor[0]]

    def float_literal(value):
        # C++ needs `1.0f`, not `1f`; `:.9g` drops the fraction on whole floats.
        text = f"{float(value):.9g}"
        if "." not in text and "e" not in text and "E" not in text:
            text += ".0"
        return text + "f"

    def float_row(provider):
        return (f"{{WgFloatProviderKind::{provider[0]}, {float_literal(provider[1])}, "
                f"{float_literal(provider[2])}, {float_literal(provider[3])}}}")

    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// Generated by tools/gen_worldgen_registry_table.py — do not edit by hand.")
    out.append("//")
    out.append(f"// The pack's configured carvers ({len(carvers)}) with the config fields the")
    out.append("// carving stage samples, the blocks each one may replace, and every biome's")
    out.append("// own `carvers` list in the order that list indexes the carver seeds.")
    out.append("// clang-format off")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("enum class WgCarverKind : uint8_t { kCave, kCanyon, kNetherCave };")
    out.append("enum class WgFloatProviderKind : uint8_t { kConstant, kUniform, kTrapezoid };")
    out.append("enum class WgAnchorKind : uint8_t { kAbsolute, kAboveBottom, kBelowTop };")
    out.append("")
    out.append("// `FloatProvider`: a constant, a uniform [min, max), or a trapezoid over")
    out.append("// [min, max] with a `plateau`-wide top.")
    out.append("struct WgFloatProvider {")
    out.append("  WgFloatProviderKind kind;")
    out.append("  float min;")
    out.append("  float max;")
    out.append("  float plateau;")
    out.append("};")
    out.append("")
    out.append("// `HeightProvider.uniform`: a `Mth.randomBetweenInclusive` between two")
    out.append("// resolved `VerticalAnchor`s.")
    out.append("struct WgHeightProvider {")
    out.append("  WgAnchorKind min_kind;")
    out.append("  int32_t min_value;")
    out.append("  WgAnchorKind max_kind;")
    out.append("  int32_t max_value;")
    out.append("};")
    out.append("")
    out.append("struct WgCarver {")
    out.append("  const char* name;")
    out.append("  WgCarverKind kind;")
    out.append("  float probability;")
    out.append("  WgHeightProvider y;")
    out.append("  WgFloatProvider y_scale;")
    out.append("  WgAnchorKind lava_kind;")
    out.append("  int32_t lava_value;")
    out.append("  // The cave half: `CaveCarverConfiguration`'s three providers.")
    out.append("  WgFloatProvider horizontal_radius_multiplier;")
    out.append("  WgFloatProvider vertical_radius_multiplier;")
    out.append("  WgFloatProvider floor_level;")
    out.append("  // The canyon half: `CanyonCarverConfiguration` and its shape.")
    out.append("  WgFloatProvider vertical_rotation;")
    out.append("  WgFloatProvider canyon_distance_factor;")
    out.append("  WgFloatProvider canyon_thickness;")
    out.append("  WgFloatProvider canyon_horizontal_radius_factor;")
    out.append("  int32_t canyon_width_smoothness;")
    out.append("  float canyon_vertical_radius_default_factor;")
    out.append("  float canyon_vertical_radius_center_factor;")
    out.append("  uint32_t replaceable_offset;")
    out.append("  uint32_t replaceable_count;")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgCarver kWgCarvers[{len(carvers)}] = {{")
    constant = ("kConstant", 0.0, 0.0, 0.0)
    for name, ctype, cfg in carvers:
        kind = {
            "minecraft:cave": "kCave",
            "minecraft:canyon": "kCanyon",
            "minecraft:nether_cave": "kNetherCave",
        }[ctype]
        y = parse_height_provider(cfg["y"])
        lava = parse_anchor(cfg["lava_level"])
        replaceable = replaceables[tuple(
            tags.resolve(cfg["replaceable"].lstrip("#"))
            if isinstance(cfg["replaceable"], str) else sorted(cfg["replaceable"]))]
        rows = sum(len(names) for names in replaceable_rows[:replaceable])
        out.append("    {")
        out.append(f"        {cstr(name)}, WgCarverKind::{kind}, "
                   f"{float_literal(cfg['probability'])},")
        out.append(f"        {{WgAnchorKind::{anchor_name(y[0])}, {y[0][1]}, "
                   f"WgAnchorKind::{anchor_name(y[1])}, {y[1][1]}}},")
        out.append(f"        {float_row(parse_float_provider(cfg['yScale']))},")
        out.append(f"        WgAnchorKind::{anchor_name(lava)}, {lava[1]},")
        out.append(f"        {float_row(parse_float_provider(cfg.get('horizontal_radius_multiplier', 0.0)))},")
        out.append(f"        {float_row(parse_float_provider(cfg.get('vertical_radius_multiplier', 0.0)))},")
        out.append(f"        {float_row(parse_float_provider(cfg.get('floor_level', 0.0)))},")
        shape = cfg.get("shape", {})
        out.append(f"        {float_row(parse_float_provider(cfg.get('vertical_rotation', 0.0)))},")
        out.append(f"        {float_row(parse_float_provider(shape.get('distance_factor', 0.0)))},")
        out.append(f"        {float_row(parse_float_provider(shape.get('thickness', 0.0)))},")
        out.append(f"        {float_row(parse_float_provider(shape.get('horizontal_radius_factor', 0.0)))},")
        out.append(f"        {int(shape.get('width_smoothness', 0))}, "
                   f"{float_literal(shape.get('vertical_radius_default_factor', 0.0))}, "
                   f"{float_literal(shape.get('vertical_radius_center_factor', 0.0))},")
        out.append(f"        {rows}, {len(replaceable_rows[replaceable])},")
        out.append("    },")
    out.append("};")
    out.append("")
    out.append("// Every distinct `replaceable` tag, expanded to concrete block names, in")
    out.append("// carver-table order; `WgCarver.replaceable_offset` counts into it.")
    total_replaceable = sum(len(names) for names in replaceable_rows)
    out.append(f"inline constexpr const char* kWgCarverReplaceables[{max(total_replaceable, 1)}] = {{")
    for names in replaceable_rows:
        for block in names:
            out.append(f"    {cstr(block)},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr uint32_t kWgCarverCount = {len(carvers)};")
    out.append("")
    out.append("// One biome's `carvers` list: `count` entries at `offset` in")
    out.append("// `kWgBiomeCarverRefs`, in the biome JSON's own order (the index is part")
    out.append("// of the carver seed: `seed + index`).")
    out.append("struct WgBiomeCarvers {")
    out.append("  const char* biome;")
    out.append("  uint32_t offset;")
    out.append("  uint32_t count;")
    out.append("};")
    out.append("")
    grouped = {}
    for biome, carver in biome_carvers:
        grouped.setdefault(biome, []).append(carver)
    ordered = sorted(grouped)
    out.append(f"inline constexpr const char* kWgBiomeCarverRefs[{max(len(biome_carvers), 1)}] = {{")
    for biome in ordered:
        for carver in grouped[biome]:
            out.append(f"    {cstr(carver)},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgBiomeCarvers kWgBiomeCarvers[{max(len(ordered), 1)}] = {{")
    offset = 0
    for biome in ordered:
        out.append(f"    {{{cstr(biome)}, {offset}, {len(grouped[biome])}}},")
        offset += len(grouped[biome])
    out.append("};")
    out.append(f"inline constexpr uint32_t kWgBiomeCarversCount = {len(ordered)};")
    out.append("")
    out.append("}  // namespace mc::world")
    out.append("// clang-format on")
    return "\n".join(out) + "\n"


def emit_structure_table(pack, biome_tags):
    structures = collect_structures(pack, biome_tags)
    placements, sets, entries = collect_structure_sets(pack, biome_tags)

    structure_biomes = []
    structure_rows = []
    for name, stype, biomes, names in structures:
        structure_rows.append((name, stype, biomes, len(structure_biomes), len(names)))
        structure_biomes.extend(names)

    ring_biomes = []
    placement_offsets = []
    for row in placements:
        names = row["preferred_biome_names"]
        placement_offsets.append((len(ring_biomes), len(names)))
        ring_biomes.extend(names)

    def f32(value):
        text = f"{float(value):.9g}"
        if "." not in text and "e" not in text and "E" not in text:
            text += ".0"
        return text + "f"

    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// Generated by tools/gen_worldgen_registry_table.py — do not edit by hand.")
    out.append("//")
    out.append(f"// The pack's structures ({len(structures)}) and structure sets ({len(sets)}), with")
    out.append("// each set's own placement config — the spacing/separation/salt/spread type")
    out.append("// `RandomSpreadStructurePlacement` names a candidate chunk from, plus its")
    out.append("// frequency gate, exclusion zone and (for concentric rings) the ring fields.")
    out.append("// The registries are carried so the generator can name and select them; the")
    out.append("// pieces are not built yet (docs/PHASE4.md).")
    out.append("// clang-format off")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("struct WgStructure {")
    out.append("  const char* name;")
    out.append("  const char* type;")
    out.append("  const char* biomes;         // the pack's HolderSet reference")
    out.append("  uint32_t biome_offset;      // into kWgStructureBiomes")
    out.append("  uint32_t biome_count;")
    out.append("};")
    out.append("struct WgStructureSetEntry { const char* structure; uint32_t weight; };")
    out.append("struct WgStructurePlacement {")
    out.append("  const char* type; // minecraft:random_spread | minecraft:concentric_rings")
    out.append("  uint32_t salt;")
    out.append("  uint16_t spacing;")
    out.append("  uint16_t separation;")
    out.append("  uint8_t spread_type; // 0 linear, 1 triangular")
    out.append("  float frequency;     // 1.0 when the set has no gate")
    out.append("  const char* frequency_reduction; // default | legacy_type_1..3")
    out.append("  const char* exclusion_set;       // \"\" when none")
    out.append("  uint16_t exclusion_chunks;")
    out.append("  int16_t locate_x, locate_y, locate_z;")
    out.append("  uint16_t distance, spread, count; // concentric rings")
    out.append("  const char* preferred_biomes;     // concentric rings, \"\" when none")
    out.append("  uint32_t preferred_biome_offset;  // into kWgRingPreferredBiomes")
    out.append("  uint16_t preferred_biome_count;")
    out.append("};")
    out.append("struct WgStructureSet {")
    out.append("  const char* name;")
    out.append("  uint32_t placement;")
    out.append("  uint32_t entry_offset;")
    out.append("  uint32_t entry_count;")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgStructure kWgStructures[{len(structures)}] = {{")
    for name, stype, biomes, offset, count in structure_rows:
        out.append(f"    {{{cstr(name)}, {cstr(stype)}, {cstr(biomes)}, {offset}, {count}}},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr const char* kWgStructureBiomes[{len(structure_biomes)}] = {{")
    for biome in structure_biomes:
        out.append(f"    {cstr(biome)},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgStructurePlacement kWgStructurePlacements[{len(placements)}] = {{")
    for row, (biome_offset, biome_count) in zip(placements, placement_offsets):
        locate = row["locate"]
        out.append(
            f"    {{{cstr(row['type'])}, {row['salt']}u, {row['spacing']}, {row['separation']}, "
            f"{row['spread_type']}, {f32(row['frequency'])}, {cstr(row['frequency_reduction'])}, "
            f"{cstr(row['exclusion_set'])}, {row['exclusion_chunks']}, "
            f"{locate[0]}, {locate[1]}, {locate[2]}, "
            f"{row['distance']}, {row['spread']}, {row['count']}, "
            f"{cstr(row['preferred_biomes'])}, {biome_offset}, {biome_count}}},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr const char* kWgRingPreferredBiomes[{len(ring_biomes)}] = {{")
    for biome in ring_biomes:
        out.append(f"    {cstr(biome)},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgStructureSetEntry kWgStructureSetEntries[{len(entries)}] = {{")
    for structure, weight in entries:
        out.append(f"    {{{cstr(structure)}, {weight}}},")
    out.append("};")
    out.append(f"inline constexpr WgStructureSet kWgStructureSets[{len(sets)}] = {{")
    for name, placement, offset, count in sets:
        out.append(f"    {{{cstr(name)}, {placement}, {offset}, {count}}},")
    out.append("};")
    out.append("")
    out.append("}  // namespace mc::world")
    out.append("// clang-format on")
    return "\n".join(out) + "\n"


# --- fossils ----------------------------------------------------------------

# The two fossil features place structure templates (`StructureTemplate`), and
# their config is the only one in the pack that names registry objects (the
# templates) rather than blocks. The templates and the processor lists are read
# here so the runtime never depends on the pack at run time, the way every other
# registry this tool carries is.

_NBT_END = 0
_NBT_BYTE = 1
_NBT_SHORT = 2
_NBT_INT = 3
_NBT_LONG = 4
_NBT_FLOAT = 5
_NBT_DOUBLE = 6
_NBT_BYTE_ARRAY = 7
_NBT_STRING = 8
_NBT_LIST = 9
_NBT_COMPOUND = 10
_NBT_INT_ARRAY = 11
_NBT_LONG_ARRAY = 12


def _nbt_read(path):
    """The root compound of a (gzipped or raw) big-endian NBT file.

    Only the tags a structure template uses are decoded; a tag this reader does
    not know raises, so a future template cannot be silently truncated.
    """
    raw = path.read_bytes()
    try:
        raw = gzip.decompress(raw)
    except OSError:
        pass
    pos = 0

    def take(n):
        nonlocal pos
        if pos + n > len(raw):
            raise ValueError(f"truncated NBT in {path}")
        out = raw[pos:pos + n]
        pos += n
        return out

    def u8():
        return take(1)[0]

    def i16():
        return struct.unpack(">h", take(2))[0]

    def i32():
        return struct.unpack(">i", take(4))[0]

    def i64():
        return struct.unpack(">q", take(8))[0]

    def f32():
        return struct.unpack(">f", take(4))[0]

    def f64():
        return struct.unpack(">d", take(8))[0]

    def string():
        return take(struct.unpack(">H", take(2))[0]).decode("utf-8")

    def payload(tag):
        if tag == _NBT_END:
            # Only an empty list is allowed to name the end tag as its element
            # type; it has no payload either way.
            return None
        if tag == _NBT_BYTE:
            value = u8()
            return value - 256 if value > 127 else value
        if tag == _NBT_SHORT:
            return i16()
        if tag == _NBT_INT:
            return i32()
        if tag == _NBT_LONG:
            return i64()
        if tag == _NBT_FLOAT:
            return f32()
        if tag == _NBT_DOUBLE:
            return f64()
        if tag == _NBT_BYTE_ARRAY:
            return list(take(i32()))
        if tag == _NBT_STRING:
            return string()
        if tag == _NBT_LIST:
            item_tag = u8()
            count = i32()
            return [payload(item_tag) for _ in range(count)]
        if tag == _NBT_COMPOUND:
            value = {}
            while True:
                child_tag = u8()
                if child_tag == _NBT_END:
                    return value
                # The name is read before the payload: `value[string()] =
                # payload(...)` would evaluate the right-hand side first and
                # consume the payload at the name's offset.
                child_name = string()
                value[child_name] = payload(child_tag)
        if tag == _NBT_INT_ARRAY:
            return [i32() for _ in range(i32())]
        if tag == _NBT_LONG_ARRAY:
            return [i64() for _ in range(i32())]
        raise ValueError(f"unknown NBT tag {tag} in {path}")

    root_tag = u8()
    string()  # root name
    if root_tag != _NBT_COMPOUND:
        raise ValueError(f"{path}: root tag {root_tag} is not a compound")
    return payload(root_tag)


def _fossil_processor(proc_name, pack, tags):
    """`(integrity, protected block names, rules)` for a fossil processor list.

    The processors the fossil features name are `block_rot` (the integrity
    roll), `protected_blocks` (a tag a placement may not replace) and, in the
    diamonds overlay, `rule` — one `block_match` input predicate whose location
    predicate is always true, i.e. "every surviving overlay block becomes this
    state". Anything else is a processor this engine does not model, and
    generating a config that silently ignored it would be worse than failing the
    generator.
    """
    if not isinstance(proc_name, str) or not proc_name.startswith("minecraft:"):
        raise ValueError(f"fossil processor list {proc_name!r} is not a minecraft id")
    doc = load(pack / "processor_list" / (proc_name.split(":", 1)[1] + ".json"))
    integrity = None
    protected = None
    rules = []
    for entry in doc.get("processors", []):
        kind = entry.get("processor_type")
        if kind == "minecraft:block_rot":
            if "rottable_blocks" in entry:
                raise ValueError(f"{proc_name}: rottable_blocks is not modelled")
            integrity = float(entry.get("integrity", 1.0))
        elif kind == "minecraft:protected_blocks":
            value = entry.get("value", "")
            if isinstance(value, str) and value.startswith("#"):
                protected = tags.resolve(value[1:])
            elif isinstance(value, str):
                protected = [value]
            else:
                raise ValueError(f"{proc_name}: protected_blocks value {value!r}")
        elif kind == "minecraft:rule":
            for rule in entry.get("rules", []):
                input_predicate = rule.get("input_predicate", {})
                location_predicate = rule.get("location_predicate", {})
                if input_predicate.get("predicate_type") != "minecraft:block_match":
                    raise ValueError(f"{proc_name}: rule input {input_predicate!r}")
                if location_predicate.get("predicate_type") != "minecraft:always_true":
                    raise ValueError(f"{proc_name}: rule location {location_predicate!r}")
                if "position_predicate" in rule:
                    raise ValueError(f"{proc_name}: rule position predicate is not modelled")
                if "block_entity_modifier" in rule:
                    raise ValueError(f"{proc_name}: rule block entity modifier is not modelled")
                rules.append((input_predicate.get("block"),
                              state_of(rule.get("output_state", {}))))
        else:
            raise ValueError(f"{proc_name}: processor {kind!r} is not modelled")
    if integrity is None or protected is None:
        raise ValueError(f"{proc_name}: missing block_rot/protected_blocks")
    return integrity, protected, rules


def emit_fossil_table(pack, tags):
    base = pack.parent / "structure" / "fossil"
    if not base.is_dir():
        raise ValueError(f"no fossil templates at {base}")

    states = []
    state_index = {}
    blocks = []
    templates = []
    for path in sorted(base.glob("*.nbt")):
        doc = _nbt_read(path)
        size = [int(v) for v in doc["size"]]
        palette = []
        for entry in doc.get("palette", []):
            palette.append((entry["Name"], property_suffix(entry.get("Properties") or {})))
        offset = len(blocks)
        for entry in doc.get("blocks", []):
            x, y, z = [int(v) for v in entry["pos"]]
            name, props = palette[int(entry.get("state", 0))]
            key = (name, props)
            index = state_index.get(key)
            if index is None:
                index = len(states)
                state_index[key] = index
                states.append(key)
            blocks.append((x, y, z, index))
        templates.append({
            "name": "minecraft:fossil/" + path.stem,
            "size": size,
            "blocks": len(blocks) - offset,
            "offset": offset,
        })
    template_index = {entry["name"]: index for index, entry in enumerate(templates)}

    def template_refs(refs):
        out = []
        for ref in refs:
            if ref not in template_index:
                raise ValueError(f"fossil config names unknown template {ref}")
            out.append(template_index[ref])
        return out

    configs = []
    for name, path in ids_of(pack / "configured_feature"):
        doc = load(path)
        if doc.get("type") != "minecraft:fossil":
            continue
        config = doc.get("config", {})
        structures = template_refs(config.get("fossil_structures", []))
        overlays = template_refs(config.get("overlay_structures", []))
        if len(structures) != len(overlays) or not structures:
            raise ValueError(f"{name}: fossil/overlay template counts differ")
        fossil_integrity, protected, fossil_rules = _fossil_processor(
            config.get("fossil_processors"), pack, tags)
        overlay_integrity, protected2, overlay_rules = _fossil_processor(
            config.get("overlay_processors"), pack, tags)
        if protected != protected2:
            raise ValueError(f"{name}: the two processor lists protect different blocks")
        if fossil_rules:
            raise ValueError(f"{name}: a fossil process rule is not modelled")
        configs.append({
            "name": name,
            "structures": structures,
            "overlays": overlays,
            "max_empty_corners": int(config.get("max_empty_corners_allowed", 4)),
            "fossil_integrity": fossil_integrity,
            "overlay_integrity": overlay_integrity,
            "protected": protected,
            "rules": overlay_rules,
        })
    if not configs:
        raise ValueError("no minecraft:fossil configured features in the pack")

    refs = []
    rules = []
    rows = []
    for config in configs:
        structure_offset = len(refs)
        refs.extend(config["structures"])
        overlay_offset = len(refs)
        refs.extend(config["overlays"])
        rule_offset = len(rules)
        rules.extend(config["rules"])
        rows.append((config, structure_offset, overlay_offset, rule_offset))

    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// Generated by tools/gen_worldgen_registry_table.py — do not edit by hand.")
    out.append("//")
    out.append("// The pack's fossil features and the structure templates they place: the")
    out.append(f"// {len(templates)} `structure/fossil/*.nbt` templates resolved to their block lists")
    out.append("// (in the template's own block order, which the rot processor's random")
    out.append("// draws follow), the two configured features' processor integrities read")
    out.append("// from the pack's own processor lists, and the `features_cannot_replace`")
    out.append("// blocks the protected-block processor reads. This is the fossil-shaped")
    out.append("// half of the template engine the carried structures need (docs/PHASE4.md).")
    out.append("// clang-format off")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append('#include "world/worldgen_feature_table.h" // WgState')
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("struct WgFossilBlock { int16_t x; int16_t y; int16_t z; uint16_t state; };")
    out.append("struct WgFossilTemplate {")
    out.append("  const char* name;")
    out.append("  int16_t size_x;")
    out.append("  int16_t size_y;")
    out.append("  int16_t size_z;")
    out.append("  uint32_t block_offset;")
    out.append("  uint32_t block_count;")
    out.append("};")
    out.append("")
    out.append("// A `RuleProcessor` rule of the one shape the pack's fossil overlays use:")
    out.append("// a `block_match` on the template block's own state, an always-true")
    out.append("// location predicate, and the state the block becomes.")
    out.append("struct WgFossilRule { const char* input_block; WgState output; };")
    out.append("struct WgFossilConfig {")
    out.append("  uint32_t structure_offset; // into kWgFossilTemplateRefs")
    out.append("  uint32_t structure_count;")
    out.append("  uint32_t overlay_offset;")
    out.append("  uint32_t overlay_count;")
    out.append("  uint32_t rule_offset; // into kWgFossilRules")
    out.append("  uint32_t rule_count;")
    out.append("  uint32_t max_empty_corners;")
    out.append("  float structure_integrity;")
    out.append("  float overlay_integrity;")
    out.append("  const char* protected_blocks; // '|' list, resolved")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgState kWgFossilStates[{max(len(states), 1)}] = {{")
    for name, props in states:
        out.append(f"    {{{cstr(name)}, {cstr(props)}}},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgFossilBlock kWgFossilBlocks[{max(len(blocks), 1)}] = {{")
    for template in templates:
        out.append(f"    // {template['name']}")
        start = template["offset"]
        for x, y, z, state in blocks[start:start + template["blocks"]]:
            out.append(f"    {{{x}, {y}, {z}, {state}}},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgFossilTemplate kWgFossilTemplates[{len(templates)}] = {{")
    for template in templates:
        sx, sy, sz = template["size"]
        out.append(f"    {{{cstr(template['name'])}, {sx}, {sy}, {sz}, "
                   f"{template['offset']}, {template['blocks']}}},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr uint32_t kWgFossilTemplateRefs[{len(refs)}] = {{")
    for index in refs:
        out.append(f"    {index},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgFossilRule kWgFossilRules[{max(len(rules), 1)}] = {{")
    for input_block, output in rules:
        out.append(f"    {{{cstr(input_block)}, {{{cstr(output[0])}, {cstr(output[1])}}}}},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgFossilConfig kWgFossilConfigs[{len(rows)}] = {{")
    for config, structure_offset, overlay_offset, rule_offset in rows:
        protected = "|".join(config["protected"])
        out.append(f"    // {config['name']}")
        out.append(f"    {{{structure_offset}, {len(config['structures'])}, "
                   f"{overlay_offset}, {len(config['overlays'])}, "
                   f"{rule_offset}, {len(config['rules'])}, "
                   f"{config['max_empty_corners']}, "
                   f"{_float_literal(config['fossil_integrity'])}, "
                   f"{_float_literal(config['overlay_integrity'])}, "
                   f"{cstr(protected)}}},")
    out.append("};")
    out.append("")
    out.append("}  // namespace mc::world")
    out.append("// clang-format on")
    return "\n".join(out) + "\n"


# --- structure pieces -------------------------------------------------------
#
# The first structure *piece* kind this engine builds is `ShipwreckStructure`:
# one `TemplateStructurePiece` per start, placed through
# `StructureTemplate.placeInWorld`. The templates live in
# `structure/shipwreck/*.nbt`; this section resolves them the way the fossil
# emitter does, plus the palettes (each shipwreck carries eight material
# variants of the same block list) and the two template lists the structure's
# Java picks from.
#
# The two lists are code, not data: they are `ShipwreckPieces`' own arrays, in
# their own order, because `Util.getRandom` indexes them with `nextInt` and a
# re-ordered list would silently choose another ship. They are transcribed
# here with the ids the game writes (`minecraft:shipwreck/<name>`), and the
# emitter refuses a pack that is missing one.

_SHIPWRECK_OCEAN = [
    "with_mast",
    "upsidedown_full",
    "upsidedown_fronthalf",
    "upsidedown_backhalf",
    "sideways_full",
    "sideways_fronthalf",
    "sideways_backhalf",
    "rightsideup_full",
    "rightsideup_fronthalf",
    "rightsideup_backhalf",
    "with_mast_degraded",
    "upsidedown_full_degraded",
    "upsidedown_fronthalf_degraded",
    "upsidedown_backhalf_degraded",
    "sideways_full_degraded",
    "sideways_fronthalf_degraded",
    "sideways_backhalf_degraded",
    "rightsideup_full_degraded",
    "rightsideup_fronthalf_degraded",
    "rightsideup_backhalf_degraded",
]
_SHIPWRECK_BEACHED = [
    "with_mast",
    "sideways_full",
    "sideways_fronthalf",
    "sideways_backhalf",
    "rightsideup_full",
    "rightsideup_fronthalf",
    "rightsideup_backhalf",
    "with_mast_degraded",
    "rightsideup_full_degraded",
    "rightsideup_fronthalf_degraded",
    "rightsideup_backhalf_degraded",
]

# `IglooPieces.PIVOTS`, keyed by the template name without its namespace.
_IGLOO_PIVOTS = {
    "top": (3, 5, 5),
    "middle": (1, 3, 1),
    "bottom": (3, 6, 7),
}

# `Direction` ordinals as they appear in a jigsaw block's `orientation`.
_DIRECTIONS = {"down": 0, "up": 1, "north": 2, "south": 3, "west": 4, "east": 5}


def emit_structure_piece_table(pack):
    """`worldgen_structure_piece_table.h`: every template the piece builders use.

    Shipwrecks and igloos keep their own lists; ocean ruins, ruined portals,
    woodland mansions and villages are emitted from their whole directories.
    Each template carries its block list, palettes, rotation pivot,
    `structure_block` marker names and jigsaw connectors. The village template
    pools and their rule processors are emitted beside the templates so the
    jigsaw assembler is data-driven.
    """
    groups = [
        ("shipwreck", "minecraft:shipwreck/", list(dict.fromkeys(_SHIPWRECK_OCEAN + _SHIPWRECK_BEACHED)),
         {stem: (4, 0, 15) for stem in _SHIPWRECK_OCEAN + _SHIPWRECK_BEACHED}),
        ("igloo", "minecraft:igloo/", list(_IGLOO_PIVOTS), _IGLOO_PIVOTS),
        ("underwater_ruin", "minecraft:underwater_ruin/", None, {}),
        ("ruined_portal", "minecraft:ruined_portal/", None, {}),
        ("woodland_mansion", "minecraft:woodland_mansion/", None, {}),
        ("village", "minecraft:village/", None, {}),
        ("pillager_outpost", "minecraft:pillager_outpost/", None, {}),
        ("ancient_city", "minecraft:ancient_city/", None, {}),
        ("trail_ruins", "minecraft:trail_ruins/", None, {}),
        ("bastion", "minecraft:bastion/", None, {}),
        ("end_city", "minecraft:end_city/", None, {}),
    ]

    states = []
    state_index = {}
    blocks = []
    templates = []
    palettes = []
    palette_states = []
    markers = []
    marker_index = {}
    loot_tables = []
    loot_index = {}
    jigsaws = []
    jigsaw_strings = []
    jigsaw_string_index = {}
    jigsaw_final_states = []
    jigsaw_final_index = {}

    def intern_string(value):
        index = jigsaw_string_index.get(value)
        if index is None:
            index = len(jigsaw_strings)
            jigsaw_string_index[value] = index
            jigsaw_strings.append(value)
        return index

    def intern_final(value):
        index = jigsaw_final_index.get(value)
        if index is None:
            index = len(jigsaw_final_states)
            jigsaw_final_index[value] = index
            jigsaw_final_states.append(value)
        return index

    def intern_loot(value):
        index = loot_index.get(value)
        if index is None:
            index = len(loot_tables)
            loot_index[value] = index
            loot_tables.append(value)
        return index

    # `FeaturePoolElement.fillDefaultJigsawNBT` names these three; interning them
    # unconditionally means the runtime can always build a feature element's
    # synthesised connector out of the table instead of guessing an index.
    for _default_jigsaw_string in ("minecraft:bottom", "minecraft:empty", "minecraft:air"):
        intern_string(_default_jigsaw_string)

    pool_element_features = []
    pool_element_feature_index = {}

    def intern_feature(value):
        index = pool_element_feature_index.get(value)
        if index is None:
            index = len(pool_element_features)
            pool_element_feature_index[value] = index
            pool_element_features.append(value)
        return index

    def orientation(value):
        parts = value.split("_")
        if len(parts) != 2 or parts[0] not in _DIRECTIONS or parts[1] not in _DIRECTIONS:
            raise ValueError(f"unsupported jigsaw orientation {value!r}")
        return _DIRECTIONS[parts[0]], _DIRECTIONS[parts[1]]

    for directory, prefix, wanted, pivots in groups:
        base = pack.parent / "structure" / directory
        if not base.is_dir():
            raise ValueError(f"no {directory} templates at {base}")
        if wanted is None:
            wanted = sorted(p.relative_to(base).with_suffix("").as_posix()
                            for p in base.rglob("*.nbt"))
        for stem in wanted:
            path = base / f"{stem}.nbt"
            if not path.is_file():
                raise ValueError(f"{directory} template {path} is missing")
            doc = _nbt_read(path)
            size = [int(v) for v in doc["size"]]
            # The writer clips every cell to the target chunk, so a template's
            # size is bounded by nothing but the signed 16-bit offsets the
            # generated table stores. Keep a generous ceiling so a corrupt or
            # unexpected template fails generation instead of wrapping.
            if max(size) > 48:
                raise ValueError(f"{stem}: {size[0]}x{size[1]}x{size[2]} exceeds the "
                                 "48-block template ceiling")
            raw_palettes = doc.get("palettes") or ([doc.get("palette")]
                                                    if doc.get("palette") else None)
            if not raw_palettes:
                raise ValueError(f"{stem}: no `palette` or `palettes`")
            rows = []
            for entries in raw_palettes:
                row = []
                for entry in entries:
                    key = (entry["Name"], property_suffix(entry.get("Properties") or {}))
                    index = state_index.get(key)
                    if index is None:
                        index = len(states)
                        state_index[key] = index
                        states.append(key)
                    row.append(index)
                rows.append(row)
            width = len(rows[0])
            if any(len(row) != width for row in rows):
                raise ValueError(f"{stem}: palettes have different sizes")
            block_offset = len(blocks)
            jigsaw_offset = len(jigsaws)
            for entry in doc.get("blocks", []):
                x, y, z = [int(v) for v in entry["pos"]]
                if not (0 <= x < size[0] and 0 <= y < size[1] and 0 <= z < size[2]):
                    raise ValueError(f"{stem}: block {(x, y, z)} outside {size}")
                state = int(entry.get("state", 0))
                if state >= width:
                    raise ValueError(f"{stem}: block state {state} outside {width}-entry palette")
                data = entry.get("nbt") or {}
                metadata = data.get("metadata")
                marker = 0
                if metadata:
                    marker = marker_index.get(metadata)
                    if marker is None:
                        marker = len(markers)
                        marker_index[metadata] = marker
                        markers.append(metadata)
                    marker += 1  # 0 means "no marker"
                # A template chest/barrel/dispenser carries its table directly on
                # the block entity (`LootTable` in the NBT) rather than a
                # `structure_block` marker. Keeping it here is what lets a
                # village or ancient-city chest arrive with contents.
                loot_table = intern_loot(data["LootTable"]) if data.get("LootTable") else 0xFFFF
                blocks.append((x, y, z, state, marker, loot_table))
                if data.get("id") == "minecraft:jigsaw":
                    props = states[rows[0][state]][1]
                    orientation_value = None
                    for part in props.split(","):
                        if part.startswith("orientation="):
                            orientation_value = part.split("=", 1)[1]
                    if orientation_value is None:
                        raise ValueError(f"{stem}: jigsaw at {(x, y, z)} has no orientation")
                    front, top = orientation(orientation_value)
                    joint = 0 if data.get("joint", "rollable") == "rollable" else 1
                    jigsaws.append((
                        x, y, z, front, top, joint,
                        intern_string(data["name"]),
                        intern_string(data["target"]),
                        intern_string(data["pool"]),
                        intern_final(data.get("final_state", "minecraft:air")),
                        int(data.get("placement_priority", 0)),
                        int(data.get("selection_priority", 0)),
                    ))
            palette_offset = len(palettes)
            for row in rows:
                palettes.append((len(palette_states), len(row)))
                palette_states.extend(row)
            pivot = pivots.get(stem, (0, 0, 0))
            templates.append({
                "name": prefix + stem,
                "size": size,
                "pivot": pivot,
                "palette_offset": palette_offset,
                "palette_count": len(rows),
                "block_offset": block_offset,
                "block_count": len(blocks) - block_offset,
                "jigsaw_offset": jigsaw_offset,
                "jigsaw_count": len(jigsaws) - jigsaw_offset,
            })
    template_index = {entry["name"]: index for index, entry in enumerate(templates)}

    def ensure_template(name):
        """The index of a pool reference, adding vanilla's empty template.

        A few packs reference a template the jar does not ship (26.2's
        `ancient_city/walls/no_corners` names
        `intact_horizontal_wall_stairs_5`). Vanilla's `StructureTemplateManager`
        answers an empty `StructureTemplate`, so the pool weighted draw still
        reaches it and the piece occupies `pos - 1 .. pos`. Recording that as a
        zero-size row is the same behaviour; dropping the element would move
        every later draw in the pool.
        """
        if name in template_index:
            return template_index[name]
        print(f"note: {name} has no template; emitting the empty template", file=sys.stderr)
        templates.append({
            "name": name,
            "size": [0, 0, 0],
            "pivot": (0, 0, 0),
            "palette_offset": 0,
            "palette_count": 0,
            "block_offset": len(blocks),
            "block_count": 0,
            "jigsaw_offset": len(jigsaws),
            "jigsaw_count": 0,
        })
        template_index[name] = len(templates) - 1
        return template_index[name]

    def refs(prefix, names):
        out = []
        for name in names:
            key = prefix + name
            if key not in template_index:
                raise ValueError(f"piece list names unknown template {key}")
            out.append(template_index[key])
        return out

    ocean = refs("minecraft:shipwreck/", _SHIPWRECK_OCEAN)
    beached = refs("minecraft:shipwreck/", _SHIPWRECK_BEACHED)
    igloo = refs("minecraft:igloo/", list(_IGLOO_PIVOTS))

    # Template pools of the applied jigsaw families. Weighted elements are
    # expanded exactly as `StructureTemplatePool` expands them; empty/feature
    # elements are kept as zero-size pieces so a jigsaw can still connect to
    # them. A `list_pool_element` is flattened into its inner elements (the
    # list's projection replaces theirs, as `ListPoolElement` does), so the
    # assembler sees the same weighted candidates vanilla's pool does.
    pool_roots = [
        ("minecraft:village/", pack.parent / "worldgen" / "template_pool" / "village"),
        ("minecraft:pillager_outpost/",
         pack.parent / "worldgen" / "template_pool" / "pillager_outpost"),
        ("minecraft:ancient_city/",
         pack.parent / "worldgen" / "template_pool" / "ancient_city"),
        ("minecraft:trail_ruins/",
         pack.parent / "worldgen" / "template_pool" / "trail_ruins"),
        ("minecraft:bastion/",
         pack.parent / "worldgen" / "template_pool" / "bastion"),
    ]
    pool_names = ["minecraft:empty"]
    for prefix, pool_root in pool_roots:
        pool_names += [prefix + p.relative_to(pool_root).with_suffix("").as_posix()
                       for p in sorted(pool_root.rglob("*.json"))]
    pool_names = sorted(set(pool_names))
    pool_index = {name: index for index, name in enumerate(pool_names)}
    processor_root = pack.parent / "worldgen" / "processor_list"
    # `processor_id = 0` is the pool element's "no processors" value, so the
    # empty list must keep index 0; sorting the whole set would put
    # `ancient_city_generic_degradation` there.
    processor_names = ["minecraft:empty"]
    processor_names += sorted({"minecraft:" + p.relative_to(processor_root).with_suffix("").as_posix()
                               for p in processor_root.glob("*.json")} - {"minecraft:empty"})
    processor_index = {name: index for index, name in enumerate(processor_names)}

    block_tags = BlockTags(pack.parent)
    processor_rows = []
    processor_lists = []
    processor_unsupported = []
    processor_blocks = []
    # One `capped` pass: the delegate rules plus the per-piece replacement
    # budget. `CappedProcessor.finalizeProcessing` runs after every per-block
    # processor, so the table keeps these separate from `processor_rows`' list
    # order and applies them in the pass order recorded here.
    processor_capped = []

    def emit_rules(rules):
        start = len(processor_rows)
        for rule in rules:
            input_predicate = rule["input_predicate"]
            input_kind = input_predicate.get("predicate_type", "")
            input_blocks = []
            if input_kind == "minecraft:block_match":
                predicate, value = 0, input_predicate["block"]
            elif input_kind == "minecraft:random_block_match":
                predicate, value = 1, input_predicate["block"]
            elif input_kind == "minecraft:blockstate_match":
                block_state = input_predicate["block_state"]
                value = state_text(block_state["Name"],
                                   block_state.get("Properties") or {})
                predicate = 3
            elif input_kind == "minecraft:tag_match":
                # `TagMatchTest` is a membership question with no state
                # component. Resolve the tag now, exactly as the rot and
                # protected lists are resolved, rather than carrying a block
                # tag registry the piece table has no other use for. The
                # predicate's `tag` field is written without the leading `#`
                # (`rottable_blocks` has it); only strip one when present.
                predicate, value = 2, input_predicate["tag"]
                tag_name = value[1:] if value.startswith("#") else value
                input_blocks = block_tags.resolve(tag_name)
                if not input_blocks:
                    raise ValueError(f"unknown or empty block tag #{tag_name}")
            else:
                continue
            probability = float(input_predicate.get("probability", 1.0))
            location = rule.get("location_predicate", {})
            location_kind = 1 if location.get("predicate_type") == "minecraft:block_match" else 0
            location_name = location.get("block", "") if location_kind else ""
            output = rule["output_state"]
            state_key = (output["Name"], property_suffix(output.get("Properties") or {}))
            state_index_value = state_index.get(state_key)
            if state_index_value is None:
                state_index_value = len(states)
                state_index[state_key] = state_index_value
                states.append(state_key)
            # `append_loot` is the only block-entity modifier the applied
            # families use: the rule stores a table on the written block (trail
            # ruins' suspicious gravel/sand). `passthrough` (the default) and
            # `clear` carry none.
            loot_table = 0xFFFF
            modifier = rule.get("block_entity_modifier") or {}
            if modifier.get("type") == "minecraft:append_loot":
                loot_table = intern_loot(modifier["loot_table"])
            input_block_offset = len(processor_blocks)
            processor_blocks.extend(input_blocks)
            processor_rows.append((predicate, intern_string(value), state_index_value,
                                   probability, location_kind, intern_string(location_name),
                                   loot_table, input_block_offset, len(input_blocks)))
        return start, len(processor_rows) - start

    for name in processor_names:
        if name == "minecraft:empty":
            processor_lists.append((0, 0, 1.0, 0, 0, 0, 0, 0, 0))
            processor_unsupported.append(False)
            continue
        path = processor_root / (name[len("minecraft:"):] + ".json")
        doc = load(path)
        start = len(processor_rows)
        integrity = None
        rottable = []
        protected = []
        has_rules = False
        unsupported = False
        capped_start = len(processor_capped)
        # Capped delegates are finalize passes, so their rules must not join
        # the list's per-block rule range: `structure_piece.cpp` applies
        # `rule_offset..+rule_count` to every cell. Keep them out of that range
        # and emit them after it, preserving their order in `processor_capped`.
        capped_delegates = []
        rule_split = start
        for processor in doc.get("processors", []):
            kind = processor.get("processor_type")
            if kind == "minecraft:block_rot":
                # `BlockRotProcessor` drops a block when the per-position draw
                # is above the integrity. The piece table carries one list-level
                # value and applies it before the rule pass; the ancient-city
                # lists put it first too. `rottable_blocks`, when present, is
                # the tag the state must belong to for the draw to apply.
                if integrity is not None or has_rules:
                    unsupported = True
                integrity = float(processor.get("integrity", 1.0))
                raw = processor.get("rottable_blocks")
                if raw is not None:
                    rottable = (block_tags.resolve(raw[1:]) if isinstance(raw, str)
                                and raw.startswith("#") else [raw])
                continue
            if kind == "minecraft:protected_blocks":
                # `ProtectedBlockProcessor` skips a write when the *world* cell
                # is in this set. The ancient-city lists end with it.
                value = processor.get("value", "")
                protected = (block_tags.resolve(value[1:]) if value.startswith("#")
                             else [value])
                continue
            if kind == "minecraft:capped":
                delegate = processor.get("delegate") or {}
                if delegate.get("processor_type") != "minecraft:rule":
                    unsupported = True
                    continue
                limit = processor.get("limit")
                if isinstance(limit, dict):
                    if limit.get("type") != "minecraft:constant":
                        unsupported = True
                        continue
                    limit = limit["value"]
                capped_delegates.append((delegate.get("rules", []), int(limit)))
                continue
            if kind != "minecraft:rule":
                unsupported = True
                continue
            # `block_rot` + rules is the ancient-city shape; the table applies
            # the rot first, which is the order those lists write it in. A list
            # that puts a rule before `block_rot` is rejected above.
            has_rules = True
            emit_rules(processor.get("rules", []))
        rule_split = len(processor_rows)
        for rules, limit in capped_delegates:
            rule_offset, rule_count = emit_rules(rules)
            processor_capped.append((rule_offset, rule_count, limit, limit))
        rot_offset = len(processor_blocks)
        for name in rottable:
            processor_blocks.append(name)
        protect_offset = len(processor_blocks)
        for name in protected:
            processor_blocks.append(name)
        processor_lists.append(
            (start, rule_split - start, 1.0 if integrity is None else integrity,
             rot_offset, len(rottable), protect_offset, len(protected),
             capped_start, len(processor_capped) - capped_start))
        processor_unsupported.append(unsupported)

    pool_elements = []
    pool_element_parts = []
    pools = []
    used_processor_lists = set()
    for name in pool_names:
        if name == "minecraft:empty":
            pools.append((pool_index["minecraft:empty"], 0, 0))
            continue
        path = pack.parent / "worldgen" / "template_pool" / (name[len("minecraft:"):] + ".json")
        doc = load(path)
        fallback = doc.get("fallback", "minecraft:empty")
        if fallback not in pool_index:
            raise ValueError(f"{name}: unknown fallback {fallback}")
        start = len(pool_elements)

        def element_part(element):
            """`(kind, template, processor)` for one non-list pool element."""
            kind_name = element.get("element_type")
            processor = element.get("processors")
            processor_id = 0
            if isinstance(processor, str):
                if processor not in processor_index:
                    raise ValueError(f"{name}: unknown processor list {processor}")
                processor_id = processor_index[processor]
            elif isinstance(processor, dict) and processor.get("processors"):
                raise ValueError(f"{name}: inline processors are unsupported")
            if kind_name in ("minecraft:single_pool_element", "minecraft:legacy_single_pool_element"):
                location = element["location"]
                kind = 1 if kind_name == "minecraft:single_pool_element" else 2
                return (kind, ensure_template(location), processor_id)
            return (0, 0xFFFF, 0)

        def element_rows(element, weight):
            """Rows for one pool element, keeping a `list_pool_element` whole.

            `ListPoolElement` is one piece that places every inner element in
            order; flattening it into separate candidates would change both
            the assembly draw and the cells (the towers pool's overgrown
            watchtower is the second of two such templates). It is stored as
            kind 3 plus a part range, and its projection is the list's own
            (the constructor applies it to every inner element).
            """
            kind_name = element.get("element_type")
            projection = 0 if element.get("projection") == "rigid" else 1
            # `FeaturePoolElement` is a real candidate: a 1 x 1 x 1 piece whose
            # one connector is synthesised. Folding it into the empty element
            # dropped the decoration and shortened village starts by a piece.
            if kind_name == "minecraft:feature_pool_element":
                feature = element["feature"]
                return [(4, projection, 0xFFFF, 0, 0, 0, intern_feature(feature))] * weight
            if kind_name == "minecraft:list_pool_element":
                inner = element.get("elements", [])
                if any(part.get("element_type") == "minecraft:list_pool_element" for part in inner):
                    raise ValueError(f"{name}: nested list_pool_element is unsupported")
                parts = [element_part(part) for part in inner]
                if not parts:
                    raise ValueError(f"{name}: empty list_pool_element")
                for _, _, processor_id in parts:
                    if processor_id:
                        used_processor_lists.add(processor_id)
                offset = len(pool_element_parts)
                pool_element_parts.extend(parts)
                first_template = parts[0][1]
                row = (3, projection, first_template, 0, offset, len(parts), 0xFFFF)
            else:
                kind, template, processor_id = element_part(element)
                if processor_id:
                    used_processor_lists.add(processor_id)
                row = (kind, projection, template, processor_id, 0, 0, 0xFFFF)
            return [row] * weight

        for entry in doc.get("elements", []):
            pool_elements.extend(element_rows(entry["element"], int(entry["weight"])))
        pools.append((pool_index[fallback], start, len(pool_elements) - start))

    for processor_id in sorted(used_processor_lists):
        if processor_unsupported[processor_id]:
            raise ValueError(f"{processor_names[processor_id]}: unsupported processor mix used "
                             "by an applied template pool")

    # Ruined portal setups and ocean-ruin configs, read from the structure JSON.
    structure_root = pack.parent / "worldgen" / "structure"
    placements = ["on_land_surface", "partly_buried", "on_ocean_floor",
                  "in_mountain", "underground", "in_nether"]
    portal_configs = []
    for name in ("ruined_portal", "ruined_portal_desert", "ruined_portal_jungle",
                 "ruined_portal_mountain", "ruined_portal_nether", "ruined_portal_ocean",
                 "ruined_portal_swamp"):
        doc = load(structure_root / (name + ".json"))
        start = len([row for row in portal_configs])
        rows = []
        for setup in doc.get("setups", []):
            rows.append((placements.index(setup["placement"]),
                         float(setup["air_pocket_probability"]),
                         float(setup["mossiness"]),
                         1 if setup["overgrown"] else 0,
                         1 if setup["vines"] else 0,
                         1 if setup["can_be_cold"] else 0,
                         1 if setup["replace_with_blackstone"] else 0,
                         float(setup["weight"])))
        portal_configs.append((f"minecraft:{name}", rows))
    portal_setup_rows = []
    portal_rows = []
    for name, rows in portal_configs:
        portal_rows.append((name, len(portal_setup_rows), len(rows)))
        portal_setup_rows.extend(rows)

    ocean_rows = []
    for name in ("ocean_ruin_cold", "ocean_ruin_warm"):
        doc = load(structure_root / (name + ".json"))
        ocean_rows.append((f"minecraft:{name}", 1 if doc["biome_temp"] == "cold" else 0,
                           float(doc["large_probability"]), float(doc["cluster_probability"])))

    jigsaw_rows = []
    jigsaw_configs = [
        ("minecraft:village_plains", "village_plains", "minecraft:village/plains/town_centers"),
        ("minecraft:village_desert", "village_desert", "minecraft:village/desert/town_centers"),
        ("minecraft:village_savanna", "village_savanna", "minecraft:village/savanna/town_centers"),
        ("minecraft:village_snowy", "village_snowy", "minecraft:village/snowy/town_centers"),
        ("minecraft:village_taiga", "village_taiga", "minecraft:village/taiga/town_centers"),
        ("minecraft:pillager_outpost", "pillager_outpost",
         "minecraft:pillager_outpost/base_plates"),
        ("minecraft:ancient_city", "ancient_city",
         "minecraft:ancient_city/city_center"),
        ("minecraft:trail_ruins", "trail_ruins", "minecraft:trail_ruins/tower"),
        ("minecraft:bastion_remnant", "bastion_remnant", "minecraft:bastion/starts"),
    ]
    for name, structure_file, pool in jigsaw_configs:
        if pool not in pool_index:
            raise ValueError(f"jigsaw start pool missing: {pool}")
        structure_doc = load(structure_root / (structure_file + ".json"))
        start_height = structure_doc["start_height"]
        if "absolute" in start_height:
            height_kind = 0
            height_min = height_max = int(start_height["absolute"])
        elif start_height.get("type") == "minecraft:uniform":
            # `HeightProvider.sample` draws one value from the context random,
            # before the rotation draw. Both bounds in these packs are absolute.
            height_kind = 1
            height_min = int(start_height["min_inclusive"]["absolute"])
            height_max = int(start_height["max_inclusive"]["absolute"])
        else:
            raise ValueError(f"{name}: unsupported start_height {start_height!r}")
        project = structure_doc.get("project_start_to_heightmap")
        if project not in (None, "WORLD_SURFACE_WG"):
            raise ValueError(f"{name}: unsupported project_start_to_heightmap {project!r}")
        max_distance = structure_doc["max_distance_from_center"]
        if isinstance(max_distance, dict):
            max_distance = int(max_distance["horizontal"])
        start_jigsaw = structure_doc.get("start_jigsaw_name")
        jigsaw_rows.append((name, pool_index[pool], int(structure_doc["size"]), height_kind,
                            height_min, height_max, 1 if project else 0,
                            1 if structure_doc.get("use_expansion_hack") else 0,
                            int(max_distance),
                            intern_string(start_jigsaw) if start_jigsaw else 0xFFFF))

    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// Generated by tools/gen_worldgen_registry_table.py — do not edit by hand.")
    out.append("//")
    out.append("// Every structure template the piece builders use: the twenty")
    out.append("// `structure/shipwreck/*.nbt`, the three `structure/igloo/*.nbt`, the")
    out.append("// ocean-ruin, ruined-portal, woodland-mansion, village, pillager-outpost,")
    out.append("// ancient-city, trail-ruins, bastion and end-city templates.")
    out.append("// Each row carries the")
    out.append("// palette-local block list, the template pivot and its jigsaw connectors.")
    out.append("// `kWgJigsawStrings` interns names/targets/pools and")
    out.append("// `kWgJigsawFinalStates` interns the replacement states. The village and")
    out.append("// outpost template pools and the rule/block_rot processor lists follow")
    out.append("// the templates.")
    out.append("// clang-format off")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append('#include "world/worldgen_feature_table.h" // WgState')
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("struct WgPieceBlock {")
    out.append("  int16_t x;")
    out.append("  int16_t y;")
    out.append("  int16_t z;")
    out.append("  uint16_t state;")
    out.append("  uint8_t marker; // 0 none, otherwise 1 + an index into kWgPieceMarkers")
    out.append("  // 0xFFFF none, otherwise an index into kWgPieceLootTables: the")
    out.append("  // block entity's own `LootTable` (a village or ancient-city chest).")
    out.append("  uint16_t loot_table;")
    out.append("};")
    out.append("struct WgPiecePalette { uint32_t state_offset; uint16_t state_count; };")
    out.append(f"inline constexpr const char* kWgPieceLootTables[{max(1, len(loot_tables))}] = {{")
    if loot_tables:
        for table in loot_tables:
            out.append(f"    {cstr(table)},")
    else:
        out.append("    nullptr,")
    out.append("};\n")
    out.append("struct WgPieceJigsaw {")
    out.append("  int16_t x;")
    out.append("  int16_t y;")
    out.append("  int16_t z;")
    out.append("  uint8_t front; // 0 down, 1 up, 2 north, 3 south, 4 west, 5 east")
    out.append("  uint8_t top;")
    out.append("  uint8_t joint; // 0 rollable, 1 aligned")
    out.append("  uint16_t name;")
    out.append("  uint16_t target;")
    out.append("  uint16_t pool;")
    out.append("  uint16_t final_state;")
    out.append("  int16_t placement_priority;")
    out.append("  int16_t selection_priority;")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr const char* kWgPieceMarkers[{max(1, len(markers))}] = {{")
    if markers:
        for marker in markers:
            out.append(f"    {cstr(marker)},")
    else:
        out.append("    nullptr,")
    out.append("};")
    out.append("struct WgPieceTemplate {")
    out.append("  const char* name;")
    out.append("  int16_t size_x;")
    out.append("  int16_t size_y;")
    out.append("  int16_t size_z;")
    out.append("  int16_t pivot_x;")
    out.append("  int16_t pivot_y;")
    out.append("  int16_t pivot_z;")
    out.append("  uint32_t palette_offset;")
    out.append("  uint8_t palette_count;")
    out.append("  uint32_t block_offset;")
    out.append("  uint32_t block_count;")
    out.append("  uint32_t jigsaw_offset;")
    out.append("  uint16_t jigsaw_count;")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgState kWgPieceStates[{len(states)}] = {{")
    for name, props in states:
        out.append(f"    {{{cstr(name)}, {cstr(props)}}},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgPieceBlock kWgPieceBlocks[{len(blocks)}] = {{")
    for template in templates:
        out.append(f"    // {template['name']}")
        start = template["block_offset"]
        for x, y, z, state, marker, loot in blocks[start:start + template["block_count"]]:
            loot_literal = "0xFFFF" if loot == 0xFFFF else str(loot)
            out.append(f"    {{{x}, {y}, {z}, {state}, {marker}, {loot_literal}}},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr uint16_t kWgPiecePaletteStates[{len(palette_states)}] = {{")
    for index in palette_states:
        out.append(f"    {index},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgPiecePalette kWgPiecePalettes[{len(palettes)}] = {{")
    for offset, count in palettes:
        out.append(f"    {{{offset}, {count}}},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr const char* kWgJigsawStringTable[{max(1, len(jigsaw_strings))}] = {{")
    if jigsaw_strings:
        for value in jigsaw_strings:
            out.append(f"    {cstr(value)},")
    else:
        out.append("    nullptr,")
    out.append("};")
    out.append(f"inline constexpr const char* kWgJigsawFinalStates[{max(1, len(jigsaw_final_states))}] = {{")
    if jigsaw_final_states:
        for value in jigsaw_final_states:
            out.append(f"    {cstr(value)},")
    else:
        out.append("    nullptr,")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgPieceJigsaw kWgPieceJigsaws[{max(1, len(jigsaws))}] = {{")
    if jigsaws:
        for row in jigsaws:
            out.append(f"    {{{row[0]}, {row[1]}, {row[2]}, {row[3]}, {row[4]}, {row[5]}, "
                       f"{row[6]}, {row[7]}, {row[8]}, {row[9]}, {row[10]}, {row[11]}}},")
    else:
        out.append("    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgPieceTemplate kWgPieceTemplates[{len(templates)}] = {{")
    for template in templates:
        sx, sy, sz = template["size"]
        px, py, pz = template["pivot"]
        out.append(f"    {{{cstr(template['name'])}, {sx}, {sy}, {sz}, "
                   f"{px}, {py}, {pz}, "
                   f"{template['palette_offset']}, {template['palette_count']}, "
                   f"{template['block_offset']}, {template['block_count']}, "
                   f"{template['jigsaw_offset']}, {template['jigsaw_count']}}},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr uint8_t kWgShipwreckOcean[{len(ocean)}] = {{")
    for index in ocean:
        out.append(f"    {index},")
    out.append("};")
    out.append(f"inline constexpr uint8_t kWgShipwreckBeached[{len(beached)}] = {{")
    for index in beached:
        out.append(f"    {index},")
    out.append("};")
    out.append(f"inline constexpr uint8_t kWgIglooTemplates[{len(igloo)}] = {{")
    for index in igloo:
        out.append(f"    {index},")
    out.append("};")
    out.append("")
    out.append("struct WgPieceProcessorRule {")
    out.append("  uint8_t input_kind; // 0 block, 1 random block, 2 tag, 3 blockstate")
    out.append("  uint16_t input;")
    out.append("  uint16_t output_state;")
    out.append("  float probability;")
    out.append("  uint8_t location_kind; // 0 always, 1 block match")
    out.append("  uint16_t location;")
    out.append("  // `append_loot`: the table written onto the rule's output block, or")
    out.append("  // 0xFFFF for a plain `passthrough`/`clear` rule.")
    out.append("  uint16_t loot_table;")
    out.append("  // `tag_match`: the resolved member blocks. Empty for other kinds.")
    out.append("  uint32_t input_block_offset;")
    out.append("  uint16_t input_block_count;")
    out.append("};")
    out.append("struct WgPieceCappedPass {")
    out.append("  uint32_t rule_offset;")
    out.append("  uint16_t rule_count;")
    out.append("  uint16_t limit_min; // constant providers in this pack")
    out.append("  uint16_t limit_max;")
    out.append("};")
    out.append("struct WgPieceProcessorList {")
    out.append("  uint32_t rule_offset;")
    out.append("  uint16_t rule_count;")
    out.append("  // `BlockRotProcessor`'s integrity: a block is dropped when the")
    out.append("  // per-position draw is above this. 1.0 when the list has none.")
    out.append("  float block_rot_integrity;")
    out.append("  // `BlockRotProcessor.rottable_blocks`: the state must be one of these")
    out.append("  // for the draw to apply; empty means every state.")
    out.append("  uint32_t rot_block_offset;")
    out.append("  uint16_t rot_block_count;")
    out.append("  // `ProtectedBlockProcessor`: a write is skipped when the world cell")
    out.append("  // already holds one of these blocks.")
    out.append("  uint32_t protected_block_offset;")
    out.append("  uint16_t protected_block_count;")
    out.append("  // `CappedProcessor` passes, in list order. Each shuffles the whole")
    out.append("  // piece's surviving blocks and applies its delegate to at most")
    out.append("  // `limit` of them; empty for a list without one.")
    out.append("  uint32_t capped_offset;")
    out.append("  uint16_t capped_count;")
    out.append("};")
    out.append(f"inline constexpr const char* kWgPieceProcessorBlocks[{max(1, len(processor_blocks))}] = {{")
    if processor_blocks:
        for name in processor_blocks:
            out.append(f"    {cstr(name)},")
    else:
        out.append("    nullptr,")
    out.append("};")
    out.append(f"inline constexpr WgPieceProcessorRule kWgPieceProcessorRules[{max(1, len(processor_rows))}] = {{")
    if processor_rows:
        for row in processor_rows:
            out.append(f"    {{{row[0]}, {row[1]}, {row[2]}, {_float_literal(row[3])}, "
                       f"{row[4]}, {row[5]}, {row[6]}, {row[7]}, {row[8]}}},")
    else:
        out.append("    {0, 0, 0, 0.0f, 0, 0, 65535, 0, 0},")
    out.append("};")
    out.append(f"inline constexpr WgPieceCappedPass kWgPieceCappedPasses[{max(1, len(processor_capped))}] = {{")
    if processor_capped:
        for rule_offset, rule_count, limit_min, limit_max in processor_capped:
            out.append(f"    {{{rule_offset}, {rule_count}, {limit_min}, {limit_max}}},")
    else:
        out.append("    {0, 0, 0, 0},")
    out.append("};")
    out.append(f"inline constexpr WgPieceProcessorList kWgPieceProcessorLists[{max(1, len(processor_lists))}] = {{")
    if processor_lists:
        for (offset, count, integrity, rot_offset, rot_count, protect_offset, protect_count,
             capped_offset, capped_count) in processor_lists:
            out.append(f"    {{{offset}, {count}, {_float_literal(integrity)}, "
                       f"{rot_offset}, {rot_count}, {protect_offset}, {protect_count}, "
                       f"{capped_offset}, {capped_count}}},")
    else:
        out.append("    {0, 0, 1.0f, 0, 0, 0, 0, 0, 0},")
    out.append("};")
    out.append("")
    out.append("struct WgPoolElementPart {")
    out.append("  uint8_t kind; // 0 empty, 1 single, 2 legacy single")
    out.append("  uint16_t template_index;")
    out.append("  uint16_t processor_list;")
    out.append("};")
    out.append(f"inline constexpr WgPoolElementPart kWgPoolElementParts[{max(1, len(pool_element_parts))}] = {{")
    if pool_element_parts:
        for kind, template, processor_id in pool_element_parts:
            out.append(f"    {{{kind}, {template}, {processor_id}}},")
    else:
        out.append("    {0, 65535, 0},")
    out.append("};")
    out.append("struct WgPoolElementRow {")
    out.append("  uint8_t kind; // 0 empty, 1 single, 2 legacy single, 3 list, 4 feature")
    out.append("  uint8_t projection; // 0 rigid, 1 terrain matching")
    out.append("  uint16_t template_index; // first part for a list; unused for a feature")
    out.append("  uint16_t processor_list;")
    out.append("  uint16_t part_offset; // kind 3 only")
    out.append("  uint16_t part_count;")
    out.append("  uint16_t feature_index; // kind 4 only: kWgPoolElementFeatures")

    out.append("};")
    out.append("struct WgTemplatePoolRow {")
    out.append("  uint16_t fallback;")
    out.append("  uint32_t element_offset;")
    out.append("  uint32_t element_count;")
    out.append("};")
    out.append(f"inline constexpr const char* kWgPoolElementFeatures[{max(1, len(pool_element_features))}] = {{")
    if pool_element_features:
        for value in pool_element_features:
            out.append(f"    {cstr(value)},")
    else:
        out.append("    nullptr,")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgPoolElementRow kWgPoolElements[{max(1, len(pool_elements))}] = {{")
    if pool_elements:
        for row in pool_elements:
            out.append(f"    {{{row[0]}, {row[1]}, {row[2]}, {row[3]}, {row[4]}, {row[5]}, {row[6]}}},")
    else:
        out.append("    {0, 0, 65535, 0, 0, 0, 0},")
    out.append("};")
    out.append(f"inline constexpr WgTemplatePoolRow kWgTemplatePools[{max(1, len(pools))}] = {{")
    if pools:
        for fallback, offset, count in pools:
            out.append(f"    {{{fallback}, {offset}, {count}}},")
    else:
        out.append("    {0, 0, 0},")
    out.append("};")
    out.append(f"inline constexpr const char* kWgTemplatePoolNames[{max(1, len(pool_names))}] = {{")
    for name in pool_names:
        out.append(f"    {cstr(name)},")
    out.append("};")
    out.append("")
    out.append("struct WgRuinedPortalSetup {")
    out.append("  uint8_t placement; // 0 land, 1 partly buried, 2 ocean floor, 3 mountain, 4 underground, 5 nether")
    out.append("  float air_pocket;")
    out.append("  float mossiness;")
    out.append("  uint8_t overgrown;")
    out.append("  uint8_t vines;")
    out.append("  uint8_t cold;")
    out.append("  uint8_t blackstone;")
    out.append("  float weight;")
    out.append("};")
    out.append("struct WgRuinedPortalConfig {")
    out.append("  const char* name;")
    out.append("  uint16_t setup_offset;")
    out.append("  uint16_t setup_count;")
    out.append("};")
    out.append(f"inline constexpr WgRuinedPortalSetup kWgRuinedPortalSetups[{max(1, len(portal_setup_rows))}] = {{")
    if portal_setup_rows:
        for row in portal_setup_rows:
            out.append(f"    {{{row[0]}, {_float_literal(row[1])}, {_float_literal(row[2])}, "
                       f"{row[3]}, {row[4]}, {row[5]}, {row[6]}, {_float_literal(row[7])}}},")
    else:
        out.append("    {0, 0.0f, 0.0f, 0, 0, 0, 0, 0.0f},")
    out.append("};")
    out.append(f"inline constexpr WgRuinedPortalConfig kWgRuinedPortalConfigs[{len(portal_rows)}] = {{")
    for name, offset, count in portal_rows:
        out.append(f"    {{{cstr(name)}, {offset}, {count}}},")
    out.append("};")
    out.append("")
    out.append("struct WgOceanRuinConfig {")
    out.append("  const char* name;")
    out.append("  uint8_t cold;")
    out.append("  float large_probability;")
    out.append("  float cluster_probability;")
    out.append("};")
    out.append(f"inline constexpr WgOceanRuinConfig kWgOceanRuinConfigs[{len(ocean_rows)}] = {{")
    for name, cold, large, cluster in ocean_rows:
        out.append(f"    {{{cstr(name)}, {cold}, {_float_literal(large)}, {_float_literal(cluster)}}},")
    out.append("};")
    out.append("")
    out.append("struct WgJigsawConfig {")
    out.append("  const char* name;")
    out.append("  uint16_t start_pool;")
    out.append("  uint8_t max_depth; // the structure JSON's `size`")
    out.append("  // `start_height`: 0 absolute (min == max), 1 uniform [min, max].")
    out.append("  uint8_t start_height_kind;")
    out.append("  int16_t start_height_min;")
    out.append("  int16_t start_height_max;")
    out.append("  uint8_t project_heightmap; // 1 = WORLD_SURFACE_WG")
    out.append("  uint8_t use_expansion_hack;")
    out.append("  uint16_t max_distance; // horizontal and vertical")
    out.append("  // `start_jigsaw_name` as a `kWgJigsawStringTable` index, or 0xFFFF.")
    out.append("  uint16_t start_jigsaw;")
    out.append("};")
    out.append(f"inline constexpr WgJigsawConfig kWgJigsawConfigs[{len(jigsaw_rows)}] = {{")
    for (name, pool, depth, height_kind, height_min, height_max, project, expand,
         max_distance, start_jigsaw) in jigsaw_rows:
        out.append(f"    {{{cstr(name)}, {pool}, {depth}, {height_kind}, {height_min}, "
                   f"{height_max}, {project}, {expand}, {max_distance}, {start_jigsaw}}},")
    out.append("};")
    out.append("")
    # Reach over every template's four rotations.
    def pivot_xy(x, z, rotation, pivot_x, pivot_z):
        if rotation == 1:
            return (pivot_x + pivot_z - z, pivot_z - pivot_x + x)
        if rotation == 2:
            return (pivot_x + pivot_x - x, pivot_z + pivot_z - z)
        if rotation == 3:
            return (pivot_x - pivot_z + z, pivot_x + pivot_z - x)
        return (x, z)

    west = east = north = south = 0
    for template in templates:
        sx, _, sz = template["size"]
        px, _, pz = template["pivot"]
        for rotation in range(4):
            a = pivot_xy(0, 0, rotation, px, pz)
            b = pivot_xy(sx - 1, sz - 1, rotation, px, pz)
            west = max(west, -min(a[0], b[0]))
            east = max(east, max(a[0], b[0]))
            north = max(north, -min(a[1], b[1]))
            south = max(south, max(a[1], b[1]))
    reach = max(-(-west // 16), east // 16, -(-north // 16), south // 16)
    out.append("// How far any rotation of any structure-piece template's box can reach")
    out.append(f"// from the source chunk's minimum corner: {west} west, {east} east,")
    out.append(f"// {north} north, {south} south. With 16-block chunks that is {reach}")
    out.append("// chunk(s) away at most, so a start can only touch chunks within that")
    out.append("// distance of its source.")
    out.append(f"inline constexpr int32_t kWgPieceChunkReach = {reach};")
    out.append("")
    out.append("}  // namespace mc::world")
    out.append("// clang-format on")
    return "\n".join(out) + "\n"


_LOOT_ENTRY_KINDS = {
    "minecraft:empty": 0,
    "minecraft:item": 1,
    "minecraft:loot_table": 2,
    "minecraft:alternatives": 3,
    "minecraft:group": 4,
    "minecraft:sequence": 5,
}
_LOOT_FUNCTION_KINDS = {
    "minecraft:set_count": 0,
    "minecraft:set_damage": 1,
    "minecraft:enchant_randomly": 2,
    "minecraft:enchant_with_levels": 3,
    "minecraft:set_potion": 4,
    "minecraft:set_name": 5,
    "minecraft:set_stew_effect": 6,
    "minecraft:set_ominous_bottle_amplifier": 7,
    "minecraft:set_enchantments": 8,
    "minecraft:exploration_map": 9,
}


def number_range(value):
    """A loot number provider as (uniform, min, max)."""
    if isinstance(value, (int, float)):
        return (0, float(value), float(value))
    if not isinstance(value, dict):
        raise ValueError(f"unsupported loot number {value!r}")
    kind = value.get("type", "")
    if kind in ("minecraft:constant", "constant"):
        v = float(value["value"])
        return (0, v, v)
    if kind in ("minecraft:uniform", "uniform"):
        return (1, float(value["min"]), float(value["max"]))
    raise ValueError(f"unsupported loot number provider {kind!r}")


def loot_conditions(conditions) -> tuple[int, float]:
    """(has_chance, chance). `location_check` is treated as satisfied."""
    has_chance = 0
    chance = 1.0
    for condition in conditions or []:
        kind = condition.get("condition")
        if kind == "minecraft:random_chance":
            has_chance = 1
            chance = float(condition["chance"])
        elif kind == "minecraft:location_check":
            # The only one in the structure chest tables is a biome predicate on
            # a sulfur-cave pool; the resolver has no level context, so it lets
            # the pool through rather than silently deleting it.
            continue
        else:
            raise ValueError(f"unsupported loot condition {kind!r}")
    return has_chance, chance


def emit_loot_table(pack):
    """`worldgen_loot_table.h`: the structure chest and dispenser tables."""
    enchantment_tags = EnchantmentTags(pack.parent)
    root = pack.parent / "loot_table"
    paths = []
    for sub in ("chests", "dispensers", "archaeology", "pots", "spawners"):
        paths += [p for p in (root / sub).rglob("*.json")]
    documents = {}
    for path in paths:
        name = "minecraft:" + "/".join(path.relative_to(root).with_suffix("").parts)
        documents[name] = load(path)
    names = sorted(documents)
    table_index = {name: index for index, name in enumerate(names)}

    effect_rows = []
    enchantment_rows = []
    enchantment_lists = {}
    function_rows = []
    entry_rows = []

    def add_enchantment_options(options):
        """A HolderSet or tag as names, deduplicated into the shared list."""
        if options is None:
            return 0, 0
        if isinstance(options, str):
            names = enchantment_tags.resolve(options[1:]) if options.startswith("#") \
                else [options]
        elif isinstance(options, list):
            names = []
            for option in options:
                if option.startswith("#"):
                    for name in enchantment_tags.resolve(option[1:]):
                        if name not in names:
                            names.append(name)
                elif option not in names:
                    names.append(option)
        else:
            raise ValueError(f"unsupported enchantment options {options!r}")
        if not names:
            raise ValueError(f"empty enchantment options {options!r}")
        key = tuple(names)
        if key in enchantment_lists:
            return enchantment_lists[key]
        offset = len(enchantment_rows)
        enchantment_rows.extend(names)
        enchantment_lists[key] = (offset, len(names))
        return offset, len(names)

    def add_function(function):
        kind_name = function.get("function")
        if kind_name not in _LOOT_FUNCTION_KINDS:
            raise ValueError(f"unsupported loot function {kind_name!r}")
        if function.get("conditions"):
            raise ValueError(f"{kind_name}: conditional functions are not modelled")
        kind = _LOOT_FUNCTION_KINDS[kind_name]
        uniform, low, high = 0, 0.0, 0.0
        text = None
        data = 0
        effect_offset = 0
        effect_count = 0
        enchantment_offset = 0
        enchantment_count = 0
        if kind in (0, 1, 7):
            field = {0: "count", 1: "damage", 7: "amplifier"}[kind]
            uniform, low, high = number_range(function.get(field, 1.0))
        elif kind == 3:
            uniform, low, high = number_range(function.get("levels", 0.0))
            text = function.get("options")
            enchantment_offset, enchantment_count = add_enchantment_options(function.get("options"))
        elif kind == 2:
            text = function.get("options")
            enchantment_offset, enchantment_count = add_enchantment_options(function.get("options"))
        elif kind == 4:
            text = function["id"]
        elif kind == 5:
            name = function.get("name", {})
            text = name.get("translate") or name.get("text") or ""
        elif kind == 6:
            effect_offset = len(effect_rows)
            for effect in function.get("effects", []):
                _, dmin, dmax = number_range(effect.get("duration", 0.0))
                effect_rows.append((effect["type"], dmin, dmax))
            effect_count = len(effect_rows) - effect_offset
        elif kind == 8:
            enchantments = function.get("enchantments", {})
            if len(enchantments) != 1:
                raise ValueError("set_enchantments with more than one entry")
            text = next(iter(enchantments))
            data = int(next(iter(enchantments.values())))
        elif kind == 9:
            text = function.get("decoration")
            data = int(function.get("zoom", 1))
        function_rows.append((kind, uniform, low, high, text, data, effect_offset, effect_count,
                              enchantment_offset, enchantment_count))
        return len(function_rows) - 1

    def add_entries(entries):
        """Append the list's entries and return (offset, count)."""
        offset = len(entry_rows)
        for entry in entries:
            kind_name = entry.get("type")
            if kind_name not in _LOOT_ENTRY_KINDS:
                raise ValueError(f"unsupported loot entry {kind_name!r}")
            kind = _LOOT_ENTRY_KINDS[kind_name]
            has_chance, chance = loot_conditions(entry.get("conditions"))
            weight = int(entry.get("weight", 1))
            quality = int(entry.get("quality", 0))
            item = None
            table = 0xFFFF
            child_offset = 0
            child_count = 0
            function_offset = 0
            function_count = 0
            if kind == 1:
                item = entry["name"]
            elif kind == 2:
                name = entry.get("value") or entry.get("name")
                if name not in table_index:
                    raise ValueError(f"loot table entry names unknown table {name!r}")
                table = table_index[name]
            elif kind in (3, 4, 5):
                child_offset, child_count = add_entries(entry.get("children", []))
            if kind in (1, 2):
                functions = entry.get("functions", [])
                function_offset = len(function_rows)
                for function in functions:
                    add_function(function)
                function_count = len(function_rows) - function_offset
            entry_rows.append((kind, has_chance, chance, weight, quality, item, table,
                               child_offset, child_count, function_offset, function_count))
        return offset, len(entry_rows) - offset

    pool_rows = []
    table_rows = []
    for name in names:
        document = documents[name]
        pool_offset = len(pool_rows)
        for pool in document.get("pools", []):
            has_chance, chance = loot_conditions(pool.get("conditions"))
            uniform, low, high = number_range(pool.get("rolls", 1.0))
            entry_offset, entry_count = add_entries(pool.get("entries", []))
            pool_rows.append((entry_offset, entry_count, int(round(low)), int(round(high)),
                              has_chance, chance))
        table_rows.append((name, pool_offset, len(pool_rows) - pool_offset))

    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// Generated by tools/gen_worldgen_registry_table.py — do not edit by hand.")
    out.append("//")
    out.append("// The vanilla 26.2 structure container loot tables (chests, dispensers,")
    out.append("// archaeology, pots and spawners), flattened for")
    out.append("// `world/worldgen_loot.cpp`. A table is a list of pools; a pool rolls a")
    out.append("// number of weighted entries; an entry is an item, an empty slot, another")
    out.append("// table, or a composite of child entries. Functions are the component")
    out.append("// producers this engine models; a function it does not model is refused")
    out.append("// by the emitter rather than silently dropped.")
    out.append("// clang-format off")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("enum class WgLootEntryKind : uint8_t {")
    out.append("  kEmpty = 0,")
    out.append("  kItem = 1,")
    out.append("  kTable = 2,")
    out.append("  kAlternatives = 3,")
    out.append("  kGroup = 4,")
    out.append("  kSequence = 5,")
    out.append("};")
    out.append("enum class WgLootFunctionKind : uint8_t {")
    out.append("  kSetCount = 0,")
    out.append("  kSetDamage = 1,")
    out.append("  kEnchantRandomly = 2,")
    out.append("  kEnchantWithLevels = 3,")
    out.append("  kSetPotion = 4,")
    out.append("  kSetName = 5,")
    out.append("  kSetStewEffect = 6,")
    out.append("  kSetOminousBottleAmplifier = 7,")
    out.append("  kSetEnchantments = 8,")
    out.append("  kExplorationMap = 9,")
    out.append("};")
    out.append("")
    out.append("struct WgLootEffectRow { const char* effect; float duration_min; float duration_max; };")
    out.append("struct WgLootFunctionRow {")
    out.append("  uint8_t kind;")
    out.append("  uint8_t range; // 0 constant, 1 uniform")
    out.append("  float min;")
    out.append("  float max;")
    out.append("  const char* text; // potion, tag, name or enchantment")
    out.append("  int16_t data;")
    out.append("  uint16_t effect_offset;")
    out.append("  uint8_t effect_count;")
    out.append("  uint16_t enchantment_offset;")
    out.append("  uint8_t enchantment_count;")
    out.append("};")
    out.append("struct WgLootEntryRow {")
    out.append("  uint8_t kind;")
    out.append("  uint8_t has_chance;")
    out.append("  float chance;")
    out.append("  int32_t weight;")
    out.append("  int32_t quality;")
    out.append("  const char* item;")
    out.append("  uint16_t table_index;")
    out.append("  uint16_t child_offset;")
    out.append("  uint16_t child_count;")
    out.append("  uint32_t function_offset;")
    out.append("  uint8_t function_count;")
    out.append("};")
    out.append("struct WgLootPoolRow {")
    out.append("  uint32_t entry_offset;")
    out.append("  uint16_t entry_count;")
    out.append("  int16_t rolls_min;")
    out.append("  int16_t rolls_max;")
    out.append("  uint8_t has_chance;")
    out.append("  float chance;")
    out.append("};")
    out.append("struct WgLootTableRow {")
    out.append("  const char* name;")
    out.append("  uint32_t pool_offset;")
    out.append("  uint16_t pool_count;")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgLootEffectRow kWgLootEffects[{max(1, len(effect_rows))}] = {{")
    for effect, low, high in effect_rows:
        out.append(f"    {{{cstr(effect)}, {flit(low)}, {flit(high)}}},")
    if not effect_rows:
        out.append("    {nullptr, 0.0f, 0.0f},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr const char* kWgLootEnchantments[{max(1, len(enchantment_rows))}] = {{")
    for name in enchantment_rows:
        out.append(f"    {cstr(name)},")
    if not enchantment_rows:
        out.append("    nullptr,")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgLootFunctionRow kWgLootFunctions[{max(1, len(function_rows))}] = {{")
    for (kind, uniform, low, high, text, data, effect_offset, effect_count,
         enchantment_offset, enchantment_count) in function_rows:
        out.append("    {" + f"{kind}, {uniform}, {flit(low)}, {flit(high)}, " +
                   f"{cstr(text) if text is not None else 'nullptr'}, {data}, " +
                   f"{effect_offset}, {effect_count}, {enchantment_offset}, " +
                   f"{enchantment_count}" + "},")
    if not function_rows:
        out.append("    {0, 0, 0.0f, 0.0f, nullptr, 0, 0, 0, 0, 0},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgLootEntryRow kWgLootEntries[{max(1, len(entry_rows))}] = {{")
    for (kind, has_chance, chance, weight, quality, item, table, child_offset, child_count,
         function_offset, function_count) in entry_rows:
        out.append("    {" + f"{kind}, {has_chance}, {flit(chance)}, {weight}, {quality}, " +
                   f"{cstr(item) if item is not None else 'nullptr'}, {table}, " +
                   f"{child_offset}, {child_count}, {function_offset}, {function_count}" + "},")
    if not entry_rows:
        out.append("    {0, 0, 0.0f, 0, 0, nullptr, 0, 0, 0, 0, 0},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgLootPoolRow kWgLootPools[{max(1, len(pool_rows))}] = {{")
    for entry_offset, entry_count, rolls_min, rolls_max, has_chance, chance in pool_rows:
        out.append("    {" + f"{entry_offset}, {entry_count}, {rolls_min}, {rolls_max}, " +
                   f"{has_chance}, {flit(chance)}" + "},")
    if not pool_rows:
        out.append("    {0, 0, 0, 0, 0, 0.0f},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr WgLootTableRow kWgLootTables[{max(1, len(table_rows))}] = {{")
    for name, pool_offset, pool_count in table_rows:
        out.append(f"    {{{cstr(name)}, {pool_offset}, {pool_count}}},")
    if not table_rows:
        out.append("    {nullptr, 0, 0},")
    out.append("};")
    out.append("")
    out.append("}  // namespace mc::world")
    out.append("// clang-format on")
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pack", type=pathlib.Path, default=pathlib.Path(_PACK))
    parser.add_argument("--feature-header", type=pathlib.Path,
                        default=pathlib.Path("src/world/worldgen_feature_table.h"))
    parser.add_argument("--carver-header", type=pathlib.Path,
                        default=pathlib.Path("src/world/worldgen_carver_table.h"))
    parser.add_argument("--structure-header", type=pathlib.Path,
                        default=pathlib.Path("src/world/worldgen_structure_table.h"))
    parser.add_argument("--fossil-header", type=pathlib.Path,
                        default=pathlib.Path("src/world/worldgen_fossil_table.h"))
    parser.add_argument("--piece-header", type=pathlib.Path,
                        default=pathlib.Path("src/world/worldgen_structure_piece_table.h"))
    parser.add_argument("--loot-header", type=pathlib.Path,
                        default=pathlib.Path("src/world/worldgen_loot_table.h"))
    args = parser.parse_args()
    if not args.pack.is_dir():
        print(f"no worldgen tree at {args.pack}", file=sys.stderr)
        return 1

    tags = BlockTags(args.pack.parent)
    biome_tags = BiomeTags(args.pack.parent)
    args.feature_header.parent.mkdir(parents=True, exist_ok=True)
    args.feature_header.write_text(emit_feature_table(args.pack, tags))
    args.carver_header.write_text(emit_carver_table(args.pack, tags))
    args.structure_header.write_text(emit_structure_table(args.pack, biome_tags))
    args.fossil_header.write_text(emit_fossil_table(args.pack, tags))
    args.piece_header.write_text(emit_structure_piece_table(args.pack))
    args.loot_header.write_text(emit_loot_table(args.pack))
    print(f"wrote {args.feature_header}")
    print(f"wrote {args.carver_header}")
    print(f"wrote {args.structure_header}")
    print(f"wrote {args.fossil_header}")
    print(f"wrote {args.piece_header}")
    print(f"wrote {args.loot_header}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
