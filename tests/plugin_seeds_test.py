#!/usr/bin/env python3
"""The seed plugin set, loaded together without a joining client.

Phase 6's gate asks for five or more example plugins. `examples/greeter` is the
full feature walk; these four are the small, task-shaped ones a plugin author
copies:

  * **caretaker** — `/heal`, `/feed`, `/gm` through the player-state writes;
  * **announcer** — a config-driven repeating broadcast with `/announce`;
  * **homes** — `/sethome`/`/home`/`/delhome`/`/homes`, a private save file and
    the entity teleport;
  * **kit** — `/kit`/`/kits` through the inventory writer.

This test installs each module with its checked-in manifest, starts a server
with no client, and pins that all four are discovered, loaded, enabled in the
same process, register their commands (their own load lines say how many), and
are disabled and unloaded by the ordinary shutdown path. It needs no vanilla
data, so it also runs where `plugin-load` cannot.

Run: plugin_seeds_test.py <plusmc> <lib> <toml> <lib> <toml> <lib> <toml> <lib> <toml>
"""

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
    raise AssertionError(f"'{needle}' never reached the log: {read_log(path)[-1500:]}")


def install(root, library, manifest):
    """Copy one example into <root>/plugins/<manifest name>/."""
    with open(manifest, "r", encoding="utf-8") as fh:
        text = fh.read()
    match = re.search(r'^name\s*=\s*"([^"]+)"', text, re.MULTILINE)
    if match is None:
        raise AssertionError(f"no name in {manifest}")
    name = match.group(1)
    directory = os.path.join(root, "plugins", name)
    os.makedirs(directory, exist_ok=True)
    shutil.copyfile(manifest, os.path.join(directory, "+mc-plugin.toml"))
    shutil.copyfile(library, os.path.join(directory, os.path.basename(library)))
    return name


def main():
    if len(sys.argv) != 10:
        print(f"usage: {sys.argv[0]} <plusmc> <lib> <toml> <lib> <toml> <lib> <toml> <lib> <toml>",
              file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1])
    pairs = [(os.path.realpath(sys.argv[i]), os.path.realpath(sys.argv[i + 1]))
             for i in range(2, 10, 2)]
    env = wire_client.server_env(None)
    root = tempfile.mkdtemp(prefix="plusmc-plugin-seeds-")
    log_path = os.path.join(root, "seeds.log")
    try:
        names = [install(root, library, manifest) for library, manifest in pairs]
        assert len(names) == 4, names
        plugins = os.path.join(root, "plugins")
        log = open(log_path, "wb")
        try:
            proc = wire_client.start_process(
                [binary, "--host", "127.0.0.1", "--port", "0", "--offline-mode",
                 "--plugins", plugins],
                stdout=log, stderr=subprocess.STDOUT, env=env)
        except BaseException:
            log.close()
            raise
        try:
            wait_for_log(log_path, "plugins: 4 loaded, 0 failed from")
            for name in names:
                wait_for_log(log_path, f"plugins: enabled {name}")
            # Each example names its command count in its own load line, so a
            # registration that silently failed is a red test rather than a
            # plugin that loads with no surface.
            wait_for_log(log_path, "caretaker: 3 commands ready (heal, feed, gm)")
            wait_for_log(log_path, "announcer: 2 commands, every 6000 ticks")
            wait_for_log(log_path, "homes: 4 commands, 0 saved homes")
            wait_for_log(log_path, "kit: 2 commands ready (kit, kits)")
            wait_for_log(log_path, "listening on")
            assert proc.poll() is None, "the server died after loading the seed set"
        except BaseException:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5)
            raise
        finally:
            log.flush()
            log.close()
        proc.terminate()
        try:
            code = proc.wait(timeout=STEP_TIMEOUT)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
            raise AssertionError("the server did not exit after a stop request")
        text = read_log(log_path)
        assert code == 0, f"shutdown with the seed set exited {code}: {text[-1200:]}"
        for name in names:
            assert f"plugins: disabled {name}" in text, f"{name} was not disabled"
            assert f"plugins: unloaded {name}" in text, f"{name} was not unloaded"
        print("[seeds] loaded, enabled, registered and unloaded caretaker, announcer, "
              "homes and kit", flush=True)
    finally:
        shutil.rmtree(root, ignore_errors=True)

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
