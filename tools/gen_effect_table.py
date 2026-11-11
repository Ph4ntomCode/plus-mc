#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/world/effect_table.h — the 26.2 minecraft:mob_effect registry.

Two outputs, one per input mode (the same shape as gen_item_table.py):

  --reports DIR --java FILE
                  the authoritative mode. `DIR` is the vanilla data generator's
                  reports directory (it holds registries.json, which carries
                  every mob_effect's protocol_id — the id the client is told in
                  an update_mob_effect packet, because
                  `MobEffect.STREAM_CODEC` is `ByteBufCodecs.holderRegistry`).
                  `FILE` is the decompiled net/minecraft/world/effect/
                  MobEffects.java, the *only* place an effect's colour,
                  category, attribute modifiers and blend durations exist: an
                  effect is a code-built object, so there is no data file to
                  read them from. Writes both the flattened extraction
                  (data/vanilla/26.2/mob_effects.tsv) and the header.
  --tsv FILE      rebuilds only the header from the committed extraction, so
                  anyone can regenerate the header without the reports or the
                  decompiled sources.

Why the Java source and not a bundle: the colour and the category are
constructor arguments and the attribute modifiers are chained builder calls, so
they are compiled into the class rather than carried in any registry dump. The
parser below is deliberately strict — it matches one `MobEffects.register(...)`
statement per line and *refuses to run* on a line it cannot account for — so a
future version whose registration lines change shape fails the generator instead
of silently dropping an effect's colour or speed modifier.

Usage:
    tools/gen_effect_table.py --reports build/_dl/boot/generated/reports \\
        --java build/_decomp/net/minecraft/world/effect/MobEffects.java
    tools/gen_effect_table.py --tsv data/vanilla/26.2/mob_effects.tsv
