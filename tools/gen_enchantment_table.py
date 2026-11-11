#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/world/enchantment_table.h — the 26.2 minecraft:enchantment registry.

Sources, all of them data rather than code:

  data/minecraft/enchantment/*.json        the 43 enchantments: max_level,
                                           slots, supported_items,
                                           exclusive_set, the
                                           `minecraft:damage_protection`
                                           entries with the
                                           `damage_source_properties` predicate
                                           that gates each one, and the melee
                                           package: `minecraft:damage` (with
                                           the `entity_properties` predicate
                                           Smite and Bane gate on),
                                           `minecraft:knockback`, the mace's
                                           `minecraft:smash_damage_per_fallen_block`
                                           (Density), and the
                                           `minecraft:attributes` add_value on
                                           sweeping_damage_ratio, and the
                                           `minecraft:post_attack` entries that
                                           land on the victim (Fire Aspect's
                                           ignite, Bane of Arthropods' Slowness)
  data/minecraft/tags/entity_type/*.json   the tags those damage predicates
                                           name, expanded (nested `#tag`
                                           included) into the generated
                                           src/world/entity_type_tags.h
  data/minecraft/enchantment/tags/*.json   `exclusive_set/*`, expanded to
                                           enchantment ids
  data/minecraft/tags/item/**/*.json       the `enchantable/*` tags behind
                                           supported_items, expanded to item
                                           registry ids
  clientconfig/registries/enchantment.txt  the wire order (`minecraft:enchantment`
                                           is a *datapack* registry, so unlike
                                           mob_effect there is no built-in
                                           registration order — the id in the
                                           `minecraft:enchantments` item
                                           component is this index)
  reports/registries.json                  the
                                           `minecraft:enchantment_effect_component_type`
                                           ids, so the generated `effects`
                                           bitmask can name which components an
                                           enchantment carries

The generator refuses to run if the json directory and the captured wire order
disagree, and refuses any effect component or requirement condition it does not
model — a silent skip there would read as a working enchantment that does
nothing.

Usage:
  tools/gen_enchantment_table.py \\
      --data data/vanilla/26.2/data \\
      --order data/vanilla/26.2/clientconfig/registries/enchantment.txt \\
      --items data/vanilla/26.2/item_components.tsv \\
      --reports build/_dl/boot/generated/reports
"""

import argparse
import json
import pathlib
import sys

_NS = "minecraft:"

# EquipmentSlotGroup -> the bitmask the generated header uses. The slot bits are
# the header's own order (mainhand, offhand, feet, legs, chest, head, body,
# saddle), not the protocol's.
_GROUPS = {
    "any": 0xFF,
    "mainhand": 1 << 0,
    "offhand": 1 << 1,
    "hand": (1 << 0) | (1 << 1),
    "feet": 1 << 2,
    "legs": 1 << 3,
    "chest": 1 << 4,
    "head": 1 << 5,
    "armor": (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5),
    "body": 1 << 6,
    "saddle": 1 << 7,
}


def camel(name: str) -> str:
    return "".join(p.capitalize() for p in name.split(":", 1)[-1].split("_"))


def load_jsons(directory: pathlib.Path) -> dict:
    out = {}
    for path in sorted(directory.glob("*.json")):
        out[_NS + path.stem] = json.loads(path.read_text())
    return out


def load_tags(directory: pathlib.Path) -> dict:
    """{minecraft:enchantable/armor: parsed json} for a nested tag directory."""
    out = {}
    for path in sorted(directory.rglob("*.json")):
        rel = path.relative_to(directory).with_suffix("").as_posix()
        out[_NS + rel] = json.loads(path.read_text())
    return out


def expand_tag(tag: str, tags: dict, seen: set) -> list:
    """A tag's members with `#other_tag` references resolved, deduplicated in order."""
    if tag in seen:
        return []
    seen.add(tag)
    if tag not in tags:
        raise SystemExit(f"unknown tag {tag}")
    out = []
    for value in tags[tag]["values"]:
        if value.startswith("#"):
            out += expand_tag(value[1:], tags, seen)
        elif isinstance(value, dict):
            # A tag entry may carry `required: false` or an entry list.
            if "id" not in value:
                raise SystemExit(f"{tag}: unrecognized tag entry {value!r}")
            if value.get("required", True) is False:
                continue
            out.append(value["id"])
        else:
            out.append(value)
    # De-duplicate, keeping first-seen order.
    seen_values, unique = set(), []
    for v in out:
        if v not in seen_values:
            seen_values.add(v)
            unique.append(v)
    return unique


def resolve_holder_set(spec, tags: dict) -> list:
    """A `supported_items`/`exclusive_set` value: a tag, one element, or a list."""
    if spec is None:
        return []
    if isinstance(spec, list):
        out = []
        for item in spec:
            out += resolve_holder_set(item, tags)
        return out
    if spec.startswith("#"):
        return expand_tag(spec[1:], tags, set())
    return [spec]


