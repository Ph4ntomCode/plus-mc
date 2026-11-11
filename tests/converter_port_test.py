#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The +MC Converter porter (Phase 7, slice 3).

`plusmc-convert port` turns a plugin's manifest and mapping into a native
project scaffold plus a machine-readable diff of what a human must finish. This
test builds a real JAR in Python (importing `converter_test` so the fixture
writer cannot drift), then pins:

  * the generated manifest, source, CMake project and README;
  * the command and event stubs and their registration in `on_load`;
  * the work list: command stubs, permission gaps, partial and gap references;
  * that the generated source compiles against the real ABI with the same
    compiler that built the engine (warning-clean), so "it generates code"
    cannot drift from "the code builds";
  * deterministic output, and refusals for a missing manifest, a non-empty
    output directory, a file in the output path and a missing `--out`.

Run: converter_port_test.py /path/to/plusmc-convert /path/to/cxx /path/to/plusmc /path/to/cmake
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import converter_test  # noqa: E402

TIMEOUT = 120

MANIFEST = """name: Example Port
version: 1.2.3
main: com.example.Plugin
api-version: '1.20'
author: Tester
description: Says "hi"
commands:
  greet:
    description: Say hi
  help:
    description: Help
permissions:
  example.greet:
    default: true
"""

UPDATED_MANIFEST = """name: Example Port
version: 2.0.0
main: com.example.Plugin
api-version: '1.20'
author: Tester
description: Says "hi"
commands:
  greet:
    description: Say hi
  help:
    description: Help
  wave:
    description: Wave
permissions:
  example.greet:
    default: true
"""


def run(cli, args, expect=0):
    proc = subprocess.run(
        [str(cli), *[str(arg) for arg in args]],
        text=True,
        capture_output=True,
        timeout=TIMEOUT,
    )
    if expect is not None and proc.returncode != expect:
        raise AssertionError(
            f"plusmc-convert exited {proc.returncode}, expected {expect}: {proc.args}\n"
            f"stdout:\n{proc.stdout[-6000:]}\nstderr:\n{proc.stderr[-6000:]}"
        )
    return proc


def compile_scaffold(compiler, source_root, source, output):
    """Compile one generated source against the real plugin ABI."""
    name = Path(compiler).name.lower()
    if os.name == "nt":
        # MinGW appends `.dll` to a suffix-less `-shared -o` path (and has
        # written the bare path in other releases); naming the module
        # explicitly keeps the compile and the existence check in agreement.
        output = output.with_name(output.name + ".dll")
    if name in ("cl", "cl.exe") or name.startswith("clang-cl"):
        args = [
            compiler,
            "/nologo",
            "/std:c++20",
            "/EHsc",
            "/LD",
            f"/I{source_root / 'src'}",
            str(source),
            f"/Fe:{output}",
        ]
    else:
        args = [
            compiler,
            "-std=c++20",
            "-shared",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{source_root / 'src'}",
            str(source),
            "-o",
            str(output),
        ]
        # MinGW's g++ warns that -fPIC is ignored (PE is position independent),
        # and -Werror would turn that warning into a failed scaffold build.
        if os.name != "nt":
            args.insert(2, "-fPIC")
    # Run from the output directory: a toolchain that treats `-o` as a bare
    # name then still writes beside the module the test is about to inspect.
    proc = subprocess.run(
        args,
        text=True,
        capture_output=True,
        timeout=TIMEOUT,
        cwd=str(output.parent),
    )
    if proc.returncode != 0:
        raise AssertionError(
            "the generated scaffold did not compile:\n"
            f"{' '.join(str(arg) for arg in args)}\n"
            f"stdout:\n{proc.stdout[-6000:]}\nstderr:\n{proc.stderr[-6000:]}"
        )
    # The other toolchains write exactly the path they were given; MinGW has
    # both appended `.dll` and written the bare path across releases. Accept
    # either, and any same-stem sibling the linker may have named itself.
    candidates = [output, output.with_name(output.name + ".dll")]
    candidates += sorted(output.parent.glob(output.stem + "*"))
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    listing = sorted(path.name for path in output.parent.iterdir())
    raise AssertionError(
        f"the compiler wrote no module at {output}; directory holds {listing}"
    )


