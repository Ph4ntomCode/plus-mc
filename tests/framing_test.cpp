// SPDX-License-Identifier: GPL-3.0-or-later
//
// `encode_frame` and `write_encoded_frame` — the split that lets one
// compression be shared by many connections (docs/WRITE_PATH.md).
//
// The property that matters is an equality: the bytes `encode_frame` returns
// are exactly the bytes `write_frame` used to put on the wire, for every
// threshold shape (compression off, compression on with an under-threshold and
// an over-threshold payload) and at the payload sizes the engine actually
// sends (a movement packet of tens of bytes, a chunk body of tens of KiB).
// `write_encoded_frame` then writes those bytes verbatim, which is what a
// server does when 60 viewers of one chunk share one compressed frame.
//
// The test also pins the edges: a threshold of 0 compresses everything,
// including a one-byte payload; a refused write reports failure; and a
// zero-length *payload* is not a legal frame on the read side (the engine never
// produces one, and `consume_frame` treats a zero frame length as an
// unrecoverable stream).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "net/byte_buffer.h"
#include "net/stream.h"
#include "protocol/framing.h"

namespace {

// A Stream that keeps everything written to it, and can be handed the same
// bytes through both write paths so they can be compared.
struct CaptureStream : mc::net::Stream {
  std::vector<uint8_t> bytes;
  bool fail = false;

  bool read_exact(uint8_t*, size_t) override {
    return false;
  }
  bool write_all(const uint8_t* data, size_t n) override {
    if (fail)
      return false;
    bytes.insert(bytes.end(), data, data + n);
    return true;
  }
};

// The read side of `read_frame`, which `CaptureStream` (write-only) cannot
// exercise: an in-memory frame sequence with a cursor.
struct ReadStream : mc::net::Stream {
  std::vector<uint8_t> bytes;
  size_t pos = 0;

  bool read_exact(uint8_t* out, size_t n) override {
    if (pos + n > bytes.size())
      return false;
    std::memcpy(out, bytes.data() + pos, n);
    pos += n;
    return true;
  }
  bool write_all(const uint8_t*, size_t) override {
    return false;
  }
};

int failures = 0;
void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
  }
}

std::vector<uint8_t> payload_of(size_t n) {
  std::vector<uint8_t> out(n);
  // A compressible-but-not-trivial pattern: packet ids and positions in a
  // chunk body are mostly low-entropy bytes, which is why the wire compresses
  // 5-40 KB bodies at all. A solid run of one byte would flatter the ratio.
  for (size_t i = 0; i < n; ++i)
    out[i] = static_cast<uint8_t>((i * 31 + (i >> 8)) & 0x3F);
  return out;
}

// `write_frame` and `encode_frame` must agree byte for byte, and a verbatim
// write of the encoder's output must produce the same bytes again.
void check_equivalence(const std::vector<uint8_t>& payload, int threshold,
                       const std::string& label) {
  CaptureStream via_write;
  check(mc::protocol::write_frame(via_write, threshold, payload), label + ": write_frame");

  const std::vector<uint8_t> encoded = mc::protocol::encode_frame(payload, threshold);
  check(encoded == via_write.bytes, label + ": encode_frame == write_frame's bytes");

  CaptureStream via_encoded;
  check(mc::protocol::write_encoded_frame(via_encoded, encoded), label + ": write_encoded_frame");
  check(via_encoded.bytes == via_write.bytes, label + ": verbatim write reproduces the frame");

  // A shared frame may be written to many connections: the second write must
  // be byte-identical to the first.
  CaptureStream second;
  check(mc::protocol::write_encoded_frame(second, encoded), label + ": second connection");
  check(second.bytes == via_write.bytes, label + ": two connections get identical bytes");
}

// The *shape on the wire* at the compression boundary, spelled out from the
// protocol rather than from `encode_frame`. The equality checks above only say
// the encoder and the writer agree; a change that moved the boundary for both
// (say `<` to `<=`) would leave them agreeing and every client broken. So this
// pins the two cases either side of the boundary against bytes this test builds
// itself. The rule is vanilla's: a payload *smaller* than the threshold is
// [dataLength 0][raw payload], and a payload at or over it is
// [dataLength = size][zlib stream] — so `size == threshold` is the first
// compressed frame, which is the edge an off-by-one moves.
void check_boundary_shape(size_t threshold) {
  const std::vector<uint8_t> under = payload_of(threshold - 1);
  const std::vector<uint8_t> frame = mc::protocol::encode_frame(under, static_cast<int>(threshold));

  std::vector<uint8_t> body;
  mc::net::encode_varint(0, body); // dataLength 0: not compressed
  body.insert(body.end(), under.begin(), under.end());
  std::vector<uint8_t> expected;
  mc::net::encode_varint(static_cast<uint32_t>(body.size()), expected);
  expected.insert(expected.end(), body.begin(), body.end());
  check(frame == expected, "size < threshold frames as [dataLength 0][payload]");

  const std::vector<uint8_t> over = payload_of(threshold);
  const std::vector<uint8_t> compressed_frame =
      mc::protocol::encode_frame(over, static_cast<int>(threshold));
  size_t pos = 0;
  uint32_t frame_length = 0;
  uint32_t data_length = 0;
  check(mc::net::decode_varint(compressed_frame, &pos, &frame_length),
        "size > threshold: the frame length is a varint");
  const size_t body_start = pos;
  check(mc::net::decode_varint(compressed_frame, &pos, &data_length),
        "size == threshold: dataLength is a varint");
  check(data_length == over.size(), "dataLength is the uncompressed size");
  check(pos < compressed_frame.size() && compressed_frame[pos] != 0x00,
        "size == threshold is the first compressed frame");
  check(frame_length == compressed_frame.size() - body_start,
        "the frame length covers the body exactly");

  std::vector<uint8_t> round_tripped = compressed_frame;
  std::vector<uint8_t> out;
  check(mc::protocol::consume_frame(round_tripped, static_cast<int>(threshold), &out) &&
            out == over,
        "the at-threshold frame inflates to the payload");
}