def flatten_requirements(req: dict, out: dict, who: str) -> None:
    """Folds a requirement tree into the (required, excluded) damage-tag sets.

    Only the two conditions the vanilla data actually uses for
    `minecraft:damage_protection` are modelled: `damage_source_properties` (a
    list of `{id, expected}` damage-type tags) and `all_of` over them (Fire
    Protection wraps its single term in one). Anything else is refused.
    """
    # 26.3 renamed the requirement discriminant from `condition` to `type`;
    # accept both so the same generator serves the 26.2 and 26.3 packs.
    condition = req.get("type", req.get("condition"))
    if condition == "minecraft:all_of":
        for term in req["terms"]:
            flatten_requirements(term, out, who)
        return
    if condition != "minecraft:damage_source_properties":
        raise SystemExit(f"{who}: unmodelled requirement condition {condition!r}")
    predicate = req["predicate"]
    extra = set(predicate) - {"tags"}
    if extra:
        raise SystemExit(f"{who}: unmodelled damage-source predicate keys {sorted(extra)}")
    for entry in predicate.get("tags", []):
        tag, expected = entry["id"], entry["expected"]
        # 26.3 writes tag ids with the `#` prefix here; 26.2 omitted it.
        if tag.startswith("#"):
            tag = tag[1:]
        (out["required"] if expected else out["excluded"]).add(tag)


def linear(value: dict, who: str, what: str) -> tuple:
    """A `minecraft:linear` level-based value: (base, per_level_above_first).

    `Enchantment.applyEffects` processes one value effect per level; every value
    the four modelled components use is linear, so a constant or a
    `minecraft:exponent`/
    `minecraft:lookup` form is refused rather than quietly flattened.
    """
    if not isinstance(value, dict) or value.get("type") != "minecraft:linear":
        raise SystemExit(f"{who}: unmodelled {what} value {value.get('type')!r}")
    return (float(value["base"]), float(value["per_level_above_first"]))


def parse_value_effects(entries: list, who: str, what: str) -> list:
    """`minecraft:damage`/`minecraft:knockback`: (base, per, tag, tested entity).

    `Enchantment.modifyDamage` -> `modifyDamageFilteredValue` applies each entry
    as an `add` gated by its `requirements`, which `Enchantment.damageContext`
    evaluates against the loot context of the hit: `entity: "this"` is the
    *victim* (`LootContextParams.THIS_ENTITY`) and `entity: "direct_attacker"`
    is the entity that dealt it. Sharpness carries no requirement; Smite and
    Bane of Arthropods gate on the victim's type being in a `sensitive_to_*` tag;
    Power gates on the *attacker* being in `#minecraft:arrows`, which a melee
    swing by a player never is — modelled, so it stays inert here for the reason
    vanilla gives rather than by omission.

    The tested entity is returned as `"this"` / `"direct_attacker"`, or None for
    no requirement at all; anything else is refused.
    """
    out = []
    for entry in entries:
        effect = entry.get("effect", {})
        if effect.get("type") != "minecraft:add":
            raise SystemExit(f"{who}: unmodelled damage effect type {effect.get('type')!r}")
        base, per = linear(effect.get("value", {}), who, what)
        tag = None
        tested = None
        if "requirements" in entry:
            req = entry["requirements"]
            if req.get("type", req.get("condition")) != "minecraft:entity_properties":
                raise SystemExit(f"{who}: unmodelled {what} requirement {req!r}")
            tested = req.get("entity")
            if tested not in ("this", "direct_attacker"):
                raise SystemExit(f"{who}: unmodelled {what} requirement entity "
                                 f"{tested!r}")
            predicate = req.get("predicate", {})
            if set(predicate) != {"minecraft:entity_type"}:
                raise SystemExit(f"{who}: unmodelled {what} entity predicate "
                                 f"{sorted(predicate)}")
            spec = predicate["minecraft:entity_type"]
            if not isinstance(spec, str) or not spec.startswith("#"):
                raise SystemExit(f"{who}: {what} requirement names a bare entity type "
                                 f"{spec!r}")
            tag = spec[1:]
        out.append((base, per, tag, tested))
    return out


def parse_sweeping_ratio(entries: list, who: str) -> tuple:
    """Sweeping Edge's sweep ratio as the four linear parameters of a fraction.

    `minecraft:attributes` with `operation: add_value` adds `amount` to the
    attribute, and Sweeping Edge's amount is `minecraft:fraction` of two linear
    expressions, which is why the level's ratio is `level / (level + 1)` and not
    a table. Every other attribute an enchantment carries (Depth Strider's
    movement efficiency, Soul Speed's speed) stays unmodelled — the `effects`
    bitmask is what makes that visible — so only `sweeping_damage_ratio` is read
    here.
    """
    out = (0.0, 0.0, 0.0, 0.0)
    for entry in entries:
        if entry.get("attribute") != _NS + "sweeping_damage_ratio":
            continue
        if entry.get("operation") != "add_value":
            raise SystemExit(f"{who}: unmodelled sweeping ratio operation "
                             f"{entry.get('operation')!r}")
        amount = entry.get("amount", {})
        if not isinstance(amount, dict) or amount.get("type") != "minecraft:fraction":
            raise SystemExit(f"{who}: unmodelled sweeping ratio amount "
                             f"{amount.get('type')!r}")
        num = linear(amount["numerator"], who, "sweeping numerator")
        den = linear(amount["denominator"], who, "sweeping denominator")
        out = (num[0], num[1], den[0], den[1])
    return out


def level_value(spec, who: str, what: str) -> tuple:
    """A `LevelBasedValue`: a bare number is `minecraft:constant`, and a dict is
    a linear (the only two forms the post-attack data uses)."""
    if isinstance(spec, (int, float)):
        return (float(spec), 0.0)
    return linear(spec, who, what)


