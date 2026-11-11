#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates src/protocol/packet_ids.generated.h (and a data/vanilla/26.3/
# packet_ids.tsv snapshot) from the decompiled Minecraft 26.3 (protocol 777)
# protocol templates, or from an official reports/packets.json.
#
# A packet's wire id is not a property anybody wrote down: it is the *index of
# its registration in the connection state's template*. Vanilla builds each
# state's protocol as a chain of `addPacket(TYPE, CODEC)` calls, so the first
# entry is 0x00, the second 0x01, and the `withBundlePacket` call that opens the
# play template takes id 0x00 (which is why the play tables are shifted by one
# against a list that forgets it). The templates live in:
#
#   net/minecraft/network/protocol/handshake/HandshakeProtocols.java
#   net/minecraft/network/protocol/status/StatusProtocols.java
#   net/minecraft/network/protocol/login/LoginProtocols.java
#   net/minecraft/network/protocol/configuration/ConfigurationProtocols.java
#   net/minecraft/network/protocol/game/GameProtocols.java      (play)
#
# and each defines SERVERBOUND_TEMPLATE and (except the handshake, which is
# client-request-only) CLIENTBOUND_TEMPLATE, so this one parse covers both
# directions for every state.
#
# The canonical name this tool emits is vanilla's own packet-type constant,
# lowercased with the direction prefix dropped:
#
#   GamePacketTypes.CLIENTBOUND_LEVEL_CHUNK_WITH_LIGHT -> level_chunk_with_light
#   LoginPacketTypes.CLIENTBOUND_LOGIN_FINISHED        -> login_finished
#
# That is the same vocabulary the wiki and minecraft-data use where it matters,
# and it is the name the clientbound id constants in protocol/ids.h are written
# against. The generated header is the data those constants are *derived* from
# (a constexpr name lookup), so a name that vanishes in a new release is a
# compile error rather than a silently wrong id.
#
# The same tables are the ground truth `tests/protocol_ids_test.cpp` compares
# the hand-written serverbound table in protocol/packet_table.cpp against: those
# rows were transcribed from minecraft-data 26.1 (775) and several are marked
# "re-verify"; the generated 776 template is that verification.
#
# Regenerate with: python3 tools/gen_protocol_table.py
# (requires the decompiled tree, the same input the other gen_* tools read:# build/_decomp/net/minecraft/network/protocol/ for 26.3).

import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DECOMP = ROOT / "build" / "_decomp" / "net" / "minecraft" / "network" / "protocol"

# (state, template file) in the order the connection runs through them.
STATES = [
    ("Handshake", "handshake/HandshakeProtocols.java"),
    ("Status", "status/StatusProtocols.java"),
    ("Login", "login/LoginProtocols.java"),
    ("Configuration", "configuration/ConfigurationProtocols.java"),
    ("Play", "game/GameProtocols.java"),
]

DIRECTIONS = ["SERVERBOUND", "CLIENTBOUND"]

# `addPacket(TYPE, CODEC)` and `withBundlePacket(TYPE, ...)` are the two calls
# that consume an id. The type reference is `<PacketTypes>.<CONSTANT>`.
CALL_RE = re.compile(r"(?:addPacket|withBundlePacket)\(\s*([A-Za-z0-9_]+)\.([A-Z0-9_]+)\s*[,)]")

# The data generator names packet types by their registry key, which is not
# always the Java constant the templates use. Keep the canonical names the rest
# of the engine already keys on where the two differ.
REPORT_RENAMES = {
    "bundle_delimiter": "bundle",
    "debug/block_value": "debug_block_value",
    "debug/chunk_value": "debug_chunk_value",
    "debug/entity_value": "debug_entity_value",
    "debug/event": "debug_event",
}


def find_template(text, name):
    """The `name = ProtocolInfoBuilder.<kind>Protocol(...)` call, paren-matched."""
    match = re.search(rf"\b{name}\s*=\s*ProtocolInfoBuilder\.", text)
    if match is None:
        return None
    start = text.index("(", match.end())
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
    raise SystemExit(f"unbalanced parentheses in {name}")


