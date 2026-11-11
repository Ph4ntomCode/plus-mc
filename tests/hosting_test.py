#!/usr/bin/env python3
"""Integration test: Milestone F hosting-panel compatibility + day/night.

Boots plusmc against a temp config directory (the way a hosting panel lays out
a server root) and checks:

  1. server.properties is honoured   — motd, max-players, gamemode, server-port
  2. day/night cycle                — the world clock advances and the server
                                       re-syncs clients (SetTime) while running
  3. eula.txt gate                  — eula=false refuses to start (non-zero exit)
  4. whitelist.json                 — a name not on the list is rejected at login
  5. ops.json                       — once ops exist, non-ops are denied commands

Usage: hosting_test.py /path/to/plusmc
   or: PLUSMC_REPLAY=transcript.jsonl hosting_test.py   (no binary needed)

Replaying re-runs the parts of this file that are *the wire* — the status
response, logins, abilities, the command gate's chat — over a recorded run, and
says out loud which parts it did not re-examine: the server's own boot, its
exit codes, the files it wrote under the config directory, and the log lines it
reported its port in. Those are the server's side of hosting, and a transcript
holds only what crossed the socket.
"""
import json
import os
import pathlib
import re
import socket
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text  # noqa: E402  (text components are network NBT)
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"
BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
DATA_ROOT = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")

FAILURES = []


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    FAILURES.append(msg)


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            return bytes(out)