"""

import argparse
import json
import pathlib
import re
import sys

_NS = "minecraft:"

# The applyEffectTick subclass an effect is built from, as an enum the generated
# header mirrors. Everything else (BadOmen, RaidOmen, Infested, Oozing, Weaving,
# WindCharged, and the plain MobEffect) has no per-tick behaviour this server
# models, so they land on `plain` — which is exactly what
# `MobEffect.applyEffectTick` returning true means.
_KINDS = {
    "MobEffect": "plain",
    "RegenerationMobEffect": "regeneration",
    "PoisonMobEffect": "poison",
    "WitherMobEffect": "wither",
    "HungerMobEffect": "hunger",
    "SaturationMobEffect": "saturation",
    "HealOrHarmMobEffect": "instant_health_or_harm",
    "AbsorptionMobEffect": "absorption",
    # These have real behaviour in vanilla (spawning reinforcements, inflicting
    # oozing on death), but none of it is reachable without the entity-spawning
    # machinery this server does not have; they tick as no-ops.
    "BadOmenMobEffect": "plain",
    "RaidOmenMobEffect": "plain",
    "InfestedMobEffect": "plain",
    "OozingMobEffect": "plain",
    "WeavingMobEffect": "plain",
    "WindChargedMobEffect": "plain",
}

# InstantaneousMobEffect subclasses: their shouldApplyEffectTickThisTick is
# `remainingDuration >= 1`, so they fire once and expire, and `/effect give`
# gives them a duration of 1 rather than 600.
_INSTANTANEOUS = {"SaturationMobEffect", "HealOrHarmMobEffect"}

_OPERATIONS = {
    "ADD_VALUE": "add_value",
    "ADD_MULTIPLIED_BASE": "add_multiplied_base",
    "ADD_MULTIPLIED_TOTAL": "add_multiplied_total",
}

_REGISTER = re.compile(
    r"^\s*public static final Holder<MobEffect> \w+ = MobEffects\.register\(\s*"
    r'"(?P<name>[a-z_]+)"\s*,\s*'
    r"new (?P<cls>\w+)\(\s*"
    r"MobEffectCategory\.(?P<category>BENEFICIAL|HARMFUL|NEUTRAL)\s*,\s*"
    r"(?P<color>0[xX][0-9a-fA-F]+|\d+)"
    # HealOrHarmMobEffect's third constructor argument: whether this instance
    # harms (instant_damage) rather than heals (instant_health).
    r"(?:\s*,\s*(?P<harm>true|false))?"
    r"(?P<rest>.*)$"
)

_ATTRIBUTE = re.compile(
    r"\.addAttributeModifier\(\s*Attributes\.(?P<attr>[A-Z_]+)\s*,\s*"
    r'Identifier\.withDefaultNamespace\("[^"]*"\)\s*,\s*'
    r"(?P<amount>-?[0-9.]+)[fFdD]?\s*,\s*"
    r"AttributeModifier\.Operation\.(?P<op>[A-Z_]+)\s*\)"
)

_BLEND1 = re.compile(r"\.setBlendDuration\((?P<ticks>\d+)\)")
_BLEND3 = re.compile(
    r"\.setBlendDuration\((?P<i>\d+)\s*,\s*(?P<o>\d+)\s*,\s*(?P<a>\d+)\)"
)

# The three effect classes that take a spawn-count function as their last
# constructor argument (Weaving, Oozing, Infested). Their bodies are the only
# thing they can be — a constant or a Mth.randomBetweenInclusive call — so the
# parser accepts exactly those two shapes rather than trying to guess.
_LAMBDA = re.compile(
    r"^,\s*random\s*->\s*(?:2|Mth\.randomBetweenInclusive\(random,\s*\d+,\s*\d+\))"
    r"\)+;?\s*$"
)

# HealOrHarmMobEffect's isHarm, and Infested's chance — plain constructor
# constants between the colour and the closing brackets.
_BOOL_ARG = re.compile(r",\s*(?:true|false)")
_FLOAT_ARG = re.compile(r",\s*-?\d+(?:\.\d+)?[fFdD]")

_TSV_HEADER = ("name\tid\tcategory\tcolor\tkind\tinstantaneous\tharm\t"
               "blend_in\tblend_out\tblend_out_advance")
_TSV_ATTR_HEADER = "effect\tattribute\tamount\toperation"


def parse_java(text: str) -> list:
    """One dict per MobEffects.register(...) statement, in file order."""
    rows = []
    for lineno, line in enumerate(text.splitlines(), 1):
        if "MobEffects.register(" not in line:
            continue
        m = _REGISTER.match(line.rstrip())
        if not m:
            raise SystemExit(f"MobEffects.java:{lineno}: unrecognized registration:"
                             f"\n  {line.strip()}")
        rest = m.group("rest")
        # The statement must end in `);` with nothing but known chained builder
        # calls in between: anything else means the line shape changed and the
        # parse would silently drop data.
        attrs = []
        for a in _ATTRIBUTE.finditer(rest):
            attrs.append((_NS + a.group("attr").lower(),
                          float(a.group("amount")),
                          _OPERATIONS[a.group("op")]))
        blend = (0, 0, 0)
        m1 = _BLEND1.search(rest)
        m3 = _BLEND3.search(rest)
        if m3:
            blend = (int(m3.group("i")), int(m3.group("o")), int(m3.group("a")))
        elif m1:
            t = int(m1.group("ticks"))
            blend = (t, t, t)
        residual = rest
        for a in _ATTRIBUTE.finditer(rest):
            residual = residual.replace(a.group(0), "")
        for pat in (_BLEND1, _BLEND3):
            residual = pat.sub("", residual)
        # Particle-type constructor args and the sound hook are the only other
        # things a registration carries; everything else must be gone.
        residual = re.sub(r",\s*ParticleTypes\.\w+", "", residual)
        residual = re.sub(r"\.withSoundOnAdded\([^)]*\)", "", residual)
        # A trailing spawn-count lambda swallows everything after it.
        if ", random" in residual:
            cut = residual.index(", random")
            tail, residual = residual[cut:], residual[:cut]
            if not _LAMBDA.match(tail):
                raise SystemExit(f"MobEffects.java:{lineno}: unparsed spawn-count "
                                 f"argument {tail!r}\n  {line.strip()}")
        residual = _BOOL_ARG.sub("", residual)
        residual = _FLOAT_ARG.sub("", residual)
        # All that may remain is the closing brackets of the constructor and
        # the register call: `)`, `);`, `))` or `));`.
        residual = residual.strip().strip(");").strip()
        if residual:
            raise SystemExit(f"MobEffects.java:{lineno}: unparsed trailing text "
                             f"{residual!r}\n  {line.strip()}")
        cls = m.group("cls")
        if cls not in _KINDS:
            raise SystemExit(f"MobEffects.java:{lineno}: unknown effect class "
                             f"{cls!r} — add it to _KINDS (plain or a behaviour)")
        color = (int(m.group("color"), 16) if m.group("color").lower().startswith("0x")
                 else int(m.group("color")))
        category = {"BENEFICIAL": "beneficial", "HARMFUL": "harmful",
                    "NEUTRAL": "neutral"}[m.group("category")]
        rows.append({
            "name": _NS + m.group("name"),
            "category": category,
            "color": color,
            "kind": _KINDS[cls],
            "instantaneous": cls in _INSTANTANEOUS,
            "harm": m.group("harm") == "true",
            "blend": blend,
            "attributes": attrs,
        })
    if not rows:
        raise SystemExit("no MobEffects.register(...) lines found")
    return rows


def apply_protocol_ids(rows: list, registries: pathlib.Path) -> list:
    """Sorts `rows` by protocol_id and checks the ids and names all agree."""
    data = json.loads(registries.read_text())["minecraft:mob_effect"]
    by_name = {name: entry["protocol_id"] for name, entry in data["entries"].items()}
    java_names = {r["name"] for r in rows}
    if java_names != set(by_name):
        missing = sorted(set(by_name) - java_names)
        extra = sorted(java_names - set(by_name))
        raise SystemExit(f"MobEffects.java disagrees with the registry report: "
                         f"missing={missing} extra={extra}")
    rows.sort(key=lambda r: by_name[r["name"]])
    ids = [by_name[r["name"]] for r in rows]
    if ids != list(range(len(rows))):
        raise SystemExit(f"mob_effect protocol ids are not dense 0..N-1: {ids}")
    for row, pid in zip(rows, ids):
        row["id"] = pid
    return rows


def read_tsv(path: pathlib.Path) -> list:
    rows, attrs = [], []
    mode = None
    for line in path.read_text().splitlines():
        line = line.rstrip("\n")
        if not line:
            continue
        if line.startswith("#"):
            mode = line.lstrip("#").strip()
            continue
        # The section headers are written unprefixed as well as commented, so a
        # line with no tab is a marker rather than a row.
        if "\t" not in line:
            mode = line.strip()
            continue
        parts = line.split("\t")
        if parts[0] in ("name", "effect"):
            continue  # the column headers, repeated here for readability
        if mode == "ATTRIBUTES":
            if len(parts) != 4:
                raise SystemExit(f"{path}: bad attribute row: {line!r}")
            attrs.append((parts[0], parts[1], float(parts[2]), parts[3]))
        else:
            if len(parts) != 10:
                raise SystemExit(f"{path}: bad effect row: {line!r}")
            rows.append({
                "name": parts[0],
                "id": int(parts[1]),
                "category": parts[2],
                "color": int(parts[3]),
                "kind": parts[4],
                "instantaneous": int(parts[5]),
                "harm": int(parts[6]),
                "blend": (int(parts[7]), int(parts[8]), int(parts[9])),
                "attributes": [],
            })
    for owner, attr, amount, op in attrs:
        for row in rows:
            if row["name"] == owner:
                row["attributes"].append((attr, amount, op))
                break
        else:
            raise SystemExit(f"{path}: attribute for unknown effect {owner}")
    if not rows:
        raise SystemExit(f"{path}: no effects")
    return rows


def render_tsv(rows: list) -> str:
    lines = [
        "# Generated by tools/gen_effect_table.py --reports ... --java ...",
        "#",
        "# minecraft:mob_effect is a *built-in* registry, so unlike the datapack",
        "# registries there is no data file to read. The ids come from the data",
        "# generator's registries.json and everything else from the decompiled",
        "# MobEffects.java, which is the only place an effect's colour and",
        "# attribute modifiers exist (they are constructor arguments).",
        "#",
        "# The `kind` column names the applyEffectTick subclass the effect is",
        "# built from; `instantaneous` marks an InstantaneousMobEffect subclass.",
        "EFFECTS",
        _TSV_HEADER,
    ]
    for r in rows:
        b = r["blend"]
        lines.append("\t".join([r["name"], str(r["id"]), r["category"], str(r["color"]),
                                r["kind"], str(int(r["instantaneous"])),
                                str(int(r["harm"])),
                                str(b[0]), str(b[1]), str(b[2])]))
    lines += ["", "ATTRIBUTES", _TSV_ATTR_HEADER]
    for r in rows:
        for attr, amount, op in r["attributes"]:
            lines.append("\t".join([r["name"], attr, f"{amount:g}", op]))
    return "\n".join(lines) + "\n"


def render(rows: list) -> str:
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        f"// The {len(rows)} 26.2 minecraft:mob_effect entries, in protocol-id order",
        "// (generated — do not hand-edit).",
        "//",
        "// minecraft:mob_effect is a *built-in* registry, so the wire id in an",
        "// update_mob_effect / remove_mob_effect packet is the protocol_id the",
        "// data generator's registries.json assigns — which for this registry is",
        "// also the registration order (the generator checks that the ids are",
        "// dense 0..N-1). The table index *is* that id.",
        "//",
        "// Everything that is not an id — the colour the client tints the",
        "// particles with, the category, the attribute modifiers and the blend",
        "// durations — is compiled into MobEffects.java rather than carried in",
        "// any data file, so those columns come from parsing the decompiled",
        "// net/minecraft/world/effect/MobEffects.java.",
        "//",
        "// `amount` on a modifier is *per level*: MobEffect.AttributeTemplate",
        "// .create multiplies it by (amplifier + 1) at runtime, which is why",
        "// Speed II is +40% and not +20%.",
        "//",
        "// Regenerate with tools/gen_effect_table.py.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mc::world {",
        "",
        "// MobEffectCategory. The ordinal is vanilla's enum order.",
        "constexpr uint8_t kEffectCategoryBeneficial = 0;",
        "constexpr uint8_t kEffectCategoryHarmful = 1;",
        "constexpr uint8_t kEffectCategoryNeutral = 2;",
        "",
        "// The applyEffectTick subclass an effect is built from, i.e. what it does",
        "// on the ticks it fires. `plain` is MobEffect itself (a no-op that returns",
        "// true), which is what an effect with no server-visible behaviour ticks on.",
        "constexpr uint8_t kEffectPlain = 0;",
        "constexpr uint8_t kEffectRegeneration = 1;",
        "constexpr uint8_t kEffectPoison = 2;",
        "constexpr uint8_t kEffectWither = 3;",
        "constexpr uint8_t kEffectHunger = 4;",
        "constexpr uint8_t kEffectSaturation = 5;",
        "constexpr uint8_t kEffectInstantHealthOrHarm = 6;",
        "constexpr uint8_t kEffectAbsorption = 7;",
        "constexpr int kEffectKindCount = 8;",
        "",
        "// AttributeModifier.Operation, in vanilla's enum order.",
        "constexpr uint8_t kModifierAddValue = 0;",
        "constexpr uint8_t kModifierAddMultipliedBase = 1;",
        "constexpr uint8_t kModifierAddMultipliedTotal = 2;",
        "",
        "struct GeneratedEffectModifier {",
        "  const char* attribute;   // minecraft:movement_speed",
        "  float amount;            // per level: scaled by (amplifier + 1) at runtime",
        "  uint8_t operation;       // kModifier*",
        "};",
        "",
        "struct GeneratedMobEffect {",
        "  const char* name;                // minecraft:speed",
        "  uint8_t category;                // kEffectCategory*",
        "  uint8_t kind;                    // kEffect*",
        "  bool instantaneous;              // MobEffect.isInstantaneous()",
        "  bool harm;                      // HealOrHarmMobEffect's isHarm flag",
        "                                  // (instant_damage rather than _health)",
        "  uint8_t blend_in;                // MobEffect.getBlendInDurationTicks",
        "  uint8_t blend_out;               // ...setBlendDuration's out ticks",
        "  uint8_t blend_out_advance;       // ...and the advance, for Darkness",
        "  uint32_t color;                  // MobEffect.getColor (the client's tint)",
        "  const GeneratedEffectModifier* modifiers;",
        "  int modifier_count;",
        "};",
        "",
    ]
    if not any(r["attributes"] for r in rows):
        lines += [
            "inline constexpr GeneratedEffectModifier kEffectModifiersNone[] = {};",
            "",
        ]
    for r in rows:
        if not r["attributes"]:
            continue
        symbol = "k" + "".join(p.capitalize() for p in r["name"].split(":", 1)[1].split("_"))
        lines.append(f"// clang-format off")
        lines.append(f"inline constexpr GeneratedEffectModifier {symbol}Modifiers[] = {{")
        for attr, amount, op in r["attributes"]:
            # 0.0 reads as an int literal without a decimal point.
            text = f"{amount:.7g}"
            if "." not in text and "e" not in text and "E" not in text:
                text += ".0"
            lines.append(f'    {{"{attr}", {text}f, kModifier'
                         + "".join(p.capitalize() for p in op.split("_"))
                         + "},")
        lines.append("};")
        lines.append("// clang-format on")
    lines += [
        "",
        "// clang-format off",
        "// The table index is the protocol id.",
        "inline constexpr GeneratedMobEffect kMobEffects[] = {",
    ]
    kinds = {
        "plain": "kEffectPlain",
        "regeneration": "kEffectRegeneration",
        "poison": "kEffectPoison",
        "wither": "kEffectWither",
        "hunger": "kEffectHunger",
        "saturation": "kEffectSaturation",
        "instant_health_or_harm": "kEffectInstantHealthOrHarm",
        "absorption": "kEffectAbsorption",
    }
    for r in rows:
        if r["attributes"]:
            symbol = "k" + "".join(p.capitalize() for p in r["name"].split(":", 1)[1].split("_"))
            mods = f"{symbol}Modifiers"
            count = len(r["attributes"])
        else:
            mods, count = "nullptr", 0
        lines.append(
            f'    {{"{r["name"]}", {r["category"].replace("beneficial", "kEffectCategoryBeneficial").replace("harmful", "kEffectCategoryHarmful").replace("neutral", "kEffectCategoryNeutral")}, '
            f'{kinds[r["kind"]]}, {"true" if r["instantaneous"] else "false"}, '
            f'{"true" if r["harm"] else "false"}, '
            f'{r["blend"][0]}, {r["blend"][1]}, {r["blend"][2]}, 0x{r["color"]:08x}u, '
            f'{mods}, {count}}},')
    lines += [
        "};",
        f"inline constexpr int kMobEffectCount = {len(rows)};",
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
    parser.add_argument("--java", type=pathlib.Path,
                        help="decompiled net/minecraft/world/effect/MobEffects.java")
    parser.add_argument("--tsv", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.2/mob_effects.tsv"),
                        help="the flattened extraction (written in --reports mode)")
    parser.add_argument("--header", type=pathlib.Path,
                        default=pathlib.Path("src/world/effect_table.h"))
    args = parser.parse_args()

    if args.reports:
        if not args.java:
            raise SystemExit("--reports needs --java (the colours are code-only)")
        rows = apply_protocol_ids(parse_java(args.java.read_text()), args.reports / "registries.json")
        args.tsv.parent.mkdir(parents=True, exist_ok=True)
        args.tsv.write_text(render_tsv(rows))
        attrs = sum(len(r["attributes"]) for r in rows)
        print(f"wrote {args.tsv} ({len(rows)} effects, {attrs} modifiers)")
    else:
        rows = read_tsv(args.tsv)

    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(render(rows))
    print(f"wrote {args.header} ({len(rows)} effects)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
