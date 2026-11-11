#!/usr/bin/env python3
"""Full-join integration test for +MC (protocol 777 / 26.3, offline mode).

Acts as a raw Minecraft client and asserts:
  1. login: Set Compression + Login Success, ack -> configuration
  2. configuration: brand/feature-flags/known-packs -> registry data + tags ->
     Finish Configuration -> ack -> play
  3. play: Join Game decodes, abilities + spawn position arrive
  4. spawn chunk batch: Chunk Batch Start, 9 all-air chunks, Chunk Batch
     Finished(9) arrive and parse
  5. keep-alives flow and are acked; idle players are not dropped
  6. two players: each sees the other in the tab list (Player Info Update
     0x47) and spawned in-world (Spawn Entity 0x01, player type 159); the
     joining player also gets its own tab entry
  7. two players: A's chat message is echoed to B (and A) as system chat
  8. movement relay: A's rel move (0x36), look (0x39), move+look (0x37) and
     big-jump teleport (0x23) are received by B with correct deltas/angles;
     interest management then drops A's entity when it leaves the 48-block
     tracking range and pairs it again on re-entry, and a walk that crosses
     the boundary twice sends exactly one Remove Entities and one pairing
     burst per crossing, with nothing relayed while out of range
  9. leave: the remaining player receives Remove Entities (0x4d) + Player Info
     Remove (0x46) for the departed player
  10. SIGTERM -> clean exit 0

Keep-alive timing is scaled down via PLUSMC_KEEPALIVE_MS/TIMEOUT_MS so the
cadence/ack paths are exercised quickly. Usage: join_test.py /path/to/plusmc
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
import uuid
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_client  # noqa: E402  (the shared frame reader)
from nbt_component import chat_text  # noqa: E402  (text components are network NBT)

BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
HOST = "127.0.0.1"
PROTO = 777
PLAYER_A = "PlayerA"
PLAYER_B = "PlayerB"


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


def offline_uuid(name: str) -> bytes:
    """Replicates the server's deterministic offline uuid (FNV-1a over
    'OfflinePlayer:<name>'; see src/server/session.cpp offline_uuid)."""
    base = "OfflinePlayer:" + name
    h = 1469598103934665603
    for ch in base.encode("utf-8"):
        h ^= ch
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    out = bytearray(16)
    for i in range(8):
        out[i] = (h >> (8 * i)) & 0xFF
    for i in range(8):
        out[8 + i] = base[i % len(base)].encode("utf-8")[0]
    out[6] = (out[6] & 0x0F) | 0x30  # version nibble 3
    out[8] = (out[8] & 0x3F) | 0x80  # RFC 4122 variant
    return bytes(out)


def decode_player_info_add_entries(body: bytes):
    """Decodes a Player Info Update (0x47) body into per-player dicts.

    Handles the 777 shape: u8 actions bitmask, prefixed array of players, each
    with per-action payloads in mask order (0x01 add player, 0x02 init chat,
    0x04 game mode, 0x08 listed, 0x10 latency, 0x20 display name, 0x40
    priority, 0x80 hat). Requires the body to be consumed exactly, so a
    field-order slip cannot hide behind trailing bytes.
    """
    actions = body[0]
    count, i = rd_varint(body, 1)
    out = []
    for _ in range(count):
        uuid = body[i:i + 16]
        i += 16
        entry = {"uuid": uuid, "name": None, "props": [], "hat": None}
        if actions & 0x01:
            entry["name"], i = rd_string(body, i)
            nprops, i = rd_varint(body, i)
            for _ in range(nprops):
                pn, i = rd_string(body, i)
                pv, i = rd_string(body, i)
                has = body[i]
                i += 1  # has signature
                ps = ""
                if has:
                    ps, i = rd_string(body, i)
                entry["props"].append((pn, pv, has, ps))
        if actions & 0x02:
            i += 1  # initialize chat: has session (absent on this server)
        if actions & 0x04:
            entry["gamemode"], i = rd_varint(body, i)
        if actions & 0x08:
            entry["listed"] = body[i]
            i += 1
        if actions & 0x10:
            entry["latency"], i = rd_varint(body, i)
        if actions & 0x20:
            i += 1
            if body[i - 1]:
                ln, i = rd_varint(body, i)
                i += ln
        if actions & 0x40:
            entry["list_order"], i = rd_varint(body, i)
        if actions & 0x80:
            entry["hat"] = body[i]
            i += 1
        out.append(entry)
    assert i == len(body), f"player info update: {len(body) - i} trailing byte(s)"
    return out


def decode_player_info_add(body: bytes):
    """Returns the names in a Player Info Update (0x47) body."""
    return [e["name"] for e in decode_player_info_add_entries(body)]


def decode_spawn_entity(body: bytes):
    """Returns (entity id, uuid, type varint) of a Spawn Entity (0x01)."""
    eid, i = rd_varint(body, 0)
    u = body[i:i + 16]
    i += 16
    typ, _ = rd_varint(body, i)
    return eid, u, typ


def decode_entity_data(body: bytes):
    """Decodes Set Entity Data (0x65) into (entity id, {index: (serializer, value)}).

    Layout is [varint eid] then repeated [u8 index][varint serializer][value]
    terminated by 0xFF. The BYTE serializer (id 0) is what this server derives
    for a player, the FLOAT one (id 3) is the words it sends with another
    player's synched data, and PARTICLES (id 17) is the status-effect particle
    list at index 10 (``LivingEntity.DATA_EFFECT_PARTICLES``); anything else is
    an error rather than a guess.
    """
    eid, i = rd_varint(body, 0)
    data = {}
    while body[i] != 0xFF:
        index = body[i]
        i += 1
        ser, i = rd_varint(body, i)
        assert ser in (0, 3, 17), \
            f"unexpected synched-data serializer {ser} at index {index}"
        if ser == 0:
            data[index] = (ser, body[i])
            i += 1
        elif ser == 3:
            data[index] = (ser, struct.unpack(">f", body[i:i + 4])[0])
            i += 4
        else:
            # PARTICLES: a varint count, then each entry's particle type id and
            # that type's own payload — the ARGB colour for
            # minecraft:entity_effect, nothing for a bare SimpleParticleType.
            count, i = rd_varint(body, i)
            particles = []
            for _ in range(count):
                ptype, i = rd_varint(body, i)
                color = None
                if ptype == 28:
                    color = int.from_bytes(body[i:i + 4], "big")
                    i += 4
                particles.append((ptype, color))
            data[index] = (ser, particles)
    assert i == len(body) - 1, \
        f"set entity data: {len(body) - 1 - i} byte(s) before/after the 0xFF terminator"
    return eid, data


def decode_rel_move(body: bytes):
    """Rel Entity Move (0x36): eid + 3 x i16 deltas (1/4096 block) + ground."""
    eid, i = rd_varint(body, 0)
    dx, dy, dz = struct.unpack(">hhh", body[i:i + 6])
    return eid, dx, dy, dz, body[i + 6]


def decode_move_look(body: bytes):
    """Entity Move & Look (0x37): eid + deltas + yaw/pitch bytes + ground."""
    eid, i = rd_varint(body, 0)
    dx, dy, dz = struct.unpack(">hhh", body[i:i + 6])
    return eid, dx, dy, dz, body[i + 6], body[i + 7], body[i + 8]


def decode_entity_look(body: bytes):
    """Entity Look (0x39): eid + yaw/pitch bytes + ground."""
    eid, i = rd_varint(body, 0)
    return eid, body[i], body[i + 1], body[i + 2]


def decode_teleport(body: bytes):
    """Sync Entity Position (0x23): eid + absolute x/y/z (f64) + velocity + yaw/pitch."""
    eid, i = rd_varint(body, 0)
    x, y, z = struct.unpack(">ddd", body[i:i + 24])
    return eid, x, y, z


def decode_remove_entities(body: bytes):
    """Remove Entities (0x4e): varint count, then that many entity ids."""
    count, i = rd_varint(body, 0)
    ids = []
    for _ in range(count):
        eid, i = rd_varint(body, i)
        ids.append(eid)
    return ids


def scan_for(client: "Client", label: str, pred):
    """Reads packets until `pred(pid, body)` is true (acking keep-alives)."""
    for _ in range(200):
        pid, body = client.read_pkt()
        if pid == 0x2D:
            unpack_keepalive(client, body)
            continue
        if pred(pid, body):
            return pid, body
    raise AssertionError(f"{label}: expected packet not seen")


def make_frame(payload: bytes, threshold: int) -> bytes:
    # After Set Compression (threshold >= 0) EVERY frame uses
    # [length][dataLength][data]; under-threshold packets carry dataLength = 0
    # followed by the raw packet. Before that, plain [length][payload].
    if threshold < 0:
        return varint(len(payload)) + payload
    if len(payload) < threshold:
        body = varint(0) + payload
    else:
        body = varint(len(payload)) + zlib.compress(payload)
    return varint(len(body)) + body


def recvn(sock, n):
    """`n` bytes, or EOFError if the peer closed first.

    EOF is an EOF, not an assertion: at login or in a drain it fails the test
    just the same (the exception propagates), while on the background reader's
    read path it is the ordinary way a session ends, and `Reader._loop` treats it
    as such. Raising `AssertionError` here made the reader thread die with a
    printed traceback whenever a session closed while it was parked in `recv`,
    which is a *passing* test's log carrying a crash.
    """
    chunks = []
    got = 0
    while got < n:
        c = sock.recv(n - got)
        if not c:
            raise EOFError(f"unexpected EOF (wanted {n}, got {got})")
        chunks.append(c)
        got += len(c)
    return b"".join(chunks)


def read_varint_bytes(sock) -> bytes:
    out = b""
    while True:
        c = recvn(sock, 1)
        out += c
        if not (c[0] & 0x80):
            return out


def read_packet(sock, threshold: int):
    """Reads one packet, honoring the connection compression threshold."""
    ln, _ = rd_varint(read_varint_bytes(sock))
    if threshold < 0:
        data = recvn(sock, ln)
    else:
        # Compressed framing: [length][dataLength][data].
        dl_raw = read_varint_bytes(sock)
        data_len, nbytes = rd_varint(dl_raw)
        data = recvn(sock, ln - nbytes)
        if data_len != 0:
            try:
                data = zlib.decompress(data)
            except zlib.error as e:
                if os.environ.get("JOIN_DEBUG"):
                    open("/tmp/failframe.bin", "wb").write(data)
                    print(f"frame len={ln} data_len={data_len} got {len(data)} bytes: {e}")
                    print("prefix:", data[:40].hex())
                raise
    pid, off = rd_varint(data)
    return pid, data[off:]


class Client:
    def __init__(self, name: str, port: int):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(10)
        self.threshold = -1
        self.entity_id = None  # the server's id for this player's own entity
        # From the end of the join, a background thread owns the read side and
        # `read_pkt` takes packets off its queue. A drain that reads the socket
        # itself can lose a packet it has half-taken when its window closes, and
        # every packet after that one is garbage; the thread reads whole frames
        # on a blocking socket, so a window that closes early loses nothing and
        # a keep-alive is answered wherever it arrives. See tests/wire_client.py.
        self.reader = wire_client.Reader(self.sock, self)

    def send_pkt(self, pkt_id: int, body: bytes = b""):
        self.reader.send(pkt_id, body)

    def recv(self):
        """The raw read: the join's own reads and the reader thread's."""
        return read_packet(self.sock, self.threshold)

    def read_pkt(self, timeout: float = 10.0):
        """One packet, waiting up to `timeout` seconds for it."""
        if not self.reader.started:
            return self.recv()
        try:
            return self.reader.pop(timeout)
        except TimeoutError as exc:  # the socket's own spelling, for callers
            raise socket.timeout(str(exc)) from None

    def close(self):
        self.reader.stop()
        try:
            self.sock.close()
        except OSError:
            pass

    def join(self):
        """Runs the full offline join and returns (join_game_body, flags)."""
        hs = (varint(0) + varint(PROTO) + varint(len(HOST)) + HOST.encode() +
              struct.pack(">H", 0) + varint(2))
        self.sock.sendall(make_frame(hs, -1))
        self.send_pkt(0x00, varint(len(self.name)) + self.name.encode() + b"\x00" * 16)

        # --- login ---
        saw_compression = False
        while True:
            pid, body = self.read_pkt()
            if pid == 0x03:  # Set Compression
                threshold, _ = rd_varint(body)
                self.threshold = threshold
                saw_compression = True
            elif pid == 0x02:  # Login Success
                u = body[:16]
                name, i = rd_string(body, 16)
                assert name == self.name and u != b"\x00" * 16
                assert len(body) >= i + 1 + 16  # properties + session id
                self.send_pkt(0x03)  # login acknowledged
                break
            else:
                raise AssertionError(f"unexpected login packet 0x{pid:02x}")

        # --- configuration ---
        flags = {"brand": False, "flags": False, "packs": False,
                 "registry": 0, "tags": False}
        while True:
            pid, body = self.read_pkt()
            if pid == 0x01:
                ch, i = rd_string(body)
                flags["brand"] = ch == "minecraft:brand"
                # The body after the channel is BrandPayload.STREAM_CODEC, i.e.
                # FriendlyByteBuf.writeUtf: [varint length][UTF-8]. Decoding it
                # fully is the regression lock for a raw (unlengthed) brand —
                # the client then reads the first character as the length and
                # throws DecoderException before the config phase can proceed.
                brand, i = rd_string(body, i)
                assert brand, "brand payload is empty"
                assert i == len(body), (
                    f"brand payload has {len(body) - i} trailing byte(s) "
                    f"{body[i:].hex()} (not a writeUtf string?)")
            elif pid == 0x0D:
                # ClientboundUpdateEnabledFeatures: [varint count][Identifier...]
                nfeat, i = rd_varint(body)
                feats = []
                for _ in range(nfeat):
                    feat, i = rd_string(body, i)
                    feats.append(feat)
                assert "minecraft:vanilla" in feats, f"feature flags: {feats}"
                assert i == len(body), (
                    f"feature flags: {len(body) - i} trailing byte(s)")
                flags["flags"] = True
            elif pid == 0x0F:  # known packs (clientbound) -> echo back
                npacks, i = rd_varint(body)
                packs = []
                for _ in range(npacks):
                    ns, i = rd_string(body, i)
                    pk, i = rd_string(body, i)
                    ver, i = rd_string(body, i)
                    packs.append((ns, pk, ver))
                assert packs, "server sent no known packs"
                assert i == len(body), (
                    f"known packs: {len(body) - i} trailing byte(s)")
                out = varint(len(packs))
                for ns, pk, ver in packs:
                    out += (varint(len(ns)) + ns.encode() + varint(len(pk)) + pk.encode() +
                            varint(len(ver)) + ver.encode())
                self.send_pkt(0x07, out)
                flags["packs"] = True
            elif pid == 0x07:
                flags["registry"] += 1
            elif pid == 0x0E:
                flags["tags"] = len(body) > 100
            elif pid == 0x03:  # Finish Configuration
                self.send_pkt(0x03)  # acknowledge
                break
            else:
                raise AssertionError(f"unexpected config packet 0x{pid:02x}")

        # --- play: consume until Join Game (0x32) ---
        while True:
            pid, body = self.read_pkt()
            if pid == 0x32:
                self.entity_id = decode_login_body(body)["entity_id"]
                # Join Game is the last of the join's own sequential reads; the
                # play stream from here belongs to the reader thread.
                self.reader.start()
                return body, flags, saw_compression


def wait_for_listen_line(proc, deadline=None):
    # The bind line is the startup event. With no deadline this waits until the
    # line arrives or the process closes its pipe; CTest's overall timeout is
    # the guard against a real hang. A deadline remains available for callers
    # that want a shorter, explicit bound.
    assert proc.stdout is not None
    while deadline is None or time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                raise AssertionError("plusmc exited before listening")
            continue
        m = re.search(rb"listening on [\d.]+:(\d+)", line)
        if m:
            return int(m.group(1))
    raise AssertionError("plusmc did not report a listening port in time")


def unpack_keepalive(client: "Client", body: bytes):
    """Acks a clientbound keep-alive (0x2d): echo the payload back (0x1c)."""
    assert len(body) >= 8
    client.send_pkt(0x1C, body[:8])


def settle_play(client: "Client", label: str, want_ka: bool):
    """Consumes the post-join play stream until the spawn chunk batch finished
    and (optionally) the first keep-alive have been seen and handled.

    Returns a dict of what was observed.
    """
    seen = {"batch_start": False, "chunks": 0, "chunk_coords": set(), "batch_size": None,
            "ka": 0, "info": [], "spawns": [], "self_data": None,
            "other_data": []}

    def note(pid: int, body: bytes):
        if pid == 0x47:
            seen["info"].extend(decode_player_info_add(body))
        elif pid == 0x01:
            seen["spawns"].append(decode_spawn_entity(body))
        elif pid == 0x65:
            eid, data = decode_entity_data(body)
            if eid == client.entity_id:
                seen["self_data"] = data
            else:
                seen["other_data"].append((eid, data))

    for _ in range(400):
        pid, body = client.read_pkt()
        if pid == 0x0C:
            assert not seen["batch_start"], "duplicate Chunk Batch Start"
            assert body == b"", "batch start must be empty"
            seen["batch_start"] = True
        elif pid == 0x2E:
            chunk = assert_lit_void_chunk(body)
            assert -1 <= chunk["x"] <= 1 and -1 <= chunk["z"] <= 1, (chunk["x"], chunk["z"])
            seen["chunk_coords"].add((chunk["x"], chunk["z"]))
            seen["chunks"] += 1
        elif pid == 0x0B:
            n, _ = rd_varint(body)
            seen["batch_size"] = n
            break
        elif pid == 0x2D:
            unpack_keepalive(client, body)
            seen["ka"] += 1
        note(pid, body)  # anything else (abilities, spawn position, ...) ignored
    assert seen["batch_start"], f"{label}: no Chunk Batch Start"
    assert seen["chunks"] == 9, f"{label}: expected 9 chunks, saw {seen['chunks']}"
    assert len(seen["chunk_coords"]) == 9 and (0, 0) in seen["chunk_coords"], \
        f"{label}: chunk coords wrong: {seen['chunk_coords']}"
    assert seen["batch_size"] == 9, f"{label}: batch size {seen['batch_size']}"
    # The local player's own synched data. `Avatar.defineSynchedData` defaults
    # DATA_PLAYER_MODE_CUSTOMISATION (index 16) to 0, so omitting this packet
    # renders every player with no cape and no outer skin layer at all.
    assert seen["self_data"] is not None, \
        f"{label}: no Set Entity Data for the local player (entity {client.entity_id})"
    # ...and no health word (index 9) alongside it: the local client's health is
    # its own set_health, so the burst it is sent about itself leaves the word to
    # the *other* clients' copy of it.
    assert seen["self_data"] == {16: (0, 0x7F)}, \
        f"{label}: local DATA_PLAYER_MODE_CUSTOMISATION must be serializer 0 value " \
        f"0x7F (all second-skin layers, cape included); got {seen['self_data']}"
    if want_ka:
        # A keep-alive is due ~1.2s after join; wait for one and ack it.
        for _ in range(60):
            pid, body = client.read_pkt()
            if pid == 0x2D:
                unpack_keepalive(client, body)
                seen["ka"] += 1
                break
            if pid == 0x0B or pid == 0x0C or pid == 0x2E:
                raise AssertionError(f"{label}: unexpected late chunk packet")
            note(pid, body)
        assert seen["ka"] >= 1, f"{label}: no keep-alive observed"
    return seen


# The spawn chunks of the in-memory world: all-air columns, so the light engine's
# answer for them is exact and worth pinning rather than merely reading back.
#
# The world is 24 sections tall from y = -4 (min_section_y), and its columns hold
# nothing at all, so every section has a full-sky array; the one *padding* light
# section below the world (slot 0, y = -5) and both block-light layers are empty.
SKY_SECTION_SLOTS = 24
LIGHT_ARRAY_BYTES = 2048
# The same body before Phase 4 milestone B wired the engine in: 1104 bytes, of
# which the light half was four zero masks and two empty array lists (6 bytes).
UNLIT_VOID_BODY = 1104
# 26.3 masks are length-prefixed byte arrays: sky bits 1..24 -> 4 bytes,
# empty_sky bit 0 -> 1, empty_block bits 0..24 -> 4, block empty.
LIT_VOID_MASK_BYTES = (1 + 4) + 1 + (1 + 1) + (1 + 4)
LIT_VOID_BODY = (UNLIT_VOID_BODY - 6 + LIT_VOID_MASK_BYTES +
                 (1 + SKY_SECTION_SLOTS * (2 + LIGHT_ARRAY_BYTES)) + 1)


def decode_chunk_light(body: bytes) -> dict:
    """The light half of a clientbound level_chunk_with_light (0x2e) body.

    Walks every field up to the light data — x, z, the heightmaps, the section
    blob, the block entities — then decodes what follows: the four masks (26.3's
    length-prefixed byte arrays, `BitSet.toByteArray`, so slot `i` of the sky
    mask is the light section at `min_section_y - 1 + i`), then the two array
    lists (a varint count of varint-length byte arrays). The tail must be
    consumed exactly, so a slip in the walk cannot hide behind a prefix that
    happens to look sane.
    """
    (x, z) = struct.unpack(">ii", body[0:8])
    pos = 8
    n_heightmaps, pos = rd_varint(body, pos)
    for _ in range(n_heightmaps):
        _, pos = rd_varint(body, pos)  # the heightmap type
        n_longs, pos = rd_varint(body, pos)
        pos += 8 * n_longs
    section_bytes, pos = rd_varint(body, pos)
    pos += section_bytes
    n_block_entities, pos = rd_varint(body, pos)
    assert n_block_entities == 0, f"a void chunk held {n_block_entities} block entities"
    masks = {}
    for name in ("sky", "block", "empty_sky", "empty_block"):
        n_bytes, pos = rd_varint(body, pos)
        bits = int.from_bytes(body[pos:pos + n_bytes], "little")
        pos += n_bytes
        masks[name] = bits
    arrays = {}
    for name in ("sky", "block"):
        n, pos = rd_varint(body, pos)
        out = []
        for _ in range(n):
            length, pos = rd_varint(body, pos)
            out.append(body[pos:pos + length])
            pos += length
        arrays[name] = out
    assert pos == len(body), (
        f"chunk body not consumed exactly: {len(body) - pos} trailing byte(s)")
    return {"x": x, "z": z, "masks": masks, **arrays}


def assert_lit_void_chunk(body: bytes) -> dict:
    """A spawn chunk carries full sky light and no block light, exactly."""
    assert len(body) == LIT_VOID_BODY, (
        f"chunk body {len(body)} != {LIT_VOID_BODY} (the lit all-air body)")
    chunk = decode_chunk_light(body)
    assert len(chunk["sky"]) == SKY_SECTION_SLOTS, (
        f"{len(chunk['sky'])} sky arrays, expected one per section")
    assert all(a == b"\xff" * LIGHT_ARRAY_BYTES for a in chunk["sky"]), (
        "an open-sky all-air column must be 15 everywhere")
    assert chunk["block"] == [], "an all-air column emits no block light"
    # Slot 0 is the padding light section under the world; slots 1..24 are the
    # world's own sections, every one of them real.
    assert chunk["masks"]["sky"] == ((1 << (SKY_SECTION_SLOTS + 1)) - 2), (
        f"sky mask {chunk['masks']['sky']:#x} is not slots 1..{SKY_SECTION_SLOTS}")
    assert chunk["masks"]["empty_sky"] == 1, "only the padding section is empty above"
    assert chunk["masks"]["block"] == 0, "an all-air chunk has no non-empty block light"
    assert chunk["masks"]["empty_block"] == ((1 << (SKY_SECTION_SLOTS + 1)) - 1), \
        "every block-light section is empty"
    return chunk


def decode_login_body(body: bytes) -> dict:
    """Fully decodes ClientboundLoginPacket (the play "login" packet).

    Walks every field of ClientboundLoginPacket.write / CommonPlayerSpawnInfo.write
    in order and requires the buffer to be consumed exactly, so a slip in field
    order or width can never hide behind a prefix that happens to look sane.
    """
    (entity_id,) = struct.unpack(">i", body[0:4])
    pos = 4
    hardcore = body[pos]
    pos += 1
    n, pos = rd_varint(body, pos)
    dims = []
    for _ in range(n):
        d, pos = rd_string(body, pos)
        dims.append(d)
    max_players, pos = rd_varint(body, pos)
    chunk_radius, pos = rd_varint(body, pos)
    simulation_distance, pos = rd_varint(body, pos)
    reduced_debug_info = body[pos]
    pos += 1
    show_death_screen = body[pos]
    pos += 1
    do_limited_crafting = body[pos]
    pos += 1
    # ---- CommonPlayerSpawnInfo ----
    dim_type_id, pos = rd_varint(body, pos)
    dimension, pos = rd_string(body, pos)
    (seed,) = struct.unpack(">q", body[pos:pos + 8])
    pos += 8
    game_type = body[pos]
    pos += 1
    previous_game_type = body[pos]
    pos += 1
    is_debug = body[pos]
    pos += 1
    is_flat = body[pos]
    pos += 1
    has_death_location = body[pos]
    pos += 1
    if has_death_location:  # Optional<GlobalPos>: dimension + packed BlockPos
        _, pos = rd_string(body, pos)
        pos += 8
    portal_cooldown, pos = rd_varint(body, pos)
    sea_level, pos = rd_varint(body, pos)
    online_mode = body[pos]
    pos += 1
    enforces_secure_chat = body[pos]
    pos += 1
    assert pos == len(body), (
        f"login packet not consumed exactly: {len(body) - pos} trailing byte(s)")
    return {
        "entity_id": entity_id, "hardcore": hardcore, "dims": dims,
        "max_players": max_players, "chunk_radius": chunk_radius,
        "simulation_distance": simulation_distance,
        "reduced_debug_info": reduced_debug_info,
        "show_death_screen": show_death_screen,
        "do_limited_crafting": do_limited_crafting,
        "dim_type_id": dim_type_id, "dimension": dimension, "seed": seed,
        "game_type": game_type, "previous_game_type": previous_game_type,
        "is_debug": is_debug, "is_flat": is_flat,
        "has_death_location": has_death_location,
        "portal_cooldown": portal_cooldown, "sea_level": sea_level,
        "online_mode": online_mode, "enforces_secure_chat": enforces_secure_chat,
    }


def decode_join_game(body: bytes, expect_online=None):
    """Back-compat wrapper: entity id + dimension list, with an optional check
    that Login's online-mode byte agrees with how the server was started."""
    info = decode_login_body(body)
    assert info["entity_id"] >= 0 and info["hardcore"] == 0 and info["dims"], \
        "join game fields invalid"
    assert info["chunk_radius"] > 0, info
    assert info["dimension"] in info["dims"], info
    if expect_online is not None:
        assert info["online_mode"] == expect_online, (
            f"login online-mode byte is {info['online_mode']}, expected "
            f"{expect_online} (server is {'online' if expect_online else 'offline'})")
    return info["entity_id"], info["dims"]


