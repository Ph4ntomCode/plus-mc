#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Dumps seed-exact world-generation ground truth out of the vanilla server jar.

`docs/PLAN.md` §11 asks Phase 4 for "a world generator that reads vanilla's own
JSON (density functions etc.)" and the standing bar for every mechanic in this
repo is the same: the game itself is the judge. This tool produces that judge
for terrain. It compiles a small Java probe against the 26.2 server jar, loads
the vanilla data pack's worldgen registries **through the game's own codecs**
(so the JSON under `data/vanilla/<version>/data/minecraft/worldgen/` is read the
way the server reads it), and writes four artifacts:

* `primitives.tsv` — the random and noise primitives the generator is built
  from, so a divergence can be localized to `XoroshiroRandomSource`, a
  positional factory, `NormalNoise`/`PerlinNoise`, or `BlendedNoise` long
  before it shows up as a wrong block:
  - `random` rows: the first longs/ints(bound)/doubles/floats/booleans of
    `XoroshiroRandomSource(seed)` for several seeds;
  - `positional` rows: `forkPositional()`, `fromHashOf(name)`, `fromSeed(s)`
    and `at(x,y,z)` sample values;
  - `noise` rows: each `minecraft:*` normal-noise parameter set's value at
    sample points, instantiated exactly as `RandomState` does
    (`Noises.instantiate`: `NormalNoise.create(factory.fromHashOf(key), params)`);
  - `blended` rows: `BlendedNoise` values for the overworld's own
    `base_3d_noise` parameters, wired to the seed's `minecraft:terrain`
    factory as `RandomState` wires it.
* `router.tsv` — `NoiseRouter` entries (temperature, vegetation, continents,
  erosion, depth, ridges, barrier, fluid-level noises, lava, preliminary
  surface, final density) at a grid of points.
* `named.tsv` — every `density_function` in the registry, by name, at a grid of
  points, each one wired the way `RandomState`'s `NoiseWiringHelper` wires the
  router (a registry function on its own is *unwired*: its `NoiseHolder`s carry
  no `NormalNoise` and `NoiseHolder.getValue` answers 0.0). When one router
  entry disagrees, this is what says which subtree does.
* `biome_parameters.tsv` — the overworld biome source's parameter list, in the
  preset's own order: the seven quantized `Climate.Parameter` pairs per point
  and the biome each one names. This is the search space the biome stage
  selects from.
* `biomes_seed<S>_c<X>_<Z>.tsv` — the biome *fill* of one chunk, cell for cell:
  for every quart cell of every section, the six quantized climate coordinates
  (`Climate.Sampler.sample` -> `Climate.TargetPoint`), the biome the game's own
  `MultiNoiseBiomeSource.getNoiseBiome` answers, and the same query brute-forced
  over the parameter list (`Climate.ParameterList.findValueBruteForce`).
  `biome_meta.tsv` counts the rows and the rows where the two disagree — the
  RTree index has a history-dependent tie break, so this is where the fixture
  says whether the reproducible linear scan happens to match it.
* `interpolated_seed<S>_c<X>_<Z>.tsv` — `getInterpolatedNoiseValue` at a
  grid of points, i.e. the exact density the fill loop samples.
* `noise_seed<S>_c<X>_<Z>.bin.gz` — the **noise-stage chunk**: for every one
  of the 16x16 columns, the full 384-cell block column `getBaseColumn`
  returns, one byte per cell: 0 air, 1 default block (stone), 2 default fluid
  (water), 3 lava, then the ore-vein states (granite, copper ore, raw copper
  block, tuff, deepslate iron ore, raw iron block) — `noise_meta.tsv` carries
  the exact code-to-state mapping. This is `ChunkGenerator.fillFromNoise`'s
  output before surface rules, carvers and features — the K1 target.

All floating-point values are printed as `Double.toHexString`, so the C++ side
compares bit patterns rather than decimal prefixes.

Usage:
    gen_worldgen_dump.py --jar build/_dl/boot/versions/26.2/server-26.2.jar \\
        --libraries build/_dl/boot/libraries \\
        --out tests/fixtures/worldgen --seed 42 --chunk 0 0 --chunk -1 -1

Requires a JDK (javac/java) on PATH, like the other probes in this directory.
"""

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

_PROBE = r"""
import java.io.FileWriter;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.zip.GZIPOutputStream;
import net.minecraft.SharedConstants;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderGetter;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.Registry;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.RegistryDataLoader;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.RegistryLayer;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.server.packs.resources.ResourceManager;
import net.minecraft.tags.TagLoader;
import net.minecraft.util.Util;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.NoiseColumn;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterList;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterLists;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ChunkGenerator;
import net.minecraft.world.level.dimension.LevelStem;
import net.minecraft.world.level.levelgen.DensityFunction;
import net.minecraft.world.level.levelgen.LegacyRandomSource;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.synth.PerlinSimplexNoise;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.Noises;
import net.minecraft.world.level.levelgen.NoiseRouter;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.PositionalRandomFactory;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.WorldDimensions;
import net.minecraft.world.level.levelgen.XoroshiroRandomSource;
import net.minecraft.world.level.levelgen.presets.WorldPresets;
import net.minecraft.world.level.levelgen.synth.BlendedNoise;
import net.minecraft.world.level.levelgen.synth.NormalNoise;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.chunk.CarvingMask;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.PalettedContainerFactory;
import net.minecraft.world.level.chunk.ProtoChunk;
import net.minecraft.world.level.chunk.UpgradeData;
import net.minecraft.world.level.levelgen.Aquifer;
import net.minecraft.world.level.levelgen.Beardifier;
import net.minecraft.world.level.levelgen.NoiseChunk;
import net.minecraft.world.level.levelgen.blending.Blender;
import net.minecraft.world.level.levelgen.WorldGenerationContext;
import net.minecraft.world.level.levelgen.carver.CarvingContext;
import net.minecraft.world.level.levelgen.carver.ConfiguredWorldCarver;
import net.minecraft.core.QuartPos;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.function.Function;
import java.util.stream.Collectors;

public class DumpWorldgen {
  static Path outDir;
  static Throwable failure;

  static PrintWriter open(String name) throws Exception {
    return new PrintWriter(new FileWriter(outDir.resolve(name).toFile()));
  }

  static String hex(double value) {
    return Double.toHexString(value);
  }

