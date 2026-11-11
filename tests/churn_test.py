#!/usr/bin/env python3
"""Join/leave churn + movement stress for +MC (protocol 777).

Spawns STEADY bots that reach play and stream movement packets for the whole
run while CHURN bots repeatedly join and disconnect, so registry add/remove and
the presence/movement broadcasts to survivors are hammered concurrently. A
clean run under ASan/UBSan is the point: before the Phase-1 broadcast-locking
fix, a broadcaster could touch another session's transport after it tore down
(use-after-free) during this churn.

The last step is the *other* half of the same teardown question, and it is the
one a loaded box actually hit (Phase 5's 200-bot soak): the bots above
disconnect before the server is signalled, so the session threads are already
gone when `main` returns — but a host's Ctrl-C arrives with players still
connected, and then the detached per-connection threads outlive `main` and
static destruction destroys the mutexes they are still using. The server aborts
(`std::system_error: mutex lock failed: Invalid argument`) instead of exiting.
This step keeps two clients connected through the signal and pins the property
TcpServer's shutdown drain exists for, in the log order: every live session logs
`disconnected` *before* the server logs `bye`. Without the drain the sessions
never see EOF, so that order cannot hold.

Usage: churn_test.py /path/to/plusmc [cycles]
"""
import os
import pathlib
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_client  # noqa: E402  (the shared client: transport, record, drain)

BIN = pathlib.Path(sys.argv[1]).resolve()
CYCLES = int(sys.argv[2]) if len(sys.argv) > 2 else 6
HOST = "127.0.0.1"
PROTO = 777
STEADY = 2   # bots that stay and move the whole run
CHURN = 4    # bots that join + disconnect per cycle
# Serverbound play ids (777, from the packet table).
P_POSITION = 0x1e  # Move Player Position (3 f64 + u8)
P_POSLOOK = 0x1f   # Move Player Position & Rotation (3 f64 + 2 f32 + u8)
P_LOOK = 0x20      # Move Player Rotation (2 f32 + u8)
# Clientbound play ids.
CB_SYNC_POS = 0x49
CB_KEEPALIVE = 0x2D


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
            raise ValueError("past end")
        byte = b[i]
        v |= (byte & 0x7F) << (7 * k)
        i += 1
        if not (byte & 0x80):
            return v, i
    raise ValueError("bad varint")


def rd_string(b: bytes, i: int = 0):
    ln, i = rd_varint(b, i)
    return b[i:i + ln].decode("utf-8"), i + ln


def recvn(sock, n):
    chunks = []
    got = 0
    while got < n:
        c = sock.recv(n - got)
        if not c:
            raise AssertionError(f"unexpected EOF (wanted {n}, got {got})")
        chunks.append(c)
        got += len(c)
    return b"".join(chunks)


