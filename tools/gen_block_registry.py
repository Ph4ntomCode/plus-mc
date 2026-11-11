#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the complete 26.3 block-state -> network id table.

The chunk encoder must emit the exact clientbound network state ids a vanilla
26.3 client expects. The authoritative source for those ids is the vanilla
server jar itself: its data generator (BlockListReport) writes reports/blocks.json
listing every block state with its registry (== network) id, in registration
order. Generate the report from the real jar with:

    java -DbundlerMainClass=net.minecraft.data.Main -jar server-boot.jar \\
         --reports --output /tmp/mc_reports

    (server-boot.jar = the bundled 26.2 server; the report lands in
     /tmp/mc_reports/reports/blocks.json)

This tool turns that JSON into src/world/block_states.h: an array of
(name, canonical sorted properties string, id) in ascending id order,
covering all 35723 states (ids 0..35722, contiguous).

It also *validates* the generated table against the byte-verified ground
truth the project already owns:

  - src/world/observed_block_states.h (146 states cell-aligned against a
    live vanilla 26.2 overworld capture), and
  - the milestone-A hand table (air/dirt/grass_block/bedrock, byte-verified
    against the committed flat-world fixture capture).

Every (name, props) -> id pair in those must agree with the report, or the
script exits non-zero: a mismatch would mean either this report is from the
wrong version or one of the observed pairs was a drift artifact.

Usage:
    gen_block_registry.py BLOCKS_JSON OBSERVED_HEADER OUT_HEADER
"""

import json
import re
import sys


def canonical_props(props):
    """Sorted k=v, comma joined — the key form the encoder builds (chunk.cpp
    parse_chunk_nbt sorts property pairs by key)."""
    if not props:
        return ""
    return ",".join(f"{k}={props[k]}" for k in sorted(props))


def load_observed(header_path):
    """Parses the generated observed header back into {(name, props): id}."""
    text = open(header_path).read()
    pairs = dict(re.findall(r'\{"([^"]+)", "([^"]*)"\}', text))
    # ids array appears after the states array; extract in document order.
    state_block = text.split("kObservedBlockStates[]", 1)[1].split("};", 1)[0]
    name_props = re.findall(r'\{"([^"]+)", "([^"]*)"\}', state_block)
    id_block = text.split("kObservedBlockStateIds[]", 1)[1].split("};", 1)[0]
    ids = [int(x) for x in re.findall(r"-?\d+", id_block)]
    assert len(name_props) == len(ids), (len(name_props), len(ids))
    return {(n, p): sid for (n, p), sid in zip(name_props, ids)}


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    blocks_json, observed_header, out_path = sys.argv[1:]

    report = json.load(open(blocks_json))
    full = {}  # (name, props) -> id
    id_seen = {}
    n_blocks = len(report)
    n_states = 0
    for name, blk in sorted(report.items()):
        for st in blk["states"]:
            sid = int(st["id"])
            key = (name, canonical_props(st.get("properties")))
            if key in full and full[key] != sid:
                print(f"duplicate key {key} with conflicting ids")
                return 1
            if sid in id_seen and id_seen[sid] != key:
                print(f"duplicate id {sid}: {id_seen[sid]} vs {key}")
                return 1
            full[key] = sid
            id_seen[sid] = key
            n_states += 1
    ids = sorted(id_seen)
    if ids != list(range(len(ids))):
        print("ids not contiguous:", ids[0], ids[-1], len(ids))
        return 1

    # Validate every byte-verified observed + milestone-A pair against the
    # report. kObservedBlockStateCount covers observed only; fold the 4
    # milestone-A states in manually (they were merged at runtime).
    observed = load_observed(observed_header)
    observed[("minecraft:air", "")] = 0
    observed[("minecraft:bedrock", "")] = 88
    observed[("minecraft:dirt", "")] = 10
    observed[("minecraft:grass_block", "snowy=false")] = 9
    mismatches = []
    for (name, props), sid in sorted(observed.items()):
        got = full.get((name, props))
        if got != sid:
            mismatches.append(((name, props), sid, got))
    if mismatches:
        print(f"{len(mismatches)} observed-id mismatch(es) vs the report:")
        for m in mismatches[:20]:
            print("  ", m)
        return 1
    print(f"validated {len(observed)} byte-verified observed pairs against the "
          f"report ({n_blocks} blocks, {n_states} states, ids 0..{max(ids)})")

    # Emit src/world/block_states.h in ascending id order. Entries carry the
    # canonical properties string used as the lookup key; the array's index
    # order also gives id -> (name, props) for free.
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// clang-format off",
        "//",
        f"// Complete 26.3 block-state table: all {n_states} states of the",
        "// vanilla block registry with their exact clientbound network ids",
        "// (protocol 777), in ascending id order. Generated by",
        "// tools/gen_block_registry.py from the 26.3 server jar's own data-generator report",
        "// (reports/blocks.json, BlockListReport); the ids were validated",
        "// against the byte-verified observed/milestone-A pairs of",
        f"// {observed_header}. Regenerate, don't hand-edit.",
        "//",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mc::world {",
        "struct GeneratedBlockState {",
        "  const char* name;   // \"minecraft:stone\"",
        "  const char* props;  // canonical sorted k=v,k=v string (\"\" if none)",
        "  int32_t id;         // clientbound network state id",
        "};",
        f"inline constexpr GeneratedBlockState kBlockStates[] = {{",
    ]
    for (name, props), sid in sorted(full.items(), key=lambda kv: kv[1]):
        lines.append(f'    {{"{name}", "{props}", {sid}}},')
    lines += [
        "};",
        f"inline constexpr int kBlockStateCount = {n_states};",
        "",
        "} // namespace mc::world",
        "",
    ]
    open(out_path, "w").write("\n".join(lines))
    print(f"wrote {out_path} ({n_states} states, {len(lines)} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