  static RegistryAccess.Frozen loadWorldgen() throws Exception {
    ResourceManager resources = new MultiPackResourceManager(PackType.SERVER_DATA,
        List.of(ServerPacksSource.createVanillaPackSource()));
    var layers = RegistryLayer.createRegistryAccess();
    List<Registry.PendingTags<?>> staticTags =
        TagLoader.loadTagsForExistingRegistries(resources, layers.getLayer(RegistryLayer.STATIC));
    RegistryAccess.Frozen worldgenContext = layers.getAccessForLoading(RegistryLayer.WORLDGEN);
    List<HolderLookup.RegistryLookup<?>> lookups =
        TagLoader.buildUpdatedLookups(worldgenContext, staticTags);
    RegistryAccess.Frozen loaded = RegistryDataLoader
        .load(resources, lookups, RegistryDataLoader.WORLDGEN_REGISTRIES, Util.backgroundExecutor())
        .join();
    // `buildUpdatedLookups` only makes the tags visible to the *codecs*; the
    // `HolderSet`s they decode from `#tag` stay unbound until the pending tags
    // are committed, which `RegistryDataLoader` does for the registries it owns
    // but not for the static layer's (`minecraft:block` among them). The game
    // does this in `ReloadableServerResources.loadResources`; without it
    // `CarverConfiguration.replaceable` is a `HolderSet.Named` over an unbound
    // `#minecraft:overworld_carver_replaceables`, so `WorldCarver.canReplaceBlock`
    // never matches and the carvers carve nothing at all.
    staticTags.forEach(Registry.PendingTags::apply);
    return loaded;
  }

  static void dumpPrimitives(RegistryAccess.Frozen regs, long seed) throws Exception {
    try (PrintWriter w = open("primitives.tsv")) {
      w.println("# kind\tkey\tx\ty\tz\tvalue");
      long[] seeds = new long[] {0L, 1L, 42L, -1L, 123456789L, -987654321L};
      for (long s : seeds) {
        XoroshiroRandomSource r = new XoroshiroRandomSource(s);
        StringBuilder longs = new StringBuilder();
        for (int i = 0; i < 8; i++) {
          longs.append(i == 0 ? "" : ",").append(r.nextLong());
        }
        w.println("random\tlongs\t" + s + "\t0\t0\t" + longs);
        r = new XoroshiroRandomSource(s);
        StringBuilder ints = new StringBuilder();
        for (int i = 0; i < 6; i++) {
          ints.append(i == 0 ? "" : ",").append(r.nextInt(1000));
        }
        w.println("random\tints\t" + s + "\t0\t0\t" + ints);
        r = new XoroshiroRandomSource(s);
        StringBuilder doubles = new StringBuilder();
        for (int i = 0; i < 5; i++) {
          doubles.append(i == 0 ? "" : ",").append(hex(r.nextDouble()));
        }
        w.println("random\tdoubles\t" + s + "\t0\t0\t" + doubles);
        r = new XoroshiroRandomSource(s);
        StringBuilder floats = new StringBuilder();
        for (int i = 0; i < 5; i++) {
          floats.append(i == 0 ? "" : ",")
              .append(Float.toHexString(r.nextFloat()));
        }
        w.println("random\tfloats\t" + s + "\t0\t0\t" + floats);
        r = new XoroshiroRandomSource(s);
        StringBuilder bools = new StringBuilder();
        for (int i = 0; i < 8; i++) {
          bools.append(i == 0 ? "" : ",").append(r.nextBoolean() ? 1 : 0);
        }
        w.println("random\tbools\t" + s + "\t0\t0\t" + bools);
      }

      // The worldgen positional stack, exactly as RandomState builds it.
      PositionalRandomFactory root = new XoroshiroRandomSource(seed).forkPositional();
      PositionalRandomFactory aquifer = root.fromHashOf(Identifier.withDefaultNamespace("aquifer")).forkPositional();
      PositionalRandomFactory ore = root.fromHashOf(Identifier.withDefaultNamespace("ore")).forkPositional();
      String[] names = new String[] {"minecraft:terrain", "minecraft:aquifer", "minecraft:ore",
          "minecraft:overworld/continents", "minecraft:cave_cheese", "minecraft:surface"};
      for (String name : names) {
        w.println("positional\tfromHashOf\t" + name + "\t" + seed + "\t0\t"
            + root.fromHashOf(name).nextLong());
      }
      w.println("positional\tfromSeed\t-\t" + seed + "\t0\t" + root.fromSeed(7L).nextLong());
      int[][] pts = new int[][] {{0, 0, 0}, {1, 2, 3}, {-17, 64, 412}, {1000, -30, -1000}, {3, 4, 5}};
      for (int[] p : pts) {
        w.println("positional\tat\t-\t" + p[0] + "\t" + p[1] + ";" + p[2] + "\t"
            + root.at(p[0], p[1], p[2]).nextLong());
        w.println("positional\taquifer.at\t-\t" + p[0] + "\t" + p[1] + ";" + p[2] + "\t"
            + aquifer.at(p[0], p[1], p[2]).nextLong());
        w.println("positional\tore.at\t-\t" + p[0] + "\t" + p[1] + ";" + p[2] + "\t"
            + ore.at(p[0], p[1], p[2]).nextLong());
      }

      // Every noise the probe can see, at every sample point. The key names are
      // the data pack's; the C++ table carries the same names.
      HolderLookup.RegistryLookup<NormalNoise.NoiseParameters> noises =
          regs.lookupOrThrow(Registries.NOISE);
      List<ResourceKey<NormalNoise.NoiseParameters>> noiseKeys = noises.listElementIds()
          .sorted(java.util.Comparator.comparing(k -> k.identifier().toString()))
          .collect(Collectors.toList());
      double[][] samples = {
        {0.0, 0.0, 0.0},
        {0.25, 1.5, -3.75},
        {8.0, 64.0, 8.0},
        {12.5, -30.25, 1000.125},
        {-1234.5, 200.75, -4321.25},
      };
      for (ResourceKey<NormalNoise.NoiseParameters> noiseKey : noiseKeys) {
        NormalNoise noise = NormalNoise.create(
            root.fromHashOf(noiseKey.identifier().toString()),
            noises.getOrThrow(noiseKey).value());
        for (double[] s : samples) {
          w.println("noise\t" + noiseKey.identifier() + "\t" + hex(s[0]) + "\t" + hex(s[1])
              + "\t" + hex(s[2]) + "\t" + hex(noise.getValue(s[0], s[1], s[2])));
        }
      }

      // The overworld's base_3d_noise, taken out of the loaded registry and
      // wired to the seed's terrain factory exactly as RandomState.wrapNew does.
      BlendedNoise unseeded = (BlendedNoise) regs
          .lookupOrThrow(Registries.DENSITY_FUNCTION)
          .getOrThrow(ResourceKey.create(Registries.DENSITY_FUNCTION,
              Identifier.withDefaultNamespace("overworld/base_3d_noise")))
          .value();
      BlendedNoise blended = unseeded.withNewRandom(root.fromHashOf("minecraft:terrain"));
      double[][] blendSamples = {
        {0.0, 0.0, 0.0},
        {1.0, 64.0, 2.0},
        {-31.5, 128.25, 47.75},
        {512.0, -32.0, -768.0},
        {4096.5, 300.5, 1024.25},
      };
      for (double[] s : blendSamples) {
        DensityFunction.FunctionContext ctx =
            new DensityFunction.SinglePointContext((int) Math.floor(s[0]), (int) Math.floor(s[1]),
                (int) Math.floor(s[2]));
        w.println("blended\tminecraft:overworld/base_3d_noise\t" + hex(s[0]) + "\t" + hex(s[1])
            + "\t" + hex(s[2]) + "\t" + hex(blended.compute(ctx)));
      }
    }
  }

