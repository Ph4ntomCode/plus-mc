#!/usr/bin/env python3
"""Online-mode login integration test for +MC (protocol 777 / 26.3).

Spawns a fake Yggdrasil session server (plain HTTP via PLUSMC_AUTH_URL) and a
plusmc instance in --online-mode, then acts as a raw Minecraft client that
speaks the full encryption handshake:

  1. premium join: Login Start -> Encryption Request -> Encryption Response
     (RSA-encrypted shared secret + verify token) -> AES/CFB8 stream -> Set
     Compression + Login Success with the Mojang profile -> configuration ->
     play. The fake server's hasJoined is queried with the correct server hash.
  2. bad verify token: the connection is closed silently (vanilla behaviour).
  3. not-joined player: hasJoined returns 204, the client receives an
     *encrypted* Login Disconnect ("Failed to verify username!").

Requires the python 'cryptography' package for client-side RSA + AES/CFB8
(the vanilla client would otherwise do this). Skips when it is unavailable.

Every client here takes its socket from `wire_client.transport` and is handed to
`wire_client.record`, so a failing run leaves a transcript:

Usage: online_join_test.py /path/to/plusmc
   or: PLUSMC_REPLAY=transcript.jsonl online_join_test.py   (no binary needed)

Replaying re-runs all three cases with no server. Two things are said out loud
rather than passed quietly: the hasJoined lookup is the *server's* call to the
session server (a transcript does not hold it, and a replay has no server to
make it), and the AES layer is skipped — the transcript stores the packets the
client read, i.e. after decryption, so what a replay re-examines is everything
above the cipher. The RSA step is made byte-stable on purpose
(`rsa_encrypt_fixed`) so the encrypted *serverbound* packets still compare
exactly.
"""
import base64
import http.server
import json
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
import join_test as JT  # noqa: E402  (login/config decoding helpers)
import wire_client  # noqa: E402  (the shared client: transport, record)

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    HAVE_CRYPTO = True
except ImportError:  # pragma: no cover - environment dependent
    HAVE_CRYPTO = False

BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
HOST = "127.0.0.1"
PROTO = 777
PREMIUM_UUID = "069a79f4-44e9-4726-a5be-fca90e38aaf5"

# Every read in this file blocks on the socket, and this timeout bounds a
# *failure* (a server that never answers), not a success: a read returns the
# moment its bytes land. Under a loaded `ctest -j4` the server can be several
# seconds behind the packet the client is waiting for, and a tight bound turns
# "slow" into "the transport timed out" — a red test whose server was merely
# busy. Ten seconds was tight enough to do exactly that; the bound is generous
# now, and this test's own loops (the 2000-packet tab-entry wait) are what stop
# it early when the server is healthy.
READ_TIMEOUT = 60


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
        byte = b[i]
        v |= (byte & 0x7F) << (7 * k)
        i += 1
        if not (byte & 0x80):
            return v, i
    raise ValueError("bad varint")


def rd_string(b: bytes, i: int = 0):
    ln, i = rd_varint(b, i)
    return b[i:i + ln].decode("utf-8"), i + ln


def make_frame(payload: bytes, threshold: int) -> bytes:
    if threshold < 0:
        return varint(len(payload)) + payload
    if len(payload) < threshold:
        body = varint(0) + payload
    else:
        body = varint(len(payload)) + zlib.compress(payload)
    return varint(len(body)) + body


# ---------------------------------------------------------------------------
# Fake Yggdrasil session server
# ---------------------------------------------------------------------------

def fake_profile(name: str):
    texture_value = base64.b64encode(
        json.dumps({"timestamp": 0, "profileId": PREMIUM_UUID.replace("-", ""),
                    "profileName": name, "textures": {}}).encode()).decode()
    return {"id": PREMIUM_UUID.replace("-", ""), "name": name,
            "properties": [{"name": "textures", "value": texture_value,
                            "signature": base64.b64encode(b"fake-sig").decode()}]}


