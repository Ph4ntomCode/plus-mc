# NOTICE

+MC (PlusMC) — Third-party data and attribution

The code in this repository is licensed as described in `LICENSE` (GPL-3.0 for the core) and
`LICENSE.PLUGIN-API` (MIT OR Apache-2.0 for the plugin API). **Third-party data is separate.**

## Mojang data

`tools/vanilla-data` downloads and extracts data from Mojang's official Minecraft version
artifacts (server jars / piston-meta version metadata). That data — e.g. `version.json`,
registries, recipes, loot tables, tags, advancements, worldgen JSON, server-side assets — is
Mojang's and is subject to Mojang's terms (Minecraft EULA / brand & asset usage guidelines).

Conventions adopted in this project (same approach as PumpkinMC's `assets/NOTICE.md`):

- Extracted data lives under `data/vanilla/` and is **gitignored** — it is never committed to
  this repository.
- The client asset tree prepared by `tools/vanilla_assets.py` (models, textures, sounds, fonts,
  shaders, language files) is read from the player's own installed client jar and asset store
  and also stays under `data/vanilla/`; it is never committed or shipped by this repository.
- If +MC ever *ships* extracted vanilla data in a release artifact, it ships with this
  NOTICE file plus per-data attribution, and only to the extent Mojang's terms permit.
- Engine code loads data files at runtime; nothing from Mojang's artifacts is copied into
  source code. See `docs/PLAN.md` §2.3 (legal ground rules).

## Other reference material

Protocol/version reference tables (e.g. PrismarineJS `minecraft-data`) are used as external
reference data for validation; they are not vendored. Consult their respective licenses if
any of their data is ever included in a release artifact.