  // The legacy simplex stack `Biome`'s static noises are built from, at fixed
  // seeds and octave sets. `PerlinSimplexNoise` seeds itself from a
  // `LegacyRandomSource`, so these values pin both the RNG and the simplex
  // implementation the surface system's temperature conditions read.
  static void dumpSurfaceNoise() throws Exception {
    PerlinSimplexNoise temperature = new PerlinSimplexNoise(
        new WorldgenRandom(new LegacyRandomSource(1234L)), List.of(0));
    PerlinSimplexNoise frozen = new PerlinSimplexNoise(
        new WorldgenRandom(new LegacyRandomSource(3456L)), List.of(-2, -1, 0));
    PerlinSimplexNoise info = new PerlinSimplexNoise(
        new WorldgenRandom(new LegacyRandomSource(2345L)), List.of(0));
    double[][] points = {
      {0.0, 0.0},
      {1.5, -2.25},
      {12.5, 8.0},
      {-1234.5, -4321.25},
      {250.125, 0.0},
      {37.7, -88.25},
    };
    try (PrintWriter w = open("surface_noise.tsv")) {
      w.println("# key\tx\tz\tvalue");
      for (double[] p : points) {
        w.println("temperature\t" + hex(p[0]) + "\t" + hex(p[1]) + "\t"
            + hex(temperature.getValue(p[0], p[1], false)));
        w.println("frozen\t" + hex(p[0]) + "\t" + hex(p[1]) + "\t"
            + hex(frozen.getValue(p[0], p[1], false)));
        w.println("info\t" + hex(p[0]) + "\t" + hex(p[1]) + "\t"
            + hex(info.getValue(p[0], p[1], false)));
      }
    }
  }

  static void dumpRouter(RegistryAccess.Frozen regs, RandomState rs, long seed, int[] ys)
      throws Exception {
    NoiseRouter router = rs.router();
    String[] names = {"barrier", "fluid_level_floodedness", "fluid_level_spread", "lava",
        "temperature", "vegetation", "continents", "erosion", "depth", "ridges",
        "preliminary_surface", "final_density"};
    DensityFunction[] fns = {router.barrierNoise(), router.fluidLevelFloodednessNoise(),
        router.fluidLevelSpreadNoise(), router.lavaNoise(), router.temperature(),
        router.vegetation(), router.continents(), router.erosion(), router.depth(),
        router.ridges(), router.preliminarySurfaceLevel(), router.finalDensity()};
    int[] xs = {0, 3, 8, 15, 31};
    int[] zs = {0, 4, 15, -3};
    try (PrintWriter w = open("router.tsv")) {
      w.println("# seed\tentry\tx\ty\tz\tvalue");
      for (int xi = 0; xi < xs.length; xi++) {
        for (int yi = 0; yi < ys.length; yi++) {
          for (int zi = 0; zi < zs.length; zi++) {
            DensityFunction.FunctionContext ctx =
                new DensityFunction.SinglePointContext(xs[xi], ys[yi], zs[zi]);
            for (int i = 0; i < fns.length; i++) {
              w.println(seed + "\t" + names[i] + "\t" + xs[xi] + "\t" + ys[yi] + "\t" + zs[zi]
                  + "\t" + hex(fns[i].compute(ctx)));
            }
          }
        }
      }
    }
  }

  static String biomeName(Holder<Biome> biome) {
    return biome.unwrapKey().orElseThrow().identifier().toString();
  }

  // The biome source's search space, exactly as the `minecraft:overworld`
  // multi-noise preset builds it (the registry value the overworld generator's
  // biome source resolves). The C++ table is generated from this file.
  static Climate.ParameterList<Holder<Biome>> dumpBiomeParameters(
      RegistryAccess.Frozen regs,
      ResourceKey<MultiNoiseBiomeSourceParameterList> preset) throws Exception {
    Climate.ParameterList<Holder<Biome>> parameters =
        regs.lookupOrThrow(Registries.MULTI_NOISE_BIOME_SOURCE_PARAMETER_LIST)
            .getOrThrow(preset)
            .value()
            .parameters();
    List<com.mojang.datafixers.util.Pair<Climate.ParameterPoint, Holder<Biome>>> values =
        parameters.values();
    try (PrintWriter w = open("biome_parameters.tsv")) {
      w.println("# index\ttemperature_min\ttemperature_max\thumidity_min\thumidity_max"
          + "\tcontinentalness_min\tcontinentalness_max\terosion_min\terosion_max"
          + "\tdepth_min\tdepth_max\tweirdness_min\tweirdness_max\toffset\tbiome");
      for (int i = 0; i < values.size(); i++) {
        Climate.ParameterPoint p = values.get(i).getFirst();
        Holder<Biome> biome = values.get(i).getSecond();
        w.println(i + "\t" + p.temperature().min() + "\t" + p.temperature().max()
            + "\t" + p.humidity().min() + "\t" + p.humidity().max()
            + "\t" + p.continentalness().min() + "\t" + p.continentalness().max()
            + "\t" + p.erosion().min() + "\t" + p.erosion().max()
            + "\t" + p.depth().min() + "\t" + p.depth().max()
            + "\t" + p.weirdness().min() + "\t" + p.weirdness().max()
            + "\t" + p.offset() + "\t" + biomeName(biome));
      }
    }
    return parameters;
  }

