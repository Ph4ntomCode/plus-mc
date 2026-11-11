#!/usr/bin/env python3
"""A cheap bot driver: N protocol clients on one thread, non-blocking.

`bench/soak.py`'s first engine gave every bot its own Python thread, which
described the server honestly and the *harness* dishonestly: at 60 bots the
threads and the sim were both queued on a 4-core box, and the measured
work-per-tick jumped 10x between 40 and 60 bots for reasons that had nothing to
do with the server (`docs/PHASE5.md` says so in the baseline table). The fix is
not less load, it is cheaper load: one thread, one `selectors` loop, N
non-blocking sockets. Per bot that is a socket, a few hundred bytes of buffer and
a state machine — so the bot side stops being the thing under test.

What each bot does is exactly what the threaded engine did, because both are the
same protocol walk: handshake -> login (compression + login success) ->
configuration (echo the known packs, acknowledge the finish) -> play (answer
keep-alives, stream movement at 20 Hz until the window closes). What changed is
only *who does the waiting*: the loop parks in `selectors.select` and serves
whichever socket is ready, instead of one OS thread blocking per connection.

The driver is deliberately not a test: it asserts almost nothing and reports what
happened (bots that joined, when, how many packets went out, which bots failed
and why), so `bench/soak.py` can decide what a run means.

It can also *time* a round trip. `Fleet(probe_period=S)` makes every bot send
`ServerboundPingRequest` (0x26) every S seconds once it is in play, and records
how long the `ClientboundPongResponse` (0x3E) that echoes its payload took to
come back. The server answers that packet in its handler, on the connection's own
thread (`session.cpp`'s `handle_ping_request` writes the pong immediately, it is
not queued to the tick), so the number is a real packet -> response time: the
kernel, both threads' scheduling, and this driver's own select loop are all
inside it. That is what §8's "player packet -> response within one tick" has to
be measured as, and `bench/soak.py --probe-period` is how it is reported. Nothing
is measured unless the caller asks for it (`probe_period=0` is the default), so a
run without probes is the same run it was before.

It also counts the world a client is handed. Every `level_chunk_with_light` a bot
receives is added to `Bot.chunks` with its uncompressed body length in
`Bot.chunk_bytes`, and `Fleet.snapshot()` reports both as `chunks_received` /
`chunk_body_bytes`. The server paces chunk sends (a fixed budget per 50 ms tick,
`SessionSettings::chunks_per_tick`), so "how many chunks does this client hold" is
a fact about a run rather than something geometry can be trusted for — and it is
the denominator the per-chunk RSS number needs (docs/PHASE5.md). Counting it costs
one comparison per inbound packet and changes nothing a run measures.

A *count* of sends is not, however, the set the server is holding. The same chunk
is re-sent to the same bot on a re-stream, and a cluster of bots at spawn is handed
the same chunk once per bot, so `chunks_received` over-counts the distinct chunks
by however many bots and re-streams there were — by 8x for a cluster of 8. The body
starts with the chunk's two int32 coordinates (`chunk.cpp`'s `encode_chunk_body`:
`u32(chunk.x)`, `u32(chunk.z)` before the heightmaps), so the exact set is one
`struct.unpack` and a set add away: `Bot.chunk_keys` is that set per bot and
`Fleet.snapshot()` reports `chunks_distinct` (the union over bots — the chunks this
run's clients were ever handed) and `chunk_keys_max` (the biggest single bot's
view). That union is the denominator a per-chunk RSS number can be divided by
without assuming geometry, which is the whole point of counting here.

Finally it can be one of *several* processes driving one server. The cost above is
per-packet Python, so a 200-mover population is capped by whichever single thread
is varint-decoding and decompressing every frame the server sends it — which is
what `bench/soak.py --bot-processes K` exists to lift (docs/PHASE5.md §6 item 1).
A worker joins its share, announces readiness, waits for the parent's window plan,
and measures exactly the interval the plan names; K of them share one absolute
window and one global spread grid, so the K fleets are one population and not K
overlapping clusters. It also carries the distinct chunks' *(x, z) pairs* back with
`--keys`, because the parent cannot count a union it cannot see: the spawn-area
stream every joining client gets means the K sets overlap whatever the layout, so
the parent unions the pairs rather than adding counts (`bench/soak.py`).
"""
import json
import math
import os
import pathlib
import selectors
import socket
import struct
import sys
import time
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tests"))
import wire_client  # noqa: E402  (the shared test client: varint helpers)

