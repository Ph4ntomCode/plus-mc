#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The +MC WASM component adapter (Phase 8, slice 3).

`plusmc-wasm-adapt` reads a component's import and export sections and maps
every imported interface onto the tracked `pumpkin:plugin@0.2.0` table. The
table's signatures are the real `pumpkin-plugin-wit` v0.2 declarations, so this
test pins the shapes that matter: `log(level, message)`, resource methods with
an implicit `self`, `text-component` and `async` members. It assembles real
component bytes in Python, plus one checked-in compiler-produced fixture, and pins:

  * a mapped component: the pinned interfaces, their backed and unbacked
    members, the lifecycle exports, the JSON and its determinism;
  * that component sections may repeat and appear in any order (two core
    modules, two import sections), which a core-module reader must not refuse;
  * stub verdicts: WASI and a real-but-unpinned Pumpkin interface map to the
    disclosed `component_stub`; a plain function import and a component without
    `on-load` are still blocked;
  * a checked-in compiler-produced component with the full 55-import v0.2
    world, mapped to a partial surface of real members and stubs;
  * the name forms: the legacy 0x01 lead, `implements`, and a version suffix;
  * structural refusals: an unknown name/external/attribute kind, a section
    past the end, a duplicate import/export, an unknown section id and a bad
    optional export type;
  * a package directory and its manifest fields;
  * the CLI's exit codes and usage.

Run: wasm_component_test.py <plusmc-wasm> <plusmc-wasm-adapt>
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT = 60
MAGIC = b"\x00asm"
CORE_VERSION = b"\x01\x00\x00\x00"
COMPONENT_VERSION = b"\x0d\x00\x01\x00"


def run(cli, args, expect=0):
    proc = subprocess.run(
        [str(cli), *[str(arg) for arg in args]],
        text=True,
        capture_output=True,
        timeout=TIMEOUT,
    )
    if expect is not None and proc.returncode != expect:
        raise AssertionError(
            f"{Path(cli).name} exited {proc.returncode}, expected {expect}: {proc.args}\n"
            f"stdout:\n{proc.stdout[-6000:]}\nstderr:\n{proc.stderr[-6000:]}"
        )
    return proc


def uleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def name(text):
    encoded = text.encode("utf-8") if isinstance(text, str) else text
    return uleb(len(encoded)) + encoded


def section(section_id, payload):
    return bytes([section_id]) + uleb(len(payload)) + payload


def component(*sections):
    return MAGIC + COMPONENT_VERSION + b"".join(sections)


def core_module():
    return MAGIC + CORE_VERSION


def extern(text, lead=0x00):
    return bytes([lead]) + name(text)


def instance_import(text, lead=0x00, type_index=0):
    return extern(text, lead) + b"\x05" + uleb(type_index)


def func_import(text, lead=0x00, type_index=0):
    return extern(text, lead) + b"\x01" + uleb(type_index)


def func_export(text, index=0, ascribed=False):
    tail = b"\x01\x01" + uleb(0) if ascribed else b"\x00"
    return extern(text) + b"\x01" + uleb(index) + tail


def import_section(entries):
    return section(10, uleb(len(entries)) + b"".join(entries))


def export_section(entries):
    return section(11, uleb(len(entries)) + b"".join(entries))