  static void dumpInterpolated(NoiseBasedChunkGenerator generator, RandomState rs, long seed,
      int chunkX, int chunkZ, int[] ys) throws Exception {
    int[] xs = {0, 1, 3, 4, 7, 8, 12, 15};
    int[] zs = {0, 1, 3, 4, 7, 8, 12, 15};
    try (PrintWriter w = open("interpolated_seed" + seed + "_c" + chunkX + "_" + chunkZ + ".tsv")) {
      w.println("# seed\tchunkX\tchunkZ\tx\ty\tz\tvalue");
      for (int xi : xs) {
        for (int zi : zs) {
          for (int yi : ys) {
            int x = chunkX * 16 + xi;
            int z = chunkZ * 16 + zi;
            double value = generator.getInterpolatedNoiseValue(rs,
                new DensityFunction.SinglePointContext(x, yi, z));
            w.println(seed + "\t" + chunkX + "\t" + chunkZ + "\t" + x + "\t" + yi + "\t" + z
                + "\t" + hex(value));
          }
        }
      }
    }
  }

  // Every density function in the registry, by name, at a grid of points, so a
  // disagreement can be localized to a subtree rather than guessed at from
  // `final_density`. The router rows above already cover the ones the overworld
  // uses; this covers the whole registry.
  //
  // A registry function is *unwired*: `NoiseHolder` still holds a null
  // `NormalNoise` and `NoiseHolder.getValue` answers 0.0 for it, so a bare
  // `compute` would read zeros. `RandomState` wires a copy for the router
  // (`NoiseWiringHelper`); this repeats that wiring, because the fixture has to
  // pin the path the game actually takes.
  static void dumpNamed(RegistryAccess.Frozen regs, RandomState rs, NoiseGeneratorSettings settings,
      long seed, int[] ys) throws Exception {
    var lookup = regs.lookupOrThrow(Registries.DENSITY_FUNCTION);
    java.util.List<ResourceKey<DensityFunction>> keys = lookup.listElementIds()
        .sorted(java.util.Comparator.comparing(k -> k.identifier().toString()))
        .collect(Collectors.toList());
    final PositionalRandomFactory root =
        settings.getRandomSource().newInstance(seed).forkPositional();
    final java.util.Map<DensityFunction, DensityFunction> wrapped = new java.util.HashMap<>();
    DensityFunction.Visitor wiring = new DensityFunction.Visitor() {
      DensityFunction wrapNew(DensityFunction function) {
        if (function instanceof BlendedNoise noise) {
          return noise.withNewRandom(
              root.fromHashOf(Identifier.withDefaultNamespace("terrain")));
        }
        return function;
      }

      @Override
      public DensityFunction apply(DensityFunction function) {
        return wrapped.computeIfAbsent(function, this::wrapNew);
      }

      @Override
      public DensityFunction.NoiseHolder visitNoise(DensityFunction.NoiseHolder noise) {
        Holder<NormalNoise.NoiseParameters> noiseData = noise.noiseData();
        // The two legacy nether sets are not instantiated from the seed's
        // positional factory but from a `LegacyRandomSource`, as in
        // `RandomState.NoiseWiringHelper.visitNoise`.
        if (noiseData.is(Noises.TEMPERATURE_NETHER)) {
          return new DensityFunction.NoiseHolder(noiseData,
              NormalNoise.createLegacyNetherBiome(new LegacyRandomSource(seed), noiseData.value()));
        }
        if (noiseData.is(Noises.VEGETATION_NETHER)) {
          return new DensityFunction.NoiseHolder(noiseData,
              NormalNoise.createLegacyNetherBiome(
                  new LegacyRandomSource(seed + 1L), noiseData.value()));
        }
        return new DensityFunction.NoiseHolder(noiseData,
            rs.getOrCreateNoise(noiseData.unwrapKey().orElseThrow()));
      }
    };
    int[] xs = {0, 8, 15};
    int[] zs = {0, 4, 15};
    try (PrintWriter w = open("named.tsv")) {
      w.println("# name\tx\ty\tz\tvalue");
      for (ResourceKey<DensityFunction> key : keys) {
        DensityFunction function = lookup.getOrThrow(key).value().mapAll(wiring);
        for (int x : xs) {
          for (int y : ys) {
            for (int z : zs) {
              DensityFunction.FunctionContext ctx =
                  new DensityFunction.SinglePointContext(x, y, z);
              w.println(key.identifier() + "\t" + x + "\t" + y + "\t" + z + "\t"
                  + hex(function.compute(ctx)));
            }
          }
        }
      }
    }
  }

  // A canonical `minecraft:name[a=b,c=d]` spelling (properties sorted), so the
  // C++ side can compare state strings without depending on Java's map order.
  @SuppressWarnings({"rawtypes", "unchecked"})
  static String stateName(BlockState state) {
    List<String> parts = new ArrayList<>();
    for (Property p : state.getProperties()) {
      parts.add(p.getName() + "=" + p.getName(state.getValue(p)));
    }
    Collections.sort(parts);
    return BuiltInRegistries.BLOCK.getKey(state.getBlock())
        + (parts.isEmpty() ? "" : "[" + String.join(",", parts) + "]");
  }