PROTO = 777
# Serverbound play ids (777, the same table the wire tests use).
P_POSITION = 0x1E
P_POSLOOK = 0x1F
P_LOOK = 0x20
# Clientbound play ids.
CB_SYNC_POS = 0x49
CB_KEEPALIVE = 0x2D
CB_PONG = 0x3F
# Serverbound ping_request (src/protocol/ids.h / packet_table.cpp: 0x26 in 777).
# Its payload is an int32 and the server echoes it, so the token is the clock.
SB_PING_REQUEST = 0x26
# Clientbound level_chunk_with_light in 26.3. The id is generated
# (src/protocol/packet_ids.generated.h: `{0x2e, "level_chunk_with_light"}`,
# which is what src/protocol/ids.h's kPlayChunkDataId resolves to).
CB_CHUNK_DATA = 0x2E

# The states a bot walks. The names are the packet it is waiting for; `PLAYING`
# is the steady state (movement out, keep-alives answered).
STATE_LOGIN = "login"
STATE_CONFIG = "config"
STATE_PLAY = "play"
STATE_PLAYING = "playing"
STATE_DONE = "done"
STATE_FAILED = "failed"


def frame_of(buf, threshold):
    """`(pid, body, consumed)` from the front of `buf`, or `None` when the frame
    is not all there yet. The compressed form is `[dataLength][data]`, where a
    dataLength of 0 means the payload is raw."""
    if not buf:
        return None
    try:
        length, nbytes = wire_client.rd_varint(buf, 0)
    except ValueError:
        return None
    if nbytes <= 0 or len(buf) < nbytes + length:
        return None
    raw = bytes(buf[nbytes:nbytes + length])
    consumed = nbytes + length
    try:
        if threshold < 0:
            pid, off = wire_client.rd_varint(raw)
            return pid, raw[off:], consumed
        declared, dn = wire_client.rd_varint(raw, 0)
        data = raw[dn:]
        if declared != 0:
            data = zlib.decompress(data)
        pid, off = wire_client.rd_varint(data)
        return pid, data[off:], consumed
    except (ValueError, zlib.error):
        return None


