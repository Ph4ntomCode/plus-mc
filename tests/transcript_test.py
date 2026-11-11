#!/usr/bin/env python3
"""Unit test for the wire client's transcripts (tests/wire_client.py).

No server, and nothing to start: a transcript is a file of packets, so all of
this runs in-process (plus two subprocesses — the CLI, and one that fails
deliberately to prove a failed run still leaves its transcript behind).

What is asserted:

  * `record` logs both directions of a bot of the tests' shape — the decoded
    packets it sends and receives, exactly once each, plus the raw send that
    goes straight to the socket (the login handshake) — and it records the
    keep-alive ack that the reader's *own* thread sends;
  * `save`/`load` round-trips that log, including the compression threshold each
    packet was received under, which is what lets `ReplaySocket` re-frame it;
  * a replay serves whole frames and never fuses two, and running out is a
    `socket.timeout` while a timeout is armed and an `EOFError` on a blocking
    read — a replay cannot desynchronise the stream the way a windowed drain on
    a live socket could, which is the failure class the transcript exists to
    make re-examinable;
  * a replayed *bot* drives its own threshold discovery off the replayed login
    packets, so a real client reads a replay the way it read the live run;
  * the sends a replay's client makes are compared with the recorded ones, and a
    replayed test that took a different path is loud about it (strict), or
    records the difference and carries on (`strict=False`);
  * a run that raises still writes its transcript, and a file that is not one is
    refused rather than half-read;
  * both client shapes `record` knows are wrapped — `send`/`recv` and
    `send_pkt`/`read_pkt` — and a client with neither is refused by name;
  * a client that never starts a `Reader` still records windows (a direct read
    is window n), which is what keeps a replayed drain from being handed the
    rest of the session at once;
  * keep-alive acks are counted in both directions and never compared, because
    which thread's send lands first is a race a replay cannot reproduce;
  * `RawClient`'s handshake writes the port a server ignores (0), and the two
    mode helpers — `skip_server_check`, `refuse_replay` — answer for the mode
    they are in.
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import textwrap

import wire_client as wc

HERE = os.path.dirname(os.path.abspath(__file__))

# Some ids to name things with; the ids themselves do not matter here, only that
# the same number comes back out.
ID_PING = 0x26
ID_PONG = 0x3E
ID_KEEP_ALIVE = wc.CB_KEEP_ALIVE
SB_FLYING_ID = wc.SB_FLYING
ID_SET_COMPRESSION = 0x03
ID_LOGIN_SUCCESS = 0x02


class FakeBot:
    """The shape `record` needs, around the client end of a socket pair: a
    socket, `send`/`recv`, a `threshold`, and a `Reader`."""

    def __init__(self, sock, threshold=-1):
        self.sock = sock
        self.threshold = threshold
        self.reader = wc.Reader(sock, self)

    def send(self, pid, body=b""):
        self.reader.send(pid, body)

    def recv(self):
        return wc.read_packet(self.sock, self.threshold)


def test_record_instruments_both_ends():
    client, server = socket.socketpair()
    try:
        bot = FakeBot(client)
        transcript = wc.record(bot, name="unit")

        # A send of the shape every test's bot has: log it decoded.
        bot.send(ID_PING, struct.pack(">i", 7))
        assert wc.read_packet(server, -1) == (ID_PING, struct.pack(">i", 7))
        assert [e["id"] for e in transcript.serverbound()] == [ID_PING], transcript.events
        # ... and not again as raw bytes: the guard covers the whole send path.
        assert not [e for e in transcript.events if "raw" in e], transcript.events

        # A send that goes straight to the socket — the login handshake is the
        # real one — is kept raw, because nothing decoded it.
        bot.sock.sendall(b"\x01\x02\x03")
        assert server.recv(3) == b"\x01\x02\x03"
        raw = [e for e in transcript.events if "raw" in e]
        assert len(raw) == 1 and raw[0]["raw"] == "010203", transcript.events

        # Reads are logged before the reader thread takes the socket...
        server.sendall(wc.make_frame(wc.varint(ID_PONG) + struct.pack(">i", 9), -1))
        assert bot.recv() == (ID_PONG, struct.pack(">i", 9))

        # ... and through it. The keep-alive is the interesting one: its ack is
        # sent from the reader's thread, not the test's.
        bot.reader.start()
        ka = b"\x11" * 8
        server.sendall(wc.make_frame(wc.varint(ID_KEEP_ALIVE) + ka, -1))
        assert bot.reader.pop(1.0) == (ID_KEEP_ALIVE, ka)
        assert wc.read_packet(server, -1) == (wc.SB_KEEP_ALIVE, ka), "the ack"

        assert [e["id"] for e in transcript.clientbound()] == [ID_PONG, ID_KEEP_ALIVE], \
            transcript.events
        # The serverbound side carries the raw handshake too, so name the
        # decoded packets when asking about them.
        decoded = [e["id"] for e in transcript.serverbound() if "id" in e]
        assert decoded == [ID_PING, wc.SB_KEEP_ALIVE], transcript.events
        bot.reader.stop()
    finally:
        client.close()
        server.close()


def test_round_trip_and_replay():
    transcript = wc.Transcript(note="unit", test="unit")
    small = b"\x00\x01\x02"
    big = bytes(range(256)) * 4  # over the threshold, so it really is compressed
    transcript.packet("C", 0x2C, small, thr=-1, conn=1)
    transcript.packet("C", 0x5A, big, thr=64, conn=1)
    transcript.packet("S", ID_PING, struct.pack(">i", 3), thr=-1, conn=1)
    transcript.raw(b"\x10", conn=1)
    transcript.packet("C", 0x2C, b"\x00" * 8, thr=-1, conn=2)

    with tempfile.TemporaryDirectory() as directory:
        # `save` makes the directory: PLUSMC_TRANSCRIPT_DIR need not exist yet.
        path = transcript.save(os.path.join(directory, "nested", "unit.jsonl"))
        back = wc.Transcript.load(path)

    assert [(e["id"], bytes.fromhex(e["b"])) for e in back.clientbound(1)] == \
        [(0x2C, small), (0x5A, big)], back.events
    assert [e["id"] for e in back.clientbound(2)] == [0x2C], back.events
    assert back.connections == [1, 2], back.connections
    assert back.counts()["S"] == {"0x26": 1, "raw": 1}, back.counts()
    assert back.note == "unit" and back.test == "unit", back.note

    # The replay's bytes are the recorded packets, at the recorded thresholds:
    # reading them back with the same threshold returns what was recorded, byte
    # for byte, compression and all.
    sock = wc.ReplaySocket(back, conn=1)
    assert wc.read_packet(sock, -1) == (0x2C, small)
    assert wc.read_packet(sock, 64) == (0x5A, big)
    # Connection 2 replays its own stream, not the first one.
    assert wc.read_packet(wc.ReplaySocket(back, conn=2), -1) == (0x2C, b"\x00" * 8)


def test_replay_serves_whole_frames():
    transcript = wc.Transcript()
    first, second = b"\xaa" * 4, b"\xbb" * 6
    transcript.packet("C", 0x2C, first)
    transcript.packet("C", 0x2D, second)
    sock = wc.ReplaySocket(transcript)

    # Asked for a megabyte, a replay hands over *one* frame: it can never fuse
    # two together, so nothing downstream can read across a boundary.
    assert sock.recv(1 << 20) == wc.make_frame(wc.varint(0x2C) + first, -1)
    assert sock.recv(1 << 20) == wc.make_frame(wc.varint(0x2D) + second, -1)

    # Exhausted: a window that closes first is a timeout (what ends a drain
    # early), and a blocking read is an EOF that says so (what stops the reader
    # thread) — never a partial packet.
    sock.settimeout(0.1)
    try:
        sock.recv(1)
        raise AssertionError("exhaustion must not look like data")
    except socket.timeout:
        pass
    sock.settimeout(None)
    try:
        sock.recv(1)
        raise AssertionError("exhaustion must not look like data")
    except EOFError as exc:
        assert "transcript ended after 2 clientbound packet(s)" in str(exc), exc


class ReplayStub:
    """What `Reader` needs of its bot: a socket it reads through, a threshold,
    and `recv`. The real tests' Bots, cut down."""

    def __init__(self, sock, threshold=-1):
        self.sock = sock
        self.threshold = threshold
        self.reader = wc.Reader(sock, self)

    def recv(self):
        return wc.read_packet(self.sock, self.threshold)


