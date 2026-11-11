#!/usr/bin/env python3
"""The shared wire-test client: frame I/O and a background packet reader.

Every wire test in this directory grew its own copy of the same five framing
functions (`recv_exact`, `recv_varint_bytes`, `make_frame`, `read_frame`,
`read_packet`), and every one of them read the socket *inside a drain window*:

    end = time.time() + timeout
    while time.time() < end:
        bot.sock.settimeout(end - time.time())
        pid, body = bot.recv()      # <-- can time out mid-frame

That is where a wire test desynchronises. `recv_exact` accumulates a packet a
few bytes at a time, and a `socket.timeout` in the middle throws the accumulator
away: the bytes already taken off the socket are gone, the next read resumes in
the middle of a packet, and every packet after it is garbage. The failure is a
function of machine load and of where the window happens to fall, which is why
it shows up as an intermittent failure rather than a reproducible one.

`Reader` fixes it by giving the read side to a daemon thread that owns the
socket while it runs:

  * it reads *whole frames* on a **blocking** socket, so no frame is ever
    abandoned half-read — a packet either arrives complete or has not arrived;
  * completed frames go on a queue, and a drain just pops what is there, so a
    window that closes early loses nothing and the next drain resumes exactly at
    the next packet boundary;
  * keep-alives are answered on that thread (an ack must not wait for a test to
    happen to be draining), wrapped in a send lock, because two threads writing
    to one socket can interleave their bytes.

`Reader` is created in a Bot's constructor and started once the join's own
sequential reads are done (`Reader.start`), which is the one moment a test
knows the read side is idle. Everything after that — the whole play session —
goes through the queue.

And the same file carries the *transcript*: every packet a test's client sent or
received, in order, written to a file when the run ends — `record(bot)`. A
failure is an assertion about what the server said, and by the time you read the
traceback the server is gone, so `PLUSMC_TRANSCRIPT_DIR` turns the run into a
record that outlives it. The record can be read back (`Transcript.dump`, or
`python3 tests/wire_client.py FILE [--dir C|S] [--id 0x4c] ...`) and *replayed*:
`transport()` hands the client a `ReplaySocket` instead of a connection when
`PLUSMC_REPLAY` names a transcript, so the test re-executes its own decoding and
assertions over the same packets with no server at all — and hands each packet
over only when the replayed client asks for the read window it arrived in, so the
windows fall where they did live (`docs/PHASE3.md`, "A failure you can read
back").

Every socket test in this directory is on that path: its client's socket comes
from `transport`, the client is handed to `record`, and the server it would boot
comes from `start_server` (which starts none when replaying). A client only has
to offer one of the two packet pairs — `send`/`recv` or `send_pkt`/`read_pkt` —
and a test whose protocol is a few request/response packets can use `RawClient`
instead of hand-rolling one. What a transcript *cannot* cover is the server's own
side — its log, its files, its exit code, the session server it called — so a
test that checks those as well calls `skip_server_check` around them, and a test
whose whole subject is that side calls `refuse_replay`.
"""
import atexit
import io
import json
import os
import queue
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib


# --- the frame codec --------------------------------------------------------
#
# The canonical copy: the twenty private copies the tests carried were
# byte-for-byte equivalent, so a test can import these and delete its own.

def rd_varint(buf, pos=0):
    result, shift = 0, 0
    for _ in range(5):
        if pos >= len(buf):
            # The buffer ends inside this varint, i.e. the frame is not all here
            # yet. That is the same condition as "too long" for every caller —
            # they catch ValueError and wait for more bytes (`bench/botnet.py`'s
            # frame_of, the frame readers below) — so it must not surface as an
            # IndexError, which nothing handles: 200 bots' worth of reads hit a
            # split varint and took the soak harness down with it.
            raise ValueError("varint truncated")
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7
    raise ValueError("varint too long")


def varint(value):
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def pack_block_pos(x, y, z):
    """BlockPos.asLong, 26 bits of x | 26 of z | 12 of y, as the 8 wire bytes.

    Written unsigned on purpose: masking a *negative* x leaves a value above
    2**63 and `">q"` refuses it (`struct.error`), while `">Q"` writes the same
    eight bytes for every cell that fits — negative ones included.
    """
    packed = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
    return struct.pack(">Q", packed & 0xFFFFFFFFFFFFFFFF)


def unpack_block_pos(raw):
    """BlockPos.getX/getY/getZ — the inverse of `pack_block_pos`, signs and all.

    Java's reading of the packed long is `(int)((l << 38) >> 38)` per field, so
    this masks each field out and then puts its sign back. Both halves matter:
    `(raw >> 12) & 0x3FFFFFF` answers 67108863 for z = -1, and a plain
    `raw >> 38` is only right for x because it relies on Python's arithmetic
    shift — passing an unsigned (`">Q"`) read to it would answer billions. The
    first line normalises the argument so either read is fine, which is why the
    failing `tier-a-gate` floor check (four cells reported still air that had
    been set) cannot come back through a different reader.
    """
    raw &= 0xFFFFFFFFFFFFFFFF
    x = (raw >> 38) & 0x3FFFFFF
    z = (raw >> 12) & 0x3FFFFFF
    y = raw & 0xFFF
    if x & (1 << 25):
        x -= 1 << 26
    if z & (1 << 25):
        z -= 1 << 26
    if y & (1 << 11):
        y -= 1 << 12
    return x, y, z


def recv_exact(sock, n):
    chunks, got = [], 0
    while got < n:
        c = sock.recv(n - got)
        if not c:
            raise EOFError(f"EOF {got}/{n}")
        chunks.append(c)
        got += len(c)
    return b"".join(chunks)


def recv_varint_bytes(sock):
    out = b""
    while True:
        c = recv_exact(sock, 1)
        out += c
        if not (c[0] & 0x80):
            return out


def make_frame(payload, threshold):
    """Build a wire frame, honoring the compression threshold (`-1` disables)."""
    if threshold < 0:
        return varint(len(payload)) + payload
    if len(payload) < threshold:
        body = varint(0) + payload
    else:
        body = varint(len(payload)) + zlib.compress(payload)
    return varint(len(body)) + body


def read_frame(sock, threshold):
    """Read one frame from `sock`, returning the raw packet bytes."""
    ln, _ = rd_varint(recv_varint_bytes(sock))
    if threshold < 0:
        return recv_exact(sock, ln)
    dl_raw = recv_varint_bytes(sock)
    data_len, nbytes = rd_varint(dl_raw)
    data = recv_exact(sock, ln - nbytes)
    if data_len != 0:
        data = zlib.decompress(data)
    return data


def read_packet(sock, threshold):
    """Read one packet, returning (id, body)."""
    frame = read_frame(sock, threshold)
    pid, off = rd_varint(frame)
    return pid, frame[off:]


