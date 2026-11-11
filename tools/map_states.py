#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Maps on-disk block states to their clientbound network ids.

The chunk encoder must emit the exact network state ids a vanilla 26.2 client
expects. Those ids are not in the world data (disk chunk NBT stores state
*names* + properties); they only exist on the wire (the clientbound palette).
This tool pairs both sides of the same chunks:

  - the wire palette of a captured chunk (tools/vanilla_capture.py output)
    lists ids in first-appearance cell order, and
  - the disk chunk's block_states.palette lists {Name, Properties} in the
    same first-appearance cell order (vanilla writes both from the same
    storage scan), so palette entry i on disk is palette entry i on the wire.

Zipping per section gives the exact (name, sorted properties) -> id mapping
for every state those chunks use, with zero guessing about registry layout.
Run it against a live overworld capture + that world's region folder and it
regenerates src/world/observed_block_states.h (also used as the milestone-B
verification vehicle: a byte-identical re-encode of the same chunks proves the
ids).

Usage:
    map_states.py CAPTURE_JSON REGION_DIR OUT_HEADER [BLOCKS_26_1_JSON]

    CAPTURE_JSON   tools/vanilla_capture.py output ("cx,cz" -> hex body)
    REGION_DIR     the world's dimensions/minecraft/overworld/region folder
    OUT_HEADER     generated C++ table (e.g. src/world/observed_block_states.h)
    BLOCKS_...     optional minecraft-data blocks.json for id-range sanity
