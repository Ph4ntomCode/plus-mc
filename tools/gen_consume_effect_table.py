#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/world/consume_effect_table.h — `minecraft:consumable`'s
`on_consume_effects` list for every 26.2 item that carries one.

Two outputs, one per input mode (the same shape as gen_item_table.py and
gen_effect_table.py):

  --reports DIR   the authoritative mode. `DIR` is the vanilla data generator's
                  reports directory: the item components carry the whole
                  `minecraft:consumable` component, `on_consume_effects`
                  included, and registries.json resolves the effect names to
                  their protocol ids (an `update_mob_effect` carries the id, so
                  the table stores ids). Writes both the flattened extraction
                  (data/vanilla/26.2/consume_effects.tsv) and the header.
  --tsv FILE      rebuilds only the header from the committed extraction, so
                  anyone can regenerate the header without the reports.

The kind column is the `minecraft:consume_effect_type` *protocol id* (the
registry's own registration order, 0..4), because that is the order vanilla
declares them in and so what a reader would expect a wire id to be. The
generator checks the ids against registries.json in --reports mode and refuses
to run if they disagree.

The list is deliberately complete — all five kinds, all eleven items — rather
than only the ones the engine can act on. What the engine *does* with each row
is documented in world/consume.h; a row that cannot be acted on is visible in
the table as data that no code reads, which is the same "unimplemented rather
than silently inert" line the enchantment table takes.

Usage:
    tools/gen_consume_effect_table.py --reports build/_dl/boot/generated/reports
    tools/gen_consume_effect_table.py --tsv data/vanilla/26.2/consume_effects.tsv
"""

import argparse
import json
import pathlib
import sys

_ITEM = "minecraft:item"
_EFFECT = "minecraft:mob_effect"
_CONSUME_EFFECT = "minecraft:consume_effect_type"
_COMPONENT = "minecraft:consumable"

# JSON type name -> (enum name, protocol id). The ids are the registry's
# registration order (ConsumeEffect.Type's static initialisers), and
# _validate_kinds below is what keeps this list honest.
_KINDS = [
    ("minecraft:apply_effects", "apply_effects", 0),
    ("minecraft:remove_effects", "remove_effects", 1),
    ("minecraft:clear_all_effects", "clear_all_effects", 2),
    ("minecraft:teleport_randomly", "teleport_randomly", 3),
    ("minecraft:play_sound", "play_sound", 4),
]
_KIND_TO_ID = {name: pid for name, _, pid in _KINDS}


def _validate_kinds(reports: pathlib.Path) -> None:
    registries = json.loads((reports / "registries.json").read_text())
    entries = registries[_CONSUME_EFFECT]["entries"]
    for name, _, pid in _KINDS:
        got = entries.get(name, {}).get("protocol_id")
        if got != pid:
            raise SystemExit(f"{name}: consume_effect_type id is {got}, the header says {pid}")


def _status_effect(entry: dict, ids: dict) -> tuple:
    """One ApplyStatusEffectsConsumeEffect entry, as a table row.

    The field names and defaults are `MobEffectInstance.Details.MAP_CODEC`:
    `amplifier` 0, `duration` 0, `ambient` false, `show_particles` true, and
    `show_icon` defaulting to `show_particles` (`Details.create`). A stack's
    nested `hidden_effect` is refused rather than dropped: no 26.2 consumable
    uses one, and a silent drop would be wrong rather than merely incomplete.
    """
    if "hidden_effect" in entry:
        raise SystemExit(f"{entry['id']}: hidden_effect is not modelled by the table")
    name = entry["id"]
    if name not in ids:
        raise SystemExit(f"unknown mob_effect {name}")
    show_particles = bool(entry.get("show_particles", True))
    return (name, ids[name], int(entry.get("duration", 0)), int(entry.get("amplifier", 0)),
            bool(entry.get("ambient", False)), show_particles,
            bool(entry.get("show_icon", show_particles)))


def _remove_effects(entry: dict, ids: dict) -> list:
    """RemoveStatusEffectsConsumeEffect's HolderSet, as a list of effect rows.

    A HolderSet is written as one name, a list of names, or a `#tag`. No 26.2
    item uses a tag (the honey bottle names `minecraft:poison`), and resolving
    one would need the tag data this generator is not given, so a tag is an
    error rather than an empty list.
    """
    raw = entry["effects"]
    names = raw if isinstance(raw, list) else [raw]
    out = []
    for name in names:
        if name.startswith("#"):
            raise SystemExit(f"tag {name} is not supported by the extraction")
        if name not in ids:
            raise SystemExit(f"unknown mob_effect {name}")
        out.append((name, ids[name]))
    return out


def rows_from_reports(reports: pathlib.Path):
    """Returns (effect rows, status rows, remove rows) from the reports."""
    _validate_kinds(reports)
    registries = json.loads((reports / "registries.json").read_text())
    items = registries[_ITEM]["entries"]
    effects = {name: meta["protocol_id"] for name, meta in registries[_EFFECT]["entries"].items()}

    comp_dir = reports / "minecraft" / "components" / "item"
    effect_rows, status_rows, remove_rows = [], [], []
    for name, meta in sorted(items.items(), key=lambda kv: kv[1]["protocol_id"]):
        path = comp_dir / (name.split(":", 1)[1] + ".json")
        if not path.exists():
            continue
        consumable = json.loads(path.read_text())["components"].get(_COMPONENT)
        if not consumable:
            continue
        for entry in consumable.get("on_consume_effects") or []:
            kind = entry["type"]
            if kind not in _KIND_TO_ID:
                raise SystemExit(f"{name}: unknown consume effect type {kind}")
            kind_id = _KIND_TO_ID[kind]
            value, sound, first, count = 1.0, None, 0, 0
            if kind == "minecraft:apply_effects":
                value = float(entry.get("probability", 1.0))
                if not 0.0 <= value <= 1.0:
                    raise SystemExit(f"{name}: probability {value} is outside 0..1")
                first = len(status_rows)
                status_rows.extend(_status_effect(e, effects) for e in entry["effects"])
                count = len(status_rows) - first
            elif kind == "minecraft:remove_effects":
                first = len(remove_rows)
                remove_rows.extend(_remove_effects(entry, effects))
                count = len(remove_rows) - first
            elif kind == "minecraft:teleport_randomly":
                value = float(entry.get("diameter", 16.0))
            elif kind == "minecraft:play_sound":
                sound = entry["sound"]
            effect_rows.append((name, int(meta["protocol_id"]), kind_id, value, sound,
                                first, count))
    return effect_rows, status_rows, remove_rows


def rows_from_tsv(path: pathlib.Path):
    """Returns (effect rows, status rows, remove rows) from the extraction."""
    effect_rows, status_rows, remove_rows = [], [], []
    section = None
    for i, line in enumerate(path.read_text().splitlines()):
        if not line.strip():
            continue
        parts = line.split("\t")
        if parts[0].startswith("#"):
            # `#<section>\t<columns...>` opens a section; any other `#` line is
            # a comment (including the file's own header block).
            name = parts[0][1:]
            if len(parts) > 1 and name in ("effect", "status", "remove"):
                section = name
            continue
        if section == "effect":
            if len(parts) != 7:
                raise SystemExit(f"{path}:{i + 1}: expected 7 fields, got {len(parts)}")
            effect_rows.append((parts[0], int(parts[1]), int(parts[2]), float(parts[3]),
                                None if parts[4] == "-" else parts[4], int(parts[5]),
                                int(parts[6])))
        elif section == "status":
            if len(parts) != 7:
                raise SystemExit(f"{path}:{i + 1}: expected 7 fields, got {len(parts)}")
            status_rows.append((parts[0], int(parts[1]), int(parts[2]), int(parts[3]),
                                bool(int(parts[4])), bool(int(parts[5])), bool(int(parts[6]))))
        elif section == "remove":
            if len(parts) != 2:
                raise SystemExit(f"{path}:{i + 1}: expected 2 fields, got {len(parts)}")
            remove_rows.append((parts[0], int(parts[1])))
        else:
            raise SystemExit(f"{path}:{i + 1}: a row before any section header")
    return effect_rows, status_rows, remove_rows


def render_tsv(effect_rows, status_rows, remove_rows) -> str:
    lines = [
        "#effect\titem\titem_id\tkind\tvalue\tsound\tfirst\tcount",
    ]
    for name, iid, kind, value, sound, first, count in effect_rows:
        lines.append("\t".join([name, str(iid), str(kind), f"{value:g}", sound or "-",
                                str(first), str(count)]))
    lines.append("#status\teffect\teffect_id\tduration\tamplifier\tambient\tvisible\tshow_icon")
    for name, eid, duration, amplifier, ambient, visible, show_icon in status_rows:
        lines.append("\t".join([name, str(eid), str(duration), str(amplifier),
                                str(int(ambient)), str(int(visible)), str(int(show_icon))]))
    lines.append("#remove\teffect\teffect_id")
    for name, eid in remove_rows:
        lines.append("\t".join([name, str(eid)]))
    return "\n".join(lines) + "\n"


def _float_literal(value: float) -> str:
    text = f"{value:.7g}"
    if "." not in text and "e" not in text and "E" not in text:
        text += ".0"
    return text + "f"


def render_header(effect_rows, status_rows, remove_rows) -> str:
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        f"// `minecraft:consumable`'s `on_consume_effects` list: the "
        f"{len(effect_rows)} 26.2 items",
        "// that carry one (generated — do not hand-edit).",
        "//",
        "// Source: the vanilla data generator's reports — the component itself from",
        "// reports/minecraft/components/item/*.json (the same files gen_item_table.py",
        "// reads the food and consumable columns from), the effect ids from",
        "// reports/registries.json — flattened into",
        "// data/vanilla/26.2/consume_effects.tsv.",
        "// Regenerate with tools/gen_consume_effect_table.py --reports <reports dir>.",
        "//",
        "// `kind` is the `minecraft:consume_effect_type` protocol id, i.e.",
        "// ConsumeEffect.Type's registration order, and the constants below are that",
        "// order spelled out. `value` is the one scalar a kind carries: a",
        "// probability for apply_effects (the roll `nextFloat()` is compared",
        "// against), a diameter for teleport_randomly (TeleportRandomlyConsumeEffect",
        "// .DEFAULT_DIAMETER), and 1 otherwise.",
        "//",
        "// `first`/`count` index the arrays below: an apply_effects row into",
        "// kConsumeStatusEffects (the MobEffectInstance list it applies, with",
        "// MobEffectInstance.Details' own field names and defaults), a",
        "// remove_effects row into kConsumeRemoveEffects (the HolderSet it names).",
        "// Every other kind has count 0.",
        "//",
        "// `sound` is carried for the play_sound rows and nothing reads it: this",
        "// server models no sounds. It is kept so the table shows *what* an ominous",
        "// bottle would play rather than implying the row is empty.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mc::world {",
        "",
        "// ConsumeEffect.Type, in registration order.",
        "constexpr uint8_t kConsumeKindApplyEffects = 0;",
        "constexpr uint8_t kConsumeKindRemoveEffects = 1;",
        "constexpr uint8_t kConsumeKindClearAllEffects = 2;",
        "constexpr uint8_t kConsumeKindTeleportRandomly = 3;",
        "constexpr uint8_t kConsumeKindPlaySound = 4;",
        "constexpr int kConsumeKindCount = 5;",
        "",
        "struct GeneratedConsumeStatusEffect {",
        "  int32_t effect;      // the mob_effect id, i.e. its wire id",
        "  int32_t duration;    // ticks",
        "  int32_t amplifier;",
        "  bool ambient;",
        "  bool visible;        // show_particles",
        "  bool show_icon;      // defaults to show_particles (Details.create)",
        "};",
        "",
        "struct GeneratedConsumeEffect {",
        "  int32_t item;        // the item id the row belongs to",
        "  uint8_t kind;        // kConsumeKind*",
        "  float value;         // probability / diameter",
        "  const char* sound;   // play_sound only; never read",
        "  int32_t first;       // index into kConsumeStatusEffects / kConsumeRemoveEffects",
        "  int32_t count;",
        "};",
        "",
        "inline constexpr int32_t kConsumeRemoveEffects[] = {",
    ]
    for name, eid in remove_rows:
        lines.append(f"    {eid},  // {name}")
    lines += [
        "};",
        f"inline constexpr int kConsumeRemoveEffectCount = {len(remove_rows)};",
        "",
        "inline constexpr GeneratedConsumeStatusEffect kConsumeStatusEffects[] = {",
    ]
    # clang-format would reflow these; the generated file is exempt like the rest.
    for name, eid, duration, amplifier, ambient, visible, show_icon in status_rows:
        lines.append(f"    {{/*{name}*/ {eid}, {duration}, {amplifier}, "
                     f"{str(ambient).lower()}, {str(visible).lower()}, "
                     f"{str(show_icon).lower()}}},")
    lines += [
        "};",
        f"inline constexpr int kConsumeStatusEffectCount = {len(status_rows)};",
        "",
        "// clang-format off",
        "inline constexpr GeneratedConsumeEffect kConsumeEffects[] = {",
    ]
    for name, iid, kind, value, sound, first, count in effect_rows:
        literal = "nullptr" if sound is None else f'"{sound}"'
        lines.append(f'    {{/*{name}*/ {iid}, {kind}, {_float_literal(value)}, {literal}, '
                     f"{first}, {count}}},")
    lines += [
        "};",
        f"inline constexpr int kConsumeEffectCount = {len(effect_rows)};",
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
                        default=pathlib.Path("data/vanilla/26.2/consume_effects.tsv"),
                        help="flattened extraction (written in --reports mode)")
    parser.add_argument("--header", type=pathlib.Path,
                        default=pathlib.Path("src/world/consume_effect_table.h"),
                        help="generated header")
    args = parser.parse_args()

    if args.reports:
        effect_rows, status_rows, remove_rows = rows_from_reports(args.reports)
    else:
        effect_rows, status_rows, remove_rows = rows_from_tsv(args.tsv)
    if not effect_rows:
        raise SystemExit("no on_consume_effects rows: the reports are not what this expects")

    for _, _, kind, _, _, _, count in effect_rows:
        if kind == _KIND_TO_ID["minecraft:apply_effects"] and count == 0:
            raise SystemExit("an apply_effects row with no effects")
        if kind not in (_KIND_TO_ID["minecraft:apply_effects"],
                        _KIND_TO_ID["minecraft:remove_effects"]) and count != 0:
            raise SystemExit("only apply_effects and remove_effects index the arrays")

    if args.reports:
        args.tsv.parent.mkdir(parents=True, exist_ok=True)
        args.tsv.write_text(render_tsv(effect_rows, status_rows, remove_rows))
        print(f"wrote {args.tsv} ({len(effect_rows)} items, {len(status_rows)} status effects, "
              f"{len(remove_rows)} removals)")

    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(render_header(effect_rows, status_rows, remove_rows))
    kinds = {}
    for _, _, kind, _, _, _, _ in effect_rows:
        kinds[kind] = kinds.get(kind, 0) + 1
    print(f"wrote {args.header} ({len(effect_rows)} items: "
          + ", ".join(f"{name}={kinds.get(pid, 0)}" for name, _, pid in _KINDS) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
