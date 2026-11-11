#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates `src/world/advancement_table.h`: the advancement registry, as data.

Phase 4 milestone N wants advancements "real": the full registry the data pack
ships, a progress model, and the `update_advancements` packet. This tool is the
reading half. It walks the *actual* `data/minecraft/advancement` tree of an
extracted version (1,688 JSON files in 26.2 -- 126 with a display, the rest the
generated recipe advancements), and emits one flat table:

  * each advancement's id, parent, display (title/description translation keys,
    icon item, frame, background and the three flag bits), the criteria and the
    requirements, and the rewards (recipe keys + experience);
  * each criterion's trigger as an enum id, plus the condition subset this
    engine can evaluate: item predicates for `inventory_changed` and
    `consume_item` (with `#tag` references *expanded here*, so the runtime never
    resolves a tag), a recipe key for `recipe_unlocked` / `recipe_crafted`,
    an entity type for `player_killed_entity`, `entity_killed_player`,
    `tame_animal` and the three `bred_animals` slots, the `location`
    trigger's `biomes` list (the pack's 60 biome criteria -- every
    `adventuring_time` biome and the five nether ones -- again with `#tag`
    references expanded), the block an `enter_block` criterion names, and the
    effect set an `effects_changed` criterion requires. A criterion whose
    conditions use a predicate shape this engine does not model (durability
    predicates, a `location` predicate that is not its `biomes` field,
    equipment, component matchers, `slots`, `killing_blow`, a `state`
    property predicate, an effect amount bound) is emitted with the
    `kAdvConditionUnsupported` flag: it is still in the requirements (the wire
    layout needs it) but never matches, so the advancement cannot be
    auto-completed and is honest about it;
  * the display *coordinates*: `ServerAdvancementManager.apply` runs
    `TreeNodePosition.run` over every displayed root, and those x/y are written
    into the packet, so the layout is computed here with a port of the same
    Reingold-Tilford walker. Children are ordered by id (the pack's own load
    order is not a stable thing to imitate), which is deterministic and valid
    even where it differs from a given vanilla load.

Usage:
    gen_advancement_table.py <data/minecraft dir> <out header>

e.g.
    gen_advancement_table.py data/vanilla/26.2/data/minecraft \\
        src/world/advancement_table.h
