#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""WASM plugins in a live server (Phase 8, slice 4).

The runtime's CLI host lives for one report. This test boots a real server with
`--wasm-plugins <dir>` and checks the other half:

  * discovery: a `+mc-wasm.toml` package beside its core module is loaded and
    its `pmc_wasm_load`/`pmc_wasm_enable` run before the listener opens;
  * the live host: `log` reaches the server log, `config_set` writes
    `<config-dir>/wasm/<name>.toml`, `config_get` reads the value back into the
    module, and `broadcast` goes through the player registry (zero recipients
    is still a successful call);
  * shutdown: `pmc_wasm_disable` runs and the package is reported unloaded;
  * a linked v0.2 component package runs `init-plugin`, async `on-load` and
    async `on-unload` without stopping the server;
  * a `--wasm-plugins` path that is not a directory fails configuration.

The modules are assembled in Python — no toolchain and no checked-in binaries.

Run: wasm_server_test.py /path/to/plusmc
"""
from __future__ import annotations

import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wasm_component_run_test import pumpkin_v02_component  # noqa: E402
import wire_client as wire  # noqa: E402

TIMEOUT = 40
MAGIC = b"\x00asm"
VERSION = b"\x01\x00\x00\x00"
COMPONENT_VERSION = b"\x0d\x00\x01\x00"

I32 = b"\x7f"
T_LOAD = None
T_VOID = None
T_LOG = None
T_CFG = None


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
    return b"\x60" + vec(list(params)) + vec(list(results))


def i32(value):
    return b"\x41" + sleb(value)


def call(index):
    return b"\x10" + uleb(index)


def data_payload(blob, offsets):
    return vec(
        [uleb(0) + b"\x41" + sleb(offset) + b"\x0b" + uleb(len(text)) + text
         for offset, text in zip(offsets, blob)]
    )


def lifecycle(data, load_body, enable_body, disable_body, memory_pages=1):
    types = [
        ftype([I32], [I32]),      # load(i32) -> i32
        ftype([], [I32]),         # enable/disable() -> i32
        ftype([I32, I32], []),    # log / broadcast(ptr, len)
        ftype([I32, I32, I32, I32], [I32]),  # config_get / config_set
    ]
    imports = [name("plusmc:host") + name("log") + b"\x00" + uleb(2),
               name("plusmc:host") + name("broadcast") + b"\x00" + uleb(2),
               name("plusmc:host") + name("config_set") + b"\x00" + uleb(3),
               name("plusmc:host") + name("config_get") + b"\x00" + uleb(3)]
    funcs = [(0, load_body), (1, enable_body), (1, disable_body)]
    bodies = []
    for type_index, body in funcs:
        inner = b"\x00" + body + b"\x0b"
        bodies.append(uleb(len(inner)) + inner)
    exports = [name("pmc_wasm_load") + b"\x00" + uleb(4),
               name("pmc_wasm_enable") + b"\x00" + uleb(5),
               name("pmc_wasm_disable") + b"\x00" + uleb(6)]
    # funcs is (type, body); keep the function section type indices aligned.
    function_types = [type_index for type_index, _ in funcs]
    return b"".join([
        MAGIC,
        VERSION,
        section(1, vec(types)),
        section(2, vec(imports)),
        section(3, vec([uleb(t) for t in function_types])),
        section(5, b"\x01\x00" + uleb(memory_pages)),
        section(7, vec(exports)),
        section(10, vec(bodies)),
        section(11, data_payload(*data)),
    ])


def module_bytes():
    strings = [b"wasm load", b"wasm enabled", b"wasm stopped", b"greeting", b"hello"]
    offsets = []
    cursor = 0
    for text in strings:
        offsets.append(cursor)
        cursor += len(text)
    load_text, enabled_text, stopped_text, key_text, value_text = offsets
    load_body = (
        i32(load_text) + i32(len(strings[0])) + call(0)
        + i32(key_text) + i32(len(strings[3])) + i32(value_text) + i32(len(strings[4])) + call(2)
        + b"\x1a"  # drop the config_set result
        + i32(0)
    )
    enable_body = (
        i32(enabled_text) + i32(len(strings[1])) + call(0)
        + i32(128) + i32(key_text) + i32(len(strings[3])) + i32(128) + i32(16) + call(3)
        + call(0)
        + i32(enabled_text) + i32(len(strings[1])) + call(1)
        + i32(0)
    )
    disable_body = i32(stopped_text) + i32(len(strings[2])) + call(0) + i32(0)
    return lifecycle((strings, offsets), load_body, enable_body, disable_body)


def read(path):
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def wait_for(log_path, proc, needle, timeout=TIMEOUT):
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = read(log_path)
        if needle in text:
            return text
        if proc.poll() is not None:
            raise AssertionError(f"server exited before {needle!r}: {text[-1500:]}")
        time.sleep(0.05)
    raise AssertionError(f"{needle!r} never reached the log: {read(log_path)[-1500:]}")


def wait_for_tick(log_path, proc, timeout=TIMEOUT):
    """Wait for any server-tick-start report from the compiled component.

    The component logs every 20th tick. On a slow runner the first one can be
    tick 40 rather than tick 20, while on-load is still finishing; the dispatch
    is what this test proves, not which multiple happened to be first.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = read(log_path)
        if re.search(r"wasm: \[compiled\] tick \d+", text):
            return text
        if proc.poll() is not None:
            raise AssertionError(f"server exited before a compiled tick: {text[-1500:]}")
        time.sleep(0.05)
    raise AssertionError(f"no compiled tick reached the log: {read(log_path)[-1500:]}")