  static void dumpNoiseChunk(ChunkGenerator generator, RandomState rs, long seed, int chunkX,
      int chunkZ) throws Exception {
    NoiseGeneratorSettings settings = ((NoiseBasedChunkGenerator) generator).generatorSettings().value();
    int minY = settings.noiseSettings().minY();
    int height = settings.noiseSettings().height();
    LevelHeightAccessor accessor = LevelHeightAccessor.create(minY, height);
    byte[] cells = new byte[16 * 16 * height];
    int index = 0;
    // The noise stage can only produce the default block/fluid, the lava floor
    // and the ore-vein states; a ninth state would mean this list is stale.
    BlockState[] states = {Blocks.AIR.defaultBlockState(), settings.defaultBlock(),
        settings.defaultFluid(), Blocks.LAVA.defaultBlockState(), Blocks.GRANITE.defaultBlockState(),
        Blocks.COPPER_ORE.defaultBlockState(), Blocks.RAW_COPPER_BLOCK.defaultBlockState(),
        Blocks.TUFF.defaultBlockState(), Blocks.DEEPSLATE_IRON_ORE.defaultBlockState(),
        Blocks.RAW_IRON_BLOCK.defaultBlockState()};
    for (int x = 0; x < 16; x++) {
      for (int z = 0; z < 16; z++) {
        NoiseColumn column = generator.getBaseColumn(chunkX * 16 + x, chunkZ * 16 + z, accessor, rs);
        for (int y = 0; y < height; y++) {
          BlockState state = column.getBlock(minY + y);
          if (state == null) {
            throw new IllegalStateException("null column cell at " + (minY + y));
          }
          int code = -1;
          for (int i = 0; i < states.length; i++) {
            if (state == states[i]) {
              code = i;
              break;
            }
          }
          if (code < 0) {
            throw new IllegalStateException("unexpected noise-stage block " + state + " at "
                + (chunkX * 16 + x) + "," + (minY + y) + "," + (chunkZ * 16 + z));
          }
          cells[index++] = (byte) code;
        }
      }
    }
    try (var out = new GZIPOutputStream(Files.newOutputStream(
        outDir.resolve("noise_seed" + seed + "_c" + chunkX + "_" + chunkZ + ".bin.gz")))) {
      out.write(cells);
    }
    StringBuilder codeMap = new StringBuilder();
    for (int i = 0; i < states.length; i++) {
      codeMap.append(i == 0 ? "" : ",").append(i).append("=").append(states[i]);
    }
    try (PrintWriter w = new PrintWriter(new FileWriter(
        outDir.resolve("noise_meta.tsv").toFile(), true))) {
      w.println(seed + "\t" + chunkX + "\t" + chunkZ + "\t" + minY + "\t" + height + "\t"
          + (16 * 16 * height) + "\t" + stateName(settings.defaultBlock()) + "\t"
          + stateName(settings.defaultFluid()) + "\t" + codeMap);
    }
  }

  // The chunk after `buildSurface` and before carvers/features: the surface
  // rules are applied by the game's own `SurfaceSystem` and the resulting
  // block states are dumped cell for cell, in the noise dump's own layout.
  // The state-code map is first-appearance order and is written to
  // `surface_meta.tsv` with canonical state names, one row per chunk.
  static void dumpSurface(NoiseBasedChunkGenerator generator, RandomState rs, long seed,
      RegistryAccess.Frozen regs, int chunkX, int chunkZ) throws Exception {
    NoiseGeneratorSettings settings = generator.generatorSettings().value();
    int minY = settings.noiseSettings().minY();
    int height = settings.noiseSettings().height();
    LevelHeightAccessor accessor = LevelHeightAccessor.create(minY, height);
    ProtoChunk proto = new ProtoChunk(new ChunkPos(chunkX, chunkZ), UpgradeData.EMPTY, accessor,
        PalettedContainerFactory.create(regs), null);
    // Pre-create the NoiseChunk with an empty Beardifier so the structure
    // manager can stay null (no structures exist in this probe).
    Aquifer.FluidStatus lavaStatus = new Aquifer.FluidStatus(-54, Blocks.LAVA.defaultBlockState());
    Aquifer.FluidStatus seaStatus =
        new Aquifer.FluidStatus(settings.seaLevel(), settings.defaultFluid());
    Aquifer.FluidPicker fluidPicker = (x, y, z) ->
        y < Math.min(-54, settings.seaLevel()) ? lavaStatus : seaStatus;
    proto.getOrCreateNoiseChunk(c -> NoiseChunk.forChunk(c, rs, Beardifier.EMPTY, settings,
        fluidPicker, Blender.empty()));
    generator.fillFromNoise(Blender.empty(), rs, null, proto).join();
    BiomeManager biomeManager = new BiomeManager(
        (qx, qy, qz) -> generator.getBiomeSource().getNoiseBiome(qx, qy, qz, rs.sampler()),
        BiomeManager.obfuscateSeed(seed));
    generator.buildSurface(proto, new WorldGenerationContext(generator, accessor), rs, null,
        biomeManager, Blender.empty(), null);

    Map<BlockState, Integer> codes = new LinkedHashMap<>();
    byte[] cells = new byte[16 * 16 * height];
    int index = 0;
    BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
    for (int x = 0; x < 16; x++) {
      for (int z = 0; z < 16; z++) {
        for (int y = 0; y < height; y++) {
          BlockState state = proto.getBlockState(pos.set(chunkX * 16 + x, minY + y, chunkZ * 16 + z));
          Integer code = codes.get(state);
          if (code == null) {
            if (codes.size() > 255) {
              throw new IllegalStateException("more than 256 surface states");
            }
            code = codes.size();
            codes.put(state, code);
          }
          cells[index++] = (byte) (int) code;
        }
      }
    }
    try (var out = new GZIPOutputStream(Files.newOutputStream(
        outDir.resolve("surface_seed" + seed + "_c" + chunkX + "_" + chunkZ + ".bin.gz")))) {
      out.write(cells);
    }
    StringBuilder codeMap = new StringBuilder();
    for (Map.Entry<BlockState, Integer> entry : codes.entrySet()) {
      if (codeMap.length() > 0) {
        codeMap.append(",");
      }
      codeMap.append(entry.getValue()).append("=").append(stateName(entry.getKey()));
    }
    try (PrintWriter w = new PrintWriter(new FileWriter(
        outDir.resolve("surface_meta.tsv").toFile(), true))) {
      w.println(seed + "\t" + chunkX + "\t" + chunkZ + "\t" + minY + "\t" + height + "\t"
          + (16 * 16 * height) + "\t" + stateName(settings.defaultBlock()) + "\t"
          + settings.seaLevel() + "\t" + codeMap);
    }
  }