// The frame a reader sees must decode back to the payload (a weaker check, but
// it catches a length-prefix slip that byte equality alone cannot, because both
// sides would be wrong together).
void check_round_trip(const std::vector<uint8_t>& payload, int threshold,
                      const std::string& label) {
  const std::vector<uint8_t> frame = mc::protocol::encode_frame(payload, threshold);
  std::vector<uint8_t> buffer = frame;
  std::vector<uint8_t> out;
  const bool consumed = mc::protocol::consume_frame(buffer, threshold, &out);
  check(consumed, label + ": consume_frame accepts the frame");
  check(out == payload, label + ": round trip returns the payload");
  check(buffer.empty(), label + ": the frame is consumed exactly");
}

} // namespace

int main() {
  const size_t sizes[] = {1, 10, 255, 256, 300, 2048, 4096, 50000};
  const int thresholds[] = {-1, 0, 1, 256, 4096, 65536};
  for (const int threshold : thresholds) {
    for (const size_t size : sizes) {
      const std::vector<uint8_t> payload = payload_of(size);
      const std::string label =
          "size " + std::to_string(size) + ", threshold " + std::to_string(threshold);
      check_equivalence(payload, threshold, label);
      check_round_trip(payload, threshold, label);
    }
  }

  // The frame-length claim itself: a 50 KB payload at no compression is
  // [varint 50000][50000 bytes], and nothing shorter starts with it.
  {
    const std::vector<uint8_t> payload = payload_of(50000);
    const std::vector<uint8_t> frame = mc::protocol::encode_frame(payload, -1);
    check(frame.size() == payload.size() + 3, "plain frame length is payload + varint(50000)");
    uint32_t length = 0;
    int bytes = 0;
    for (int i = 0; i < 5; ++i) {
      const uint8_t byte = frame[static_cast<size_t>(i)];
      length |= static_cast<uint32_t>(byte & 0x7F) << (7 * i);
      ++bytes;
      if ((byte & 0x80) == 0)
        break;
    }
    check(length == 50000 && bytes == 3, "the frame's own length varint is 50000 in 3 bytes");
  }

  // A zero-length payload encodes to a "frame" whose length varint is 0 — and
  // the read side refuses that as unrecoverable, which is exactly what should
  // happen to a peer that sends it. Keep the two halves pinned together so the
  // encoder's edge cannot drift away from the decoder's.
  {
    const std::vector<uint8_t> empty = mc::protocol::encode_frame({}, -1);
    check(empty.size() == 1 && empty[0] == 0, "an empty payload frames as a single zero length");
    std::vector<uint8_t> buffer = empty;
    std::vector<uint8_t> out;
    check(!mc::protocol::consume_frame(buffer, -1, &out), "a zero-length frame is refused");
    check(buffer.empty(), "the refused zero-length frame is cleared, not left to reparse");
  }

  // Both sides of the compression boundary, at a size big enough to be real.
  check_boundary_shape(256);
  check_boundary_shape(4096);

  // A compressed frame's dataLength varint is bounded by the frame it belongs
  // to. A body of one continuation byte (0x80) must be refused as malformed —
  // not read into the *next* frame's length byte, which is what a varint read
  // straight from the stream did. The second frame then has to decode normally,
  // which is how the test sees where the refused read stopped.
  {
    ReadStream stream;
    stream.bytes = {0x01, 0x80}; // [length=1][continuation byte]
    const size_t after_first = stream.bytes.size();
    // A valid raw frame at threshold 256: [length=2][dataLength=0][payload].
    stream.bytes.insert(stream.bytes.end(), {0x02, 0x00, 0x25});
    check(!mc::protocol::read_frame(stream, 256).has_value(),
          "dataLength may not read past its own frame");
    check(stream.pos == after_first, "the refused frame consumed only its own bytes");
    const auto next = mc::protocol::read_frame(stream, 256);
    check(next && *next == std::vector<uint8_t>{0x25},
          "the frame after a refused one still decodes");
  }

  // A caller that hands write_encoded_frame a frame the stream refuses gets
  // the failure back (it must not be swallowed): the connection is broken.
  {
    CaptureStream failing;
    failing.fail = true;
    check(!mc::protocol::write_encoded_frame(failing, {1, 2, 3}), "a refused write reports false");
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
