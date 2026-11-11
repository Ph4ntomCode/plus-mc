#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture clientbound packets from a live vanilla 26.2 server (offline mode).

Joins as a raw client (protocol 776), walks login + configuration, enters
play, and records every level_chunk_with_light (play 0x2d) body it receives
keyed by the chunk coordinates parsed from the body itself.

Output is a JSON dict "cx,cz" -> hex body bytes (matching tools/chunk_decode.py).
Pass -o to choose the output file (default stdout). Useful fixtures:
    - a flat seed-42 world for the milestone-A Anvil fixture, and
    - a void world, to byte-check our air-section encoder.

The config-phase client is deliberately tolerant: it responds to Select Known
Packs, acks Finish Configuration, ignores everything else it can (registry
data, tags, custom payloads, cookie requests get a cookie-response ack), and
times out after -t seconds (default 20) once in play.
"""

import argparse
import json
import socket
import struct
import sys
import time
import zlib

VERBOSE = False


def dbg(msg: str):
    if VERBOSE:
        print(f"[capture] {msg}", file=sys.stderr)

PROTO = 776
HOST = "127.0.0.1"


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


def make_frame(payload: bytes, threshold: int) -> bytes:
    """Frame a packet for the given compression threshold.

    threshold < 0: plain [length][payload]. Otherwise every frame is
    [length][dataLength][data]; under-threshold packets use dataLength = 0
    followed by the raw payload, larger ones a zlib stream.
    """
    if threshold < 0:
        return varint(len(payload)) + payload
    if len(payload) < threshold:
        body = varint(0) + payload
    else:
        body = varint(len(payload)) + zlib.compress(payload)
    return varint(len(body)) + body


def recvn(sock, n: int) -> bytes:
    chunks = []
    got = 0
    while got < n:
        c = sock.recv(n - got)
        if not c:
            raise ConnectionError(f"EOF (wanted {n}, got {got})")
        chunks.append(c)
        got += len(c)
    return b"".join(chunks)


def read_varint_sock(sock):
    raw = b""
    while True:
        c = recvn(sock, 1)
        raw += c
        if not (c[0] & 0x80):
            break
    v, _ = rd_varint(raw)
    return v, len(raw)


def read_packet(sock, threshold: int):
    ln, _ = read_varint_sock(sock)
    if threshold < 0:
        data = recvn(sock, ln)
    else:
        # compressed frame: [data length varint][zlib stream] fills ln bytes
        rest = recvn(sock, ln)
        dlen, i = rd_varint(rest)
        if dlen > 0:
            data = zlib.decompress(rest[i:])
        else:
            data = rest[i:]  # below threshold: payload sent uncompressed
    pid, off = rd_varint(data)
    return pid, data[off:]



class Client:
    def __init__(self, name: str, port: int, timeout: float):
        self.name = name
        self.sock = socket.create_connection((HOST, port), timeout=10)
        self.sock.settimeout(timeout)
        self.threshold = -1

    def send_pkt(self, pkt_id: int, body: bytes = b""):
        self.sock.sendall(make_frame(varint(pkt_id) + body, self.threshold))

    def read_pkt(self):
        return read_packet(self.sock, self.threshold)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def capture(port: int, name: str, seconds: float):
    """Joins and returns {cx,cz: hex body} for every 0x2d seen in play."""
    chunks = {}
    c = Client(name, port, 10)
    try:
        hs = (varint(0) + varint(PROTO) + varint(len(HOST)) + HOST.encode() +
              struct.pack(">H", 0) + varint(2))
        c.sock.sendall(make_frame(hs, -1))
        c.send_pkt(0x00, varint(len(name)) + name.encode() + b"\x00" * 16)
        dbg("login start sent")
        while True:  # login
            pid, body = c.read_pkt()
            dbg(f"login packet 0x{pid:02x}")
            if pid == 0x03:
                th, _ = rd_varint(body)
                c.threshold = th
                dbg(f"compression threshold {th}")
            elif pid == 0x02:  # Login Success
                c.send_pkt(0x03)  # login acknowledged
                dbg("login acknowledged sent")
                break
            else:
                raise AssertionError(f"unexpected login packet 0x{pid:02x}")
        while True:  # configuration
            pid, body = c.read_pkt()
            dbg(f"config packet 0x{pid:02x} ({len(body)}b)")
            if pid == 0x0E:  # Select Known Packs (clientbound) -> echo back
                npacks, i = rd_varint(body)
                packs = []
                for _ in range(npacks):
                    ns, i = rd_string(body, i)
                    pk, i = rd_string(body, i)
                    ver, i = rd_string(body, i)
                    packs.append((ns, pk, ver))
                out = varint(len(packs))
                for ns, pk, ver in packs:
                    out += (varint(len(ns)) + ns.encode() +
                            varint(len(pk)) + pk.encode() +
                            varint(len(ver)) + ver.encode())
                c.send_pkt(0x07, out)
            elif pid == 0x06:  # Cookie Request -> Cookie Response
                key, i = rd_string(body, 0)
                c.send_pkt(0x08, varint(len(key)) + key.encode())
            elif pid == 0x03:  # Finish Configuration -> ack
                c.send_pkt(0x03)
                dbg("finish configuration acked")
                break
            # registry data (0x07), tags (0x0D), brand (0x01), feature flags
            # (0x0C), custom payload (0x02), ping (0x04) etc: ignore
        dbg("entering play")
        deadline = time.time() + seconds
        while time.time() < deadline:
            pid, body = c.read_pkt()
            if pid == 0x2d:  # level chunk with light
                x, z = struct.unpack(">ii", body[:8])
                chunks[f"{x},{z}"] = body.hex()
    except (ConnectionError, socket.timeout):
        dbg("connection ended or timed out")
    finally:
        c.close()
    return chunks


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("port", type=int)
    ap.add_argument("-o", "--out", default=None)
    ap.add_argument("-n", "--name", default="CaptureBot")
    ap.add_argument("-t", "--time", type=float, default=20.0)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    global VERBOSE
    VERBOSE = args.verbose
    chunks = capture(args.port, args.name, args.time)
    text = json.dumps(chunks, indent=1, sort_keys=True)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text + "\n")
        print(f"wrote {len(chunks)} chunks to {args.out}", file=sys.stderr)
    else:
        print(text)


if __name__ == "__main__":
    main()
