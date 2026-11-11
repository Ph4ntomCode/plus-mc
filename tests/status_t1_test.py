#!/usr/bin/env python3
"""Phase 9 T1 entry test: status negotiation for protocols 766-774.

The tier table is the first real translation boundary.  A T1 client must see
its own protocol in the status JSON (otherwise it refuses the server before
login), while the configured 26.3 name remains untouched for canonical clients.
1.21.11 (774) now proceeds to login/configuration and is covered by
t1_config_test; the other T1 protocols still pin the explicit, tier-aware
refusal rather than an accidental generic parse failure.

Usage: status_t1_test.py /path/to/plusmc
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
import wire_client  # noqa: E402

BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
PROTOCOL = 777
VERSION_NAME = "26.3-test"
MOTD = "t1 status motd"

T0_VERSIONS = {
    775: "26.1",
    776: "26.2",
}

T1_VERSIONS = {
    766: "1.20.5",
    767: "1.21",
    768: "1.21.2",
    769: "1.21.4",
    770: "1.21.5",
    771: "1.21.6",
    772: "1.21.7",
    773: "1.21.9",
    774: "1.21.11",
}


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
    assert proc.stdout is not None
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                raise AssertionError("plusmc exited before listening")
            continue
        match = re.search(rb"listening on [\d.]+:(\d+)", line)
        if match:
            return int(match.group(1))
    raise AssertionError("plusmc did not report a listening port in time")


def status_exchange(port: int, protocol: int):
    client = wire_client.RawClient("127.0.0.1", port, timeout=5)
    try:
        client.handshake(1, protocol, host="localhost")
        client.send(0x00)
        pid, body = client.recv()
        assert pid == 0x00, f"protocol {protocol}: expected status response, got {pid}"
        status = json.loads(read_string(body))

        token = (0x5A5A0000 + protocol) & 0x7FFFFFFFFFFFFFFF
        client.send(0x01, struct.pack(">q", token))
        pid, body = client.recv()
        assert pid == 0x01, f"protocol {protocol}: expected pong, got {pid}"
        (echo,) = struct.unpack(">q", body)
        assert echo == token, f"protocol {protocol}: pong mismatch: {echo} != {token}"
        return status
    finally:
        client.close()


def expect_t1_login_refusal(port: int, protocol: int):
    client = wire_client.RawClient("127.0.0.1", port, timeout=5)
    try:
        client.handshake(2, protocol, host="localhost")
        pid, body = client.recv()
        assert pid == 0x00, f"expected login disconnect, got {pid}"
        reason = read_string(body)
        assert "T1" in reason, reason
        assert str(protocol) in reason, reason
        return reason
    finally:
        client.close()


def expect_snapshot_login_refusal(port: int):
    # 26.4 is not a release yet; its snapshots use synthetic protocols. The
    # server must say that plainly rather than pretending 777 is that snapshot.
    client = wire_client.RawClient("127.0.0.1", port, timeout=5)
    try:
        client.handshake(2, 0x40000001, host="localhost")
        pid, body = client.recv()
        assert pid == 0x00, f"expected login disconnect, got {pid}"
        reason = read_string(body)
        assert "development snapshots" in reason, reason
        assert "26.3" in reason, reason
        return reason
    finally:
        client.close()


def main() -> None:
    if BIN is None:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1

    proc = None
    try:
        proc, port = wire_client.start_server(
            [str(BIN), "--host", "127.0.0.1", "--port", "0", "--protocol",
             str(PROTOCOL), "--version-name", VERSION_NAME, "--motd", MOTD,
             "--offline-mode"],
            wait_listening)

        for protocol, version_name in T1_VERSIONS.items():
            status = status_exchange(port, protocol)
            assert status["version"]["protocol"] == protocol, (
                f"protocol {protocol}: {status['version']}")
            assert status["version"]["name"] == version_name, (
                f"protocol {protocol}: {status['version']}")
            assert status["description"]["text"] == MOTD, status

        # Canonical clients keep the operator-configured display name.
        status = status_exchange(port, PROTOCOL)
        assert status["version"]["protocol"] == PROTOCOL, status
        assert status["version"]["name"] == VERSION_NAME, status

        # 26.1/26.2 are translated T0 clients: they must see their own protocol
        # before login, exactly like T1, but keep the T0 release names.
        for protocol, version_name in T0_VERSIONS.items():
            status = status_exchange(port, protocol)
            assert status["version"]["protocol"] == protocol, (
                f"protocol {protocol}: {status['version']}")
            assert status["version"]["name"] == version_name, (
                f"protocol {protocol}: {status['version']}")

        # T2 (765) is not a T1 status: keep advertising canonical 777 so the
        # client shows an incompatibility instead of a false compatible status.
        status = status_exchange(port, 765)
        assert status["version"]["protocol"] == PROTOCOL, status
        assert status["version"]["name"] == VERSION_NAME, status

        # 26.4 snapshots use synthetic protocols and must not be advertised as
        # compatible with the released 777 protocol.
        status = status_exchange(port, 0x40000001)
        assert status["version"]["protocol"] == PROTOCOL, status
        assert status["version"]["name"] == VERSION_NAME, status

        # 1.21.11 is the first T1 release with login/configuration; the other
        # eight still fail closed before a login decode is attempted.
        for protocol in T1_VERSIONS:
            if protocol == 774:
                continue
            reason = expect_t1_login_refusal(port, protocol)
        print(f"T1 login refusal (766-773; 774 has its own config test): {reason}")
        reason = expect_snapshot_login_refusal(port)
        print(f"snapshot login refusal: {reason}")

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

        print("status T1 negotiation: OK")
        print("PASS")
        return 0
    except BaseException:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        raise


if __name__ == "__main__":
    sys.exit(main() or 0)
