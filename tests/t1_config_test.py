#!/usr/bin/env python3
"""Phase 9 T1 slices 2-3: 1.21.11 login, configuration and play entry.

The server accepts protocol 774, emits 1.21.11's Login Success shape (UUID +
name + properties, no 26.3 session id), serves 1.21.11's own registry listings
and tags, completes Finish Configuration, then enters Play with the 774 Join
Game, the load-start game event, a full view-distance chunk batch and the
initial player position. The chunk bodies use 1.21.11's advertised void biome;
block-state translation is the next slice.

Usage: t1_config_test.py /path/to/plusmc
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
import wire_client  # noqa: E402
from nbt_component import chat_text, rd_nbt_component  # noqa: E402

BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
HOST = "127.0.0.1"
PROTO = 774
NAME = "T1Cap"
LOGIN_SUCCESS = 0x02
SET_COMPRESSION = 0x03
CONFIG_BRAND = 0x01
CONFIG_FINISH = 0x03
CONFIG_REGISTRY = 0x07
CONFIG_FEATURE_FLAGS = 0x0C
CONFIG_TAGS = 0x0D
CONFIG_KNOWN_PACKS = 0x0E
PLAY_DISCONNECT = 0x20
PLAY_JOIN = 0x30
PLAY_COMMANDS = 0x10
PLAY_GAME_EVENT = 0x26
PLAY_CHUNK = 0x2C
PLAY_BATCH_START = 0x0C
PLAY_BATCH_FINISHED = 0x0B
PLAY_POSITION = 0x46
PLAY_KEEPALIVE = 0x2B
PLAY_SYSTEM_CHAT = 0x77
PLAY_MOVE_POS = 0x1D
PLAY_CHAT = 0x08
PLAY_CHAT_COMMAND = 0x06


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
    value = 0
    for k in range(5):
        byte = b[i]
        i += 1
        value |= (byte & 0x7F) << (7 * k)
        if not (byte & 0x80):
            return value, i
    raise ValueError("bad varint")


def rd_string(b: bytes, i: int = 0):
    n, i = rd_varint(b, i)
    return b[i:i + n].decode(), i + n


def recvn(sock, n: int) -> bytes:
    out = b""
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise EOFError(f"unexpected EOF (wanted {n}, got {len(out)})")
        out += chunk
    return out


def read_varint_sock(sock):
    out = bytearray()
    while True:
        c = recvn(sock, 1)
        out += c
        if not c[0] & 0x80:
            return bytes(out), len(out)


def frame(payload: bytes, threshold: int) -> bytes:
    if threshold < 0:
        return varint(len(payload)) + payload
    if len(payload) < threshold:
        body = varint(0) + payload
    else:
        body = varint(len(payload)) + zlib.compress(payload)
    return varint(len(body)) + body


def read_packet(sock, threshold: int):
    raw, n = read_varint_sock(sock)
    length, _ = rd_varint(raw)
    if threshold < 0:
        data = recvn(sock, length)
    else:
        dlen_raw, dn = read_varint_sock(sock)
        dlen, _ = rd_varint(dlen_raw)
        data = recvn(sock, length - dn)
        if dlen:
            data = zlib.decompress(data)
    pid, off = rd_varint(data)
    return pid, data[off:]


def send(sock, threshold: int, pid: int, body: bytes = b""):
    sock.sendall(frame(varint(pid) + body, threshold))


def wait_listening(proc: subprocess.Popen, timeout: float = 15) -> int:
    assert proc.stdout is not None
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                raise AssertionError("plusmc exited before listening")
            continue
        match = re.search(rb"listening on [\d.]+:(\d+)", line)
        if match:
            return int(match.group(1))
    raise AssertionError("plusmc did not report a listening port in time")


def main() -> None:
    if BIN is None:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    data_root = pathlib.Path(__file__).resolve().parent.parent / "data" / "vanilla"
    assert (data_root / "1.21.11" / "clientconfig" / "tags.bin").is_file(), \
        "the 1.21.11 client-config capture is missing"

    proc = None
    sock = None
    try:
        proc, port = wire_client.start_server(
            [str(BIN), "--host", HOST, "--port", "0", "--protocol", "777",
             "--version-name", "26.3-test", "--offline-mode",
             "--data", str(data_root)],
            wait_listening)

        sock = wire_client.transport(HOST, port, timeout=10)
        sock.settimeout(10)
        threshold = -1

        handshake = (varint(0) + varint(PROTO) + varint(len(HOST)) + HOST.encode() +
                     struct.pack(">H", port) + varint(2))
        sock.sendall(frame(handshake, -1))
        send(sock, -1, 0x00, varint(len(NAME)) + NAME.encode() + b"\x00" * 16)

        while True:
            pid, body = read_packet(sock, threshold)
            if pid == SET_COMPRESSION:
                threshold, _ = rd_varint(body)
                assert threshold >= 0
            elif pid == LOGIN_SUCCESS:
                break
            else:
                raise AssertionError(f"unexpected login packet 0x{pid:02x}")
        assert len(body) == 16 + 1 + len(NAME) + 1, (
            f"1.21.11 Login Success has {len(body)} bytes, expected UUID + name + "
            f"properties count and no session id")
        name, i = rd_string(body, 16)
        props, i = rd_varint(body, i)
        assert name == NAME and props == 0 and i == len(body), (
            f"login success shape: name={name!r} props={props} consumed={i}/{len(body)}")
        send(sock, threshold, 0x03)

        registries = 0
        tags = 0
        brand = feature_flags = known_packs = False
        while True:
            pid, body = read_packet(sock, threshold)
            if pid == CONFIG_BRAND:
                channel, i = rd_string(body)
                value, i = rd_string(body, i)
                assert channel == "minecraft:brand" and value and i == len(body)
                brand = True
            elif pid == CONFIG_FEATURE_FLAGS:
                count, i = rd_varint(body)
                flags = []
                for _ in range(count):
                    flag, i = rd_string(body, i)
                    flags.append(flag)
                assert "minecraft:vanilla" in flags and i == len(body), flags
                feature_flags = True
            elif pid == CONFIG_KNOWN_PACKS:
                count, i = rd_varint(body)
                packs = []
                for _ in range(count):
                    ns, i = rd_string(body, i)
                    pack, i = rd_string(body, i)
                    version, i = rd_string(body, i)
                    packs.append((ns, pack, version))
                assert (ns, pack, version) == ("minecraft", "core", "1.21.11"), packs
                assert i == len(body)
                out = varint(len(packs))
                for ns, pack, version in packs:
                    for text in (ns, pack, version):
                        out += varint(len(text)) + text.encode()
                send(sock, threshold, 0x07, out)
                known_packs = True
            elif pid == CONFIG_REGISTRY:
                rid, i = rd_string(body)
                count, i = rd_varint(body, i)
                for _ in range(count):
                    _, i = rd_string(body, i)
                    assert body[i] == 0, f"{rid}: unexpected NBT"
                    i += 1
                assert i == len(body), f"{rid}: trailing bytes"
                registries += 1
            elif pid == CONFIG_TAGS:
                assert len(body) > 20000, f"1.21.11 tags is only {len(body)} bytes"
                tags = len(body)
            elif pid == CONFIG_FINISH:
                send(sock, threshold, 0x03)
                break
            else:
                raise AssertionError(f"unexpected configuration packet 0x{pid:02x}")

        assert brand and feature_flags and known_packs, (brand, feature_flags, known_packs)
        assert registries == 23, f"1.21.11 sends 23 registries, got {registries}"
        assert tags > 20000, tags

        pid, body = read_packet(sock, threshold)
        assert pid == PLAY_JOIN, f"expected 1.21.11 Join Game 0x30, got 0x{pid:02x}"
        entity_id = struct.unpack(">i", body[:4])[0]
        i = 4
        hardcore = body[i]
        i += 1
        dim_count, i = rd_varint(body, i)
        dimensions = []
        for _ in range(dim_count):
            name, i = rd_string(body, i)
            dimensions.append(name)
        max_players, i = rd_varint(body, i)
        view_distance, i = rd_varint(body, i)
        simulation_distance, i = rd_varint(body, i)
        reduced_debug, i = body[i], i + 1
        respawn_screen, i = body[i], i + 1
        limited_crafting, i = body[i], i + 1
        dim_type, i = rd_varint(body, i)
        dimension, i = rd_string(body, i)
        hashed_seed = struct.unpack(">q", body[i:i + 8])[0]
        i += 8
        gamemode = body[i]
        previous_gamemode = body[i + 1]
        is_debug = body[i + 2]
        is_flat = body[i + 3]
        has_death = body[i + 4]
        i += 5
        portal_cooldown, i = rd_varint(body, i)
        sea_level, i = rd_varint(body, i)
        enforces_secure_chat = body[i]
        i += 1
        assert i == len(body), f"Join Game left {len(body) - i} bytes"
        assert entity_id > 0 and hardcore == 0
        assert dimensions == ["minecraft:overworld", "minecraft:the_nether", "minecraft:the_end"]
        assert dimension == "minecraft:overworld" and view_distance > 0
        assert previous_gamemode == 0xFF and has_death == 0
        assert enforces_secure_chat == 0

        chunks = set()
        saw_event = saw_position = saw_batch_start = saw_commands = False
        batch_count = -1
        while True:
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_GAME_EVENT:
                assert len(body) == 5 and body[0] == 13, body.hex()
                saw_event = True
            elif pid == PLAY_POSITION:
                saw_position = True
            elif pid == PLAY_BATCH_START:
                saw_batch_start = True
            elif pid == PLAY_CHUNK:
                x, z = struct.unpack(">ii", body[:8])
                chunks.add((x, z))
            elif pid == PLAY_COMMANDS:
                saw_commands = True
            elif pid == PLAY_BATCH_FINISHED:
                batch_count, _ = rd_varint(body)
                break
            elif pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"play entry disconnected: {reason}")
        side = 2 * view_distance + 1
        assert saw_event and saw_position and saw_batch_start
        assert saw_commands, "1.21.11 client got no command tree"
        assert batch_count == side * side == len(chunks), (batch_count, side, len(chunks))
        assert (0, 0) in chunks
        print(f"T1 1.21.11 login/configuration: {registries} registries, {tags} tag bytes")
        print(f"play entry: {len(chunks)} chunks ({side}x{side}), entity {entity_id}")

        # Movement: 774 Set Player Position is three doubles plus a flags byte
        # (0x01 on ground, 0x02 horizontal collision). Crossing into chunk (2, 0)
        # must re-center the cache and stream a full batch around the new chunk.
        send(sock, threshold, PLAY_MOVE_POS,
             struct.pack(">ddd", 40.5, 64.0, 7.5) + b"\x01")
        moved_chunks = set()
        moved_batch = -1
        while moved_batch < 0:
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_CHUNK:
                x, z = struct.unpack(">ii", body[:8])
                moved_chunks.add((x, z))
            elif pid == PLAY_BATCH_FINISHED:
                moved_batch, _ = rd_varint(body)
            elif pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"T1 movement disconnected: {reason}")
        assert moved_batch == side * side, (moved_batch, side)
        assert (2, 0) in moved_chunks, sorted(moved_chunks)[:5]
        assert len(moved_chunks) == side * side, (len(moved_chunks), side)
        print(f"movement: streamed {len(moved_chunks)} chunks around (2, 0)")

        # Chat: the 774 Chat Message starts with the 256-char string; the
        # signature fields follow and are not verified by this server, but the
        # test sends their real shape so a field-order slip cannot hide.
        message = "hello from T1"
        suffix = (struct.pack(">qq", 0, 0) + varint(0) + varint(0) +
                  b"\x00\x00\x00" + b"\x00")
        send(sock, threshold, PLAY_CHAT,
             varint(len(message)) + message.encode() + suffix)
        while True:
            pid, body = read_packet(sock, threshold)
            if pid == PLAY_SYSTEM_CHAT:
                text = chat_text(body)
                assert text == f"<{NAME}> {message}", text
                break
            if pid == PLAY_DISCONNECT:
                reason, _ = rd_nbt_component(body)
                raise AssertionError(f"T1 chat disconnected: {reason}")
        print(f"chat: {text!r}")

        # Commands: 774 Chat Command (0x06) carries the command string first;
        # the signature tail follows and is not verified by this server. The
        # translated dispatcher serves /time and /weather for now, with every
        # reply on the 774 system_chat channel.
        def read_command_reply(context):
            while True:
                pid, body = read_packet(sock, threshold)
                if pid == PLAY_SYSTEM_CHAT:
                    return chat_text(body)
                if pid == PLAY_DISCONNECT:
                    reason, _ = rd_nbt_component(body)
                    raise AssertionError(f"T1 {context} disconnected: {reason}")

        command = "time query daytime"
        send(sock, threshold, PLAY_CHAT_COMMAND,
             varint(len(command)) + command.encode() + suffix)
        text = read_command_reply("time command")
        assert text.startswith("daytime is "), text
        print(f"command: {text!r}")

        for command, expected in [
                ("weather clear 100", "Set the weather to clear"),
                ("gamemode creative",
                 "Only /time and /weather are available on a translated connection yet.")]:
            send(sock, threshold, PLAY_CHAT_COMMAND,
                 varint(len(command)) + command.encode() + suffix)
            text = read_command_reply(f"command {command!r}")
            assert text == expected, (command, text)
            print(f"command: {text!r}")

        sock.close()
        sock = None

        if not wire_client.skip_server_check(
                "the SIGTERM exit code (this run has no server process)"):
            proc.terminate()
            try:
                rc = proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                raise AssertionError("plusmc did not exit after SIGTERM")
            assert rc == 0, f"expected clean exit on SIGTERM, got {rc}"

        print("T1 1.21.11 login/configuration/play entry: OK")
        print("PASS")
        return 0
    except BaseException:
        if sock is not None:
            sock.close()
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        raise


if __name__ == "__main__":
    sys.exit(main() or 0)
