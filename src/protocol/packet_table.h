// SPDX-License-Identifier: GPL-3.0-or-later
//
// Data-driven serverbound packet tables (T0 codec architecture, docs/PLAN.md
// §6.1). Numeric packet ids move between protocol versions; the *canonical
// name* of a packet is the stable contract. Protocol code therefore resolves
// each incoming id through these tables to a canonical name and dispatches on
// the name — so the version difference (which id means what) lives in data,
// and a future version target or a T1..T9 translation tier only swaps or
// augments the table, never the session logic.
//
// Names follow minecraft-data's protocol.json snake_case (the machine-readable
// schema we also use to cross-check 776 shapes); the wiki spellings they map
// to are noted where they differ.
//
// Coverage is "everything the Phase-1 server recognizes": handled packets plus
// the ones it must accept silently (movement, acks). The complete serverbound
// template (all 69 play packets, every other state) and the clientbound side
// are generated from vanilla's own registration order into
// protocol/packet_ids.generated.h; this file keeps only the recognized subset
// and the minecraft-data names dispatch keys on. A test cross-checks every row
// here against the generated 776 table (tests/protocol_ids_test.cpp).
#pragma once

#include <cstdint>

#include "protocol/packet_ids.h"

namespace mc::protocol {

// Looks up a serverbound packet by connection state + wire id. Returns the
// entry, or nullptr when the id is unknown in this state.
const PacketEntry* serverbound_packet(State state, uint32_t id);

} // namespace mc::protocol