class FakeSessionServer:
    """Serves /session/minecraft/hasJoined: 200 for the premium name, else 204."""

    def __init__(self):
        self.requests = []
        handler = self._make_handler()
        self.httpd = http.server.ThreadingHTTPServer((HOST, 0), handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    @property
    def port(self):
        return self.httpd.server_address[1]

    def _make_handler(self):
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                owner.requests.append(self.path)
                if "PremiumBob" in self.path:
                    body = json.dumps(fake_profile("PremiumBob")).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                else:
                    self.send_response(204)
                    self.send_header("Content-Length", "0")
                    self.end_headers()

            def log_message(self, *args):  # silence
                pass

        return Handler

    def start(self):
        self.thread.start()

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


# ---------------------------------------------------------------------------
# Encryption-capable raw client
# ---------------------------------------------------------------------------

class Enc:
    """AES-128-CFB8 with the secret as both key and IV; state persists."""

    def __init__(self, secret: bytes):
        self._dec = Cipher(algorithms.AES(secret), modes.CFB8(secret)).decryptor()
        self._enc = Cipher(algorithms.AES(secret), modes.CFB8(secret)).encryptor()

    def decrypt(self, data: bytes) -> bytes:
        return self._dec.update(data)

    def encrypt(self, data: bytes) -> bytes:
        return self._enc.update(data)


class Client:
    def __init__(self, name: str, port: int):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=READ_TIMEOUT)
        self.sock.settimeout(READ_TIMEOUT)
        self.threshold = -1
        self.enc = None
        self._plain = b""
        wire_client.record(self, note=f"online-join: the {name} login")

    def _feed(self, n: int) -> bytes:
        while len(self._plain) < n:
            raw = self.sock.recv(65536)
            if not raw:
                raise EOFError("connection closed")
            data = self.enc.decrypt(raw) if self.enc else raw
            self._plain += data
        out, self._plain = self._plain[:n], self._plain[n:]
        return out

    def _read_varint(self):
        raw = b""
        for _ in range(5):
            raw += self._feed(1)
            if not (raw[-1] & 0x80):
                v, _ = rd_varint(raw)
                return v
        raise ValueError("varint too long")

    def send_pkt(self, pkt_id: int, body: bytes = b""):
        frame = make_frame(varint(pkt_id) + body, self.threshold)
        if self.enc:
            frame = self.enc.encrypt(frame)
        self.sock.sendall(frame)

    def read_pkt(self):
        ln = self._read_varint()
        if self.threshold < 0:
            data = self._feed(ln)
        else:
            dl = self._read_varint()
            data = self._feed(ln - len(varint(dl)))
            if dl != 0:
                data = zlib.decompress(data)
        pid, off = rd_varint(data)
        return pid, data[off:]

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class EncryptedClient(Client):
    def enable_encryption(self, secret: bytes):
        if wire_client.replaying():
            # The recorded stream is what this client *read* — the decoded
            # packets — so in a replay it arrives already decrypted and the
            # cipher must be skipped, or every packet would be decrypted twice.
            # What a replay re-examines is therefore the protocol above AES, not
            # AES itself; the reply that establishes it is still sent and still
            # compared (see `rsa_encrypt_fixed`).
            return
        self.enc = Enc(secret)


def handshake_and_login_start(client: Client):
    hs = (varint(0) + varint(PROTO) + varint(len(HOST)) + HOST.encode() +
          struct.pack(">H", 0) + varint(2))
    client.sock.sendall(make_frame(hs, -1))
    client.send_pkt(0x00, varint(len(client.name)) + client.name.encode() + b"\x00" * 16)


def expect_encryption_request(client: EncryptedClient):
    pid, body = client.read_pkt()
    assert pid == 0x01, f"expected Encryption Request, got 0x{pid:02x}"
    i = 0
    server_id, i = rd_string(body, i)
    (pk_len, i) = rd_varint(body, i)
    pub_der = body[i:i + pk_len]
    i += pk_len
    (tok_len, i) = rd_varint(body, i)
    token = body[i:i + tok_len]
    i += tok_len
    assert server_id == "" and body[i] in (0, 1)
    return pub_der, token