def main() -> None:
    data_root = pathlib.Path(__file__).resolve().parent.parent / "data" / "vanilla"
    assert data_root.is_dir(), "no data/vanilla extraction present; run tools/vanilla-data"

    # Optional: drive an already-running server (useful when debugging with a
    # visible server log) instead of spawning our own.
    external = os.environ.get("PLUSMC_PORT")
    proc = None
    if external:
        port = int(external)
    else:
        env = dict(os.environ)
        env["PLUSMC_KEEPALIVE_MS"] = "1200"
        env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "4000"
        # The join burst is read sequentially (and its keep-alives acked by
        # hand), so nothing here is paced by survival ticks; the clock is pinned
        # — one packet, one tick, no wall-time catch-up — so that stays true of
        # any step added later.
        env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
        # --view-distance 1 keeps the spawn burst at 3x3 so the chunk-batch
        # assertions below stay tight and fast; the burst is always the full
        # advertised radius (see world_join_test for the view-distance path).
        proc, port = wire_client.start_server(
            [str(BIN), "--host", HOST, "--port", "0", "--motd", "join test",
             "--offline-mode", "--view-distance", "1", "--data", str(data_root)],
            wait_for_listen_line, env=env)
    try:
        # A replay has no server to reach: `transport` serves the recording.
        if not wire_client.replaying():
            deadline = time.time() + 10
            while True:
                try:
                    s = socket.create_connection((HOST, port), timeout=3)
                    s.close()
                    break
                except OSError:
                    if proc is not None and proc.poll() is not None:
                        raise AssertionError("plusmc exited early")
                    if time.time() > deadline:
                        raise AssertionError("could not reach plusmc")
                    time.sleep(0.05)

        a = Client(PLAYER_A, port)
        wire_client.record(a, note="join: the login/config/play handshake, step by step")
        b = Client(PLAYER_B, port)
        wire_client.record(b, note="join: the second client of the same session")
        a_join, a_flags, a_comp = a.join()
        a_eid, a_dims = decode_join_game(a_join, expect_online=0)
        assert a_flags["brand"] and a_flags["flags"] and a_flags["packs"] and \
            a_flags["tags"] and a_flags["registry"] >= 20, f"config incomplete: {a_flags}"
        assert a_comp, "compression was not enabled"
        assert a_dims[:1] == ["minecraft:overworld"], a_dims

        # A is alone: settle its play entry (chunks + keep-alive) and check
        # its only tab entry is its own.
        a_seen = settle_play(a, "A", want_ka=True)
        assert a_seen["ka"] >= 1
        assert a_seen["info"] == [PLAYER_A], a_seen["info"]

        # A carries absorption *before* B arrives. The synched data a joining
        # client is sent about a player already in the world is vanilla's
        # `ServerEntity.sendPairingData` — every value that differs from its
        # default — and absorption's default is 0 (`Player.defineSynchedData`),
        # so a client that joins while the hearts are up has to be told about
        # them. Without the pairing entry its copy stays at 0 until the number
        # next changes, which for a player who is not being hit is never; the
        # *change* path is the other test (health_test's step 10).
        cmd = f"effect give {PLAYER_A} absorption 100 0"
        a.send_pkt(0x07, varint(len(cmd)) + cmd.encode())
        word = None
        for _ in range(40):
            pid, body = a.read_pkt()
            if pid == 0x2D:
                unpack_keepalive(a, body)
            elif pid == 0x65:
                eid, data = decode_entity_data(body)
                if eid == a_eid and 17 in data:
                    word = data[17]
                    break
        assert word == (3, 4.0), \
            f"A's own client must be told its 4.0 absorption (index 17): {word}"
        print("pairing: A carries 4.0 absorption before B joins")

        # B joins while A is in play. B must be told about A (tab entry +
        # spawn) and about itself; A must receive B's entry + spawn too.
        b_join, b_flags, b_comp = b.join()
        b_eid, _ = decode_join_game(b_join, expect_online=0)
        assert b_comp, "compression was not enabled"
        assert a_eid != b_eid, "entity ids must be unique"
        b_seen = settle_play(b, "B", want_ka=True)
        assert b_seen["ka"] >= 1
        assert sorted(b_seen["info"]) == [PLAYER_A, PLAYER_B], b_seen["info"]
        assert len(b_seen["spawns"]) == 1, b_seen["spawns"]
        s_eid, s_uuid, s_type = b_seen["spawns"][0]
        assert s_type == 159, f"spawn type must be player (159), got {s_type}"
        # The existing player's synched data must follow its spawn, or A renders
        # for B with no cape and no outer skin layer — and it carries A's health
        # *and absorption* words, which is how a newcomer learns a bar and the
        # hearts over it: `ServerEntity.sendPairingData` packs every non-default
        # value (health's default is 1.0, absorption's is 0.0; the *change* path
        # is the same words). Index 10 is the status-effect particle list, which
        # is the same kind of pairing entry and carries the effect A is standing
        # under: `update_mob_effect` went to A's own client and is long gone, so
        # this list is the only thing that tells B there is anything to draw.
        # (minecraft:entity_effect, id 28, tinted with Absorption's colour
        # 0x2552A5 under the 255 alpha MobEffect's own factory uses.)
        assert b_seen["other_data"] == [
            (s_eid, {9: (3, 20.0), 10: (17, [(28, 0xFF2552A5)]),
                     16: (0, 0x7F), 17: (3, 4.0)})], b_seen["other_data"]
        assert s_eid == a_eid and s_uuid == offline_uuid(PLAYER_A), (s_eid, a_eid, s_uuid)
        print("presence: B sees A in tab + spawned")

        # A's view of B's join arrived while B was settling; drain it.
        a_got = {"info": [], "spawns": []}
        for _ in range(80):
            pid, abody = a.read_pkt()
            if pid == 0x47:
                a_got["info"].extend(decode_player_info_add(abody))
            elif pid == 0x01:
                a_got["spawns"].append(decode_spawn_entity(abody))
            elif pid == 0x2D:
                unpack_keepalive(a, abody)
            if PLAYER_B in a_got["info"] and a_got["spawns"]:
                break
        assert PLAYER_B in a_got["info"], f"A missing B in tab: {a_got}"
        assert len(a_got["spawns"]) == 1, a_got["spawns"]
        s_eid, s_uuid, s_type = a_got["spawns"][0]
        assert s_type == 159 and s_eid == b_eid and s_uuid == offline_uuid(PLAYER_B), \
            (s_eid, b_eid, s_uuid)
        print("presence: A sees B in tab + spawned")

        # A sends a chat message; both A and B must receive it as system chat.
        msg = "hello " + PLAYER_B + "!"
        body = (varint(len(msg)) + msg.encode() + struct.pack(">q", int(time.time() * 1000)) +
                struct.pack(">q", 0) + b"\x00" + varint(0) + b"\x00\x00\x00" + b"\x01")
        a.send_pkt(0x09, body)

        def expect_chat(client: Client, label: str):
            for _ in range(60):
                pid, cbody = client.read_pkt()
                if pid == 0x7C:
                    text = chat_text(cbody)
                    if "<" + PLAYER_A + ">" in text and msg in text:
                        print(f"{label}: chat echo OK")
                        return
                elif pid == 0x2D:
                    unpack_keepalive(client, cbody)
            raise AssertionError(f"{label}: chat echo not received")

        expect_chat(a, "A")
        expect_chat(b, "B")

        # ---- Movement: accepted without disconnect, and relayed to B ----
        # A starts at the default spawn (8, 100, 8). The first payload sent after
        # a tracking relationship starts is *absolute* (0x23): B's position base
        # is the Spawn Entity it just read, while a delta is relative to the
        # relay's own base, and the two cannot be mixed on a fresh viewer. So this
        # +0.5/+0.5 step (position+rotation packet, 0x1f) arrives as a Sync Entity
        # Position carrying the new absolute position.
        mv = struct.pack(">ddd", 8.5, 100.0, 8.5) + struct.pack(">ff", 0.0, 0.0) + b"\x01"
        a.send_pkt(0x1F, mv)
        try:
            pid, _ = a.read_pkt(1.0)  # must not raise EOF; any packet is fine
            assert pid != 0x20, "server sent a play disconnect"
        except socket.timeout:
            pass  # nothing sent back: fine

        pid, body = scan_for(b, "B sees A's first move", lambda p, b: p == 0x23)
        eid, tx, ty, tz = decode_teleport(body)
        assert eid == a_eid and (tx, ty, tz) == (8.5, 100.0, 8.5), \
            f"first move wrong: {(eid, tx, ty, tz)}"
        print("movement relay: B sees A's first move as the absolute teleport")

        # From here the pair shares a base, so a step is a relative move (0x35)
        # again: another +0.5/+0.5 blocks is deltas 0.5 * 4096 = 2048.
        mv = struct.pack(">ddd", 9.0, 100.0, 9.0) + b"\x01"
        a.send_pkt(0x1E, mv)
        pid, body = scan_for(b, "B sees A's small move", lambda p, b: p == 0x36)
        eid, dx, dy, dz, ground = decode_rel_move(body)
        assert (eid, dx, dy, dz, ground) == (a_eid, 2048, 0, 2048, 1), \
            f"rel move wrong: {(eid, dx, dy, dz, ground)}"
        print("movement relay: B sees A's rel move")

        # Rotation only (0x20, look): yaw 90 -> angle byte 64 (0x40).
        a.send_pkt(0x20, struct.pack(">ff", 90.0, 0.0) + b"\x01")
        pid, body = scan_for(b, "B sees A's look", lambda p, b: p == 0x39)
        eid, yaw, pitch, ground = decode_entity_look(body)
        assert (eid, yaw, pitch, ground) == (a_eid, 64, 0, 1), \
            f"look wrong: {(eid, yaw, pitch, ground)}"
        print("movement relay: B sees A's look")

        # Step +1.5/+0.5 blocks with a simultaneous rotation change to yaw 180
        # (angle byte 128): Entity Move & Look (0x36).
        mv = struct.pack(">ddd", 10.5, 100.0, 9.5) + struct.pack(">ff", 180.0, 0.0) + b"\x01"
        a.send_pkt(0x1F, mv)
        pid, body = scan_for(b, "B sees A's move+look", lambda p, b: p == 0x37)
        eid, dx, dy, dz, yaw, pitch, ground = decode_move_look(body)
        assert (eid, dx, dy, dz, yaw, pitch, ground) == (a_eid, 6144, 0, 2048, 128, 0, 1), \
            f"move+look wrong: {(eid, dx, dy, dz, yaw, pitch, ground)}"
        print("movement relay: B sees A's move+look")

        # Big jump (position-only, 0x1e): the delta (+17.5 blocks) does not fit
        # the i16 field, so the relay falls back to an absolute entity teleport
        # (0x23). It stays inside the 48-block tracking range, which is what
        # keeps B a viewer of A through it.
        mv = struct.pack(">ddd", 28.0, 100.0, 9.5) + b"\x01"
        a.send_pkt(0x1E, mv)
        pid, body = scan_for(b, "B sees A's teleport", lambda p, b: p == 0x23)
        eid, tx, ty, tz = decode_teleport(body)
        assert eid == a_eid and (tx, ty, tz) == (28.0, 100.0, 9.5), \
            f"teleport wrong: {(eid, tx, ty, tz)}"
        print("movement relay: B sees A's teleport")

        # ---- Interest management: out of range is not tracked ----
        # A leaves the 48-block tracking range. Vanilla's ChunkMap.TrackedEntity
        # drops the entity here, so B is sent Remove Entities (0x4d) and nothing
        # about A after it. The tab entry stays: Player Info Update is global,
        # so this is the in-world entity, not the player.
        mv = struct.pack(">ddd", 5000.0, 100.0, 5000.0) + b"\x01"
        a.send_pkt(0x1E, mv)
        pid, body = scan_for(b, "B sees A despawn", lambda p, b: p == 0x4E)
        removed = decode_remove_entities(body)
        assert a_eid in removed, f"despawn was for {removed}, not A ({a_eid})"
        print("interest management: A leaves B's range -> Remove Entities")

        # A keeps moving, far away: B must hear nothing about A's entity. The
        # assertion is on the entity ids, not on silence — keep-alives and
        # anything about another entity may still arrive.
        for i in range(4):
            a.send_pkt(0x1E, struct.pack(">ddd", 5040.0 + i, 100.0, 5000.0) + b"\x01")
        unexpected = []
        deadline = time.time() + 1.0
        while time.time() < deadline:
            try:
                pid, nbody = b.read_pkt(max(0.05, deadline - time.time()))
            except socket.timeout:
                break
            if pid == 0x2D:
                unpack_keepalive(b, nbody)
            elif pid in (0x01, 0x23, 0x36, 0x37, 0x39, 0x4E, 0x65):
                unexpected.append(pid)
        assert not unexpected, f"B got entity packets for an untracked A: {unexpected}"
        print("interest management: no relay while A is 5000 blocks away")

        # A comes back inside the range: B is sent the pairing burst again — a
        # fresh Spawn Entity (0x01) and the synched data (0x65) — and the move
        # that follows is relayed against the fresh entity.
        a.send_pkt(0x1E, struct.pack(">ddd", 8.0, 100.0, 8.0) + b"\x01")
        pid, body = scan_for(b, "B sees A respawn", lambda p, b: p == 0x01)
        eid, uuid, typ = decode_spawn_entity(body)
        assert (eid, uuid, typ) == (a_eid, offline_uuid(PLAYER_A), 159), (eid, uuid, typ)
        pid, body = scan_for(b, "B sees A's synched data again", lambda p, b: p == 0x65)
        eid, _ = decode_entity_data(body)
        assert eid == a_eid, eid
        print("interest management: A re-enters -> fresh Spawn Entity + synched data")

        a.send_pkt(0x1E, struct.pack(">ddd", 8.5, 100.0, 8.5) + b"\x01")
        pid, body = scan_for(b, "B sees A move again", lambda p, b: p == 0x36)
        eid, dx, dy, dz, ground = decode_rel_move(body)
        assert (eid, dx, dy, dz) == (a_eid, 2048, 0, 2048), (eid, dx, dy, dz)
        print("interest management: the relay resumes inside the range")

        # ---- The same transitions under churn: a walk across the boundary ----
        # The steps above proved one clean exit and one clean re-entry. What a
        # player hovering on the line exercises is the *bookkeeping*: every exit
        # must send exactly one Remove Entities and every re-entry exactly one
        # pairing burst, with no leaked viewer (a later move relayed while A is
        # out of range, which the silence step above already catches) and no
        # duplicate one (an extra Spawn Entity on a single crossing, which only a
        # repeated crossing can expose). B stays at x = 8.0 and A walks out to
        # 70.5 blocks of it and back to 40.5, two blocks per step, twice: two
        # exits and two re-entries, four boundary events to count.
        #
        # Two-block steps are ordinary rel moves (2 * 4096 = 8192, inside the i16
        # field), so the relay stays on the movement path this slice gates; at
        # 0.02 s per step this is a client moving at a client's rate (20 Hz), and
        # the assertions below are per crossing rather than per second, so the
        # pacing only decides how many steps a crossing takes. A starts 40.5
        # blocks from B and steps *up* to 70.5 before coming back, so the walk
        # crosses both the 48-block entry line and the wider drop line whatever
        # the hysteresis margin is (PLUSMC_PLAYER_TRACKING_HYSTERESIS, 8 by
        # default). z stays 8.5 on both sides, so the distance is |dx|.
        walk = []
        for _ in range(2):
            walk += [8.5 + 2.0 * i for i in range(1, 36)]   # 10.5 -> 78.5, out of range
            walk += [8.5 + 2.0 * i for i in range(34, 21, -1)]  # 76.5 -> 52.5, back in
        for x in walk:
            a.send_pkt(0x1E, struct.pack(">ddd", x, 100.0, 8.5) + b"\x01")
            time.sleep(0.02)

        # What B saw about A, in order: one TCP stream is the server's own
        # processing order, so the event list is the transition log.
        seen, deadline = [], time.time() + 3.0
        while time.time() < deadline:
            try:
                pid, nbody = b.read_pkt(max(0.05, deadline - time.time()))
            except socket.timeout:
                break
            if pid == 0x2D:
                unpack_keepalive(b, nbody)
                continue
            if pid in (0x01, 0x4E, 0x65, 0x23, 0x36, 0x37, 0x39):
                seen.append(pid)
        events = [p for p in seen if p in (0x01, 0x4E)]
        assert events == [0x4E, 0x01, 0x4E, 0x01], \
            (f"two crossings must be remove/spawn twice, got "
             f"{[hex(p) for p in events]} (all entity packets: {[hex(p) for p in seen]})")
        assert seen.count(0x65) == 2, \
            f"one synched-data push per re-entry: {[hex(p) for p in seen]}"
        # Nothing about A's entity while A is out of range: no movement packet may
        # sit between an exit and the next re-entry.
        for i, pid in enumerate(seen):
            if pid != 0x4E:
                continue
            rest = seen[i + 1:]
            after = rest[:rest.index(0x01)] if 0x01 in rest else rest
            assert not [p for p in after if p in (0x23, 0x36, 0x37, 0x39)], \
                (f"movement relayed while out of range: "
                 f"{[hex(p) for p in seen]}")
        print("interest management under churn: 2 exits -> 2 removes, 2 re-entries -> "
              "2 pairing bursts, nothing relayed in between")

        # The last re-entry left the relationship live, and its base is that
        # spawn: the next two-block step is relayed as a rel move again. It lands
        # at 54.5 (46.5 blocks from B), inside the 48-block entry line, so this
        # holds whichever hysteresis the server runs — at 48.5 blocks the exit it
        # would take under the strict single-threshold rule is the very flap the
        # drop margin exists to remove.
        a.send_pkt(0x1E, struct.pack(">ddd", 54.5, 100.0, 8.5) + b"\x01")
        pid, body = scan_for(b, "B sees A move after the churn", lambda p, b: p == 0x36)
        eid, dx, dy, dz, ground = decode_rel_move(body)
        assert (eid, dx, dy, dz) == (a_eid, 8192, 0, 0), (eid, dx, dy, dz)
        print("interest management under churn: the relay still carries rel moves")

        # Ping request (0x26) is echoed as Pong (0x3f) with the same int32.
        # After the big jump above we have a whole chunk-stream burst queued
        # behind the teleport — drain it before demanding the pong.
        a.send_pkt(0x26, struct.pack(">i", 0x11223344))
        pong = None
        for _ in range(400):
            pid, pbody = a.read_pkt()
            if pid == 0x3F:
                pong = pbody
                break
            if pid == 0x2D:
                unpack_keepalive(a, pbody)
            elif pid in (0x0C, 0x0B, 0x2E, 0x47, 0x01):
                # chunk stream / presence — just drain
                continue
        assert pong == struct.pack(">i", 0x11223344), f"pong mismatch: {pong!r}"
        print("ping/pong: OK")

        # B leaves: A must be told to despawn B's entity (0x4e) and remove B
        # from the tab list (0x46), mirroring vanilla's leave burst.
        b.close()
        leave = {"despawn": None, "remove": None}
        for _ in range(80):
            pid, abody = a.read_pkt()
            if pid == 0x4E:
                n, i = rd_varint(abody)
                e, _ = rd_varint(abody, i)
                leave["despawn"] = (n, e)
            elif pid == 0x46:
                n, i = rd_varint(abody)
                leave["remove"] = (n, abody[i:i + 16])
            elif pid == 0x2D:
                unpack_keepalive(a, abody)
            if leave["despawn"] and leave["remove"]:
                break
        assert leave["despawn"] == (1, b_eid), f"despawn wrong: {leave}"
        assert leave["remove"] == (1, offline_uuid(PLAYER_B)), f"remove wrong: {leave}"
        print("leave: A sees B despawn + tab remove")

        a.close()
        print("join: OK (flags", a_flags, ")")

        if proc is not None:
            proc.terminate()
            try:
                rc = proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                raise AssertionError("plusmc did not exit after SIGTERM")
            assert rc == 0, f"expected clean exit on SIGTERM, got {rc}"
            print("graceful shutdown: OK")
    except BaseException:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        raise

    print("PASS")


if __name__ == "__main__":
    main()