def test_replay_serves_the_recorded_windows():
    """A replay hands over the packets of a window only once the replayed client
    asks for that window — which is what makes a window assertion mean the same
    thing it did live, and what makes a loop that reads until it sees a packet
    terminate in the same window."""
    transcript = wc.Transcript()
    transcript.packet("C", 0x2C, b"\x01", w=0)   # the join: read synchronously
    transcript.packet("C", 0x08, b"\x02", w=1)
    transcript.packet("C", 0x08, b"\x03", w=1)
    transcript.packet("C", 0x08, b"\x04", w=2)
    assert [w for w, _ in transcript.frame_groups(1)] == [0, 1, 2], \
        transcript.frame_groups(1)

    sock = wc.ReplaySocket(transcript)
    stub = ReplayStub(sock)
    # Window 0 is read straight off the socket, before the reader takes over.
    assert stub.recv() == (0x2C, b"\x01")
    stub.reader.start()

    first = stub.reader.drain(0.4)   # one window: two packets, and only two
    assert first == [(0x08, b"\x02"), (0x08, b"\x03")], first
    assert stub.reader.windows == 1, stub.reader.windows
    # The reader thread is parked on window 2's frame and can read nothing more
    # until this client asks for that window — which is the whole point: the
    # next packet cannot leak into a window that never saw it live.
    assert stub.reader.queue.empty(), "window 2's packet is not readable yet"

    second = stub.reader.drain(0.4)  # the next window: the third packet
    assert second == [(0x08, b"\x04")], second
    assert stub.reader.windows == 2, stub.reader.windows
    # Past the last window the read side ends, and a drain that finds nothing
    # gets nothing rather than a partial frame.
    assert stub.reader.drain(0.2) == [], "nothing is left after the last window"
    stub.reader.stop()

    # A transcript with no window information is served as it is read: there is
    # nothing better to do with it, and the dump says so (`w=-`).
    bare = wc.Transcript()
    bare.packet("C", 0x2C, b"\x01")
    bare.packet("C", 0x2D, b"\x02")
    assert [w for w, _ in bare.frame_groups()] == [0], bare.frame_groups()


