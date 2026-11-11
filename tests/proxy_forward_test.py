#!/usr/bin/env python3
"""Proxy forwarding at join (Phase 5, ordered item 7), wire-level.

A server behind a proxy is connected to by the *proxy*, so the client's
identity never arrives the ordinary way. Two protocols forward it, and both
supply an identity the server would otherwise derive itself:

  1. **Legacy BungeeCord** — the proxy appends `<ip>\\0<uuid>\\0<properties>` to
     the handshake's server-address field (NUL-separated). With `--bungeecord`
     the server must take the UUID and game-profile properties from it rather
     than deriving them from Login Start (the format carries no username, so
     Login Start's name stays authoritative).
  2. Opt-in — the same handshake against a server without `--bungeecord` must
     be treated as an ordinary connection: Login Start's name, and an identity
     that is not the forwarded one. (The address field is client-controlled;
     honouring it unasked is an impersonation hole.)
  3. **Velocity modern** — a `velocity:player_info` Login Plugin Request whose
     one-byte payload advertises the forwarding version, answered with an
     HMAC-SHA256 signed blob. A valid response substitutes the identity; a
     tampered signature, or a proxy that refuses, disconnects the player.
  4. **Both again, through a live proxy** (`tests/proxy_live.py`). Sections 1-3
     build the forwarded handshake in this process, which is the format; these
     put a real second TCP hop in the way — the proxy accepts the client,
     writes the handshake the backend sees and relays the session. The legacy
     arm's handshake is written in two pieces a few tens of milliseconds apart
     (a reader that assumes one read per handshake fails here), and the
     Velocity arm is answered by the proxy itself with a name that is not the
     one Login Start carried. Before either, the relay loop is checked on its
     own: three echo sessions at once, both directions, plus one that closes
     immediately, all through the one selector.

Stops at Login Success, so it needs no client-config assets. Usage:

  proxy_forward_test.py /path/to/plusmc
  PLUSMC_REPLAY=transcript.jsonl proxy_forward_test.py   (no binary needed)

The live-proxy arms are skipped in a replay: a transcript holds one end's wire
and has no second hop.
"""
import hashlib
import hmac
import os
import pathlib
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import proxy_live  # noqa: E402  (a real second TCP hop: the forwarding proxy)
import wire_client  # noqa: E402  (the shared client: transport, record, RawClient)

BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
HOST = "127.0.0.1"
PROTO = 777
SECRET = "proxy-secret"

# Notch's real UUID (dashed 069a79f4-44e9-4726-a5be-fca90e38aaf5) as the
# undashed hex the legacy handshake carries.
LEGACY_UUID_HEX = "069a79f444e94726a5befca90e38aaf5"
LEGACY_UUID = bytes.fromhex(LEGACY_UUID_HEX)
VELOCITY_UUID = uuid.UUID("11223344-5566-7788-99aa-bbccddeeff00").bytes
PROPS = [("textures", "dGVzdA==", "c2lnbmF0dXJl")]

# The identity the *proxy* vouches for in the live arms. The client below sends
# none of it, so a substituted UUID can only have come from the proxy.
PROXY_UUID = bytes.fromhex("fedcba9876543210fedcba9876543210")
PROXY_PROPS_JSON = ("[{\"name\":\"textures\",\"value\":\"dGVzdA==\","
                    "\"signature\":\"c2lnbmF0dXJl\"}]")
VELOCITY_LIVE_NAME = "VelAuthed"


# --- varint / string helpers -------------------------------------------------

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


def mc_string(s: str) -> bytes:
    raw = s.encode("utf-8")
    return varint(len(raw)) + raw


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


# --- login exchange ----------------------------------------------------------

def start(config_dir, extra):
    return wire_client.start_server(
        [str(BIN), "--host", HOST, "--port", "0", "--offline-mode",
         "--protocol", str(PROTO), "--config-dir", config_dir] + extra,
        wait_listening)


def connect(port, note, address=None):
    """A RawClient that has sent handshake (next=2) + Login Start.

    `address` overrides the handshake's server-address field — the channel the
    legacy forwarding data rides in; the login name is always "WireName", so a
    substituted name in Login Success can only have come from the proxy.

    `record` runs before the handshake, not after: the handshake and Login Start
    are part of the exchange, and a transcript that starts at the first read
    cannot replay what the client said first.
    """
    client = wire_client.RawClient(HOST, port, timeout=10)
    wire_client.record(client, note=note)
    client.handshake(2, PROTO, host=address if address is not None else HOST)
    name = b"WireName"
    client.send(0x00, varint(len(name)) + name + b"\x00" * 16)
    return client