class Bot:
    """One connection's socket, buffers and state."""

    __slots__ = ("name", "tag", "sock", "inbuf", "out", "threshold", "state",
                 "joined_at", "sent", "keepalives", "failure", "next_send", "x", "z",
                 "announced",
                 "probes", "rtts", "next_probe", "probe_token", "probes_sent",
                 "probes_answered", "probes_orphan",
                 "chunks", "chunk_bytes", "chunk_keys", "recv_packets", "recv_bytes")

    def __init__(self, name, tag, x, z):
        self.name = name
        self.tag = tag
        self.sock = None
        self.inbuf = bytearray()
        self.out = bytearray()
        self.threshold = -1
        self.state = STATE_LOGIN
        self.joined_at = None
        self.sent = 0
        self.keepalives = 0
        self.failure = None
        self.next_send = 0.0
        self.x = x
        self.z = z
        # Whether this bot has told the server where it is. A client's position
        # only reaches the server in a position packet, so a bot that never
        # sends one is at the join position however it was placed locally.
        self.announced = False
        # Latency probes (module docstring): token -> the monotonic time it went
        # out, so a pong can be turned back into a round trip. `rtts` keeps
        # `(answered_at, rtt_ms)` for whatever window the caller drains.
        self.probes = {}
        self.rtts = []
        self.next_probe = 0.0
        self.probe_token = 0
        self.probes_sent = 0
        self.probes_answered = 0
        self.probes_orphan = 0
        # Streamed chunks this bot received, and the total *uncompressed* body
        # bytes of them (frame_of decompresses before the body length is taken).
        # This is the per-view memory experiment's denominator: the server paces
        # chunk sends (`SessionSettings::chunks_per_tick`, src/server/session.cpp),
        # so how many chunks a client actually holds is a fact about the run and
        # not something geometry can be trusted for (docs/PHASE5.md).
        self.chunks = 0
        self.chunk_bytes = 0
        # The *distinct* chunks behind that count: (cx, cz) pairs read out of the
        # bodies. A re-stream sends the same pair twice and a cluster sends one
        # pair once per bot, so this is the set the run is really holding a view
        # of — the denominator that geometry cannot supply and a packet count
        # over-counts (see the module docstring).
        self.chunk_keys = set()
        # Clientbound frames this bot actually read off the wire, and their
        # *uncompressed* sizes (the same convention as `chunk_bytes`; the
        # compressed length on the wire is not what the server spent time
        # writing). Nothing else in the harness counts inbound traffic, and it is
        # the one place a movement relay's fan-out is visible: a stance relayed
        # to N viewers is N writes to N sockets, so `packets_received` per bot per
        # second is the audience a mover's packets reached (docs/PHASE5.md).
        self.recv_packets = 0
        self.recv_bytes = 0

    def queue(self, pid, body=b""):
        payload = wire_client.varint(pid) + body
        if self.threshold < 0:
            frame = wire_client.varint(len(payload)) + payload
        else:
            inner = wire_client.varint(0) + payload
            frame = wire_client.varint(len(inner)) + inner
        self.out += frame
        self.sent += 1