def test_replay_compares_sends():
    transcript = wc.Transcript()
    transcript.packet("C", 0x2C, b"\x00" * 8)
    payload = struct.pack(">i", 3)
    transcript.packet("S", ID_PING, payload, thr=-1)

    def frame(pid, body=b"", thr=-1):
        return wc.make_frame(wc.varint(pid) + body, thr)

    # The same send the recording made: no divergence.
    same = wc.ReplaySocket(transcript, strict=True)
    same.sendall(frame(ID_PING, payload))
    assert not same.divergences, same.divergences
    assert same.sent == [frame(ID_PING, payload)]

    # And a send big enough to have been compressed, which is the shape every
    # packet in the play state has: the comparison inflates it first, so a frame
    # cut at the wrong offset is a divergence and not a pass (`rd_varint`
    # returns the position after the varint, not its length — getting that
    # wrong sliced one byte off every compressed frame).
    thick = wc.Transcript()
    chunky = bytes(range(256)) * 2
    thick.packet("S", 0x1E, chunky, thr=64)
    compressed = wc.ReplaySocket(thick)
    compressed.sendall(wc.make_frame(wc.varint(0x1E) + chunky, 64))
    assert not compressed.divergences, compressed.divergences

    # A different packet: loud, and named.
    other = wc.ReplaySocket(transcript, strict=True)
    try:
        other.sendall(frame(SB_FLYING_ID, b"\x01"))
        raise AssertionError("a divergent replay must not pass silently")
    except AssertionError as exc:
        assert "replay divergence at send #1" in str(exc), exc
        assert "0x21" in str(exc) and "0x26" in str(exc), exc

    # More sends than were recorded, too.
    extra = wc.ReplaySocket(transcript, strict=True)
    extra.sendall(frame(ID_PING, payload))
    try:
        extra.sendall(b"\x00")
        raise AssertionError("an unrecorded extra send must not pass silently")
    except AssertionError as exc:
        assert "more byte(s) than the transcript records" in str(exc), exc

    # Relaxed, the replay carries on and keeps the difference on the record.
    relaxed = wc.ReplaySocket(transcript, strict=False)
    relaxed.sendall(frame(SB_FLYING_ID, b"\x01"))
    assert relaxed.divergences and "0x21" in relaxed.divergences[0], relaxed.divergences

    # Bytes that are not one decodable frame at the recorded threshold are a
    # divergence too, not a crash inside the comparison.
    junk = wc.ReplaySocket(transcript)
    try:
        junk.sendall(b"\x7f" * 20)
        raise AssertionError("undecodable bytes must be reported")
    except AssertionError as exc:
        assert "not a frame decodable at threshold" in str(exc), exc