def read_login_success(client):
    """Read login packets past Set Compression; return (uuid, name, props).

    `props` is a list of `(name, value, signature-or-'')`.
    """
    while True:
        pid, body = client.recv()
        if pid == 0x03:  # Set Compression
            client.threshold, _ = rd_varint(body)
            continue
        if pid == 0x00:  # login disconnect
            raise AssertionError("server disconnected during login: "
                                 + body.decode("utf-8", "replace"))
        assert pid == 0x02, f"expected Login Success 0x02, got 0x{pid:02x}"
        u, i = body[:16], 16
        name, i = rd_string(body, i)
        count, i = rd_varint(body, i)
        props = []
        for _ in range(count):
            pname, i = rd_string(body, i)
            pvalue, i = rd_string(body, i)
            has_sig = body[i]
            i += 1
            sig = ""
            if has_sig:
                sig, i = rd_string(body, i)
            props.append((pname, pvalue, sig))
        assert len(body) - i == 16, f"session id should be 16 bytes, got {len(body) - i}"
        return u, name, props


def read_login_disconnect(client):
    """The login disconnect reason, asserting the server sent one."""
    pid, body = client.recv()
    assert pid == 0x00, f"expected a login disconnect 0x00, got 0x{pid:02x}"
    return body.decode("utf-8", "replace")


def read_velocity_request(client):
    """The velocity:player_info request; returns (msg_id, payload)."""
    pid, body = client.recv()
    assert pid == 0x04, f"expected Login Plugin Request 0x04, got 0x{pid:02x}"
    msg_id, i = rd_varint(body)
    channel, i = rd_string(body, i)
    assert channel == "velocity:player_info", channel
    return msg_id, body[i:]


def velocity_response(address, player_uuid, username, props, signature=None):
    """A `velocity:player_info` response body: [32-byte HMAC][version][...].

    The HMAC covers everything after the signature, which is exactly what
    server/proxy.cpp verifies.
    """
    rest = varint(1) + mc_string(address) + player_uuid + mc_string(username)
    rest += varint(len(props))
    for name, value, sig in props:
        rest += mc_string(name) + mc_string(value)
        rest += (b"\x01" + mc_string(sig)) if sig is not None else b"\x00"
    mac = signature if signature is not None else hmac.new(
        SECRET.encode(), rest, hashlib.sha256).digest()
    return mac + rest


def send_plugin_response(client, msg_id, success, data=b""):
    body = varint(msg_id) + bytes([1 if success else 0])
    if success:
        body += varint(len(data)) + data
    client.send(0x02, body)


# --- the relay loop ----------------------------------------------------------

def check_relay(relay_loops=1):
    """Selector loops carry several sessions, both ways, independently.

    A plain echo backend stands in for plusmc here: the subject is the proxy's
    relay, not the protocol. Three sessions run at once through one proxy, each
    pushing payloads several `recv` calls long and checking every byte returns;
    a fourth joins and closes at once, which must not disturb the others (the
    selector drops one pair, not the loop). Run once with one loop and once
    with several, where the same traffic must still be exact and the pairs
    must actually be spread over more than one loop.
    """
    backend = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    backend.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    backend.bind((HOST, 0))
    backend.listen(8)
    stop = threading.Event()
    errors = []

    def echo(conn):
        try:
            while True:
                data = conn.recv(65536)
                if not data:
                    return
                conn.sendall(data)
        except OSError:
            pass
        finally:
            conn.close()

    def accept():
        while not stop.is_set():
            try:
                conn, _ = backend.accept()
            except OSError:
                return
            threading.Thread(target=echo, args=(conn,), daemon=True).start()

    threading.Thread(target=accept, daemon=True).start()
    proxy = proxy_live.Proxy((HOST, backend.getsockname()[1]), mode="bungeecord",
                             uuid_hex=LEGACY_UUID_HEX, relay_loops=relay_loops)
    port = proxy.start()

    def exact(sock, count):
        buf = bytearray()
        while len(buf) < count:
            chunk = sock.recv(count - len(buf))
            if not chunk:
                raise AssertionError("the relay closed a session early")
            buf.extend(chunk)
        return bytes(buf)

    def session(index):
        try:
            sock = socket.create_connection((HOST, port), timeout=10)
            sock.sendall(proxy_live.frame(
                proxy_live.build_handshake(PROTO, f"echo-{index}", 25565, 2)))
            # Backend -> client: the echo of the handshake the proxy wrote.
            echoed = proxy_live.read_packet(sock)
            assert echoed[:1] == b"\x00", "the echo backend's handshake came back mangled"
            payload = bytes([index]) * (192 * 1024)
            for _ in range(3):
                sock.sendall(payload)
                assert exact(sock, len(payload)) == payload, \
                    f"session {index} got its bytes back corrupted"
            sock.close()
        except BaseException as exc:  # surfaced on the main thread
            errors.append(exc)

    def flash():
        try:
            sock = socket.create_connection((HOST, port), timeout=10)
            sock.sendall(proxy_live.frame(
                proxy_live.build_handshake(PROTO, "echo-flash", 25565, 2)))
            sock.close()
        except BaseException as exc:
            errors.append(exc)

    try:
        threads = [threading.Thread(target=session, args=(i,)) for i in range(3)]
        threads.append(threading.Thread(target=flash))
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=60)
            assert not t.is_alive(), "the relay stalled"
        if errors:
            raise errors[0]
        assert len(proxy._loops) == relay_loops
        if relay_loops > 1:
            used = sum(1 for loop in proxy._loops if loop.handoffs)
            assert used >= 2, \
                f"the {relay_loops}-loop proxy put the whole fleet on {used} loop(s)"
    finally:
        proxy.close()
        stop.set()
        backend.close()
    print(f"live proxy: {relay_loops} selector loop(s) relayed three sessions both "
          "ways; a fourth closing did not disturb them")