def rsa_encrypt_numbers(modulus: int, exponent: int, message: bytes) -> bytes:
    """PKCS#1 v1.5 RSA encryption, with *deterministic* padding.

    `cryptography`'s `public_key.encrypt` randomises the padding block, so one
    message encrypts to different bytes on every run — which would make the
    encryption response the single serverbound packet a replayed run could not
    reproduce, and would leave the encrypted legs replayable only with the send
    comparison switched off. The padding is built by hand instead (0x00 0x02,
    nonzero padding, 0x00, message) and the RSA step is the same textbook
    computation, so the server decrypts it identically while the bytes stay
    stable across runs.

    Kept on plain integers so `check_fixed_rsa` below can verify the block —
    including the round trip through a private exponent — on a machine where
    the `cryptography` package is not installed at all.
    """
    size = (modulus.bit_length() + 7) // 8
    if len(message) > size - 11:
        raise ValueError(f"{len(message)} bytes will not fit in {size}")
    block = b"\x00\x02" + b"\xff" * (size - len(message) - 3) + b"\x00" + message
    return pow(int.from_bytes(block, "big"), exponent, modulus).to_bytes(size, "big")


def rsa_encrypt_fixed(pub_der: bytes, message: bytes) -> bytes:
    """`rsa_encrypt_numbers` against a DER SubjectPublicKeyInfo."""
    numbers = serialization.load_der_public_key(pub_der).public_numbers()
    return rsa_encrypt_numbers(numbers.n, numbers.e, message)


# A two-prime modulus small enough to check by hand and large enough for a
# PKCS#1 block (32 bytes) to hold the 16-byte shared secret.
_SMALL_P = 312876260060401910193112047967301532737
_SMALL_Q = 288828914180538290462582050619044333279
_SMALL_N = _SMALL_P * _SMALL_Q
_SMALL_E = 65537


def check_fixed_rsa() -> None:
    """Verify the deterministic RSA block without needing `cryptography`.

    Encrypts with the public exponent and decrypts with the private one, then
    checks the recovered block really has the 0x00 0x02 / nonzero padding /
    0x00 / message shape an OpenSSL `RSA_PKCS1_PADDING` decrypt insists on. This
    runs before the `cryptography` gate, so the one hand-rolled piece of crypto
    is exercised on every machine the test runs on rather than only where the
    package happens to be installed.
    """
    phi = (_SMALL_P - 1) * (_SMALL_Q - 1)
    private = pow(_SMALL_E, -1, phi)
    message = b"\x5e" * 16
    ciphertext = rsa_encrypt_numbers(_SMALL_N, _SMALL_E, message)
    assert len(ciphertext) == 32, len(ciphertext)
    recovered = pow(int.from_bytes(ciphertext, "big"), private,
                    _SMALL_N).to_bytes(32, "big")
    assert recovered[:2] == b"\x00\x02", recovered[:2].hex()
    separator = recovered.index(b"\x00", 2)
    padding = recovered[2:separator]
    assert len(padding) >= 8, len(padding)
    assert b"\x00" not in padding, "PKCS#1 padding must contain no zero byte"
    assert recovered[separator + 1:] == message, recovered[separator + 1:].hex()
    # And the same message twice really is the same bytes (that is the point).
    assert rsa_encrypt_numbers(_SMALL_N, _SMALL_E, message) == ciphertext
    print("deterministic PKCS#1 padding: OK (hand-checked round trip)")


def send_encryption_response(client: EncryptedClient, pub_der: bytes, token: bytes) -> bytes:
    secret = b"\x5e" * 16  # fixed secret makes failures reproducible
    ct_secret = rsa_encrypt_fixed(pub_der, secret)
    ct_token = rsa_encrypt_fixed(pub_der, token)
    client.send_pkt(0x01, varint(len(ct_secret)) + ct_secret + varint(len(ct_token)) + ct_token)
    client.enable_encryption(secret)
    return secret