def test_replay_through_a_real_bot():
    """A replay is read by a client whose threshold comes from the replayed
    login handshake — the same discovery a live join does."""
    transcript = wc.Transcript()
    login_body = wc.varint(64)  # set_compression: everything above is compressed
    transcript.packet("C", ID_SET_COMPRESSION, login_body, thr=-1)
    transcript.packet("C", ID_LOGIN_SUCCESS, b"player", thr=64)

    bot = FakeBot(wc.ReplaySocket(transcript))
    pid, body = bot.recv()
    assert pid == ID_SET_COMPRESSION, hex(pid)
    threshold, _ = wc.rd_varint(body)
    assert threshold == 64, threshold
    bot.threshold = threshold
    assert bot.recv() == (ID_LOGIN_SUCCESS, b"player")


def test_two_clients_share_one_file():
    """A run's second client joins the same transcript as another connection,
    so a replay can hand each client back its own stream."""
    directory = tempfile.mkdtemp(prefix="plusmc-transcript-")
    path = os.path.join(directory, "run.jsonl")
    left, right = socket.socketpair()
    other_left, other_right = socket.socketpair()
    try:
        bot_a, bot_b = FakeBot(left), FakeBot(other_left)
        first = wc.record(bot_a, path=path, name="run", note="one")
        second = wc.record(bot_b, path=path, name="run", note="two")
        assert first is second, "two clients of one run share its transcript"
        bot_a.send(ID_PING, b"\x01")
        bot_b.send(ID_PONG, b"\x02")
        assert [e["c"] for e in first.serverbound()] == [1, 2], first.events
        assert first.note == "one | two", first.note
        assert os.listdir(directory) == [], "nothing is written until the run ends"
        first.save(path)
        back = wc.Transcript.load(path)
        assert back.connections == [1, 2], back.connections
        assert [e["id"] for e in back.serverbound(2)] == [ID_PONG], back.events
        # Each connection compares against its own send stream, not the other's.
        sock = wc.ReplaySocket(back, conn=2)
        sock.sendall(wc.make_frame(wc.varint(ID_PONG) + b"\x02", -1))
        assert not sock.divergences, sock.divergences
    finally:
        for sock in (left, right, other_left, other_right):
            sock.close()


def test_a_failed_run_leaves_the_file():
    """The whole point of writing at exit: a test that raises still hands you
    the packets that made it raise."""
    script = textwrap.dedent(f"""
        import socket, sys
        sys.path.insert(0, {HERE!r})
        import wire_client as wc

        class Bot:
            def __init__(self, sock):
                self.sock = sock
                self.threshold = -1
                self.reader = None
            def send(self, pid, body=b""):
                self.sock.sendall(wc.make_frame(wc.varint(pid) + body, self.threshold))
            def recv(self):
                return wc.read_packet(self.sock, self.threshold)

        a, b = socket.socketpair()
        bot = Bot(a)
        wc.record(bot, name="unit", note="a run that failed")
        bot.send({ID_PING}, b"\\x03\\x00\\x00\\x00")
        raise RuntimeError("the test blew up")
    """)
    with tempfile.TemporaryDirectory() as directory:
        env = dict(os.environ, PLUSMC_TRANSCRIPT_DIR=directory)
        env.pop("PLUSMC_REPLAY", None)  # a replay of a replay is not this test
        proc = subprocess.run([sys.executable, "-c", script], env=env,
                              capture_output=True, text=True)
        assert proc.returncode != 0, proc.stdout
        assert "the test blew up" in proc.stderr, proc.stderr
        files = os.listdir(directory)
        assert len(files) == 1, files
        assert files[0].startswith("unit-") and files[0].endswith(".jsonl"), files
        assert "[transcript]" in proc.stderr, proc.stderr
        back = wc.Transcript.load(os.path.join(directory, files[0]))
        assert [e["id"] for e in back.serverbound()] == [ID_PING], back.events
        assert back.note == "a run that failed", back.note


