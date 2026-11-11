# +MC vanilla-data (Phase 0)

The first real tool of the project: download any Minecraft server version and extract the
vanilla data the engine will be built on — `version.json` (protocol + data version), the
datapack tree (`data/minecraft/**` and any other namespaces), `pack.mcmeta`, plus an
`index.json` summary.

This is the backbone of the data-first strategy in `docs/PLAN.md` §5/§7:

- the **engine loads Mojang's own data** (content parity by construction), and
- **every Mojang release is a data refresh + diff**, not a rewrite (run it for the new
  version, diff against the previous one, get the change report that drives mechanics work).

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Dependencies: CMake ≥ 3.24, C++20 compiler, zlib. Downloads use libcurl if found at configure
time, otherwise the `curl` CLI. The JSON parser and zip reader are self-contained in this repo
(`src/json.hpp`, zip logic in `src/main.cpp`).

## Usage

```bash
# Latest stable release (26.2 as of Sep 2026) -> data/vanilla/<version>/
./build/plusmc-vanilla-data --version latest --out data/vanilla

# A specific release
./build/plusmc-vanilla-data --version 1.21.11 --out data/vanilla

# No network: point at an already-downloaded server jar
./build/plusmc-vanilla-data --jar ~/Downloads/server.jar --out data/vanilla

# See what versions Mojang currently lists
./build/plusmc-vanilla-data --list-versions

# Keep the downloaded jar for offline re-runs
./build/plusmc-vanilla-data --version latest --out data/vanilla --keep-jar

# Re-check Mojang for new releases (manifest/metadata are cached under data/vanilla/_cache;
# without --refresh a cached manifest silently decides what "latest" means)
./build/plusmc-vanilla-data --version latest --out data/vanilla --refresh
```

## Output layout

```
data/vanilla/<version-id>/
├─ version.json          # id, protocol_version, world_version (= data version), …
├─ data/minecraft/…      # registries, recipes, loot_tables, tags, worldgen, …
│                        #   (datapack-level pack.mcmeta live under data/minecraft/datapacks/*/;
│                        #    modern jars have no root pack.mcmeta)
└─ index.json            # machine-readable summary (version, counts, generatedAt)
```

## Notes

- **Bundler jars handled automatically:** recent server downloads (verified on 1.21.11 *and*
  26.2) nest the real server under `META-INF/versions/<ver>/server-<ver>.jar`; the tool
  detects and recurses into it. Verified: 26.2 (protocol 776) → 8,527 files (~9 MB);
  1.21.11 (protocol 774) → 7,558 files (~8.3 MB).
- Extraction is **data-only**: no game code is (or may be) copied. Mojang's data is used
  under their terms; see `docs/PLAN.md` §2.3 and the open licensing decision in §14.
- Data folder names change between versions (e.g. `loot_tables` vs `loot_table`, new
  `datapacks/`/`dialog` trees in 26.x) — consumers must read the extracted tree, never
  hardcode paths.
- Dev builds (snapshots/pre-releases, synthetic protocol numbers `≥ 0x40000000`) are not
  interesting targets; releases only.
- Next iteration (roadmap): diff two extracted versions and emit a machine-readable
  "content delta" report to drive per-release mechanics work.