def mapped_component():
    return component(
        # Component sections are definitions: two core modules and two import
        # sections are legal, and in this order.
        section(1, core_module()),
        section(1, core_module()),
        import_section(
            [
                instance_import("pumpkin:plugin/logging@0.2.0"),
                instance_import("pumpkin:plugin/server@0.2.0"),
            ]
        ),
        import_section(
            [
                instance_import("pumpkin:plugin/world@0.2.0"),
                instance_import("pumpkin:plugin/scheduler@0.2.0"),
                instance_import("pumpkin:plugin/player@0.2.0"),
            ]
        ),
        export_section(
            [
                func_export("on-load", 0, ascribed=True),
                func_export("init-plugin", 1),
                func_export("handle-event", 2),
            ]
        ),
    )


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: wasm_component_test.py <plusmc-wasm> <plusmc-wasm-adapt>")
    inspect_cli = Path(sys.argv[1]).resolve()
    adapt_cli = Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="plusmc-wasm-component-") as temporary:
        root = Path(temporary)
        good_bytes = mapped_component()
        good = root / "claims.wasm"
        good.write_bytes(good_bytes)

        # The gate still inspects the same file, and now accepts the repeated
        # component sections a component is allowed to have.
        gate = json.loads(run(inspect_cli, ["inspect", good, "--json"]).stdout)
        assert gate["ok"] is True, gate
        assert gate["status"] == "component", gate
        assert gate["module"]["kind"] == "component", gate
        print("PASS gate accepts repeated component sections")

        text = run(adapt_cli, [good]).stdout
        assert "+MC wasm component adapter" in text, text
        assert "kind: component" in text, text
        assert "status: mapped" in text, text
        assert "coverage: full" in text, text
        assert "imports: 5" in text, text
        assert "pumpkin:plugin/logging@0.2.0  instance  pinned" in text, text
        assert (
            "backed: log  func(level: level, message: string) -> plusmc:host/log_level"
            in text
        ), text
        assert (
            "backed: log-tracing  func(event: list<u8>) -> plusmc:host/log_tracing"
            in text
        ), text
        assert "pumpkin:plugin/server@0.2.0  instance  pinned" in text, text
        assert (
            "backed: [method]server.get-motd  func() -> string -> "
            "plusmc:host/server_motd" in text
        ), text
        assert (
            "backed: [method]server.get-player-count  func() -> u32 -> "
            "plusmc:host/player_count" in text
        ), text
        assert (
            "backed: [method]server.get-all-players  func() -> list<player> -> "
            "plusmc:host/server_all_players" in text
        ), text
        assert (
            "backed: [method]server.get-player-count-in-world  "
            "func(world-ref: %world) -> u32 -> "
            "plusmc:host/server_player_count_in_world" in text
        ), text
        assert (
            "backed: [method]server.get-players-in-world  "
            "func(world-ref: %world) -> list<player> -> "
            "plusmc:host/server_players_in_world" in text
        ), text
        assert (
            "backed: [method]server.get-player-by-name  func(name: string) -> "
            "option<player> -> plusmc:host/server_player_by_name" in text
        ), text
        assert (
            "backed: [method]server.get-world-by-name  func(name: string) -> "
            "option<%world> -> plusmc:host/pumpkin_server_world_by_name" in text
        ), text
        assert (
            "backed: [method]server.has-world  func(name: string) -> bool -> "
            "plusmc:host/server_has_world" in text
        ), text
        assert (
            "backed: [method]server.broadcast  async func(message: string) -> "
            "plusmc:host/server_broadcast" in text
        ), text
        assert "pumpkin:plugin/world@0.2.0  instance  pinned" in text, text
        assert (
            "backed: [method]world.get-block-state-id  func(pos: block-pos) -> u16 -> "
            "plusmc:host/world_get_block" in text
        ), text
        assert (
            "backed: [method]world.get-time-of-day  func() -> u64 -> plusmc:host/world_time"
            in text
        ), text
        assert (
            "backed: [method]world.get-block  func(pos: block-pos) -> block -> "
            "plusmc:host/world_get_block_info" in text
        ), text
        assert (
            "backed: [method]world.get-block-id  func(pos: block-pos) -> u16 -> "
            "plusmc:host/world_get_block_id" in text
        ), text
        assert (
            "backed: [method]world.set-time-of-day  func(time: u64) -> "
            "plusmc:host/world_set_time" in text
        ), text
        assert "pumpkin:plugin/scheduler@0.2.0  instance  pinned" in text, text
        assert (
            "backed: schedule-delayed-task  func(handler-id: u32, delay-ticks: u64) -> "
            "u32 -> plusmc:host/scheduler_schedule_delayed" in text
        ), text
        assert (
            "backed: schedule-repeating-task  func(handler-id: u32, delay-ticks: u64, "
            "period-ticks: u64) -> u32 -> plusmc:host/scheduler_schedule_repeating"
            in text
        ), text
        assert (
            "backed: cancel-task  func(task-id: u32) -> plusmc:host/scheduler_cancel"
            in text
        ), text
        assert "pumpkin:plugin/player@0.2.0  instance  pinned" in text, text
        assert (
            "backed: [method]player.get-id  func() -> uuid -> "
            "plusmc:host/player_get_id" in text
        ), text
        assert (
            "backed: [method]player.get-position  func() -> position -> "
            "plusmc:host/player_get_position" in text
        ), text
        assert (
            "backed: [method]player.get-yaw  func() -> f32 -> "
            "plusmc:host/player_get_yaw" in text
        ), text
        assert (
            "backed: [method]player.get-pitch  func() -> f32 -> "
            "plusmc:host/player_get_pitch" in text
        ), text
        assert (
            "backed: [method]player.get-gamemode  func() -> game-mode -> "
            "plusmc:host/player_get_gamemode" in text
        ), text
        assert (
            "backed: [method]player.get-health  func() -> f32 -> "
            "plusmc:host/player_get_health" in text
        ), text
        assert (
            "backed: [method]player.get-max-health  func() -> f32 -> "
            "plusmc:host/player_get_max_health" in text
        ), text
        assert (
            "backed: [method]player.get-food-level  func() -> u8 -> "
            "plusmc:host/player_get_food_level" in text
        ), text
        assert (
            "backed: [method]player.get-saturation  func() -> f32 -> "
            "plusmc:host/player_get_saturation" in text
        ), text
        assert (
            "backed: [method]player.get-exhaustion  func() -> f32 -> "
            "plusmc:host/player_get_exhaustion" in text
        ), text
        assert (
            "backed: [method]player.get-absorption  func() -> f32 -> "
            "plusmc:host/player_get_absorption" in text
        ), text
        assert (
            "backed: [method]player.get-experience-level  func() -> s32 -> "
            "plusmc:host/player_get_experience_level" in text
        ), text
        assert (
            "backed: [method]player.get-experience-progress  func() -> f32 -> "
            "plusmc:host/player_get_experience_progress" in text
        ), text
        assert (
            "backed: [method]player.get-experience-points  func() -> s32 -> "
            "plusmc:host/player_get_experience_points" in text
        ), text
        assert (
            "backed: [method]player.get-camera-entity-id  func() -> u32 -> "
            "plusmc:host/player_get_camera_entity_id" in text
        ), text
        assert (
            "backed: [method]player.chat  async func(message: string) -> "
            "plusmc:host/player_chat" in text
        ), text
        assert (
            "backed: [method]player.send-system-message  "
            "func(text: text-component, overlay: bool) -> "
            "plusmc:host/player_send_system_message" in text
        ), text
        assert "async func(context: context) -> result<_, string>" in text, text
        assert "on-enable" not in text, text
        assert "exports: 3" in text, text
        assert "on-load  func #0" in text, text
        assert "init-plugin  func #1" in text, text
        assert "lifecycle: on-load exported" in text, text
        assert "init-plugin (Pumpkin's constructor hook)" in " ".join(text.split()), text
        print("PASS mapped component")

        report = json.loads(run(adapt_cli, [good, "--json"]).stdout)
        assert report["tool"] == "plusmc-wasm", report
        assert report["schema"] == 1 and report["mode"] == "adapt", report
        assert report["ok"] is True and report["status"] == "mapped", report
        assert report["coverage"] == "full", report
        assert report["package"]["name"] == "", report
        assert report["module"]["kind"] == "component", report
        assert report["module"]["sha256"] == hashlib.sha256(good_bytes).hexdigest(), report
        assert report["module"]["size"] == len(good_bytes), report
        names = [entry["name"] for entry in report["imports"]]
        assert names == [
            "pumpkin:plugin/logging@0.2.0",
            "pumpkin:plugin/server@0.2.0",
            "pumpkin:plugin/world@0.2.0",
            "pumpkin:plugin/scheduler@0.2.0",
            "pumpkin:plugin/player@0.2.0",
        ], report
        logging_import = report["imports"][0]
        assert logging_import["kind"] == "instance", report
        assert logging_import["pinned"] is True, report
        assert logging_import["backed"] == [
            "log  func(level: level, message: string) -> plusmc:host/log_level",
            "log-tracing  func(event: list<u8>) -> plusmc:host/log_tracing",
        ], report
        assert logging_import["unbacked"] == [], report
        server_import = report["imports"][1]
        assert server_import["backed"] == [
            "[method]server.get-difficulty  func() -> difficulty -> "
            "plusmc:host/server_difficulty",
            "[method]server.get-motd  func() -> string -> plusmc:host/server_motd",
            "[method]server.get-player-count  func() -> u32 -> plusmc:host/player_count",
            "[method]server.get-all-players  func() -> list<player> -> "
            "plusmc:host/server_all_players",
            "[method]server.get-player-count-in-world  func(world-ref: %world) -> "
            "u32 -> plusmc:host/server_player_count_in_world",
            "[method]server.get-players-in-world  func(world-ref: %world) -> "
            "list<player> -> plusmc:host/server_players_in_world",
            "[method]server.get-max-players  func() -> u32 -> "
            "plusmc:host/server_max_players",
            "[method]server.is-online-mode  func() -> bool -> "
            "plusmc:host/server_online_mode",
            "[method]server.has-whitelist  func() -> bool -> "
            "plusmc:host/server_whitelist_enabled",
            "[method]server.get-view-distance  func() -> u8 -> "
            "plusmc:host/server_view_distance",
            "[method]server.get-simulation-distance  func() -> u8 -> "
            "plusmc:host/server_simulation_distance",
            "[method]server.get-default-gamemode  func() -> game-mode -> "
            "plusmc:host/server_default_gamemode",
            "[method]server.get-player-by-name  func(name: string) -> option<player> -> "
            "plusmc:host/server_player_by_name",
            "[method]server.get-world-by-name  func(name: string) -> option<%world> -> "
            "plusmc:host/pumpkin_server_world_by_name",
            "[method]server.has-world  func(name: string) -> bool -> "
            "plusmc:host/server_has_world",
            "[method]server.broadcast  async func(message: string) -> "
            "plusmc:host/server_broadcast",
        ], report
        assert server_import["unbacked"] == [], report
        world_import = report["imports"][2]
        assert len(world_import["backed"]) == 8, report
        assert (
            "[method]world.get-block-state-id  func(pos: block-pos) -> u16 -> "
            "plusmc:host/world_get_block" in world_import["backed"]
        ), report
        assert (
            "[method]world.get-block  func(pos: block-pos) -> block -> "
            "plusmc:host/world_get_block_info" in world_import["backed"]
        ), report
        assert (
            "[method]world.get-block-id  func(pos: block-pos) -> u16 -> "
            "plusmc:host/world_get_block_id" in world_import["backed"]
        ), report
        assert (
            "[method]world.set-time-of-day  func(time: u64) -> "
            "plusmc:host/world_set_time" in world_import["backed"]
        ), report
        assert (
            "[method]world.set-block  async func(pos: block-pos, block: block, "
            "update-flags: block-flags) -> plusmc:host/pumpkin_world_set_block"
            in world_import["backed"]
        ), report
        assert world_import["unbacked"] == [], report
        scheduler_import = report["imports"][3]
        assert scheduler_import["backed"] == [
            "schedule-delayed-task  func(handler-id: u32, delay-ticks: u64) -> "
            "u32 -> plusmc:host/scheduler_schedule_delayed",
            "schedule-repeating-task  func(handler-id: u32, delay-ticks: u64, "
            "period-ticks: u64) -> u32 -> plusmc:host/scheduler_schedule_repeating",
            "cancel-task  func(task-id: u32) -> plusmc:host/scheduler_cancel",
        ], report
        assert scheduler_import["unbacked"] == [], report
        player_import = report["imports"][4]
        assert player_import["backed"] == [
            "[method]player.get-id  func() -> uuid -> plusmc:host/player_get_id",
            "[method]player.get-name  func() -> string -> "
            "plusmc:host/player_get_name",
            "[method]player.get-position  func() -> position -> "
            "plusmc:host/player_get_position",
            "[method]player.get-yaw  func() -> f32 -> plusmc:host/player_get_yaw",
            "[method]player.get-pitch  func() -> f32 -> plusmc:host/player_get_pitch",
            "[method]player.get-gamemode  func() -> game-mode -> "
            "plusmc:host/player_get_gamemode",
            "[method]player.get-health  func() -> f32 -> "
            "plusmc:host/player_get_health",
            "[method]player.get-max-health  func() -> f32 -> "
            "plusmc:host/player_get_max_health",
            "[method]player.get-food-level  func() -> u8 -> "
            "plusmc:host/player_get_food_level",
            "[method]player.get-saturation  func() -> f32 -> "
            "plusmc:host/player_get_saturation",
            "[method]player.get-exhaustion  func() -> f32 -> "
            "plusmc:host/player_get_exhaustion",
            "[method]player.get-absorption  func() -> f32 -> "
            "plusmc:host/player_get_absorption",
            "[method]player.get-experience-level  func() -> s32 -> "
            "plusmc:host/player_get_experience_level",
            "[method]player.get-experience-progress  func() -> f32 -> "
            "plusmc:host/player_get_experience_progress",
            "[method]player.get-experience-points  func() -> s32 -> "
            "plusmc:host/player_get_experience_points",
            "[method]player.get-camera-entity-id  func() -> u32 -> "
            "plusmc:host/player_get_camera_entity_id",
            "[method]player.send-system-message  func(text: text-component, "
            "overlay: bool) -> plusmc:host/player_send_system_message",
            "[method]player.chat  async func(message: string) -> "
            "plusmc:host/player_chat",
        ], report
        assert player_import["unbacked"] == [], report
        assert report["exports"] == [
            {"name": "on-load", "kind": "func", "index": 0},
            {"name": "init-plugin", "kind": "func", "index": 1},
            {"name": "handle-event", "kind": "func", "index": 2},
        ], report
        assert report["reasons"] == [], report
        assert run(adapt_cli, [good, "--json"]).stdout == run(
            adapt_cli, [good, "--json"]
        ).stdout, "the report is not deterministic"
        run(adapt_cli, [good, "--require-full"])
        print("PASS mapped JSON")

        # Every listed member of the tracked callable interfaces is backed now,
        # so a component that imports only logging has full coverage and
        # --require-full must accept it.
        logging_only = root / "logging-only.wasm"
        logging_only.write_bytes(
            component(
                import_section([instance_import("pumpkin:plugin/logging@0.2.0")]),
                export_section([func_export("on-load", 0)]),
            )
        )
        logging_text = run(adapt_cli, [logging_only]).stdout
        assert "status: mapped" in logging_text, logging_text
        assert "coverage: full" in logging_text, logging_text
        logging_report = json.loads(run(adapt_cli, [logging_only, "--json"]).stdout)
        assert logging_report["status"] == "mapped", logging_report
        assert logging_report["coverage"] == "full", logging_report
        assert logging_report["imports"][0]["unbacked"] == [], logging_report
        run(adapt_cli, [logging_only, "--require-full"])
        print("PASS logging-only component has full coverage")

        # Type-only v0.2 interfaces have no host calls to lower. They are
        # tracked explicitly, so importing one is full coverage rather than a
        # generic no-op stub.
        type_only = root / "type-only.wasm"
        type_only.write_bytes(
            component(
                import_section(
                    [
                        instance_import("pumpkin:plugin/event@0.2.0"),
                        instance_import("pumpkin:plugin/biomes@0.2.0"),
                    ]
                ),
                export_section([func_export("on-load", 0)]),
            )
        )
        type_only_text = run(adapt_cli, [type_only]).stdout
        assert "status: mapped" in type_only_text, type_only_text
        assert "coverage: full" in type_only_text, type_only_text
        assert "pumpkin:plugin/event@0.2.0  instance  pinned" in type_only_text, type_only_text
        type_only_report = json.loads(run(adapt_cli, [type_only, "--json"]).stdout)
        assert type_only_report["status"] == "mapped", type_only_report
        assert type_only_report["coverage"] == "full", type_only_report
        assert all(entry["backed"] == [] for entry in type_only_report["imports"]), (
            type_only_report
        )
        run(adapt_cli, [type_only, "--require-full"])
        print("PASS type-only interfaces have full coverage")

        # uuid and i18n are complete callable interfaces: every member has a
        # host call, and a component importing only them passes --require-full.
        uuid_i18n = root / "uuid-i18n.wasm"
        uuid_i18n.write_bytes(
            component(
                import_section(
                    [
                        instance_import("pumpkin:plugin/uuid@0.2.0"),
                        instance_import("pumpkin:plugin/i18n@0.2.0"),
                    ]
                ),
                export_section([func_export("on-load", 0)]),
            )
        )
        uuid_i18n_text = run(adapt_cli, [uuid_i18n]).stdout
        assert "status: mapped" in uuid_i18n_text, uuid_i18n_text
        assert "coverage: full" in uuid_i18n_text, uuid_i18n_text
        assert "imports: 2" in uuid_i18n_text, uuid_i18n_text
        assert "pumpkin:plugin/uuid@0.2.0  instance  pinned" in uuid_i18n_text, uuid_i18n_text
        assert (
            "backed: generate  func() -> uuid -> plusmc:host/uuid_generate"
            in uuid_i18n_text
        ), uuid_i18n_text
        assert (
            "backed: parse  func(s: string) -> option<uuid> -> plusmc:host/uuid_parse"
            in uuid_i18n_text
        ), uuid_i18n_text
        assert (
            "backed: to-string  func(id: uuid) -> string -> plusmc:host/uuid_to_string"
            in uuid_i18n_text
        ), uuid_i18n_text
        assert "pumpkin:plugin/i18n@0.2.0  instance  pinned" in uuid_i18n_text, uuid_i18n_text
        assert (
            "backed: translate  func(key: string, locale: locale) -> string -> "
            "plusmc:host/i18n_translate" in uuid_i18n_text
        ), uuid_i18n_text
        assert (
            "backed: load-translations  func(namespace: string, json: string, "
            "locale: locale) -> plusmc:host/i18n_load_translations" in uuid_i18n_text
        ), uuid_i18n_text
        uuid_i18n_report = json.loads(run(adapt_cli, [uuid_i18n, "--json"]).stdout)
        assert uuid_i18n_report["coverage"] == "full", uuid_i18n_report
        assert uuid_i18n_report["imports"][0]["backed"] == [
            "generate  func() -> uuid -> plusmc:host/uuid_generate",
            "parse  func(s: string) -> option<uuid> -> plusmc:host/uuid_parse",
            "to-string  func(id: uuid) -> string -> plusmc:host/uuid_to_string",
        ], uuid_i18n_report
        assert uuid_i18n_report["imports"][1]["backed"] == [
            "translate  func(key: string, locale: locale) -> string -> "
            "plusmc:host/i18n_translate",
            "load-translations  func(namespace: string, json: string, locale: locale) -> "
            "plusmc:host/i18n_load_translations",
        ], uuid_i18n_report
        run(adapt_cli, [uuid_i18n, "--require-full"])
        print("PASS uuid and i18n have full coverage")

        # The full tracked item-stack surface is backed: identity/count, the
        # durability fields, standard and custom enchantments, lore and the
        # custom name. A component importing only this interface passes
        # --require-full.
        item_stack = root / "item-stack.wasm"
        item_stack.write_bytes(
            component(
                import_section([instance_import("pumpkin:plugin/item-stack@0.2.0")]),
                export_section([func_export("on-load", 0)]),
            )
        )
        item_stack_text = run(adapt_cli, [item_stack]).stdout
        assert "status: mapped" in item_stack_text, item_stack_text
        assert "coverage: full" in item_stack_text, item_stack_text
        assert "imports: 1" in item_stack_text, item_stack_text
        assert "pumpkin:plugin/item-stack@0.2.0  instance  pinned" in item_stack_text, item_stack_text
        item_stack_report = json.loads(run(adapt_cli, [item_stack, "--json"]).stdout)
        assert item_stack_report["coverage"] == "full", item_stack_report
        assert item_stack_report["imports"][0]["unbacked"] == [], item_stack_report
        assert item_stack_report["imports"][0]["backed"] == [
            "[constructor]item-stack  constructor(registry-key: string, count: u8) -> "
            "plusmc:host/item_stack_new",
            "[method]item-stack.get-registry-key  async func() -> string -> "
            "plusmc:host/item_stack_get_registry_key",
            "[method]item-stack.get-count  async func() -> u8 -> "
            "plusmc:host/item_stack_get_count",
            "[method]item-stack.set-count  async func(count: u8) -> "
            "plusmc:host/item_stack_set_count",
            "[method]item-stack.get-max-count  async func() -> u8 -> "
            "plusmc:host/item_stack_get_max_count",
            "[method]item-stack.clone  async func() -> item-stack -> "
            "plusmc:host/item_stack_clone",
            "[method]item-stack.with-count  async func(count: u8) -> item-stack -> "
            "plusmc:host/item_stack_with_count",
            "[method]item-stack.is-similar  async func(other: item-stack) -> bool -> "
            "plusmc:host/item_stack_is_similar",
            "[method]item-stack.get-damage  async func() -> s32 -> "
            "plusmc:host/item_stack_get_damage",
            "[method]item-stack.set-damage  async func(damage: s32) -> "
            "plusmc:host/item_stack_set_damage",
            "[method]item-stack.get-max-damage  async func() -> s32 -> "
            "plusmc:host/item_stack_get_max_damage",
            "[method]item-stack.is-damageable  async func() -> bool -> "
            "plusmc:host/item_stack_is_damageable",
            "[method]item-stack.is-unbreakable  async func() -> bool -> "
            "plusmc:host/item_stack_is_unbreakable",
            "[method]item-stack.set-unbreakable  async func(unbreakable: bool) -> "
            "plusmc:host/item_stack_set_unbreakable",
            "[method]item-stack.get-repair-cost  async func() -> s32 -> "
            "plusmc:host/item_stack_get_repair_cost",
            "[method]item-stack.set-repair-cost  async func(cost: s32) -> "
            "plusmc:host/item_stack_set_repair_cost",
            "[method]item-stack.get-item-model  async func() -> option<string> -> "
            "plusmc:host/item_stack_get_item_model",
            "[method]item-stack.set-item-model  async func(model: option<string>) -> "
            "plusmc:host/item_stack_set_item_model",
            "[method]item-stack.get-enchantment-glint-override  async func() -> "
            "option<bool> -> plusmc:host/item_stack_get_enchantment_glint_override",
            "[method]item-stack.set-enchantment-glint-override  async "
            "func(glint: option<bool>) -> "
            "plusmc:host/item_stack_set_enchantment_glint_override",
            "[method]item-stack.get-attribute-modifiers  async func() -> "
            "list<item-attribute-modifier> -> "
            "plusmc:host/item_stack_get_attribute_modifiers",
            "[method]item-stack.add-attribute-modifier  async "
            "func(modifier: item-attribute-modifier) -> "
            "plusmc:host/item_stack_add_attribute_modifier",
            "[method]item-stack.remove-attribute-modifiers  async "
            "func(attribute: attribute) -> "
            "plusmc:host/item_stack_remove_attribute_modifiers",
            "[method]item-stack.clear-attribute-modifiers  async func() -> "
            "plusmc:host/item_stack_clear_attribute_modifiers",
            "[method]item-stack.set-custom-data  async func(namespace: string, "
            "key: string, value: nbt-tree) -> "
            "plusmc:host/item_stack_set_custom_data",
            "[method]item-stack.get-custom-data  async func(namespace: string, "
            "key: string) -> option<nbt-tree> -> "
            "plusmc:host/item_stack_get_custom_data",
            "[method]item-stack.remove-custom-data  async func(namespace: string, "
            "key: string) -> plusmc:host/item_stack_remove_custom_data",
            "[method]item-stack.has-custom-data  async func(namespace: string, "
            "key: string) -> bool -> plusmc:host/item_stack_has_custom_data",
            "[method]item-stack.get-components  async func() -> "
            "list<data-component-value> -> plusmc:host/item_stack_get_components",
            "[method]item-stack.set-component  async func(component: "
            "data-component, value: list<u8>) -> "
            "plusmc:host/item_stack_set_component",
            "[method]item-stack.remove-component  async func(component: "
            "data-component) -> plusmc:host/item_stack_remove_component",
            "[method]item-stack.get-enchantments  async func() -> "
            "list<enchantment-value> -> plusmc:host/item_stack_get_enchantments",
            "[method]item-stack.add-enchantment  async func(enchantment: enchantment, "
            "level: u32) -> plusmc:host/item_stack_add_enchantment",
            "[method]item-stack.remove-enchantment  async func(enchantment: "
            "enchantment) -> plusmc:host/item_stack_remove_enchantment",
            "[method]item-stack.get-custom-enchantments  async func() -> "
            "list<custom-enchantment-value> -> "
            "plusmc:host/item_stack_get_custom_enchantments",
            "[method]item-stack.add-custom-enchantment  async "
            "func(enchantment-id: string, level: u32) -> "
            "plusmc:host/item_stack_add_custom_enchantment",
            "[method]item-stack.remove-custom-enchantment  async "
            "func(enchantment-id: string) -> "
            "plusmc:host/item_stack_remove_custom_enchantment",
            "[method]item-stack.get-custom-enchantment-level  async "
            "func(enchantment-id: string) -> option<u32> -> "
            "plusmc:host/item_stack_get_custom_enchantment_level",
            "[method]item-stack.has-custom-enchantment  async "
            "func(enchantment-id: string) -> bool -> "
            "plusmc:host/item_stack_has_custom_enchantment",
            "[method]item-stack.get-lore  async func() -> list<text-component> -> "
            "plusmc:host/item_stack_get_lore",
            "[method]item-stack.set-lore  async func(lore: list<text-component>) -> "
            "plusmc:host/item_stack_set_lore",
            "[method]item-stack.add-lore  async func(line: text-component) -> "
            "plusmc:host/item_stack_add_lore",
            "[method]item-stack.get-custom-name  async func() -> "
            "option<text-component> -> plusmc:host/item_stack_get_custom_name",
            "[method]item-stack.set-custom-name  async func(name: "
            "option<text-component>) -> plusmc:host/item_stack_set_custom_name",
        ], item_stack_report
        run(adapt_cli, [item_stack, "--require-full"])
        print("PASS item-stack has full coverage")

        # A real compiler artifact: the full v0.2 world, standard32 names and
        # a callback lifecycle. The adapter maps it even though most members
        # are stubs; it must name the stub path rather than refuse the file.
        compiled = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "wasm"
            / "v02_compiled_plugin.wasm"
        )
        compiled_report = json.loads(run(adapt_cli, [compiled, "--json"]).stdout)
        assert compiled_report["status"] == "mapped", compiled_report
        assert compiled_report["coverage"] == "partial", compiled_report
        assert len(compiled_report["imports"]) >= 20, compiled_report
        assert compiled_report["reasons"] == [], compiled_report
        assert {
            "init-plugin",
            "on-load",
            "on-unload",
        } <= {entry["name"] for entry in compiled_report["exports"]}, compiled_report
        assert any(
            backed.endswith("plusmc:host/component_stub")
            for entry in compiled_report["imports"]
            for backed in entry["backed"]
        ), compiled_report
        print("PASS compiled full-surface component maps to stubs")

        # A component that imports a tracked interface plus an unlisted one is
        # partial: the unlisted interface is a deterministic stub. The gate
        # must say so, because a stub is never full coverage.
        partial = root / "partial.wasm"
        partial.write_bytes(
            component(
                import_section(
                    [
                        instance_import("pumpkin:plugin/server@0.2.0"),
                        instance_import("pumpkin:plugin/gui@0.2.0"),
                    ]
                ),
                export_section([func_export("on-load", 0)]),
            )
        )
        partial_report = json.loads(run(adapt_cli, [partial, "--json"]).stdout)
        assert partial_report["status"] == "mapped", partial_report
        assert partial_report["coverage"] == "partial", partial_report
        proc = run(adapt_cli, [partial, "--require-full"], expect=1)
        assert "partial coverage, not full" in proc.stderr, proc.stderr
        print("PASS partial coverage gate")

        def blocked(path, needle):
            proc = run(adapt_cli, [path])
            assert needle in proc.stdout or needle in proc.stderr, (
                f"expected {needle!r}:\n{proc.stdout}\n{proc.stderr}"
            )
            # A verdict is a result; the CI gate is what turns it into exit 1.
            run(adapt_cli, [path, "--require-mapped"], expect=1)
            return proc

        def stubbed(path, name):
            text = run(adapt_cli, [path]).stdout
            assert "status: mapped" in text, text
            assert "coverage: partial" in text, text
            assert f"{name}  instance  pinned" in text, text
            assert "backed: all members -> plusmc:host/component_stub" in text, text
            assert "trapping host import" in text, text
            report = json.loads(run(adapt_cli, [path, "--json"]).stdout)
            assert report["status"] == "mapped" and report["coverage"] == "partial", report
            # The compatibility binding is accepted as mapped, but a stub is
            # never full coverage: --require-full must stay a real gate.
            run(adapt_cli, [path, "--require-mapped"])
            full = run(adapt_cli, [path, "--require-full"], expect=1)
            assert "partial coverage, not full" in full.stderr, full.stderr
            return text

        wasi = root / "wasi.wasm"
        wasi.write_bytes(
            component(
                import_section([instance_import("wasi:filesystem/preopens@0.2.0")]),
                export_section([func_export("on-load", 0)]),
            )
        )
        stubbed(wasi, "wasi:filesystem/preopens@0.2.0")
        print("PASS WASI is mapped to the component stub")

        gui = root / "gui.wasm"
        gui.write_bytes(
            component(
                import_section([instance_import("pumpkin:plugin/gui@0.2.0")]),
                export_section([func_export("on-load", 0)]),
            )
        )
        stubbed(gui, "pumpkin:plugin/gui@0.2.0")
        print("PASS unpinned Pumpkin interface is mapped to the component stub")

        plain = root / "plain.wasm"
        plain.write_bytes(
            component(
                import_section([func_import("clock")]),
                export_section([func_export("on-load", 0)]),
            )
        )
        proc = blocked(
            plain, "as a func; the adapter hosts interface instances and types only"
        )
        assert "unpinned" in proc.stdout, proc.stdout
        print("PASS component function import is blocked")

        no_lifecycle = root / "no-lifecycle.wasm"
        no_lifecycle.write_bytes(
            component(
                import_section([instance_import("pumpkin:plugin/logging@0.2.0")]),
                export_section([func_export("handle-event", 0)]),
            )
        )
        proc = blocked(no_lifecycle, "does not export on-load")
        assert "lifecycle: on-load missing" in proc.stdout, proc.stdout
        print("PASS missing lifecycle is blocked")

        # The name forms. The legacy 0x01 lead is still a plain name; an
        # `implements` name is the interface actually imported.
        legacy = root / "legacy.wasm"
        legacy.write_bytes(
            component(
                import_section(
                    [instance_import("pumpkin:plugin/logging@0.2.0", lead=0x01)]
                ),
                export_section([func_export("on-load", 0)]),
            )
        )
        assert json.loads(run(adapt_cli, [legacy, "--json"]).stdout)["status"] == "mapped"

        implements = root / "implements.wasm"
        implements.write_bytes(
            component(
                import_section(
                    [
                        b"\x02"
                        + name("local-logging")
                        + uleb(1)
                        + b"\x00"
                        + name("pumpkin:plugin/logging@0.2.0")
                        + b"\x05"
                        + uleb(0)
                    ]
                ),
                export_section([func_export("on-load", 0)]),
            )
        )
        implements_report = json.loads(run(adapt_cli, [implements, "--json"]).stdout)
        assert implements_report["status"] == "mapped", implements_report
        assert implements_report["imports"][0]["name"] == "pumpkin:plugin/logging@0.2.0", (
            implements_report
        )

        suffix = root / "suffix.wasm"
        suffix.write_bytes(
            component(
                import_section(
                    [
                        b"\x02"
                        + name("pumpkin:plugin/logging@0.2.0")
                        + uleb(1)
                        + b"\x01"
                        + name(".1")
                        + b"\x05"
                        + uleb(0)
                    ]
                ),
                export_section([func_export("on-load", 0)]),
            )
        )
        text = stubbed(suffix, "pumpkin:plugin/logging@0.2.0.1")
        # A version suffix must not silently bind to the tracked interface;
        # it is an unlisted name and gets the disclosed stub.
        assert "plusmc:host/log_level" not in text, text
        print("PASS name forms")

        def unreadable(path, needle):
            proc = run(adapt_cli, [path], expect=1)
            assert needle in proc.stdout or needle in proc.stderr, (
                f"expected {needle!r}:\n{proc.stdout}\n{proc.stderr}"
            )
            return proc

        bad_name = root / "bad-name.wasm"
        bad_name.write_bytes(
            component(
                import_section([b"\x03" + name("x") + b"\x05\x00"]),
                export_section([func_export("on-load", 0)]),
            )
        )
        unreadable(bad_name, "name uses unknown kind 3")

        bad_kind = root / "bad-kind.wasm"
        bad_kind.write_bytes(
            component(
                import_section([extern("x") + b"\x06\x00"]),
                export_section([func_export("on-load", 0)]),
            )
        )
        unreadable(bad_kind, "unknown component external kind 6")

        bad_attribute = root / "bad-attribute.wasm"
        bad_attribute.write_bytes(
            component(
                import_section(
                    [b"\x02" + name("x") + uleb(1) + b"\x03" + name("y") + b"\x05\x00"]
                ),
                export_section([func_export("on-load", 0)]),
            )
        )
        unreadable(bad_attribute, "name attribute uses unknown kind 3")

        duplicate = root / "duplicate.wasm"
        duplicate.write_bytes(
            component(
                import_section(
                    [
                        instance_import("pumpkin:plugin/logging@0.2.0"),
                        instance_import("pumpkin:plugin/logging@0.2.0"),
                    ]
                ),
                export_section([func_export("on-load", 0)]),
            )
        )
        unreadable(duplicate, "declared twice")

        duplicate_export = root / "duplicate-export.wasm"
        duplicate_export.write_bytes(
            component(
                import_section([instance_import("pumpkin:plugin/logging@0.2.0")]),
                export_section([func_export("on-load", 0), func_export("on-load", 1)]),
            )
        )
        unreadable(duplicate_export, "declared twice")

        unknown_section = root / "unknown-section.wasm"
        unknown_section.write_bytes(
            component(section(13, b""), export_section([func_export("on-load", 0)]))
        )
        unreadable(unknown_section, "unknown component section id 13")

        past_end = root / "past-end.wasm"
        past_end.write_bytes(component(b"\x0a" + uleb(20) + b"\x00"))
        unreadable(past_end, "runs past the file")

        truncated = root / "truncated.wasm"
        truncated.write_bytes(component(section(10, b"\x01\x00\x05ab")))
        unreadable(truncated, "runs past the section")

        bad_optional = root / "bad-optional.wasm"
        bad_optional.write_bytes(
            component(
                import_section([instance_import("pumpkin:plugin/logging@0.2.0")]),
                section(11, uleb(1) + extern("on-load") + b"\x01\x00\x02"),
            )
        )
        unreadable(bad_optional, "unknown optional-type kind 2")

        bad_magic = root / "bad-magic.wasm"
        bad_magic.write_bytes(b"\x00asx" + COMPONENT_VERSION)
        unreadable(bad_magic, "bad magic")
        print("PASS structural refusals")

        # A package is resolved by the gate, so the manifest fields appear in
        # the adapter report without a second parser.
        package = root / "package"
        package.mkdir()
        (package / "+mc-wasm.toml").write_text(
            'name = "Claims"\nversion = "0.1.0"\napi = 1\nmodule = "claims.wasm"\n',
            encoding="utf-8",
        )
        (package / "claims.wasm").write_bytes(good_bytes)
        packaged = json.loads(run(adapt_cli, [package, "--json"]).stdout)
        assert packaged["ok"] is True and packaged["status"] == "mapped", packaged
        assert packaged["package"] == {"name": "Claims", "version": "0.1.0", "api": 1}, packaged
        assert Path(packaged["module"]["file"]).resolve() == (package / "claims.wasm").resolve()
        assert "package: Claims 0.1.0, api 1" in run(adapt_cli, [package]).stdout
        print("PASS package directory")

        core = root / "core.wasm"
        core.write_bytes(core_module())
        core_report = json.loads(run(adapt_cli, [core, "--json"]).stdout)
        assert core_report["ok"] is True, core_report
        assert core_report["status"] == "not-component", core_report
        assert core_report["module"]["kind"] == "core", core_report
        run(adapt_cli, [core, "--require-mapped"], expect=1)
        print("PASS core module is not a component")

        empty = run(adapt_cli, [], expect=2)
        assert "usage:" in empty.stdout, empty.stdout
        assert "unknown option" in run(adapt_cli, [good, "--nope"], expect=2).stderr
        assert "one module or package" in run(adapt_cli, [good, good], expect=2).stderr
        assert run(adapt_cli, [root / "absent.wasm"], expect=1).returncode == 1
        print("PASS usage refusals")

    print("wasm-component: all checks passed")


if __name__ == "__main__":
    main()
