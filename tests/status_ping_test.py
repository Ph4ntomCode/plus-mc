#!/usr/bin/env python3
"""Integration test for the +MC Phase-1 status/ping slice.

Boots plusmc on a kernel-assigned port and acts as a Minecraft client through
the server-list-ping flow: handshake -> status request -> status response JSON
-> ping -> pong. Finally sends SIGTERM and asserts plusmc exits cleanly (0).

The client is `wire_client.RawClient` — the packet pair and a socket from
`wire_client.transport` — and it is handed to `wire_client.record`, so a failing
run leaves a transcript that can be replayed with no server:

Usage: status_ping_test.py /path/to/plusmc
   or: PLUSMC_REPLAY=transcript.jsonl status_ping_test.py   (no binary needed)

A replay re-runs the whole exchange and says that the SIGTERM exit code is the
server process's, not the wire's.
"""
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_client  # noqa: E402  (the shared client: transport, record, RawClient)

BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
PROTOCOL = 777
VERSION_NAME = "26.3-test"
MOTD = "smoke motd"


def varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            return bytes(out)


def read_string(payload: bytes):
    pos = 0
    length = 0
    for i in range(5):
        byte = payload[pos]
        pos += 1
        length |= (byte & 0x7F) << (7 * i)
        if not (byte & 0x80):
            break
    return payload[pos : pos + length].decode("utf-8")


def wait_listening(proc: subprocess.Popen, timeout: float = 15) -> int:
    """Reads the server log until 'listening on <host>:<port>' appears."""
    assert proc.stdout is not None
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                raise AssertionError("plusmc exited before listening")
            continue
        m = re.search(rb"listening on [\d.]+:(\d+)", line)
        if m:
            return int(m.group(1))
    raise AssertionError("plusmc did not report a listening port in time")


def main() -> None:
    if BIN is None and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    proc = None
    try:
        proc, port = wire_client.start_server(
            [str(BIN), "--host", "127.0.0.1", "--port", "0", "--protocol",
             str(PROTOCOL), "--version-name", VERSION_NAME, "--motd", MOTD],
            wait_listening)
        client = wire_client.RawClient("127.0.0.1", port, timeout=5)
        wire_client.record(client, note="status-ping: the server-list ping")
        try:
            # Handshake (next_state = 1: status), then the status request
            # (serverbound id 0x00, empty body).
            client.handshake(1, PROTOCOL, host="localhost")
            client.send(0x00)

            pid, body = client.recv()
            assert pid == 0x00, f"expected status response id 0, got {pid}"
            status = json.loads(read_string(body))
            assert status["version"]["protocol"] == PROTOCOL, status
            assert status["version"]["name"] == VERSION_NAME, status
            assert status["description"]["text"] == MOTD, status
            assert status["players"]["max"] == 20, status
            assert status["enforcesSecureChat"] is False, status

            # Ping (serverbound id 0x01, 8-byte payload) -> pong echo.
            token = 0x1234567890ABCDEF
            client.send(0x01, struct.pack(">q", token))
            pid, body = client.recv()
            assert pid == 0x01, f"expected pong id 1, got {pid}"
            (echo,) = struct.unpack(">q", body)
            assert echo == token, f"pong mismatch: {echo} != {token}"
        finally:
            client.close()

        print("status/ping: OK")

        # Graceful shutdown: SIGTERM must produce exit code 0.
        if not wire_client.skip_server_check(
                "the SIGTERM exit code (this run has no server process)"):
            proc.terminate()
            try:
                rc = proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                raise AssertionError("plusmc did not exit after SIGTERM")
            assert rc == 0, f"expected clean exit on SIGTERM, got {rc}"
            print("graceful shutdown: OK")
    except BaseException:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        raise

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
