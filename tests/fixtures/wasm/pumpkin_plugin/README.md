# pumpkin_plugin — a real `cargo component` artifact

`../pumpkin_plugin.wasm` is the `cargo component build --release` output of this
project (Rust 1.97.1, cargo-component 0.21.1, target `wasm32-wasip1`). It is compiled
from Rust, not assembled in Python: `src/lib.rs` implements the generated `Guest`
trait and calls the generated `pumpkin:plugin` bindings.

The world keeps Pumpkin v0.2's real package, interface and member names for the
members the host backs: `logging.log`, `server.get-player-count`, `server.get-all-players`, `server.get-player-count-in-world`, `server.get-players-in-world`, `server.get-motd`,
`server.get-world-by-name`, `server.get-player-by-name`, `player.get-id`, `player.get-name`,
`player.get-position`, `player.get-yaw`,`player.get-pitch`, `player.get-gamemode`, `player.get-health`,
`player.get-max-health`, `player.get-food-level`, `player.get-saturation`,
`player.get-exhaustion`, `player.get-absorption`, `player.get-experience-level`,
`player.get-experience-progress`, `player.get-experience-points`, `player.get-camera-entity-id`, `server.broadcast`,
`context.register-event`, `context.register-command`, `context.register-permission`,
`context.get-marketplace-metadata`, `context.get-data-folder`, the command resource
(`constructor`, `then`, `execute-with-handler-id`) and its `command-node`
builder (`literal`, `argument` with all 53 `argument-type` cases, nested `then`,
execute/suggest/require handlers), the `scheduler` delayed/repeating/cancel calls, `uuid`
generate/parse/format, `i18n` translation loading/lookup, `item-stack`
identity/count, durability, enchantment and lore operations,
`world.get-time-of-day`, `world.is-raining`,
`world.is-thundering`, `world.get-block-state-id`, the async `world.set-block` with
its full `block` record and `block-flags`, the async `player.chat`,
`player.send-system-message` with both overlay values, and the `text-component`
builders (`text`, `add-text`, `add-child`, named/RGB colour, the five boolean
styles, insertion, font, click and hover-text), with their real signatures
(including the `block-pos` and `rgb-color` records and the `context` / `server` /
`world` resource handles). It also exports the first 41 `event-type`/`event` cases,
so `server-tick-start-event` keeps its real index 40, and the callback is synchronous:

```wit
export init-plugin: func();
export on-load: func(ctx: context);
export on-unload: func(ctx: context);
export handle-event: func(event-id: u32, server: server, event: event) -> event;
export handle-task: func(handler-id: u32, server: server);
```

## Why this is a subset, not the full v0.2 world

The full `pumpkin-plugin-wit` v0.2 WIT in Pumpkin's tree is 676 KiB and does not parse
with stock tooling: 132 identifiers end in a digit suffix (for example
`unknown-uuid-1`, `notches-6`, `resonate-1`), which `wit-parser` rejects as invalid
identifiers. A patched copy parses and generates bindings, but a world with Pumpkin's
async exports does not compile with cargo-component 0.21.1: its generated bindings
hardcode `AsyncConfig::None`, so the shims call `T::async_on_load` while the generated
`Guest` trait only has the sync methods, and `wit_bindgen_rt::async_support` is never
generated. This fixture therefore uses a valid subset of the same package and a sync
lifecycle. +MC hosts both the async v0.2 lifecycle and this sync shape; the sync
lifecycle is not Pumpkin's ABI.

## What running it proves

The component imports the Rust runtime's `wasi:` interfaces and imports resource
methods with implicit handles, so the strict linker does not model its type graph;
the explicit compiled-core path hosts it. On load it registers handler 7 for
`server-tick-start-event`, schedules a delayed task (handler 11), a long-period
repeating task (handler 12) and a repeating task it immediately cancels (handler
13), then round-trips a generated UUID, parses an invalid one, loads a flat JSON
translation map and translates a present and a missing key, constructs a
`minecraft:diamond` stack and verifies its count/max-count, clone, with-count
and similarity, then constructs a `minecraft:diamond_sword` and verifies its
damage, max damage, damageability, unbreakable flag, repair cost, standard and
custom enchantments, lore and custom name, then builds a four-node command tree (`sub` with
execute, suggestion and requirement handlers; `leaf`; an `integer(1..10)` argument; and a
greedy `string` argument, each with an execute handler), attaches it
to the command, executes the command, registers it with a permission string, registers
a valid namespaced permission and an invalid one (checking the returned host error), reads
the marketplace metadata, reads the host-chosen data folder, and calls `context.get-server`,
reads the player count, enumerates all connected players, reads its per-world count and gets the named world's player handles,
looks up `minecraft:overworld`, reads time, rain, thunder and a block state id, calls
`world.set-block` at `(2, 64, -3)` with a real 15-field `block` record (default state
7), logs the result, calls `server.broadcast("compiled broadcast")`, looks up the
player named `Buffy`, reads the host-issued handle's UUID, name, position, yaw, pitch, gamemode, health, max health, food, saturation, exhaustion, absorption, experience values and camera entity id
(and, with `--host-player Buffy`, builds a styled text resource
and calls `player.send-system-message` with `overlay=true`) and calls
`player.chat("compiled player chat")` on that host-issued handle; on load and unload
it also reads `server.get-motd()` and logs the configured MOTD; on unload it repeats
the report and broadcast. Without `--host-player Buffy` the lookup returns `none`, so
the chat and system-message calls are skipped. The handles are typed and
instance-scoped, and a dropped handle is invalid; there is no fabricated handle. The live server dispatches every
tick to `handle-event` and to `handle-task`; the delayed and repeating tasks log
`task 11` / `task 12`, the cancelled task logs nothing, and the fixture logs every
twentieth tick. `tests/wasm_server_test.py` waits for both callbacks. Unlisted
interfaces and WASI imports are deterministic stubs.

## Rebuild

```sh
cargo component build --release
cp target/wasm32-wasip1/release/compiled_plugin.wasm ../pumpkin_plugin.wasm
```

`src/bindings.rs` and `target/` are generated by cargo-component and ignored. The
checked-in binary's SHA-256 is
`312c9656007e9580d7dad323bd0489e2028a8b6fe4bb16fa9ef83224bd0027be`.

cargo-component 0.21.1 generates synchronous Rust wrappers for the async imports
(`[async method]player.chat`, `[async method]world.set-block` and
`[async method]server.broadcast`). The compatibility host binds those names and
completes all three calls synchronously; the checked-in compiler artifact exercises
their canonical parameter/lowering paths, not the component model async task ABI.
The component runtime also accepts real async lowers for `player.chat`,
`world.set-block` and `server.broadcast`: a hand-built fixture in
`tests/wasm_component_run_test.py` joins both returned subtasks and waits for their
returned events. The compiler artifact remains on its synchronous wrappers.
