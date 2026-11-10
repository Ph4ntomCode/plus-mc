# +MC

+MC (PlusMC) is a Minecraft Java Edition server written in C++20. It is built
around the vanilla game's own data and behavior, with a focus on performance
and a small, understandable codebase.

The server currently targets Minecraft 26.3 (protocol 777). Translation for
older clients is in progress, but not every legacy protocol is supported yet.

## What it does

- Generates and streams the overworld, including caves, biomes, structures,
  and lighting.
- Handles login, configuration, chunk streaming, chat, movement, inventory,
  containers, and world persistence.
- Implements survival gameplay: health, hunger, damage, mobs, breeding,
  villagers and trading, brewing, enchantments, advancements, and scoreboards.
- Supports redstone, pistons, fluids, TNT, fire, weather, and scheduled block
  ticks.
- Provides a native C++ plugin API with hot reload and a working example
  plugin, plus tools for inspecting and running Pumpkin WASM plugins.
- Reads Mojang's data files directly. Game data is not bundled with the
  source.

## Build

You need CMake 3.24 or newer, a C++20 compiler, Ninja, zlib, OpenSSL, and
libcurl or the `curl` command-line tool.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The main server binary is `build/src/plusmc`. On Windows, use an MSYS2 UCRT64
shell and the `windows-mingw` CMake preset.

## Run

Online mode is the default:

```bash
./build/src/plusmc --host 0.0.0.0 --port 25565 --motd "A +MC server"
```

For a local or offline server:

```bash
./build/src/plusmc --host 0.0.0.0 --port 25565 --offline-mode --motd "A +MC server"
```

Extract the vanilla data for the version you want to run before starting:

```bash
./build/plusmc-vanilla-data --version latest --out data/vanilla
```

The server uses the newest extract under `data/vanilla/`. You can override the
advertised version with `--version-name` and `--protocol`.

## Status

+MC is under active development. The core server and the current-version join
flow work, but some features are incomplete and behavior can change between
commits. It is not yet a drop-in replacement for a production Paper or Vanilla
server.

## License

The engine is licensed under GPLv3. The native plugin API is available under
either MIT or Apache-2.0. Mojang's game data is not included and remains subject
to Mojang's own terms.