class Fleet:
    """N bots served by one selector loop."""

    def __init__(self, host, port, count, period=0.05, connect_timeout=10.0, stationary=False,
                 spread=0.0, probe_period=0.0, grid_offset=0, grid_cols=None):
        self.host = host
        self.port = port
        self.period = period
        # 0 = no probes at all, and that is the default: a run without
        # `--probe-period` has to stay the run the recorded baseline was
        # measured with, so the probe may not add a packet or a field to it.
        self.probe_period = probe_period
        # `spread` places the bots on a square grid `spread` blocks apart instead
        # of in one cluster. The point is the *viewers*: a clustered mover's
        # stance is a candidate for every other bot, while a spread mover's
        # neighbours are all outside the 48-block player tracking range (a
        # distance-gated relay has nobody to write to there, which is what the
        # `packets_received` counter below measures directly). The
        # difference between the two layouts is therefore a measurement of
        # whether the fan-out is distance-aware at all (docs/PHASE5.md).
        self.spread = spread
        # `stationary` splits the two costs a soak mixes: a bot that stands
        # still costs entity ticks and (once anything moves) the broadcast
        # fan-out, while a player who keeps walking also costs chunk streaming.
        # Measuring both is how a per-tick number stops being an unexplained
        # sum (docs/PHASE5.md).
        self.stationary = stationary
        # A stationary bot still has to say something now and then: the socket
        # backstop is derived from the keep-alive (docs/PHASE1.md) and is 30 s at
        # the shipped 10 s + 15 s cadence, so a client that goes quiet for half a
        # minute is *correctly* disconnected — a 45 s run proved it the first
        # time this engine had a stationary mode. A run that raises
        # PLUSMC_KEEPALIVE_MS raises the backstop with it; the default is what
        # this harness measures, so the look every ten seconds stays.
        self.idle_period = 10.0
        # `grid_offset`/`grid_cols` place this fleet's bots inside a grid that is
        # bigger than the fleet: `bench/soak.py --bot-processes K` runs K drivers
        # against one server, and a spread population split over them must still
        # land on *one* grid — the same cells it would occupy as a single fleet —
        # or the K processes would measure K overlapping clusters instead of one
        # spread population. Both default to the single-fleet behaviour.
        self.grid_offset = grid_offset
        self.grid_cols = grid_cols
        self.bots = []
        if spread and spread > 0.0:
            # A square grid around the spawn area: bot i at (col, row) x spread.
            cols = grid_cols or max(1, int(math.ceil(math.sqrt(count))))
            for i in range(count):
                tag = grid_offset + i
                col, row = tag % cols, tag // cols
                self.bots.append(Bot(f"B{tag}", tag, 8.0 + col * spread, 8.0 + row * spread))
        else:
            for i in range(count):
                # The original cluster: every bot inside a few blocks of spawn,
                # the way the threaded engine places them, so the two engines
                # describe the same world state.
                self.bots.append(Bot(f"B{i}", i, 8.0 + (i % 8) * 0.5, 8.0 + (i % 5) * 0.5))
        self.sel = selectors.DefaultSelector()
        self.connect_timeout = connect_timeout
        self.failures = []
        self.connected = 0

    # --- setup ------------------------------------------------------------
    def connect_all(self):
        started = time.time()
        for bot in self.bots:
            try:
                # A blocking connect on loopback is a syscall, not a wait; the
                # sockets go non-blocking the moment they exist. Doing it the
                # other way (EINPROGRESS + a writable check) would add a state
                # to every bot for no measurement benefit.
                sock = socket.create_connection((self.host, self.port), timeout=5)
                sock.setblocking(False)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                bot.sock = sock
                self.sel.register(sock, selectors.EVENT_READ)
                self.connected += 1
                handshake = (wire_client.varint(PROTO) + wire_client.varint(len(self.host)) +
                             self.host.encode() + struct.pack(">H", self.port) +
                             wire_client.varint(2))
                bot.queue(0, handshake)
                bot.queue(0x00, wire_client.varint(len(bot.name)) + bot.name.encode() + b"\x00" * 16)
                self._want_write(bot)
            except OSError as exc:
                bot.state = STATE_FAILED
                bot.failure = f"{bot.name}: connect: {exc!r}"
                self.failures.append(bot.failure)
        return time.time() - started

    def _want_write(self, bot):
        events = selectors.EVENT_READ
        if bot.out:
            events |= selectors.EVENT_WRITE
        self.sel.modify(bot.sock, events)

    def relayout(self, spread, grid_offset=None, grid_cols=None):
        """Re-place the whole fleet on a grid `spread` blocks apart, mid-run.

        The paired form of the layout experiment: one server process, one
        population, two windows — first clustered, then spread — so the ambient
        load and any drift a shared box has fall on *both* arms instead of being
        compared across two runs (docs/PHASE5.md). The next flush carries the new
        cell in the ordinary movement packet; a stationary bot has to announce
        itself again, since a position only reaches the server in that packet.
        """
        offset = self.grid_offset if grid_offset is None else grid_offset
        cols = (grid_cols if grid_cols is not None else self.grid_cols)
        if not cols:
            cols = max(1, int(math.ceil(math.sqrt(len(self.bots)))))
        for i, bot in enumerate(self.bots):
            tag = offset + i
            col, row = tag % cols, tag // cols
            bot.x = 8.0 + col * spread
            bot.z = 8.0 + row * spread
            bot.announced = False

    # --- the loop ---------------------------------------------------------
    def run(self, until):
        """Drive every bot until `until` (a `time.monotonic` deadline)."""
        while True:
            now = time.monotonic()
            if now >= until and all(b.state in (STATE_DONE, STATE_FAILED) for b in self.bots):
                break
            timeout = 0.005
            for key, mask in self.sel.select(timeout):
                bot = next((b for b in self.bots if b.sock is key.fileobj and b.sock is not None),
                           None)
                if bot is None:
                    continue
                if mask & selectors.EVENT_READ:
                    self._read(bot)
                if mask & selectors.EVENT_WRITE:
                    self._write(bot)
            now = time.monotonic()
            if now >= until:
                self.flush(now)
                break
            self.flush(now)

    def flush(self, now):
        """Queue this tick's movement for every bot that is due, and answer
        keep-alives (already queued by `_read`)."""
        for bot in self.bots:
            if bot.state != STATE_PLAYING:
                continue
            if self.stationary:
                # A bot that stands still *at its own cell* is not a bot standing
                # at spawn: the server only learns where a client is from a
                # position packet, and the whole point of a spread population is
                # that its players are in different places. So the first thing a
                # stationary bot does is announce its cell once — without this a
                # spread run's stationary arm would still be measuring the one
                # shared spawn region (docs/PHASE5.md).
                if not bot.announced:
                    bot.announced = True
                    bot.next_send = now + self.idle_period
                    bot.queue(P_POSITION, struct.pack(">ddd", bot.x, 100.0, bot.z) + b"\x01")
                    continue
                # Then a look packet every ten seconds: inside the 30 s backstop
                # the default keep-alive derives, far outside anything a stream
                # would be.
                if now >= bot.next_send:
                    bot.next_send = now + self.idle_period
                    bot.queue(P_LOOK, struct.pack(">ff", 90.0 * bot.tag, 10.0) + b"\x01")
            elif now >= bot.next_send:
                bot.next_send = now + self.period
                bot.queue(P_POSITION, struct.pack(">ddd", bot.x, 100.0, bot.z) + b"\x01")
                bot.queue(P_POSLOOK, struct.pack(">ddd", bot.x, 100.2, bot.z) +
                          struct.pack(">ff", 45.0 * bot.tag, 0.0) + b"\x01")
                bot.queue(P_LOOK, struct.pack(">ff", 90.0 * bot.tag, 10.0) + b"\x01")
            # The probe rides whatever else this bot is doing (a stationary bot
            # has nothing else), and it is the only packet here whose *reply* is
            # timed. See the module docstring.
            if self.probe_period > 0 and now >= bot.next_probe:
                bot.next_probe = now + self.probe_period
                bot.probe_token = (bot.probe_token + 1) & 0x7FFFFFFF
                bot.probes[bot.probe_token] = now
                bot.probes_sent += 1
                bot.queue(SB_PING_REQUEST, struct.pack(">i", bot.probe_token))
        for bot in self.bots:
            if bot.sock is not None and bot.out:
                try:
                    self._write(bot)
                except KeyError:
                    pass

    def _write(self, bot):
        if not bot.out:
            self._want_write(bot)
            return
        try:
            sent = bot.sock.send(bot.out)
        except (BlockingIOError, InterruptedError):
            return
        except OSError as exc:
            self._fail(bot, f"{bot.name}: send: {exc!r}")
            return
        if sent:
            del bot.out[:sent]
        self._want_write(bot)

    def _read(self, bot):
        try:
            chunk = bot.sock.recv(65536)
        except (BlockingIOError, InterruptedError):
            return
        except OSError as exc:
            self._fail(bot, f"{bot.name}: recv: {exc!r}")
            return
        if not chunk:
            self._fail(bot, f"{bot.name}: unexpected EOF")
            return
        bot.inbuf += chunk
        while True:
            parsed = frame_of(bot.inbuf, bot.threshold)
            if parsed is None:
                return
            pid, body, consumed = parsed
            del bot.inbuf[:consumed]
            bot.recv_packets += 1
            bot.recv_bytes += consumed
            self._handle(bot, pid, body)

    def _handle(self, bot, pid, body):
        if bot.state == STATE_LOGIN:
            if pid == 0x03:  # set compression
                threshold, _ = wire_client.rd_varint(body)
                bot.threshold = threshold
            elif pid == 0x02:  # login success
                bot.queue(0x03)  # login acknowledged
                bot.state = STATE_CONFIG
            return
        if bot.state == STATE_CONFIG:
            if pid == 0x0F:  # known packs (26.3 clientbound) -> echo the same list back
                count, i = wire_client.rd_varint(body)
                packs = []
                for _ in range(count):
                    ns, i = self._string(body, i)
                    pk, i = self._string(body, i)
                    ver, i = self._string(body, i)
                    packs.append((ns, pk, ver))
                out = wire_client.varint(len(packs))
                for ns, pk, ver in packs:
                    out += (wire_client.varint(len(ns)) + ns.encode() +
                            wire_client.varint(len(pk)) + pk.encode() +
                            wire_client.varint(len(ver)) + ver.encode())
                bot.queue(0x07, out)
            elif pid == 0x03:  # finish configuration
                bot.queue(0x03)  # acknowledge
                bot.state = STATE_PLAY
            return
        if bot.state in (STATE_PLAY, STATE_PLAYING):
            if pid == CB_KEEPALIVE:
                payload, _ = wire_client.rd_varint(body)
                bot.queue(0x1C, wire_client.varint(payload))
                bot.keepalives += 1
            elif pid == CB_PONG and len(body) >= 4:
                # `handle_ping_request` echoed a probe back: the token is the one
                # we sent, so the difference is exactly one round trip.
                (token,) = struct.unpack(">i", body[:4])
                sent_at = bot.probes.pop(token, None)
                if sent_at is None:
                    bot.probes_orphan += 1
                else:
                    now = time.monotonic()
                    bot.rtts.append((now, (now - sent_at) * 1000.0))
                    bot.probes_answered += 1
            elif pid == CB_CHUNK_DATA:
                bot.chunks += 1
                bot.chunk_bytes += len(body)
                # int32 x, int32 z, then the chunk body (chunk.cpp's
                # encode_chunk_body writes them as u32 bit patterns, which is
                # the same two's-complement big-endian pair).
                if len(body) >= 8:
                    bot.chunk_keys.add(struct.unpack(">ii", body[:8]))
            elif pid == CB_SYNC_POS and bot.state == STATE_PLAY:
                bot.joined_at = time.monotonic()
                bot.state = STATE_PLAYING
                bot.next_send = time.monotonic()
                bot.next_probe = time.monotonic()

    @staticmethod
    def _string(body, i):
        length, i = wire_client.rd_varint(body, i)
        return body[i:i + length].decode("utf-8"), i + length

    def _fail(self, bot, message):
        bot.state = STATE_FAILED
        bot.failure = message
        self.failures.append(message)
        try:
            self.sel.unregister(bot.sock)
        except (KeyError, ValueError):
            pass
        try:
            bot.sock.close()
        except OSError:
            pass
        bot.sock = None

    # --- results ----------------------------------------------------------
    def snapshot(self, include_keys=False):
        """This fleet's counters as of now. `include_keys` adds the keys
        themselves, as sorted `[x, z]` pairs, under `chunk_keys`.

        A union over K processes cannot be recovered from K per-process *counts*:
        a bot is streamed the chunks around the world spawn before its own
        position packet has arrived, so every worker's set contains that block on
        top of its own cells, and the workers' sets overlap by more than any
        layout's cells do (measured: a single bot placed 512 blocks out is handed
        50 chunks at view distance 2 — its own 5x5 and spawn's 5x5). The parent
        therefore unions the pairs instead of guessing a rule from the counts
        (`bench/soak.py`'s `merge_worker_snapshots`). Off by default: only the
        worker path needs them, and at 200 players they are tens of thousands of
        pairs.
        """
        joined = [b for b in self.bots if b.joined_at is not None]
        keys = set()
        for b in self.bots:
            keys |= b.chunk_keys
        out = {
            "bots": len(self.bots),
            "connected": self.connected,
            "joined": len(joined),
            "stationary": self.stationary,
            "spread": self.spread,
            "keepalives": sum(b.keepalives for b in self.bots),
            "packets_sent": sum(b.sent for b in self.bots),
            "chunks_received": sum(b.chunks for b in self.bots),
            "chunk_body_bytes": sum(b.chunk_bytes for b in self.bots),
            # The union over bots, and the largest single bot's view: with them a
            # snapshot says both how big a view was and how much of the world all
            # of the views covered (the two are identical for one bot, and differ
            # by the layout for many).
            "chunks_distinct": len(keys),
            "chunk_keys_max": max((len(b.chunk_keys) for b in self.bots), default=0),
            "packets_received": sum(b.recv_packets for b in self.bots),
            "received_bytes": sum(b.recv_bytes for b in self.bots),
            "probes_sent": sum(b.probes_sent for b in self.bots),
            "probes_answered": sum(b.probes_answered for b in self.bots),
            "probes_pending": sum(len(b.probes) for b in self.bots),
            "probes_orphan": sum(b.probes_orphan for b in self.bots),
            "failures": list(self.failures),
        }
        if include_keys:
            out["chunk_keys"] = [[x, z] for x, z in sorted(keys)]
        return out

    def drain_rtts(self):
        """Every probe answered since the last call, oldest first, as
        `(answered_at, rtt_ms)` on the driver's own `time.monotonic` clock.

        Draining is how a caller says which window a number belongs to:
        `bench/soak.py` throws the join transient away, then takes the loaded
        window's samples, so a p99 is never a p99 of the join storm.
        """
        out = []
        for bot in self.bots:
            out.extend(bot.rtts)
            bot.rtts.clear()
        out.sort(key=lambda sample: sample[0])
        return out

    def close_all(self):
        for bot in self.bots:
            bot.state = STATE_DONE
            if bot.sock is None:
                continue
            try:
                self.sel.unregister(bot.sock)
            except (KeyError, ValueError):
                pass
            try:
                bot.sock.close()
            except OSError:
                pass
            bot.sock = None
        self.sel.close()