  // The chunk after `applyCarvers`: buildSurface, then the game's own carver
  // classes driven by the same 17 x 17 source-chunk loop
  // `NoiseBasedChunkGenerator.applyCarvers` runs. The probe cannot call that
  // method itself — it needs a `WorldGenRegion`, which needs a `ServerLevel` —
  // so the loop is transcribed here; every carve below it is the server's own
  // `ConfiguredWorldCarver.carve`, including its `topMaterial` re-top, which is
  // why this dump is the judge for the C++ stage's missing half as well.
  static void dumpCarved(NoiseBasedChunkGenerator generator, RandomState rs, long seed,
      RegistryAccess.Frozen regs, int chunkX, int chunkZ) throws Exception {
    NoiseGeneratorSettings settings = generator.generatorSettings().value();
    int minY = settings.noiseSettings().minY();
    int height = settings.noiseSettings().height();
    LevelHeightAccessor accessor = LevelHeightAccessor.create(minY, height);
    ProtoChunk proto = new ProtoChunk(new ChunkPos(chunkX, chunkZ), UpgradeData.EMPTY, accessor,
        PalettedContainerFactory.create(regs), null);
    Aquifer.FluidStatus lavaStatus = new Aquifer.FluidStatus(-54, Blocks.LAVA.defaultBlockState());
    Aquifer.FluidStatus seaStatus =
        new Aquifer.FluidStatus(settings.seaLevel(), settings.defaultFluid());
    Aquifer.FluidPicker fluidPicker = (x, y, z) ->
        y < Math.min(-54, settings.seaLevel()) ? lavaStatus : seaStatus;
    NoiseChunk noiseChunk = proto.getOrCreateNoiseChunk(c -> NoiseChunk.forChunk(c, rs,
        Beardifier.EMPTY, settings, fluidPicker, Blender.empty()));
    generator.fillFromNoise(Blender.empty(), rs, null, proto).join();
    BiomeManager biomeManager = new BiomeManager(
        (qx, qy, qz) -> generator.getBiomeSource().getNoiseBiome(qx, qy, qz, rs.sampler()),
        BiomeManager.obfuscateSeed(seed));
    generator.buildSurface(proto, new WorldGenerationContext(generator, accessor), rs, null,
        biomeManager, Blender.empty(), null);

    CarvingContext context =
        new CarvingContext(generator, regs, accessor, noiseChunk, rs, settings.surfaceRule());
    CarvingMask mask = proto.getOrCreateCarvingMask();
    Aquifer aquifer = noiseChunk.aquifer();
    Function<BlockPos, Holder<Biome>> biomeGetter = pos ->
        generator.getBiomeSource().getNoiseBiome(QuartPos.fromBlock(pos.getX()),
            QuartPos.fromBlock(pos.getY()), QuartPos.fromBlock(pos.getZ()), rs.sampler());
    WorldgenRandom random = new WorldgenRandom(new LegacyRandomSource(0L));
    for (int dx = -8; dx <= 8; ++dx) {
      for (int dz = -8; dz <= 8; ++dz) {
        ChunkPos sourcePos = new ChunkPos(chunkX + dx, chunkZ + dz);
        Holder<Biome> sourceBiome = generator.getBiomeSource().getNoiseBiome(
            QuartPos.fromBlock(sourcePos.getMinBlockX()), 0,
            QuartPos.fromBlock(sourcePos.getMinBlockZ()), rs.sampler());
        int index = 0;
        for (Holder<ConfiguredWorldCarver<?>> holder
            : sourceBiome.value().getGenerationSettings().getCarvers()) {
          ConfiguredWorldCarver<?> carver = holder.value();
          random.setLargeFeatureSeed(seed + (long) index, sourcePos.x(), sourcePos.z());
          if (carver.isStartChunk(random)) {
            carver.carve(context, proto, biomeGetter, random, aquifer, sourcePos, mask);
          }
          ++index;
        }
      }
    }

    Map<BlockState, Integer> codes = new LinkedHashMap<>();
    byte[] cells = new byte[16 * 16 * height];
    int index = 0;
    BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
    for (int x = 0; x < 16; x++) {
      for (int z = 0; z < 16; z++) {
        for (int y = 0; y < height; y++) {
          BlockState state = proto.getBlockState(pos.set(chunkX * 16 + x, minY + y, chunkZ * 16 + z));
          Integer code = codes.get(state);
          if (code == null) {
            if (codes.size() > 255) {
              throw new IllegalStateException("more than 256 carved states");
            }
            code = codes.size();
            codes.put(state, code);
          }
          cells[index++] = (byte) (int) code;
        }
      }
    }
    try (var out = new GZIPOutputStream(Files.newOutputStream(
        outDir.resolve("carved_seed" + seed + "_c" + chunkX + "_" + chunkZ + ".bin.gz")))) {
      out.write(cells);
    }
    StringBuilder codeMap = new StringBuilder();
    for (Map.Entry<BlockState, Integer> entry : codes.entrySet()) {
      if (codeMap.length() > 0) {
        codeMap.append(",");
      }
      codeMap.append(entry.getValue()).append("=").append(stateName(entry.getKey()));
    }
    try (PrintWriter w = new PrintWriter(new FileWriter(
        outDir.resolve("carved_meta.tsv").toFile(), true))) {
      w.println(seed + "\t" + chunkX + "\t" + chunkZ + "\t" + minY + "\t" + height + "\t"
          + (16 * 16 * height) + "\t" + stateName(settings.defaultBlock()) + "\t"
          + settings.seaLevel() + "\t" + codeMap);
    }
  }

  // The biome fill of one chunk, in `ChunkAccess.fillBiomesFromNoise`'s own
  // query order: sections bottom-up, and within a section x then y then z.
  // The sampler is built from the bare router entries, which is what
  // `NoiseChunk.cachedClimateSampler` wraps without changing any value at a
  // `SinglePointContext`; every row also carries the brute-force answer so the
  // fixture can say whether the RTree's tie break ever differs from a linear
  // scan (it is free to, and vanilla's own answer is then history-dependent).
  static void dumpBiomes(NoiseBasedChunkGenerator generator, RandomState rs, long seed,
      Climate.ParameterList<Holder<Biome>> parameters, int chunkX, int chunkZ)
      throws Exception {
    NoiseGeneratorSettings settings = generator.generatorSettings().value();
    int minY = settings.noiseSettings().minY();
    int height = settings.noiseSettings().height();
    int minSectionY = Math.floorDiv(minY, 16);
    int maxSectionY = Math.floorDiv(minY + height, 16) - 1;
    NoiseRouter router = rs.router();
    Climate.Sampler sampler = new Climate.Sampler(router.temperature(), router.vegetation(),
        router.continents(), router.erosion(), router.depth(), router.ridges(), List.of());
    int cells = 0;
    int mismatches = 0;
    StringBuilder firstMismatch = new StringBuilder();
    try (PrintWriter w = open("biomes_seed" + seed + "_c" + chunkX + "_" + chunkZ + ".tsv")) {
      w.println("# seed\tchunkX\tchunkZ\tquartX\tquartY\tquartZ\ttemperature\thumidity"
          + "\tcontinentalness\terosion\tdepth\tweirdness\tbiome\tbruteForce");
      for (int sectionY = minSectionY; sectionY <= maxSectionY; sectionY++) {
        for (int x = 0; x < 4; x++) {
          for (int y = 0; y < 4; y++) {
            for (int z = 0; z < 4; z++) {
              int quartX = chunkX * 4 + x;
              int quartY = sectionY * 4 + y;
              int quartZ = chunkZ * 4 + z;
              Climate.TargetPoint target = sampler.sample(quartX, quartY, quartZ);
              String biome = biomeName(generator.getBiomeSource().getNoiseBiome(
                  quartX, quartY, quartZ, sampler));
              // The End's `TheEndBiomeSource` is not a parameter list: there is
              // no brute-force answer to compare against, so the column repeats
              // the source's own answer.
              String brute = parameters == null ? biome
                  : biomeName(parameters.findValueBruteForce(target));
              if (!biome.equals(brute)) {
                mismatches++;
                if (firstMismatch.length() == 0) {
                  firstMismatch.append(quartX).append(",").append(quartY).append(",")
                      .append(quartZ).append(" rtree=").append(biome).append(" brute=")
                      .append(brute);
                }
              }
              w.println(seed + "\t" + chunkX + "\t" + chunkZ + "\t" + quartX + "\t" + quartY
                  + "\t" + quartZ + "\t" + target.temperature() + "\t" + target.humidity()
                  + "\t" + target.continentalness() + "\t" + target.erosion() + "\t"
                  + target.depth() + "\t" + target.weirdness() + "\t" + biome + "\t" + brute);
              cells++;
            }
          }
        }
      }
    }
    try (PrintWriter w = new PrintWriter(new FileWriter(
        outDir.resolve("biome_meta.tsv").toFile(), true))) {
      w.println(seed + "\t" + chunkX + "\t" + chunkZ + "\t" + minY + "\t" + height + "\t"
          + cells + "\t" + mismatches + "\t" + (firstMismatch.length() == 0 ? "-" : firstMismatch));
    }
  }

