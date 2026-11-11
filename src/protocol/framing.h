// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minecraft framing: each packet is [VarInt length][payload]; the payload is
// the packet-id VarInt followed by the packet body.
//
// Compression (protocol 776): once Set Compression announces a threshold T >= 0,
// every frame becomes [VarInt length][VarInt dataLength][data] where
//   - dataLength == 0: data is the raw packet (smaller than T), or
//   - dataLength != 0: data is the packet zlib-compressed (RFC 1950), and
//     dataLength is its uncompressed size.
// Threshold < 0 means compression is off (plain framing). Clients compress
// their own outbound frames with the same threshold after Set Compression, so
// both directions use one per-connection threshold.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "net/stream.h"

namespace mc::protocol {

// Hard cap on a single uncompressed frame. Vanilla lets clients request up to
// 2^31-1 but a server should bound it; 8 MiB is generous for status/login and
// configuration-phase registry data. Revisit when large chunks stream in.
constexpr size_t kMaxFrameSize = 8 * 1024 * 1024;

// Reads one frame with `threshold` compression (see header). Returns nullopt on
// EOF / timeout / oversized / malformed / decompression failure.
std::optional<std::vector<uint8_t>> read_frame(mc::net::Stream& stream, int threshold);

// Writes one frame with `threshold` compression. Returns false on write error.
bool write_frame(mc::net::Stream& stream, int threshold, const std::vector<uint8_t>& payload);

// The exact bytes `write_frame` would put on the wire for `payload` at
// `threshold`, without a stream. Compression is deterministic (zlib's default
// level, no dictionary), so two callers that encode the same payload at the
// same threshold produce identical bytes — which is what lets a server
// compress a body once and hand the frame to many connections
// (docs/WRITE_PATH.md). Encryption is not part of this: it is a property of a
// connection, and happens in the stream as it writes these bytes.
std::vector<uint8_t> encode_frame(const std::vector<uint8_t>& payload, int threshold);

// Appends the same bytes `encode_frame` returns to `*out`. One buffer, one
// append: a fan-out that puts a tick's packets in one write per connection
// needs the bytes in sequence, and a fresh `std::vector` per frame is three
// allocations the caller does not keep. Returns false when compression fails
// (`*out` then holds a partial frame; the caller should not write it).
bool append_frame(std::vector<uint8_t>* out, const std::vector<uint8_t>& payload, int threshold);

// Writes already-framed bytes verbatim. `frame` must come from `encode_frame`
// (at the threshold that connection uses); the bytes are not re-checked.
// Returns false on write error.
bool write_encoded_frame(mc::net::Stream& stream, const std::vector<uint8_t>& frame);

// Buffer-based frame consumer for poll-driven loops (play state). If `buf`
// holds at least one complete frame, removes it from the front, decompresses
// it per `threshold`, stores the packet in `*out` and returns true. If only a
// partial frame is present the buffer is left untouched and false is returned
// (callers refill and retry). Oversized / malformed frames return false AND
// clear the buffer (the connection is unrecoverable).
bool consume_frame(std::vector<uint8_t>& buf, int threshold, std::vector<uint8_t>* out);

} // namespace mc::protocol