def worker(argv=None):
    """One driver process — alone, or as one of `bench/soak.py --bot-processes
    K`'s K workers against one server.

    `K` processes that each open their window "now" would not be measuring one
    interval, so the window is an absolute wall-clock stamp the parent hands to
    all of them: the worker joins its share, writes `--ready`, waits for the
    parent's `--go` file to carry the plan, and only then opens the window. With
    no `--ready`/`--go` it is the old smoke check:

        python3 bench/botnet.py <port> --bots 10 --seconds 5

    Returns the run's record (also written to `--json` when asked).
    """
    import argparse
    ap = argparse.ArgumentParser(prog="botnet.py", description=worker.__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", type=int)
    ap.add_argument("--bots", type=int, default=10)
    ap.add_argument("--seconds", type=float, default=5.0,
                    help="length of each measured window (default 5)")
    ap.add_argument("--stationary", action="store_true")
    ap.add_argument("--spread", type=float, default=0.0, metavar="BLOCKS")
    ap.add_argument("--grid-offset", type=int, default=0,
                    help="this process's first bot's index in the *global* grid")
    ap.add_argument("--grid-cols", type=int, default=None,
                    help="the global grid's width; every process must agree on it")
    ap.add_argument("--probe-period", type=float, default=0.0, metavar="SECONDS",
                    help="send a latency probe every SECONDS per bot (0 = none)")
    ap.add_argument("--relayout", type=float, default=None, metavar="BLOCKS",
                    help="the paired arm: re-place every bot BLOCKS apart when the "
                         "parent's plan says so, and measure a second window")
    ap.add_argument("--prewarm", action="store_true",
                    help="with --relayout, visit the second layout and come back "
                         "before the rendezvous, so neither measured window pays "
                         "for a teleport's streaming")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="seconds a prewarm leg is given (default 2)")
    ap.add_argument("--ready", default=None, metavar="PATH",
                    help="write this file once every bot is in play")
    ap.add_argument("--go", default=None, metavar="PATH",
                    help="wait for this file, then read the window plan from it")
    ap.add_argument("--keys", action="store_true",
                    help="carry the distinct chunks' (x, z) pairs in every snapshot, so "
                         "the parent can union K workers' sets exactly instead of "
                         "adding up counts (bench/soak.py's merge_worker_snapshots)")
    ap.add_argument("--json", default=None, help="where to write the record")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    fleet = Fleet("127.0.0.1", args.port, args.bots, stationary=args.stationary,
                  spread=args.spread, probe_period=args.probe_period,
                  grid_offset=args.grid_offset, grid_cols=args.grid_cols)
    t0 = time.monotonic()
    fleet.connect_all()
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        in_play = sum(1 for b in fleet.bots if b.joined_at is not None)
        if in_play + len(fleet.failures) >= args.bots:
            break
        fleet.run(min(time.monotonic() + 0.05, deadline))
    joined = sum(1 for b in fleet.bots if b.joined_at is not None)
    join_s = time.monotonic() - t0

    if args.prewarm and args.relayout is not None and not args.spread:
        fleet.relayout(args.relayout)
        fleet.run(time.monotonic() + args.settle)
        fleet.relayout(0.0)
        fleet.run(time.monotonic() + args.settle)

    if args.ready:
        pathlib.Path(args.ready).write_text("ready\n")
    plan = None
    if args.go:
        waiting_until = time.monotonic() + 300
        while time.monotonic() < waiting_until and plan is None:
            go = pathlib.Path(args.go)
            if go.exists():
                try:
                    plan = json.loads(go.read_text())
                except ValueError:
                    plan = None
            if plan is None:
                time.sleep(0.05)
        if plan is None:
            raise SystemExit("the parent never wrote a window plan")
    else:
        plan = {"at": time.time(), "relayout_at": None}

    def wait_until(unix_ts):
        """Hold the fleet (keep-alives still answered) until the plan's
        wall-clock stamp. The plan is wall clock and this driver's clock is
        `time.monotonic`, so the conversion happens here, once, rather than
        being smeared across the call sites."""
        target = time.monotonic() + max(0.0, unix_ts - time.time())
        if target > time.monotonic():
            fleet.run(target)
        return target

    windows = []
    start = wait_until(plan["at"])
    if args.probe_period > 0:
        fleet.drain_rtts()  # the join storm's probes are not the measurement
    before = fleet.snapshot(include_keys=args.keys)
    fleet.run(start + args.seconds)
    windows.append({"label": f"spread{args.spread:.0f}" if args.spread else "clustered",
                    "before": before, "after": fleet.snapshot(include_keys=args.keys),
                    "rtts_ms": [round(rtt, 2) for _, rtt in fleet.drain_rtts()]})

    relayout_at = plan.get("relayout_at")
    if args.relayout is not None and relayout_at:
        fleet.relayout(args.relayout)
        second = wait_until(relayout_at)  # the settle: the teleport streams first
        if args.probe_period > 0:
            fleet.drain_rtts()
        before2 = fleet.snapshot(include_keys=args.keys)
        fleet.run(second + args.seconds)
        windows.append({"label": f"spread{args.relayout:.0f}" if args.relayout else "clustered",
                        "before": before2, "after": fleet.snapshot(include_keys=args.keys),
                        "rtts_ms": [round(rtt, 2) for _, rtt in fleet.drain_rtts()]})

    record = {
        "pid": os.getpid(),
        "bots": len(fleet.bots),
        "joined": joined,
        "connected": fleet.connected,
        "join_s": round(join_s, 2),
        "grid_offset": args.grid_offset,
        "grid_cols": args.grid_cols,
        "stationary": args.stationary,
        "spread": args.spread,
        "relayout": args.relayout,
        "seconds": args.seconds,
        "windows": windows,
        "failures": list(fleet.failures),
    }
    fleet.close_all()
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(record) + "\n")
    if not args.quiet:
        received = sum(w["after"]["packets_received"] - w["before"]["packets_received"]
                       for w in windows)
        print(f"botnet: {joined}/{len(fleet.bots)} bots joined in {join_s:.2f} s "
              f"(grid offset {args.grid_offset}); {received} packets received over "
              f"{len(windows)} window(s)")
        for failure in fleet.failures[:5]:
            print(f"  {failure}")
    return record


if __name__ == "__main__":  # a single worker, or the smoke check
    sys.exit(0 if worker(sys.argv[1:]) else 1)
