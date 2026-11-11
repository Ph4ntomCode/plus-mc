#!/usr/bin/env python3
"""A minimal forwarding proxy, for the server to be tested *behind* (item 7).

`tests/proxy_forward_test.py` pins the proxy protocols from the client's side of
the wire: the raw client builds the forwarded handshake itself, which is the
format but not the traffic.  A real proxy terminates the client's connection,
writes the handshake the backend sees, and then relays the session; this module
is that proxy, small enough to read in one sitting:

  * ``bungeecord`` mode rewrites the handshake's server-address field to
    ``<host>\\0<client ip>\\0<uuid hex>[\\0<properties JSON>]`` -- the layout
    ``src/server/proxy.cpp`` parses (three or four NUL-separated fields) -- and
    then relays bytes in both directions;
  * ``velocity`` mode relays the client's own handshake and answers the
    backend's ``velocity:player_info`` Login Plugin Request with the signed blob
    (``[32-byte HMAC-SHA256][version][address][uuid][username][properties]``,
    the HMAC covering everything after itself), which is what
    ``server/proxy.cpp`` verifies.

Deliberately minimal: no compression handling past Set Compression, no
keep-alive, no reconnect, no plugin messages.  Its job is a second real TCP hop
with a real handshake rewrite -- not to be a product.  Used from
`tests/proxy_forward_test.py`; runnable by hand:

    python3 tests/proxy_live.py --backend-port 25565 --mode bungeecord --uuid aabb...
    python3 tests/proxy_live.py --backend-port 25565 --mode velocity --secret s3cret
    python3 tests/proxy_live.py --backend-port 25565 --mode bungeecord --uuid-seed soak
"""
import argparse
import hashlib
import hmac
import selectors
import socket
import struct
import sys
import threading
import time

# --- the protocol subset a proxy needs ---------------------------------------


def varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            byte |= 0x80
        out.append(byte)
        if not value:
            return bytes(out)


def rd_varint(buf, i=0):
    value = 0
    for k in range(5):
        if i >= len(buf):
            raise ValueError("varint past end")
        byte = buf[i]
        value |= (byte & 0x7F) << (7 * k)
        i += 1
        if not (byte & 0x80):
            return value, i
    raise ValueError("varint too long")


def mc_string(text):
    raw = text.encode("utf-8")
    return varint(len(raw)) + raw


def rd_string(buf, i=0):
    length, i = rd_varint(buf, i)
    return buf[i:i + length].decode("utf-8"), i + length


def read_exact(sock, count):
    """Exactly `count` bytes, or raise EOFError/OSError: a relay cannot guess."""
    chunks = []
    while count:
        chunk = sock.recv(count)
        if not chunk:
            raise EOFError("peer closed")
        chunks.append(chunk)
        count -= len(chunk)
    return b"".join(chunks)


def read_packet(sock):
    """One uncompressed frame's payload (the varint length is consumed)."""
    length = 0
    for k in range(5):
        byte = read_exact(sock, 1)[0]
        length |= (byte & 0x7F) << (7 * k)
        if not (byte & 0x80):
            break
    else:
        raise ValueError("frame length is not a varint")
    return read_exact(sock, length)


def frame(payload):
    return varint(len(payload)) + payload


def parse_handshake(payload):
    """`(protocol, address, port, next_state)`; raises ValueError on a non-0x00."""
    packet_id, i = rd_varint(payload)
    if packet_id != 0x00:
        raise ValueError(f"first packet is 0x{packet_id:02x}, not a handshake")
    protocol, i = rd_varint(payload, i)
    address, i = rd_string(payload, i)
    port = struct.unpack(">H", payload[i:i + 2])[0]
    i += 2
    next_state, i = rd_varint(payload, i)
    return protocol, address, port, next_state


def build_handshake(protocol, address, port, next_state):
    return (varint(0) + varint(protocol) + mc_string(address)
            + struct.pack(">H", port) + varint(next_state))