def decode_frame(data, threshold):
    """The single frame in `data` as `(pid, body)`, for a *bytes* buffer.

    The mirror of `read_frame`/`read_packet` for a buffer already in memory;
    used to check what a replay's client just sent against what the transcript
    recorded it sending. Raises `ValueError` if `data` is not exactly one
    well-formed frame at that threshold.
    """
    ln, pos = rd_varint(data, 0)
    if pos + ln != len(data):
        raise ValueError(f"{len(data)} byte(s) is not one frame of {pos + ln}")
    if threshold < 0:
        payload = data[pos:]
    else:
        # `rd_varint` returns the position *after* the varint, so the body starts
        # at it — the same sum `read_frame` does, which is why it uses `nbytes`
        # from a buffer it started reading at zero.
        data_len, body_at = rd_varint(data, pos)
        body = data[body_at:]
        payload = zlib.decompress(body) if data_len != 0 else body
    pid, off = rd_varint(payload, 0)
    return pid, payload[off:]


# --- the reader thread ------------------------------------------------------

# The play-state keep-alive ids (src/protocol/ids.h): clientbound 0x2D,
# serverbound 0x1C. The reader is started after the join, so it only ever sees
# the play state — the configuration state's keep-alive is 0x04/0x04 and never
# reaches it.
CB_KEEP_ALIVE = 0x2D
SB_KEEP_ALIVE = 0x1C

# The ping/pong and the packet every test's own `tick` helper sends (src/protocol
# /ids.h: `kPlayPongCbId`, the serverbound template's ping_request and
# `flying` — the last carrying the on-ground flag as its one byte).
CB_PONG = 0x3F
SB_PING_REQUEST = 0x26
SB_FLYING = 0x21


# The payload of the next `barrier` ping. Monotonic so two barriers in one test
# cannot mistake each other's pong for their own.
_ping_payload = 0x1000


def barrier(bot, ticks=1, timeout=15.0):
    """Spend exactly `ticks` survival ticks and hand back everything the server
    sent for them, in order — and nothing generated after them.

    `handle_ping_request` echoes its payload, so a ping is a barrier: the pong
    cannot be written until the server has processed every packet sent before
    it, and TCP keeps the order. That is what turns "the effect expired
    somewhere in this drain" into a statement about a tick count — the packets
    below are counted, not timed, so neither a slow machine nor a fast one moves
    the answer. The ping is itself a recognized serverbound packet, so under
    `PLUSMC_SURVIVAL_CLOCK=packet` it is the *last* of the `ticks` counted here
    (a test whose server is on the wall clock should not use this: the ticks
    would then be wall time, which is the very thing a barrier exists to avoid).

    Returns the packets up to and including the pong. Raises `AssertionError`
    if no pong arrives, printing what did — a missing pong means the session or
    the stream is broken, not that the assertion under test failed.

    The timeout bounds a *failure*, not a success: the return is as soon as the
    pong is read. It is generous because the pong queues behind everything the
    server was already writing — a player who has just moved far enough to pull
    in new chunks has hundreds of them in flight, and a loaded machine takes a
    while to write and read them all. (Two seconds was not enough there; the
    ticks are still exact, because they are packets and not elapsed time.)
    """
    global _ping_payload
    assert ticks >= 1, "a barrier is at least its own ping"
    for _ in range(ticks - 1):
        bot.send(SB_FLYING, bytes([1]))
    _ping_payload += 1
    payload = _ping_payload
    bot.reader.start()  # idempotent: the reader may not have been needed yet
    bot.send(SB_PING_REQUEST, struct.pack(">i", payload))
    out = []
    deadline = time.time() + timeout
    while True:
        try:
            pid, body = bot.reader.pop(max(0.05, deadline - time.time()))
        except TimeoutError:
            raise AssertionError(
                f"no pong for {payload:#x} after {timeout:g}s, and {len(out)} "
                f"packet(s) arrived first: {[hex(p) for p, _ in out[-12:]]}") from None
        out.append((pid, body))
        if pid == CB_PONG and struct.unpack(">i", body[:4])[0] == payload:
            return out