def parse_post_attack(entries: list, who: str, skipped: list) -> list:
    """The `minecraft:post_attack` entries this server lands on the victim.

    EnchantmentHelper.doPostAttackEffects applies each entry after a hit lands,
    to whichever entity `affected` names, using the enchantment level of
    whichever entity `enchanted` names. Only `enchanted: attacker` /
    `affected: victim` entries are this hook's to apply; the other three vanilla
    carriers reach past it — Thorns and Wind Burst affect the *attacker* (a
    damage-entity and an explosion), Channeling summons a lightning bolt under a
    weather check — so they stay carried only as the `effects` bit.

    Of the two that do land here, the effect types are `minecraft:ignite`
    (Fire Aspect, one level-based duration in seconds) and
    `minecraft:apply_mob_effect` (Bane of Arthropods' Slowness IV, whose
    duration and amplifier are each a `[min, max]` pair vanilla rolls between —
    at level I the duration pair is a point, 1.5 s, and at level V it is
    1.5-3.5 s). Both require `is_direct` (a melee swing always is) and Bane also
    gates on the victim's type, which is the same `entity_properties` test the
    damage entries use.

    Returns `{kind, effect, duration_min, duration_max, amplifier_min,
    amplifier_max, tag, tested}` per applied entry, in data order; the
    durations are in *seconds* and the amplifiers in levels.
    """
    out = []
    for entry in entries:
        if entry.get("affected") != "victim" or entry.get("enchanted") != "attacker":
            continue
        effect = entry.get("effect", {})
        kind = effect.get("type")
        if kind not in ("minecraft:ignite", "minecraft:apply_mob_effect"):
            # A victim-facing entry whose effect class this server has no home
            # for — Channeling's `all_of(summon_entity, play_sound)`. It stays
            # carried only as the `effects` bit, and `skipped` reports it so the
            # generator says so out loud instead of implying it applies.
            skipped.append((who, kind))
            continue
        found = {"tag": None, "tested": None}

        def walk(node):
            condition = node.get("type", node.get("condition"))
            if condition == "minecraft:all_of":
                for term in node["terms"]:
                    walk(term)
                return
            if condition == "minecraft:damage_source_properties":
                predicate = node["predicate"]
                if set(predicate) != {"is_direct"} or predicate["is_direct"] is not True:
                    raise SystemExit(f"{who}: unmodelled post-attack source predicate "
                                     f"{predicate!r}")
                return
            if condition == "minecraft:entity_properties":
                tested = node.get("entity")
                if tested != "this":
                    raise SystemExit(f"{who}: unmodelled post-attack requirement entity "
                                     f"{tested!r}")
                predicate = node.get("predicate", {})
                if set(predicate) != {"minecraft:entity_type"}:
                    raise SystemExit(f"{who}: unmodelled post-attack entity predicate "
                                     f"{sorted(predicate)}")
                spec = predicate["minecraft:entity_type"]
                if not isinstance(spec, str) or not spec.startswith("#"):
                    raise SystemExit(f"{who}: post-attack requirement names a bare "
                                     f"entity type {spec!r}")
                found["tag"], found["tested"] = spec[1:], tested
                return
            raise SystemExit(f"{who}: unmodelled post-attack requirement "
                             f"condition {condition!r}")

        if "requirements" in entry:
            walk(entry["requirements"])
        if kind == "minecraft:ignite":
            duration = level_value(effect.get("duration", {}), who, "ignite duration")
            out.append({"kind": "kPostAttackIgnite", "effect": None,
                        "duration_min": duration, "duration_max": duration,
                        "amplifier_min": (0.0, 0.0), "amplifier_max": (0.0, 0.0),
                        "tag": found["tag"], "tested": found["tested"]})
            continue
        if kind == "minecraft:apply_mob_effect":
            out.append({"kind": "kPostAttackApplyMobEffect",
                        "effect": effect["to_apply"],
                        "duration_min": level_value(effect["min_duration"], who,
                                                    "mob effect duration"),
                        "duration_max": level_value(effect["max_duration"], who,
                                                    "mob effect duration"),
                        "amplifier_min": level_value(effect["min_amplifier"], who,
                                                     "mob effect amplifier"),
                        "amplifier_max": level_value(effect["max_amplifier"], who,
                                                     "mob effect amplifier"),
                        "tag": found["tag"], "tested": found["tested"]})
            continue
        raise SystemExit(f"{who}: unmodelled post-attack effect type {kind!r}")
    return out


