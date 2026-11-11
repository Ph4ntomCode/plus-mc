#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decoder for a 26.x clientbound level_chunk_with_light (play 0x2d) body.

Parses everything after the packet id:
    x i32, z i32,
    heightmaps: varint count of { type: varint, longs: varint-counted i64[] },
    chunkData: varint length, then the raw section bytes (decoded here),
    blockEntities: varint count (compounds skipped, counted by length),
    four light masks (26.3: varint byte length + bytes, `BitSet.toByteArray`;
    26.1/26.2: varint-counted i64[]; `mask_encoding` selects),
    sky-light arrays and block-light arrays (varint count of varint-length u8[]).

Sections inside chunkData (26.2, ground truth from vanilla decompile):
    u16 non-air block count, u16 fluid count,
    block-states paletted container, biomes paletted container.
Container: u8 bits; bits == 0 -> singleton (one varint value follows);
otherwise varint palette count + varint entries, then the packed longs. The
long count is *not* on the wire: it is implied by the bit width
(ceil(4096 * bits / 64) longs). Pass --long-count to try the alternative
layout (explicit varint long count) for calibration.

Multi-byte integers (i32 x/z, the i64 arrays, container longs) are
big-endian, matching the wire; mask long arrays read as big-endian so a sky
mask 0x06 means slots {1,2} (sections Y = slot - 5). Byte-array masks are
Java `BitSet.toByteArray()` form: little-endian bit order, minimal length.

Usage:
    chunk_decode.py CAPTURE.JSON [CX,CZ] [--long-count]