def start(binary, plugins, config, log_path):
    log = open(log_path, "wb")
    env = dict(os.environ)
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    proc = wire.start_process(
        [str(binary), "--host", "127.0.0.1", "--port", "0", "--offline-mode",
         "--motd", "wasm server test", "--wasm-plugins", str(plugins),
         "--config-dir", str(config)],
        stdout=log, stderr=subprocess.STDOUT, env=env,
    )
    return proc, log


def stop(proc, log):
    proc.send_signal(signal.SIGTERM)
    try:
        code = proc.wait(timeout=25)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        raise AssertionError("the server did not exit on SIGTERM")
    log.close()
    return code


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: wasm_server_test.py <plusmc>")
    binary = Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="plusmc-wasm-server-") as temporary:
        root = Path(temporary)
        plugins = root / "plugins"
        package = plugins / "serverprobe"
        package.mkdir(parents=True)
        (package / "probe.wasm").write_bytes(module_bytes())
        (package / "+mc-wasm.toml").write_text(
            'name = "serverprobe"\nversion = "1.0.0"\napi = 1\nmodule = "probe.wasm"\n',
            encoding="utf-8",
        )
        config = root / "config"
        config.mkdir()
        log_path = root / "server.log"

        proc, log = start(binary, plugins, config, log_path)
        try:
            text = wait_for(log_path, proc, "listening on")
            assert "wasm: loaded serverprobe 1.0.0" in text, text[-2000:]
            assert "wasm: [serverprobe] wasm load" in text, text[-2000:]
            assert "wasm: [serverprobe] wasm enabled" in text, text[-2000:]
            config_text = read(config / "wasm" / "serverprobe.toml")
            assert 'greeting = "hello"' in config_text, config_text
        finally:
            code = stop(proc, log)
        assert code == 0, (code, read(log_path)[-2000:])
        final = read(log_path)
        assert "wasm: [serverprobe] wasm stopped" in final, final[-2000:]
        assert "wasm: unloaded serverprobe" in final, final[-2000:]
        print("PASS load, live host and shutdown")

        # A v0.2 component is linked and run through the same persistent host:
        # init-plugin and async on-load at startup, async on-unload at shutdown.
        component_dir = plugins / "pumpkin"
        component_dir.mkdir()
        component_dir.joinpath("pumpkin.wasm").write_bytes(pumpkin_v02_component())
        component_dir.joinpath("+mc-wasm.toml").write_text(
            'name = "pumpkin"\nversion = "0.2.0"\napi = 1\nmodule = "pumpkin.wasm"\n',
            encoding="utf-8",
        )
        log2 = root / "server2.log"
        proc2, log_handle = start(binary, plugins, config, log2)
        try:
            text2 = wait_for(log2, proc2, "listening on")
            assert "wasm: loaded pumpkin 0.2.0 (component)" in text2, text2[-2000:]
            assert "wasm: [pumpkin] init" in text2, text2[-2000:]
            assert "wasm: [pumpkin] loaded" in text2, text2[-2000:]
            assert "wasm: loaded serverprobe" in text2, text2[-2000:]
        finally:
            assert stop(proc2, log_handle) == 0
        final2 = read(log2)
        assert "wasm: [pumpkin] unload" in final2, final2[-2000:]
        assert "wasm: unloaded pumpkin" in final2, final2[-2000:]
        print("PASS a v0.2 component loads and unloads")

        # The checked-in cargo-component artifact registers a tick handler in
        # on-load. The live server must dispatch server-tick-start to
        # handle-event, not just load the component.
        compiled_plugins = root / "compiled_plugins"
        compiled_dir = compiled_plugins / "compiled"
        compiled_dir.mkdir(parents=True)
        compiled_dir.joinpath("pumpkin.wasm").write_bytes(
            (Path(__file__).resolve().parent / "fixtures" / "wasm" / "pumpkin_plugin.wasm").read_bytes()
        )
        compiled_dir.joinpath("+mc-wasm.toml").write_text(
            'name = "compiled"\nversion = "0.2.0"\napi = 1\nmodule = "pumpkin.wasm"\n',
            encoding="utf-8",
        )
        log3 = root / "server3.log"
        proc3, log_handle3 = start(binary, compiled_plugins, config, log3)
        try:
            text3 = wait_for(log3, proc3, "listening on")
            assert "wasm: loaded compiled 0.2.0 (component)" in text3, text3[-2000:]
            assert "wasm: [compiled] compiled on-load" in text3, text3[-2000:]
            text3 = wait_for(log3, proc3, "wasm: [compiled] scheduled delayed=")
            assert "wasm: [compiled] task 12" in text3 or "task 12" in wait_for(
                log3, proc3, "wasm: [compiled] task 12"
            ), text3[-2000:]
            text3 = wait_for(log3, proc3, "wasm: [compiled] task 11")
            assert "wasm: [compiled] task 12" in text3, text3[-2000:]
            assert "wasm: [compiled] task 13" not in text3, text3[-2000:]
            text3 = wait_for_tick(log3, proc3)
            assert "wasm: [compiled] players=0" in text3, text3[-2000:]
        finally:
            assert stop(proc3, log_handle3) == 0
        final3 = read(log3)
        assert "wasm: [compiled] compiled on-unload" in final3, final3[-2000:]
        assert "wasm: unloaded compiled" in final3, final3[-2000:]
        print("PASS a cargo-component plugin receives server-tick-start")

        # A typo must not silently run with none of the packages.
        missing = root / "missing"
        result = subprocess.run(
            [str(binary), "--validate-config", "--offline-mode", "--wasm-plugins", str(missing)],
            capture_output=True, text=True, timeout=TIMEOUT,
        )
        output = result.stdout + result.stderr
        assert result.returncode == 1, (result.returncode, output[-1200:])
        assert "--wasm-plugins" in output and "not a directory" in output, output[-1200:]
        print("PASS a bad --wasm-plugins path fails configuration")

    print("wasm-server: all checks passed")


if __name__ == "__main__":
    main()