def parse_thorns(entries: list, who: str, skipped: list) -> list:
    """The `minecraft:post_attack` entries the *victim's* own equipment lands on
    the attacker, which is Thorns.

    `EnchantmentHelper.doPostAttackEffectsWithItemSourceOnBreak` walks the
    victim's equipment with `EnchantmentTarget.VICTIM`, so an entry whose
    `enchanted` is `victim` and whose `affected` is `attacker` is the victim's
    enchantment acting on whoever just hit it: `Enchantment.doPostAttack`
    resolves `attacker` to `damageSource.getEntity()` and hands the entry's
    effect that entity. (The attacker's weapon is walked in the same call with
    `EnchantmentTarget.ATTACKER`, which is the victim-facing list above.)

    One entry, one effect class: Thorns' `minecraft:all_of` of
    `minecraft:damage_entity` (1..5 damage, `minecraft:thorns`) and
    `minecraft:change_item_damage` (2 durability), gated by a
    `minecraft:random_chance` whose probability is `0.15 x level`. Wind Burst's
    entry is `enchanted: attacker`, so it is not the victim's to land (an
    explosion has no home here either) and stays carried only as the `effects`
    bit; `skipped` names every entry this hook does not take.

    Returns `{damage_type, min_damage, max_damage, item_damage, chance}` per
    applied entry, in data order, each value a `(base, per_level)` pair except
    the damage type's name.
    """
    out = []
    for entry in entries:
        if entry.get("affected") != "attacker" or entry.get("enchanted") != "victim":
            continue
        effect = entry.get("effect", {})
        if effect.get("type") != "minecraft:all_of":
            skipped.append((who, effect.get("type")))
            continue
        damage = None
        item_damage = None
        for term in effect.get("effects", []):
            term_type = term.get("type")
            if term_type == "minecraft:damage_entity":
                damage = term
            elif term_type == "minecraft:change_item_damage":
                item_damage = term
            else:
                raise SystemExit(f"{who}: unmodelled victim post-attack effect "
                                 f"{term_type!r}")
        if damage is None:
            raise SystemExit(f"{who}: victim post-attack entry with no damage_entity")
        requirements = entry.get("requirements", {})
        if requirements.get("type", requirements.get("condition")) != "minecraft:random_chance":
            raise SystemExit(f"{who}: unmodelled victim post-attack requirement "
                             f"{requirements.get('type', requirements.get('condition'))!r}")
        chance = requirements.get("chance", {})
        if chance.get("type") != "minecraft:enchantment_level":
            raise SystemExit(f"{who}: unmodelled victim post-attack chance "
                             f"{chance.get('type')!r}")
        out.append({
            "damage_type": damage["damage_type"],
            "min_damage": level_value(damage["min_damage"], who, "thorns damage"),
            "max_damage": level_value(damage["max_damage"], who, "thorns damage"),
            "item_damage": (level_value(item_damage["amount"], who, "thorns item damage")
                            if item_damage is not None else (0.0, 0.0)),
            "chance": level_value(chance["amount"], who, "thorns chance"),
        })
    return out


def parse_protections(entries: list, who: str) -> list:
    out = []
    for entry in entries:
        effect = entry.get("effect", {})
        if effect.get("type") != "minecraft:add":
            raise SystemExit(f"{who}: unmodelled protection effect type "
                             f"{effect.get('type')!r}")
        value = effect.get("value", {})
        if value.get("type") != "minecraft:linear":
            raise SystemExit(f"{who}: unmodelled level-based value "
                             f"{value.get('type')!r}")
        tags = {"required": set(), "excluded": set()}
        if "requirements" in entry:
            flatten_requirements(entry["requirements"], tags, who)
        out.append((float(value["base"]), float(value["per_level_above_first"]),
                    tags["required"], tags["excluded"]))
    return out


def rows(enchantments: dict, order: list, item_tags: dict, enchant_tags: dict,
         item_ids: dict, component_ids: dict, skipped: list) -> list:
    if sorted(enchantments) != sorted(order):
        missing = sorted(set(enchantments) - set(order))
        extra = sorted(set(order) - set(enchantments))
        raise SystemExit(f"enchantment wire order disagrees with the data: "
                         f"missing={missing} extra={extra}")
    out = []
    for name in order:
        d = enchantments[name]
        slots = 0
        for group in d.get("slots", []):
            if group not in _GROUPS:
                raise SystemExit(f"{name}: unknown equipment slot group {group!r}")
            slots |= _GROUPS[group]
        components = 0
        for component in d.get("effects", {}):
            if component not in component_ids:
                raise SystemExit(f"{name}: unknown effect component {component!r}")
            components |= 1 << component_ids[component]
        items = []
        for item in resolve_holder_set(d.get("supported_items"), item_tags):
            if item not in item_ids:
                raise SystemExit(f"{name}: supported_items names unknown item {item!r}")
            items.append(item_ids[item])
        exclusive = []
        for other in resolve_holder_set(d.get("exclusive_set"), enchant_tags):
            if other not in order:
                raise SystemExit(f"{name}: exclusive_set names unknown enchantment "
                                 f"{other!r}")
            exclusive.append(order.index(other))
        out.append({
            "name": name,
            "slot_mask": slots,
            "components": components,
            "items": sorted(set(items)),
            "exclusive": sorted(set(exclusive)),
            "protections": parse_protections(
                d.get("effects", {}).get("minecraft:damage_protection", []), name),
            "damage": parse_value_effects(
                d.get("effects", {}).get("minecraft:damage", []), name, "damage"),
            "knockback": parse_value_effects(
                d.get("effects", {}).get("minecraft:knockback", []), name, "knockback"),
            "smash": parse_value_effects(
                d.get("effects", {}).get("minecraft:smash_damage_per_fallen_block", []),
                name, "smash damage"),
            "sweeping": parse_sweeping_ratio(
                d.get("effects", {}).get("minecraft:attributes", []), name),
            "post_attack": parse_post_attack(
                d.get("effects", {}).get("minecraft:post_attack", []), name, skipped),
            "thorns": parse_thorns(
                d.get("effects", {}).get("minecraft:post_attack", []), name, skipped),
            "max_level": int(d["max_level"]),
        })
    return out