def finish_login(client: Client) -> tuple:
    """After encryption is on (or not), finish login and return (uuid, name, nprops)."""
    saw_compression = False
    while True:
        pid, body = client.read_pkt()
        if pid == 0x03:  # Set Compression
            threshold, _ = rd_varint(body)
            client.threshold = threshold
            saw_compression = True
        elif pid == 0x02:  # Login Success
            u = body[:16]
            name, i = rd_string(body, 16)
            (nprops, i) = rd_varint(body, i)
            for _ in range(nprops):
                pn, i = rd_string(body, i)
                pv, i = rd_string(body, i)
                has_sig = body[i]
                i += 1
                if has_sig:
                    _, i = rd_string(body, i)
            client.send_pkt(0x03)  # login acknowledged
            return u, name, nprops, saw_compression
        elif pid == 0x00:  # Login Disconnect
            msg, _ = rd_string(body)
            raise AssertionError(f"login rejected: {msg}")
        else:
            raise AssertionError(f"unexpected login packet 0x{pid:02x}")


def decode_add_properties(body: bytes):
    """Parses a Player Info Update (0x47) body and returns [(name, [(prop_name,
    value, has_signature, signature), ...]), ...] for its add-player actions."""
    actions = body[0]
    count, i = rd_varint(body, 1)
    out = []
    for _ in range(count):
        i += 16  # uuid
        if actions & 0x01:
            name, i = rd_string(body, i)
            nprops, i = rd_varint(body, i)
            props = []
            for _ in range(nprops):
                pn, i = rd_string(body, i)
                pv, i = rd_string(body, i)
                has = body[i]
                i += 1
                ps = ""
                if has:
                    ps, i = rd_string(body, i)
                props.append((pn, pv, has, ps))
            out.append((name, props))
        # skip the remaining per-action payloads (masks in wiki order)
        if actions & 0x02:
            i += 1
        if actions & 0x04:
            _, i = rd_varint(body, i)
        if actions & 0x08:
            i += 1
        if actions & 0x10:
            _, i = rd_varint(body, i)
        if actions & 0x20:
            i += 1
            if body[i - 1]:
                ln, i = rd_varint(body, i)
                i += ln
        if actions & 0x40:
            _, i = rd_varint(body, i)
        if actions & 0x80:
            i += 1
    return out


def enter_play(client: Client):
    """Runs configuration and consumes play packets until Join Game (0x32)."""
    flags = {"packs": False, "registry": 0, "tags": False}
    while True:
        pid, body = client.read_pkt()
        if pid == 0x01:
            ch, i = rd_string(body)
            assert ch == "minecraft:brand"
            # BrandPayload body is writeUtf: [varint length][UTF-8]. Decode the
            # whole thing so a raw brand (which the client reads as a bogus
            # string length) fails here instead of on a real client.
            brand, i = rd_string(body, i)
            assert brand and i == len(body), f"bad brand payload: {body.hex()}"
        elif pid == 0x0D:  # update_enabled_features
            nfeat, i = rd_varint(body)
            feats = []
            for _ in range(nfeat):
                feat, i = rd_string(body, i)
                feats.append(feat)
            assert "minecraft:vanilla" in feats, f"feature flags: {feats}"
            assert i == len(body), f"feature flags: {len(body) - i} trailing byte(s)"
        elif pid == 0x0F:  # known packs -> echo
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
            client.send_pkt(0x07, out)
            flags["packs"] = True
        elif pid == 0x07:
            flags["registry"] += 1
        elif pid == 0x0E:  # update_tags
            flags["tags"] = len(body) > 100
        elif pid == 0x03:  # Finish Configuration
            client.send_pkt(0x03)
        elif pid == 0x32:  # Join Game
            # The server just authenticated us through Yggdrasil, so Login's
            # online-mode byte (vanilla: server.usesAuthentication()) must say
            # so. It used to be a hardcoded 0, so the client was told it was on
            # an offline server right after being authenticated.
            info = JT.decode_login_body(body)
            assert info["online_mode"] == 1, (
                f"premium join: login online-mode byte is {info['online_mode']}, "
                "expected 1")
            assert info["enforces_secure_chat"] == 0, info
            assert info["entity_id"] > 0 and info["chunk_radius"] > 0, info
            flags["login"] = info
            return flags
        elif pid == 0x20:
            raise AssertionError("play disconnect during join")
        # chunk batches, spawn position, abilities, ... are ignored


