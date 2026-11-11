#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The +MC WASM interpreter's core (Phase 8, slice 2).

`wasm_runtime_test.py` walks the lifecycle, the host calls and the budgets.
This test pins the machine's numeric and stateful half, which that walk touches
only lightly. It assembles real core modules in Python — no toolchain, no
checked-in binaries — and pins:

  * locals, a counted loop, `br_table` (all three arms), `select`, and an `if`
    with a result;
  * i64 shifts, f64 arithmetic, `f32.nearest`'s round-half-to-even and
    `i32.clz`;
  * a memory round trip through `i32.store`/`i32.load` and `i32.store8`;
  * the `config_set`/`config_get` round trip through the CLI host, including
    the out-pointer-under-the-arguments calling shape;
  * a mutable global (read, written, read back) and the start section;
  * the step budget's exact overshoot (a per-instruction check), and the
    host-message budget.

Run: wasm_core_test.py /path/to/plusmc-wasm
"""
from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT = 60

MAGIC = b"\x00asm"
VERSION = b"\x01\x00\x00\x00"

I32 = b"\x7f"
I64 = b"\x7e"
F32 = b"\x7d"
F64 = b"\x7c"

DATA_BASE = 1024


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


# ---------------------------------------------------------------------------
# A minimal assembler
# ---------------------------------------------------------------------------


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
    more = True
    while more:
        byte = value & 0x7F
        value >>= 7
        if (value == 0 and not byte & 0x40) or (value == -1 and byte & 0x40):
            more = False
        else:
            byte |= 0x80
        out.append(byte)
    return bytes(out)


def vec(items):
    return uleb(len(items)) + b"".join(items)


def name(text):
    encoded = text.encode("utf-8") if isinstance(text, str) else text
    return uleb(len(encoded)) + encoded


def section(section_id, payload):
    return bytes([section_id]) + uleb(len(payload)) + payload


def ftype(params, results):
    return b"\x60" + vec(params) + vec(results)


# Instructions used below.
def i32(value):
    return b"\x41" + sleb(value)


def i64(value):
    return b"\x42" + sleb(value)


def f32(value):
    return b"\x43" + struct.pack("<f", value)


def f64(value):
    return b"\x44" + struct.pack("<d", value)


def lget(index):
    return b"\x20" + uleb(index)


def lset(index):
    return b"\x21" + uleb(index)


def gget(index):
    return b"\x23" + uleb(index)


def gset(index):
    return b"\x24" + uleb(index)


def call(index):
    return b"\x10" + uleb(index)


BLOCK = b"\x02\x40"
LOOP = b"\x03\x40"
IF = b"\x04\x40"
IF_RESULT = b"\x04" + I32
ELSE = b"\x05"
END = b"\x0b"
RETURN = b"\x0f"
DROP = b"\x1a"
SELECT = b"\x1b"
BR = b"\x0c"
BR_IF = b"\x0d"
BR_TABLE = b"\x0e"
I32_ADD = b"\x6a"
I32_MUL = b"\x6c"
I32_AND = b"\x71"
I32_EQ = b"\x46"
I32_NE = b"\x47"
I32_GE_U = b"\x4f"  # i32.ge_u (0x4d is le_u)
I32_CLZ = b"\x67"
I32_LOAD = b"\x28"
I32_STORE = b"\x36"
I32_STORE8 = b"\x3a"
I64_EQ = b"\x51"
I64_SHL = b"\x86"
F32_EQ = b"\x5b"
F32_NEAREST = b"\x90"
F64_ADD = b"\xa0"
F64_MUL = b"\xa2"
F64_EQ = b"\x61"

T_VOID = 0
T_I32_I32 = 1
T_LOG = 2
T_GET = 3
T_SET = 4
T_I32 = 5

HOST_TYPES = [
    ftype([], []),
    ftype([I32], [I32]),
    ftype([I32, I32], []),
    ftype([I32, I32, I32], [I32]),
    ftype([I32, I32, I32, I32], [I32]),
    ftype([], [I32]),
]


def data_pool(*pieces):
    blob = bytearray()
    offsets = []
    for piece in pieces:
        encoded = piece.encode("utf-8") if isinstance(piece, str) else piece
        offsets.append(len(blob))
        blob += encoded
    return bytes(blob), offsets


def build(types, imports, funcs, exports, memory=None, data=b"", globals_=None, start=None):
    """Assembles a core module.

    funcs: (type_index, [local value types], body bytes without the final end).
    """
    sections = [section(1, vec(types))]
    if imports:
        sections.append(
            section(
                2,
                vec(
                    [
                        name("plusmc:host") + name(import_name) + b"\x00" + uleb(type_index)
                        for import_name, type_index in imports
                    ]
                ),
            )
        )
    sections.append(section(3, vec([uleb(type_index) for type_index, _, _ in funcs])))
    if memory is not None:
        sections.append(section(5, vec([b"\x00" + uleb(memory)])))
    if globals_ is not None:
        sections.append(section(6, globals_))
    if exports:
        sections.append(
            section(
                7,
                vec([name(export_name) + b"\x00" + uleb(index) for export_name, index in exports]),
            )
        )
    if start is not None:
        sections.append(section(8, uleb(start)))
    bodies = []
    for _, locals_, body in funcs:
        local_decls = b"\x00"  # a body always carries its locals vector
        if locals_:
            groups = []
            for local_type in locals_:
                if groups and groups[-1][1] == local_type:
                    groups[-1][0] += 1
                else:
                    groups.append([1, local_type])
            local_decls = vec([uleb(count) + local_type for count, local_type in groups])
        inner = local_decls + body + END
        bodies.append(uleb(len(inner)) + inner)
    sections.append(section(10, vec(bodies)))
    if data:
        sections.append(
            section(
                11,
                vec([b"\x00" + b"\x41" + sleb(DATA_BASE) + END + uleb(len(data)) + data]),
            )
        )
    return MAGIC + VERSION + b"".join(sections)


def pick_body():
    """pick(i): br_table 0 1, default 2, answering 1, 2 or 3."""
    return (
        BLOCK
        + BLOCK
        + BLOCK
        + lget(0)
        + BR_TABLE
        + uleb(2)
        + uleb(0)
        + uleb(1)
        + uleb(2)
        + END
        + i32(1)
        + RETURN
        + END
        + i32(2)
        + RETURN
        + END
        + i32(3)
    )


def core_module():
    """A lifecycle that exercises control flow, memory and the config host."""
    blob, (p_load, p_key, p_value) = data_pool("load", "k", "v")

    load_body = (
        lget(0)
        + i32(1)
        + I32_NE
        + IF
        + i32(9)
        + RETURN
        + END
        + i32(DATA_BASE + p_load)
        + i32(4)
        + call(0)
        + i32(0)
    )

    enable_body = (
        # sum 1..10 in a counted loop
        i32(0)
        + lset(0)
        + i32(0)
        + lset(1)
        + BLOCK
        + LOOP
        + lget(0)
        + i32(11)
        + I32_GE_U
        + BR_IF
        + uleb(1)
        + lget(1)
        + lget(0)
        + I32_ADD
        + lset(1)
        + lget(0)
        + i32(1)
        + I32_ADD
        + lset(0)
        + BR
        + uleb(0)
        + END
        + END
        # mem[0] = sum (55 is the byte "7"), logged
        + i32(0)
        + lget(1)
        + I32_STORE8
        + uleb(0)
        + uleb(0)
        + i32(0)
        + i32(1)
        + call(0)
        # mem[2] was written by the start function; log it
        + i32(2)
        + i32(1)
        + call(0)
        # config_set("k", "v"), then config_get("k", 48, 8): the out pointer
        # left under the arguments is the pointer `log` reads back.
        + i32(DATA_BASE + p_key)
        + i32(1)
        + i32(DATA_BASE + p_value)
        + i32(1)
        + call(1)
        + DROP
        + i32(48)
        + i32(DATA_BASE + p_key)
        + i32(1)
        + i32(48)
        + i32(8)
        + call(2)
        + call(0)
        # pick(0), pick(1), pick(7) as three digits at 64, logged as one string
        + i32(64)
        + i32(0)
        + call(6)
        + i32(48)
        + I32_ADD
        + I32_STORE8
        + uleb(0)
        + uleb(0)
        + i32(65)
        + i32(1)
        + call(6)
        + i32(48)
        + I32_ADD
        + I32_STORE8
        + uleb(0)
        + uleb(0)
        + i32(66)
        + i32(7)
        + call(6)
        + i32(48)
        + I32_ADD
        + I32_STORE8
        + uleb(0)
        + uleb(0)
        + i32(64)
        + i32(3)
        + call(0)
        + i32(0)
    )

    funcs = [
        (T_I32_I32, [], load_body),  # 5
        (T_I32, [I32, I32], enable_body),  # 6
        (T_I32, [], i32(0)),  # 7
        (T_I32_I32, [], pick_body()),  # 8
        (T_VOID, [], i32(2) + i32(83) + I32_STORE8 + uleb(0) + uleb(0)),  # 9: start
    ]
    return build(
        HOST_TYPES,
        [("log", T_LOG), ("config_set", T_SET), ("config_get", T_SET)],
        funcs,
        [("pmc_wasm_load", 3), ("pmc_wasm_enable", 4), ("pmc_wasm_disable", 5)],
        memory=1,
        data=blob,
        start=7,
    )


def math_module():
    """Every numeric comparison below must hold, or enable returns 1."""
    blob, (p_load, p_ok) = data_pool("load", "math ok")
    load_body = (
        lget(0)
        + i32(1)
        + I32_NE
        + IF
        + i32(9)
        + RETURN
        + END
        + i32(DATA_BASE + p_load)
        + i32(4)
        + call(0)
        + i32(0)
    )
    enable_body = (
        i32(1)
        + lset(0)
        # (1.5 + 2.25) * 2.0 == 7.5
        + f64(1.5)
        + f64(2.25)
        + F64_ADD
        + f64(2.0)
        + F64_MUL
        + f64(7.5)
        + F64_EQ
        + lget(0)
        + I32_AND
        + lset(0)
        # 6 * 7 == 42
        + i32(6)
        + i32(7)
        + I32_MUL
        + i32(42)
        + I32_EQ
        + lget(0)
        + I32_AND
        + lset(0)
        # 1 << 40 == 1099511627776
        + i64(1)
        + i64(40)
        + I64_SHL
        + i64(1099511627776)
        + I64_EQ
        + lget(0)
        + I32_AND
        + lset(0)
        # nearest(2.5) is 2 (round half to even)
        + f32(2.5)
        + F32_NEAREST
        + f32(2.0)
        + F32_EQ
        + lget(0)
        + I32_AND
        + lset(0)
        # clz(1) == 31
        + i32(1)
        + I32_CLZ
        + i32(31)
        + I32_EQ
        + lget(0)
        + I32_AND
        + lset(0)
        # select picks the first value when the condition is non-zero
        + i32(7)
        + i32(9)
        + i32(1)
        + SELECT
        + i32(7)
        + I32_EQ
        + lget(0)
        + I32_AND
        + lset(0)
        # memory round trip at 8
        + i32(8)
        + i32(305419896)
        + I32_STORE
        + uleb(2)
        + uleb(0)
        + i32(8)
        + I32_LOAD
        + uleb(2)
        + uleb(0)
        + i32(305419896)
        + I32_EQ
        + lget(0)
        + I32_AND
        + lset(0)
        # if (result i32): 0 when every check held
        + lget(0)
        + IF_RESULT
        + i32(DATA_BASE + p_ok)
        + i32(7)
        + call(0)
        + i32(0)
        + ELSE
        + i32(1)
        + END
    )
    return build(
        HOST_TYPES,
        [("log", T_LOG)],
        [
            (T_I32_I32, [], load_body),
            (T_I32, [I32], enable_body),
            (T_I32, [], i32(0)),
        ],
        [("pmc_wasm_load", 1), ("pmc_wasm_enable", 2), ("pmc_wasm_disable", 3)],
        memory=1,
        data=blob,
    )


def globals_module():
    """A mutable global: load writes it, enable reads it back."""
    globals_payload = vec([I32 + b"\x01" + i32(41) + END])
    load_body = gget(0) + i32(1) + I32_ADD + gset(0) + i32(0)
    enable_body = (
        i32(0)
        + gget(0)
        + i32(48)
        + I32_ADD
        + I32_STORE8
        + uleb(0)
        + uleb(0)
        + i32(0)
        + i32(1)
        + call(0)
        + i32(0)
    )
    return build(
        HOST_TYPES,
        [("log", T_LOG)],
        [
            (T_I32_I32, [], load_body),
            (T_I32, [], enable_body),
            (T_I32, [], i32(0)),
        ],
        [("pmc_wasm_load", 1), ("pmc_wasm_enable", 2), ("pmc_wasm_disable", 3)],
        memory=1,
        globals_=globals_payload,
    )


def host_bytes_module():
    """Eight 200-byte log calls; the host-byte budget is what stops it."""
    blob = b"x" * 200
    body = b""
    for _ in range(8):
        body += i32(DATA_BASE) + i32(200) + call(0)
    return build(
        HOST_TYPES,
        [("log", T_LOG)],
        [
            (T_I32_I32, [], i32(0)),
            (T_I32, [], body + i32(0)),
            (T_I32, [], i32(0)),
        ],
        [("pmc_wasm_load", 1), ("pmc_wasm_enable", 2), ("pmc_wasm_disable", 3)],
        memory=1,
        data=blob,
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: wasm_core_test.py <plusmc-wasm>")
    cli = Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="plusmc-wasm-core-") as temporary:
        root = Path(temporary)

        core = root / "core.wasm"
        core.write_bytes(core_module())
        text = run(cli, ["run", core]).stdout
        assert "status: completed" in text, text
        assert "log: load" in text, text
        assert "log: 7" in text, text  # the counted loop's sum
        assert "log: S" in text, text  # the start function's byte
        assert "log: v" in text, text  # config_set/config_get round trip
        assert "log: 123" in text, text  # br_table's three arms
        print("PASS control flow, memory, config and the start section")

        report = json.loads(run(cli, ["run", core, "--json"]).stdout)
        assert report["status"] == "completed" and report["ok"] is True, report
        assert report["log"] == ["log: load", "log: 7", "log: S", "log: v", "log: 123"], report
        assert report["lifecycle"] == {
            "load": {"ran": True, "result": 0},
            "enable": {"ran": True, "result": 0},
            "disable": {"ran": True, "result": 0},
        }, report
        assert report["steps"] > 50, report
        assert report["peak_pages"] == 1, report
        assert report["peak_depth"] >= 2, report
        print("PASS core JSON")

        maths = root / "math.wasm"
        maths.write_bytes(math_module())
        math_text = run(cli, ["run", maths]).stdout
        assert "status: completed" in math_text, math_text
        assert "log: math ok" in math_text, math_text
        math_report = json.loads(run(cli, ["run", maths, "--json"]).stdout)
        assert math_report["lifecycle"]["enable"]["result"] == 0, math_report
        print("PASS numeric core")

        store = root / "globals.wasm"
        store.write_bytes(globals_module())
        globals_text = run(cli, ["run", store]).stdout
        assert "status: completed" in globals_text, globals_text
        assert "log: Z" in globals_text, globals_text  # 41 + 1 + 48
        print("PASS mutable global")

        spam = root / "host-bytes.wasm"
        spam.write_bytes(host_bytes_module())
        host_stop = run(cli, ["run", spam, "--host-bytes", "100"], expect=1)
        assert "host message budget exhausted" in host_stop.stdout, host_stop.stdout
        stopped = json.loads(
            run(cli, ["run", spam, "--host-bytes", "100", "--json"]).stdout
        )
        assert stopped["ok"] is False and stopped["status"] == "budget", stopped
        assert stopped["budget_exhausted"] is True, stopped
        print("PASS host-message budget")

        # The step budget is checked per instruction, so a 100-instruction
        # budget stops on instruction 101 rather than at the next checkpoint.
        loop = root / "loop.wasm"
        loop.write_bytes(
            build(
                HOST_TYPES,
                [],
                [
                    (T_I32_I32, [], i32(0)),
                    (T_I32, [], LOOP + BR + uleb(0) + END + i32(0)),
                    (T_I32, [], i32(0)),
                ],
                [("pmc_wasm_load", 0), ("pmc_wasm_enable", 1), ("pmc_wasm_disable", 2)],
                memory=1,
            )
        )
        exact = json.loads(run(cli, ["run", loop, "--max-steps", "100", "--json"]).stdout)
        # Each call gets its own step budget, so the total is load's two
        # instructions plus enable's 101 (the 100 it may spend and the one
        # that crosses the line).
        assert exact["steps"] == 103, exact
        assert exact["trap"] == "step budget exhausted (100 instructions)", exact
        print("PASS per-instruction step budget")

    print("wasm-core: all checks passed")


if __name__ == "__main__":
    main()
