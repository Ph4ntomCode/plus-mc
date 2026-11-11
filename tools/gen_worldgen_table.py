#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates `src/world/worldgen_table.h`: the overworld noise router, as data.

Phase 4 wants "a world generator that reads vanilla's own JSON (density
functions etc.)", and this is the reading half. The tool walks the *actual*
`data/minecraft/worldgen` tree of an extracted version:

  * `noise_settings/overworld.json` — the router (fifteen density functions),
    the noise settings (min_y, height, cell sizes), the default block and
    fluid, sea level and the aquifer/ore-vein switches;
  * every `density_function/**/*.json` the router reaches, transitively,
    resolved by *name* the way the registry does — one node per named function
    (emitted as `kWgNamed`, so a test can name one), inline definitions become
    their own nodes;
  * every `noise/*.json` (the `NormalNoise` parameter sets), all of them, so
    the biome and surface work later has the table it needs.

The emitted table is a flat node list — children are indices, not pointers —
because the runtime has to reproduce vanilla's *evaluation order* too: the
`interpolated` / `cache_once` / `cache_2d` / `flat_cache` markers are nodes,
and `noise_chunk.cpp` wraps each one exactly as `NoiseChunk.wrapNew` does.
Constants are emitted as C++ hex float literals so the table is bit-exact;
`CubicSpline` points are JSON floats (IEEE binary32), and this tool rounds the
decimal text to the nearest float32 itself rather than double-rounding through
Python's binary64.

Usage:
    gen_worldgen_table.py <worldgen dir> <out header>

e.g.
    gen_worldgen_table.py data/vanilla/26.2/data/minecraft/worldgen \\
        src/world/worldgen_table.h