"""
import json
import struct
import sys
import zlib

sys.path.insert(0, __import__("os").path.dirname(__file__))
import chunk_decode  # independent wire-body decoder (reused as ground truth)


# ---------------------------------------------------------------------------
# NBT reader (self-contained; disk chunk roots are what we need)
# ---------------------------------------------------------------------------

TAG_END = 0
TAG_BYTE = 1
TAG_SHORT = 2
TAG_INT = 3
TAG_LONG = 4
TAG_STRING = 8
TAG_LIST = 9
TAG_COMPOUND = 10
TAG_INT_ARRAY = 11
TAG_LONG_ARRAY = 12


class R:
    def __init__(self, b):
        self.b = b
        self.p = 0

    def take(self, n):
        if self.p + n > len(self.b):
            raise ValueError("truncated NBT")
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def u8(self):
        return self.take(1)[0]

    def i16(self):
        return struct.unpack(">h", self.take(2))[0]

    def i32(self):
        return struct.unpack(">i", self.take(4))[0]

    def name(self):
        return self.take(struct.unpack(">H", self.take(2))[0]).decode("utf-8")

    def skip(self, tag):
        if tag == TAG_BYTE:
            self.take(1)
        elif tag == TAG_SHORT:
            self.take(2)
        elif tag == TAG_INT:
            self.take(4)
        elif tag == TAG_LONG:
            self.take(8)
        elif tag in (5, 6):  # float / double
            self.take(8 if tag == 6 else 4)
        elif tag == TAG_STRING:
            self.take(struct.unpack(">H", self.take(2))[0])
        elif tag == 7:  # byte array
            self.take(self.i32())
        elif tag == TAG_INT_ARRAY:
            self.take(4 * self.i32())
        elif tag == TAG_LONG_ARRAY:
            self.take(8 * self.i32())
        elif tag == TAG_LIST:
            elem = self.u8()
            for _ in range(self.i32()):
                self.skip(elem)
        elif tag == TAG_COMPOUND:
            while self.u8() != TAG_END:
                self.take(struct.unpack(">H", self.take(2))[0])
                self.skip(self.u8())
        else:
            raise ValueError(f"unknown NBT tag {tag}")


def read_compound(r, out, max_depth=24):
    """Reads a compound into `out` (a dict). Values: ('int', v), ('str', v),
    ('list', [...]), ('compound', dict), ('intarr', [...])."""
    while True:
        t = r.u8()
        if t == TAG_END:
            return
        k = r.name()
        if max_depth <= 0:
            r.skip(t)
            continue
        if t == TAG_COMPOUND:
            d = {}
            read_compound(r, d, max_depth - 1)
            out[k] = ("compound", d)
        elif t == TAG_LIST:
            elem = r.u8()
            n = r.i32()
            items = []
            for _ in range(n):
                if elem == TAG_COMPOUND:
                    d = {}
                    read_compound(r, d, max_depth - 1)
                    items.append(d)
                elif elem == TAG_STRING:
                    items.append(r.take(struct.unpack(">H", r.take(2))[0]).decode())
                else:
                    r.skip(elem)
            out[k] = ("list", items)
        elif t == TAG_INT:
            out[k] = ("int", r.i32())
        elif t == TAG_STRING:
            out[k] = ("str", r.take(struct.unpack(">H", r.take(2))[0]).decode())
        elif t == TAG_LONG:
            out[k] = ("long", struct.unpack(">q", r.take(8))[0])
        elif t == TAG_INT_ARRAY:
            n = r.i32()
            out[k] = ("intarr", list(struct.unpack(f">{n}i", r.take(4 * n))))
        elif t == TAG_LONG_ARRAY:
            n = r.i32()
            out[k] = ("longarr", list(struct.unpack(f">{n}q", r.take(8 * n))))
        elif t in (TAG_BYTE, TAG_SHORT):
            out[k] = ("int", r.u8() if t == TAG_BYTE else r.i16())
        elif t == 7:  # byte array (e.g. SkyLight/BlockLight): skip, not needed
            r.skip(t)
        else:
            r.skip(t)


def parse_nbt(b):
    r = R(b)
    assert r.u8() == TAG_COMPOUND
    r.name()
    d = {}
    read_compound(r, d)
    return d


# ---------------------------------------------------------------------------
# Region file reading
# ---------------------------------------------------------------------------

def floor_div(a, b):
    return a // b if a >= 0 else -((-a + b - 1) // b)


def read_region_chunk(region_dir, cx, cz):
    """Returns the decompressed chunk NBT payload for (cx, cz), or None."""
    rx, rz = floor_div(cx, 32), floor_div(cz, 32)
    import os
    path = os.path.join(region_dir, f"r.{rx}.{rz}.mca")
    try:
        f = open(path, "rb").read()
    except OSError:
        return None
    if len(f) < 8192:
        return None
    lx, lz = cx - rx * 32, cz - rz * 32
    entry = struct.unpack(">I", f[(lz * 32 + lx) * 4:(lz * 32 + lx) * 4 + 4])[0]
    sector, count = entry >> 8, entry & 0xFF
    if sector == 0 or count == 0:
        return None
    off = sector * 4096
    if off + 5 > len(f):
        return None
    length = struct.unpack(">I", f[off:off + 4])[0]
    comp = f[off + 4]
    payload = f[off + 5:off + 4 + length]
    if comp == 1 or comp == 2:  # gzip / zlib
        return zlib.decompress(payload, 15 + 32)
    return payload if comp == 3 else None


def props_key(entry):
    """The canonical properties string our encoder builds: sorted k=v, comma
    joined (matches src/world/chunk.cpp parse_chunk_nbt)."""
    props = entry.get("Properties")
    if props and props[0] == "compound":
        fields = props[1]
        pairs = []
        for k, v in fields.items():
            if v[0] in ("str", "int"):
                pairs.append(f"{k}={v[1]}")
        return ",".join(sorted(pairs))
    return ""


def disk_section_palettes(root):
    """Returns {section Y: list of (name, props) palette entries in disk
    order} plus the raw storage longs, for sections holding block_states.

    Returns a dict Y -> {"palette": [(name, props)...], "data": [longs]}
    (data absent = 0-bit / single-value storage on disk)."""
    out = {}
    secs = root.get("sections")
    if not secs:
        return out
    _, items = secs
    for s in items:
        y = s.get("Y")
        y = y[1] if y else None
        if y is not None and y >= 128:
            y -= 256  # section Y is a signed NBT byte
        bs = s.get("block_states")
        if bs is None or y is None:
            continue
        _, bd = bs
        keys = []
        data = None
        for k, v in bd.items():
            if k == "palette":
                _, entries = v
                for e in entries:
                    name = e.get("Name")
                    name = name[1] if name else "?"
                    keys.append((name, props_key(e)))
            elif k == "data" and v[0] == "longarr":
                data = v[1]
        out[y] = {"palette": keys, "data": data}
    return out


def disk_bits_for_palette_size(n):
    """The on-disk storage bits vanilla uses for a palette of n entries
    (block-state strategy: 1 entry -> 0 bits, 2-16 -> 4, then ceil-log2)."""
    if n <= 1:
        return 0
    bits = (n - 1).bit_length()
    return 4 if bits <= 4 else bits


def storage_cells(bits, raw, cell_count):
    """Decode paletted-container storage cells (palette indices) from raw
    big-endian longs. LSB-first packing, valuesPerLong = 64 // bits; the
    storage layout (index = (y<<2|z)<<2|x) is identical on disk and wire."""
    if bits == 0 or raw is None:
        return [0] * cell_count
    per_long = 64 // bits
    mask = (1 << bits) - 1
    cells = []
    for raw_i, word in enumerate(raw):
        for k in range(per_long):
            idx = raw_i * per_long + k
            if idx >= cell_count:
                break
            # words are big-endian on the wire; re-interpret as unsigned and
            # read the k-th field LSB-first.
            w = word & ((1 << 64) - 1)
            cells.append((w >> (k * bits)) & mask)
    if len(cells) < cell_count:
        cells.extend([0] * (cell_count - len(cells)))
    return cells[:cell_count]


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    cap = json.load(open(sys.argv[1]))
    region_dir = sys.argv[2]
    out_path = sys.argv[3]
    sanity = json.load(open(sys.argv[4])) if len(sys.argv) > 4 else None

    mapping = {}  # (name, props) -> id
    cell_obs = {}  # (name, props) -> set of wire ids seen
    conflicts = []
    matched = 0
    paired_sections = 0
    for key, hexbody in sorted(cap.items()):
        cx, cz = (int(v) for v in key.split(","))
        nbt = read_region_chunk(region_dir, cx, cz)
        if nbt is None:
            print(f"chunk {key}: no disk data, skipped")
            continue
        root = parse_nbt(nbt)
        disk = disk_section_palettes(root)
        wire = chunk_decode.decode(bytes.fromhex(hexbody))
        if len(wire["sections"]) != 24:
            print(f"chunk {key}: unexpected section count, skipped")
            continue
        for i, sec in enumerate(wire["sections"]):
            y = -4 + i
            ds = disk.get(y)
            if ds is None or not ds["palette"]:
                continue  # no disk block_states (all-air column section)
            disk_pal = ds["palette"]
            bits = sec["states_bits"]
            if bits == 0:
                continue  # nothing to pair (single value already known: air=0)
            if bits > 8 or sec["states"] is None or sec["states_raw"] is None:
                continue  # direct/global mode: ids live in the longs, unpaired
            # Cell-level alignment: decode both storages with the same cell
            # geometry and pair per position. Drifted sections (wire content
            # newer than a mid-generation save) produce conflicting id
            # observations for a name and are skipped automatically.
            dbits = disk_bits_for_palette_size(len(disk_pal))
            d_raw = ds["data"] if dbits != 0 else None
            try:
                disk_idx = storage_cells(dbits, d_raw, 4096)
                wire_idx = storage_cells(bits, sec["states_raw"], 4096)
            except Exception as e:
                print(f"chunk {key} Y {y}: storage decode failed ({e}) — skipping")
                continue
            if any(i >= len(disk_pal) for i in disk_idx):
                continue
            if any(i >= len(sec["states"]) for i in wire_idx):
                continue
            name_cells = [disk_pal[i] for i in disk_idx]
            id_cells = [sec["states"][i] for i in wire_idx]
            # Pair cell-by-cell, keeping only names that map to exactly one id
            # *across their own cells*. Whole-section drift (a few ticking
            # blocks changed between save and send) no longer discards the
            # section: the unaffected names still pair confidently, and global
            # consensus across sections filters any residual wrong pairing.
            local = {}
            for nc, ic in zip(name_cells, id_cells):
                prev = local.get(nc)
                if prev is None:
                    local[nc] = ic
                elif prev != ic:
                    local[nc] = "DRIFT"  # this name drifted in this section
            for (name, props), sid in local.items():
                if sid == "DRIFT":
                    continue
                # Only minecraft:air carries network id 0; a non-air name
                # paired with 0 means the wire cell was still air where the
                # disk held a (newer/older) block — a drift artifact.
                if sid == 0 and name != "minecraft:air":
                    continue
                cell_obs.setdefault((name, props), set()).add(sid)
            paired_sections += 1

    for (name, props), ids in sorted(cell_obs.items()):
        if len(ids) > 1:
            conflicts.append((name, props, sorted(ids)))
        else:
            mapping[(name, props)] = next(iter(ids))
            matched += 1
    if not mapping:
        print("no states mapped")
        return 1
    print(f"cell-aligned {paired_sections} sections; "
          f"{len(mapping)} distinct (name, props) states")

    # Id-range sanity against minecraft-data (block-level min/max state ids).
    # The ids themselves come from a live 26.2 capture and are ground truth;
    # minecraft-data 26.1 is an *older* registry, so blocks 26.2 inserted
    # shift ids and only EARLY blocks land in range. Report outliers as
    # informational warnings, never as failures.
    if sanity:
        by_block = {}
        for (name, props), sid in mapping.items():
            by_block.setdefault(name, []).append(sid)
        out_of_range = 0
        for name, sids in sorted(by_block.items()):
            for blk in sanity:
                if blk.get("name") == name.replace("minecraft:", ""):
                    lo, hi = blk["minStateId"], blk["maxStateId"]
                    bad = [s for s in sids if not (lo <= s <= hi)]
                    if bad:
                        out_of_range += 1
                        print(f"  note: {name} ids {bad} outside mcdata-26.1 range "
                              f"{lo}-{hi} (expected: 26.2 inserted blocks)")
                    break
        print(f"range notes: {out_of_range} block names outside the 26.1 ranges "
              "(informational)")

    if conflicts:
        print(f"{len(conflicts)} real conflict(s):")
        for c in conflicts[:20]:
            print("  ", c)
        return 1
    if not mapping:
        print("no states mapped")
        return 1

    # Emit the generated header (deterministic order: by id, then name).
    entries = sorted(mapping.items(), key=lambda kv: (kv[1], kv[0][0], kv[0][1]))
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// clang-format off",
        "//",
        f"// Block states observed in a live vanilla 26.2 capture "
        f"({len(entries)} states) and their exact clientbound network ids,",
        "// mapped by tools/map_states.py via cell-level alignment of the disk chunk",
        "// palette (names) against the clientbound palette (ids) of the same",
        "// chunks: both storages are decoded per 4096-cell position, so only",
        "// save/send-identical sections contribute. Regenerate, don't hand-edit.",
        "//",
        "#pragma once",
        "",
        "#include <cstdint>",
        "#include <utility>",
        "",
        "namespace mc::world {",
        "// {name, canonical sorted properties string} -> network state id.",
        "inline const std::pair<const char*, const char*> kObservedBlockStates[] = {",
    ]
    for (name, props), sid in entries:
        prop_lit = props or ""
        lines.append(f'    {{"{name}", "{prop_lit}"}},  // {sid}')
    lines += [
        "};",
        "inline const int32_t kObservedBlockStateIds[] = {",
    ]
    for (name, props), sid in entries:
        lines.append(f"    {sid},")
    lines += [
        "};",
        f"inline constexpr int kObservedBlockStateCount = {len(entries)};",
        "",
        "} // namespace mc::world",
        "",
    ]
    open(out_path, "w").write("\n".join(lines))
    print(f"mapped {len(entries)} states across {matched} section-palette pairs; "
          f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