class Reader:
    """Reads framed packets off `sock` on a daemon thread, into a queue.

    `threshold` is read afresh for every frame, so a Bot can keep updating the
    compression threshold the way its login does. `send` is the raw frame
    writer the reader shares with the Bot (see `Reader.send`), which is what
    makes the keep-alive ack safe to send from another thread.
    """

    def __init__(self, sock, bot, keep_alive=CB_KEEP_ALIVE,
                 keep_alive_reply=SB_KEEP_ALIVE):
        self.sock = sock
        self.bot = bot
        self.queue = queue.Queue()
        self.keep_alive = keep_alive
        self.keep_alive_reply = keep_alive_reply
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._started = False
        self._thread = None
        self.error = None
        # How many read windows this reader has been asked for. A window is one
        # `pop` or `collect` call — the set of packets a drain returned — and the
        # count is what a transcript records per packet and what a replay opens
        # in step (see `ReplaySocket`). Counted here, on both sides, so neither
        # the recording nor the replay has to know the other's timing.
        self.windows = 0

    @property
    def threshold(self):
        return self.bot.threshold

    @property
    def started(self):
        return self._started

    def send(self, pid, body=b""):
        """Send one packet, serialised against the reader thread's acks."""
        with self._lock:
            self.sock.sendall(make_frame(varint(pid) + body, self.threshold))

    def start(self):
        """Hand the read side to the thread. The socket goes blocking: the
        reader must be free to wait for a whole frame, and it is the only
        reader left once this returns."""
        if self._started:
            return
        self._started = True
        self.sock.settimeout(None)
        self._thread = threading.Thread(target=self._loop, name="wire-reader",
                                        daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def _loop(self):
        while not self._stop.is_set():
            try:
                # The Bot's own `recv`, so whatever a test books off a packet
                # while it is being read (the last position the server
                # teleported it to, say) keeps happening on the read path.
                pid, body = self.bot.recv()
            except (socket.timeout, TimeoutError):
                # Nothing should re-arm a timeout while the reader owns the
                # socket, and `start` left it blocking — so if this fires, the
                # read side is unusable: it may hold half a frame. Report it
                # rather than resuming in the middle of one. (This has to come
                # before the EOFError/OSError case below, since TimeoutError is
                # an OSError.)
                self.error = EOFError("the read side timed out mid-frame")
                self.queue.put(None)
                return
            except (EOFError, OSError) as exc:
                # A closed socket is how every session ends. Record it so a
                # later `pop` can report it instead of hanging, then leave.
                self.error = exc
                self.queue.put(None)
                return
            except Exception as exc:  # noqa: BLE001 — recorded, then re-raised on pop
                # Any other read-path failure — a truncated frame, a body that
                # will not inflate — must not leave the thread dead and the test
                # unaware. Same contract as the socket cases: the error is kept
                # so the next `pop` raises it with the traceback intact.
                self.error = exc
                self.queue.put(None)
                return
            if pid == self.keep_alive:
                self.send(self.keep_alive_reply, body[:8])
            self.queue.put((pid, body))

    def _window_marker(self):
        """Count a read window and, when the socket is a replay, open it."""
        self.windows += 1
        notify = getattr(self.sock, "on_window", None)
        if notify is not None:
            notify(self.windows)
        return self.windows

    def pop(self, timeout=None):
        """The next packet, or `TimeoutError` when the window closes first."""
        self._window_marker()
        if self.error is not None and self.queue.empty():
            raise EOFError(str(self.error))
        try:
            item = self.queue.get(timeout=timeout)
        except queue.Empty:
            raise TimeoutError("no packet in the window")
        if item is None:
            raise EOFError(str(self.error) if self.error else "reader stopped")
        return item

    def drain(self, timeout):
        """Every packet that arrives within `timeout` seconds."""
        return self.collect(deadline=time.time() + timeout)

    def quiet(self, quiet=0.15, limit=2.0):
        """Drain until the server has been quiet for `quiet` seconds.

        The fixed-window `drain` above waits out its whole window even when the
        server has already said everything it is going to say, and wall time is
        not free: the survival clock turns idle real seconds into game ticks, so
        a window that idles too long changes the thing under test. This returns
        as soon as the conversation stops.
        """
        return self.collect(deadline=time.time() + limit, quiet=quiet)

    def collect(self, deadline, quiet=None):
        """What arrives before `deadline`; with `quiet`, stop at the first gap
        of that many seconds instead of waiting the whole deadline out."""
        self._window_marker()
        packets = []
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                return packets
            if quiet is not None:
                remaining = min(remaining, quiet)
            try:
                item = self.queue.get(timeout=remaining)
            except queue.Empty:  # the window closed, or the server went quiet
                return packets
            if item is None:  # the read side ended
                return packets
            packets.append(item)


def drain(bot, timeout=1.5):
    """A drain that works before and after the reader starts.

    Before `Reader.start` the join is still reading the socket itself, so a
    window has to read directly (and sequentially, with no other reader).
    """
    reader = getattr(bot, "reader", None)
    if reader is not None and reader.started:
        return reader.drain(timeout)
    packets = []
    end = time.time() + timeout
    while time.time() < end:
        try:
            bot.sock.settimeout(max(0.05, end - time.time()))
            pid, body = bot.recv()
            packets.append((pid, body))
            if pid == CB_KEEP_ALIVE:
                bot.send(SB_KEEP_ALIVE, body[:8])
        except (socket.timeout, TimeoutError, EOFError):
            break
        finally:
            bot.sock.settimeout(10)
    return packets


def drain_quiet(bot, quiet=0.15, limit=2.0):
    """The quiet-window drain; see `Reader.quiet`."""
    reader = getattr(bot, "reader", None)
    if reader is not None and reader.started:
        return reader.quiet(quiet, limit)
    return drain(bot, limit)


# --- transcripts -------------------------------------------------------------
#
# A transcript is every packet one run's client sent or received, in order, as
# JSON Lines — one packet per line, so `grep` and `tail` work on it and two runs
# can be diffed.
#
# Why it exists: a wire test's failure is an assertion about what the *server*
# said, and by the time the assertion fires the server is gone. Re-running the
# test reproduces the machine's load, not the packet that was misread. A
# recorded run answers "what did it actually send there?" from a file, and the
# same file can be *replayed* through the test, so the reading and the
# assertions can be re-examined with no server at all. That is exactly how the
# "placement that never was" was diagnosed: a stream shifted by one byte decodes
# plausible cells nobody wrote, and only the server's side of a recording can
# say what it really sent (see docs/PHASE3.md).
#
# Two lines opt a test in, and both are inert unless asked for:
#
#   sock = wire_client.transport(HOST, port)   # real socket, or a replay
#   wire_client.record(bot, note="...")        # after the Bot is built
#
#   PLUSMC_TRANSCRIPT_DIR=/tmp/t   write /tmp/t/<stem>-<pid>.jsonl per run,
#                                 pass or fail (`PLUSMC_TRANSCRIPT` names one
#                                 file outright)
#   PLUSMC_REPLAY=/tmp/t/x.jsonl   run the same test against that file instead
#                                 of a server; `PLUSMC_REPLAY_STRICT=0` relaxes
#                                 the serverbound comparison

TRANSCRIPT_FORMAT = "plusmc-wire-transcript"
TRANSCRIPT_VERSION = 1


class Transcript:
    """An ordered log of one run's packets, both directions.

    A *decoded* packet — one that crossed the client's own framing, where the
    compression threshold in force is known — is `{"d": "C"/"S", "t", "c",
    "id", "b", "thr", "w"}`. A *raw* send is `{"d": "S", "t", "c", "raw"}`: the
    login handshake frame goes to the socket before the Bot has decoded
    anything, and is kept so a replay can compare it too.

    `c` is the connection's index. A run may open more than one (a viewer
    joining to watch the same world), and each replays from its own stream.
    `w` is the read window that returned a clientbound packet — the `Reader`
    call count at the moment it was popped — and `0` for the pre-reader exchange
    (the join), where every read is the client asking for the next packet. It is
    what lets a replay hand the packets back in the same groups a drain saw
    them, instead of all at once.
    """

    def __init__(self, note="", test=""):
        self.note = note
        self.test = test
        self.events = []
        self.clients = 0  # how many clients `record` has attached to this log
        # Clientbound events read but not yet returned by a `pop`/`collect`, in
        # read order: the queue `window` walks. Kept *per connection* — a run
        # may have two clients, and one of them reads on while the other is
        # between windows. Tagging them into one queue put the second client's
        # join burst into the first client's windows and shifted every window
        # after it, which is how a replay came to open the wrong four windows.
        self._pending = {}
        self._t0 = None

    def _stamp(self):
        now = time.monotonic()
        if self._t0 is None:
            self._t0 = now
        return round(now - self._t0, 6)

    def packet(self, direction, pid, body=b"", thr=-1, conn=1, t=None, w=None,
               ack=False):
        """One decoded packet. `ack=True` marks a keep-alive ack — a *reaction*
        the reader thread sends whenever a keep-alive arrives, so where it lands
        in the serverbound stream is a race between that thread and the test's
        own sends, and a replay counts it but does not compare it (see
        `ReplaySocket.sendall`)."""
        event = {"d": direction, "t": self._stamp() if t is None else float(t),
                 "c": conn, "id": pid, "b": bytes(body).hex(), "thr": thr,
                 "w": w}
        if ack:
            event["a"] = True
        self.events.append(event)
        if direction == "C" and w is None:
            self._pending.setdefault(conn, []).append(event)

    def window(self, conn, window, packets):
        """Mark the next `len(packets)` clientbound packets of `conn` as the
        ones read window `window` returned.

        Read order and queue order are the same order, so the packets a `pop`
        returned are the next unmarked ones — no matching by content, which
        would confuse two identical packets.
        """
        pending = self._pending.get(conn, [])
        count = min(len(packets), len(pending))
        for event in pending[:count]:
            event["w"] = window
        del pending[:count]

    def raw(self, data, conn=1, t=None):
        """A serverbound send that never crossed the client's framing."""
        self.events.append({"d": "S", "t": self._stamp() if t is None else float(t),
                            "c": conn, "raw": bytes(data).hex()})

    # --- views ---

    @property
    def connections(self):
        return sorted({e.get("c", 1) for e in self.events})

    def duration(self):
        return self.events[-1]["t"] if self.events else 0.0

    def packets(self, direction=None, conn=None):
        return [e for e in self.events
                if (direction is None or e["d"] == direction)
                and (conn is None or e.get("c", 1) == conn)]

    def clientbound(self, conn=None):
        """The decoded clientbound packets, in order — the replay's stream."""
        return [e for e in self.packets("C", conn) if "id" in e]

    def serverbound(self, conn=None):
        return self.packets("S", conn)

    def frames(self, conn=None):
        """The clientbound packets as wire frames, in order — the replay's
        bytes, re-framed at the threshold each one arrived under."""
        return [make_frame(varint(e["id"]) + bytes.fromhex(e["b"]), e["thr"])
                for e in self.clientbound(conn)]

    def frame_groups(self, conn=None):
        """The clientbound frames grouped by the read window that returned
        them: `[(window, [frame, ...]), ...]`, in order.

        A transcript with no window information at all — one written by hand,
        or before this field existed — is one group, window 0, which a replay
        serves as it is read; there is nothing better to do with it. The tail a
        run never popped is folded into the last window, because the client
        never asked for it and the last window is the close of the run.
        """
        events = self.clientbound(conn)
        tagged = [e for e in events if e.get("w") is not None]
        if not tagged:
            return [(0, self.frames(conn))]
        last = tagged[-1]["w"]
        groups = []
        for e in events:
            window = last if e.get("w") is None else e["w"]
            frame = make_frame(varint(e["id"]) + bytes.fromhex(e["b"]), e["thr"])
            if groups and groups[-1][0] == window:
                groups[-1][1].append(frame)
            else:
                groups.append((window, [frame]))
        return groups

    def serverbound_frames(self, conn=None):
        """What the client sent, in order, in the form a replay compares
        against: `("packet", id, body, threshold)`, `("raw", bytes)`, or
        `("ack", ...)` for a keep-alive ack, which a replay counts instead of
        comparing."""
        out = []
        for e in self.serverbound(conn):
            if "raw" in e:
                out.append(("raw", bytes.fromhex(e["raw"])))
            elif e.get("a"):
                out.append(("ack", e["id"], bytes.fromhex(e["b"]), e["thr"]))
            else:
                out.append(("packet", e["id"], bytes.fromhex(e["b"]), e["thr"]))
        return out

    def counts(self):
        """Packets per direction, per id (`"raw"` for an undecoded send)."""
        out = {"C": {}, "S": {}}
        for e in self.events:
            key = f"0x{e['id']:02x}" if "id" in e else "raw"
            out[e["d"]][key] = out[e["d"]].get(key, 0) + 1
        return out

    # --- persistence ---

    def save(self, path, note=None):
        """Write the transcript as JSON Lines, metadata first."""
        directory = os.path.dirname(os.path.abspath(path))
        if directory:
            os.makedirs(directory, exist_ok=True)
        meta = {"format": TRANSCRIPT_FORMAT, "version": TRANSCRIPT_VERSION,
                "test": self.test, "note": note if note is not None else self.note,
                "logged": time.strftime("%Y-%m-%d %H:%M:%S"),
                "events": len(self.events), "connections": self.connections}
        with open(path, "w") as fh:
            fh.write(json.dumps(meta) + "\n")
            for event in self.events:
                fh.write(json.dumps(event) + "\n")
        return path

    @classmethod
    def load(cls, path):
        with open(path) as fh:
            lines = [line for line in (raw.strip() for raw in fh) if line]
        if not lines:
            raise ValueError(f"{path}: empty transcript")
        meta = json.loads(lines[0])
        if meta.get("format") != TRANSCRIPT_FORMAT:
            raise ValueError(f"{path}: not a {TRANSCRIPT_FORMAT} file")
        if meta.get("version", 0) > TRANSCRIPT_VERSION:
            raise ValueError(f"{path}: format version {meta['version']} is newer "
                             f"than this reader ({TRANSCRIPT_VERSION})")
        transcript = cls(note=meta.get("note", ""), test=meta.get("test", ""))
        transcript.events = [json.loads(line) for line in lines[1:]]
        return transcript

    # --- the human view ---

    def dump(self, out=None, direction=None, conn=None, body_bytes=24,
             limit=None, only_id=None):
        """Print the packets, one per line: time, direction, id, length, body.

        This is what `python3 tests/wire_client.py FILE` runs. Ids stay numeric:
        a packet id only means something in a state (login, config, play), and
        the state is not in the file — a test's own decoder is what says what a
        body means, which is the point of replaying it.
        """
        out = out or sys.stdout
        events = self.packets(direction, conn)
        if only_id is not None:
            events = [e for e in events if e.get("id") == only_id]
        shown = events if limit is None else events[:limit]
        print(f"# {TRANSCRIPT_FORMAT} v{TRANSCRIPT_VERSION} "
              f"test={self.test or '?'} events={len(self.events)} "
              f"connections={self.connections} "
              f"duration={self.duration():.3f}s", file=out)
        if self.note:
            print(f"# {self.note}", file=out)
        for i, e in enumerate(shown):
            where = f"{i + 1:5d} c{e.get('c', 1)} {e['d']} +{e['t']:8.4f}s"
            if "id" in e:
                body = bytes.fromhex(e["b"])
                text = body[:body_bytes].hex() if body_bytes else ""
                tail = (f"... (+{len(body) - body_bytes} more)"
                        if len(body) > body_bytes else "")
                window = "-" if e.get("w") is None else str(e["w"])
                print(f"{where} id=0x{e['id']:02x} len={len(body):4d} "
                      f"thr={e['thr']:4d} w={window:>4} {text}{tail}", file=out)
            else:
                raw = bytes.fromhex(e["raw"])
                text = raw[:body_bytes].hex() if body_bytes else ""
                print(f"{where} raw len={len(raw):4d} {text}", file=out)
        if len(shown) != len(events):
            print(f"# {len(events) - len(shown)} more event(s) not shown", file=out)
        print(f"# counts: {self.counts()}", file=out)


# Every socket a test's client opened through `transport`, in order — what
# `replay_summary` reports over.
_connections = []
_transcripts = {}
_reporting = []

# Recordings by output path: one *run* is one file, so a test with two clients
# records both connections into it (tagged `c`) rather than each overwriting the
# other's file.
_recordings = {}


def _write_transcript(transcript, path):
    try:
        transcript.save(path)
    except OSError as exc:  # a dead disk must not turn into a test error
        print(f"[transcript] could not write {path}: {exc}", file=sys.stderr)
        return
    print(f"[transcript] {len(transcript.events)} event(s) over "
          f"{len(transcript.connections)} connection(s) -> {path}", file=sys.stderr)


# --- recording ---------------------------------------------------------------

class RecordingSocket:
    """A socket proxy that notes every byte sent through it.

    `sendall` is intercepted; everything else — `recv`, `settimeout`, `close`,
    `shutdown`, `fileno` — is forwarded, so a proxied socket stays usable by the
    Reader thread and by a test's own join code. A proxy is needed because
    Python's socket type does not allow its methods to be rebound (`sock.sendall
    = ...` raises `AttributeError: 'socket' object attribute 'sendall' is
    read-only`), and the login handshake is a send that goes straight to the
    socket — neither the Bot's `send` nor the reader ever sees it.

    The re-entrancy guard is shared with `record`'s own wrappers: a send that
    already logged itself as a decoded packet must not log again as raw bytes.
    """

    def __init__(self, sock, transcript, conn, guard):
        self._sock = sock
        self._transcript = transcript
        self._conn = conn
        self._guard = guard

    def sendall(self, data, *args, **kwargs):
        if not getattr(self._guard, "inside_send", False):
            self._transcript.raw(data, self._conn)
        return self._sock.sendall(data, *args, **kwargs)

    def recv(self, n, flags=0):
        return self._sock.recv(n, flags)

    def __getattr__(self, name):  # settimeout, close, shutdown, fileno, …
        return getattr(self._sock, name)


def _packet_method(bot, *names):
    """The first of `names` `bot` answers to, or `None`.

    The tests' clients come in two shapes: the `Bot` family (`send(pid, body)` /
    `recv()`, built on this module's `Reader`) and the clients that keep their
    own framing (`send_pkt` / `read_pkt` — join_test's and churn's clients, whose
    `read_pkt` is a question asked of a buffer they fill themselves). Both pairs
    mean the same thing to a transcript, so `record` wraps whichever one a client
    has instead of requiring one spelling.
    """
    for name in names:
        if callable(getattr(bot, name, None)):
            return name
    return None


def record(bot, path=None, note="", name=None):
    """Record every packet `bot` sends or receives into a `Transcript`.

    `bot` is any of the tests' clients — it needs `.sock` and one of the two
    packet pairs: `send(pid, body)` / `recv()` (the `Bot` family) or
    `send_pkt(pid, body)` / `read_pkt()` (the clients that own their framing).
    Both directions of whichever pair it has are wrapped, which is what the
    replay compares against and serves. A client with a `.reader` is wrapped
    there too (the keep-alive ack is sent from the reader's thread, and is a
    packet like any other; the reader's pops are what say which window returned
    which packet, so record a client *before* its join, while the reader is
    still idle). The client's own method and its reader's are wrapped together
    with a re-entrancy guard, so a send that travels through both is logged
    once; a send that never reaches either (the login handshake, written
    straight to the socket) is logged raw, which is why the socket's `sendall`
    is wrapped too.
    A client whose `send`/`recv` pair is not the packet pair it actually uses —
    one that writes raw frames itself — records only what it sends to the
    socket; `RawClient` is the shim for that shape.

    Where the file goes, in order of preference: `path`, `PLUSMC_TRANSCRIPT`,
    or `<PLUSMC_TRANSCRIPT_DIR>/<name>-<pid>.jsonl` — one file per run, so two
    concurrent runs of one test cannot overwrite each other. A second client
    recorded into the same path joins the same file as another connection
    rather than overwriting it, which is what a test with a viewer wants. The
    file is written at interpreter exit either way, so a run that *fails* leaves
    its transcript behind, which is the whole point. Returns the `Transcript`,
    which a test may also save itself; nothing is written when no path resolves.
    """
    if path is None:
        path = os.environ.get("PLUSMC_TRANSCRIPT")
        directory = os.environ.get("PLUSMC_TRANSCRIPT_DIR")
        if path is None and directory:
            stem = (name or os.environ.get("PLUSMC_TRANSCRIPT_NAME")
                    or os.path.splitext(os.path.basename(sys.argv[0]))[0] or "wire")
            path = os.path.join(directory, f"{stem}-{os.getpid()}.jsonl")
    if name is None:
        name = os.path.splitext(os.path.basename(sys.argv[0]))[0] or "wire"

    if path is not None and path in _recordings:
        # A second client of the same run joins the same file.
        transcript = _recordings[path]
        if note and note not in transcript.note:
            transcript.note = f"{transcript.note} | {note}" if transcript.note else note
    else:
        transcript = Transcript(note=note, test=name)
        if path is not None:
            _recordings[path] = transcript
            atexit.register(_write_transcript, transcript, path)
    # Connections are numbered per transcript, in the order clients are
    # recorded — which is the order `transport` hands them out during a replay,
    # so the nth client reads back the nth recorded stream.
    transcript.clients += 1
    conn = transcript.clients
    guard = threading.local()
    threshold = lambda: getattr(bot, "threshold", -1)

    send_name = _packet_method(bot, "send", "send_pkt")
    recv_name = _packet_method(bot, "recv", "read_pkt")
    if send_name is None or recv_name is None:
        raise TypeError(
            f"record: {type(bot).__name__} has no packet pair to wrap "
            f"(found send={send_name!r}, recv={recv_name!r}); give the client "
            "send/recv or send_pkt/read_pkt, or use RawClient")
    real_send, real_recv = getattr(bot, send_name), getattr(bot, recv_name)

    def send(pid, body=b""):
        transcript.packet("S", pid, body, threshold(), conn,
                          ack=(pid == SB_KEEP_ALIVE))
        guard.inside_send = True
        try:
            return real_send(pid, body)
        finally:
            guard.inside_send = False

    reader = getattr(bot, "reader", None)
    # Windows are numbered by *read attempt*, on one counter per connection,
    # because the two read paths are the same thing from different sides: a
    # direct read is a read, and a `pop` is a read of the queue the reader
    # filled. The join's reads come first (a client that never starts a reader —
    # `RawClient`, churn, persistence — keeps reading this way for the whole
    # session, since a read *is* how such a client asks for the next packet),
    # and a started reader carries the same counter on from there.
    #
    # This is what makes a direct read replayable: live it is read #n of the
    # connection, so the packet it returned is recorded as window n, and in a
    # replay the same call opens window n before blocking. Without that, every
    # direct read would be window 0 — always open — and a drain would be handed
    # the rest of the session at once (the failure that made `persistence`'s
    # placement assertions disappear under replay).
    direct = [0]

    def recv():
        if reader is not None and reader.started:
            # Read ahead by the reader thread; the packet belongs to whichever
            # window pops it, which the `pop`/`collect` wrappers below mark.
            window = None
        elif reader is not None:
            reader.windows += 1  # the join's reads are the connection's first
            window = reader.windows
        else:
            direct[0] += 1
            window = direct[0]
        if window is not None:
            # A replay: say which window this read is asking for, so the
            # recorded packets of that window become readable.
            notify = getattr(bot.sock, "on_window", None)
            if notify is not None:
                notify(window)
        pid, body = real_recv()
        transcript.packet("C", pid, body, threshold(), conn, w=window)
        return pid, body

    setattr(bot, send_name, send)
    setattr(bot, recv_name, recv)
    bot.sock = RecordingSocket(bot.sock, transcript, conn, guard)

    if reader is not None:
        real_reader_send = reader.send

        def reader_send(pid, body=b""):
            if not getattr(guard, "inside_send", False):
                transcript.packet("S", pid, body, threshold(), conn,
                                  ack=(pid == SB_KEEP_ALIVE))
            guard.inside_send = True
            try:
                return real_reader_send(pid, body)
            finally:
                guard.inside_send = False

        real_pop, real_collect = reader.pop, reader.collect

        def pop(*args, **kwargs):
            packet = real_pop(*args, **kwargs)  # raises when the window is empty
            transcript.window(conn, reader.windows, [packet])
            return packet

        def collect(*args, **kwargs):
            packets = real_collect(*args, **kwargs)
            transcript.window(conn, reader.windows, packets)
            return packets

        reader.send = reader_send
        reader.pop = pop
        reader.collect = collect

    return transcript


# --- replay ------------------------------------------------------------------

def replaying():
    """Is this run replaying a transcript instead of talking to a server?"""
    return bool(os.environ.get("PLUSMC_REPLAY"))


def replay_path():
    """The transcript this run replays, or `None`."""
    return os.environ.get("PLUSMC_REPLAY")


def _trace():
    """`PLUSMC_REPLAY_TRACE=1` prints each window a replay opens — how many
    packets it held and how many are still ahead — which is the first thing to
    read when a replayed test diverges: a window that was empty live and is not
    here (or the reverse) is a read-pattern difference, not a server one."""
    return os.environ.get("PLUSMC_REPLAY_TRACE", "") not in ("", "0")


class ReplaySocket:
    """A socket-shaped stand-in that serves a recorded run's packets.

    `transport` returns one of these instead of a connection when `PLUSMC_REPLAY`
    names a transcript. Everything above it — the Bot, its `Reader`,
    `read_packet`, `barrier`, `drain`, the test's own step code — runs unchanged,
    because this answers `recv`/`settimeout`/`sendall`/`close` the way a socket
    does.

    **It serves the packets in the windows the recording saw them in.** A
    window is one `Reader.pop`/`collect` call — the set a drain returned — and
    the `Reader` hands each one to this socket as it happens (`on_window`), so a
    packet is only readable once the replayed client has asked for the window it
    arrived in. That is what makes a replay a re-examination rather than a new,
    wrong run: a replay that dumped everything at once would make every "what
    arrived in this window" assertion see the rest of the session, and the first
    window-based step of the first test tried that way failed for exactly that
    reason. It is deterministic rather than a re-race — no packet's arrival is
    timed, so a loaded machine cannot split a burst differently — and it also
    reproduces *control flow*, since a loop that reads until it sees a packet
    sees it in the same window it did live.

    The rest of what it is:

      * `recv` serves whole frames and only raises *between* them, so a replay
        cannot desynchronise the way a windowed drain on a live socket could
        (that is the failure class this whole file exists to remove). Running out
        is a `socket.timeout` while a timeout is armed — what ends a drain
        early — and an `EOFError` on a blocking read, which is what stops the
        reader thread with a message saying the transcript ended;
      * a *barrier* still returns exactly the packets up to its own pong, in the
        recorded order (each of its pops is its own window), so the tick counts
        it asserts are the same numbers;
      * what the client sends is compared against the transcript's serverbound
        stream, because a replayed test that takes a *different* action is no
        longer re-examining the recorded failure. The first difference raises;
        `strict=False` (or `PLUSMC_REPLAY_STRICT=0`) replays anyway and keeps
        the differences in `.divergences`.

    One requirement: the client must read through the shared `Reader` (every
    test in this directory does), because that is what reports its windows.
    """

    def __init__(self, transcript, conn=1, strict=True):
        self.transcript = transcript
        self.conn = conn
        self.strict = strict
        groups = transcript.frame_groups(conn)
        self.frames = [frame for _window, frames in groups for frame in frames]
        self._windows = [window for window, frames in groups for _ in frames]
        self._by_window = {window: frames for window, frames in groups}
        self.sent = []
        self.divergences = []
        self.acks_recorded = 0  # keep-alive acks the run sent, not compared
        self.acks_sent = 0      # keep-alive acks this replay sent, not compared
        self._expected = transcript.serverbound_frames(conn)
        self._exp_i = 0
        self._frame_i = 0
        self._off = 0
        self._timeout = None

        # Window 0 is the pre-reader join, where each read *is* the request.
        self._open = {0}
        self._gate = threading.Condition()

    # --- the socket surface a test's client uses ---

    def on_window(self, window):
        """A read window the replayed client just asked for; `Reader` calls
        this. The packets of that window become readable now."""
        with self._gate:
            self._open.add(window)
            self._gate.notify_all()
        if _trace():
            print(f"[replay] c{self.conn} window {window}: "
                  f"{len(self._by_window.get(window, ()))} packet(s), "
                  f"{self._unserved()} still ahead", file=sys.stderr, flush=True)

    def _unserved(self):
        return len(self.frames) - min(
            len(self.frames), self._frame_i + (1 if self._off else 0))

    def recv(self, n, flags=0):
        while (self._frame_i < len(self.frames)
               and self._off >= len(self.frames[self._frame_i])):
            self._frame_i += 1
            self._off = 0
        if self._frame_i >= len(self.frames):
            if self._timeout is not None:
                raise socket.timeout("the transcript is exhausted")
            raise EOFError(f"the transcript ended after {len(self.frames)} "
                           "clientbound packet(s)")
        self._wait_for_window(self._windows[self._frame_i])
        frame = self.frames[self._frame_i]
        chunk = frame[self._off:self._off + n]
        self._off += len(chunk)
        return chunk

    def _wait_for_window(self, window):
        """Hold until the client asks for `window`; the reader thread is the
        only caller, and the client is the only thing that opens one."""
        with self._gate:
            if self._timeout is None:
                while window not in self._open:
                    self._gate.wait(1.0)
                return
            if window not in self._open:
                # A window with a timeout armed: wait it out like a socket with
                # nothing in it would, then say so.
                self._gate.wait(self._timeout)
                if window not in self._open:
                    raise socket.timeout("the transcript's next packet is in a "
                                         f"window ({window}) the client has not "
                                         "read yet")

    def sendall(self, data, *args, **kwargs):
        """Compare what the replayed test sends with what it sent live.

        A **keep-alive ack** is exempt in both directions. Live, the reader
        thread answers each clientbound keep-alive as it arrives, so whether its
        ack lands before or after the test's own next send is a race between two
        threads — the one thing in the serverbound stream that is not part of the
        path being re-examined. The recorded ack is counted and skipped when
        aligning, and an ack the replayed reader sends is counted and compared to
        nothing; every other send is compared in order.
        """
        data = bytes(data)
        self.sent.append(data)
        if self._is_ack(data):
            # The replayed reader's own reaction, counted and compared to
            # nothing (the recorded one is skipped below, wherever it landed).
            self.acks_sent += 1
            return None
        self._skip_recorded_acks()
        if self._exp_i >= len(self._expected):
            self._divergence(f"the test sent {len(data)} more byte(s) than the "
                             f"transcript records")
            return None
        expected = self._expected[self._exp_i]
        if expected[0] == "raw":
            self._exp_i += 1
            if data != expected[1]:
                self._divergence(f"sent {data[:24].hex()} where the transcript "
                                 f"recorded a raw frame {expected[1][:24].hex()}")
            return None
        _kind, pid, body, thr = expected
        try:
            got_pid, got_body = decode_frame(data, thr)
        except Exception as exc:  # noqa: BLE001 — a bad frame is a divergence,
            # not a crash: `decode_frame` can raise ValueError (not one frame),
            # IndexError (truncated) or zlib.error (an inflated body that is not
            # zlib). None of them means the replay should stop with a traceback.
            self._exp_i += 1
            self._divergence(f"sent {data[:24].hex()}, which is not a frame "
                             f"decodable at threshold {thr}: "
                             f"{type(exc).__name__}: {exc}")
            return None
        self._exp_i += 1
        if (got_pid, got_body) != (pid, body):
            self._divergence(f"sent 0x{got_pid:02x} ({len(got_body)} bytes) where "
                             f"the transcript recorded 0x{pid:02x} ({len(body)} "
                             f"bytes)")
        return None

    def _is_ack(self, data):
        """Is this frame a keep-alive ack? In whichever framing it was written:
        a plain frame (`decode_frame` at -1) or a compressed one (at 0). A
        genuine packet would have to decode to id 0x1c under *both* framings to
        be mistaken for an ack, which the one ack id in this protocol makes
        impossible in practice."""
        for thr in (-1, 0):
            try:
                pid, _body = decode_frame(data, thr)
            except Exception:  # noqa: BLE001 — not decodable this way, try the next
                continue
            if pid == SB_KEEP_ALIVE:
                return True
        return False

    def _skip_recorded_acks(self):
        """Step over the recorded keep-alive acks, counting them."""
        while (self._exp_i < len(self._expected)
               and self._expected[self._exp_i][0] == "ack"):
            self._exp_i += 1
            self.acks_recorded += 1

    def _divergence(self, message):
        line = f"replay divergence at send #{len(self.sent)}: {message}"
        self.divergences.append(line)
        if self.strict:
            raise AssertionError(
                line + "\n(the replayed test took a different path from the "
                "recorded run; set PLUSMC_REPLAY_STRICT=0 to replay the "
                "recorded packets anyway)")

    def settimeout(self, value):
        self._timeout = value

    def gettimeout(self):
        return self._timeout

    timeout = property(gettimeout, settimeout)

    def close(self):
        pass

    def shutdown(self, how=None):
        pass

    def summary(self):
        """One line on what this replay served, for the test's closing output."""
        served = min(len(self.frames), self._frame_i + (1 if self._off else 0))
        acks = f", {self.acks_sent} keep-alive ack(s) not compared" if self.acks_sent else ""
        return (f"connection {self.conn}: served {served} of "
                f"{len(self.frames)} clientbound packet(s), compared "
                f"{len(self.sent) - self.acks_sent} send(s){acks}, "
                f"{len(self.divergences)} divergence(s)")


def transport(host, port, timeout=10):
    """The socket a test's client opens: a real connection, or — when
    `PLUSMC_REPLAY` names a transcript — the recorded stream for this client.

    Each call is a connection: the nth one replays the nth recorded stream, so
    a test with two clients (one joining to watch another's world) replays both
    correctly, and a test that restarts its server and reconnects replays the
    connection it made after each restart. A test that still calls
    `socket.create_connection` directly is unaffected; it just cannot be
    replayed.
    """
    path = os.environ.get("PLUSMC_REPLAY")
    if path:
        if path not in _transcripts:
            _transcripts[path] = Transcript.load(path)
        transcript = _transcripts[path]
        conn = 1 + sum(1 for s in _connections
                       if isinstance(s, ReplaySocket) and s.transcript is transcript)
        sock = ReplaySocket(transcript, conn=conn,
                            strict=os.environ.get("PLUSMC_REPLAY_STRICT", "1") != "0")
        if _trace():
            # Which recorded stream this client got, in the order the run asked
            # for them: the first thing to read when a replay looks like it is
            # answering the wrong question.
            print(f"[replay] connection {conn} of "
                  f"{len(transcript.connections)} recorded", file=sys.stderr,
                  flush=True)
    else:
        sock = socket.create_connection((host, port), timeout=timeout)
    _connections.append(sock)
    if path and not _reporting:
        # One report for the run, however many connections it makes: what each
        # replay socket served and whether the client sent anything the
        # recording did not. Printed at exit so it is the last thing in a test's
        # output either way. (Nothing to do with `NoServer`: a test that starts
        # no server still reports what it replayed.)
        _reporting.append(True)
        atexit.register(_report_replay)
    return sock


class RawClient:
    """The smallest client that speaks the wire itself: `send(pid, body)`,
    `recv()`.

    For a test whose exchange is a handful of request/response packets and which
    never needs a `Reader` — the status ping and the login-plugin handshake.
    Those tests need exactly two things from a client: the packet pair `record`
    wraps and the socket `transport` returns, so this gives them both instead of
    a hand-rolled socket, which is what puts them on the transcript path too.

    `handshake(next_state)` is the one send that reaches the wire before any
    packet is decoded, and it writes a **zero port**: a server ignores that field,
    and a transcript that carried the live port could not be replayed on a
    machine that picked a different one.
    """

    def __init__(self, host, port, timeout=10):
        self.host = host
        self.sock = transport(host, port, timeout=timeout)
        self.sock.settimeout(timeout)
        self.threshold = -1

    def handshake(self, next_state, protocol, host=None, port=0):
        addr = (self.host if host is None else host).encode()
        self.sock.sendall(
            make_frame(varint(0) + varint(protocol) + varint(len(addr)) + addr
                       + struct.pack(">H", port) + varint(next_state), -1))

    def send(self, pid, body=b""):
        self.sock.sendall(make_frame(varint(pid) + body, self.threshold))

    def recv(self):
        """The next packet, as `(pid, body)`."""
        return read_packet(self.sock, self.threshold)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def skip_server_check(what):
    """Should a check on the *server* be skipped because this run replays a
    transcript? True, with a printed reason, when so.

    A transcript holds the wire: what one end sent and the other received. The
    server's own behaviour — its log lines, the files it wrote, the code it
    exited with, the session server it called — is not in it, and a replay has no
    server to produce any of it. A test that asserts both calls this and keeps
    the server-side assertion only when it returns False, so a replay says out
    loud what it did not re-examine instead of passing it by default.
    """
    if replaying():
        print(f"[replay] not checked: {what} (a transcript holds the wire, and "
              f"this run has no server)", file=sys.stderr, flush=True)
        return True
    return False


def refuse_replay(reason):
    """Stop a replay that could not re-examine what the test tests.

    The mirror of `skip_server_check` for a test whose *whole* subject is the
    server's side, or whose subject a single-threaded re-drive destroys (a
    concurrency stress, a restart that reads a region file back). Failing with
    the reason is the honest outcome: a pass here would claim the recording had
    been rechecked when nothing was.
    """
    if replaying():
        raise AssertionError(
            f"this recording cannot be replayed: {reason} (unset PLUSMC_REPLAY "
            "to run the test against a real server)")


def _report_replay():
    summary = replay_summary()
    if summary:
        print(f"[replay] {summary}", file=sys.stderr, flush=True)


class NoServer:
    """What a replayed run gets instead of a plusmc process.

    Enough of `subprocess.Popen`'s surface that a test's own start-up and
    shutdown code runs unchanged: `poll`/`wait`/`terminate`/`kill` do nothing,
    and `stdout` is empty, so a thread pumping the server's log ends at once
    instead of parking. Nothing reads a port from it — a replay has no port —
    which is why `start_server` never calls `wait_listening` either.
    """

    pid = 0

    def __init__(self):
        self.stdout = io.BytesIO(b"")
        self.stderr = self.stdout
        self.returncode = None

    def poll(self):
        return None

    def wait(self, timeout=None):
        return 0

    def terminate(self):
        pass

    def kill(self):
        pass

    def send_signal(self, *args):
        pass

    def __repr__(self):
        return "<no server: this run is replaying a transcript>"


# Natural spawning (`NaturalSpawner`, Phase 4 milestone C) is on by default in
# the server — vanilla's own `SPAWN_MOBS` floor — but a wire test is a statement
# about the feature it exercises, and a mob that spawns on its own is not part
# of that statement. It is a real source of nondeterminism: a cow that wanders
# under a thrown potion moves the break one block up (which is how
# `thrown-potion-wire` went red once per few runs before it turned spawning
# off), a natural spawn takes an entity id a test may be counting, and a mob can
# stand in the cell a test walks into or the block a test walks over. `start_server`
# therefore starts every wire test in a **mob-free world by default**: it puts
# `PLUSMC_MOB_SPAWNING=0` in the child's environment unless the caller set that
# variable itself. A test whose subject *is* the spawner asks for it the way
# `spawn_wire_test.py` does (`env["PLUSMC_MOB_SPAWNING"] = "1"`), and the knob
# is the one `src/main.cpp` documents. See also `tests/load_stability.py`.
MOB_FREE = "PLUSMC_MOB_SPAWNING"


def server_env(env):
    """The environment a wire-test server is started with: the caller's, with
    natural spawning off unless the caller chose otherwise (`MOB_FREE`).

    A copy is returned, so the caller's own dict is left as it was, and the
    default is installed with `setdefault`, so a test that turned spawning on
    keeps it on.
    """
    env = dict(os.environ if env is None else env)
    env.setdefault(MOB_FREE, "0")
    return env


class ServerProcess(subprocess.Popen):
    """A plusmc process whose `terminate()` is a clean shutdown on every platform.

    On POSIX `Popen.terminate()` sends SIGTERM and the server drains sessions,
    saves, and exits 0. Windows has no SIGTERM delivery: `Popen.terminate()`
    calls `TerminateProcess`, which the server cannot intercept, so a test that
    used it would see exit code 1 and an unsaved world. The equivalent is a
    console control event, and CTRL_BREAK_EVENT is the one that can be aimed at
    a single process group (`CTRL_C_EVENT` is delivered to every process on the
    console, including the test itself). The child must therefore be started
    with CREATE_NEW_PROCESS_GROUP; `start_process` adds it.

    Several tests name `signal.SIGTERM` directly, so `send_signal` maps that to
    the same event. Every other signal goes to `Popen` unchanged.
    """

    def terminate(self):
        if os.name == "nt":
            self.send_signal(signal.CTRL_BREAK_EVENT)
            return
        super().terminate()

    def send_signal(self, sig):
        if os.name == "nt" and sig == signal.SIGTERM:
            sig = signal.CTRL_BREAK_EVENT
        super().send_signal(sig)


def start_process(args, **popen_kwargs):
    """`ServerProcess` with the process-group flag its Windows shutdown needs.

    Use this instead of `subprocess.Popen` for a plusmc process that a test
    stops itself. Tests that call `start_server` get it automatically.
    """
    if os.name == "nt":
        popen_kwargs["creationflags"] = (
            popen_kwargs.get("creationflags", 0) | subprocess.CREATE_NEW_PROCESS_GROUP)
    return ServerProcess(args, **popen_kwargs)


def start_server(args, wait_listening, env=None, **popen_kwargs):
    """`(proc, port)` for a test's server: a real plusmc, or a stand-in when
    this run is replaying a transcript.

    `wait_listening` is the test's own port reader (`wait_listening(proc)`),
    called only when a server was really started. In a replay the return is
    `(NoServer(), 0)`: the port is never used, because `transport` ignores it
    and hands the client the recorded stream instead. That is what lets a test
    be re-run — assertions and all — with no server at all, which is the point
    of the transcript (`docs/PHASE3.md`, "A failure you can read back").

    `env` is the caller's environment, with natural spawning turned off by
    default (`server_env`/`MOB_FREE`); a test that wants the spawner passes its
    own `PLUSMC_MOB_SPAWNING`. `popen_kwargs` goes to `subprocess.Popen` (a test
    that reads its server's log as text passes `text=True`); `stdout`/`stderr`
    are this function's.
    """
    if replaying():
        print(f"[replay] no server will start; replaying {replay_path()}",
              file=sys.stderr, flush=True)
        return NoServer(), 0
    proc = start_process(args, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, env=server_env(env),
                         **popen_kwargs)
    return proc, wait_listening(proc)


def replay_summary():
    """What every replay in this run served, or `None` when not replaying."""
    sockets = [s for s in _connections if isinstance(s, ReplaySocket)]
    if not sockets:
        return None
    return "; ".join(s.summary() for s in sockets)


# --- the inspection CLI ------------------------------------------------------

def _cli(argv):
    import argparse

    ap = argparse.ArgumentParser(
        prog="wire_client.py",
        description="Inspect a wire transcript recorded by a socket test, so a "
                    "failure can be re-read (and replayed) without re-running "
                    "the server.")
    ap.add_argument("path", help="a transcript written by PLUSMC_TRANSCRIPT_DIR")
    ap.add_argument("--dir", choices=("C", "S"),
                    help="clientbound (C) or serverbound (S) only")
    ap.add_argument("--conn", type=int, help="one connection's stream")
    ap.add_argument("--id", help="one packet id, hex (0x4c) or decimal")
    ap.add_argument("--bytes", type=int, default=24,
                    help="body bytes to show per packet (0 for none)")
    ap.add_argument("--limit", type=int, help="show at most this many packets")
    args = ap.parse_args(argv)
    try:
        Transcript.load(args.path).dump(
            sys.stdout, direction=args.dir, conn=args.conn, body_bytes=args.bytes,
            limit=args.limit,
            only_id=None if args.id is None else int(args.id, 0))
    except BrokenPipeError:  # `| head` is how this is meant to be used
        pass
    finally:
        try:
            sys.stdout.close()
        except BrokenPipeError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