class Client:
    def __init__(self, port):
        # The socket comes from the shared client module so this file sits on the
        # transcript path with every other socket test (`record` below), and
        # `wire_client.drain`'s keep-alive handling is available to the bots.
        self.sock = wire_client.transport(HOST, port, timeout=5)
        self.sock.settimeout(1.5)
        self.threshold = -1
        self.buf = b""

    def send_pkt(self, pid, body=b""):
        payload = varint(pid) + body
        if self.threshold < 0:
            self.sock.sendall(varint(len(payload)) + payload)
        else:
            inner = varint(0) + payload
            self.sock.sendall(varint(len(inner)) + inner)

    def read_pkt(self):
        """Blocking frame read (compression-aware). Returns (pid, body)."""
        while True:
            try:
                pid, body, consumed = self._parse(self.buf)
            except (ValueError, zlib.error) as e:
                raise AssertionError(f"frame parse: {e!r}") from e
            if pid is not None:
                self.buf = self.buf[consumed:]
                return pid, body
            # Single bounded recv: returns whatever is available now (may be
            # fewer than 64 KiB); recvn() would block until the buffer fills.
            c = self.sock.recv(65536)
            if not c:
                raise AssertionError("unexpected EOF")
            self.buf += c

    def _parse(self, buf):
        if not buf:
            return None, None, 0
        ln, nbytes = rd_varint(buf, 0)
        if nbytes <= 0 or len(buf) < nbytes + ln:
            return None, None, 0
        body = buf[nbytes:nbytes + ln]
        consumed = nbytes + ln
        if self.threshold < 0:
            # Plain framing: body starts with the packet id.
            pid, off = rd_varint(body)
            return pid, body[off:], consumed
        # Compressed framing: [dataLength varint][data]; dataLength == 0 => raw.
        dl, dn = rd_varint(body, 0)
        data = body[dn:]
        if dl != 0:
            data = zlib.decompress(data)
        pid, off = rd_varint(data)
        return pid, data[off:], consumed

    def join_to_play(self, name):
        """Offline login + full configuration -> play (stops at sync position)."""
        self.name = name
        hs = (varint(PROTO) + varint(len(HOST)) + HOST.encode() + struct.pack(">H", 0) +
              varint(2))
        self.send_pkt(0, hs)  # handshake: id 0 + body (uncompressed regardless)
        self.send_pkt(0x00, varint(len(name)) + name.encode() + b"\x00" * 16)

        # Login: Set Compression (0x03) then Login Success (0x02) -> ack.
        while True:
            pid, body = self.read_pkt()
            if pid == 0x03:
                t, _ = rd_varint(body)
                self.threshold = t
            elif pid == 0x02:
                self.send_pkt(0x03)  # login acknowledged
                break
            else:
                raise AssertionError(f"{name}: unexpected login packet 0x{pid:02x}")

        # Configuration: echo known packs (0x0e -> serverbound 0x07), then
        # acknowledge Finish Configuration (serverbound 0x03).
        while True:
            pid, body = self.read_pkt()
            if pid == 0x0F:
                npacks, i = rd_varint(body)
                packs = []
                for _ in range(npacks):
                    ns, i = rd_string(body, i)
                    pk, i = rd_string(body, i)
                    ver, i = rd_string(body, i)
                    packs.append((ns, pk, ver))
                out = varint(len(packs))
                for ns, pk, ver in packs:
                    out += (varint(len(ns)) + ns.encode() + varint(len(pk)) + pk.encode() +
                            varint(len(ver)) + ver.encode())
                self.send_pkt(0x07, out)
            elif pid == 0x03:  # Finish Configuration
                self.send_pkt(0x03)  # acknowledge
                break
            # Registry/tags/brand packets: skip.

        # Play: consume until Synchronize Player Position.
        while True:
            pid, _ = self.read_pkt()
            if pid == CB_SYNC_POS:
                return

    def move(self, tag, y=100.0):
        x = 8.0 + (tag % 8) * 0.5
        z = 8.0 + (tag % 5) * 0.5
        self.send_pkt(P_POSITION, struct.pack(">ddd", x, y, z) + b"\x01")
        self.send_pkt(P_POSLOOK,
                      struct.pack(">ddd", x, y + 0.2, z) + struct.pack(">ff", 45.0 * tag, 0.0) +
                      b"\x01")
        self.send_pkt(P_LOOK, struct.pack(">ff", 90.0 * tag, 10.0) + b"\x01")

    def drain_acks(self, budget=0.6):
        """Read what arrives for up to `budget` seconds, answering keep-alives."""
        self.sock.settimeout(0.05)
        end = time.time() + budget
        try:
            while time.time() < end:
                try:
                    pid, body = self.read_pkt()
                except socket.timeout:
                    continue
                if pid == CB_KEEPALIVE:
                    payload, _ = rd_varint(body)
                    self.send_pkt(0x1c, varint(payload))  # keep-alive ack
        finally:
            self.sock.settimeout(1.5)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def steady_bot(port, tag, failures):
    try:
        c = Client(port)
        wire_client.record(c, note=f"churn: the steady bot S{tag}")
        c.join_to_play(f"S{tag}")
        end = time.time() + 16
        while time.time() < end:
            c.move(tag)
            c.drain_acks(budget=0.15)
        c.close()
    except Exception as e:  # noqa: BLE001
        failures.append(f"steady S{tag}: {e!r}")


