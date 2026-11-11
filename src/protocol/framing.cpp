// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol/framing.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "net/byte_buffer.h"

namespace mc::protocol {

namespace {

// The zlib level the outbound frame compressor uses.
//
// Every frame above the connection's compression threshold is deflated once and
// the resulting bytes are what the socket carries; a chunk frame is the big
// one (a 58.6 KB body), and the send path compresses one per column per client
// (the frame cache in session.cpp shares it across the sessions that want the
// same column). Level 6 is what zlib calls default and what this used to pay.
// Measured on the overworld body in tests/fixtures/ow3_chunk_n2m1_body.hex
// (38.9 KB, `zlib.compress`, best of 30):
//
//   level 6   1.23 ms   7959 bytes
//   level 5   0.84 ms   8034 bytes  (-32% CPU, +0.9% bytes)
//   level 4   0.58 ms   8146 bytes  (-53% CPU, +2.3% bytes)
//   level 3   0.52 ms   8687 bytes  (-58% CPU, +9.1% bytes)
//
// 4 is the default because the send path is CPU-bound — one deflate per column
// per client, and the bytes are ~8 KB either way — so halving the cost is worth
// 187 bytes of a 58 KB frame. `PLUSMC_WIRE_ZLEVEL=1..9` is the arm for a box
// that would rather spend bandwidth (see docs/PHASE5.md for the table). The
// value is read once: a per-frame getenv would be the thing that shows up in
// the profile.
int wire_compression_level() {
  static const int level = [] {
    const char* raw = std::getenv("PLUSMC_WIRE_ZLEVEL");
    if (raw == nullptr)
      return 4;
    const int parsed = std::atoi(raw);
    return parsed >= 1 && parsed <= 9 ? parsed : 4;
  }();
  return level;
}

// `compress2` builds and frees a z_stream per call: deflateInit allocates
// roughly 256 KiB of state, and a chunk send compresses one ~60 KB body per
// column per client. One stream per thread, reset between frames, emits
// byte-identical output (same level, 15 window bits, memLevel 8 and
// Z_DEFAULT_STRATEGY as `compress2`) and pays the allocation once. The A/B arm
// is `PLUSMC_FRAME_ZSTREAM=off`, which uses the one-shot path.
struct DeflateStream {
  z_stream zs{};
  bool ready = false;
  ~DeflateStream() {
    if (ready)
      deflateEnd(&zs);
  }
};
thread_local DeflateStream tls_deflate;

bool frame_stream_reuse_enabled() {
  static const bool kOn = [] {
    const char* text = std::getenv("PLUSMC_FRAME_ZSTREAM");
    return !(text != nullptr && text[0] == 'o' && text[1] == 'f' && text[2] == 'f' &&
             text[3] == '\0');
  }();
  return kOn;
}

bool deflate_payload(const std::vector<uint8_t>& payload, std::vector<uint8_t>* scratch,
                     uLongf* produced) {
  if (!frame_stream_reuse_enabled()) {
    const uLong bound = compressBound(static_cast<uLong>(payload.size()));
    if (scratch->size() < bound)
      scratch->resize(static_cast<size_t>(bound));
    *produced = bound;
    return compress2(scratch->data(), produced, payload.data(), static_cast<uLong>(payload.size()),
                     wire_compression_level()) == Z_OK;
  }
  DeflateStream& state = tls_deflate;
  if (!state.ready) {
    if (deflateInit2(&state.zs, wire_compression_level(), Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK)
      return false;
    state.ready = true;
  } else if (deflateReset(&state.zs) != Z_OK) {
    return false;
  }
  const uLong bound = deflateBound(&state.zs, static_cast<uLong>(payload.size()));
  if (scratch->size() < bound)
    scratch->resize(static_cast<size_t>(bound));
  state.zs.next_in = const_cast<Bytef*>(payload.data());
  state.zs.avail_in = static_cast<uInt>(payload.size());
  state.zs.next_out = scratch->data();
  state.zs.avail_out = static_cast<uInt>(scratch->size());
  if (deflate(&state.zs, Z_FINISH) != Z_STREAM_END)
    return false;
  *produced = state.zs.total_out;
  return true;
}

// Reads a VarInt from the connection, byte by byte. On success returns the
// value and the number of bytes it occupied on the wire.
struct VarIntResult {
  uint32_t value = 0;
  int bytes = 0;
};
std::optional<VarIntResult> read_varint(mc::net::Stream& stream) {
  VarIntResult r;
  for (int i = 0; i < 5; ++i) {
    uint8_t byte = 0;
    if (!stream.read_exact(&byte, 1))
      return std::nullopt;
    ++r.bytes;
    r.value |= static_cast<uint32_t>(byte & 0x7F) << (7 * i);
    if ((byte & 0x80) == 0)
      return r;
  }
  return std::nullopt; // VarInt over 5 bytes
}

// Writes a VarInt into a caller-provided buffer (at most five bytes) and
// returns the byte count. `append_frame` needs the compressed size as a length
// prefix before the bytes that follow it; the `std::vector`-target form below
// would be a heap allocation for at most five bytes.
int encode_varint_to(uint32_t value, uint8_t* out) {
  int count = 0;
  do {
    uint8_t byte = static_cast<uint8_t>(value & 0x7F);
    value >>= 7;
    if (value != 0)
      byte |= 0x80;
    out[count++] = byte;
  } while (value != 0);
  return count;
}

// VarInt over a byte buffer, non-consuming. Returns the byte count on success.
int peek_varint(const std::vector<uint8_t>& buf, size_t pos, uint32_t* value) {
  uint32_t v = 0;
  for (int i = 0; i < 5; ++i) {
    if (pos + static_cast<size_t>(i) >= buf.size())
      return 0; // need more bytes
    const uint8_t byte = buf[pos + static_cast<size_t>(i)];
    v |= static_cast<uint32_t>(byte & 0x7F) << (7 * i);
    if ((byte & 0x80) == 0) {
      *value = v;
      return i + 1;
    }
  }
  return -1; // >5 bytes: invalid
}

bool inflate_all(const uint8_t* src, size_t src_len, size_t expected, std::vector<uint8_t>* out) {
  if (expected == 0 || expected > kMaxFrameSize)
    return false;
  z_stream zs{};
  if (inflateInit(&zs) != Z_OK)
    return false; // zlib wrapper (RFC 1950)
  // Inflate in bounded chunks and grow the output only as bytes are actually
  // produced. A stream that lies about `expected` (or is garbage) therefore
  // costs almost nothing, instead of a full up-to-8-MiB pre-allocation per
  // hostile frame.
  std::array<uint8_t, 16384> chunk{};
  zs.next_in = const_cast<Bytef*>(src);
  zs.avail_in = static_cast<uInt>(src_len);
  bool ok = false;
  for (;;) {
    zs.next_out = chunk.data();
    zs.avail_out = static_cast<uInt>(chunk.size());
    const int rc = inflate(&zs, Z_NO_FLUSH);
    const size_t produced = chunk.size() - zs.avail_out;
    if (produced > 0) {
      if (out->size() + produced > expected || out->size() + produced > kMaxFrameSize) {
        ok = false;
        break;
      }
      out->insert(out->end(), chunk.data(), chunk.data() + produced);
    }
    if (rc == Z_STREAM_END) {
      ok = (out->size() == expected) && (zs.avail_in == 0);
      break;
    }
    if (rc != Z_OK) {
      ok = false; // Z_BUF_ERROR / Z_DATA_ERROR / ...: corrupt or truncated
      break;
    }
    // rc == Z_OK: continue only while there is more input to feed; with no
    // input left the next call would return Z_BUF_ERROR and we fail above.
    if (zs.avail_in == 0 && zs.avail_out != 0)
      break; // no progress possible without more input: truncated stream
  }
  inflateEnd(&zs);
  return ok;
}

} // namespace

// Reads `n` bytes from the stream into `out`, but in bounded slices so the
// buffer grows only as data actually arrives. A frame length claim therefore
// cannot force an up-to-kMaxFrameSize allocation up front: a peer that claims
// a huge frame and sends nothing costs at most one slice.
bool read_exact_chunked(mc::net::Stream& stream, size_t n, std::vector<uint8_t>* out) {
  out->clear();
  out->reserve(std::min(n, kMaxFrameSize));
  std::array<uint8_t, 16384> chunk{};
  size_t need = n;
  while (need > 0) {
    const size_t take = std::min(need, chunk.size());
    if (!stream.read_exact(chunk.data(), take))
      return false;
    out->insert(out->end(), chunk.data(), chunk.data() + take);
    need -= take;
  }
  return true;
}

std::optional<std::vector<uint8_t>> read_frame(mc::net::Stream& stream, int threshold) {
  auto length = read_varint(stream);
  if (!length || length->value == 0 || length->value > kMaxFrameSize + 5)
    return std::nullopt;

  if (threshold < 0) {
    // Plain frame: [length][packet data].
    if (length->value > kMaxFrameSize)
      return std::nullopt;
    std::vector<uint8_t> payload;
    if (!read_exact_chunked(stream, length->value, &payload))
      return std::nullopt;
    return payload;
  }

  // Compressed frame: [length][dataLength][data]; `length` counts the bytes of
  // dataLength plus the data that follows it.
  //
  // The whole body is read before `dataLength` is parsed, rather than reading
  // the varint straight off the stream. A VarInt read from the stream can run
  // past the frame the length declared — a body whose only byte is 0x80 (a
  // continuation) makes the read take the next frame's first byte — which
  // leaves the connection parsing from the middle of a later frame. Bounding
  // the read by `length` makes that body a malformed frame instead of a
  // desync, and `length` is already capped above.
  std::vector<uint8_t> body;
  if (!read_exact_chunked(stream, length->value, &body))
    return std::nullopt;
  uint32_t data_length = 0;
  const int data_length_bytes = peek_varint(body, 0, &data_length);
  if (data_length_bytes <= 0 || static_cast<size_t>(data_length_bytes) > body.size())
    return std::nullopt;
  const size_t data_size = body.size() - static_cast<size_t>(data_length_bytes);
  if (data_size > kMaxFrameSize)
    return std::nullopt;
  const uint8_t* data = body.data() + data_length_bytes;

  if (data_length == 0) {
    // Under-threshold packet, transmitted raw. Drop the dataLength prefix in
    // place: the frame is small (it was under the threshold) and the caller
    // gets the body vector rather than a copy of it.
    body.erase(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(data_length_bytes));
    return body;
  }
  // Compressed packet: dataLength is the uncompressed size.
  std::vector<uint8_t> payload;
  if (!inflate_all(data, data_size, data_length, &payload))
    return std::nullopt;
  return payload;
}

bool append_frame(std::vector<uint8_t>* out, const std::vector<uint8_t>& payload, int threshold) {
  if (threshold < 0) {
    // Plain framing (compression disabled): [length][packet data].
    mc::net::encode_varint(static_cast<uint32_t>(payload.size()), *out);
    out->insert(out->end(), payload.begin(), payload.end());
    return true;
  }

  // Compression enabled: EVERY frame uses [dataLength][data], where under-
  // threshold packets carry dataLength = 0 followed by the raw packet. That
  // branch is written straight into `out`: a fan-out that batches a tick's
  // packets for one connection pays one append and no temporary frame at all.
  if (payload.size() < static_cast<size_t>(threshold)) {
    mc::net::encode_varint(static_cast<uint32_t>(payload.size() + 1), *out); // frame length
    out->push_back(0);                                                       // dataLength
    out->insert(out->end(), payload.begin(), payload.end());
    return true;
  }
  // The deflate output lives in a per-thread scratch that keeps its high-water
  // mark. Every compressed frame used to allocate a `compressBound` block (a
  // chunk frame is ~60 KB in, ~8 KB out) and a second `std::vector` for the
  // two-to-three-byte `dataLength` prefix; a fan-out does this once per
  // recipient threshold, so the steady state is now allocation-free.
  thread_local std::vector<uint8_t> scratch;
  uLongf produced = 0;
  if (!deflate_payload(payload, &scratch, &produced))
    return false;
  uint8_t data_length[5]; // dataLength = the uncompressed size
  const int data_length_bytes =
      encode_varint_to(static_cast<uint32_t>(payload.size()), data_length);
  // Frame length: the dataLength varint plus the compressed bytes.
  mc::net::encode_varint(static_cast<uint32_t>(static_cast<size_t>(data_length_bytes) + produced),
                         *out);
  out->insert(out->end(), data_length, data_length + data_length_bytes);
  out->insert(out->end(), scratch.data(), scratch.data() + produced);
  return true;
}

std::vector<uint8_t> encode_frame(const std::vector<uint8_t>& payload, int threshold) {
  std::vector<uint8_t> frame;
  if (!append_frame(&frame, payload, threshold))
    return {}; // empty: callers that write it will fail the connection
  return frame;
}

bool write_frame(mc::net::Stream& stream, int threshold, const std::vector<uint8_t>& payload) {
  const std::vector<uint8_t> frame = encode_frame(payload, threshold);
  if (frame.empty() && !payload.empty())
    return false; // a compression failure is not a frame
  return stream.write_all(frame.data(), frame.size());
}

bool write_encoded_frame(mc::net::Stream& stream, const std::vector<uint8_t>& frame) {
  return stream.write_all(frame.data(), frame.size());
}

bool consume_frame(std::vector<uint8_t>& buf, int threshold, std::vector<uint8_t>* out) {
  uint32_t length = 0;
  const int length_bytes = peek_varint(buf, 0, &length);
  if (length_bytes == 0)
    return false; // partial length varint
  if (length_bytes < 0 || length == 0 || length > kMaxFrameSize + 5) {
    buf.clear(); // unrecoverable framing
    return false;
  }
  const size_t frame_bytes = static_cast<size_t>(length_bytes) + length;
  if (buf.size() < frame_bytes)
    return false; // partial frame

  const uint8_t* body = buf.data() + length_bytes;
  const size_t body_len = length;
  std::vector<uint8_t> payload;
  if (threshold < 0) {
    if (length > kMaxFrameSize) {
      buf.clear();
      return false;
    }
    payload.assign(body, body + body_len);
  } else {
    // Compressed framing: [dataLength][data]; the frame length counts the
    // bytes of the dataLength varint plus the data that follows it.
    uint32_t data_length = 0;
    const int dl_bytes = peek_varint(buf, static_cast<size_t>(length_bytes), &data_length);
    // The frame is already complete, so an incomplete dataLength varint here
    // means the stream is corrupt, not merely partial.
    if (dl_bytes <= 0 || static_cast<size_t>(dl_bytes) > body_len) {
      buf.clear();
      return false;
    }
    const size_t data_size = body_len - static_cast<size_t>(dl_bytes);
    if (data_size > kMaxFrameSize) {
      buf.clear();
      return false;
    }
    const uint8_t* data = body + dl_bytes;
    if (data_length == 0) {
      payload.assign(data, data + data_size);
    } else if (!inflate_all(data, data_size, data_length, &payload)) {
      buf.clear();
      return false;
    }
  }

  buf.erase(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(frame_bytes));
  *out = std::move(payload);
  return true;
}

} // namespace mc::protocol
