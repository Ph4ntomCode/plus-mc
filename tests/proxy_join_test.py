#!/usr/bin/env python3
"""The forwarded address at the ops surface: it follows the session's log lines.

A proxy terminates the client's connection, so the backend's socket peer is the
proxy. The identity it forwards carries the real client address, and until this
test only the login line showed it: the join and disconnect lines named the
player and nothing else, so an operator could not correlate a session with the
client behind the proxy from the log alone.

This starts plusmc with `--bungeecord`, puts the live proxy
(`tests/proxy_live.py`) in front of it with a distinctive forwarded address
(203.0.113.7, not the 127.0.0.1 the proxy's own socket would show), full-joins
one client through it, and asserts:

  * the login line names the forwarded address (the proxy, not the socket);
  * the join line carries the same address;
  * the disconnect line carries it too;
  * no line reports the proxy's own 127.0.0.1 as the player's address.

Needs the extracted client-config assets, like `join_test.py`.
"""
import os
import pathlib
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import join_test  # noqa: E402  (the full-join client; this test adds the hop)
import proxy_live  # noqa: E402  (a real second TCP hop, forwarding the address)
import wire_client  # noqa: E402  (server_env)

BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
HOST = "127.0.0.1"
UUID_HEX = "fedcba9876543210fedcba9876543210"
REAL_CLIENT = "203.0.113.7"
NAME = "ProxyLine"
LISTEN_RE = re.compile(r"listening on [\d.]+:(\d+)")
STEP_TIMEOUT = 20.0


def wait_line(lines, needle, timeout=STEP_TIMEOUT):
    """The first captured log line containing `needle`."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in list(lines):
            if needle in line:
                return line
        time.sleep(0.05)
    raise AssertionError(f"no log line contained {needle!r}; log so far:\n"
                         + "".join(lines[-20:]))


def wait_port(lines, proc, timeout=STEP_TIMEOUT):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in list(lines):
            m = LISTEN_RE.search(line)
            if m:
                port = int(m.group(1))
                for _ in range(30):
                    try:
                        sock = socket.create_connection((HOST, port), timeout=1)
                        sock.close()
                        return port
                    except OSError:
                        time.sleep(0.1)
        if proc.poll() is not None:
            raise AssertionError("plusmc exited before listening:\n" + "".join(lines[-20:]))
        time.sleep(0.05)
    raise AssertionError(f"plusmc did not listen in {timeout:g} s")


def main() -> None:
    data_root = pathlib.Path(__file__).resolve().parent.parent / "data" / "vanilla"
    assert data_root.is_dir(), "no data/vanilla extraction present; run tools/vanilla-data"
    config_dir = tempfile.mkdtemp(prefix="plusmc-proxy-join-")
    args = [str(BIN), "--host", HOST, "--port", "0", "--offline-mode",
            "--bungeecord", "--config-dir", config_dir,
            "--data", str(data_root), "--view-distance", "1"]
    proc = wire_client.start_process(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1,
                            env=wire_client.server_env(
                                {"PLUSMC_SURVIVAL_CLOCK": "packet"}))
    lines = []

    def pump():
        assert proc.stdout is not None
        for line in proc.stdout:
            lines.append(line)

    threading.Thread(target=pump, daemon=True).start()
    proxy = None
    try:
        port = wait_port(lines, proc)
        proxy = proxy_live.Proxy((HOST, port), mode="bungeecord",
                                 uuid_hex=UUID_HEX, client_address=REAL_CLIENT)
        proxy_port = proxy.start()

        client = join_test.Client(NAME, proxy_port)
        client.join()
        login = wait_line(lines, f"login: {NAME} (forwarded by BungeeCord from ")
        assert REAL_CLIENT in login, login
        joined = wait_line(lines, f"{NAME} joined the game (entity ")
        assert f", from {REAL_CLIENT})" in joined, joined
        assert "127.0.0.1" not in joined, joined
        print(f"join line: {joined.strip()}")

        client.close()
        gone = wait_line(lines, f"{NAME} disconnected")
        assert f"(from {REAL_CLIENT})" in gone, gone
        assert "127.0.0.1" not in gone, gone
        print(f"disconnect line: {gone.strip()}")
    finally:
        if proxy is not None:
            proxy.close()
        proc.terminate()
        try:
            code = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            code = -1
        if code != 0:
            raise AssertionError(f"plusmc did not exit cleanly (rc={code}):\n"
                                 + "".join(lines[-20:]))
    print("PASS")


if __name__ == "__main__":
    main()