def velocity_response(secret, address, player_uuid, username, props):
    """The `velocity:player_info` payload `server/proxy.cpp` verifies."""
    rest = varint(1) + mc_string(address) + player_uuid + mc_string(username)
    rest += varint(len(props))
    for name, value, signature in props:
        rest += mc_string(name) + mc_string(value)
        rest += (b"\x01" + mc_string(signature)) if signature else b"\x00"
    mac = hmac.new(secret.encode("utf-8"), rest, hashlib.sha256).digest()
    return mac + rest


class _RelayLoop:
    """One `selectors` loop and the sessions assigned to it.

    `Proxy(relay_loops=N)` starts N. Each owns its selector, wake socket and
    session table, so a tick's ready set is split N ways instead of waiting
    behind one loop's whole batch. CPython's GIL still lets only one loop run
    at a time -- this does not make the relay parallel -- it bounds the queue
    in front of a given packet. Whether that lowers the tail is a measurement
    (`bench/soak.py --relay-loops`), not something this class assumes.

    The handshake threads cannot register sockets while a loop is selecting, so
    a pair is queued and the loop is woken, exactly as the single loop did.
    """

    def __init__(self):
        self.selector = selectors.DefaultSelector()
        self.wake_read, self.wake_write = socket.socketpair()
        self.wake_read.setblocking(False)
        self.wake_write.setblocking(False)
        self.selector.register(self.wake_read, selectors.EVENT_READ, ("wake", None))
        self.lock = threading.Lock()
        self.pending = []
        self.peer = {}
        self.outbox = {}
        self.want_write = set()
        # Pairs this loop was handed: a multi-loop run can assert the fleet
        # really was split rather than all landing on one loop.
        self.handoffs = 0
        self.closed = False
        self.started = False
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.started = True
        self.thread.start()

    def stop(self):
        with self.lock:
            self.closed = True
        try:
            self.wake_write.send(b"\0")
        except OSError:
            pass
        if self.started:
            self.thread.join(timeout=2.0)

    def dispose(self):
        try:
            self.selector.close()
        except (OSError, ValueError):
            pass
        for sock in (self.wake_read, self.wake_write):
            try:
                sock.close()
            except OSError:
                pass

    def enqueue(self, pair):
        with self.lock:
            self.pending.append(pair)
            self.handoffs += 1
        try:
            self.wake_write.send(b"\0")
        except OSError:
            pass

    def run(self):
        while not self.closed:
            try:
                events = self.selector.select(timeout=0.5)
            except (OSError, ValueError):
                break
            for key, mask in events:
                kind, _ = key.data
                sock = key.fileobj
                if kind == "wake":
                    try:
                        sock.recv(4096)
                    except OSError:
                        pass
                    self._drain_pending()
                    continue
                if mask & selectors.EVENT_READ:
                    self._relay_read(sock)
                if mask & selectors.EVENT_WRITE and sock in self.peer:
                    self._relay_flush(sock)
        self._close_all()
        self._drain_pending()

    def _drain_pending(self):
        with self.lock:
            pending, self.pending = self.pending, []
        for client, backend in pending:
            if self.closed:
                self._close_sockets(client, backend)
                continue
            for sock, peer in ((client, backend), (backend, client)):
                try:
                    sock.setblocking(False)
                    self.selector.register(sock, selectors.EVENT_READ,
                                           ("relay", None))
                except (OSError, ValueError):
                    self._close_sockets(client, backend)
                    break
                self.peer[sock] = peer
                self.outbox[sock] = bytearray()

    def _relay_read(self, sock):
        if sock not in self.peer:
            return
        try:
            data = sock.recv(65536)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            self._close_pair(sock)
            return
        if not data:
            self._close_pair(sock)
            return
        peer = self.peer.get(sock)
        if peer is None:
            return
        self.outbox[peer].extend(data)
        self._relay_flush(peer)

    def _relay_flush(self, sock):
        out = self.outbox.get(sock)
        if out is None:
            return
        while out:
            try:
                sent = sock.send(out)
            except (BlockingIOError, InterruptedError):
                break
            except OSError:
                self._close_pair(sock)
                return
            if sent <= 0:
                break
            del out[:sent]
        want = bool(out)
        if want == (sock in self.want_write):
            return
        if want:
            self.want_write.add(sock)
        else:
            self.want_write.discard(sock)
        try:
            self.selector.modify(
                sock, selectors.EVENT_READ
                | (selectors.EVENT_WRITE if want else 0), ("relay", None))
        except (KeyError, ValueError, OSError):
            pass

    def _close_pair(self, sock):
        self._close_sockets(sock, self.peer.get(sock))

    def _close_sockets(self, *sockets):
        for sock in sockets:
            if sock is None:
                continue
            self.peer.pop(sock, None)
            self.outbox.pop(sock, None)
            self.want_write.discard(sock)
            try:
                self.selector.unregister(sock)
            except (KeyError, ValueError, OSError):
                pass
            try:
                sock.close()
            except OSError:
                pass

    def _close_all(self):
        for sock in list(self.peer):
            self._close_sockets(sock)


