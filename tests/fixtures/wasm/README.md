# WASM component fixtures

`v02_compiled_plugin.wasm` is a compiler-produced component for the full Pumpkin
`pumpkin-plugin-wit` v0.2 `plugin` world (55 core imports). It was generated with
`wasm-tools component embed --dummy` followed by `component new`, then its
`init-plugin` / `on-load` / `on-unload` bodies were replaced with logging calls so
a run is observable. It uses standard32 `cm32p2|...` names and the callback
lifecycle shape `on-load: (i32) -> i32`.

It is a shape fixture, not a Pumpkin plugin: its logging lifecycle is backed, and
every other import traps if called on the host side. SHA-256:
`4702650fb9418e4e60509eb3247cad687c316f1d2426b8215e17b54e576ed9ce`.

`rust_lifecycle.wasm` is a real `rustc` output for `wasm32-wasip1`, built from
`rust_lifecycle.rs`. It deliberately uses a funcref table, an element segment,
`call_indirect`, `memory.copy`, `memory.fill` and Rust's formatting machinery.
Stripping its custom sections leaves SHA-256:
`699323b07980da53fa98beafcccab7900c06e6ade6a98247da6d10fc189b063d`.

`rust_component.wasm` wraps `rust_component.rs` with
`wasm-tools component embed` and `component new` against `rust_component.wit`.
It is the compiler-shaped component regression for the spaced lift indices
(0, 2, 4) that `wit-component` emits. Stripping custom sections leaves SHA-256:
`251b806d7c2a712f4c9b5c9eb48d5463ebd11268c462c7771b1e073b4c68c191`.

`pumpkin_plugin.wasm` is a real `cargo component build --release` artifact compiled
from Rust, not assembled in Python. Its world uses the real `pumpkin:plugin@0.2.0`
package, interface and member names for the backed members (`logging.log`,
`server.get-player-count`, `server.get-all-players`, `server.get-players-in-world`, `server.get-world-by-name`, `server.get-player-by-name`,
`server.broadcast`, `player.get-id`, `player.get-name`, `player.get-position`, `player.get-yaw`,
`player.get-pitch`, `player.get-gamemode`, `player.get-health`,
`player.get-max-health`, `player.get-food-level`, `player.get-saturation`,
`player.get-exhaustion`, `player.get-absorption`, `player.get-experience-level`,
`player.get-experience-progress`,
`player.get-experience-points`, `player.get-camera-entity-id`, `context.register-event`,
the `scheduler` delayed/repeating/cancel
calls, `uuid` generate/parse/format, `i18n` translation loading/lookup and
`item-stack` identity/count, durability, enchantment and lore operations,
`world.get-time-of-day`,
`world.is-raining`, `world.is-thundering`, `world.get-block-state-id`, async
`world.set-block`, async `player.chat`, `player.send-system-message` with
`overlay=true`, and the `text-component` builders including named/RGB colour,
boolean styles, insertion, font, click and hover-text), a
synchronous `init-plugin` / `on-load(ctx)` / `on-unload(ctx)` lifecycle, a
synchronous `handle-event` and a `handle-task` callback. The first 41
`event-type`/`event` cases keep Pumpkin's real names and indices, including
`server-tick-start-event` at 40. It is a valid subset of the v0.2 WIT, not the full
world: the full WIT has 132 digit-suffixed identifiers that `wit-parser` rejects, and
cargo-component 0.21.1 cannot compile async exports. The component imports WASI and
resource methods, so it takes the explicit compiled-core path. Its source and rebuild
command live in `pumpkin_plugin/`; SHA-256:
`312c9656007e9580d7dad323bd0489e2028a8b6fe4bb16fa9ef83224bd0027be`.

Rebuild commands (Rust 1.97.1 and wasm-tools 1.263.0):

```sh
rustc --target wasm32-wasip1 -O -C target-feature=+bulk-memory,+reference-types \
  --crate-type cdylib -o rust_lifecycle.wasm rust_lifecycle.rs
wasm-tools strip rust_lifecycle.wasm -o rust_lifecycle.wasm

rustc --target wasm32-wasip1 -O -C target-feature=+bulk-memory,+reference-types \
  --crate-type cdylib -o rust_component_core.wasm rust_component.rs
wasm-tools component embed . rust_component_core.wasm -o embedded.wasm
wasm-tools component new embedded.wasm -o rust_component.wasm
wasm-tools strip rust_component.wasm -o rust_component.wasm
```