def test_load_rejects_junk():
    with tempfile.TemporaryDirectory() as directory:
        empty = os.path.join(directory, "empty.jsonl")
        open(empty, "w").close()
        try:
            wc.Transcript.load(empty)
            raise AssertionError("an empty file is not a transcript")
        except ValueError as exc:
            assert "empty transcript" in str(exc), exc

        foreign = os.path.join(directory, "foreign.jsonl")
        with open(foreign, "w") as fh:
            fh.write('{"format": "something-else", "version": 1}\n')
        try:
            wc.Transcript.load(foreign)
            raise AssertionError("another format is not a transcript")
        except ValueError as exc:
            assert "not a plusmc-wire-transcript" in str(exc), exc


def test_cli_reads_a_transcript():
    transcript = wc.Transcript(note="cli check", test="unit")
    transcript.packet("C", 0x2C, b"\x00" * 8, thr=-1)
    transcript.packet("S", ID_PING, struct.pack(">i", 5), thr=-1)
    transcript.packet("C", 0x5A, bytes(range(40)), thr=64)  # long: gets truncated
    transcript.raw(b"\x10\x11")
    with tempfile.TemporaryDirectory() as directory:
        path = transcript.save(os.path.join(directory, "unit.jsonl"))
        cli = os.path.join(HERE, "wire_client.py")

        env = {k: v for k, v in os.environ.items() if k != "PLUSMC_REPLAY"}

        def run(*args):
            proc = subprocess.run([sys.executable, cli, path, *args], env=env,
                                  capture_output=True, text=True)
            assert proc.returncode == 0, proc.stderr
            return proc.stdout

        out = run()
        assert wc.TRANSCRIPT_FORMAT in out and "cli check" in out, out
        assert "id=0x2c len=   8" in out, out
        assert "... (+16 more)" in out, out          # 40 bytes shown as 24
        assert "raw len=   2" in out, out
        assert "counts:" in out, out

        only_s = run("--dir", "S")
        assert "id=0x26" in only_s and "id=0x2c" not in only_s, only_s

        one = run("--id", "0x5a", "--bytes", "0")
        assert "id=0x5a" in one and "id=0x26" not in one, one
        assert "thr=  64" in one, one

        limited = run("--limit", "1")
        assert "3 more event(s) not shown" in limited, limited


class PktBot:
    """The tests' *other* client shape: it owns its framing (`send_pkt` /
    `read_pkt`) and has no `Reader` — join_test's, churn's and online_join's
    clients. Such a client reads directly for the whole session, which is what
    `test_direct_reads_are_their_own_windows` is about."""

    def __init__(self, sock, threshold=-1, name="PktBot"):
        self.sock = sock
        self.threshold = threshold
        self.name = name

    def send_pkt(self, pid, body=b""):
        self.sock.sendall(wc.make_frame(wc.varint(pid) + body, self.threshold))

    def read_pkt(self):
        return wc.read_packet(self.sock, self.threshold)


def test_record_wraps_the_other_client_shape():
    """`record` wraps a `send_pkt`/`read_pkt` client too, so the tests whose
    client owns its framing record and replay like a `Bot` — and a client with
    none of the four names is refused by name, at the call, rather than failing
    later inside a replay."""
    client, server = socket.socketpair()
    try:
        bot = PktBot(client)
        transcript = wc.record(bot, name="unit")
        bot.send_pkt(ID_PING, struct.pack(">i", 5))
        assert wc.read_packet(server, -1) == (ID_PING, struct.pack(">i", 5))
        server.sendall(wc.make_frame(wc.varint(ID_PONG) + b"\x07", -1))
        assert bot.read_pkt() == (ID_PONG, b"\x07")

        assert [e["id"] for e in transcript.serverbound() if "id" in e] == [ID_PING], \
            transcript.events
        assert [e["id"] for e in transcript.clientbound()] == [ID_PONG], transcript.events
    finally:
        client.close()
        server.close()

    class Neither:
        sock = None

    try:
        wc.record(Neither())
        raise AssertionError("a client with no packet pair must be refused")
    except TypeError as exc:
        assert "send_pkt" in str(exc) and "RawClient" in str(exc), exc