Reads a JSON dict of "cx,cz" -> hex body (as captured by tools/capture.py).
"""

import json
import struct
import sys


class End(Exception):
    pass


class R:
    def __init__(self, b):
        self.b = b
        self.p = 0

    def need(self, n):
        if self.p + n > len(self.b):
            raise End(f"underflow at {self.p}: want {n}, have {len(self.b) - self.p}")
        out = self.b[self.p:self.p + n]
        self.p += n
        return out

    def u8(self):
        return self.need(1)[0]

    # All wire integers are big-endian.
    def u16(self):
        return struct.unpack(">H", self.need(2))[0]

    def i32(self):
        return struct.unpack(">i", self.need(4))[0]

    def varint(self):
        v = 0
        for i in range(5):
            c = self.u8()
            v |= (c & 0x7F) << (7 * i)
            if not c & 0x80:
                return v
        raise End("varint too long")

    def i64s(self):
        n = self.varint()
        return [struct.unpack(">q", self.need(8))[0] for _ in range(n)]

    def bytes_varint(self):
        n = self.varint()
        return self.need(n)

    def remaining(self):
        return len(self.b) - self.p


def container(r, name, long_count, cells=4096, section_budget=None):
    """Parse a paletted container; returns (bits, entries, n_longs_consumed).

    Codec (26.2 ground truth from the vanilla decompile, PalettedContainer):
      - u8 bits
      - bits == 0: single-value palette, one varint follows
      - bits in [1, max_listed]: varint palette size + varint entries
      - bits above max_listed: *direct/global* mode — no palette at all, the
        longs hold global ids (GlobalPalette.read is a no-op)
      - then the packed longs; count implied by the *storage geometry*:
        SimpleBitStorage packs floor(64 / bits) values per long, so
        n_longs = ceil(cells / floor(64 / bits)).  That is NOT
        ceil(cells * bits / 64) when 64 % bits != 0 (bits=5 -> 342 longs for
        4096 cells, not 320) — a real-world chunk made this bug obvious.
    max_listed is 8 for block states (4096 cells) and 3 for biomes (64 cells).
    The 64-cell sizing for biomes matters: a multi-biome section exposes it.
    """
    bits = r.u8()
    if bits == 0:
        val = r.varint()
        return bits, [val], 0, []
    per_long = 64 // bits
    n_longs = (cells + per_long - 1) // per_long
    if long_count:
        n_longs = r.varint()
    max_listed = 8 if cells == 4096 else 3
    if bits <= max_listed:
        size = r.varint()
        entries = [r.varint() for _ in range(size)]
    else:
        # Direct: entries are raw global ids packed in the longs.
        entries = []
    raw = struct.unpack(f">{n_longs}q", r.need(n_longs * 8))
    return bits, entries, n_longs, list(raw)


def decode(body, long_count=False, mask_encoding="bytes"):
    r = R(body)
    out = {"x": r.i32(), "z": r.i32(), "heightmaps": [], "sections": [],
           "block_entities": [], "masks": {}, "sky": [], "block": []}

    n_hm = r.varint()
    for _ in range(n_hm):
        t = r.varint()
        longs = r.i64s()
        out["heightmaps"].append((t, longs))

    data_len = r.varint()
    sec_r = R(r.need(data_len))
    while sec_r.remaining() > 0:
        n_air = sec_r.u16()
        n_fluid = sec_r.u16()
        sb, sentries, _, sraw = container(sec_r, "states", long_count, cells=4096)
        bb, bentries, _, braw = container(sec_r, "biomes", long_count, cells=64)
        out["sections"].append({
            "non_air": n_air, "fluid": n_fluid,
            "states_bits": sb, "states": sentries, "states_raw": sraw,
            "biomes_bits": bb, "biomes": bentries, "biomes_raw": braw,
        })

    n_be = r.varint()
    for _ in range(n_be):
        # Block entity: a full NBT compound; we don't parse NBT here, so skip
        # by scanning: this is only reachable for non-empty chunks.
        raise End("block entities present; NBT skipping not implemented")

    for name in ("sky", "block", "empty_sky", "empty_block"):
        if mask_encoding == "bytes":
            raw = r.bytes_varint()
            bits = int.from_bytes(raw, "little")
            words = []
            while bits:
                words.append(bits & ((1 << 64) - 1))
                bits >>= 64
            out["masks"][name] = words
        else:
            out["masks"][name] = r.i64s()
    for name in ("sky", "block"):
        n = r.varint()
        arrs = []
        for _ in range(n):
            arrs.append(list(r.bytes_varint()))
        out[name] = arrs
    if r.remaining() != 0:
        raise End(f"{r.remaining()} trailing bytes after full parse")
    return out


def main():
    path = sys.argv[1]
    key = sys.argv[2] if len(sys.argv) > 2 else "0,0"
    long_count = "--long-count" in sys.argv
    d = json.load(open(path))
    b = bytes.fromhex(d[key])
    try:
        res = decode(b, long_count)
    except End as e:
        print(f"FAIL ({e}) with long_count={long_count}")
        sys.exit(1)
    print(f"chunk {key}: x={res['x']} z={res['z']} len={len(b)}")
    print("heightmaps:", [(t, len(l)) for t, l in res["heightmaps"]])
    print(f"{len(res['sections'])} sections")
    for i, s in enumerate(res["sections"]):
        y = -4 + i
        tag = []
        if s["non_air"]:
            tag.append(f"non_air={s['non_air']}")
        if s["fluid"]:
            tag.append(f"fluid={s['fluid']}")
        tag.append(f"sb={s['states_bits']}")
        if s["states_bits"] == 0:
            tag.append(f"single={s['states'][0]}")
        else:
            tag.append(f"palette({len(s['states'])})")
            tag.append(f"first={s['states'][:4]}")
        tag.append(f"bb={s['biomes_bits']}")
        if s["biomes_bits"] == 0:
            tag.append(f"bsingle={s['biomes'][0]}")
        else:
            tag.append(f"bpalette({len(s['biomes'])})")
        print(f"  Y {y:3d}: " + ", ".join(tag))
    print("masks:", {k: v for k, v in res["masks"].items()})
    print(f"sky arrays: {len(res['sky'])}, block arrays: {len(res['block'])}")


if __name__ == "__main__":
    main()