def build_with_cmake(cmake, source_root, project_dir):
    """Build the scaffold the way its README tells a user to."""
    build = project_dir / "build"
    configure = [
        cmake,
        "-S",
        str(project_dir),
        "-B",
        str(build),
        f"-DPLUSMC_ROOT={source_root}",
    ]
    proc = subprocess.run(configure, text=True, capture_output=True, timeout=TIMEOUT)
    if proc.returncode != 0:
        raise AssertionError(
            "the generated CMake project did not configure:\n"
            f"{' '.join(configure)}\n{proc.stdout[-6000:]}\n{proc.stderr[-6000:]}"
        )
    compile_command = [cmake, "--build", str(build)]
    proc = subprocess.run(compile_command, text=True, capture_output=True, timeout=TIMEOUT)
    if proc.returncode != 0:
        raise AssertionError(
            "the generated CMake project did not build:\n"
            f"{' '.join(compile_command)}\n{proc.stdout[-6000:]}\n{proc.stderr[-6000:]}"
        )
    modules = [
        path
        for pattern in ("example_port.dylib", "example_port.so", "example_port.dll")
        for path in build.rglob(pattern)
    ]
    assert modules, f"no plugin module under {build}"
    return modules[0]


def fixture_jar(path, manifest=MANIFEST, extra_classes=()):
    plugin = converter_test.class_bytes(
        "com/example/Plugin",
        super_name="org/bukkit/plugin/java/JavaPlugin",
        interfaces=["org/bukkit/event/Listener", "org/bukkit/command/CommandExecutor"],
        classes=[
            "org/bukkit/event/player/PlayerJoinEvent",
            "org/bukkit/event/player/AsyncPlayerChatEvent",
            "org/bukkit/World",
            "org/bukkit/inventory/Inventory",
            *extra_classes,
        ],
        methods=[
            (
                "org/bukkit/plugin/java/JavaPlugin",
                "getConfig",
                "()Lorg/bukkit/configuration/FileConfiguration;",
            ),
            (
                "org/bukkit/World",
                "spawn",
                "(Lorg/bukkit/Location;Ljava/lang/Class;)Lorg/bukkit/entity/Entity;",
            ),
        ],
    )
    return converter_test.make_jar(
        path, {"com/example/Plugin.class": plugin}, manifest
    )