class Proxy:
    """A listening proxy in front of `backend`: a thread per handshake, one
    selector loop (or `relay_loops` of them) for every relayed session.

    `mode` is ``"bungeecord"`` or ``"velocity"``.  In legacy mode `uuid_hex` is
    the 32-char undashed UUID the proxy vouches for and `properties_json` is the
    game-profile array BungeeCord appends (optional).  In velocity mode `secret`
    is the shared secret, `uuid` is 16 raw bytes, `username` is the name the
    proxy authenticates, and `props` is a list of `(name, value, signature)`
    triples.  `client_address` overrides the forwarded client IP (default: the
    address the proxy really sees the client at).

    `uuid_seed` is the fleet form: with it, every accepted connection is
    forwarded as its own UUID derived from the seed and the peer address,
    instead of `uuid_hex` / `uuid` for all of them.  A soak's bots are many
    players behind one proxy, not many attempts to log in as one player.

    `split_handshake` writes the backend handshake in two TCP segments -- the
    first byte, a pause, then the rest -- so a backend that assumes one read per
    handshake is caught rather than excused.

    The relay is one `selectors` loop over both sockets of every connection, not
    two blocking threads per client: a fleet of 200 movers used to be 400 relay
    threads, and their wakeups were part of the hop's measured cost. With
    `relay_loops` > 1 the sessions are spread over that many loops, so a tick's
    ready set is split instead of served by one (`_RelayLoop`; the GIL means
    this bounds each queue, it does not run the loops in parallel). The
    handshake (and, for Velocity, the login exchange) still runs in its own
    short-lived thread, because it is a stateful conversation with two endpoints;
    it hands the pair over as soon as the session is past login.
    """

    def __init__(self, backend, mode="bungeecord", uuid_hex=None, properties_json=None,
                 secret=None, uuid=None, username=None, props=(), client_address=None,
                 split_handshake=False, uuid_seed=None, relay_loops=1):
        if relay_loops < 1:
            raise ValueError("relay_loops must be at least 1")
        self.backend = backend
        self.mode = mode
        self.uuid_hex = uuid_hex
        self.uuid_seed = uuid_seed
        self.properties_json = properties_json
        self.secret = secret
        self.uuid = uuid
        self.username = username
        self.props = list(props)
        self.client_address = client_address
        self.split_handshake = split_handshake
        # How many connections really got the split write: the arm that asks for
        # it asserts this moved, so a split that silently took the other branch
        # cannot pass as coverage.
        self.splits = 0
        self.port = None
        self.address = "127.0.0.1"
        self._listener = None
        self._sockets = []
        self._lock = threading.Lock()
        self._closed = False
        # The relay loops. A handshake thread cannot register sockets on a
        # selector while its loop is selecting, so it queues the pair on one
        # loop and wakes it (round-robin here, so a joining fleet is spread).
        self._relay_loops = relay_loops
        self._loops = []
        self._next_loop = 0

    # --- lifecycle -----------------------------------------------------------

    def start(self):
        """Bind, listen and accept in a thread; returns the listening port."""
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((self.address, 0))
        listener.listen(64)  # a fleet joins at once; 8 is the single-client size
        self._listener = listener
        self.port = listener.getsockname()[1]
        self._loops = [_RelayLoop() for _ in range(self._relay_loops)]
        for loop in self._loops:
            loop.start()
        threading.Thread(target=self._accept_loop, daemon=True).start()
        return self.port

    def close(self):
        self._closed = True
        if self._listener is not None:
            try:
                self._listener.close()
            except OSError:
                pass
        for loop in self._loops:
            loop.stop()
        with self._lock:
            for sock in self._sockets:
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                try:
                    sock.close()
                except OSError:
                    pass
            self._sockets.clear()
        for loop in self._loops:
            loop.dispose()

    def _track(self, sock):
        with self._lock:
            self._sockets.append(sock)

    def _accept_loop(self):
        while not self._closed:
            try:
                conn, _ = self._listener.accept()
            except OSError:
                return
            conn.settimeout(15.0)
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    # --- one connection ------------------------------------------------------

    def _connection_uuid(self, client):
        """The UUID this connection is forwarded as, as (32-hex, 16 bytes).

        With `uuid_seed` it is the seed and the peer address through SHA-256's
        little sibling; without it, the one identity the proxy was constructed
        with (the single-client case every earlier test uses).
        """
        if self.uuid_seed is None:
            return self.uuid_hex, self.uuid
        peer = client.getpeername()
        digest = hashlib.sha1(
            f"{self.uuid_seed}:{peer[0]}:{peer[1]}".encode("utf-8")).digest()
        return digest[:16].hex(), digest[:16]

    def _handle(self, client):
        backend = None
        handed_off = False
        try:
            payload = read_packet(client)
            protocol, address, port, next_state = parse_handshake(payload)
            uuid_hex, uuid_bytes = self._connection_uuid(client)
            backend = socket.create_connection(self.backend, timeout=15.0)
            backend.settimeout(15.0)
            self._track(backend)
            self._track(client)

            if self.mode == "bungeecord":
                client_ip = self.client_address or client.getpeername()[0]
                address += "\x00" + client_ip + "\x00" + uuid_hex
                if self.properties_json:
                    address += "\x00" + self.properties_json
                payload = build_handshake(protocol, address, port, next_state)

            out = frame(payload)
            if self.split_handshake and len(out) > 1:
                backend.sendall(out[:1])
                time.sleep(0.05)
                backend.sendall(out[1:])
                self.splits += 1
            else:
                backend.sendall(out)

            if self.mode == "velocity":
                # Login Start follows in its own frame; the proxy relays it and
                # keeps the name so it can answer the plugin request with the
                # identity it authenticated (which may differ from the client's
                # own claim -- that is the substitution under test).  The login
                # exchange is the one part still done on this thread; once it is
                # past the plugin request both sockets go to the relay loop.
                login_start = read_packet(client)
                backend.sendall(frame(login_start))
                observed = self._login_name(login_start)
                if self._login_phase(backend, client, observed, uuid_bytes):
                    handed_off = self._handoff(client, backend)
            else:
                handed_off = self._handoff(client, backend)
        except (OSError, EOFError, ValueError, struct.error):
            pass
        finally:
            if not handed_off:
                for sock in (client, backend):
                    if sock is not None:
                        try:
                            sock.close()
                        except OSError:
                            pass

    def _handoff(self, client, backend):
        """Queue a logged-in pair on the next relay loop; False if closing."""
        with self._lock:
            if self._closed or not self._loops:
                return False
            loop = self._loops[self._next_loop % len(self._loops)]
            self._next_loop += 1
        loop.enqueue((client, backend))
        return True

    @staticmethod
    def _login_name(login_start):
        """Login Start's username (``[name][uuid]``), or "" if unreadable."""
        try:
            packet_id, i = rd_varint(login_start)
            if packet_id != 0x00:
                return ""
            name, _ = rd_string(login_start, i)
            return name
        except (ValueError, UnicodeDecodeError):
            return ""

    def _login_phase(self, backend, client, observed, uuid_bytes):
        """Backend -> client during login: answer the Velocity request, else relay.

        Returns True once the exchange is past the one packet this proxy
        understands (Login Success 0x02, Set Compression 0x03 or Disconnect
        0x00), so the caller can hand both sockets to the relay loop; False if
        either side went away first.  Everything after that point is relayed
        verbatim, including anything compressed.
        """
        try:
            while True:
                payload = read_packet(backend)
                packet_id, i = rd_varint(payload)
                if packet_id == 0x04:  # Login Plugin Request
                    msg_id, i = rd_varint(payload, i)
                    channel, i = rd_string(payload, i)
                    if channel == "velocity:player_info":
                        name = self.username or observed
                        data = velocity_response(self.secret, self.client_address or "127.0.0.1",
                                                 uuid_bytes, name, self.props)
                        body = varint(msg_id) + b"\x01" + varint(len(data)) + data
                        backend.sendall(frame(varint(0x02) + body))
                        continue
                client.sendall(frame(payload))
                if packet_id in (0x00, 0x02, 0x03):
                    return True
        except (OSError, EOFError, ValueError, struct.error):
            return False