"""

import argparse
import json
import pathlib
import struct
import sys
from fractions import Fraction

# The density-function node kinds. The names are the JSON type names minus the
# namespace; `kBeardifier` and the two blend markers have no fields.
KINDS = [
    "kConstant",
    "kYClampedGradient",
    "kNoise",
    "kShiftedNoise",
    "kShiftA",
    "kShiftB",
    "kShift",
    "kRangeChoice",
    "kIntervalSelect",
    "kClamp",
    "kAbs",
    "kSquare",
    "kCube",
    "kHalfNegative",
    "kQuarterNegative",
    "kInvert",
    "kSqueeze",
    "kAdd",
    "kMul",
    "kMin",
    "kMax",
    "kSpline",
    "kFindTopSurface",
    "kInterpolated",
    "kFlatCache",
    "kCache2D",
    "kCacheOnce",
    "kCacheAllInCell",
    "kBlendDensity",
    "kBlendAlpha",
    "kBlendOffset",
    "kBeardifier",
    "kBlendedNoise",
    # A code-only density kind (`DensityFunctions.EndIslandDensityFunction`):
    # the End's `end_islands` node has no JSON payload beyond its type.
    "kEndIslands",
]

# The fifteen router fields, in `NoiseRouter`'s own order.
ROUTER_FIELDS = [
    "barrier",
    "fluid_level_floodedness",
    "fluid_level_spread",
    "lava",
    "temperature",
    "vegetation",
    "continents",
    "erosion",
    "depth",
    "ridges",
    "preliminary_surface_level",
    "final_density",
    "vein_toggle",
    "vein_ridged",
    "vein_gap",
]

MARKERS = {
    "minecraft:interpolated": "kInterpolated",
    "minecraft:flat_cache": "kFlatCache",
    "minecraft:cache_2d": "kCache2D",
    "minecraft:cache_once": "kCacheOnce",
    "minecraft:cache_all_in_cell": "kCacheAllInCell",
    "minecraft:blend_density": "kBlendDensity",
}

MAPPED = {
    "minecraft:abs": "kAbs",
    "minecraft:square": "kSquare",
    "minecraft:cube": "kCube",
    "minecraft:half_negative": "kHalfNegative",
    "minecraft:quarter_negative": "kQuarterNegative",
    "minecraft:invert": "kInvert",
    "minecraft:squeeze": "kSqueeze",
}

TWO_ARG = {
    "minecraft:add": "kAdd",
    "minecraft:mul": "kMul",
    "minecraft:min": "kMin",
    "minecraft:max": "kMax",
}

# `RandomState.wrapNew` swaps these two noise parameter sets to the legacy
# (non-positional) initialisation; the overworld router does not use them, but
# the table has to say so rather than pretend they are ordinary.
LEGACY_NOISES = {"minecraft:nether/temperature", "minecraft:nether/vegetation"}


def f32(text: str) -> float:
    """Rounds a JSON decimal to the nearest IEEE-754 binary32 (ties to even).

    `Codec.FLOAT` parses the text to the nearest float, not to the nearest
    double and then to the nearest float, so the distinction matters for the
    rare half-way decimal (and the spline tables are full of decimals).
    """
    if isinstance(text, float):
        value = Fraction(text)
    else:
        value = Fraction(str(text))
    if value == 0:
        return 0.0
    negative = value < 0
    value = abs(value)
    # Normalise to m * 2^e with 2^23 <= m < 2^24 (or subnormal).
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    while Fraction(2) ** exponent > value:
        exponent -= 1
    while Fraction(2) ** (exponent + 1) <= value:
        exponent += 1
    # Now 2^exponent <= value < 2^(exponent+1); scale so the mantissa is 24 bits.
    shift = 23 - exponent
    scaled = value * Fraction(2) ** shift
    mantissa = scaled.numerator // scaled.denominator
    remainder = scaled - mantissa
    if remainder > Fraction(1, 2) or (remainder == Fraction(1, 2) and mantissa % 2 == 1):
        mantissa += 1
    if mantissa >= 1 << 24:
        mantissa >>= 1
        exponent += 1
    if exponent < -126:
        # Subnormal: re-scale to the smallest exponent.
        scaled = value * Fraction(2) ** 149
        mantissa = scaled.numerator // scaled.denominator
        remainder = scaled - mantissa
        if remainder > Fraction(1, 2) or (remainder == Fraction(1, 2) and mantissa % 2 == 1):
            mantissa += 1
        result = mantissa / 2**149
    else:
        result = mantissa * 2.0 ** (exponent - 23)
    result_f = struct.unpack("f", struct.pack("f", result))[0]
    return -result_f if negative else result_f


def hex_double(value: float) -> str:
    return value.hex()


def hex_float(value: float) -> str:
    return value.hex() + "f"


class Generator:
    def __init__(self, worldgen: pathlib.Path, dimension: str = "overworld"):
        self.root = worldgen
        self.dimension = dimension
        self.nodes: list[dict] = []
        self.named: dict[str, int] = {}
        self.children: list[int] = []
        self.thresholds: list[float] = []
        self.splines: list[dict] = []
        self.spline_points: list[dict] = []
        self.blended: list[dict] = []
        self.noise_params: dict[str, dict] = {}
        self.noise_order: list[str] = []
        self.settings = json.loads(
            (self.root / f"noise_settings/{dimension}.json").read_text())
        self._load_noises()

    # ---- helpers -----------------------------------------------------------
    def density_json(self, name: str) -> dict:
        path = self.root / "density_function" / (name.split(":", 1)[1] + ".json")
        return json.loads(path.read_text())

    def add_node(self, **fields) -> int:
        fields.setdefault("i0", 0)
        fields.setdefault("i1", 0)
        fields.setdefault("i2", 0)
        fields.setdefault("i3", 0)
        fields.setdefault("d0", 0.0)
        fields.setdefault("d1", 0.0)
        self.nodes.append(fields)
        return len(self.nodes) - 1

    def named_node(self, name: str) -> int:
        if name in self.named:
            if self.named[name] < 0:
                raise SystemExit(f"density function reference cycle at {name}")
            return self.named[name]
        self.named[name] = -1  # cycle guard while the definition is walked
        index = self.node_for(self.density_json(name), name)
        self.named[name] = index
        return index

    def _load_noises(self) -> None:
        for path in sorted((self.root / "noise").rglob("*.json")):
            name = "minecraft:" + str(path.relative_to(self.root / "noise").with_suffix(""))
            doc = json.loads(path.read_text())
            self.noise_params[name] = doc
            self.noise_order.append(name)

    def noise_index(self, name: str) -> int:
        if name not in self.noise_params:
            raise SystemExit(f"unknown noise parameter set: {name}")
        return self.noise_order.index(name)

    def spline_index(self, doc) -> int:
        if isinstance(doc, (int, float)) and not isinstance(doc, bool):
            # `Codec.either(FLOAT, Multipoint)` maps a bare number to
            # `CubicSpline.Constant`; keep it as its own leaf kind so the
            # sampler does not have to fake a one-point multipoint.
            self.splines.append({"coordinate_index": 0, "point_offset": 0, "point_count": 0,
                                 "constant": f32(doc)})
            return len(self.splines) - 1
        # Resolve the points into a local list first: a nested spline appends
        # its own points, and the parent's `point_offset`..`point_offset+count`
        # range has to stay contiguous or the sampler would read the nested
        # spline's points as its own.
        points = []
        for point in doc["points"]:
            value = point["value"]
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                value_constant = f32(value)
                value_spline = -1
            else:
                value_constant = 0.0
                value_spline = self.spline_index(value)
            points.append({
                "location": f32(point["location"]),
                "derivative": f32(point.get("derivative", 0.0)),
                "constant": value_constant,
                "spline": value_spline,
            })
        point_offset = len(self.spline_points)
        self.spline_points.extend(points)
        self.splines.append({
            "coordinate_index": self.node_for(doc["coordinate"]),
            "point_offset": point_offset,
            "point_count": len(doc["points"]),
            "constant": None,
        })
        return len(self.splines) - 1

    def blended_index(self, doc: dict) -> int:
        params = {
            "xz_scale": float(doc["xz_scale"]),
            "y_scale": float(doc["y_scale"]),
            "xz_factor": float(doc["xz_factor"]),
            "y_factor": float(doc["y_factor"]),
            "smear_scale_multiplier": float(doc["smear_scale_multiplier"]),
        }
        self.blended.append(params)
        return len(self.blended) - 1

    # ---- the walk ----------------------------------------------------------
    def node_for(self, doc, debug_name: str | None = None) -> int:
        if isinstance(doc, str):
            if not doc.startswith("minecraft:"):
                raise SystemExit(f"non-minecraft density function reference: {doc}")
            return self.named_node(doc)
        if isinstance(doc, bool):
            raise SystemExit("boolean is not a density function")
        if isinstance(doc, (int, float)):
            return self.add_node(kind="kConstant", d0=float(doc))
        kind = doc["type"]
        if kind in MAPPED:
            return self.add_node(kind=MAPPED[kind], i0=self.node_for(doc["argument"]))
        if kind in TWO_ARG:
            return self.add_node(kind=TWO_ARG[kind],
                                 i0=self.node_for(doc["argument1"]),
                                 i1=self.node_for(doc["argument2"]))
        if kind in MARKERS:
            return self.add_node(kind=MARKERS[kind], i0=self.node_for(doc["argument"]))
        if kind == "minecraft:noise":
            return self.add_node(kind="kNoise", i0=self.noise_index(doc["noise"]),
                                 d0=float(doc["xz_scale"]), d1=float(doc["y_scale"]))
        if kind == "minecraft:shifted_noise":
            return self.add_node(kind="kShiftedNoise",
                                 i0=self.node_for(doc["shift_x"]),
                                 i1=self.node_for(doc["shift_y"]),
                                 i2=self.node_for(doc["shift_z"]),
                                 i3=self.noise_index(doc["noise"]),
                                 d0=float(doc["xz_scale"]), d1=float(doc["y_scale"]))
        if kind in ("minecraft:shift_a", "minecraft:shift_b", "minecraft:shift"):
            table = {"minecraft:shift_a": "kShiftA", "minecraft:shift_b": "kShiftB",
                     "minecraft:shift": "kShift"}
            return self.add_node(kind=table[kind], i0=self.noise_index(doc["argument"]))
        if kind == "minecraft:range_choice":
            return self.add_node(kind="kRangeChoice",
                                 i0=self.node_for(doc["input"]),
                                 i1=self.node_for(doc["when_in_range"]),
                                 i2=self.node_for(doc["when_out_of_range"]),
                                 d0=float(doc["min_inclusive"]), d1=float(doc["max_exclusive"]))
        if kind == "minecraft:interval_select":
            functions = [self.node_for(f) for f in doc["functions"]]
            offset = len(self.children)
            self.children.extend(functions)
            threshold_offset = len(self.thresholds)
            self.thresholds.extend(float(t) for t in doc["thresholds"])
            return self.add_node(kind="kIntervalSelect",
                                 i0=self.node_for(doc["input"]),
                                 i1=offset,
                                 i2=len(functions),
                                 i3=threshold_offset)
        if kind == "minecraft:clamp":
            return self.add_node(kind="kClamp", i0=self.node_for(doc["input"]),
                                 d0=float(doc["min"]), d1=float(doc["max"]))
        if kind == "minecraft:spline":
            return self.add_node(kind="kSpline", i0=self.spline_index(doc["spline"]))
        if kind == "minecraft:constant":
            return self.add_node(kind="kConstant", d0=float(doc["value"]))
        if kind == "minecraft:y_clamped_gradient":
            return self.add_node(kind="kYClampedGradient",
                                 i0=int(doc["from_y"]), i1=int(doc["to_y"]),
                                 d0=float(doc["from_value"]), d1=float(doc["to_value"]))
        if kind == "minecraft:find_top_surface":
            return self.add_node(kind="kFindTopSurface",
                                 i0=self.node_for(doc["density"]),
                                 i1=self.node_for(doc["upper_bound"]),
                                 i2=int(doc["lower_bound"]),
                                 i3=int(doc["cell_height"]))
        if kind == "minecraft:blend_alpha":
            return self.add_node(kind="kBlendAlpha")
        if kind == "minecraft:blend_offset":
            return self.add_node(kind="kBlendOffset")
        if kind == "minecraft:beardifier":
            return self.add_node(kind="kBeardifier")
        if kind == "minecraft:old_blended_noise":
            return self.add_node(kind="kBlendedNoise", i0=self.blended_index(doc))
        if kind == "minecraft:end_islands":
            return self.add_node(kind="kEndIslands")
        raise SystemExit(f"unmodelled density function type: {kind} ({debug_name or ''})")

    def build(self) -> None:
        router = self.settings["noise_router"]
        self.router = {}
        for field in ROUTER_FIELDS:
            self.router[field] = self.node_for(router[field], f"<router:{field}>")
        # `NoiseChunk`'s own root: cache_all_in_cell(add(final_density, beardifier)).
        beardifier = self.add_node(kind="kBeardifier")
        final_add = self.add_node(kind="kAdd", i0=self.router["final_density"], i1=beardifier)
        self.noise_density = self.add_node(kind="kCacheAllInCell", i0=final_add)


def emit(gen: Generator) -> str:  # noqa: C901 - one flat emitter, one pass over the tables
    s = gen.settings
    noise = s["noise"]
    lines: list[str] = []
    a = lines.append
    a("// SPDX-License-Identifier: GPL-3.0-or-later")
    a("// Generated by tools/gen_worldgen_table.py — do not edit by hand.")
    a("//")
    a("// The overworld noise router of the 26.2 data pack, compiled from")
    a("// data/minecraft/worldgen: the fifteen router entries, every density function")
    a("// they reach (the named ones are listed in `kWgNamed`, plus the inline nodes),")
    a("// and every noise parameter set in the pack. Constants are C++ hex float")
    a("// literals, so the table carries the pack's exact bit patterns and not a")
    a("// printed approximation.")
    a("//")
    a("// Nodes are a flat list; children are indices. `worldgen_density.cpp` walks it;")
    a("// `worldgen_chunk.cpp` wraps the marker nodes (`interpolated`, `cache_*`, ...)")
    a("// with the same state vanilla's `NoiseChunk` keeps.")
    a("// clang-format off")
    a("#pragma once")
    a("")
    a("#include <cstdint>")
    a("")
    a("namespace mc::world {")
    a("")
    a("struct WgNoiseParams {")
    a("  const char* name;")
    a("  int32_t first_octave;")
    a("  uint32_t amplitude_offset;")
    a("  uint32_t amplitude_count;")
    a("  bool legacy_random;")
    a("};")
    a("")
    a(f"inline constexpr WgNoiseParams kWgNoiseParams[{len(gen.noise_order)}] = {{")
    amplitudes: list[float] = []
    for name in gen.noise_order:
        doc = gen.noise_params[name]
        offset = len(amplitudes)
        amps = [float(v) for v in doc["amplitudes"]]
        amplitudes.extend(amps)
        legacy = "true" if name in LEGACY_NOISES else "false"
        a(f"    {{\"{name}\", {doc['firstOctave']}, {offset}, {len(amps)}, {legacy}}},")
    a("};")
    a("")
    a(f"inline constexpr double kWgNoiseAmplitudes[{len(amplitudes)}] = {{")
    for i in range(0, len(amplitudes), 4):
        a("    " + ", ".join(hex_double(v) for v in amplitudes[i:i + 4]) + ",")
    a("};")
    a("")
    a("enum class WgDfKind : uint8_t {")
    for kind in KINDS:
        a(f"  {kind},")
    a("};")
    a("")
    a("struct WgDf {")
    a("  WgDfKind kind;")
    a("  int32_t i0, i1, i2, i3;")
    a("  double d0, d1;")
    a("};")
    a("")
    a(f"inline constexpr WgDf kWgDfs[{len(gen.nodes)}] = {{")
    for node in gen.nodes:
        a(f"    {{WgDfKind::{node['kind']}, {node['i0']}, {node['i1']}, {node['i2']}, "
          f"{node['i3']}, {hex_double(node['d0'])}, {hex_double(node['d1'])}}},")
    a("};")
    a("")
    if gen.children:
        a(f"inline constexpr uint32_t kWgDfChildren[{len(gen.children)}] = {{")
        for i in range(0, len(gen.children), 8):
            a("    " + ", ".join(str(v) for v in gen.children[i:i + 8]) + ",")
        a("};")
        a("")
    if gen.thresholds:
        a(f"inline constexpr double kWgDfThresholds[{len(gen.thresholds)}] = {{")
        for i in range(0, len(gen.thresholds), 4):
            a("    " + ", ".join(hex_double(v) for v in gen.thresholds[i:i + 4]) + ",")
        a("};")
        a("")
    if gen.splines:
        a("struct WgSpline {")
        a("  uint32_t coordinate;")
        a("  uint32_t point_offset;")
        a("  uint32_t point_count;")
        a("  float constant;")
        a("  bool is_constant;")
        a("};")
        a("")
        a("struct WgSplinePoint {")
        a("  float location;")
        a("  float derivative;")
        a("  float constant;")
        a("  int32_t spline;")
        a("};")
        a("")
        a(f"inline constexpr WgSpline kWgSplines[{len(gen.splines)}] = {{")
        for spline in gen.splines:
            if spline["constant"] is None:
                a(f"    {{{spline['coordinate_index']}, {spline['point_offset']}, "
                  f"{spline['point_count']}, 0.0f, false}},")
            else:
                a(f"    {{0, {spline['point_offset']}, {spline['point_count']}, "
                  f"{hex_float(spline['constant'])}, true}},")
        a("};")
        a("")
        a(f"inline constexpr WgSplinePoint kWgSplinePoints[{len(gen.spline_points)}] = {{")
        for point in gen.spline_points:
            a(f"    {{{hex_float(point['location'])}, {hex_float(point['derivative'])}, "
              f"{hex_float(point['constant'])}, {point['spline']}}},")
        a("};")
        a("")
    if gen.blended:
        a("struct WgBlendedNoiseParams {")
        a("  double xz_scale, y_scale, xz_factor, y_factor, smear_scale_multiplier;")
        a("};")
        a("")
        a(f"inline constexpr WgBlendedNoiseParams "
          f"kWgBlendedNoise[{len(gen.blended)}] = {{")
        for params in gen.blended:
            a("    {" + ", ".join(hex_double(params[k]) for k in
                                 ("xz_scale", "y_scale", "xz_factor", "y_factor",
                                  "smear_scale_multiplier")) + "},")
        a("};")
        a("")
    a("enum class WgRouterField : uint8_t {")
    for field in ROUTER_FIELDS:
        a(f"  k{''.join(part.title() for part in field.split('_'))},")
    a("};")
    a("")
    a(f"inline constexpr uint32_t kWgRouter[{len(ROUTER_FIELDS)}] = {{")
    a("    " + ", ".join(str(gen.router[field]) for field in ROUTER_FIELDS) + ",")
    a("};")
    a("")
    a("// The named density functions the router reaches, by registry name. A named")
    a("// function is one node shared by every reference (`HolderHolder` in vanilla),")
    a("// which is also what makes `worldgen_density.cpp`'s wrapper memo give them a")
    a("// single `CacheOnce` slot apiece.")
    a("struct WgNamedDf {")
    a("  const char* name;")
    a("  uint32_t node;")
    a("};")
    a("")
    a(f"inline constexpr WgNamedDf kWgNamed[{len(gen.named)}] = {{")
    for name in sorted(gen.named):
        a(f'    {{"{name}", {gen.named[name]}}},')
    a("};")
    a("")
    a("// `NoiseChunk`'s block-state rule root: cache_all_in_cell(add(final_density,"
      " beardifier)).")
    a(f"inline constexpr uint32_t kWgNoiseDensityRoot = {gen.noise_density};")
    a("")
    a("// NoiseGeneratorSettings, read from noise_settings/overworld.json.")
    a(f"inline constexpr int32_t kWgMinY = {noise['min_y']};")
    a(f"inline constexpr int32_t kWgHeight = {noise['height']};")
    a(f"inline constexpr int32_t kWgCellWidth = {4 * noise['size_horizontal']};")
    a(f"inline constexpr int32_t kWgCellHeight = {4 * noise['size_vertical']};")
    a(f"inline constexpr int32_t kWgSeaLevel = {s['sea_level']};")
    a(f"inline constexpr bool kWgAquifersEnabled = {'true' if s['aquifers_enabled'] else 'false'};")
    a(f"inline constexpr bool kWgOreVeinsEnabled = {'true' if s['ore_veins_enabled'] else 'false'};")
    a(f"inline constexpr bool kWgLegacyRandomSource = "
      f"{'true' if s.get('legacy_random_source', False) else 'false'};")
    default_block = s["default_block"]["Name"]
    default_fluid = s["default_fluid"]["Name"]
    fluid_level = s["default_fluid"].get("Properties", {}).get("level", "0")
    a(f"inline constexpr const char* kWgDefaultBlock = \"{default_block}\";")
    a(f"inline constexpr const char* kWgDefaultFluid = \"{default_fluid}\";")
    a(f"inline constexpr int32_t kWgDefaultFluidLevel = {fluid_level};")
    a("")
    a("}  // namespace mc::world")
    a("// clang-format on")
    return "\n".join(lines) + "\n"


def emit_dimension(gen: Generator, dimension: str) -> str:
    """Emit one non-overworld table, prefixed so it can share a translation unit
    with the overworld table and the shared `WgDf` / `WgSpline` types."""
    prefix = "Nether" if dimension == "nether" else "End"
    s = gen.settings
    noise = s["noise"]
    lines: list[str] = []
    a = lines.append
    a("// SPDX-License-Identifier: GPL-3.0-or-later")
    a("// Generated by tools/gen_worldgen_table.py — do not edit by hand.")
    a("//")
    a(f"// The {dimension} noise router of the 26.2 data pack. The node kinds,")
    a("// spline and blended-noise shapes are `worldgen_table.h`'s; only the arrays")
    a(f"// are dimension-specific (prefix `kWg{prefix}`).")
    a("// clang-format off")
    a("#pragma once")
    a("")
    a('#include "world/worldgen_table.h"')
    a("")
    a("namespace mc::world {")
    a("")
    a(f"inline constexpr WgDf kWg{prefix}Dfs[{len(gen.nodes)}] = {{")
    for node in gen.nodes:
        a(f"    {{WgDfKind::{node['kind']}, {node['i0']}, {node['i1']}, {node['i2']}, "
          f"{node['i3']}, {hex_double(node['d0'])}, {hex_double(node['d1'])}}},")
    a("};")
    a("")
    if gen.children:
        a(f"inline constexpr uint32_t kWg{prefix}DfChildren[{len(gen.children)}] = {{")
        for i in range(0, len(gen.children), 8):
            a("    " + ", ".join(str(v) for v in gen.children[i:i + 8]) + ",")
        a("};")
        a("")
    if gen.thresholds:
        a(f"inline constexpr double kWg{prefix}DfThresholds[{len(gen.thresholds)}] = {{")
        for i in range(0, len(gen.thresholds), 4):
            a("    " + ", ".join(hex_double(v) for v in gen.thresholds[i:i + 4]) + ",")
        a("};")
        a("")
    if gen.splines:
        a(f"inline constexpr WgSpline kWg{prefix}Splines[{len(gen.splines)}] = {{")
        for spline in gen.splines:
            if spline["constant"] is None:
                a(f"    {{{spline['coordinate_index']}, {spline['point_offset']}, "
                  f"{spline['point_count']}, 0.0f, false}},")
            else:
                a(f"    {{0, {spline['point_offset']}, {spline['point_count']}, "
                  f"{hex_float(spline['constant'])}, true}},")
        a("};")
        a("")
        a(f"inline constexpr WgSplinePoint kWg{prefix}SplinePoints"
          f"[{len(gen.spline_points)}] = {{")
        for point in gen.spline_points:
            a(f"    {{{hex_float(point['location'])}, {hex_float(point['derivative'])}, "
              f"{hex_float(point['constant'])}, {point['spline']}}},")
        a("};")
        a("")
    if gen.blended:
        a(f"inline constexpr WgBlendedNoiseParams kWg{prefix}BlendedNoise"
          f"[{len(gen.blended)}] = {{")
        for params in gen.blended:
            a("    {" + ", ".join(hex_double(params[k]) for k in
                                 ("xz_scale", "y_scale", "xz_factor", "y_factor",
                                  "smear_scale_multiplier")) + "},")
        a("};")
        a("")
    a(f"inline constexpr uint32_t kWg{prefix}Router[{len(ROUTER_FIELDS)}] = {{")
    a("    " + ", ".join(str(gen.router[field]) for field in ROUTER_FIELDS) + ",")
    a("};")
    a("")
    a(f"inline constexpr WgNamedDf kWg{prefix}Named[{len(gen.named)}] = {{")
    for name in sorted(gen.named):
        a(f'    {{"{name}", {gen.named[name]}}},')
    a("};")
    a("")
    a(f"inline constexpr uint32_t kWg{prefix}NoiseDensityRoot = {gen.noise_density};")
    a(f"inline constexpr int32_t kWg{prefix}MinY = {noise['min_y']};")
    a(f"inline constexpr int32_t kWg{prefix}Height = {noise['height']};")
    a(f"inline constexpr int32_t kWg{prefix}CellWidth = {4 * noise['size_horizontal']};")
    a(f"inline constexpr int32_t kWg{prefix}CellHeight = {4 * noise['size_vertical']};")
    a(f"inline constexpr int32_t kWg{prefix}SeaLevel = {s['sea_level']};")
    a(f"inline constexpr bool kWg{prefix}AquifersEnabled = "
      f"{'true' if s['aquifers_enabled'] else 'false'};")
    a(f"inline constexpr bool kWg{prefix}OreVeinsEnabled = "
      f"{'true' if s['ore_veins_enabled'] else 'false'};")
    a(f"inline constexpr bool kWg{prefix}LegacyRandomSource = "
      f"{'true' if s.get('legacy_random_source', False) else 'false'};")
    a(f"inline constexpr const char* kWg{prefix}DefaultBlock = "
      f"\"{s['default_block']['Name']}\";")
    a(f"inline constexpr const char* kWg{prefix}DefaultFluid = "
      f"\"{s['default_fluid']['Name']}\";")
    a(f"inline constexpr int32_t kWg{prefix}DefaultFluidLevel = "
      f"{s['default_fluid'].get('Properties', {}).get('level', '0')};")
    a("")
    a("}  // namespace mc::world")
    a("// clang-format on")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("worldgen", type=pathlib.Path, help="<version>/data/minecraft/worldgen")
    parser.add_argument("out", type=pathlib.Path, help="output header")
    parser.add_argument("--dimension", choices=("overworld", "nether", "end"),
                        default="overworld", help="noise_settings/<dimension>.json")
    args = parser.parse_args()

    settings_path = args.worldgen / f"noise_settings/{args.dimension}.json"
    if not settings_path.is_file():
        print(f"no {settings_path}", file=sys.stderr)
        return 1

    generator = Generator(args.worldgen, args.dimension)
    generator.build()
    if args.dimension == "overworld":
        text = emit(generator)
    else:
        text = emit_dimension(generator, args.dimension)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text)
    print(f"wrote {args.out} ({args.dimension}, {len(generator.nodes)} nodes, "
          f"{len(generator.named)} named density functions, "
          f"{len(generator.noise_order)} noise parameter sets, "
          f"{len(generator.splines)} splines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
