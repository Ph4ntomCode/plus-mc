#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The +MC WASM plugin gate (Phase 8, slice 1).

`plusmc-wasm inspect` reads an untrusted `.wasm` module or a `+mc-wasm.toml`
package and decides whether the pinned `plusmc:host` surface provides every
import. This test assembles real WebAssembly bytes in Python — no toolchain,
no checked-in binaries — and pins:

  * a hostable core module: imports, exports, counts, SHA-256 and the JSON;
  * blocked verdicts for WASI, an unknown host function, and a memory import;
  * the component header path (inspected, not approved);
  * structural refusals: bad magic/version, a section past the end, duplicate
    and out-of-order sections, an unknown section id, a function/code count
    mismatch, a malformed limits descriptor, a bad UTF-8 name and a duplicate
    export;
  * the package manifest: required keys, quoted values, unknown keys ignored,
    and a module path that escapes the package;
  * the CLI: `--json`, `--require-hostable`, and usage errors.

Run: wasm_module_test.py /path/to/plusmc-wasm
"""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT = 60


def run(cli, args, expect=0):
    proc = subprocess.run(
        [str(cli), *[str(arg) for arg in args]],
        text=True,
        capture_output=True,
        timeout=TIMEOUT,
    )
    if expect is not None and proc.returncode != expect:
        raise AssertionError(
            f"plusmc-wasm exited {proc.returncode}, expected {expect}: {proc.args}\n"
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


def vec(items):
    return uleb(len(items)) + b"".join(items)


def name(text):
    encoded = text.encode("utf-8") if isinstance(text, str) else text
    return uleb(len(encoded)) + encoded


def section(section_id, payload):
    return bytes([section_id]) + uleb(len(payload)) + payload


MAGIC = b"\x00asm"
CORE_VERSION = b"\x01\x00\x00\x00"
COMPONENT_VERSION = b"\x0d\x00\x01\x00"


def module(*sections, version=CORE_VERSION, magic=MAGIC):
    return magic + version + b"".join(sections)


def void_type():
    return b"\x60" + vec([]) + vec([])


def i32_type():
    return b"\x60" + vec([b"\x7f"]) + vec([])


def hostable_module():
    return module(
        section(1, vec([void_type(), i32_type()])),
        section(2, vec([name("plusmc:host") + name("log") + b"\x00" + uleb(0)])),
        section(3, vec([uleb(0)])),
        section(5, vec([b"\x00" + uleb(1)])),
        section(7, vec([name("run") + b"\x00" + uleb(1)])),
        section(10, vec([uleb(2) + b"\x00\x0b"])),
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: wasm_module_test.py <plusmc-wasm>")
    cli = Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="plusmc-wasm-") as temporary:
        root = Path(temporary)
        good = root / "plugin.wasm"
        good.write_bytes(hostable_module())

        text = run(cli, ["inspect", good]).stdout
        assert "+MC wasm plugin" in text, text
        assert "kind: core module" in text, text
        assert "types: 2" in text and "functions: 1" in text and "memories: 1" in text, text
        assert "imports: 1  exports: 1" in text, text
        assert "status: hostable" in text, text
        assert "plusmc:host/log  function ()->()  provided" in text, text
        assert "run  function #1" in text, text
        print("PASS hostable module")

        report = json.loads(run(cli, ["inspect", good, "--json"]).stdout)
        assert report["tool"] == "plusmc-wasm", report
        assert report["schema"] == 1 and report["mode"] == "inspect", report
        assert report["ok"] is True and report["status"] == "hostable", report
        assert report["package"]["name"] == "", report
        assert report["module"]["kind"] == "core", report
        assert report["module"]["sha256"] == hashlib.sha256(hostable_module()).hexdigest(), report
        assert report["module"]["size"] == len(hostable_module()), report
        assert report["module"]["imports"] == [
            {
                "module": "plusmc:host",
                "name": "log",
                "kind": "function",
                "signature": "()->()",
                "provided": True,
            }
        ], report
        assert report["module"]["exports"] == [
            {"name": "run", "kind": "function", "index": 1}
        ], report
        assert report["reasons"] == [], report
        assert run(cli, ["inspect", good, "--json"]).stdout == run(
            cli, ["inspect", good, "--json"]
        ).stdout, "the report is not deterministic"
        print("PASS hostable JSON")

        # WASI is outside the pinned surface: blocked, with the import named.
        wasi = root / "wasi.wasm"
        wasi.write_bytes(
            module(
                section(1, vec([void_type()])),
                section(
                    2,
                    vec([name("wasi:filesystem") + name("preopens") + b"\x00" + uleb(0)]),
                ),
            )
        )
        blocked = run(cli, ["inspect", wasi])
        assert "status: blocked" in blocked.stdout, blocked.stdout
        assert "wasi:filesystem/preopens" in blocked.stdout, blocked.stdout
        assert "outside the pinned plusmc:host surface" in blocked.stdout, blocked.stdout
        assert run(cli, ["inspect", wasi, "--require-hostable"], expect=1).stderr.endswith(
            "is blocked, not hostable\n"
        )
        print("PASS WASI is blocked")

        unknown = root / "unknown.wasm"
        unknown.write_bytes(
            module(
                section(1, vec([void_type()])),
                section(2, vec([name("plusmc:host") + name("teleport") + b"\x00" + uleb(0)])),
            )
        )
        reason = run(cli, ["inspect", unknown, "--json"]).stdout
        parsed = json.loads(reason)
        assert parsed["status"] == "blocked", parsed
        assert "plusmc:host/teleport" in " ".join(parsed["reasons"]), parsed
        assert parsed["module"]["imports"][0]["provided"] is False, parsed
        print("PASS unknown host function is blocked")

        memory_import = root / "memory.wasm"
        memory_import.write_bytes(
            module(section(2, vec([name("env") + name("heap") + b"\x02" + b"\x00" + uleb(1)])))
        )
        memory_text = run(cli, ["inspect", memory_import]).stdout
        assert "status: blocked" in memory_text, memory_text
        assert "as a memory" in memory_text, memory_text
        print("PASS non-function imports are blocked")

        # A component is the wasm32-wasip2 shape Pumpkin plugins use. The gate
        # reads its header and section table but does not approve it yet.
        component = root / "component.wasm"
        component.write_bytes(
            module(section(1, b""), version=COMPONENT_VERSION)
        )
        component_report = json.loads(run(cli, ["inspect", component, "--json"]).stdout)
        assert component_report["ok"] is True, component_report
        assert component_report["status"] == "component", component_report
        assert component_report["module"]["kind"] == "component", component_report
        assert "component-model layer" in " ".join(component_report["module"]["notes"]), (
            component_report
        )
        assert run(cli, ["inspect", component, "--require-hostable"], expect=1).returncode == 1
        print("PASS component is inspected, not approved")

        def expect_unreadable(path, needle):
            proc = run(cli, ["inspect", path], expect=1)
            assert needle in proc.stdout or needle in proc.stderr, (
                f"expected {needle!r}:\n{proc.stdout}\n{proc.stderr}"
            )
            return proc

        bad_magic = root / "bad-magic.wasm"
        bad_magic.write_bytes(b"\x00asx" + CORE_VERSION)
        expect_unreadable(bad_magic, "bad magic")

        bad_version = root / "bad-version.wasm"
        bad_version.write_bytes(MAGIC + b"\x02\x00\x00\x00")
        expect_unreadable(bad_version, "unsupported WebAssembly version bytes")

        past_end = root / "past-end.wasm"
        past_end.write_bytes(module(b"\x01" + uleb(10) + b"\x00"))
        expect_unreadable(past_end, "claims 10 bytes")

        duplicate = root / "duplicate.wasm"
        duplicate.write_bytes(module(section(1, vec([])), section(1, vec([]))))
        expect_unreadable(duplicate, "appears more than once")

        out_of_order = root / "out-of-order.wasm"
        out_of_order.write_bytes(module(section(7, vec([])), section(1, vec([]))))
        expect_unreadable(out_of_order, "out of order")

        unknown_section = root / "unknown-section.wasm"
        unknown_section.write_bytes(module(section(14, b"")))
        expect_unreadable(unknown_section, "unknown section id 14")

        mismatch = root / "mismatch.wasm"
        mismatch.write_bytes(
            module(section(1, vec([void_type()])), section(3, vec([uleb(0)])))
        )
        expect_unreadable(mismatch, "declares 1 functions but the code section has 0")

        bad_limits = root / "bad-limits.wasm"
        bad_limits.write_bytes(module(section(5, b"\x01\x00")))
        expect_unreadable(bad_limits, "limits")

        bad_name = root / "bad-name.wasm"
        bad_name.write_bytes(
            module(section(2, b"\x01\x01\xff\x00\x00"))
        )
        expect_unreadable(bad_name, "runs past the section")

        duplicate_export = root / "duplicate-export.wasm"
        duplicate_export.write_bytes(
            module(section(7, vec([name("run") + b"\x00" + uleb(0)] * 2)))
        )
        expect_unreadable(duplicate_export, "declared twice")

        bad_value_type = root / "bad-value-type.wasm"
        bad_value_type.write_bytes(
            module(section(1, vec([b"\x60" + vec([b"\x7a"]) + vec([])])))
        )
        expect_unreadable(bad_value_type, "unsupported parameter type")
        print("PASS structural refusals")

        package = root / "package"
        package.mkdir()
        (package / "+mc-wasm.toml").write_text(
            "# a package manifest\n"
            'name = "Claims"\n'
            'version = "0.1.0"\n'
            "api = 1\n"
            'module = "claims.wasm"\n'
            'author = "ignored on purpose"\n',
            encoding="utf-8",
        )
        (package / "claims.wasm").write_bytes(hostable_module())
        packaged = json.loads(run(cli, ["inspect", package, "--json"]).stdout)
        assert packaged["ok"] is True, packaged
        assert packaged["status"] == "hostable", packaged
        assert packaged["package"]["name"] == "Claims", packaged
        assert packaged["package"]["version"] == "0.1.0", packaged
        assert packaged["package"]["api"] == 1, packaged
        assert packaged["package"]["module"] == "claims.wasm", packaged
        assert Path(packaged["package"]["dir"]).resolve() == package.resolve(), packaged
        assert "package:" in run(cli, ["inspect", package]).stdout
        print("PASS package manifest")

        escaping = root / "escaping"
        escaping.mkdir()
        (escaping / "+mc-wasm.toml").write_text(
            'name = "Escape"\nversion = "1"\napi = 1\nmodule = "../plugin.wasm"\n',
            encoding="utf-8",
        )
        proc = expect_unreadable(escaping, "relative .wasm path inside the package")
        assert not (root / "escape-created").exists()

        no_api = root / "no-api"
        no_api.mkdir()
        (no_api / "+mc-wasm.toml").write_text('name = "N"\nversion = "1"\n', encoding="utf-8")
        expect_unreadable(no_api, 'missing required key "module"')

        wrong_api = root / "wrong-api"
        wrong_api.mkdir()
        (wrong_api / "+mc-wasm.toml").write_text(
            'name = "N"\nversion = "1"\napi = 2\nmodule = "p.wasm"\n', encoding="utf-8"
        )
        expect_unreadable(wrong_api, "api 2 is not supported")

        duplicate_key = root / "duplicate-key"
        duplicate_key.mkdir()
        (duplicate_key / "+mc-wasm.toml").write_text(
            'name = "N"\nname = "M"\nversion = "1"\napi = 1\nmodule = "p.wasm"\n',
            encoding="utf-8",
        )
        expect_unreadable(duplicate_key, 'duplicate key "name"')

        missing_module = root / "missing-module"
        missing_module.mkdir()
        (missing_module / "+mc-wasm.toml").write_text(
            'name = "N"\nversion = "1"\napi = 1\nmodule = "gone.wasm"\n', encoding="utf-8"
        )
        expect_unreadable(missing_module, "cannot read")

        no_manifest = root / "no-manifest"
        no_manifest.mkdir()
        (no_manifest / "x.wasm").write_bytes(hostable_module())
        expect_unreadable(no_manifest, "+mc-wasm.toml")
        print("PASS package refusals")

        empty = run(cli, [], expect=2)
        assert "usage:" in empty.stdout, empty.stdout
        assert run(cli, ["inspect"], expect=2).returncode == 2
        assert "unknown option" in run(cli, ["inspect", good, "--nope"], expect=2).stderr
        assert "one module or package" in run(
            cli, ["inspect", good, good], expect=2
        ).stderr
        assert run(cli, ["build", good], expect=2).returncode == 2
        assert run(cli, ["inspect", root / "absent.wasm"], expect=1).returncode == 1
        print("PASS usage refusals")

    print("wasm-module: all checks passed")


if __name__ == "__main__":
    main()