def main() -> None:
    if BIN is None and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1

    config_dir = tempfile.mkdtemp(prefix="plusmc-proxy-")
    procs = []
    try:
        # --- 1. legacy BungeeCord: the handshake's identity wins -------------
        proc, port = start(config_dir, ["--bungeecord"])
        procs.append(proc)
        addr = (HOST + "\x00" + "127.0.0.1" + "\x00" + LEGACY_UUID_HEX + "\x00"
                + '[{"name":"textures","value":"dGVzdA==","signature":"c2lnbmF0dXJl"}]')
        client = connect(port, "legacy BungeeCord forwarding", address=addr)
        got_uuid, got_name, got_props = read_login_success(client)
        assert got_uuid == LEGACY_UUID, f"uuid not forwarded: {got_uuid.hex()}"
        assert got_name == "WireName", f"login start's name should stand, got {got_name!r}"
        assert [tuple(p) for p in got_props] == [(n, v, s) for n, v, s in PROPS], got_props
        print("legacy forwarding: UUID and properties taken from the handshake")
        client.close()

        if not wire_client.skip_server_check("the server's own log lines"):
            proc.terminate()
            assert proc.wait(timeout=5) == 0, "expected clean exit on SIGTERM"
            log = proc.stdout.read().decode("utf-8", "replace")
            assert "forwarded by BungeeCord from 127.0.0.1" in log, \
                "the forwarded client address was not honoured"
            print("legacy forwarding: the forwarded client address is honoured")

        # --- 2. the same handshake, forwarding off: it is just an address ----
        proc, port = start(config_dir, [])
        procs.append(proc)
        client = connect(port, "no forwarding: the handshake data is inert", address=addr)
        got_uuid, got_name, got_props = read_login_success(client)
        assert got_name == "WireName", f"forwarded name used without opting in: {got_name!r}"
        assert got_uuid != LEGACY_UUID, "forwarded UUID used without opting in"
        assert got_props == [], f"forwarded properties used without opting in: {got_props}"
        print("forwarding is opt-in: the handshake's extra data is ignored by default")
        client.close()
        if not wire_client.skip_server_check("the SIGTERM exit code"):
            proc.terminate()
            assert proc.wait(timeout=5) == 0, "expected clean exit on SIGTERM"

        # --- 3. Velocity modern forwarding ----------------------------------
        proc, port = start(config_dir, ["--velocity-forwarding", "--velocity-secret", SECRET])
        procs.append(proc)

        client = connect(port, "velocity modern forwarding: signed identity")
        msg_id, payload = read_velocity_request(client)
        assert msg_id == 1 and payload == b"\x01", (msg_id, payload)
        send_plugin_response(client, msg_id, True,
                             velocity_response("203.0.113.7", VELOCITY_UUID, "VelName", PROPS))
        got_uuid, got_name, got_props = read_login_success(client)
        assert got_uuid == VELOCITY_UUID, f"uuid not forwarded: {got_uuid.hex()}"
        assert got_name == "VelName", f"forwarded name ignored, got {got_name!r}"
        assert [tuple(p) for p in got_props] == [(n, v, s) for n, v, s in PROPS], got_props
        print("velocity forwarding: the signed UUID, name and properties are substituted")
        client.close()

        # A forged signature must never be honoured.
        client = connect(port, "velocity modern forwarding: forged signature")
        msg_id, payload = read_velocity_request(client)
        send_plugin_response(
            client, msg_id, True,
            velocity_response("203.0.113.7", VELOCITY_UUID, "VelName", PROPS,
                              signature=b"\x00" * 32))
        reason = read_login_disconnect(client)
        assert "verify" in reason.lower(), reason
        print("velocity forwarding: a forged signature is refused")
        client.close()

        # A refused response is refused too (the proxy did not forward).
        client = connect(port, "velocity modern forwarding: proxy refused")
        msg_id, payload = read_velocity_request(client)
        send_plugin_response(client, msg_id, False)
        reason = read_login_disconnect(client)
        assert "velocity" in reason.lower(), reason
        print("velocity forwarding: a refused response disconnects the player")
        client.close()

        if not wire_client.skip_server_check("the SIGTERM exit code"):
            proc.terminate()
            assert proc.wait(timeout=5) == 0, "expected clean exit on SIGTERM"
            print("graceful shutdown: OK")

        # --- 4. a live proxy in front of the server --------------------------
        # Sections 1-3 build the forwarded handshake in this process, which is
        # the format. These two put a real proxy in the way: it accepts the
        # client, writes the handshake the backend sees and relays the session
        # -- the traffic a server actually meets behind a proxy. A transcript
        # holds one end's wire and has no second hop, so a replay skips them.
        if wire_client.skip_server_check("the live-proxy arms (a second TCP hop)"):
            print("live proxy: skipped")
        else:
            # The relay itself needs no server, and a broken one there would
            # otherwise show up as a confusing login timeout in 4a/4b. The
            # multi-loop arm is the layout the hop's p99 diagnosis says to
            # measure from (docs/PHASE5.md §4); it must relay exactly here.
            check_relay()
            check_relay(relay_loops=3)

            # 4a. Legacy: the proxy's UUID wins, over a handshake the proxy
            # deliberately writes in two TCP segments (a reader that assumes one
            # read per handshake is caught here rather than excused).
            proc, port = start(config_dir, ["--bungeecord"])
            procs.append(proc)
            proxy = proxy_live.Proxy((HOST, port), mode="bungeecord",
                                     uuid_hex=PROXY_UUID.hex(),
                                     properties_json=PROXY_PROPS_JSON,
                                     split_handshake=True)
            proxy.start()
            try:
                # The client's own handshake carries no forwarding data: its
                # address is a plain host, so the identity below is the proxy's.
                client = wire_client.RawClient(HOST, proxy.port, timeout=10)
                client.handshake(2, PROTO, host="backend.example")
                name = b"LiveName"
                client.send(0x00, varint(len(name)) + name + b"\x00" * 16)
                got_uuid, got_name, got_props = read_login_success(client)
                assert got_uuid == PROXY_UUID, \
                    f"uuid not forwarded by the proxy: {got_uuid.hex()}"
                assert got_name == "LiveName", \
                    f"login start's name should stand, got {got_name!r}"
                assert [tuple(p) for p in got_props] == [(n, v, s) for n, v, s in PROPS], \
                    got_props
                # The split is the arm's other half: it is only coverage if the
                # proxy really wrote the handshake in two pieces.
                assert proxy.splits >= 1, "the handshake arm was not split"
                client.close()
                print("live proxy: the legacy identity is the proxy's, "
                      "handshake split mid-varint")
            finally:
                proxy.close()
            proc.terminate()
            assert proc.wait(timeout=5) == 0, "expected clean exit on SIGTERM"
            log = proc.stdout.read().decode("utf-8", "replace")
            assert "forwarded by BungeeCord from 127.0.0.1" in log, \
                "the address the proxy forwarded was not honoured"

            # 4b. Velocity: the proxy answers the signed request itself, and the
            # name it authenticates replaces the one Login Start claims.
            proc, port = start(config_dir, ["--velocity-forwarding",
                                            "--velocity-secret", SECRET])
            procs.append(proc)
            proxy = proxy_live.Proxy((HOST, port), mode="velocity", secret=SECRET,
                                     uuid=VELOCITY_UUID, username=VELOCITY_LIVE_NAME,
                                     props=PROPS, client_address="203.0.113.7")
            proxy.start()
            try:
                client = wire_client.RawClient(HOST, proxy.port, timeout=10)
                client.handshake(2, PROTO, host="backend.example")
                name = b"VelLive"
                client.send(0x00, varint(len(name)) + name + b"\x00" * 16)
                got_uuid, got_name, got_props = read_login_success(client)
                assert got_uuid == VELOCITY_UUID, \
                    f"uuid not forwarded by the proxy: {got_uuid.hex()}"
                assert got_name == VELOCITY_LIVE_NAME, \
                    "the proxy's authenticated name did not replace Login " \
                    f"Start's: {got_name!r}"
                assert [tuple(p) for p in got_props] == [(n, v, s) for n, v, s in PROPS], \
                    got_props
                client.close()
                print("live proxy: the signed Velocity identity replaces the "
                      "client's own claim")
            finally:
                proxy.close()
            proc.terminate()
            assert proc.wait(timeout=5) == 0, "expected clean exit on SIGTERM"
    except BaseException:
        for p in procs:
            if p.poll() is None:
                p.kill()
                p.wait()
        if procs and procs[0].stdout:
            sys.stderr.write("--- plusmc log ---\n"
                             + procs[0].stdout.read().decode("utf-8", "replace")
                             + "\n------------------\n")
        raise
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
