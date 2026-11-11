#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/world/item_registry.h — the complete 26.2 item table.

Two outputs, one per input mode:

  --reports DIR   reads the vanilla data generator's reports directory and
                  writes BOTH the flattened extraction
                  (data/vanilla/26.2/item_components.tsv) and the header. This
                  is the authoritative mode; it needs the generator output, not
                  a JDK of its own (the reports are produced by
                  tools/vanilla-data's generator run — see the header comment in
                  the tool's README).
  --tsv FILE      rebuilds only the header from the committed extraction, so
                  anyone can regenerate the header without the reports dir.

Why the reports and not a Java probe: in 1.20.5+ an item's stack size,
durability, food and tool data live only in data components built in code, so
the ids alone (reports/registries.json, which the old hand-made header came
from) are not enough. `RegistryComponentsReport` writes one JSON per element
under reports/minecraft/components/<registry>/ with the element's full
DataComponentPatch, and `RegistryDumpReport` writes the ids. Both come out of the
same generator run, so the id and the attributes are guaranteed consistent.

Usage:
    gen_item_table.py --reports build/_dl/boot/generated/reports
    gen_item_table.py --tsv data/vanilla/26.2/item_components.tsv
"""

import argparse
import json
import pathlib
import sys

_ITEM = "minecraft:item"
_BLOCK = "minecraft:block"

# flags bits (mirrored in the generated header's ItemFlags enum)
FLAG_BLOCK = 1
FLAG_FOOD = 2
FLAG_DAMAGEABLE = 4
FLAG_TOOL = 8
FLAG_ALWAYS_EDIBLE = 16 # minecraft:food can_always_eat
FLAG_SWORD = 32 # #minecraft:swords (Player.isSweepAttack's ItemTags.SWORDS test)
FLAG_DAMAGE_ON_HURT = 64 # minecraft:equippable's damage_on_hurt (absent = true)
FLAG_FIRE_RESISTANT = 128 # minecraft:damage_resistant against #minecraft:is_fire

_TSV_HEADER = ("id\tname\tmax_stack_size\tmax_damage\tarmor\tarmor_toughness\tnutrition\t"
               "saturation\tconsume_ticks\tattack_damage\tattack_speed\tflags\t"
               "remainder\tremainder_count\tcooldown_ticks\tcooldown_group\t"
               "potion_duration_scale")

# DataComponentPatch entries on the wire are keyed by the
# minecraft:data_component_type registry id, so the handful of components the
# engine writes need their ids in the generated header. They ride along in the
# TSV as '#<name>\t<id>' comment lines so --tsv mode reproduces the header
# byte-for-byte without the reports directory.
_WIRE_COMPONENTS = [
    "damage",
    "max_stack_size",
    "max_damage",
    "custom_name",
    "food",
    "tool",
    "equippable",
    "enchantments",
    "custom_model_data",
    "potion_contents",
    "trim",
]
_COMPONENT_REGISTRY = "minecraft:data_component_type"


def _component(components: dict, key: str):
    return components.get("minecraft:" + key)


def damage_on_hurt_value(components: dict) -> bool:
    """`Equippable.damageOnHurt()`: whether being hurt wears this item.

    `LivingEntity.doHurtEquipment` asks the *equippable* component, and the
    component's own default is true — so the flag is set for every equippable
    item that does not carry an explicit `damage_on_hurt: false`. The 26.2 data
    sets it false on exactly twelve items: the eleven mount/nautilus armours
    (equippable slot `body`) and `minecraft:elytra` (slot `chest`). The elytra
    is why the flag has to exist: it is damageable and sits in a player armour
    slot, so a hard-coded "armour wears" would break it.
    """
    equippable = _component(components, "equippable")
    if not equippable:
        return False
    return equippable.get("damage_on_hurt", True) is not False


def fire_resistant_value(components: dict) -> bool:
    """`ItemStack.canBeHurtBy`: the item's `minecraft:damage_resistant` against
    the damaging type.

    The component carries a damage-type *tag*, and the whole pack has only two:
    sixteen `#minecraft:is_fire` (every netherite item) and one
    `#minecraft:is_explosion` (`minecraft:nether_star`). Only the fire half is
    carried as a flag, and the only item the missing half could ever apply to is
    the nether star, which is equippable in the offhand and never wear-gated in
    the first place — so the two readings agree on every item a player can wear
    into a hit. A tag this table does not know is a loud failure rather than a
    silently wrong flag.
    """
    resistant = _component(components, "damage_resistant") or {}
    types = resistant.get("types")
    if types not in (None, "#minecraft:is_fire", "#minecraft:is_explosion"):
        raise SystemExit(f"minecraft:damage_resistant against {types!r} is not modelled "
                         "(only #minecraft:is_fire is carried as a flag)")
    return types == "#minecraft:is_fire"


def armor_value(components: dict) -> int:
    """Sum of the item's `minecraft:armor` attribute modifiers.

    Vanilla models armour points as attribute modifiers (not a dedicated
    component), so this is where a chestplate's 8 points live.
    """
    total = 0.0
    for mod in components.get("minecraft:attribute_modifiers") or []:
        if mod.get("type") == "minecraft:armor":
            total += float(mod.get("amount", 0.0))
    return int(round(total))


def armor_toughness_value(components: dict) -> int:
    """Sum of the item's `minecraft:armor_toughness` attribute modifiers.

    Toughness rides along with armour points (diamond +2, netherite +3) and is
    what makes the armour formula's `armor - damage / (2 + toughness / 4)` term
    differ between materials (CombatRules.getDamageAfterAbsorb).
    """
    total = 0.0
    for mod in components.get("minecraft:attribute_modifiers") or []:
        if mod.get("type") == "minecraft:armor_toughness":
            total += float(mod.get("amount", 0.0))
    return int(round(total))


def attack_value(components: dict, attribute: str, default: float) -> float:
    """The item's `minecraft:<attribute>` modifier sum (weapons only).

    A sword's damage and its attack *speed* are both attribute modifiers on the
    `mainhand` slot: a diamond sword adds +6.0 to `attack_damage` (the player's
    base is 1.0) and -2.4 to `attack_speed` (the player's base is 4.0, so the
    swing takes 20/1.6 ticks). Both are what Player.getAttackStrengthScale and
    Player.attack read, so combat needs them for the *cooldown* and the
    critical-hit window, not just for the number.
    """
    total = float(default)
    for mod in components.get("minecraft:attribute_modifiers") or []:
        if mod.get("type") == "minecraft:" + attribute:
            total += float(mod.get("amount", 0.0))
    return total


def saturation_value(components: dict) -> float:
    """FoodData.eat's saturation: FoodProperties.saturation, a float."""
    food = components.get("minecraft:food") or {}
    return float(food.get("saturation", 0.0))


def use_remainder_value(components: dict, items: dict) -> tuple:
    """`minecraft:use_remainder` -> (item id, count), or (0, 0) when absent.

    The component is an ItemStackTemplate: `{"id": "minecraft:bowl"}` with an
    optional `count` (ItemStackTemplate.CODEC, whose default is 1). A template
    that carries `components` is refused rather than dropped — no 26.2 item has
    one, and a remainder that lost an enchantment or a custom name would be
    wrong rather than merely incomplete.
    """
    template = components.get("minecraft:use_remainder")
    if not template:
        return 0, 0
    if template.get("components"):
        raise SystemExit(f"use_remainder with components is not modelled: {template}")
    name = template["id"]
    if name not in items:
        raise SystemExit(f"use_remainder names unknown item {name}")
    return int(items[name]["protocol_id"]), int(template.get("count", 1))


def use_cooldown_value(components: dict) -> tuple:
    """`minecraft:use_cooldown` -> (ticks, group or None), (0, None) when absent.

    UseCooldown.ticks() is `(int)(seconds * 20)` — what `finishUsingItem` puts
    the item on cooldown for. The group is an optional Identifier, and
    ItemCooldowns.getCooldownGroup falls back to the item's own registry id, so
    None here means "this item's own name".
    """
    cooldown = components.get("minecraft:use_cooldown")
    if not cooldown:
        return 0, None
    ticks = int(float(cooldown["seconds"]) * 20.0)
    if not 0 < ticks <= 65535:
        raise SystemExit(f"use_cooldown {cooldown} is outside the table's range")
    group = cooldown.get("cooldown_group")
    if group is not None and not isinstance(group, str):
        raise SystemExit(f"use_cooldown group is not an identifier: {group!r}")
    return ticks, group


def potion_duration_scale_value(components: dict) -> float:
    """`minecraft:potion_duration_scale` -> the item's own default, 1.0 when absent.

    A stack's component list falls back to the *item's* prototype components, so
    this column is what `stack.getOrDefault(POTION_DURATION_SCALE, 1.0f)` returns
    for a stack that does not override it — and the one item that carries a
    non-default value is the lingering potion, at 0.25. That is not a display
    detail: `AreaEffectCloud.applyComponentsFromItemStack` takes it as the
    cloud's own `potionDurationScale`, and the cloud scales every effect it
    applies by it, so a lingering potion's cloud grants a quarter of the
    duration the bottle's data declares.
    """
    value = components.get("minecraft:potion_duration_scale")
    if value is None:
        return 1.0
    return float(value)


def consume_ticks_value(components: dict) -> int:
    """How long `use` takes to consume the item, in ticks.

    Eating is gated on the `minecraft:consumable` component, not on `food`:
    the four fish buckets carry food values but no consumable (they place a
    fish instead), so 0 here means "`use` does nothing on this item".
    Consumable consumes over `consume_seconds` (default 1.6 s = 32 ticks).
    """
    consumable = components.get("minecraft:consumable")
    if consumable is None:
        return 0
    return int(round(float(consumable.get("consume_seconds", 1.6)) * 20.0))


def block_items(tsv: pathlib.Path | None = None):
    """Every item that places a block.

    An item is a block item when it is *built* as one in `Items.java`/the item
    registry, which is not the same as its name matching a block:
    `minecraft:wheat_seeds` places `minecraft:wheat`, and the item
    `minecraft:wheat` places nothing. When `tsv` is given it is the game's own
    `BlockItem` dump (`item\\tblock`); otherwise the decompiled-source parser is
    used.
    """
    if tsv is not None:
        names = set()
        for line in tsv.read_text().splitlines():
            if not line or line.startswith("item\t"):
                continue
            names.add(line.split("\t", 1)[0])
        return names
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    import gen_block_item_table as gen

    return set(gen.block_item_pairs())


def sword_names(tags_dir: pathlib.Path) -> set:
    """`#minecraft:swords`: the item names Player.isSweepAttack tests for."""
    path = tags_dir / "swords.json"
    if not path.exists():
        raise SystemExit(f"{path}: missing (pass --tags at data/minecraft/tags/item)")
    out = set()
    for value in json.loads(path.read_text())["values"]:
        if value.startswith("#"):
            raise SystemExit(f"{path}: nested tag {value!r} is not modelled")
        out.add(value if isinstance(value, str) else value["id"])
    return out


def rows_from_reports(reports: pathlib.Path, tags_dir: pathlib.Path,
                      block_items_tsv: pathlib.Path | None = None):
    """Returns (item rows, wire component ids) from the generator's reports."""
    registries = json.loads((reports / "registries.json").read_text())
    items = registries[_ITEM]["entries"]
    block_item_names = block_items(block_items_tsv)
    swords = sword_names(tags_dir)
    comp_types = registries[_COMPONENT_REGISTRY]["entries"]
    component_ids = {name.split(":", 1)[1]: meta["protocol_id"]
                     for name, meta in comp_types.items()}

    comp_dir = reports / "minecraft" / "components" / "item"
    rows = []
    for name, meta in items.items():
        path = comp_dir / (name.split(":", 1)[1] + ".json")
        item_components = json.loads(path.read_text())["components"] if path.exists() else {}
        max_stack = int(_component(item_components, "max_stack_size") or 1)
        max_damage = int(_component(item_components, "max_damage") or 0)
        food = _component(item_components, "food") or {}
        flags = 0
        if name in block_item_names:
            flags |= FLAG_BLOCK
        if food:
            flags |= FLAG_FOOD
        if max_damage > 0:
            flags |= FLAG_DAMAGEABLE
        if _component(item_components, "tool"):
            flags |= FLAG_TOOL
        if food.get("can_always_eat"):
            flags |= FLAG_ALWAYS_EDIBLE
        if name in swords:
            flags |= FLAG_SWORD
        if damage_on_hurt_value(item_components):
            flags |= FLAG_DAMAGE_ON_HURT
        if fire_resistant_value(item_components):
            flags |= FLAG_FIRE_RESISTANT
        remainder_item, remainder_count = use_remainder_value(item_components, items)
        cooldown_ticks, cooldown_group = use_cooldown_value(item_components)
        rows.append((int(meta["protocol_id"]), name, max_stack, max_damage,
                     armor_value(item_components), armor_toughness_value(item_components),
                     int(food.get("nutrition", 0)), saturation_value(item_components),
                     consume_ticks_value(item_components),
                     attack_value(item_components, "attack_damage", 0.0),
                     attack_value(item_components, "attack_speed", 0.0), flags,
                     remainder_item, remainder_count, cooldown_ticks, cooldown_group,
                     potion_duration_scale_value(item_components)))
    rows.sort()
    return rows, component_ids


def rows_from_tsv(path: pathlib.Path):
    """Returns (rows, component_ids) from the flattened extraction."""
    rows = []
    component_ids: dict[str, int] = {}
    for i, line in enumerate(path.read_text().splitlines()):
        if not line.strip():
            continue
        parts = line.split("\t")
        if parts[0] == "id":
            continue  # the column header
        if line.startswith("#"):
            # '#<component name>\t<id>' — rebuilt into the header's constants.
            component_ids[parts[0][1:]] = int(parts[1])
            continue
        if len(parts) != 17:
            raise SystemExit(f"{path}:{i + 1}: expected 17 tab-separated fields")
        rows.append((int(parts[0]), parts[1], int(parts[2]), int(parts[3]),
                     int(parts[4]), int(parts[5]), int(parts[6]), float(parts[7]),
                     int(parts[8]), float(parts[9]), float(parts[10]), int(parts[11]),
                     int(parts[12]), int(parts[13]), int(parts[14]),
                     None if parts[15] == "-" else parts[15], float(parts[16])))
    rows.sort()
    return rows, component_ids


def validate(rows) -> None:
    """The registry index IS the protocol id, so ids must be dense and unique."""
    seen = set()
    for want, row in enumerate(rows):
        if row[0] != want:
            raise SystemExit(f"item ids are not dense: entry {want} has id {row[0]}")
        if row[1] in seen:
            raise SystemExit(f"duplicate item name {row[1]}")
        seen.add(row[1])


def tsv_row(row) -> str:
    """One TSV row. The cooldown group is the one None-able cell: '-' is the
    "no group" the header's `nullptr` renders, so a row never contains the Python
    repr of an absent value."""
    return "\t".join("-" if v is None else str(v) for v in row)


def render_header(rows, component_ids: dict[str, int]) -> str:
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        f"// Complete 26.3 item registry: {len(rows)} items with their protocol ids and the",
        "// attributes the engine needs (generated — do not hand-edit).",
        "//",
        "// Source: the vanilla data generator's reports — ids from",
        "// reports/registries.json (RegistryDumpReport), attributes from",
        "// reports/minecraft/components/item/*.json (RegistryComponentsReport),",
        "// flattened into data/vanilla/26.3/item_components.tsv.",
        "// Regenerate with tools/gen_item_table.py --reports <reports dir>.",
        "//",
        "// `id` is the minecraft:item registry index, i.e. the varint a 26.3 client",
        "// reads in an ItemStack. Max stack size and durability come from the",
        "// MAX_STACK_SIZE / MAX_DAMAGE data components (the same values",
        "// Item.getDefaultMaxStackSize() and ItemStack.getMaxDamage() return);",
        "// armour points and toughness are the item's minecraft:armor /",
        "// minecraft:armor_toughness attribute modifiers (vanilla models both as",
        "// attribute modifiers rather than dedicated components); `nutrition` and",
        "// `saturation` come from minecraft:food; `consume_ticks` is how long `use`",
        "// takes to eat the item (minecraft:consumable, 0 when absent);",
        "// `attack_damage` / `attack_speed` are the item's minecraft:attack_damage /",
        "// minecraft:attack_speed attribute modifiers, i.e. the bonus a weapon adds",
        "// to the player's own 1.0 / 4.0 base values (0 for everything else).",
        "// `remainder_item` / `remainder_count` are minecraft:use_remainder — the",
        "// ItemStackTemplate handed back when the stack is emptied (0 for none) — and",
        "// `cooldown_ticks` / `cooldown_group` are minecraft:use_cooldown, with a null",
        "// group meaning the item's own name. `potion_duration_scale` is",
        "// minecraft:potion_duration_scale — the item's prototype value, which is 1.0",
        "// for everything except the lingering potion (0.25).",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mc::world {",
    ]
    if component_ids:
        lines += [
            "",
            "// minecraft:data_component_type ids. A DataComponentPatch entry on the wire",
            "// is [type id varint][value], so writing a component needs its registry id.",
        ]
        for name in _WIRE_COMPONENTS:
            if name in component_ids:
                camel = "".join(p.capitalize() for p in name.split("_"))
                lines.append(f"constexpr int32_t kComponent{camel}Id = {component_ids[name]};")
    lines += [
        "",
        "// ItemFlags bit values (uint8_t flags column).",
        "constexpr uint8_t kItemIsBlock = 1;",
        "constexpr uint8_t kItemIsFood = 2;",
        "constexpr uint8_t kItemIsDamageable = 4;",
        "constexpr uint8_t kItemHasTool = 8;",
        "// minecraft:food's can_always_eat: edible even with a full food bar.",
        "constexpr uint8_t kItemCanAlwaysEat = 16;",
        "// In #minecraft:swords. Player.isSweepAttack asks for this tag, so a",
        "// fully charged swing sweeps with a sword and nothing else.",
        "constexpr uint8_t kItemIsSword = 32;",
        "// minecraft:equippable's damage_on_hurt (the component's own default when",
        "// the field is absent). LivingEntity.doHurtEquipment wears a piece only",
        "// when this is set: the pack clears it on the eleven mount/nautilus",
        "// armours and on minecraft:elytra.",
        "constexpr uint8_t kItemDamageOnHurt = 64;",
        "// minecraft:damage_resistant against #minecraft:is_fire, i.e.",
        "// ItemStack.canBeHurtBy's fire half (every netherite item).",
        "constexpr uint8_t kItemFireResistant = 128;",
        "",
        "struct GeneratedItem {",
        "  const char* name;",
        "  int32_t id;",
        "  uint16_t max_stack_size;",
        "  uint16_t max_damage;",
        "  uint8_t armor;",
        "  uint8_t armor_toughness;",
        "  uint8_t nutrition;",
        "  float saturation;",
        "  uint8_t consume_ticks;",
        "  float attack_damage;",
        "  float attack_speed;",
        "  uint8_t flags;",
        "  // minecraft:use_remainder: the template left behind when this stack is",
        "  // emptied (a stew's bowl), or 0 for no remainder.",
        "  int32_t remainder_item;",
        "  uint8_t remainder_count;",
        "  // minecraft:use_cooldown: ticks the item is locked for after use, and the",
        "  // group those ticks are shared under (nullptr = this item alone).",
        "  int32_t cooldown_ticks;",
        "  const char* cooldown_group;",
        "  // minecraft:potion_duration_scale: the item's own default scale for the",
        "  // effects of a potion it carries (1.0 for everything but the lingering",
        "  // potion, whose 0.25 is what makes a cloud last a quarter as long).",
        "  float potion_duration_scale;",
        "};",
        "",
        "// clang-format off",
        "inline constexpr GeneratedItem kItems[] = {",
    ]

    def float_literal(value: float) -> str:
        text = f"{value:.7g}"
        if "." not in text and "e" not in text and "E" not in text:
            text += ".0"
        return text + "f"

    for row in rows:
        (iid, name, stack, damage, armor, toughness, nutrition, saturation, consume,
         attack_damage, attack_speed, flags, remainder_item, remainder_count,
         cooldown_ticks, cooldown_group, duration_scale) = row
        sat = float_literal(saturation)
        atk = float_literal(attack_damage)
        speed = float_literal(attack_speed)
        scale = float_literal(duration_scale)
        group = "nullptr" if cooldown_group is None else f'"{cooldown_group}"'
        lines.append(f'    {{"{name}", {iid}, {stack}, {damage}, {armor}, {toughness}, '
                     f"{nutrition}, {sat}, {consume}, {atk}, {speed}, {flags}, "
                     f"{remainder_item}, {remainder_count}, {cooldown_ticks}, {group}, {scale}}},")
    lines += [
        "};",
        f"inline constexpr int kItemCount = {len(rows)};",
        "// clang-format on",
        "",
        "} // namespace mc::world",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--reports", type=pathlib.Path,
                        help="vanilla data generator reports directory")
    parser.add_argument("--tsv", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.3/item_components.tsv"),
                        help="flattened extraction (written in --reports mode)")
    parser.add_argument("--tags", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.3/data/minecraft/tags/item"),
                        help="item tags (#minecraft:swords, for the flags column)")
    parser.add_argument("--block-items", type=pathlib.Path,
                        help="the game's item->block dump (item\tblock); required without a decompile")
    parser.add_argument("--header", type=pathlib.Path,
                        default=pathlib.Path("src/world/item_registry.h"),
                        help="generated header")
    args = parser.parse_args()

    if args.reports:
        rows, component_ids = rows_from_reports(args.reports, args.tags, args.block_items)
    else:
        rows, component_ids = rows_from_tsv(args.tsv)
    validate(rows)

    if args.reports:
        comment = [f"#{name}\t{component_ids[name]}" for name in _WIRE_COMPONENTS
                   if name in component_ids]
        args.tsv.parent.mkdir(parents=True, exist_ok=True)
        args.tsv.write_text("\n".join(comment + [_TSV_HEADER] + [tsv_row(row) for row in rows]) + "\n")
        print(f"wrote {args.tsv} ({len(rows)} items)")

    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(render_header(rows, component_ids))

    blocks = sum(1 for r in rows if r[11] & FLAG_BLOCK)
    foods = sum(1 for r in rows if r[11] & FLAG_FOOD)
    damageable = sum(1 for r in rows if r[11] & FLAG_DAMAGEABLE)
    tools = sum(1 for r in rows if r[11] & FLAG_TOOL)
    print(f"wrote {args.header} ({len(rows)} items: {blocks} block, {foods} food, "
          f"{damageable} damageable, {tools} with a tool component)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