def premium_join(port: int) -> tuple:
    c = EncryptedClient("PremiumBob", port)
    handshake_and_login_start(c)
    pub_der, token = expect_encryption_request(c)
    secret = send_encryption_response(c, pub_der, token)
    uuid_raw, name, nprops, comp = finish_login(c)
    flags = enter_play(c)
    # Presence: the joining player receives its own tab entry (0x47) after the
    # chunk batch; online mode must carry the hasJoined "textures" property so
    # a real client can render the skin. 0x47 is 26.3's Player Info Update
    # (0x46 is its remove action), and the play stream after Join Game has only
    # finitely many packets, so a stale id waits forever rather than failing
    # loudly. Budget for the whole spawn burst plus slack: the tab entry arrives after
    # it, and the burst covers the full advertised view distance.
    textures = None
    for _ in range(2000):
        pid, body = c.read_pkt()
        if pid == 0x2D:
            c.send_pkt(0x1C, body[:8])  # ack keep-alive
            continue
        if pid == 0x47:
            for pname, props in decode_add_properties(body):
                for pn, pv, has, ps in props:
                    if pn == "textures":
                        textures = (pv, has, ps)
        if textures:
            break
    assert textures, "premium join: own tab entry did not carry the textures property"
    fake_tex = fake_profile("PremiumBob")["properties"][0]
    assert textures == (fake_tex["value"], True, fake_tex["signature"]), \
        f"textures property mismatch: {textures[:2]}..."
    print("premium join: textures property relayed in tab entry")
    c.close()
    return uuid_raw, name, nprops, comp, flags, pub_der, secret


def mc_hexdigest(secret: bytes, pub_der: bytes) -> str:
    """Reference implementation of the Java-style signed SHA-1 hexdigest."""
    import hashlib

    d = hashlib.sha1(b"" + secret + pub_der).digest()
    value = int.from_bytes(d, "big", signed=True)
    return ("-" + format(-value, "x")) if value < 0 else format(value, "x")


def bad_token_join(port: int):
    c = EncryptedClient("BadToken", port)
    handshake_and_login_start(c)
    pub_der, token = expect_encryption_request(c)
    secret = b"\x99" * 16
    ct_secret = rsa_encrypt_fixed(pub_der, secret)
    ct_token = rsa_encrypt_fixed(pub_der, b"\x00" * len(token))  # wrong token
    c.send_pkt(0x01, varint(len(ct_secret)) + ct_secret + varint(len(ct_token)) + ct_token)
    c.enable_encryption(secret)
    try:
        c.read_pkt()  # server must close without sending anything
        raise AssertionError("server did not close on a bad verify token")
    except EOFError:
        pass
    finally:
        c.close()


def not_joined(port: int) -> str:
    c = EncryptedClient("NotJoined", port)
    handshake_and_login_start(c)
    pub_der, token = expect_encryption_request(c)
    send_encryption_response(c, pub_der, token)
    pid, body = c.read_pkt()
    assert pid == 0x00, f"expected an encrypted Login Disconnect, got 0x{pid:02x}"
    msg, _ = rd_string(body)
    text = json.loads(msg)["text"]
    c.close()
    return text