def render(rows: list, component_ids: dict, damage_tag_bits: dict) -> str:
    protection_id = component_ids["minecraft:damage_protection"]
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        f"// The {len(rows)} 26.3 minecraft:enchantment entries, in wire order",
        "// (generated — do not hand-edit).",
        "//",
        "// minecraft:enchantment is a *datapack* registry, so the id is the index",
        "// the captured wire order (clientconfig/registries/enchantment.txt)",
        "// assigns; that index is what the `minecraft:enchantments` item component",
        "// carries on the wire.",
        "//",
        "// `effects` is a bitmask over minecraft:enchantment_effect_component_type,",
        "// keyed by that registry's own protocol id (reports/registries.json), so",
        "// the bits line up with the component names rather than with a local",
        "// ordering. Five of them are modelled, as value effects with a linear",
        "// level term:",
        f"//   kEffectComponentDamageProtection (bit {protection_id}): the",
        "//     minecraft:damage_protection entries this header carries;",
        "//   minecraft:damage and minecraft:knockback: the `damage_add` /",
        "//     `knockback_add` fields, the melee hooks Sharpness, Smite, Bane of",
        "//     Arthropods and Knockback are (world/combat.cpp applies them);",
        "//   minecraft:smash_damage_per_fallen_block: the `smash_damage_add`",
        "//     field, Density's damage per block fallen, which world/combat.cpp",
        "//     multiplies by the fall distance of a mace smash;",
        "//   minecraft:attributes: only `sweeping_damage_ratio`, as the",
        "//     `sweeping_num`/`sweeping_den` fraction Sweeping Edge scales the",
        "//     sweep with;",
        "//   minecraft:post_attack: the entries that land on the *victim* — Fire",
        "//     Aspect's ignite and Bane of Arthropods' Slowness IV — as the",
        "//     `post_attack` list (world/enchantment.cpp resolves them for the",
        "//     level the attacker's weapon carries).",
        "// The rest (minecraft:projectile_piercing, a post-attack entry that",
        "// affects the attacker or needs an entity or an explosion, ...) are",
        "// recorded so the bits can be *tested* — see docs/PHASE3.md for what that",
        "// means for each enchantment.",
        "//",
        "// A `minecraft:damage_protection` entry is a `minecraft:linear` value",
        "// (base + per_level_above_first * (level - 1)) gated by a",
        "// `damage_source_properties` damage-type tag predicate. required_tags must",
        "// all be set on the damage type and excluded_tags must all be clear, which",
        "// is how Protection covers everything while Blast Protection covers only",
        "// minecraft:is_explosion.",
        "//",
        "// Regenerate with tools/gen_enchantment_table.py.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "#include \"world/damage_table.h\"",
        "",
        "namespace mc::world {",
        "",
        "// EquipmentSlotGroup, as a bit per equipment slot. The mask exists only to",
        "// answer Enchantment.matchingSlot / EquipmentSlotGroup.test: which slots an",
        "// enchantment's effects apply in.",
        "constexpr uint8_t kEnchantSlotMainhand = 1 << 0;",
        "constexpr uint8_t kEnchantSlotOffhand = 1 << 1;",
        "constexpr uint8_t kEnchantSlotFeet = 1 << 2;",
        "constexpr uint8_t kEnchantSlotLegs = 1 << 3;",
        "constexpr uint8_t kEnchantSlotChest = 1 << 4;",
        "constexpr uint8_t kEnchantSlotHead = 1 << 5;",
        "constexpr uint8_t kEnchantSlotBody = 1 << 6;",
        "constexpr uint8_t kEnchantSlotSaddle = 1 << 7;",
        "constexpr uint8_t kEnchantSlotHand = kEnchantSlotMainhand | kEnchantSlotOffhand;",
        "constexpr uint8_t kEnchantSlotArmor = kEnchantSlotFeet | kEnchantSlotLegs |",
        "                                          kEnchantSlotChest | kEnchantSlotHead;",
        "constexpr uint8_t kEnchantSlotAny = 0xFF;",
        "",
        "// The enchantment effect components this server models: the four value",
        "// effects with a linear level term (see above).",
        f"constexpr int kEffectComponentDamageProtection = {protection_id};",
        f"constexpr int kEffectComponentDamage = {component_ids['minecraft:damage']};",
        f"constexpr int kEffectComponentKnockback = {component_ids['minecraft:knockback']};",
        "constexpr int kEffectComponentSmashDamagePerFallenBlock = "
        f"{component_ids['minecraft:smash_damage_per_fallen_block']};",
        f"constexpr int kEffectComponentAttributes = {component_ids['minecraft:attributes']};",
        "constexpr int kEffectComponentPostAttack = "
        f"{component_ids['minecraft:post_attack']};",
        "",
        "// The two minecraft:post_attack effect types this server lands on the",
        "// victim. Entries that summon an entity or an explosion (Channeling's",
        "// summon, Wind Burst) are carried only as the `effects` bit above; the",
        "// generator names every such entry as it writes this header, so none of",
        "// them is silently inert. Thorns is neither: it is the one entry aimed",
        "// at the *attacker* from the victim's own equipment, and it is the",
        "// `thorns` list below rather than a bit with no home.",
        "constexpr uint8_t kPostAttackIgnite = 1;          // minecraft:ignite",
        "constexpr uint8_t kPostAttackApplyMobEffect = 2;  // minecraft:apply_mob_effect",
        "",
        "struct GeneratedProtection {",
        "  float base;                    // LevelBasedValue.linear base",
        "  float per_level_above_first;   // ...and its per_level_above_first",
        "  uint64_t required_tags;        // every kDamageTag* bit must be set",
        "  uint64_t excluded_tags;        // ...and every one here must be clear",
        "};",
        "",
        "// Which entity a minecraft:damage requirement tests its entity-type tag",
        "// against: Enchantment.damageContext's loot context carries both, and the",
        "// vanilla data uses both (Smite names `this`, Power `direct_attacker`).",
        "constexpr uint8_t kDamageTagEntityNone = 0;           // no requirement",
        "constexpr uint8_t kDamageTagEntityVictim = 1;         // entity: this",
        "constexpr uint8_t kDamageTagEntityDirectAttacker = 2; // entity: direct_attacker",
        "",
        "// A `minecraft:linear` level-based value: base + per_level_above_first x",
        "// (level - 1).",
        "struct GeneratedLinear {",
        "  float base;",
        "  float per_level_above_first;",
        "};",
        "",
        "// One minecraft:post_attack entry, as Enchantment.applyEffects would",
        "// compute it for a level: `kind` says which effect, the duration pair is",
        "// in *seconds* (the two are equal for ignite, which has one duration and",
        "// no roll) and the amplifier pair in levels, and `tag`/`tag_entity` are",
        "// the entity-properties requirement the entry is gated on",
        "// (kDamageTagEntityNone is unconditional).",
        "struct GeneratedPostAttack {",
        "  uint8_t kind;                  // kPostAttack*",
        "  const char* effect;            // apply_mob_effect's to_apply; nullptr",
        "                                 //   for ignite",
        "  GeneratedLinear duration_min;  // seconds, linear in the level",
        "  GeneratedLinear duration_max;  // ...the pair vanilla rolls between",
        "  GeneratedLinear amplifier_min;  // levels, likewise",
        "  GeneratedLinear amplifier_max;",
        "  const char* tag;               // the entity-type tag it is gated on",
        "  uint8_t tag_entity;            // ...and which entity tests it",
        "};",
        "",
        "// One minecraft:post_attack entry the *victim's* equipment lands on its",
        "// attacker (Thorns). `Enchantment.doPostAttack` resolves `affected:",
        "// attacker` to `damageSource.getEntity()` and gives the effect that",
        "// entity; DamageEntity then rolls `Mth.randomBetween(min, max)` and hurts",
        "// it with a source whose *causing* entity is the enchanted item's owner —",
        "// the victim — and ChangeItemDamage takes `item_damage` off the enchanted",
        "// piece. `chance` is the random_chance requirement the entry is gated on",
        "// before any of that runs. Every value is linear in the level.",
        "struct GeneratedThorns {",
        "  const char* damage_type;       // minecraft:thorns",
        "  GeneratedLinear min_damage;    // Mth.randomBetween's bounds, by level",
        "  GeneratedLinear max_damage;",
        "  GeneratedLinear item_damage;   // change_item_damage, in durability",
        "  GeneratedLinear chance;        // random_chance, 0.15 x level",
        "};",
        "",
        "struct GeneratedEnchantment {",
        "  const char* name;              // minecraft:protection",
        "  uint8_t max_level;             // Enchantment.getMaxLevel",
        "  uint8_t slots;                 // kEnchantSlot* mask",
        "  uint32_t effects;              // bit per effect component protocol id",
        "  const GeneratedProtection* protections;",
        "  int protection_count;",
        "  const uint32_t* items;         // supported_items, as item registry ids",
        "  int item_count;",
        "  const uint32_t* exclusive_set; // exclusive_set, as enchantment ids",
        "  int exclusive_count;",
        "  // The modelled melee value effects; all-zero when the enchantment does",
        "  // not carry the component (see `effects`).",
        "  GeneratedLinear damage_add;    // minecraft:damage",
        "  const char* damage_tag;        // ...gated on an entity's type being in this",
        "                                 // tag; nullptr is unconditional",
        "  uint8_t damage_tag_entity;     // ...and which entity the tag is tested",
        "                                 // against (kDamageTagEntity*)",
        "  GeneratedLinear knockback_add; // minecraft:knockback",
        "  const char* knockback_tag;     // ...gated the same way as damage_add",
        "  uint8_t knockback_tag_entity;",
        "  // minecraft:smash_damage_per_fallen_block (Density): the damage per",
        "  // block fallen, which the mace's smash multiplies by the fall distance.",
        "  GeneratedLinear smash_damage_add;",
        "  const char* smash_damage_tag;  // ...gated like damage_add",
        "  uint8_t smash_damage_tag_entity;",
        "  GeneratedLinear sweeping_num;  // minecraft:attributes on",
        "  GeneratedLinear sweeping_den;  //   sweeping_damage_ratio, as a fraction",
        "  // minecraft:post_attack's victim-facing entries, in data order; all-zero",
        "  // (with a null list) for every other enchantment.",
        "  const GeneratedPostAttack* post_attack;",
        "  int post_attack_count;",
        "  // ...and the attacker-facing ones, which only the *victim's* equipment",
        "  // carries (Thorns); null for every other enchantment.",
        "  const GeneratedThorns* thorns;",
        "  int thorns_count;",
        "};",
        "",
    ]
    def flit(value: float) -> str:
        # `2f` is not a float literal (it reads as a bad integer suffix), so keep
        # a decimal point on whole numbers.
        text = f"{value:.7g}"
        if "." not in text and "e" not in text and "E" not in text:
            text += ".0"
        return text + "f"

    def pair_field(pair: tuple) -> str:
        return "{" + flit(pair[0]) + ", " + flit(pair[1]) + "}"

    for r in rows:
        symbol = "k" + camel(r["name"])
        if r["protections"]:
            lines.append("// clang-format off")
            lines.append(f"inline constexpr GeneratedProtection {symbol}Protections[] = {{")
            for base, per, req, exc in r["protections"]:
                def bits(tags):
                    value = 0
                    for tag in sorted(tags):
                        if tag not in damage_tag_bits:
                            raise SystemExit(f"{r['name']}: unknown damage tag {tag!r}")
                        value |= 1 << damage_tag_bits[tag]
                    return f"0x{value:016x}ull"

                lines.append(f"    {{{flit(base)}, {flit(per)}, {bits(req)}, {bits(exc)}}},")
            lines.append("};")
            lines.append("// clang-format on")
        if r["items"]:
            lines.append("// clang-format off")
            lines.append(f"inline constexpr uint32_t {symbol}Items[] = {{")
            for i in range(0, len(r["items"]), 12):
                lines.append("    " + " ".join(f"{v}," for v in r["items"][i:i + 12]))
            lines.append("};")
            lines.append("// clang-format on")
        if r["exclusive"]:
            lines.append("// clang-format off")
            lines.append(f"inline constexpr uint32_t {symbol}Exclusive[] = {{"
                         + " ".join(f"{v}," for v in r["exclusive"]) + "};")
            lines.append("// clang-format on")
        if r["post_attack"]:
            lines.append("// clang-format off")
            lines.append(f"inline constexpr GeneratedPostAttack {symbol}PostAttack[] = {{")
            for pa in r["post_attack"]:
                effect = f'"{pa["effect"]}"' if pa["effect"] else "nullptr"
                tag = f'"{pa["tag"]}"' if pa["tag"] else "nullptr"
                tested = ("kDamageTagEntityVictim" if pa["tested"] == "this"
                          else "kDamageTagEntityNone")
                lines.append(f"    {{{pa['kind']}, {effect}, "
                             f"{pair_field(pa['duration_min'])}, "
                             f"{pair_field(pa['duration_max'])}, "
                             f"{pair_field(pa['amplifier_min'])}, "
                             f"{pair_field(pa['amplifier_max'])}, "
                             f"{tag}, {tested}}},")
            lines.append("};")
            lines.append("// clang-format on")
        if r["thorns"]:
            lines.append("// clang-format off")
            lines.append(f"inline constexpr GeneratedThorns {symbol}Thorns[] = {{")
            for t in r["thorns"]:
                damage_type = '"' + t["damage_type"] + '"'
                lines.append(f"    {{{damage_type}, "
                             f"{pair_field(t['min_damage'])}, "
                             f"{pair_field(t['max_damage'])}, "
                             f"{pair_field(t['item_damage'])}, "
                             f"{pair_field(t['chance'])}}},")
            lines.append("};")
            lines.append("// clang-format on")
    lines += [
        "",
        "// clang-format off",
        "// The table index is the wire id.",
        "inline constexpr GeneratedEnchantment kEnchantments[] = {",
    ]
    def flit(value: float) -> str:
        text = f"{value:.7g}"
        if "." not in text and "e" not in text and "E" not in text:
            text += ".0"
        return text + "f"

    def linear_field(entries: list) -> str:
        # One entry per component in the vanilla data, so a second one would be a
        # silent drop; the generator refuses instead.
        if len(entries) > 1:
            raise SystemExit("more than one entry for a modelled component")
        base, per = entries[0][:2] if entries else (0.0, 0.0)
        return "{" + flit(base) + ", " + flit(per) + "}"

    def tag_field(entries: list) -> str:
        tag = entries[0][2] if entries else None
        return '"' + tag + '"' if tag else "nullptr"

    def tag_entity_field(entries: list) -> str:
        tested = entries[0][3] if entries else None
        if tested == "this":
            return "kDamageTagEntityVictim"
        if tested == "direct_attacker":
            return "kDamageTagEntityDirectAttacker"
        return "kDamageTagEntityNone"

    def fraction_field(pair: tuple) -> str:
        return pair_field(pair)

    for r in rows:
        symbol = "k" + camel(r["name"])
        prot = (f"{symbol}Protections, {len(r['protections'])}"
                if r["protections"] else "nullptr, 0")
        items = f"{symbol}Items, {len(r['items'])}" if r["items"] else "nullptr, 0"
        exc = (f"{symbol}Exclusive, {len(r['exclusive'])}"
               if r["exclusive"] else "nullptr, 0")
        row = (f'    {{"{r["name"]}", {r["max_level"]}, {r["slot_mask"]}, '
               f'0x{r["components"]:08x}u, {prot}, {items}, {exc}, '
               f'{linear_field(r["damage"])}, {tag_field(r["damage"])}, '
               f'{tag_entity_field(r["damage"])}, '
               f'{linear_field(r["knockback"])}, {tag_field(r["knockback"])}, '
               f'{tag_entity_field(r["knockback"])}, '
               f'{linear_field(r["smash"])}, {tag_field(r["smash"])}, '
               f'{tag_entity_field(r["smash"])}, '
               + fraction_field(r["sweeping"][:2]) + ", "
               + fraction_field(r["sweeping"][2:]) + ", "
               + (f'{symbol}PostAttack, {len(r["post_attack"])}'
                  if r["post_attack"] else "nullptr, 0") + ", "
               + (f'{symbol}Thorns, {len(r["thorns"])}'
                  if r["thorns"] else "nullptr, 0") + "},")
        lines.append(row)
    lines += [
        "};",
        f"inline constexpr int kEnchantmentCount = {len(rows)};",
        "// clang-format on",
        "",
        "} // namespace mc::world",
        "",
    ]
    return "\n".join(lines)