  // The y rows each dimension's router/named/interpolated dumps sample. The
  // overworld grid is the original fixture's; a dimension with a shorter stack
  // gets an evenly spaced ladder inside its own bounds.
  static int[] sampleYs(int minY, int height, boolean overworld) {
    if (overworld) {
      return new int[] {-64, -63, -60, -56, -48, -40, -32, -24, -16, -8, 0, 8, 16, 24, 32, 40,
          48, 56, 63, 64, 80, 96, 128, 200, 300, 319};
    }
    java.util.TreeSet<Integer> rows = new java.util.TreeSet<>();
    for (int i = 0; i <= 16; ++i) {
      rows.add(minY + Math.min(height - 1, i * height / 16));
    }
    rows.add(32);
    rows.add(64);
    rows.add(127);
    // Cell boundaries: a 4-tall cell can cross zero between the coarse rows.
    for (int y = minY; y < minY + height; y += 4) {
      rows.add(y);
    }
    return rows.stream().filter(y -> y >= minY && y < minY + height)
        .mapToInt(Integer::intValue).toArray();
  }

  public static void main(String[] args) {
    String error = null;
    try {
      outDir = Path.of(args[0]);
      long seed = Long.parseLong(args[1]);
      // `--carved` means "only the post-carver dumps"; `--dimension` selects
      // the LevelStem. Both are stripped before the chunk pairs are parsed.
      boolean carvedOnly = false;
      String dimension = "overworld";
      java.util.List<int[]> chunks = new java.util.ArrayList<>();
      for (int i = 2; i < args.length; ++i) {
        if ("--carved".equals(args[i])) {
          carvedOnly = true;
        } else if ("--dimension".equals(args[i]) && i + 1 < args.length) {
          dimension = args[++i];
        } else {
          chunks.add(new int[] {Integer.parseInt(args[i]), Integer.parseInt(args[++i])});
        }
      }
      if (chunks.isEmpty()) {
        throw new IllegalArgumentException("no chunk given");
      }
      Files.createDirectories(outDir);
      Files.deleteIfExists(outDir.resolve("error.txt"));

      SharedConstants.tryDetectVersion();
      Bootstrap.bootStrap();
      RegistryAccess.Frozen regs = loadWorldgen();

      WorldDimensions dimensions = WorldPresets.createNormalWorldDimensions(regs);
      boolean isOverworld = "overworld".equals(dimension);
      boolean isNether = "nether".equals(dimension);
      ResourceKey<LevelStem> stemKey = isNether ? LevelStem.NETHER
          : "end".equals(dimension) ? LevelStem.END : LevelStem.OVERWORLD;
      LevelStem stem = dimensions.get(stemKey).orElseThrow();
      NoiseBasedChunkGenerator generator = (NoiseBasedChunkGenerator) stem.generator();
      NoiseGeneratorSettings settings = generator.generatorSettings().value();
      ResourceKey<NoiseGeneratorSettings> settingsKey = ResourceKey.create(
          Registries.NOISE_SETTINGS, Identifier.withDefaultNamespace(
              isNether ? "nether" : "end".equals(dimension) ? "end" : "overworld"));
      RandomState randomState = RandomState.create(regs, settingsKey, seed);
      int[] ys = sampleYs(settings.noiseSettings().minY(), settings.noiseSettings().height(),
          isOverworld);

      if (carvedOnly) {
        try (PrintWriter w = open("carved_meta.tsv")) {
          w.println("# seed\tchunkX\tchunkZ\tminY\theight\tcells\tdefaultBlock\tseaLevel"
              + "\tstateCodes");
        }
        for (int[] chunk : chunks) {
          dumpCarved(generator, randomState, seed, regs, chunk[0], chunk[1]);
        }
        System.out.println("ok");
        return;
      }

      // The primitives and the legacy simplex stack are dimension-independent;
      // each dimension's own directory carries only what the generators need.
      if (isOverworld) {
        dumpPrimitives(regs, seed);
        dumpSurfaceNoise();
      }
      dumpRouter(regs, randomState, seed, ys);
      dumpNamed(regs, randomState, settings, seed, ys);
      Climate.ParameterList<Holder<Biome>> parameters = null;
      if (isNether) {
        parameters = dumpBiomeParameters(regs, MultiNoiseBiomeSourceParameterLists.NETHER);
      } else if (isOverworld) {
        parameters = dumpBiomeParameters(regs, MultiNoiseBiomeSourceParameterLists.OVERWORLD);
      } else {
        try (PrintWriter w = open("biome_parameters.tsv")) {
          w.println("# The End's TheEndBiomeSource has no multi-noise parameter list.");
        }
      }
      try (PrintWriter w = open("biome_meta.tsv")) {
        w.println("# seed\tchunkX\tchunkZ\tminY\theight\tcells\trtree_brute_mismatches"
            + "\tfirst_mismatch");
      }
      try (PrintWriter w = open("noise_meta.tsv")) {
        w.println("# seed\tchunkX\tchunkZ\tminY\theight\tcells\tdefaultBlock\tdefaultFluid"
            + "\tstateCodes");
      }
      try (PrintWriter w = open("surface_meta.tsv")) {
        w.println("# seed\tchunkX\tchunkZ\tminY\theight\tcells\tdefaultBlock\tseaLevel"
            + "\tstateCodes");
      }
      try (PrintWriter w = open("carved_meta.tsv")) {
        w.println("# seed\tchunkX\tchunkZ\tminY\theight\tcells\tdefaultBlock\tseaLevel"
            + "\tstateCodes");
      }
      for (int[] chunk : chunks) {
        dumpInterpolated(generator, randomState, seed, chunk[0], chunk[1], ys);
        dumpNoiseChunk(generator, randomState, seed, chunk[0], chunk[1]);
        dumpBiomes(generator, randomState, seed, parameters, chunk[0], chunk[1]);
        dumpSurface(generator, randomState, seed, regs, chunk[0], chunk[1]);
        dumpCarved(generator, randomState, seed, regs, chunk[0], chunk[1]);
      }
    } catch (Throwable t) {
      failure = t;
      error = t.toString();
      java.io.StringWriter sw = new java.io.StringWriter();
      t.printStackTrace(new PrintWriter(sw));
      try {
        Files.writeString(outDir.resolve("error.txt"), sw.toString());
      } catch (Exception ignored) {
        // fall through to the stderr print
      }
    }
    if (failure != null) {
      System.err.println(error);
      System.exit(1);
    }
    System.out.println("ok");
  }
}
"""


def library_jars(dirs: list[pathlib.Path]) -> list[pathlib.Path]:
    jars: list[pathlib.Path] = []
    for d in dirs:
        if not d.is_dir():
            print(f"warning: --libraries {d} is not a directory", file=sys.stderr)
            continue
        jars.extend(sorted(d.rglob("*.jar")))
    return jars


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jar", type=pathlib.Path, required=True, help="server jar (bundler)")
    parser.add_argument("--libraries", type=pathlib.Path, action="append", default=[],
                        help="directory of third-party library jars (repeatable)")
    parser.add_argument("--out", type=pathlib.Path, required=True, help="output directory")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--chunk", type=int, nargs=2, action="append", default=[],
                        metavar=("X", "Z"), help="chunk to dump (repeatable; default 0 0)")
    parser.add_argument("--stages", choices=("all", "carved"), default="all",
                        help="`carved` writes only the post-carver dumps into --out, "
                             "leaving the other fixtures alone")
    parser.add_argument("--dimension", choices=("overworld", "nether", "end"),
                        default="overworld",
                        help="which LevelStem to dump (default overworld). A dimension "
                             "other than the overworld is normally written into its "
                             "own --out directory")
    args = parser.parse_args()
    if not args.chunk:
        args.chunk = [[0, 0]]

    if not args.jar.is_file():
        print(f"server jar not found: {args.jar}", file=sys.stderr)
        return 1
    javac = shutil.which("javac")
    java = shutil.which("java")
    if not javac or not java:
        print("javac/java not found on PATH (a JDK is required)", file=sys.stderr)
        return 1

    classpath = [str(args.jar)] + [str(j) for j in library_jars(args.libraries)]
    cp = ":".join(classpath)

    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = pathlib.Path(tmp)
        src = tmpdir / "DumpWorldgen.java"
        src.write_text(_PROBE)
        compiled = subprocess.run(
            [javac, "-cp", cp, "-d", str(tmpdir), str(src)], capture_output=True, text=True)
        if compiled.returncode != 0:
            print("javac failed:\n" + compiled.stderr.strip()[:4000], file=sys.stderr)
            return 1

        command = [java, "-cp", f"{tmpdir}:{cp}", "DumpWorldgen", str(args.out), str(args.seed)]
        for chunk in args.chunk:
            command += [str(chunk[0]), str(chunk[1])]
        if args.stages == "carved":
            command += ["--carved"]
        command += ["--dimension", args.dimension]
        ran = subprocess.run(command, capture_output=True, text=True)
        if ran.returncode != 0:
            detail = (ran.stderr or ran.stdout).strip()[:2000]
            errfile = args.out / "error.txt"
            if errfile.exists():
                detail = (detail + "\n" + errfile.read_text()).strip()[:4000]
            print("running the probe failed:\n" + detail, file=sys.stderr)
            return 1

    if args.stages == "carved":
        rows = [line.split("\t") for line in
                (args.out / "carved_meta.tsv").read_text().splitlines()
                if line and line[0] != "#"]
        for row in rows:
            print(f"wrote {args.out}/carved_seed{row[0]}_c{row[1]}_{row[2]}.bin.gz "
                  f"({row[5]} cells, {len(row[8].split(','))} states)")
        return 0

    if (args.out / "primitives.tsv").is_file():
        primitives = (args.out / "primitives.tsv").read_text().splitlines()
        noise_rows = sum(1 for line in primitives if line.startswith("noise\t"))
        print(f"wrote {args.out}/primitives.tsv ({len(primitives) - 1} rows, "
              f"{noise_rows} noise samples)")
    chunks = ", ".join(f"c{c[0]}_{c[1]}" for c in args.chunk)
    print(f"wrote {args.out}/router.tsv and the per-chunk interpolated/noise dumps ({chunks})")
    parameters = (args.out / "biome_parameters.tsv").read_text().splitlines()
    biome_rows = (args.out / "biome_meta.tsv").read_text().splitlines()
    rows = [line.split("\t") for line in biome_rows if line and line[0] != "#"]
    cells = sum(int(row[5]) for row in rows)
    mismatches = sum(int(row[6]) for row in rows)
    print(f"wrote {args.out}/biome_parameters.tsv "
          f"({sum(1 for line in parameters if line and line[0] != '#')} parameter points)")
    print(f"dimension: {args.dimension}")
    print(f"wrote {args.out}/biomes_seed... for {len(rows)} chunks ({cells} cells, "
          f"{mismatches} rtree/brute-force mismatches)")
    surface_rows = [line for line in (args.out / "surface_meta.tsv").read_text().splitlines()
                    if line and line[0] != "#"]
    surface_states = sum(len(row.split("\t")[8].split(",")) for row in surface_rows)
    print(f"wrote {args.out}/surface_seed... for {len(surface_rows)} chunks "
          f"({surface_states} distinct surface states)")
    print(f"wrote {args.out}/surface_noise.tsv (the legacy simplex stack)")
    for row in rows:
        if int(row[6]):
            print(f"  chunk {row[1]},{row[2]}: {row[6]} mismatches, first {row[7]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
