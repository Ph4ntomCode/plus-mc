#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/world/biome_table.h: the biome climate table weather reads.

Precipitation is a *data* property of a biome in 26.2 — `has_precipitation`,
`temperature` and `temperature_modifier` live in the biome JSON, and
`Biome.getPrecipitationAt(pos, seaLevel)` is the whole rule:

    if (!this.hasPrecipitation()) return Precipitation.NONE;
    return this.coldEnoughToSnow(pos, seaLevel) ? Precipitation.SNOW
                                               : Precipitation.RAIN;

`has_precipitation: false` (the_void, the nether and end biomes) is what makes
rain impossible in a biome, which is why this table is the difference between
"the server is raining" and "rain falls here": `Level.precipitationAt` is the
only way `isRainingAt` (fire's rain removal, a burning entity's clearFire) and
the cauldron's `handlePrecipitation` are ever reached.

`coldEnoughToSnow` compares the height-adjusted temperature against 0.15, so the
base `temperature` plus a `frozen` modifier is what this table carries. Both
noise terms in `getHeightAdjustedTemperature` are documented omissions in
world/weather.h (a warm world never reaches them).

Usage:
    tools/gen_biome_table.py --biomes-dir data/vanilla/26.2/data/minecraft/worldgen/biome
    tools/gen_biome_table.py --tsv data/vanilla/26.2/biome_climate.tsv
"""

import argparse
import json
import pathlib
import sys

_NS = "minecraft:"


def registry_names(registry_path):
    """The advertised worldgen/biome order the client was given.

    A biome the client was told about but that this table lacks would silently
    answer "no precipitation" for a chunk that advertises it, so the two lists
    are checked against each other rather than trusted.
    """
    names = []
    for line in registry_path.read_text().splitlines():
        line = line.strip()
        if line:
            names.append(line)
    return names


def biome_rows(biomes_dir):
    """One row per biome JSON: (name, has_precipitation, temperature, frozen)."""
    rows = []
    for path in sorted(biomes_dir.glob("*.json")):
        data = json.loads(path.read_text())
        name = _NS + path.stem
        has_precipitation = bool(data.get("has_precipitation", True))
        temperature = float(data["temperature"])
        # `TemperatureModifier.CODEC.optionalFieldOf("temperature_modifier",
        # TemperatureModifier.NONE)`.
        modifier = data.get("temperature_modifier", "none")
        if modifier not in ("none", "frozen"):
            raise SystemExit(f"{path}: unknown temperature_modifier {modifier!r}")
        rows.append((name, has_precipitation, temperature, modifier == "frozen"))
    rows.sort(key=lambda row: row[0])
    return rows


def write_tsv(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = ["# biome\thas_precipitation\ttemperature\ttemperature_modifier"]
    for name, has_precipitation, temperature, frozen in rows:
        lines.append(
            f"{name}\t{int(has_precipitation)}\t{temperature:g}\t"
            f"{'frozen' if frozen else 'none'}")
    path.write_text("\n".join(lines) + "\n")


def write_header(path, rows):
    precipitating = sum(1 for _, has, _, _ in rows if has)
    body = "\n".join(
        '    {{"{name}", {has}, {temp}f, {frozen}}},'.format(
            name=name, has="true" if has_precipitation else "false",
            temp=repr(temperature), frozen="true" if frozen else "false")
        for name, has_precipitation, temperature, frozen in rows)
    path.write_text(f"""// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// Biome climate ({len(rows)} biomes, {precipitating} with precipitation —
// generated, do not hand-edit): the three fields `Biome.getPrecipitationAt`
// reads, transcribed from data/minecraft/worldgen/biome/*.json.
//
// `has_precipitation` is the gate: a biome without it answers
// `Biome.Precipitation.NONE` everywhere, so fire is never rained out in it and
// its cauldrons never fill (world/weather.h's `biome_precipitation_at`).
// `temperature` is compared against 0.15 (warm enough to rain) to pick RAIN or
// SNOW, and `temperature_modifier: frozen` is carried for completeness even
// though both of the modifier's noise fields are omitted (see world/weather.h).
//
// Regenerate with tools/gen_biome_table.py.
#pragma once

#include <cstdint>

namespace mc::world {{

struct BiomeClimate {{
  const char* name; // "minecraft:plains"
  bool has_precipitation;
  float temperature;
  bool frozen_modifier; // `temperature_modifier: minecraft:frozen`
}};

// Sorted by name, so biome_climate_by_name can binary-search it.
inline constexpr BiomeClimate kBiomeClimate[] = {{
{body}
}};

}} // namespace mc::world
""")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--biomes-dir", type=pathlib.Path,
                        default=pathlib.Path(
                            "data/vanilla/26.2/data/minecraft/worldgen/biome"),
                        help="the extracted biome JSONs")
    parser.add_argument("--registry", type=pathlib.Path,
                        default=pathlib.Path(
                            "data/vanilla/26.2/clientconfig/registries/"
                            "worldgen_biome.txt"),
                        help="the advertised worldgen/biome order")
    parser.add_argument("--tsv", type=pathlib.Path,
                        default=pathlib.Path("data/vanilla/26.2/biome_climate.tsv"),
                        help="the flattened extraction")
    parser.add_argument("--header", type=pathlib.Path,
                        default=pathlib.Path("src/world/biome_table.h"))
    args = parser.parse_args()

    if not args.biomes_dir.is_dir():
        raise SystemExit(f"no biome JSONs at {args.biomes_dir} (run tools/vanilla-data)")
    rows = biome_rows(args.biomes_dir)
    known = {name for name, _, _, _ in rows}
    advertised = registry_names(args.registry)
    missing = [n for n in advertised if n not in known]
    extra = sorted(known - set(advertised))
    if missing:
        raise SystemExit(f"{len(missing)} advertised biomes have no JSON: {missing[:5]}")
    if extra:
        raise SystemExit(f"{len(extra)} JSON biomes are not in the registry: {extra[:5]}")

    write_tsv(args.tsv, rows)
    write_header(args.header, rows)
    print(f"wrote {args.header}: {len(rows)} biomes, "
          f"{sum(1 for _, h, _, _ in rows if h)} with precipitation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