def main():
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: converter_port_test.py <plusmc-convert> <cxx> <source-root> <cmake>"
        )
    cli = Path(sys.argv[1]).resolve()
    compiler = sys.argv[2]
    source_root = Path(sys.argv[3]).resolve()
    cmake = sys.argv[4]

    with tempfile.TemporaryDirectory(prefix="plusmc-converter-port-") as temporary:
        root = Path(temporary)
        jar = fixture_jar(root / "example.jar")

        out = root / "scaffold"
        text = run(cli, ["port", jar, "--out", out]).stdout
        assert "plugin: Example Port 1.2.3 (example_port)" in text, text
        # The fixture names a wider Bukkit API (Inventory), exactly like the
        # mapper's fixture, so the analyzer's yellow score travels through.
        assert "score: yellow" in text, text
        assert "commands: 2 generated, permissions: 1, events: 2" in text, text
        assert "files: 5" in text, text
        assert "command  greet [generated]" in text, text
        assert "permission  example.greet [gap]" in text, text
        assert "event  org/bukkit/event/player/PlayerJoinEvent" in text, text
        assert "reference  org/bukkit/World#spawn" in text, text
        print("PASS port report")

        manifest = (out / "+mc-plugin.toml").read_text(encoding="utf-8")
        assert 'name = "Example Port"' in manifest, manifest
        assert 'version = "1.2.3"' in manifest, manifest
        assert "api = 1" in manifest, manifest
        assert 'library = "example_port"' in manifest, manifest
        assert 'entry = "pmc_plugin_entry"' in manifest, manifest
        print("PASS native manifest")

        source = (out / "example_port.cpp").read_text(encoding="utf-8")
        assert '#include "pluginapi/plugin.h"' in source, source
        assert "PMC_EVENT_PLAYER_JOIN" in source, source
        assert "PMC_EVENT_PLAYER_CHAT" in source, source
        assert "void on_player_join(void* data, pmc_event* event)" in source, source
        assert "PMC_EVENT_PLAYER_LEAVE" not in source, source
        assert 'api->command(api->host, "greet", "/greet", command_greet, state);' in source, source
        assert 'api->command(api->host, "help", "/help", command_help, state);' in source, source
        # A generated handler is real code, not a placeholder: it logs the
        # invocation and answers with a usage line, and the source carries no
        # TODO or "not implemented" text anywhere.
        assert 'std::string usage = "usage: /";' in source, source
        assert "state->api->reply(state->api->host, command->reply, usage.c_str());" in source, source
        assert "PMC_LOG_DEBUG" in source, source
        assert "TODO" not in source, source
        assert "not implemented" not in source, source
        assert 'Says \\"hi\\"' in source, source
        assert "PMC_PLUGIN_ABI_VERSION" in source, source
        handler_marker = (
            "// +mc-convert:handler-marker: new handlers are inserted above this line."
        )
        registration_marker = (
            "  // +mc-convert:registration-marker: new registrations are inserted above this line."
        )
        assert source.count(handler_marker) == 1, source
        assert source.count(registration_marker) == 1, source
        print("PASS scaffold source and insertion markers")

        cmake_text = (out / "CMakeLists.txt").read_text(encoding="utf-8")
        assert "add_library(example_port MODULE example_port.cpp)" in cmake_text, cmake_text
        assert (
            "SUFFIX \".dylib\"" in cmake_text and "SUFFIX \".so\"" in cmake_text
        ), cmake_text
        assert "SUFFIX \".dll\"" in cmake_text, cmake_text
        readme = (out / "README.md").read_text(encoding="utf-8")
        assert "# Example Port — +MC port scaffold" in readme, readme
        assert "port.json" in readme and "not a port" in readme, readme
        print("PASS build project and README")

        report = json.loads((out / "port.json").read_text(encoding="utf-8"))
        assert report["ok"] is True, report
        assert report["mode"] == "port", report
        assert report["plugin"] == {
            "name": "Example Port",
            "version": "1.2.3",
            "identifier": "example_port",
        }, report
        summary = report["summary"]
        assert summary["commands"] == 2, summary
        assert summary["permissions"] == 1, summary
        assert summary["events"] == 2, summary
        assert summary["mapped"] > 0 and summary["partial"] > 0, summary
        assert summary["gap"] > 0, summary
        work = report["work"]
        command = next(item for item in work if item["kind"] == "command" and item["reference"] == "greet")
        assert command["status"] == "generated", command
        permission = next(item for item in work if item["kind"] == "permission")
        assert permission["reference"] == "example.greet", permission
        assert permission["status"] == "gap", permission
        event = next(item for item in work if item["kind"] == "event")
        assert event["reference"] == "org/bukkit/event/player/PlayerJoinEvent", event
        spawn = next(item for item in work if item["kind"] == "reference" and "World#spawn" in item["reference"])
        assert spawn["status"] == "gap", spawn
        config = next(item for item in work if "JavaPlugin#getConfig" in item["reference"])
        assert config["status"] == "partial", config
        assert any("no ABI equivalent" in note for note in report["notes"]), report
        print("PASS machine-readable work diff")

        # The generated source is not just text: it must build as the module the
        # host loads, against the real ABI, with the engine's own compiler.
        module = compile_scaffold(
            compiler, source_root, out / "example_port.cpp", root / "example_port_module"
        )
        assert module.exists(), module
        built = build_with_cmake(cmake, source_root, out)
        assert built.exists(), built
        print("PASS scaffold compiles against the ABI and its CMake project builds")

        # No timestamps and no per-run names: the same jar produces the same
        # files (port.json carries the output path, so it is excluded).
        second = root / "scaffold-again"
        run(cli, ["port", jar, "--out", second])
        for name in ("+mc-plugin.toml", "example_port.cpp", "CMakeLists.txt", "README.md"):
            assert (out / name).read_bytes() == (second / name).read_bytes(), name
        print("PASS deterministic output")

        # `--from-port` updates the derived files without touching a human's
        # source, CMake or README; handlers new since the scaffold become
        # manual work items instead of silently claiming a stub exists.
        source_path = out / "example_port.cpp"
        original_source = source_path.read_text(encoding="utf-8")
        edited = original_source.replace(
            "// The original manifest declared /greet.",
            "// The human wrote this handler.",
        )
        assert edited != original_source, "the fixture response text moved"
        source_path.write_text(edited, encoding="utf-8")
        cmake_before = (out / "CMakeLists.txt").read_bytes()
        readme_before = (out / "README.md").read_bytes()

        updated = fixture_jar(
            root / "updated.jar",
            UPDATED_MANIFEST,
            ["org/bukkit/event/player/PlayerQuitEvent"],
        )
        text = run(cli, ["port", updated, "--out", out, "--from-port"]).stdout
        assert "plugin: Example Port 2.0.0 (example_port)" in text, text
        assert (
            "update: rewrote +mc-plugin.toml and port.json; preserved example_port.cpp" in text
        ), text
        assert "inserted: 2 handler(s) into example_port.cpp at the generated marker" in text, text
        assert "commands: 3 declared, permissions: 1, events: 3" in text, text
        assert "command  greet [preserved]" in text, text
        assert "command  wave [inserted]" in text, text
        assert "event  org/bukkit/event/player/PlayerQuitEvent [inserted]" in text, text
        updated_source = source_path.read_text(encoding="utf-8")
        assert "// The human wrote this handler." in updated_source, updated_source
        assert "TODO" not in updated_source, updated_source
        assert "void command_wave(void* data, pmc_command* command)" in updated_source
        assert 'api->command(api->host, "wave", "/wave", command_wave, state);' in updated_source
        assert "void on_player_leave(void* data, pmc_event* event)" in updated_source
        assert "PMC_EVENT_PLAYER_LEAVE" in updated_source
        assert updated_source.count(handler_marker) == 1, updated_source
        assert updated_source.count(registration_marker) == 1, updated_source
        # Every line the human had is still there, in order; only the two
        # generated blocks around the markers were added.
        remaining = updated_source.splitlines()
        position = 0
        for line in edited.splitlines():
            while position < len(remaining) and remaining[position] != line:
                position += 1
            assert position < len(remaining), f"line was removed or rewritten: {line!r}"
            position += 1
        assert (out / "CMakeLists.txt").read_bytes() == cmake_before
        assert (out / "README.md").read_bytes() == readme_before
        manifest = (out / "+mc-plugin.toml").read_text(encoding="utf-8")
        assert 'version = "2.0.0"' in manifest, manifest
        compile_scaffold(
            compiler, source_root, source_path, root / "example_port_updated_module"
        )
        print("PASS --from-port inserts new handlers and preserves every human line")

        report = json.loads((out / "port.json").read_text(encoding="utf-8"))
        assert report["from_port"] is True, report
        assert "example_port.cpp" in report["preserved"], report
        assert report["plugin"]["version"] == "2.0.0", report
        assert report["summary"]["commands"] == 3, report
        assert report["summary"]["inserted"] == 2, report
        work = report["work"]
        greet = next(
            item for item in work if item["kind"] == "command" and item["reference"] == "greet"
        )
        assert greet["status"] == "preserved", greet
        wave = next(
            item for item in work if item["kind"] == "command" and item["reference"] == "wave"
        )
        assert wave["status"] == "inserted", wave
        quit_event = next(
            item
            for item in work
            if item["kind"] == "event" and "PlayerQuitEvent" in item["reference"]
        )
        assert quit_event["status"] == "inserted", quit_event
        assert any("inserted 2 new handler" in note for note in report["notes"]), report
        print("PASS update work diff and insertion")

        # The second update sees the inserted registrations and adds nothing;
        # from there the update is idempotent, including the report.
        source_before = source_path.read_bytes()
        run(cli, ["port", updated, "--out", out, "--from-port", "--json"])
        assert source_path.read_bytes() == source_before
        second_text = run(cli, ["port", updated, "--out", out, "--from-port"]).stdout
        assert "command  wave [preserved]" in second_text, second_text
        assert "inserted: " not in second_text, second_text
        manifest_before = (out / "+mc-plugin.toml").read_bytes()
        report_before = (out / "port.json").read_bytes()
        run(cli, ["port", updated, "--out", out, "--from-port", "--json"])
        assert (out / "+mc-plugin.toml").read_bytes() == manifest_before
        assert (out / "port.json").read_bytes() == report_before
        print("PASS update is deterministic after insertion")

        # A source generated before the markers existed cannot be inserted into
        # safely; it is left byte-identical and the new work stays manual.
        legacy = root / "legacy"
        run(cli, ["port", jar, "--out", legacy])
        legacy_source = legacy / "example_port.cpp"
        legacy_text = legacy_source.read_text(encoding="utf-8")
        assert handler_marker in legacy_text and registration_marker in legacy_text
        legacy_text = legacy_text.replace(handler_marker + "\n", "")
        legacy_text = legacy_text.replace(registration_marker + "\n", "")
        legacy_source.write_text(legacy_text, encoding="utf-8")
        legacy_before = legacy_source.read_bytes()
        text = run(cli, ["port", updated, "--out", legacy, "--from-port"]).stdout
        assert legacy_source.read_bytes() == legacy_before, "source changed without markers"
        assert "inserted: " not in text, text
        assert "command  wave [manual]" in text, text
        assert "event  org/bukkit/event/player/PlayerQuitEvent [manual]" in text, text
        assert "marker" in text, text
        legacy_report = json.loads((legacy / "port.json").read_text(encoding="utf-8"))
        assert legacy_report["summary"]["inserted"] == 0, legacy_report
        assert any("marker" in note for note in legacy_report["notes"]), legacy_report
        print("PASS --from-port falls back when the marker is missing")

        duplicate = root / "duplicate"
        run(cli, ["port", jar, "--out", duplicate])
        duplicate_source = duplicate / "example_port.cpp"
        duplicate_text = duplicate_source.read_text(encoding="utf-8")
        assert duplicate_text.count(handler_marker) == 1
        duplicate_source.write_text(
            duplicate_text.replace(handler_marker, handler_marker + "\n" + handler_marker),
            encoding="utf-8",
        )
        duplicate_before = duplicate_source.read_bytes()
        text = run(cli, ["port", updated, "--out", duplicate, "--from-port"]).stdout
        assert duplicate_source.read_bytes() == duplicate_before
        assert "command  wave [manual]" in text, text
        assert "marker" in text, text
        print("PASS --from-port refuses an ambiguous marker")

        fresh = root / "fresh"
        proc = run(cli, ["port", updated, "--out", fresh, "--from-port"], expect=1)
        assert "needs an existing port" in proc.stderr, proc.stderr
        assert not fresh.exists(), fresh

        not_a_port = root / "not-a-port"
        not_a_port.mkdir()
        (not_a_port / "keep.txt").write_text("keep", encoding="utf-8")
        proc = run(cli, ["port", updated, "--out", not_a_port, "--from-port"], expect=1)
        assert "no +mc-plugin.toml" in proc.stderr, proc.stderr

        renamed = fixture_jar(
            root / "renamed.jar",
            UPDATED_MANIFEST.replace("name: Example Port", "name: Renamed Port"),
        )
        manifest_before = (out / "+mc-plugin.toml").read_bytes()
        proc = run(cli, ["port", renamed, "--out", out, "--from-port"], expect=1)
        assert "identifier changed" in proc.stderr, proc.stderr
        assert (out / "+mc-plugin.toml").read_bytes() == manifest_before

        orphan = root / "orphan"
        orphan.mkdir()
        (orphan / "+mc-plugin.toml").write_text(
            'name = "Orphan"\nversion = "1.0.0"\napi = 1\nlibrary = "orphan"\n',
            encoding="utf-8",
        )
        orphan_jar = fixture_jar(
            root / "orphan.jar", MANIFEST.replace("name: Example Port", "name: Orphan")
        )
        proc = run(cli, ["port", orphan_jar, "--out", orphan, "--from-port"], expect=1)
        assert "needs the preserved source" in proc.stderr, proc.stderr

        proc = run(cli, ["port", updated, "--from-port"], expect=2)
        assert "usage:" in proc.stdout, proc.stdout
        print("PASS --from-port refusals")

        # Refusals: never write into a directory that already holds files, and
        # never create one for a jar that is not a plugin.
        occupied = root / "occupied"
        occupied.mkdir()
        keep = occupied / "keep.txt"
        keep.write_text("do not replace me", encoding="utf-8")
        proc = run(cli, ["port", jar, "--out", occupied], expect=1)
        assert "not empty" in proc.stderr, proc.stderr
        assert keep.read_text(encoding="utf-8") == "do not replace me", keep
        print("PASS refuses a non-empty output directory")

        plain = converter_test.make_jar(
            root / "plain.jar",
            {"com/example/Plugin.class": converter_test.class_bytes("com/example/Plugin")},
        )
        missing = root / "should-not-exist"
        proc = run(cli, ["port", plain, "--out", missing], expect=1)
        assert "no plugin.yml or paper-plugin.yml" in proc.stderr, proc.stderr
        assert not missing.exists(), missing
        print("PASS refuses a jar without a manifest")

        blocker = root / "blocker"
        blocker.write_text("a file, not a directory", encoding="utf-8")
        proc = run(cli, ["port", jar, "--out", blocker], expect=1)
        assert "not a directory" in proc.stderr, proc.stderr
        print("PASS refuses a file in the output path")

        proc = run(cli, ["port", jar], expect=2)
        assert "usage:" in proc.stdout, proc.stdout
        proc = run(cli, ["port", jar, "--out", root / "a", "--out", root / "b"], expect=2)
        assert "--out given more than once" in proc.stderr, proc.stderr
        print("PASS missing and repeated --out")

    print("converter-port: all checks passed")


if __name__ == "__main__":
    main()