def test_direct_reads_are_their_own_windows():
    """A client that never starts a reader still records windows: a direct read
    *is* the client asking for the next packet, so it is window n of the
    connection, and a replay serves the packets of read n when the replayed
    client asks for window n.

    When every direct read was window 0 — always open — a replayed drain was
    handed the rest of the session at once, ahead of the packets that were
    supposed to arrive in it, and `persistence`'s placement assertions saw
    nothing. This is the fix, end to end: recorded through a live socket pair,
    then read back through a replay by the same client shape.
    """
    client, server = socket.socketpair()
    try:
        bot = PktBot(client)
        recorded = wc.record(bot, name="unit")
        bot.send_pkt(ID_PING, b"\x00")
        for pid, body in ((0x0B, b"\x01"), (0x04, b"\x02"), (0x08, b"\x03")):
            server.sendall(wc.make_frame(wc.varint(pid) + body, -1))
            assert bot.read_pkt() == (pid, body)
    finally:
        client.close()
        server.close()

    assert [e["w"] for e in recorded.clientbound()] == [1, 2, 3], recorded.events

    # The same client shape, replayed: read n returns read n's packet and cannot
    # reach ahead to a packet a later read returned live.
    replayed = PktBot(wc.ReplaySocket(recorded))
    wc.record(replayed, name="unit")
    assert replayed.read_pkt() == (0x0B, b"\x01")
    assert replayed.read_pkt() == (0x04, b"\x02")
    assert replayed.read_pkt() == (0x08, b"\x03")
    replayed.sock.settimeout(0.1)
    try:
        replayed.read_pkt()
        raise AssertionError("a read past the last window must not return data")
    except socket.timeout:
        pass


def test_keep_alive_acks_are_counted_not_compared():
    """A keep-alive ack is the reader thread's *reaction* to a server packet, so
    whether it lands before or after the test's own next send is a race between
    two threads — the one thing in the serverbound stream a replay cannot
    reproduce. It is counted and skipped instead: live it moved `persistence`'s
    sends out of order and stopped a correct replay of correct work."""
    def ack_frame():
        return wc.make_frame(wc.varint(wc.SB_KEEP_ALIVE) + b"\x11" * 8, -1)

    def flying_frame():
        return wc.make_frame(wc.varint(SB_FLYING_ID) + b"\x01", -1)

    transcript = wc.Transcript()
    transcript.packet("C", wc.CB_KEEP_ALIVE, b"\x11" * 8)
    transcript.packet("S", SB_FLYING_ID, b"\x01")
    transcript.packet("S", wc.SB_KEEP_ALIVE, b"\x11" * 8, ack=True)

    # The test's own send first, then the reader's ack: no divergence either
    # way, and the ack is not one of the compared sends.
    sock = wc.ReplaySocket(transcript)
    sock.sendall(flying_frame())
    sock.sendall(ack_frame())
    assert not sock.divergences, sock.divergences
    assert sock.acks_sent == 1, sock.acks_sent
    assert "compared 1 send(s)" in sock.summary(), sock.summary()
    assert "1 keep-alive ack(s) not compared" in sock.summary(), sock.summary()

    # The ack first — the other race outcome — and the real send still lands on
    # the right recorded entry.
    other = wc.ReplaySocket(transcript)
    other.sendall(ack_frame())
    other.sendall(flying_frame())
    assert not other.divergences, other.divergences
    assert other.acks_sent == 1, other.acks_sent

    # A recorded ack in front of the packet the replay sends next is stepped
    # over and counted, not compared against it.
    ack_first = wc.Transcript()
    ack_first.packet("S", wc.SB_KEEP_ALIVE, b"\x11" * 8, ack=True)
    ack_first.packet("S", SB_FLYING_ID, b"\x01")
    third = wc.ReplaySocket(ack_first)
    third.sendall(flying_frame())
    assert not third.divergences, third.divergences
    assert third.acks_recorded == 1, third.acks_recorded
    # An ack is recognised in either framing (plain, and compressed inside the
    # play state's frames).
    assert wc.ReplaySocket(transcript)._is_ack(ack_frame()), "plain framing"
    assert wc.ReplaySocket(transcript)._is_ack(
        wc.make_frame(wc.varint(wc.SB_KEEP_ALIVE) + b"\x11" * 8, 64)), \
        "compressed framing"