# --- runnable by hand --------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="a minimal forwarding proxy for plusmc")
    ap.add_argument("--backend-host", default="127.0.0.1")
    ap.add_argument("--backend-port", type=int, required=True)
    ap.add_argument("--mode", choices=("bungeecord", "velocity"), default="bungeecord")
    ap.add_argument("--uuid", default=None,
                    help="legacy: 32-char undashed hex; velocity: dashed or undashed "
                         "(omit with --uuid-seed for a fleet)")
    ap.add_argument("--uuid-seed", default=None,
                    help="forward a distinct UUID per connection, derived from this seed")
    ap.add_argument("--properties-json", default=None,
                    help="legacy: the game-profile JSON array BungeeCord appends")
    ap.add_argument("--secret", default=None, help="velocity: the shared secret")
    ap.add_argument("--username", default=None, help="velocity: the authenticated name")
    ap.add_argument("--client-address", default=None,
                    help="the client IP the backend is told (default: the real peer)")
    ap.add_argument("--split-handshake", action="store_true",
                    help="write the backend handshake in two TCP segments")
    ap.add_argument("--relay-loops", type=int, default=1, metavar="N",
                    help="split the relayed sessions over N selector loops "
                         "(default 1: one loop, the layout every earlier record used)")
    args = ap.parse_args()
    if args.relay_loops < 1:
        print("--relay-loops must be at least 1", file=sys.stderr)
        return 2

    if args.uuid is None and args.uuid_seed is None:
        print("one of --uuid or --uuid-seed is required", file=sys.stderr)
        return 2
    legacy = args.uuid.replace("-", "") if args.uuid else None
    if legacy is not None and len(legacy) != 32:
        print("--uuid must be 16 bytes (32 hex chars, dashed or not)", file=sys.stderr)
        return 2
    if args.mode == "velocity" and not args.secret:
        print("--mode velocity needs --secret", file=sys.stderr)
        return 2

    proxy = Proxy((args.backend_host, args.backend_port), mode=args.mode,
                  uuid_hex=legacy, properties_json=args.properties_json,
                  secret=args.secret, uuid=bytes.fromhex(legacy) if legacy else None,
                  username=args.username, client_address=args.client_address,
                  split_handshake=args.split_handshake, uuid_seed=args.uuid_seed,
                  relay_loops=args.relay_loops)
    port = proxy.start()
    print(f"proxy: {args.mode} on 127.0.0.1:{port} -> "
          f"{args.backend_host}:{args.backend_port}", flush=True)
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        proxy.close()
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
