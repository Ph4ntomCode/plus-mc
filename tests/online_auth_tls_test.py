#!/usr/bin/env python3
"""The session-server lookup must verify TLS (online mode / premium login).

The server's own HTTP client is the one place the premium path talks to a
third party, and it is the one place a wrong answer is invisible on the wire:
if the TLS certificate is not verified, *any* host that completes a handshake
can answer hasJoined and this server will hand out the identity in the reply.

So this test stands up a session server over **HTTPS with a self-signed
certificate** and points a `--online-mode` server at it through
`PLUSMC_AUTH_URL`. That stub answers 200 with a full profile for any name — it
is, by design, an impostor that a real client would trust if the server did
not check the chain. The assertion is the refusal:

  * the joining client gets the encrypted "Authentication servers are
    unreachable. Try again later." disconnect (the TLS layer failed, which is
    read as an unreachable server), never a Login Success;
  * the stub's handler saw **no request at all** — the handshake was aborted
    before the HTTP request, which is what verification means;
  * the reason is the unreachable one and not "Failed to verify username!"
    (that is the 204 path, i.e. a lookup that *was* answered).

The plain-HTTP stub path (a test-only override) stays exercised by
`join-online`; this file is the https half of the same code.

Requires the python 'cryptography' package (client-side RSA/AES, via
online_join_test) and an `openssl` binary to produce the certificate.

Usage: online_auth_tls_test.py /path/to/plusmc
"""
import http.server
import json
import os
import pathlib
import re
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import online_join_test as OJ  # noqa: E402  (the encryption-capable client, the profile)
import wire_client  # noqa: E402  (start_server, the shell-free process helpers)

BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
HOST = "127.0.0.1"


class HttpsSessionServer:
    """A session server that would authenticate anybody — over TLS with a
    self-signed certificate.

    `requests` stays empty when the client (correctly) refuses the chain: the
    handler is only reached through a completed handshake.
    """

    def __init__(self, cert: pathlib.Path, key: pathlib.Path):
        self.requests = []
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                owner.requests.append(self.path)
                body = json.dumps(OJ.fake_profile("TlsImpostor")).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):  # silence
                pass

        class Server(http.server.ThreadingHTTPServer):
            # A client that aborts the handshake (the expected outcome here)
            # must not print a traceback into the test's own output.
            def handle_error(self, request, client_address):
                pass

        self.httpd = Server((HOST, 0), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=str(cert), keyfile=str(key))
        self.httpd.socket = ctx.wrap_socket(self.httpd.socket, server_side=True)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    @property
    def port(self):
        return self.httpd.server_address[1]

    def start(self):
        self.thread.start()

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def make_self_signed(directory: pathlib.Path):
    """(cert, key) paths for a fresh self-signed certificate, or None when no
    openssl binary is available (the test then skips)."""
    openssl = shutil.which("openssl")
    if not openssl:
        return None
    cert = directory / "cert.pem"
    key = directory / "key.pem"
    proc = subprocess.run(
        [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", str(key), "-out", str(cert), "-days", "2",
         "-subj", "/CN=localhost",
         "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if proc.returncode != 0 or not cert.is_file() or not key.is_file():
        return None
    return cert, key


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


def join_with_stub(port: int) -> str:
    """Drives the full encrypted login against `port` and returns the login
    disconnect's text (a refused join is the only acceptable outcome here)."""
    client = OJ.EncryptedClient("TlsImpostor", port)
    OJ.handshake_and_login_start(client)
    pub_der, token = OJ.expect_encryption_request(client)
    OJ.send_encryption_response(client, pub_der, token)
    pid, body = client.read_pkt()
    assert pid == 0x00, f"expected an encrypted Login Disconnect, got 0x{pid:02x}"
    msg, _ = OJ.rd_string(body)
    client.close()
    return json.loads(msg)["text"]


def main() -> None:
    if not OJ.HAVE_CRYPTO:
        print("SKIP: python 'cryptography' not importable")
        return
    if BIN is None:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    # This test's whole subject is the server's side of the lookup — the TLS
    # client it refuses the impostor with — and neither that client nor the
    # impostor's own socket is in a transcript, so a replay would re-examine
    # nothing. It says so rather than passing quietly.
    wire_client.refuse_replay(
        "the subject is the server's TLS client and the session server it refuses; "
        "neither handshake is on the client's wire")
    data_root = pathlib.Path(__file__).resolve().parent.parent / "data" / "vanilla"
    assert data_root.is_dir(), "no data/vanilla extraction present; run tools/vanilla-data"

    with tempfile.TemporaryDirectory(prefix="plusmc-tls-") as tmp:
        material = make_self_signed(pathlib.Path(tmp))
        if material is None:
            print("SKIP: no openssl binary to mint a certificate")
            return
        cert, key = material
        fake = HttpsSessionServer(cert, key)
        fake.start()
        env = dict(os.environ)
        env["PLUSMC_AUTH_URL"] = f"https://{HOST}:{fake.port}/session/minecraft/hasJoined"
        env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
        proc, port = wire_client.start_server(
            [str(BIN), "--host", HOST, "--port", "0", "--online-mode", "--view-distance", "1",
             "--data", str(data_root)],
            wait_listening_line, env=env)
        try:
            reason = join_with_stub(port)
            assert "unreachable" in reason.lower(), (
                f"a self-signed session server must not be trusted; got reason {reason!r}")
            print(f"self-signed session server: OK (refused, {reason!r})")
            # The strongest half of the statement: no HTTP request reached the
            # impostor. It answered nothing, because the handshake never
            # completed — verification aborted it.
            assert not fake.requests, (
                f"the untrusted session server was asked anyway: {fake.requests}")
            print("untrusted session server: OK (no request reached it)")

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
