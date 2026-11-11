#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The +MC WASM runtime (Phase 8, slice 2).

`plusmc-wasm run` executes a hostable module's lifecycle exports under four
budgets. This test assembles core modules in Python and also runs a
compiler-produced Rust module that exercises tables, element segments, indirect
calls and bulk memory. It pins:

  * a completed run: `log` reaches the host through module memory, and the
    lifecycle results, step count and JSON are reported;
  * host world and config calls round-tripping through the host interface;
  * the instruction, wall-clock, call-depth and memory budgets;
  * traps: out-of-bounds loads, division by zero and an invalid conversion;
  * tables, element segments, indirect calls, bulk memory and saturating
    conversions in the compiler-produced Rust path;
  * refusals: a non-zero load, a missing lifecycle export, a WASI import the
    gate blocks, and a data segment outside the initial memory;
  * CLI usage errors and `--json` determinism.

Run: wasm_runtime_test.py /path/to/plusmc-wasm
"""
from __future__ import annotations

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


def sleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if (value == 0 and not (byte & 0x40)) or (value == -1 and (byte & 0x40)):
            out.append(byte)
            return bytes(out)
        out.append(byte | 0x80)


def vec(items):
    return uleb(len(items)) + b"".join(items)


def name(text):
    encoded = text.encode("utf-8") if isinstance(text, str) else text
    return uleb(len(encoded)) + encoded


def section(section_id, payload):
    return bytes([section_id]) + uleb(len(payload)) + payload


def functype(params, results):
    return b"\x60" + vec(list(params)) + vec(list(results))


I32 = lambda value: b"\x41" + sleb(value)  # noqa: E731
CALL = lambda index: b"\x10" + uleb(index)  # noqa: E731
END = b"\x0b"
DROP = b"\x1a"

T_LOAD = functype([b"\x7f"], [b"\x7f"])
T_VOID_I32 = functype([], [b"\x7f"])
T_LOG = functype([b"\x7f", b"\x7f"], [])
T_SET = functype([b"\x7f"] * 4, [b"\x7f"])
T_GET = functype([b"\x7f"] * 3, [b"\x7f"])
T_REC = functype([b"\x7f"], [b"\x7f"])


def body(code):
    return uleb(len(code) + 1) + b"\x00" + code


def imp(module, field, type_index):
    return name(module) + name(field) + b"\x00" + uleb(type_index)


def exp(field, function_index):
    return name(field) + b"\x00" + uleb(function_index)


def data_payload(segments):
    entries = []
    for offset, blob in segments:
        entries.append(uleb(0) + b"\x41" + sleb(offset) + END + uleb(len(blob)) + blob)
    return vec(entries)


def build(types, imports, function_types, codes, exports, memory=1, data=(), tables=None):
    sections = [section(1, vec(types))]
    if imports:
        sections.append(section(2, vec(imports)))
    sections.append(section(3, vec([uleb(t) for t in function_types])))
    if tables is not None:
        sections.append(section(4, tables))
    if memory is not None:
        sections.append(section(5, b"\x01\x00" + uleb(memory)))
    sections.append(section(7, vec(exports)))
    sections.append(section(10, vec(codes)))
    if data:
        sections.append(section(11, data_payload(data)))
    return b"\x00asm\x01\x00\x00\x00" + b"".join(sections)


def lifecycle(load_code, enable_code=None, disable_code=None, types=None, imports=None,
              memory=1, data=(), extra_types=(), extra_function_types=(), extra_codes=(),
              tables=None, omit=()):
    """A module exporting load(i32)->i32, enable()->i32, disable()->i32.

    Defined functions come after the imports, so their indices are explicit.
    """
    types = list(types if types is not None else [T_LOAD, T_VOID_I32])
    imports = list(imports or [])
    function_types = [0, 1, 1] + list(extra_function_types)
    codes = [
        body(load_code),
        body(enable_code if enable_code is not None else I32(0) + END),
        body(disable_code if disable_code is not None else I32(0) + END),
    ] + list(extra_codes)
    first = len(imports)
    exports = []
    if "load" not in omit:
        exports.append(exp("pmc_wasm_load", first))
    if "enable" not in omit:
        exports.append(exp("pmc_wasm_enable", first + 1))
    if "disable" not in omit:
        exports.append(exp("pmc_wasm_disable", first + 2))
    types.extend(extra_types)
    return build(types, imports, function_types, codes, exports, memory=memory, data=data,
                 tables=tables)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: wasm_runtime_test.py <plusmc-wasm>")
    cli = Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="plusmc-wasm-runtime-") as temporary:
        root = Path(temporary)

        def write(name_, content):
            path = root / name_
            path.write_bytes(content)
            return path

        # 1. A completed lifecycle: the import is linked, the data segment is
        #    in memory, and `log` reads it through the host boundary.
        hello = write(
            "hello.wasm",
            lifecycle(
                I32(0) + I32(5) + CALL(0) + I32(0) + END,
                types=[T_LOAD, T_VOID_I32, T_LOG],
                imports=[imp("plusmc:host", "log", 2)],
                data=[(0, b"hello")],
            ),
        )
        completed = run(cli, ["run", hello])
        assert "status: completed" in completed.stdout, completed.stdout
        assert "log:\n  log: hello" in completed.stdout, completed.stdout
        assert "load=0 enable=0 disable=0" in completed.stdout, completed.stdout
        assert "steps: " in completed.stdout, completed.stdout
        print("PASS completed lifecycle and host log")

        report = json.loads(run(cli, ["run", hello, "--json"]).stdout)
        assert report["tool"] == "plusmc-wasm", report
        assert report["schema"] == 1 and report["mode"] == "run", report
        assert report["ok"] is True and report["status"] == "completed", report
        assert report["budget_exhausted"] is False, report
        assert report["lifecycle"] == {
            "load": {"ran": True, "result": 0},
            "enable": {"ran": True, "result": 0},
            "disable": {"ran": True, "result": 0},
        }, report
        assert report["log"] == ["log: hello"], report
        assert report["messages"] == [], report
        assert report["limits"]["steps"] == 1000000, report
        assert report["steps"] > 0, report
        assert run(cli, ["run", hello, "--json"]).stdout == run(
            cli, ["run", hello, "--json"]
        ).stdout, "the run report is not deterministic"
        print("PASS run JSON and determinism")

        # 2. World calls round-trip: set(1,2,3,42), then get(1,2,3) == 42.
        blocks = write(
            "blocks.wasm",
            lifecycle(
                I32(1) + I32(2) + I32(3) + I32(42) + CALL(0) + DROP
                + I32(1) + I32(2) + I32(3) + CALL(1) + I32(42) + b"\x46"
                + b"\x04\x40"
                + I32(0) + I32(5) + CALL(2)
                + b"\x05"
                + I32(6) + I32(8) + CALL(2)
                + END
                + I32(0) + END,
                types=[T_LOAD, T_VOID_I32, T_SET, T_GET, T_LOG],
                imports=[
                    imp("plusmc:host", "world_set_block", 2),
                    imp("plusmc:host", "world_get_block", 3),
                    imp("plusmc:host", "log", 4),
                ],
                data=[(0, b"match\x00mismatch")],
            ),
        )
        block_run = run(cli, ["run", blocks])
        assert "status: completed" in block_run.stdout, block_run.stdout
        assert "match" in block_run.stdout, block_run.stdout
        assert "mismatch" not in block_run.stdout, block_run.stdout
        print("PASS world_set_block/world_get_block")

        # 3. The instruction budget stops an infinite loop.
        loop = write(
            "loop.wasm",
            lifecycle(b"\x03\x40" + b"\x0c\x00" + END + I32(0) + END),
        )
        step_stop = run(cli, ["run", loop, "--max-steps", "100"], expect=1)
        assert "status: budget" in step_stop.stdout, step_stop.stdout
        assert "step budget exhausted (100 instructions)" in step_stop.stdout, step_stop.stdout
        stopped = json.loads(run(cli, ["run", loop, "--max-steps", "100", "--json"]).stdout)
        assert stopped["ok"] is False and stopped["budget_exhausted"] is True, stopped
        print("PASS instruction budget")

        # 4. The wall-clock budget stops the same loop even with fuel left.
        clock_stop = run(
            cli,
            ["run", loop, "--max-steps", "1000000000", "--max-millis", "1"],
            expect=1,
        )
        assert "wall-clock budget exhausted" in clock_stop.stdout, clock_stop.stdout
        print("PASS wall-clock budget")

        # 5. Out-of-bounds memory is a trap, not a read.
        oob = write(
            "oob.wasm",
            lifecycle(I32(100000) + b"\x28\x02\x00" + DROP + I32(0) + END),
        )
        oob_run = run(cli, ["run", oob], expect=1)
        assert "trap: out of bounds memory access" in oob_run.stdout, oob_run.stdout
        assert "status: trapped" in oob_run.stdout, oob_run.stdout
        print("PASS out-of-bounds trap")

        # 6. Division by zero and an invalid conversion trap with their reasons.
        divide = write(
            "divide.wasm",
            lifecycle(I32(1) + I32(0) + b"\x6d" + DROP + I32(0) + END),
        )
        division = run(cli, ["run", divide], expect=1)
        assert "integer divide by zero" in division.stdout, division.stdout

        conversion = write(
            "conversion.wasm",
            lifecycle(b"\x43\x00\x00\xc0\x7f" + b"\xa8" + DROP + I32(0) + END),
        )
        invalid = run(cli, ["run", conversion], expect=1)
        assert "invalid conversion to integer" in invalid.stdout, invalid.stdout
        print("PASS division and conversion traps")

        # 7. Call depth: a recursive helper stopped by a small frame budget.
        recursive = write(
            "recursive.wasm",
            lifecycle(
                I32(10000) + CALL(3) + DROP + I32(0) + END,
                types=[T_LOAD, T_VOID_I32, T_REC],
                extra_function_types=[2],
                extra_codes=[
                    body(
                        b"\x20\x00" + b"\x45" + b"\x04\x7f" + I32(0)
                        + b"\x05" + b"\x20\x00" + I32(1) + b"\x6b" + CALL(3) + END
                        + END
                    )
                ],
            ),
        )
        depth = run(cli, ["run", recursive, "--max-call-depth", "16"], expect=1)
        assert "call depth budget exhausted (16 frames)" in depth.stdout, depth.stdout
        print("PASS call-depth budget")

        # 8. Memory pages: the module's minimum is checked against the budget.
        large = write("large.wasm", lifecycle(I32(0) + END, memory=300))
        memory_stop = run(cli, ["run", large, "--max-memory-pages", "256"], expect=1)
        assert "memory" in memory_stop.stdout and "256" in memory_stop.stdout, (
            memory_stop.stdout
        )
        print("PASS memory budget")

        # 9. A non-zero load and a missing export are refusals with the reason.
        refused = write("refused.wasm", lifecycle(I32(7) + END))
        refused_run = run(cli, ["run", refused], expect=1)
        assert "pmc_wasm_load returned 7" in refused_run.stdout, refused_run.stdout
        assert "status: refused" in refused_run.stdout, refused_run.stdout

        missing = write("missing.wasm", lifecycle(I32(0) + END, omit=("enable",)))
        missing_run = run(cli, ["run", missing], expect=1)
        assert "does not export pmc_wasm_enable" in missing_run.stdout, missing_run.stdout
        print("PASS lifecycle refusals")

        # 10. A table section is accepted even when the lifecycle does not use
        # it, and a saturating conversion (0xfc 0) never traps on a fraction.
        table = write("table.wasm", lifecycle(I32(0) + END, tables=b"\x01\x70\x00\x01"))
        table_run = run(cli, ["run", table])
        assert "status: completed" in table_run.stdout, table_run.stdout

        saturating = write(
            "saturating.wasm",
            lifecycle(b"\x43\x00\x00\xc0\x3f" + b"\xfc\x00" + DROP + I32(0) + END),
        )
        saturating_run = run(cli, ["run", saturating])
        assert "status: completed" in saturating_run.stdout, saturating_run.stdout

        wasi = write(
            "wasi.wasm",
            lifecycle(
                I32(0) + END,
                types=[T_LOAD, T_VOID_I32, T_LOG],
                imports=[imp("wasi:filesystem", "preopens", 2)],
            ),
        )
        wasi_run = run(cli, ["run", wasi], expect=1)
        assert "not hostable" in wasi_run.stdout, wasi_run.stdout
        assert "wasi:filesystem" in wasi_run.stdout, wasi_run.stdout

        outside = write(
            "outside.wasm",
            lifecycle(I32(0) + END, data=[(65536, b"x")]),
        )
        data_run = run(cli, ["run", outside], expect=1)
        assert "data segment" in data_run.stdout, data_run.stdout
        print("PASS feature refusals")

        # 11. A compiler-produced Rust module: its table, element segment and
        # indirect calls execute, and its memory.copy/memory.fill bulk opcodes
        # produce the formatted host messages.
        rust_module = Path(__file__).resolve().parent / "fixtures" / "wasm" / "rust_lifecycle.wasm"
        rust_run = run(cli, ["run", rust_module, "--json"])
        rust = json.loads(rust_run.stdout)
        assert rust["status"] == "completed", rust
        assert rust["log"] == [
            "log: rust load 2 2016",
            "log: rust enable",
            "log: rust disable",
        ], rust
        assert rust["steps"] > 500, rust
        print("PASS compiler-produced Rust tables, indirect calls and bulk memory")

        # 12. The CLI's usage rules.
        assert run(cli, [], expect=2).returncode == 2
        assert run(cli, ["run"], expect=2).returncode == 2
        assert "unknown option" in run(cli, ["run", hello, "--nope"], expect=2).stderr
        assert "one module" in run(cli, ["run", hello, hello], expect=2).stderr
        assert "--max-steps needs a number" in run(
            cli, ["run", hello, "--max-steps"], expect=2
        ).stderr
        assert "out of range" in run(
            cli, ["run", hello, "--max-steps", "1000000001"], expect=2
        ).stderr
        assert "out of range" in run(
            cli, ["run", hello, "--max-call-depth", "100001"], expect=2
        ).stderr
        print("PASS CLI usage refusals")

    print("wasm-runtime: all checks passed")


if __name__ == "__main__":
    main()