def rd_varint(buf, pos=0):
    val, shift = 0, 0
    while pos < len(buf):
        b = buf[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            return val, pos
    return None, pos


def rd_varlong(buf, pos=0):
    return rd_varint(buf, pos)


def rd_string(buf, pos=0):
    ln, pos = rd_varint(buf, pos)
    return buf[pos:pos + ln].decode("utf-8", "replace"), pos + ln


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Bot:
    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(2.0)
        self.threshold = -1
        # The read side belongs to a thread from the first drain after the join
        # (`wire_client.Reader`): a windowed drain that reads the socket itself
        # can abandon a half-taken packet when its window closes, which leaves
        # every packet after it garbage, and it can also miss a keep-alive.
        self.reader = wire_client.Reader(self.sock, self)

    def send(self, pid, body=b""):
        self.reader.send(pid, body)

    def recv(self):
        return read_packet(self.sock, self.threshold)

    def start_reader(self):
        self.reader.start()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def start_login(self):
        hs = (varint(0) + varint(PROTO) + varint(len(HOST)) + HOST.encode() +
              struct.pack(">H", 0) + varint(2))
        self.sock.sendall(make_frame(hs, -1))
        self.send(0x00, varint(len(self.name)) + self.name.encode() + b"\x00" * 16)

    def login(self):
        """Full offline join; returns the play-state packets seen during the
        join burst (Join Game, Abilities, chunk batch, ...)."""
        self.start_login()
        while True:
            pid, body = self.recv()
            if pid == 0x03:  # Set Compression (login)
                self.threshold, _ = rd_varint(body)
            elif pid == 0x02:  # Login Success
                self.send(0x03)
                break
            elif pid == 0x00:
                raise AssertionError("login rejected: " +
                                     body[1:].decode("utf-8", "replace"))
        seen = []
        while True:
            pid, body = self.recv()
            if pid == 0x0F:  # Known Packs (config) -> echo back
                npacks, i = rd_varint(body)
                out = varint(npacks)
                for _ in range(npacks):
                    ns, i = rd_string(body, i)
                    pk, i = rd_string(body, i)
                    ver, i = rd_string(body, i)
                    out += (varint(len(ns)) + ns.encode() + varint(len(pk)) +
                            pk.encode() + varint(len(ver)) + ver.encode())
                self.send(0x07, out)
            elif pid == 0x03:  # Finish Configuration -> acknowledge
                self.send(0x03)
                break
        # Play-state join burst, up to and including the chunk batch.
        for _ in range(600):
            pid, body = self.recv()
            seen.append((pid, body))
            if pid == 0x2D:  # keep-alive
                self.send(0x1C, body[:8])
            elif pid == 0x0B:  # chunk batch finished
                n, _ = rd_varint(body)
                self.send(0x0B, varint(n))
                break
        # The join's own reads are done; from here the reader thread collects.
        self.start_reader()
        return seen

    def drain(self, timeout=1.5):
        """Every packet the reader thread queued in the next `timeout` seconds."""
        return wire_client.drain(self, timeout)


def write_config(dirpath, properties, *, eula=True, whitelist=None, ops=None):
    dirpath.mkdir(parents=True, exist_ok=True)
    lines = [f"{k}={v}" for k, v in properties.items()]
    (dirpath / "server.properties").write_text("\n".join(lines) + "\n")
    if eula is not None:
        (dirpath / "eula.txt").write_text(f"eula={'true' if eula else 'false'}\n")
    if whitelist is not None:
        (dirpath / "whitelist.json").write_text(json.dumps(whitelist, indent=2))
    if ops is not None:
        (dirpath / "ops.json").write_text(json.dumps(ops, indent=2))


def wait_listening(proc):
    """The port from the server's own 'listening on <host>:<port>' log line.

    Startup is event-driven: full-suite load can delay a world bind past any
    fixed allowance. EOF is the deterministic failure; CTest's overall timeout
    catches a genuinely hung process.
    """
    while True:
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                fail("server exited before listening")
                return None
            continue
        m = re.search(r"listening on .*:(\d+)", line)
        if m:
            return int(m.group(1))


def boot(config_dir, extra=(), env_extra=None, expect_exit=None):
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "1500"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "6000"
    # Every boot gets the packet clock: the survival tick counts packets (one
    # tick each, no wall-time catch-up), so no server booted here can hand a
    # later assertion a survival-tick quantity that wall time produced. The day
    # cycle this file exercises is the *world* clock and is untouched.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    if env_extra:
        env.update(env_extra)
    args = [str(BIN), "--host", HOST, "--config-dir", str(config_dir),
            "--protocol", str(PROTO), "--version-name", "26.3-test",
            "--data", DATA_ROOT, "--offline-mode", *extra]
    if expect_exit is not None:
        # A boot that is *supposed* to fail is the server's own start-up: no
        # client connects to it, so there is nothing in a transcript to replay.
        if wire_client.skip_server_check(
                f"the boot that must exit {expect_exit} (no client connects "
                f"to a server that refuses to start)"):
            return None, None
        proc = wire_client.start_process(args, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, env=env, text=True,
                                bufsize=1)
        try:
            rc = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            fail(f"server did not exit (wanted {expect_exit})")
            return None, None
        if rc != expect_exit:
            fail(f"server exit code {rc}, wanted {expect_exit}")
        return proc, None
    return wire_client.start_server(args, wait_listening, env=env, text=True,
                                   bufsize=1)


def status_motd(port):
    """Returns (motd, max_players) from the status response."""
    client = wire_client.RawClient(HOST, port, timeout=5)
    wire_client.record(client, note="hosting: the status response a panel reads")
    try:
        client.handshake(1, PROTO, host="localhost")
        client.send(0x00)
        pid, body = client.recv()
        assert pid == 0x00, f"expected status response, got {pid}"
        status = json.loads(rd_string(body)[0])
        return status["description"]["text"], status["players"]["max"]
    finally:
        client.close()


def stop(proc):
    if proc is None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=6)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def test_properties_and_day_cycle():
    print("-> server.properties + day/night")
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        port = free_port()
        write_config(d, {
            "server-port": port,
            "motd": "hosting motd test",
            "max-players": 7,
            "gamemode": "creative",
            "view-distance": 4,
            "online-mode": "false",
            "level-name": "world",
        })
        proc, got_port = boot(d, env_extra={"PLUSMC_DAY_CYCLE": "1"})
        if proc is None:
            # The boot itself was skipped (a replay of a server that refuses to
            # start): no client connects to one, so there is nothing on the wire
            # for this sub-test either.
            return
        try:
            if not wire_client.skip_server_check(
                    "the world files the level-name property bootstrapped"):
                # level-name names a real, persistent world: the server
                # bootstraps the 26.2 layout (vanilla generates the level too).
                level_dat = d / "world" / "level.dat"
                if not level_dat.is_file():
                    fail("level-name property did not bootstrap a world (no level.dat)")
                elif not (d / "world" / "dimensions" / "minecraft" / "overworld").is_dir():
                    fail("bootstrapped world is missing the 26.2 dimension layout")
            if not wire_client.skip_server_check(
                    "the server-port property (the port it was told to listen on)"):
                if got_port != port:
                    fail(f"server-port property ignored: listening {got_port}, wanted {port}")
            motd, maxp = status_motd(got_port)
            if motd != "hosting motd test":
                fail(f"motd property ignored: {motd!r}")
            if maxp != 7:
                fail(f"max-players property ignored: {maxp}")
            bot = Bot("HostTester", got_port)
            wire_client.record(bot, note="hosting: creative abilities and the day cycle")
            seen = bot.login()
            # gamemode=creative -> Abilities flags include allow-flying (0x04).
            abilities = [b for p, b in seen if p == 0x41]
            if not abilities or not (abilities[0][0] & 0x04):
                fail(f"gamemode=creative not applied (abilities {abilities[:1]})")
            # Day/night: the clock advances and SetTime is re-broadcast.
            times = []
            end = time.time() + 3.5
            while time.time() < end:
                for pid, body in bot.drain(0.2):
                    if pid == 0x73:
                        cnt, pos = rd_varint(body, 8)
                        holder, pos = rd_varint(body, pos)
                        total, pos = rd_varlong(body, pos)
                        times.append((holder, total))
            if len(times) < 2:
                fail(f"day cycle did not re-sync the clock (SetTime x{len(times)})")
            elif times[-1][1] <= times[0][1]:
                fail(f"world clock did not advance: {times[0][1]} -> {times[-1][1]}")
            bot.close()
        finally:
            stop(proc)


