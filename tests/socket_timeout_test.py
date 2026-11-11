#!/usr/bin/env python3
"""Socket read timeout: the backstop moves with the keep-alive (protocol 777).

Every accepted socket carries a receive timeout, and the play state's keep-alive
is what decides liveness there: a session whose keep-alive is due at 60 s can
never answer inside a 30 s socket timeout, so a fixed 30 s backstop closed the
socket under a client that was merely silent and perfectly healthy. That was
advancement-persistence-wire's silent rejoin — the server logged "disconnected"
at 30 s and the rejoin's next play packet hit a broken pipe. `main.cpp` now
derives the timeout as `keep_alive_ms + keep_alive_timeout_ms + 1000`, floored at
vanilla's 30 s, and logs the result at boot.

The test reads that line instead of the wire: the behaviour it protects (a
silent client surviving past 30 s) takes 30 s to observe, while the derivation
takes a boot. advancement-persistence-wire is the wire half — its silent rejoin
only gets to send its first play packet because this number is right.

Three arms, because two would not pin the rule:

* **Defaults** (no env): 10 s keep-alive + 15 s reply window -> 30 s. The
  shipped configuration must stay at vanilla's number.
* **60 s / 300 s**: the cadence a silent-but-healthy client has to survive ->
  361 s. This is the arm that fails if the timeout is fixed at 30 s.
* **1.2 s / 6 s**: shorter than the floor -> still 30 s, so the rule reads as a
  floor and not as "always keep-alive + reply".

Usage: socket_timeout_test.py /path/to/plusmc
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wire_client  # noqa: E402  (start_process: graceful Ctrl-Break on Windows)

HOST = "127.0.0.1"
START = time.time()

# `main.cpp`: "socket read timeout: 30s (keep-alive 10s + 15s reply window)".
# The three numbers are whole seconds — the log divides the ms values — so this
# pins the effective value, which is what the socket gets.
TIMEOUT_LINE = re.compile(
    r"socket read timeout: (\d+)s \(keep-alive (\d+)s \+ (\d+)s reply window\)")
PORT_LINE = re.compile(r"listening on [\d.]+:(\d+)")

# label, keep-alive ms, reply window ms; None leaves the env unset (the shipped
# SessionSettings defaults: 10000 / 15000).
ARMS = (
    ("defaults", None, None),
    ("60 s / 300 s", 60000, 300000),
    ("1.2 s / 6 s", 1200, 6000),
)


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}", flush=True)


def read_until_port(proc, timeout=20.0):
    """`(port, lines)` from the server's own log, up to the port line."""
    lines = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = proc.stdout.readline().decode(errors="replace")
        if line:
            lines.append(line)
            m = PORT_LINE.search(line)
            if m:
                return int(m.group(1)), lines
        elif proc.poll() is not None:
            raise AssertionError(
                "plusmc exited before it advertised a port:\n" + "".join(lines))
        else:
            # A live process with nothing to say yet: its stdout is a pipe, so
            # an empty read here cannot mean EOF, and a breath is enough.
            time.sleep(0.01)
    raise AssertionError(
        f"plusmc did not advertise a port within {timeout:g}s:\n" + "".join(lines))


def boot(binary, data_root, world, env):
    proc = wire_client.start_process(
        [binary, "--host", HOST, "--port", "0", "--offline-mode", "--world", world,
         "--data", data_root, "--no-flat-world"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    try:
        port, lines = read_until_port(proc)
    except BaseException:
        proc.kill()
        proc.wait()
        raise
    return proc, port, lines


def read_timeout(lines):
    for line in lines:
        m = TIMEOUT_LINE.search(line)
        if m:
            return tuple(int(part) for part in m.groups())
    raise AssertionError(
        "the boot never logged 'socket read timeout: Ns (keep-alive Xs + Ys reply "
        "window)' before the port; the derivation is unobservable and the "
        "regression this test exists for cannot be caught:\n" + "".join(lines))


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1])
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-socket-timeout-")
    try:
        for label, keep_alive_ms, reply_ms in ARMS:
            env = dict(os.environ)
            # The test's own answer must not depend on the box it runs on.
            env.pop("PLUSMC_KEEPALIVE_MS", None)
            env.pop("PLUSMC_KEEPALIVE_TIMEOUT_MS", None)
            env["PLUSMC_AUTOSAVE_SECS"] = "0"
            if keep_alive_ms is not None:
                env["PLUSMC_KEEPALIVE_MS"] = str(keep_alive_ms)
                env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = str(reply_ms)
            step(f"-> {label}: boot and read the timeout off the log")
            proc, port, lines = boot(binary, data_root, world, env)
            passed = False
            try:
                assert port > 0, f"{label}: no port"
                got_timeout, got_keep_alive, got_reply = read_timeout(lines)
                if keep_alive_ms is None:
                    want_timeout, want_keep_alive, want_reply = 30, 10, 15
                else:
                    want_timeout = max(30000, keep_alive_ms + reply_ms + 1000) // 1000
                    want_keep_alive = keep_alive_ms // 1000
                    want_reply = reply_ms // 1000
                assert (got_keep_alive, got_reply) == (want_keep_alive, want_reply), (
                    f"{label}: the log reported a different cadence than the env: "
                    f"keep-alive {got_keep_alive}s, reply {got_reply}s, want "
                    f"{want_keep_alive}s / {want_reply}s")
                assert got_timeout == want_timeout, (
                    f"{label}: socket read timeout is {got_timeout}s, want "
                    f"{want_timeout}s — a backstop shorter than keep-alive + reply "
                    f"reaps a silent client the keep-alive still considers alive")
                step(f"  OK {got_timeout}s (keep-alive {got_keep_alive}s + "
                     f"{got_reply}s reply window)")
                passed = True
            finally:
                proc.send_signal(signal.SIGTERM)
                try:
                    rc = proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                    rc = None
                # Only an arm that got its answer has to show a clean shutdown:
                # a failure above is the failure worth reporting, and masking
                # it with a teardown assertion would hide what the boot said.
                if passed:
                    assert rc == 0, f"{label}: plusmc exited rc={rc} on SIGTERM"
        print("\nPASS: socket read timeout follows the keep-alive cadence")
        return 0
    finally:
        shutil.rmtree(world, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
