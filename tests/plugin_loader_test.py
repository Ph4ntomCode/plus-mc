#!/usr/bin/env python3
"""The native plugin loader without a joining client.

`tests/plugin_test.py` is the full feature walk (chat, commands, scheduler,
accessors) and needs the extracted client-config assets because it joins a
player. The loader itself — manifest, platform suffix, opening the module,
the entry symbol, ABI/name checks, and unload on shutdown — needs none of
that: a server that only starts, logs the plugin's lifecycle and stops is
enough. This test is that server, so the Windows loader path (LoadLibrary /
GetProcAddress / FreeLibrary) and the POSIX one (dlopen / dlsym / dlclose) are
both exercised by the ordinary suite, including on a runner with no vanilla
data. The optional old-host fixture is a module built against the original
host table; loading it proves the host's advertised size covers that prefix.

Run: plugin_loader_test.py /path/to/plusmc /path/to/greeter.dll /path/to/+mc-plugin.toml \
                    [/path/to/oldhost.dll /path/to/oldhost.toml \
                     /path/to/refusing.dll /path/to/refusing.toml]
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wire_client  # noqa: E402

STEP_TIMEOUT = 30.0


def read_log(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except FileNotFoundError:
        return ""


def wait_for_log(path, needle, timeout=STEP_TIMEOUT):
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = read_log(path)
        if needle in text:
            return text
        time.sleep(0.05)
    raise AssertionError(f"'{needle}' never reached the log: {read_log(path)[-1200:]}")


def write_plugin(root, manifest_path, library, name=None, api=None, depends=None):
    directory = os.path.join(root, "plugins", name or "greeter")
    os.makedirs(directory, exist_ok=True)
    with open(manifest_path, "r", encoding="utf-8") as fh:
        text = fh.read()
    if name is not None:
        text = re.sub(r"^name\s*=.*$", f'name = "{name}"', text, flags=re.M)
    if api is not None:
        text = re.sub(r"^api\s*=.*$", f"api = {api}", text, flags=re.M)
    if depends is not None:
        text = text.rstrip("\n") + "\ndepends = [" + ", ".join(
            json.dumps(dep) for dep in depends) + "]\n"
    with open(os.path.join(directory, "+mc-plugin.toml"), "w", encoding="utf-8") as fh:
        fh.write(text)
    shutil.copyfile(library, os.path.join(directory, os.path.basename(library)))
    return os.path.join(root, "plugins")


def write_config(plugins_dir, text, name="greeter"):
    directory = os.path.join(plugins_dir, name, "data")
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "config.toml"), "w", encoding="utf-8") as fh:
        fh.write(text)


def start(binary, plugins_dir, log_path, env):
    log = open(log_path, "wb")
    try:
        return wire_client.start_process(
            [binary, "--host", "127.0.0.1", "--port", "0", "--offline-mode",
             "--plugins", plugins_dir],
            stdout=log, stderr=subprocess.STDOUT, env=env), log
    except BaseException:
        log.close()
        raise


def stop(proc, log_handle):
    """Ctrl-C/CTRL_BREAK, then a clean exit 0 or a named failure."""
    log_handle.flush()
    proc.terminate()
    try:
        code = proc.wait(timeout=STEP_TIMEOUT)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        raise AssertionError("plusmc did not exit after a console stop request")
    finally:
        log_handle.close()
    return code


def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc /path/to/greeter.so "
              "/path/to/+mc-plugin.toml [/path/to/oldhost.so /path/to/oldhost.toml]",
              file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1])
    library = os.path.realpath(sys.argv[2])
    manifest = os.path.realpath(sys.argv[3])
    oldhost_library = os.path.realpath(sys.argv[4]) if len(sys.argv) > 4 else None
    oldhost_manifest = os.path.realpath(sys.argv[5]) if len(sys.argv) > 5 else None
    if (oldhost_library is None) != (oldhost_manifest is None):
        print("Usage: give both the old-host library and its manifest, or neither",
              file=sys.stderr)
        return 1
    refusing_library = os.path.realpath(sys.argv[6]) if len(sys.argv) > 6 else None
    refusing_manifest = os.path.realpath(sys.argv[7]) if len(sys.argv) > 7 else None
    if (refusing_library is None) != (refusing_manifest is None):
        print("Usage: give both the refusing library and its manifest, or neither",
              file=sys.stderr)
        return 1
    env = wire_client.server_env(None)
    root = tempfile.mkdtemp(prefix="plusmc-plugin-loader-")
    try:
        plugins_dir = write_plugin(root, manifest, library)
        write_config(plugins_dir,
                     'greeting = "hello from config"\nheartbeat_ticks = 7\ndebug = true\n')
        if oldhost_library is not None:
            write_plugin(root, oldhost_manifest, oldhost_library, name="oldhost")
        if refusing_library is not None:
            write_plugin(root, refusing_manifest, refusing_library, name="refusing")
        log_path = os.path.join(root, "loader.log")
        proc, handle = start(binary, plugins_dir, log_path, env)
        try:
            wait_for_log(log_path, "plugins: loaded greeter 1.13.0 (25 commands, 9 listeners)")
            wait_for_log(log_path, "plugins: enabled greeter")
            wait_for_log(log_path, "[greeter] enabled")
            wait_for_log(
                log_path,
                "[greeter] config: greeting=hello from config heartbeat_ticks=7 debug=1")
            if refusing_library is not None:
                # The fixture registered a tick listener and a command, then
                # returned 1 from on_load. Both must be dropped before the
                # module closes: a stale tick listener would crash the first
                # pump, which the heartbeat below proves has run.
                wait_for_log(log_path, "plugins: refusing: on_load refused the plugin (1)")
                assert "plugins: loaded refusing" not in read_log(log_path)
                wait_for_log(log_path, "[greeter] heartbeat at tick", timeout=10.0)
                assert proc.poll() is None, "the server died after refusing a plugin"
                print("[A] a plugin that refused after registering was purged, not crashed",
                      flush=True)
            if oldhost_library is not None:
                # A plugin built against the original host table loads and reads
                # its prefix; the module's own load callback refuses anything
                # shorter, so reaching this line is the assertion.
                wait_for_log(log_path, "plugins: loaded oldhost 1.0.0 (1 command, 0 listeners)")
                wait_for_log(log_path, "old-host fixture: read the original table prefix")
                print("[A] an original-host-table plugin loaded and read its prefix", flush=True)
        except BaseException:
            handle.close()
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)
            raise
        code = stop(proc, handle)
        log = read_log(log_path)
        assert code == 0, f"clean shutdown after the plugin was loaded exited {code}: {log[-900:]}"
        assert "plugins: disabled greeter" in log, "on_disable did not run"
        assert "plugins: unloaded greeter" in log, "the module was not unloaded"
        if oldhost_library is not None:
            assert "plugins: unloaded oldhost" in log, "the old-host module was not unloaded"
        marker = "[greeter] data dir: "
        assert marker in log, f"the plugin's data directory was not reported: {log[-900:]}"
        plugin_data_dir = log.split(marker, 1)[1].splitlines()[0].strip()
        assert os.path.basename(plugin_data_dir) == "data", plugin_data_dir
        assert os.path.isdir(plugin_data_dir), f"{plugin_data_dir} was not created"
        async_marker = "async task ran on the worker thread"
        assert async_marker in log, f"the async task did not run before unload: {log[-900:]}"
        assert log.index(async_marker) < log.index("plugins: unloaded greeter"), \
            "the module was unloaded under a running async task"
        assert "cancelled async task ran" not in log, "a cancelled async task still ran"
        print("[A] opened the module, found its entry, enabled and unloaded it", flush=True)
        print("[A2] the host created the plugin's private data directory", flush=True)
        print("[A3] async work ran and was drained before unload", flush=True)

        # One broken plugin must be refused and counted, and the server must
        # still reach the listener rather than dying with it.
        bad_root = tempfile.mkdtemp(prefix="plusmc-plugin-loader-badabi-")
        try:
            bad_dir = write_plugin(bad_root, manifest, library, name="greeter2", api=2)
            bad_log_path = os.path.join(bad_root, "bad.log")
            proc, handle = start(binary, bad_dir, bad_log_path, env)
            try:
                wait_for_log(bad_log_path, "wants ABI 2 but this engine is ABI 1")
                wait_for_log(bad_log_path, "plugins: 0 loaded, 1 failed")
                wait_for_log(bad_log_path, "listening on")
            except BaseException:
                handle.close()
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=5)
                raise
            code = stop(proc, handle)
            bad_log = read_log(bad_log_path)
            assert code == 0, f"a refused plugin stopped the server (rc {code}): {bad_log[-900:]}"
            print("[B] a wrong-ABI plugin was refused, counted, and did not stop the server",
                  flush=True)
        finally:
            shutil.rmtree(bad_root, ignore_errors=True)

        # The directory names sort greeter before oldhost, so only the manifest
        # dependency can put oldhost first. Disable is the reverse order.
        if oldhost_library is not None:
            dep_root = tempfile.mkdtemp(prefix="plusmc-plugin-deps-")
            try:
                dep_dir = write_plugin(dep_root, manifest, library, depends=["oldhost"])
                write_plugin(dep_root, oldhost_manifest, oldhost_library, name="oldhost")
                dep_log_path = os.path.join(dep_root, "deps.log")
                proc, handle = start(binary, dep_dir, dep_log_path, env)
                try:
                    wait_for_log(dep_log_path, "plugins: enabled oldhost")
                    wait_for_log(dep_log_path, "plugins: enabled greeter")
                except BaseException:
                    handle.close()
                    if proc.poll() is None:
                        proc.kill()
                        proc.wait(timeout=5)
                    raise
                code = stop(proc, handle)
                dep_log = read_log(dep_log_path)
                assert code == 0, f"dependency-ordered shutdown exited {code}: {dep_log[-900:]}"
                assert dep_log.index("plugins: enabled oldhost") < dep_log.index(
                    "plugins: enabled greeter"), "the dependency was not enabled first"
                assert dep_log.index("plugins: disabled greeter") < dep_log.index(
                    "plugins: disabled oldhost"), "the dependent was not disabled first"
                print("[C] a manifest dependency was enabled first and disabled last", flush=True)
            finally:
                shutil.rmtree(dep_root, ignore_errors=True)

        # A dependency that is not in the tree refuses its dependent, and only
        # its dependent: the server still reaches the listener and exits clean.
        missing_root = tempfile.mkdtemp(prefix="plusmc-plugin-missing-")
        try:
            missing_dir = write_plugin(missing_root, manifest, library, depends=["absent"])
            missing_log_path = os.path.join(missing_root, "missing.log")
            proc, handle = start(binary, missing_dir, missing_log_path, env)
            try:
                wait_for_log(missing_log_path, "depends on 'absent', which is not present")
                wait_for_log(missing_log_path, "plugins: 0 loaded, 1 failed")
                wait_for_log(missing_log_path, "listening on")
            except BaseException:
                handle.close()
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=5)
                raise
            code = stop(proc, handle)
            assert code == 0, f"a missing dependency stopped the server (rc {code})"
            print("[D] a missing dependency refused only the dependent", flush=True)
        finally:
            shutil.rmtree(missing_root, ignore_errors=True)

        # A self-dependency is the smallest cycle: it is named in the log and
        # refused, without leaving a half-loaded plugin behind.
        cycle_root = tempfile.mkdtemp(prefix="plusmc-plugin-cycle-")
        try:
            cycle_dir = write_plugin(cycle_root, manifest, library, depends=["greeter"])
            cycle_log_path = os.path.join(cycle_root, "cycle.log")
            proc, handle = start(binary, cycle_dir, cycle_log_path, env)
            try:
                wait_for_log(cycle_log_path, "dependency cycle: greeter -> greeter")
                wait_for_log(cycle_log_path, "plugins: 0 loaded, 1 failed")
                wait_for_log(cycle_log_path, "listening on")
            except BaseException:
                handle.close()
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=5)
                raise
            code = stop(proc, handle)
            assert code == 0, f"a dependency cycle stopped the server (rc {code})"
            print("[E] a dependency cycle was named and refused", flush=True)
        finally:
            shutil.rmtree(cycle_root, ignore_errors=True)

        # A malformed config refuses the plugin with the file and line; the
        # server still reaches the listener instead of starting half-configured.
        bad_config_root = tempfile.mkdtemp(prefix="plusmc-plugin-config-")
        try:
            bad_config_dir = write_plugin(bad_config_root, manifest, library)
            write_config(bad_config_dir, "greeting =\n")
            bad_config_log = os.path.join(bad_config_root, "config.log")
            proc, handle = start(binary, bad_config_dir, bad_config_log, env)
            try:
                wait_for_log(bad_config_log, "config.toml:1: value is empty")
                wait_for_log(bad_config_log, "plugins: 0 loaded, 1 failed")
                wait_for_log(bad_config_log, "listening on")
            except BaseException:
                handle.close()
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=5)
                raise
            code = stop(proc, handle)
            assert code == 0, f"a malformed config stopped the server (rc {code})"
            print("[F] a malformed config refused the plugin with its line", flush=True)
        finally:
            shutil.rmtree(bad_config_root, ignore_errors=True)
    finally:
        shutil.rmtree(root, ignore_errors=True)

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