def wait_listening(proc, timeout=15):
    """The port from the server's own 'listening on <host>:<port>' log line."""
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
    # This file's subject is *concurrency*: fourteen clients joining, moving and
    # disconnecting at once, with the server's locking under it. A transcript
    # holds each connection's packets, but not the interleaving that this test
    # exists to stress — replaying it would re-drive every connection
    # sequentially and pass without having tested anything. So it refuses.
    wire_client.refuse_replay(
        "its subject is the concurrent join/leave interleaving, which a "
        "transcript does not hold: replaying would re-drive each connection "
        "one at a time and pass vacuously")
    # Client-config assets live under the repo's data/vanilla; pass an absolute
    # path so the server finds them regardless of the caller's cwd (ctest runs
    # tests from the build directory).
    data_root = pathlib.Path(__file__).resolve().parent.parent / "data" / "vanilla"
    # The survival tick counts packets (one tick each, no wall-time catch-up),
    # which is also the shape a real client drives: the movement packets the
    # steady bots stream are exactly one tick each, so this churn hits the
    # per-packet survival path rather than whatever a drain's wall time banked.
    # Nothing is asserted here but a clean run (and a clean ASan one).
    proc, port = wire_client.start_server(
        [str(BIN), "--host", HOST, "--port", "0", "--motd", "churn", "--offline-mode", "--data", str(data_root)],
        wait_listening,
        env=dict(os.environ, PLUSMC_SURVIVAL_CLOCK="packet"),
    )

    failures = []
    steadies = [threading.Thread(target=steady_bot, args=(port, t, failures), daemon=True)
                for t in range(STEADY)]
    for th in steadies:
        th.start()
    time.sleep(1.5)  # let the steady bots reach play first

    try:
        for cyc in range(CYCLES):
            churners = []
            for i in range(CHURN):
                c = Client(port)
                wire_client.record(c, note=f"churn: the cycle {cyc} bot C{cyc}-{i}")
                c.join_to_play(f"C{cyc}-{i}")
                c.move(cyc * CHURN + i)
                churners.append(c)
            # Group movement storm, then drop them all.
            for c in churners:
                c.move(cyc + 100)
            for c in churners:
                c.close()
            time.sleep(0.15)
        time.sleep(1.0)
        for th in steadies:
            th.join(timeout=25)

        # ---- the shutdown drain (the 200-bot soak's finding) -----------------
        # Two clients stay connected *through* the signal on purpose. The pipe
        # is pumped from a thread because the server's log is the evidence, and
        # it has to be read while the process is still writing it.
        drain_lines = []

        def pump_log():
            assert proc.stdout is not None
            for line in proc.stdout:
                drain_lines.append(line.decode("utf-8", "replace"))

        pumper = threading.Thread(target=pump_log, daemon=True)
        pumper.start()
        live = []
        for i in range(2):
            c = Client(port)
            c.join_to_play(f"D{i}")
            c.move(40 + i)
            live.append(c)
        time.sleep(0.5)
        proc.terminate()  # SIGTERM, with both players still connected
        try:
            srv_rc = proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
            srv_rc = proc.wait()
            raise AssertionError("plusmc did not exit after SIGTERM with players connected")
        pumper.join(timeout=5)
        for c in live:
            c.close()
        log = "".join(drain_lines)
        if srv_rc != 0:
            sys.stderr.write(f"--- plusmc rc={srv_rc} on shutdown with players live ---\n{log}\n")
            raise AssertionError(f"plusmc exited rc={srv_rc} with sessions live")
        assert "mutex lock failed" not in log, (
            "the server aborted in teardown; the session threads outlived main():\n" + log)
        bye_at = next((i for i, line in enumerate(drain_lines)
                       if line.rstrip().endswith("bye")), None)
        assert bye_at is not None, "the server never logged `bye`:\n" + log
        # Name-matched on purpose: the churn and steady bots above disconnect
        # during the run, and their lines are in this same capture. Only the two
        # clients that were still connected when the signal arrived say anything
        # about the drain.
        gone = [i for i, line in enumerate(drain_lines)
                if any(f"{c.name} disconnected" in line for c in live)]
        assert len(gone) == len(live), (
            f"{len(gone)} of {len(live)} live session(s) logged `disconnected`: the "
            "shutdown did not close them and wait for them:\n" + log)
        assert max(gone) < bye_at, (
            "`bye` was logged while a session was still connected: shutdown() "
            "returned before its session threads did:\n" + log)
        print(f"shutdown drain: {len(gone)} live session(s) logged disconnected "
              f"before `bye` (rc={srv_rc})")
        rc = proc.poll()
        if rc is None:
            proc.terminate()
            rc = proc.wait(timeout=8)
        out = proc.stdout.read().decode("utf-8", "replace")
        if failures:
            sys.stderr.write("--- bot failures ---\n" + "\n".join(failures) + "\n")
        if rc != 0:
            sys.stderr.write(f"--- plusmc rc={rc} ---\n{out}\n")
            raise AssertionError(f"plusmc exited rc={rc}")
        if failures:
            raise AssertionError(f"{len(failures)} steady-bot failure(s)")
        print(f"churn: {CYCLES} cycles x {CHURN} churners + {STEADY} steady bots OK")
    except BaseException:
        rc = proc.poll()
        if rc is None:
            proc.kill()
            rc = proc.wait()
        out = proc.stdout.read().decode("utf-8", "replace") if proc.stdout else ""
        sys.stderr.write(f"--- plusmc rc={rc} ---\n{out}\n----------------------\n")
        raise


if __name__ == "__main__":
    main()
