#!/usr/bin/env python3
"""Login Plugin Request/Response integration test for +MC (protocol 777).

Boots plusmc with two configured plugin channels and drives a raw client
through login:

  1. Login Start -> the server sends two Login Plugin Requests (0x04) with
     message ids 1..2 and the configured channel identifiers, empty payloads
  2. the client answers request 1 with success + data and request 2 with
     success = false (refused)
  3. the server proceeds to Set Compression + Login Success regardless
  4. SIGTERM -> clean exit 0

No client-config assets are needed: the connection ends right after the
login-acknowledged ack (the server's configuration read then fails cleanly).

The client is `wire_client.RawClient` — the packet pair and a socket from
`wire_client.transport` — and it is handed to `wire_client.record`, so a failing
run leaves a transcript that can be replayed with no server:

Usage: login_plugin_test.py /path/to/plusmc
   or: PLUSMC_REPLAY=transcript.jsonl login_plugin_test.py   (no binary needed)

A replay re-runs the whole exchange and says that the SIGTERM exit code is the
server process's, not the wire's.
"""
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
HOST = "127.0.0.1"
PROTO = 777
NAME = "PluginBot"
CHANNELS = ["fml:handshake", "bungeecord:main"]


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


def rd_varint(b: bytes, i: int = 0):
    v = 0
    for k in range(5):
        if i >= len(b):
            raise ValueError("varint past end")
        byte = b[i]
        v |= (byte & 0x7F) << (7 * k)
        i += 1
        if not (byte & 0x80):
            return v, i
    raise ValueError("bad varint")


def rd_string(b: bytes, i: int = 0):
    ln, i = rd_varint(b, i)
    return b[i:i + ln].decode("utf-8"), i + ln


def wait_listening(proc, timeout=15):
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
    proc, port = wire_client.start_server(
        [str(BIN), "--host", HOST, "--port", "0", "--motd", "plugin test", "--offline-mode"] +
        [arg for ch in CHANNELS for arg in ("--login-plugin-request", ch)],
        wait_listening)
    try:
        client = wire_client.RawClient(HOST, port, timeout=10)
        wire_client.record(client, note="login-plugin: two sequential plugin requests")

        # Handshake -> Login Start.
        client.handshake(2, PROTO, host=HOST)
        client.send(0x00, varint(len(NAME)) + NAME.encode() + b"\x00" * 16)

        # The server sends one Login Plugin Request (0x04) per channel, message
        # ids 1..N, before Set Compression. The exchange is sequential: each
        # request is answered before the next is sent. Answer request 1 with
        # success + 2 bytes of data, request 2 refused.
        requests = []
        for want_id, want_channel in [(1, CHANNELS[0]), (2, CHANNELS[1])]:
            pid, body = client.recv()
            assert pid == 0x04, f"expected Login Plugin Request 0x04, got 0x{pid:02x}"
            msg_id, i = rd_varint(body)
            channel, i = rd_string(body, i)
            assert i == len(body), f"request payload not empty: {body[i:].hex()}"
            assert msg_id == want_id and channel == want_channel, (msg_id, channel)
            requests.append((msg_id, channel))
            if want_id == 1:
                client.send(0x02, varint(want_id) + b"\x01" + varint(2) + b"ok")
            else:
                client.send(0x02, varint(want_id) + b"\x00")
        assert requests == [(1, CHANNELS[0]), (2, CHANNELS[1])], requests
        print("login plugin: two sequential requests answered (message ids + channels OK)")

        # Server proceeds: Set Compression then Login Success.
        saw_compression = False
        saw_success = False
        for _ in range(4):
            pid, body = client.recv()
            if pid == 0x03 and not saw_compression:  # Set Compression (login)
                client.threshold, _ = rd_varint(body)
                saw_compression = True
            elif pid == 0x02:  # Login Success
                u = body[:16]
                nm, i = rd_string(body, 16)
                assert u != b"\x00" * 16 and nm == NAME and len(body) >= i + 1 + 16
                saw_success = True
            elif pid == 0x00:
                raise AssertionError("server sent a login disconnect")
            if saw_compression and saw_success:
                break
        assert saw_compression and saw_success, (saw_compression, saw_success)
        print("login plugin: server proceeded to Set Compression + Login Success")

        client.send(0x03)  # login acknowledged (framed at the new threshold)
        client.close()
        print("login plugin: OK")

        if not wire_client.skip_server_check(
                "the SIGTERM exit code (this run has no server process)"):
            proc.terminate()
            rc = proc.wait(timeout=5)
            assert rc == 0, f"expected clean exit on SIGTERM, got {rc}"
            print("graceful shutdown: OK")
    except BaseException:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        log = proc.stdout.read().decode("utf-8", "replace") if proc.stdout else ""
        sys.stderr.write("--- plusmc log ---\n" + log + "\n------------------\n")
        raise
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