def test_eula_gate():
    print("-> eula.txt gate")
    if wire_client.skip_server_check(
            "the eula gate (no client connects to a server that refuses to "
            "start: the sub-test is its exit code and the eula.txt it writes)"):
        return
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        write_config(d, {"server-port": free_port(), "motd": "x"}, eula=False)
        boot(d, expect_exit=1)
        # A missing eula.txt next to a server.properties is refused + bootstrapped.
        (d / "eula.txt").unlink()
        proc = wire_client.start_process(
            [str(BIN), "--host", HOST, "--config-dir", str(d), "--offline-mode"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            out, _ = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            out = ""
            fail("server started without eula.txt")
        if "EULA" not in out:
            fail("no EULA message on refusal")
        if not (d / "eula.txt").exists():
            fail("eula.txt was not bootstrapped")
        else:
            body = (d / "eula.txt").read_text()
            if "eula=false" not in body:
                fail("bootstrapped eula.txt should default to false")


def test_whitelist():
    print("-> whitelist.json")
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        write_config(d, {"server-port": 0, "white-list": "true", "online-mode": "false"},
                     whitelist=[{"uuid": "11111111-1111-1111-1111-111111111111",
                                 "name": "SomeoneElse"}])
        proc, port = boot(d)
        if proc is None:
            return
        try:
            bot = Bot("HostTester", port)
            wire_client.record(bot, note="hosting: the whitelist's refusal")
            try:
                bot.login()
                fail("whitelisted-out player was allowed to join")
            except AssertionError as exc:
                if "not whitelisted" not in str(exc):
                    fail(f"unexpected login rejection: {exc}")
            bot.close()
            # The listed name still joins.
            ok = Bot("SomeoneElse", port)
            wire_client.record(ok, note="hosting: the listed name's login")
            ok.login()
            ok.close()
        finally:
            stop(proc)


def test_ops_gate():
    print("-> ops.json command gate")
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        write_config(d, {"server-port": 0, "online-mode": "false"},
                     ops=[{"uuid": "22222222-2222-2222-2222-222222222222",
                           "name": "SomeOperator", "level": 4,
                           "bypassesPlayerLimit": False}])
        proc, port = boot(d)
        if proc is None:
            return
        try:
            bot = Bot("HostTester", port)
            wire_client.record(bot, note="hosting: a non-op's command")
            bot.login()
            bot.drain(0.7)
            cmd = "time query daytime"
            bot.send(0x07, varint(len(cmd)) + cmd.encode())
            pkts = bot.drain(2.0)
            chat = [chat_text(b) for p, b in pkts if p == 0x7C]
            if not any("permission" in c.lower() for c in chat):
                fail(f"non-op was not denied a command: {chat}")
            if any("daytime" in c.lower() and "permission" not in c.lower() for c in chat):
                fail(f"non-op command actually ran: {chat}")
            # The listed operator can run it.
            op = Bot("SomeOperator", port)
            wire_client.record(op, note="hosting: the operator's command")
            op.login()
            op.drain(0.7)
            op.send(0x07, varint(len(cmd)) + cmd.encode())
            opkts = op.drain(2.0)
            opchat = [chat_text(b) for p, b in opkts if p == 0x7C]
            if not any("daytime" in c.lower() for c in opchat):
                fail(f"operator command was not allowed: {opchat}")
            bot.close()
            op.close()
        finally:
            stop(proc)


def main():
    if BIN is None and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    test_properties_and_day_cycle()
    test_eula_gate()
    test_whitelist()
    test_ops_gate()
    if FAILURES:
        print(f"\nFAIL: hosting test ({len(FAILURES)} problem(s))", file=sys.stderr)
        return 1
    print("\nPASS: hosting test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