def canonical_name(constant):
    """`CLIENTBOUND_LEVEL_CHUNK_WITH_LIGHT` -> `level_chunk_with_light`."""
    for prefix in ("CLIENTBOUND_", "SERVERBOUND_", "CLIENT_", "SERVER_"):
        if constant.startswith(prefix):
            constant = constant[len(prefix) :]
            break
    return constant.lower()


def parse_packets_report(path):
    """Parse the vanilla data generator's reports/packets.json.

    This is the version-independent path used when no decompiled tree is
    available: the report lists each state and direction with the exact
    registration id, which is the same number the template index produces.
    """
    data = json.loads(path.read_text())
    parsed = []
    for state in ("Handshake", "Status", "Login", "Configuration", "Play"):
        rows = data.get(state.lower())
        if rows is None:
            raise SystemExit(f"{path}: no {state.lower()} state")
        tables = {}
        for direction in DIRECTIONS:
            entries = rows.get(direction.lower())
            if entries is None:
                continue
            ordered = sorted(entries.items(), key=lambda item: item[1]["protocol_id"])
            table = []
            for expected, (name, entry) in enumerate(ordered):
                pid = entry["protocol_id"]
                if pid != expected:
                    raise SystemExit(f"{state}/{direction}: id {pid} out of order")
                canonical = name.split(":", 1)[-1]
                canonical = REPORT_RENAMES.get(canonical, canonical)
                table.append((pid, canonical, name, "report"))
            if not table:
                raise SystemExit(f"{state}/{direction}: report table is empty")
            tables[direction] = table
        if "SERVERBOUND" not in tables:
            raise SystemExit(f"{state}: no serverbound table in report")
        parsed.append((state, tables))
    return parsed


def parse_state(state, path):
    """{direction: [(id, name, constant, type_ref), ...]} for one state."""
    if not path.exists():
        raise SystemExit(f"no decompiled protocol template at {path}")
    text = path.read_text()
    tables = {}
    for direction in DIRECTIONS:
        call = find_template(text, f"{direction}_TEMPLATE")
        if call is None:
            continue
        rows = []
        for type_ref, constant in CALL_RE.findall(call):
            rows.append((len(rows), canonical_name(constant), constant, type_ref))
        # The id is the registration index; state it as an invariant rather than
        # trusting the loop counter in a comment.
        for i, row in enumerate(rows):
            if row[0] != i:
                raise SystemExit(f"{state}/{direction}: id {row[0]} out of order")
        if not rows:
            raise SystemExit(f"{state}/{direction}: template parsed but empty")
        tables[direction] = rows
    if "SERVERBOUND" not in tables:
        raise SystemExit(f"{state}: no SERVERBOUND_TEMPLATE")
    return tables


def write_tsv(path, parsed):
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = ["state\tdirection\tid\tname\tconstant\ttype"]
    for state, tables in parsed:
        for direction in DIRECTIONS:
            for pid, name, constant, type_ref in tables.get(direction, []):
                lines.append(f"{state}\t{direction}\t0x{pid:02x}\t{name}\t{constant}\t{type_ref}")
    path.write_text("\n".join(lines) + "\n")


def table_name(state, direction, prefix=""):
    return f"k{prefix}{direction.title()}{state}Packets"


