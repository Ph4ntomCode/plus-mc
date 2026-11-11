#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The frozen-ABI gate's own tests.

The checker only matters if it catches a break. These cases mutate a temporary
copy of plugin.h and assert the exit status: the current header passes, appending
a host entry passes, and inserting, retyping, renaming or revaluing anything the
baseline froze fails with a line naming it.

Run: tests/plugin_abi_test.py [/path/to/check_plugin_abi.py]
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECKER = os.path.join(ROOT, "tools", "check_plugin_abi.py")
HEADER = os.path.join(ROOT, "src", "pluginapi", "plugin.h")
BASELINE = os.path.join(ROOT, "src", "pluginapi", "abi-v1.json")


def run(header_text):
    with tempfile.TemporaryDirectory(prefix="plusmc-abi-") as tmp:
        path = os.path.join(tmp, "plugin.h")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(header_text)
        proc = subprocess.run(
            [sys.executable, CHECKER, "--header", path, "--baseline", BASELINE],
            capture_output=True, text=True, timeout=30,
        )
        return proc.returncode, proc.stdout + proc.stderr


def mutate(text, old, new, label, expect_ok=False):
    assert old in text, f"{label}: anchor missing from plugin.h"
    code, output = run(text.replace(old, new, 1))
    if expect_ok:
        assert code == 0, f"{label}: expected acceptance, got {code}: {output[-900:]}"
        print(f"[ok] {label}", flush=True)
        return
    assert code == 1, f"{label}: expected a refusal, got {code}: {output[-900:]}"
    assert "field" in output or "enum" in output, \
        f"{label}: refusal did not name the break: {output[-900:]}"
    print(f"[caught] {label}", flush=True)


def main():
    with open(HEADER, "r", encoding="utf-8") as fh:
        original = fh.read()

    code, output = run(original)
    assert code == 0, f"the current header must pass: {output[-900:]}"
    print("[ok] current header matches abi-v1.json", flush=True)

    mutate(
        original,
        "} pmc_host_api;",
        "  int (*future_hook)(void* host);\n} pmc_host_api;",
        "append a host-table entry",
        expect_ok=True,
    )
    mutate(
        original,
        "  void* host; // opaque, pass back as the first argument",
        "  void* host; // opaque, pass back as the first argument\n  int (*inserted)(void* host);",
        "insert a host-table entry",
    )
    mutate(
        original,
        "uint32_t struct_size;\n  uint32_t abi_version;\n  void* host;",
        "uint64_t struct_size;\n  uint32_t abi_version;\n  void* host;",
        "retype a host-table field",
    )
    mutate(
        original,
        "int (*player_op_level)(void* host, const char* name);",
        "int (*player_level)(void* host, const char* name);",
        "rename a host-table accessor",
    )
    mutate(
        original,
        "PMC_EVENT_PLAYER_CHAT = 10",
        "PMC_EVENT_PLAYER_CHAT = 11",
        "change an event value",
    )
    print("PASS: the plugin ABI checker accepts additions and catches breaks", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