"""

import argparse
import json
import pathlib
import sys

# ---- the trigger registry ---------------------------------------------------
#
# The ids are this engine's own (the wire never carries them): the generated
# criterion says which trigger it is, and `advancement.cpp` matches. `kOther` is
# every trigger this engine does not model, `kImpossible` is vanilla's
# `minecraft:impossible` (which by definition never fires).
TRIGGERS = {
    "inventory_changed": "kAdvTriggerInventoryChanged",
    "recipe_unlocked": "kAdvTriggerRecipeUnlocked",
    "recipe_crafted": "kAdvTriggerRecipeCrafted",
    "consume_item": "kAdvTriggerConsumeItem",
    "player_killed_entity": "kAdvTriggerPlayerKilledEntity",
    "entity_killed_player": "kAdvTriggerEntityKilledPlayer",
    "bred_animals": "kAdvTriggerBredAnimals",
    "tame_animal": "kAdvTriggerTameAnimal",
    "villager_trade": "kAdvTriggerVillagerTrade",
    "tick": "kAdvTriggerTick",
    "location": "kAdvTriggerLocation",
    "enter_block": "kAdvTriggerEnterBlock",
    "effects_changed": "kAdvTriggerEffectsChanged",
    "impossible": "kAdvTriggerImpossible",
}

UNSUPPORTED = 1  # kAdvConditionUnsupported bit


def strip_ns(name):
    """`minecraft:oak_log` and `oak_log` are the same key in the pack."""
    return name.split(":", 1)[-1] if ":" in name else name


def registry_key(name):
    """A registry key as this engine's tables spell it (`minecraft:water`).

    The pack writes a bare `water` where the default namespace applies; the
    runtime compares against the world's own `minecraft:water`, so the default
    is made explicit here rather than at every comparison.
    """
    return name if ":" in name else "minecraft:" + name


class TagResolver:
    """`#minecraft:planks` -> the concrete registry keys, recursively.

    A tag value may be another tag and may carry `{"id": ..., "required":
    false}`; unknown *entries* are skipped (a missing optional entry is normal),
    but a missing tag a file actually references is a hard error: the generated
    table must not silently lose a predicate.
    """

    # The tag kinds a criterion can reference, and the directory each one's
    # files live in: `#minecraft:planks` and `#minecraft:is_forest` are the same
    # shape, only their tree differs.
    TAG_DIRS = {"item": "item", "entity_type": "entity_type", "biome": "worldgen/biome"}

    def __init__(self, root):
        self.kinds = {}
        for kind, subdir in self.TAG_DIRS.items():
            tags = {}
            for path in sorted((root / "tags" / subdir).rglob("*.json")):
                rel = path.relative_to(root / "tags" / subdir).with_suffix("")
                tags[str(rel)] = json.loads(path.read_text())["values"]
            self.kinds[kind] = tags

    def expand(self, kind, ref):
        name = strip_ns(ref[1:])
        seen = {}

        def walk(tag):
            if tag in seen:
                return seen[tag]
            if tag not in self.kinds[kind]:
                raise SystemExit(f"unknown tag #{kind}/{tag} (referenced as {ref})")
            out = []
            for value in self.kinds[kind][tag]:
                if isinstance(value, dict):
                    value = value["id"]
                if value.startswith("#"):
                    out.extend(walk(strip_ns(value[1:])))
                else:
                    out.append("minecraft:" + strip_ns(value) if ":" not in value else value)
            seen[tag] = out
            return out

        return sorted(set(walk(name)))


class Criterion:
    def __init__(self, name, trigger):
        self.name = name
        self.trigger = trigger
        self.flags = 0
        self.items = []
        self.recipe = None
        self.entity = None
        self.child = None
        self.parent = None
        self.partner = None
        # `location`'s `biomes` holder set, as concrete biome keys.
        self.biomes = []
        # `enter_block`'s block, as a concrete block key (a list to share the
        # one shape with the other condition lists).
        self.blocks = []
        # `effects_changed`'s `effects` map, as the concrete effect keys every
        # one of which the player must have.
        self.effects = []

    def unsupported(self):
        self.flags |= UNSUPPORTED
        return self


def item_predicate(tags, pred):
    """One `ItemPredicate` -> item names, or None when not modelled."""
    if not isinstance(pred, dict):
        return None
    if set(pred.keys()) - {"items"}:
        return None
    items = pred.get("items")
    if items is None:
        return None
    if isinstance(items, str):
        items = [items]
    names = []
    for value in items:
        if value.startswith("#"):
            names.extend(tags.expand("item", value))
        else:
            names.append(value)
    return names


def entity_predicate(tags, wrapped):
    """One context-aware `entity_properties` predicate -> type names, or None.

    The predicate must be exactly `minecraft:entity_type` (a key or a list of
    keys/tags): anything else (`minecraft:distance`, equipment, components,
    `location`) changes the meaning and is not modelled here. The data wraps
    every one in a list (`ContextAwarePredicate`'s codec), so a one-element
    list is the normal shape.
    """
    if isinstance(wrapped, list):
        if len(wrapped) != 1:
            return None
        wrapped = wrapped[0]
    if not isinstance(wrapped, dict):
        return None
    if set(wrapped.keys()) - {"condition", "entity", "predicate"}:
        return None
    if wrapped.get("condition") != "minecraft:entity_properties":
        return None
    if wrapped.get("entity") != "this":
        return None
    pred = wrapped.get("predicate", {})
    if set(pred.keys()) != {"minecraft:entity_type"}:
        return None
    types = pred["minecraft:entity_type"]
    if isinstance(types, str):
        types = [types]
    names = []
    for value in types:
        if value.startswith("#"):
            names.extend(tags.expand("entity_type", value))
        else:
            names.append(value)
    return names


def parse_criterion(tags, name, body):
    trigger = body["trigger"].split(":")[-1]
    criterion = Criterion(name, TRIGGERS.get(trigger, "kAdvTriggerOther"))
    conditions = body.get("conditions")
    if criterion.trigger == "kAdvTriggerOther":
        return criterion.unsupported()
    if trigger == "tick" or trigger == "impossible":
        if conditions:
            return criterion.unsupported()
        return criterion
    if trigger == "enter_block":
        # `EnterBlockTrigger.TriggerInstance` is an optional `block` and an
        # optional `state` (a `StatePropertiesPredicate` over the block's own
        # state definition); `matches` is `!block.isPresent() || state.is(block)`
        # and then the state predicate. Only the block half is modelled: a
        # criterion naming a `state` shape stays `unsupported` rather than
        # matching on its block and over-claiming. The pack's eleven criteria
        # name a block and nothing else (ten `in_water` on the boat recipe
        # advancements and `entered_end_gateway`).
        conditions = conditions or {}
        if set(conditions.keys()) - {"block"}:
            return criterion.unsupported()
        block = conditions.get("block")
        if block is None:
            return criterion  # no block named: any entry matches
        if not isinstance(block, str) or not block:
            return criterion.unsupported()
        criterion.blocks = [registry_key(block)]
        return criterion
    if trigger == "effects_changed":
        # `EffectsChangedTrigger.TriggerInstance` is an optional `effects`
        # (`MobEffectsPredicate`) and an optional `source` entity predicate.
        # `MobEffectsPredicate.matches` requires *every* effect the map names to
        # be on the entity, each instance satisfying its own amount bounds; the
        # pack's two criteria give every effect an empty `{}`, i.e. any level,
        # which is checked here rather than silently dropped. The `source` half
        # needs the entity an effect came from, and this engine's effect sinks
        # carry none, so `kill_axolotl_target` stays unsupported.
        conditions = conditions or {}
        if set(conditions.keys()) - {"effects"}:
            return criterion.unsupported()
        effects = conditions.get("effects")
        if effects is None:
            return criterion  # no effect named: any change matches
        if not isinstance(effects, dict) or not effects:
            return criterion.unsupported()
        names = []
        for effect, bounds in effects.items():
            if bounds:
                return criterion.unsupported()
            names.append(registry_key(effect))
        criterion.effects = sorted(set(names))
        return criterion
    if trigger == "location":
        # `CriteriaTriggers.LOCATION` is a `PlayerTrigger`: its only field is an
        # optional `player` `ContextAwarePredicate` wrapped around an
        # `EntityPredicate` that vanilla evaluates against the player, and the
        # pack's 66 location criteria are all one shape -- the entity
        # predicate's own `minecraft:location` field carrying a `biomes` holder
        # set (55 `adventuring_time` biomes plus the five nether ones). The
        # other fields a `LocationPredicate` can carry (`structures`, `position`,
        # `dimension`, `light`, `block`, `fluid`, `smokey`, `can_see_sky`), and
        # any *sibling* entity predicate (`minecraft:stepping_on`,
        # `minecraft:equipment`, ...), are not evaluated here: such a criterion
        # stays `unsupported` rather than matching on its biome half and
        # over-claiming.
        if not isinstance(conditions, dict) or set(conditions.keys()) - {"player"}:
            return criterion.unsupported()
        entries = conditions.get("player")
        if isinstance(entries, dict):
            entries = [entries]
        if not isinstance(entries, list) or len(entries) != 1:
            return criterion.unsupported()
        entry = entries[0]
        if not isinstance(entry, dict) or set(entry.keys()) - {"condition", "entity", "predicate"}:
            return criterion.unsupported()
        if entry.get("condition") != "minecraft:entity_properties" or entry.get("entity") != "this":
            return criterion.unsupported()
        predicate = entry.get("predicate", {})
        if not isinstance(predicate, dict) or set(predicate.keys()) != {"minecraft:location"}:
            return criterion.unsupported()
        location = predicate["minecraft:location"]
        if not isinstance(location, dict) or set(location.keys()) != {"biomes"}:
            return criterion.unsupported()
        biomes = location["biomes"]
        if isinstance(biomes, str):
            biomes = [biomes]
        if not isinstance(biomes, list) or not biomes:
            return criterion.unsupported()
        names = []
        for value in biomes:
            if value.startswith("#"):
                names.extend(tags.expand("biome", value))
            else:
                names.append(value)
        criterion.biomes = sorted(set(names))
        return criterion
    if trigger == "inventory_changed":
        if not isinstance(conditions, dict) or set(conditions.keys()) - {"items"}:
            return criterion.unsupported()
        predicates = conditions.get("items", [])
        if not predicates:
            return criterion  # any change matches
        if len(predicates) > 1:
            # Vanilla wants one predicate per distinct stack; a flat name list
            # cannot express that, so the criterion stays unearnable.
            return criterion.unsupported()
        names = item_predicate(tags, predicates[0])
        if names is None:
            return criterion.unsupported()
        criterion.items = names
        return criterion
    if trigger == "consume_item":
        if not conditions:
            return criterion
        if set(conditions.keys()) - {"item"}:
            return criterion.unsupported()
        names = item_predicate(tags, conditions.get("item"))
        if names is None:
            return criterion.unsupported()
        criterion.items = names
        return criterion
    if trigger in ("recipe_unlocked", "recipe_crafted"):
        key = "recipe" if trigger == "recipe_unlocked" else "recipe_id"
        if set((conditions or {}).keys()) - {key}:
            return criterion.unsupported()
        recipe = (conditions or {}).get(key)
        if not recipe:
            return criterion.unsupported()
        criterion.recipe = recipe
        return criterion
    if trigger in ("player_killed_entity", "entity_killed_player", "tame_animal"):
        conditions = conditions or {}
        if set(conditions.keys()) - {"entity"}:
            return criterion.unsupported()  # killing_blow, player...
        wrapped = conditions.get("entity")
        if wrapped is None:
            return criterion  # any victim
        types = entity_predicate(tags, wrapped)
        if types is None or len(types) != 1:
            return criterion.unsupported()
        criterion.entity = types[0]
        return criterion
    if trigger == "bred_animals":
        conditions = conditions or {}
        if set(conditions.keys()) - {"child", "parent", "partner"}:
            return criterion.unsupported()
        for slot in ("child", "parent", "partner"):
            wrapped = conditions.get(slot)
            if wrapped is None:
                continue
            types = entity_predicate(tags, wrapped)
            if types is None or len(types) != 1:
                return criterion.unsupported()
            setattr(criterion, slot, types[0])
        return criterion
    if trigger == "villager_trade":
        if conditions:
            return criterion.unsupported()
        return criterion
    return criterion.unsupported()


# ---- the tree layout (a port of TreeNodePosition) ---------------------------


class LayoutNode:
    """`TreeNodePosition`: the walker the server runs over a displayed root."""

    def __init__(self, node, parent, previous_sibling, child_index, depth):
        self.node = node  # an advancement dict with a display
        self.parent = parent
        self.previous_sibling = previous_sibling
        self.child_index = child_index
        self.children = []
        self.ancestor = self
        self.thread = None
        self.x = depth
        self.y = -1.0
        self.mod = 0.0
        self.change = 0.0
        self.shift = 0.0
        previous = None
        for child in node["children"]:
            previous = self.add_child(child, previous)

    def add_child(self, node, previous):
        if node.get("display") is not None:
            previous = LayoutNode(node, self, previous, len(self.children) + 1, self.x + 1)
            self.children.append(previous)
        else:
            # An invisible node is hoisted away: its displayed children attach
            # to the nearest visible ancestor (TreeNodePosition.addChild).
            for grandchild in node["children"]:
                previous = self.add_child(grandchild, previous)
        return previous

    def first_walk(self):
        if not self.children:
            self.y = self.previous_sibling.y + 1.0 if self.previous_sibling else 0.0
            return
        default_ancestor = None
        for child in self.children:
            child.first_walk()
            default_ancestor = child.apportion(child if default_ancestor is None else default_ancestor)
        self.execute_shifts()
        midpoint = (self.children[0].y + self.children[-1].y) / 2.0
        if self.previous_sibling is not None:
            self.y = self.previous_sibling.y + 1.0
            self.mod = self.y - midpoint
        else:
            self.y = midpoint

    def second_walk(self, mod_sum, depth, minimum):
        self.y += mod_sum
        self.x = depth
        minimum = min(minimum, self.y)
        for child in self.children:
            minimum = child.second_walk(mod_sum + self.mod, depth + 1, minimum)
        return minimum

    def third_walk(self, offset):
        self.y += offset
        for child in self.children:
            child.third_walk(offset)

    def execute_shifts(self):
        shift = 0.0
        change = 0.0
        for child in reversed(self.children):
            child.y += shift
            child.mod += shift
            change += child.change
            shift += child.shift + change

    def previous_or_thread(self):
        if self.thread is not None:
            return self.thread
        if self.children:
            return self.children[0]
        return None

    def next_or_thread(self):
        if self.thread is not None:
            return self.thread
        if self.children:
            return self.children[-1]
        return None

    def apportion(self, default_ancestor):
        if self.previous_sibling is None:
            return default_ancestor
        vir = self
        vor = self
        vil = self.previous_sibling
        vol = self.parent.children[0]
        sir = self.mod
        sor = self.mod
        sil = vil.mod
        sol = vol.mod
        while vil.next_or_thread() is not None and vir.previous_or_thread() is not None:
            vil = vil.next_or_thread()
            vir = vir.previous_or_thread()
            vol = vol.previous_or_thread()
            vor = vor.next_or_thread()
            vor.ancestor = self
            shift = vil.y + sil - (vir.y + sir) + 1.0
            if shift > 0.0:
                vil.get_ancestor(self, default_ancestor).move_subtree(self, shift)
                sir += shift
                sor += shift
            sil += vil.mod
            sir += vir.mod
            sol += vol.mod
            sor += vor.mod
        if vil.next_or_thread() is not None and vor.next_or_thread() is None:
            vor.thread = vil.next_or_thread()
            vor.mod += sil - sor
        else:
            if vir.previous_or_thread() is not None and vol.previous_or_thread() is None:
                vol.thread = vir.previous_or_thread()
                vol.mod += sir - sol
            default_ancestor = self
        return default_ancestor

    def move_subtree(self, right, shift):
        subtrees = right.child_index - self.child_index
        if subtrees != 0:
            right.change -= shift / subtrees
            self.change += shift / subtrees
        right.shift += shift
        right.y += shift
        right.mod += shift

    def get_ancestor(self, other, default_ancestor):
        if self.ancestor is not None and self.ancestor in other.parent.children:
            return self.ancestor
        return default_ancestor

    def finalize(self):
        display = self.node["display"]
        display["x"] = float(self.x)
        display["y"] = float(self.y)
        for child in self.children:
            child.finalize()


def run_layout(node):
    root = LayoutNode(node, None, None, 1, 0)
    root.first_walk()
    minimum = root.second_walk(0.0, 0, root.y)
    if minimum < 0.0:
        root.third_walk(-minimum)
    root.finalize()


# ---- emission --------------------------------------------------------------


def cstr(text):
    if text is None:
        return "nullptr"
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def cbool(value):
    return "true" if value else "false"


def emit_array(out, decl, ctype, values, per_line=6):
    if not values:
        return
    out.append(decl)
    for i in range(0, len(values), per_line):
        line = ", ".join(values[i:i + per_line])
        if i + per_line < len(values):
            line += ","
        out.append("  " + line)
    out.append("};")
    out.append("")


def load_advancements(root):
    tags = TagResolver(root)
    files = sorted((root / "advancement").rglob("*.json"))
    advancements = []
    for path in files:
        rel = path.relative_to(root / "advancement").with_suffix("")
        body = json.loads(path.read_text())
        adv = {
            "id": "minecraft:" + str(rel),
            "parent": body.get("parent"),
            "display": None,
            "criteria": [],
            "requirements": body.get("requirements")
            or [[name] for name in body["criteria"]],
            "rewards": body.get("rewards", {}),
            "telemetry": bool(body.get("sends_telemetry_event", False)),
        }
        for name, criterion in body["criteria"].items():
            adv["criteria"].append(parse_criterion(tags, name, criterion))
        if "display" in body:
            display = body["display"]
            adv["display"] = {
                "title": display["title"]["translate"],
                "description": display["description"]["translate"],
                "icon": display["icon"]["id"],
                "icon_count": int(display["icon"].get("count", 1)),
                "frame": {"task": 0, "challenge": 1, "goal": 2}[display.get("frame", "task")],
                "background": display.get("background"),
                "show_toast": bool(display.get("show_toast", True)),
                "announce_chat": bool(display.get("announce_to_chat", True)),
                "hidden": bool(display.get("hidden", False)),
                "x": 0.0,
                "y": 0.0,
            }
        advancements.append(adv)

    # The tree, for the layout. Every parent must exist; a dangling parent is a
    # pack bug this tool refuses to paper over.
    by_id = {adv["id"]: adv for adv in advancements}
    for adv in advancements:
        adv["children"] = []
    for adv in advancements:
        if adv["parent"] is None:
            continue
        if adv["parent"] not in by_id:
            raise SystemExit(f"{adv['id']} names missing parent {adv['parent']}")
        by_id[adv["parent"]]["children"].append(adv)
    for adv in advancements:
        adv["children"].sort(key=lambda child: child["id"])
    for adv in advancements:
        if adv["parent"] is None and adv["display"] is not None:
            run_layout(adv)

    advancements.sort(key=lambda adv: adv["id"])
    return advancements


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pack", help="the extracted data/minecraft directory")
    parser.add_argument("out", help="header to write")
    args = parser.parse_args()

    root = pathlib.Path(args.pack)
    advancements = load_advancements(root)
    displayed = [adv for adv in advancements if adv["display"] is not None]
    criteria_count = sum(len(adv["criteria"]) for adv in advancements)
    unsupported = sum(1 for adv in advancements for c in adv["criteria"] if c.flags)

    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// clang-format off")
    out.append("//")
    out.append(f"// The advancement registry: {len(advancements)} advancements, "
               f"{criteria_count} criteria,")
    out.append(f"// {len(displayed)} displays (generated -- do not hand-edit).")
    out.append("//")
    out.append("// Every `advancement/**/*.json` in the 26.2 data pack, compiled")
    out.append("// into one flat table by tools/gen_advancement_table.py. Column by")
    out.append("// column:")
    out.append("//")
    out.append("//   * `trigger` is this engine's own enum (see advancement.h), not a")
    out.append("//     wire id; `flags` bit 0 (`kAdvConditionUnsupported`) marks a")
    out.append("//     criterion whose conditions this engine cannot evaluate, so it")
    out.append("//     stays in the requirements but never matches.")
    out.append("//   * `items` are concrete item keys: `#tag` references were expanded")
    out.append("//     here, so the runtime never resolves a tag.")
    out.append("//   * `entity` / `child` / `parent` / `partner` are concrete entity")
    out.append("//     type keys, expanded the same way, for the kill and breeding")
    out.append("//     triggers.")
    out.append("//   * `biomes` is the `location` trigger's biome holder set, resolved to")
    out.append("//     concrete biome keys (the runtime compares the name it is handed).")
    out.append("//   * `blocks` is the block an `enter_block` criterion names (`ServerPlayer")
    out.append("//     .onInsideBlock`), and `effects` the effect set an")
    out.append("//     `effects_changed` criterion requires every one of.")
    out.append("//   * `requirements` is a flat list of criterion indices plus one size")
    out.append("//     per set; a set of one is AND, a set of many is OR -- vanilla's")
    out.append("//     `AdvancementRequirements`.")
    out.append("//   * `display`'s `x`/`y` are the tree coordinates vanilla's own")
    out.append("//     `TreeNodePosition` computes (a port of it ran at generation")
    out.append("//     time), which the packet carries to the client.")
    out.append("")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("// One trigger kind (ids are internal).")
    out.append("enum AdvancementTriggerId : uint8_t {")
    for name in sorted(set(TRIGGERS.values()), key=lambda n: list(TRIGGERS.values()).index(n)):
        out.append(f"  {name},")
    out.append("  kAdvTriggerOther,")
    out.append("};")
    out.append("")
    out.append("// A criterion's conditions could not be modelled: never matches.")
    out.append("inline constexpr uint8_t kAdvConditionUnsupported = 1;")
    out.append("")
    out.append("struct GeneratedCriterion {")
    out.append("  const char* name;")
    out.append("  uint8_t trigger;")
    out.append("  uint8_t flags;")
    out.append("  const char* const* items;")
    out.append("  uint16_t item_count;")
    out.append("  const char* recipe;")
    out.append("  const char* entity;")
    out.append("  const char* child;")
    out.append("  const char* parent;")
    out.append("  const char* partner;")
    out.append("  const char* const* biomes;")
    out.append("  uint16_t biome_count;")
    out.append("  const char* const* blocks;")
    out.append("  uint16_t block_count;")
    out.append("  const char* const* effects;")
    out.append("  uint16_t effect_count;")
    out.append("};")
    out.append("")
    out.append("struct GeneratedAdvancementDisplay {")
    out.append("  const char* title;")
    out.append("  const char* description;")
    out.append("  const char* icon;")
    out.append("  uint8_t icon_count;")
    out.append("  uint8_t frame;")
    out.append("  const char* background;")
    out.append("  bool show_toast;")
    out.append("  bool announce_chat;")
    out.append("  bool hidden;")
    out.append("  float x;")
    out.append("  float y;")
    out.append("};")
    out.append("")
    out.append("struct GeneratedAdvancement {")
    out.append("  const char* id;")
    out.append("  const char* parent;")
    out.append("  uint16_t criteria_first;")
    out.append("  uint16_t criteria_count;")
    out.append("  uint16_t requirement_sets_first;")
    out.append("  uint16_t requirement_criteria_first;")
    out.append("  uint8_t requirement_set_count;")
    out.append("  const GeneratedAdvancementDisplay* display;")
    out.append("  const char* const* reward_recipes;")
    out.append("  uint8_t reward_recipe_count;")
    out.append("  int32_t reward_experience;")
    out.append("  bool sends_telemetry_event;")
    out.append("};")
    out.append("")

    # Criterion biome arrays, next to the item ones and declared before the
    # rows that point at them.
    criteria_first = 0
    for adv in advancements:
        adv["criteria_first"] = criteria_first
        criteria_first += len(adv["criteria"])
        for index, criterion in enumerate(adv["criteria"]):
            if criterion.items:
                items = f"kAdvItems_{adv['id'].replace(':', '_').replace('/', '_')}_{index}"
                emit_array(out, f"inline constexpr const char* const {items}[] = {{",
                           "const char*", [cstr(item) for item in criterion.items])
            if criterion.biomes:
                biomes = f"kAdvBiomes_{adv['id'].replace(':', '_').replace('/', '_')}_{index}"
                emit_array(out, f"inline constexpr const char* const {biomes}[] = {{",
                           "const char*", [cstr(biome) for biome in criterion.biomes])
            if criterion.blocks:
                blocks = f"kAdvBlocks_{adv['id'].replace(':', '_').replace('/', '_')}_{index}"
                emit_array(out, f"inline constexpr const char* const {blocks}[] = {{",
                           "const char*", [cstr(block) for block in criterion.blocks])
            if criterion.effects:
                effects = f"kAdvEffects_{adv['id'].replace(':', '_').replace('/', '_')}_{index}"
                emit_array(out, f"inline constexpr const char* const {effects}[] = {{",
                           "const char*", [cstr(effect) for effect in criterion.effects])

    # Criterion rows.
    out.append("inline constexpr GeneratedCriterion kAdvancementCriteria[] = {")
    for adv in advancements:
        for index, criterion in enumerate(adv["criteria"]):
            items = f"kAdvItems_{adv['id'].replace(':', '_').replace('/', '_')}_{index}"
            if not criterion.items:
                items = "nullptr"
            item_count = len(criterion.items)
            biomes = f"kAdvBiomes_{adv['id'].replace(':', '_').replace('/', '_')}_{index}"
            if not criterion.biomes:
                biomes = "nullptr"
            biome_count = len(criterion.biomes)
            blocks = f"kAdvBlocks_{adv['id'].replace(':', '_').replace('/', '_')}_{index}"
            if not criterion.blocks:
                blocks = "nullptr"
            block_count = len(criterion.blocks)
            effects = f"kAdvEffects_{adv['id'].replace(':', '_').replace('/', '_')}_{index}"
            if not criterion.effects:
                effects = "nullptr"
            effect_count = len(criterion.effects)
            out.append(
                "  {" + ", ".join([
                    cstr(criterion.name),
                    criterion.trigger,
                    str(criterion.flags),
                    items,
                    str(item_count),
                    cstr(criterion.recipe),
                    cstr(criterion.entity),
                    cstr(criterion.child),
                    cstr(criterion.parent),
                    cstr(criterion.partner),
                    biomes,
                    str(biome_count),
                    blocks,
                    str(block_count),
                    effects,
                    str(effect_count),
                ]) + "},")
    out.append("};")
    out.append("")

    # Requirement sets, flattened.
    sizes = []
    flat = []
    for adv in advancements:
        adv["requirement_sets_first"] = len(sizes)
        adv["requirement_set_count"] = len(adv["requirements"])
        adv["requirement_criteria_first"] = len(flat)
        for requirement in adv["requirements"]:
            indices = []
            for name in requirement:
                match = [i for i, c in enumerate(adv["criteria"]) if c.name == name]
                if not match:
                    raise SystemExit(f"{adv['id']}: requirement names unknown criterion {name}")
                indices.append(match[0])
            sizes.append(len(indices))
            flat.extend(indices)
    emit_array(out, "inline constexpr uint8_t kAdvancementRequirementSizes[] = {",
               "uint8_t", [str(size) for size in sizes], per_line=12)
    emit_array(out, "inline constexpr uint8_t kAdvancementRequirementCriteria[] = {",
               "uint8_t", [str(index) for index in flat], per_line=16)

    # Display rows.
    out.append("inline constexpr GeneratedAdvancementDisplay kAdvancementDisplays[] = {")
    display_index = {}
    for adv in advancements:
        if adv["display"] is None:
            continue
        display = adv["display"]
        display_index[adv["id"]] = len(display_index)
        out.append("  {" + ", ".join([
            cstr(display["title"]),
            cstr(display["description"]),
            cstr(display["icon"]),
            str(display["icon_count"]),
            str(display["frame"]),
            cstr(display["background"]),
            cbool(display["show_toast"]),
            cbool(display["announce_chat"]),
            cbool(display["hidden"]),
            f"{display['x']:.6f}f",
            f"{display['y']:.6f}f",
        ]) + "},")
    out.append("};")
    out.append("")

    # Reward recipe arrays.
    for adv in advancements:
        recipes = adv["rewards"].get("recipes", [])
        if recipes:
            emit_array(out, f"inline constexpr const char* const kAdvRewards_{adv['id'].replace(':', '_').replace('/', '_')}[] = {{",
                       "const char*", [cstr(recipe) for recipe in recipes])

    # Advancement rows.
    out.append("inline constexpr GeneratedAdvancement kAdvancements[] = {")
    for adv in advancements:
        recipes = adv["rewards"].get("recipes", [])
        recipe_ref = (
            f"kAdvRewards_{adv['id'].replace(':', '_').replace('/', '_')}" if recipes else "nullptr"
        )
        display_ref = (
            f"&kAdvancementDisplays[{display_index[adv['id']]}]" if adv["id"] in display_index else "nullptr"
        )
        out.append("  {" + ", ".join([
            cstr(adv["id"]),
            cstr(adv["parent"]),
            str(adv["criteria_first"]),
            str(len(adv["criteria"])),
            str(adv["requirement_sets_first"]),
            str(adv["requirement_criteria_first"]),
            str(adv["requirement_set_count"]),
            display_ref,
            recipe_ref,
            str(len(recipes)),
            str(int(adv["rewards"].get("experience", 0))),
            cbool(adv["telemetry"]),
        ]) + "},")
    out.append("};")
    out.append("")
    out.append(f"inline constexpr int kAdvancementCount = {len(advancements)};")
    out.append(f"inline constexpr int kAdvancementCriteriaCount = {criteria_count};")
    out.append(f"inline constexpr int kAdvancementRequirementSetCount = {len(sizes)};")
    out.append(f"inline constexpr int kAdvancementRequirementCriteriaCount = {len(flat)};")
    out.append(f"inline constexpr int kAdvancementDisplayCount = {len(display_index)};")
    out.append("")
    out.append("} // namespace mc::world")
    out.append("")
    pathlib.Path(args.out).write_text("\n".join(out))
    print(f"{args.out}: {len(advancements)} advancements, {criteria_count} criteria, "
          f"{len(display_index)} displays, {unsupported} criteria unmodelled")


if __name__ == "__main__":
    main()