def write_header(path, parsed, protocol_version, source, prefix=""):
    counts = {state: {d: len(t.get(d, [])) for d in DIRECTIONS} for state, t in parsed}
    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// clang-format off")
    out.append("//")
    out.append("// GENERATED by tools/gen_protocol_table.py from " + source)
    out.append(f"// (protocol {protocol_version}) — do not hand-edit.")
    out.append("//")
    out.append("// A packet id is the *index of its registration* in the state's template")
    out.append("// (`ProtocolInfoBuilder.addPacket` chain in")
    out.append("// net/minecraft/network/protocol/<state>/<State>Protocols.java`), which is")
    out.append("// why the tables are dense 0x00..0xNN and sorted: `withBundlePacket`, the")
    out.append("// play bundle delimiter, takes the first slot of the clientbound play table.")
    out.append("//")
    out.append("// The name is vanilla's own packet-type constant lowercased with the")
    out.append("// direction prefix dropped (CLIENTBOUND_LEVEL_CHUNK_WITH_LIGHT ->")
    out.append("// level_chunk_with_light). `PacketEntry` is declared in protocol/packet_ids.h")
    out.append("// before this file is included; the lookup helpers live there too.")
    out.append("//")
    for state, tables in parsed:
        sb = counts[state].get("SERVERBOUND", 0)
        cb = counts[state].get("CLIENTBOUND", 0)
        out.append(f"// {state}: {sb} serverbound, {cb} clientbound")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace mc::protocol {")
    out.append("")
    out.append("// The protocol version these tables were generated from (`version.json`).")
    out.append(f"inline constexpr int k{prefix}GeneratedProtocolVersion = {protocol_version};")
    out.append("")
    for state, tables in parsed:
        for direction in DIRECTIONS:
            rows = tables.get(direction, [])
            if not rows:
                continue
            out.append(f"// {state} / {direction} ({len(rows)} packets).")
            out.append(f"inline constexpr PacketEntry {table_name(state, direction, prefix)}[] = {{")
            for pid, name, _constant, _type_ref in rows:
                out.append(f'    {{0x{pid:02x}, "{name}"}},')
            out.append("};")
            out.append(
                f"inline constexpr int {table_name(state, direction, prefix)}Count = {len(rows)};"
            )
            out.append("")
    out.append("} // namespace mc::protocol")
    out.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(out))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--decomp",
        type=pathlib.Path,
        default=DECOMP,
        help="the decompiled net/minecraft/network/protocol directory",
    )
    parser.add_argument(
        "--packets-json",
        type=pathlib.Path,
        default=None,
        help="use the vanilla data generator's reports/packets.json instead of decompiled sources",
    )
    parser.add_argument(
        "--protocol-version",
        type=int,
        default=777,
        help="protocol version written to the generated header (default: 777)",
    )
    parser.add_argument(
        "--prefix",
        default="",
        help="prefix for generated table/constant names (e.g. T1); default none",
    )
    parser.add_argument(
        "--header",
        type=pathlib.Path,
        default=pathlib.Path("src/protocol/packet_ids.generated.h"),
    )
    parser.add_argument(
        "--tsv",
        type=pathlib.Path,
        default=pathlib.Path("data/vanilla/26.3/packet_ids.tsv"),
    )
    args = parser.parse_args()

    if args.packets_json is not None:
        parsed = parse_packets_report(args.packets_json)
        source = f"the official Minecraft {args.protocol_version} `reports/packets.json`"
    else:
        parsed = []
        for state, rel in STATES:
            parsed.append((state, parse_state(state, args.decomp / rel)))
        source = "the decompiled Minecraft 26.3 (protocol 777) protocol templates"

    # A duplicate name inside one direction would make the constexpr
    # name lookup ambiguous; catch it here rather than at the first id.h
    # constant that resolves to the wrong row.
    for state, tables in parsed:
        for direction, rows in tables.items():
            names = [name for _id, name, _c, _t in rows]
            dupes = sorted({n for n in names if names.count(n) > 1})
            if dupes:
                raise SystemExit(f"{state}/{direction}: duplicate name(s): {dupes}")

    write_tsv(args.tsv, parsed)
    write_header(args.header, parsed, args.protocol_version, source, args.prefix)

    for state, tables in parsed:
        parts = [f"{d.lower()}={len(tables.get(d, []))}" for d in DIRECTIONS]
        print(f"{state}: " + ", ".join(parts))
    print(f"wrote {args.tsv} and {args.header}")


if __name__ == "__main__":
    main()