def test_raw_client_handshake_and_the_mode_helpers():
    """`RawClient` is the shim for the tests whose protocol is a few
    request/response packets, and its handshake writes a zero port — a server
    ignores that field, and a transcript that carried the live port could not be
    replayed on a machine that picked another. `skip_server_check` and
    `refuse_replay` are the other half: how a test says which of its assertions
    are the wire's and which belong to a server a replay does not have."""
    class FakeTransport:
        def __init__(self):
            self.sent = []
            self.timeouts = []

        def sendall(self, data, *args, **kwargs):
            self.sent.append(bytes(data))

        def settimeout(self, value):
            self.timeouts.append(value)

    fake = FakeTransport()
    real = wc.transport
    wc.transport = lambda host, port, timeout=10: fake
    try:
        client = wc.RawClient("127.0.0.1", 4242, timeout=3)
        client.handshake(1, 777, host="localhost")
        client.send(ID_PING, struct.pack(">i", 5))
    finally:
        wc.transport = real

    pid, body = wc.decode_frame(fake.sent[0], -1)
    assert pid == 0, hex(pid)
    protocol, pos = wc.rd_varint(body)
    host_len, pos = wc.rd_varint(body, pos)
    addr = body[pos:pos + host_len].decode()
    pos += host_len
    (port,) = struct.unpack(">H", body[pos:pos + 2])
    pos += 2
    next_state, _ = wc.rd_varint(body, pos)
    assert (protocol, addr, port, next_state) == (777, "localhost", 0, 1), \
        (protocol, addr, port, next_state)
    assert wc.decode_frame(fake.sent[1], -1) == (ID_PING, struct.pack(">i", 5)), \
        fake.sent[1].hex()
    assert fake.timeouts == [3], fake.timeouts

    saved = os.environ.pop("PLUSMC_REPLAY", None)
    try:
        assert wc.skip_server_check("x") is False
        wc.refuse_replay("a concurrency stress")  # live: nothing to refuse
        os.environ["PLUSMC_REPLAY"] = "nowhere.jsonl"
        assert wc.skip_server_check("x") is True
        try:
            wc.refuse_replay("a concurrency stress")
            raise AssertionError("a refusal must not pass quietly")
        except AssertionError as exc:
            assert "cannot be replayed" in str(exc), exc
            assert "a concurrency stress" in str(exc), exc
    finally:
        if saved is None:
            os.environ.pop("PLUSMC_REPLAY", None)
        else:
            os.environ["PLUSMC_REPLAY"] = saved


def main():
    tests = [
        ("record: both ends of a bot, one packet each", test_record_instruments_both_ends),
        ("record: the send_pkt/read_pkt client shape is wrapped too",
         test_record_wraps_the_other_client_shape),
        ("replay: a direct read is its own window",
         test_direct_reads_are_their_own_windows),
        ("replay: keep-alive acks are counted, not compared",
         test_keep_alive_acks_are_counted_not_compared),
        ("RawClient's handshake, and the mode helpers",
         test_raw_client_handshake_and_the_mode_helpers),
        ("save, load, and replay byte for byte", test_round_trip_and_replay),
        ("replay: whole frames, and exhaustion that cannot split one",
         test_replay_serves_whole_frames),
        ("replay: the recorded read windows are served in order",
         test_replay_serves_the_recorded_windows),
        ("replay: the client's sends are compared", test_replay_compares_sends),
        ("replay: a real client reads it, threshold and all",
         test_replay_through_a_real_bot),
        ("two clients of one run share one file", test_two_clients_share_one_file),
        ("a failed run still writes its transcript", test_a_failed_run_leaves_the_file),
        ("load: junk is refused", test_load_rejects_junk),
        ("cli: the transcript is readable", test_cli_reads_a_transcript),
    ]
    for label, fn in tests:
        fn()
        print(f"  OK {label}")
    print("\nPASS: wire transcript test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