def render_entity_type_tags(tags: dict) -> str:
    """The entity-type tags the modelled requirements name, expanded.

    A `damage_tag` in the table above is one of these names; the runtime asks
    `entity_type_has_tag` whether the victim's type is a member. Only the
    referenced tags are emitted, and every entity type the tag files list — not
    just the ones this engine has a mob row for — so a type it cannot spawn yet
    still answers the same as vanilla would.
    """
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        "// The entity-type tags the modelled enchantment requirements ask about,",
        "// expanded from data/minecraft/tags/entity_type (nested `#tag` references",
        "// resolved, generated — do not hand-edit). A tag is a list of entity type",
        "// *names*, so this stays correct for a type this engine has no mob row for.",
        "//",
        "// Regenerate with tools/gen_enchantment_table.py.",
        "#pragma once",
        "",
        "#include <string_view>",
        "",
        "namespace mc::world {",
        "",
    ]
    for tag in sorted(tags):
        symbol = "k" + camel(tag.replace(_NS, "", 1)) + "Types"
        lines.append("// clang-format off")
        lines.append(f"inline constexpr const char* {symbol}[] = {{")
        for i in range(0, len(tags[tag]), 4):
            lines.append("    " + " ".join(f'"{v}",' for v in tags[tag][i:i + 4]))
        lines.append("};")
        lines.append("// clang-format on")
    lines += [
        "",
        "struct GeneratedEntityTypeTag {",
        "  const char* name;             // minecraft:sensitive_to_smite",
        "  const char* const* types;",
        "  int type_count;",
        "};",
        "",
        "// clang-format off",
        "inline constexpr GeneratedEntityTypeTag kEntityTypeTags[] = {",
    ]
    for tag in sorted(tags):
        symbol = "k" + camel(tag.replace(_NS, "", 1)) + "Types"
        lines.append(f'    {{"{tag}", {symbol}, {len(tags[tag])}}},')
    lines += [
        "};",
        "// clang-format on",
        f"inline constexpr int kEntityTypeTagCount = {len(tags)};",
        "",
        "// EntityTypeTags.test: is `entity_type` (a name, as EntityTypes registers",
        "// them) a member of the tag?",
        "inline bool entity_type_has_tag(std::string_view tag, std::string_view entity_type) {",
        "  for (const GeneratedEntityTypeTag& row : kEntityTypeTags) {",
        "    if (tag != row.name)",
        "      continue;",
        "    for (int i = 0; i < row.type_count; ++i)",
        "      if (entity_type == row.types[i])",
        "        return true;",
        "    return false;",
        "  }",
        "  return false;",
        "}",
        "",
        "} // namespace mc::world",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.2/data"))
    parser.add_argument("--order", type=pathlib.Path,
                        default=pathlib.Path(
                            "data/vanilla/26.2/clientconfig/registries/enchantment.txt"))
    parser.add_argument("--items", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.2/item_components.tsv"))
    parser.add_argument("--reports", type=pathlib.Path,
                        default=pathlib.Path("build/_dl/boot/generated/reports"))
    parser.add_argument("--header", type=pathlib.Path,
                        default=pathlib.Path("src/world/enchantment_table.h"))
    parser.add_argument("--entity-header", type=pathlib.Path,
                        default=pathlib.Path("src/world/entity_type_tags.h"),
                        help="the entity-type tags the modelled requirements ask about")
    args = parser.parse_args()

    mc = args.data / "minecraft"
    enchantments = load_jsons(mc / "enchantment")
    order = [line.strip() for line in args.order.read_text().splitlines() if line.strip()]
    item_tags = load_tags(mc / "tags" / "item")
    enchant_tags = load_tags(mc / "tags" / "enchantment")

    item_ids = {}
    for line in args.items.read_text().splitlines():
        if line.startswith("#") or line.startswith("id\t"):
            continue
        parts = line.split("\t")
        if len(parts) > 1 and parts[0].isdigit():
            item_ids[parts[1]] = int(parts[0])
    if not item_ids:
        raise SystemExit(f"{args.items}: no item ids found")

    components = json.loads((args.reports / "registries.json").read_text())[
        "minecraft:enchantment_effect_component_type"]["entries"]
    component_ids = {name: entry["protocol_id"] for name, entry in components.items()}

    # The damage-tag bit numbering must be the one damage_table.h uses, which is
    # the sorted tag-name order (gen_damage_table.py).
    damage_tags = sorted(_NS + p.stem for p in (mc / "tags" / "damage_type").glob("*.json"))
    damage_tag_bits = {tag: i for i, tag in enumerate(damage_tags)}

    skipped = []
    rows_out = rows(enchantments, order, item_tags, enchant_tags, item_ids, component_ids,
                    skipped)
    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(render(rows_out, component_ids, damage_tag_bits))

    # The entity-type tags the damage requirements name, expanded from vanilla's
    # own tag files (a tag may include other tags).
    entity_tags = load_tags(mc / "tags" / "entity_type")
    referenced = sorted({tag for r in rows_out
                         for entry in r["damage"] + r["knockback"] + r["smash"]
                         for tag in (entry[2],) if tag})
    expanded = {tag: expand_tag(tag, entity_tags, set()) for tag in referenced}
    args.entity_header.parent.mkdir(parents=True, exist_ok=True)
    args.entity_header.write_text(render_entity_type_tags(expanded))

    protections = sum(len(r["protections"]) for r in rows_out)
    print(f"wrote {args.header} ({len(rows_out)} enchantments, "
          f"{protections} protection entries, "
          f"{sum(len(r['damage']) for r in rows_out)} damage, "
          f"{sum(len(r['knockback']) for r in rows_out)} knockback, "
          f"{sum(len(r['smash']) for r in rows_out)} smash-damage and "
          f"{sum(len(r['post_attack']) for r in rows_out)} post-attack entries)")
    for who, kind in skipped:
        print(f"  {who}: {kind} entry carried only as the effects bit (not applied)")
    print(f"wrote {args.entity_header} ({len(expanded)} entity-type tags, "
          f"{sum(len(v) for v in expanded.values())} members)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