def wait_listening_line(proc, timeout=15):
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
    check_fixed_rsa()
    if not HAVE_CRYPTO:
        print("SKIP: python 'cryptography' not importable")
        return
    if BIN is None and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    data_root = pathlib.Path(__file__).resolve().parent.parent / "data" / "vanilla"
    assert data_root.is_dir(), "no data/vanilla extraction present; run tools/vanilla-data"

    fake = FakeSessionServer()
    fake.start()
    env = dict(os.environ)
    env["PLUSMC_AUTH_URL"] = f"http://{HOST}:{fake.port}/session/minecraft/hasJoined"
    # This file's first case *does* reach the play state (it consumes packets
    # until Join Game), so the survival tick is pinned here too: one recognized
    # packet is one tick and idle wall time is none, which keeps the encrypted
    # join's own burst from being paced by however long the login took. The two
    # cases that never reach play are the bad-token disconnect and the
    # hasJoined-204 refusal, and they see no survival tick at all.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    # --view-distance 1 keeps the spawn burst at 3x3: this test is about the
    # premium auth/encryption path, not chunk volume (world_join_test covers
    # the full-radius burst).
    proc, port = wire_client.start_server(
        [str(BIN), "--host", HOST, "--port", "0", "--online-mode", "--view-distance", "1",
         "--data", str(data_root)],
        wait_listening_line, env=env)
    # `start_server` connects stdout to a pipe and only the listening line was
    # read. Once the server logs enough, the pipe fills and the server blocks
    # inside a write before it can finish the login; drain it for the rest of
    # the run, as the other long-lived join tests do.
    if not wire_client.replaying() and proc.stdout is not None:
        threading.Thread(target=lambda: list(proc.stdout), daemon=True).start()
    try:
        if not wire_client.replaying():
            deadline = time.time() + 10
            while True:
                try:
                    s = socket.create_connection((HOST, port), timeout=3)
                    s.close()
                    break
                except OSError:
                    if proc.poll() is not None:
                        raise AssertionError("plusmc exited early")
                    if time.time() > deadline:
                        raise AssertionError("could not reach plusmc")
                    time.sleep(0.05)

        uuid_raw, name, nprops, comp, flags, pub_der, secret = premium_join(port)
        expected = bytes.fromhex("069a79f444e94726a5befca90e38aaf5")
        assert uuid_raw == expected, f"uuid {uuid_raw.hex()} != Mojang profile uuid"
        assert name == "PremiumBob" and nprops >= 1, (name, nprops)
        assert comp and flags["packs"] and flags["registry"] >= 20 and flags["tags"]
        print("premium join: OK (encrypted login -> config -> play)")

        bad_token_join(port)
        print("bad verify token: OK (silently closed)")

        reason = not_joined(port)
        assert "Failed to verify username!" in reason, reason
        print("not-joined player: OK (encrypted disconnect: \"", reason, "\")")

        # The fake server must have seen a hasJoined query carrying the auth
        # digest. Cross-check the digest against an independent Python
        # implementation of the Java-style signed hex SHA-1. (The query is the
        # *server's* HTTP call — not a packet this client sent or received — so
        # a replay has nothing to re-examine here: no server, no lookup.)
        if not wire_client.skip_server_check(
                "the session server's hasJoined query and the digest it carried "
                "(the lookup is the server's own HTTP call, not a packet)"):
            hits = [p for p in fake.requests if "PremiumBob" in p and "serverId=" in p]
            assert len(hits) == 1, f"expected one hasJoined hit, saw {fake.requests}"
            assert "username=PremiumBob" in hits[0]
            import urllib.parse

            sent_hash = urllib.parse.parse_qs(hits[0].split("?", 1)[1])["serverId"][0]
            expect_hash = mc_hexdigest(secret, pub_der)
            assert sent_hash == expect_hash, f"digest {sent_hash} != reference {expect_hash}"
            print("hasJoined digest: OK (", sent_hash[:24], "...)")

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
        raise
    finally:
        fake.stop()

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
