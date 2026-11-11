// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fuzz target for the protocol parsing layer: Minecraft framing (plain and
// zlib-compressed), VarInt decoding, and the net::Reader codec. One input is
// viewed three ways:
//
//   1. as a frame buffer fed to consume_frame() (the poll-loop path),
//   2. as a raw byte stream fed to read_frame() through a memory Stream (the
//      blocking socket path),
//   3. as bare varint bytes for decode_varint().
//
// Every frame that parses is then walked with net::Reader using a
// deterministic, data-selected sequence of typed reads. The codec contract is
// asserted throughout: a failed decode must not advance the reader, and a
// successful one must. libFuzzer (LLVMFuzzerTestOneInput) and a standalone
// driver (PLUSMC_FUZZ_STANDALONE) share the same body so the target runs on
// toolchains without libFuzzer (e.g. Apple clang) under ASan/UBSan.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "net/byte_buffer.h"
#include "net/stream.h"
#include "protocol/framing.h"

namespace {

// Thresholds worth exercising: plain, threshold-0 (everything compressed),
// sub-compression, and values straddling typical packet sizes.
constexpr int kThresholds[] = {-1, 0, 1, 63, 256, 1024, 4096};

// Upper bound on work per input so neither the fuzzer nor the standalone
// driver can stall on a single pathological buffer. A few dozen reader steps
// per packet already cover the codec's decode paths; the string/bytes cases
// are capped so the walk stays allocation-cheap under ASan.
constexpr size_t kMaxFramesPerInput = 64;
constexpr size_t kMaxReaderSteps = 64;
constexpr uint32_t kMaxWalkStringLen = 256;

class MemStream final : public mc::net::Stream {
public:
  explicit MemStream(const std::vector<uint8_t>& data) : data_(data) {}

  bool read_exact(uint8_t* out, size_t n) override {
    if (pos_ + n > data_.size())
      return false;
    std::memcpy(out, data_.data() + pos_, n);
    pos_ += n;
    return true;
  }
  bool write_all(const uint8_t*, size_t) override {
    return true; // write path is not the target; discard
  }

private:
  const std::vector<uint8_t>& data_;
  size_t pos_ = 0;
};

// Walks a decoded packet with net::Reader: the byte at the current position
// selects the next typed read, so mutations explore different codec paths.
void walk_packet(const std::vector<uint8_t>& pkt) {
  mc::net::Reader r(pkt);
  uint32_t u = 0;
  uint64_t u64v = 0;
  int64_t i64v = 0;
  uint16_t u16v = 0;
  uint8_t u8v = 0;
  float f = 0.0f;
  double d = 0.0;
  std::string s;
  for (size_t step = 0; step < kMaxReaderSteps && !r.at_end(); ++step) {
    const size_t before = r.position();
    const uint8_t sel = pkt[r.position()];
    bool ok = true;
    switch (sel & 0xF) {
    case 0:
      ok = r.varint(&u);
      break;
    case 1:
      ok = r.u8(&u8v);
      break;
    case 2:
      ok = r.u16(&u16v);
      break;
    case 3:
      ok = r.u32(&u);
      break;
    case 4:
      ok = r.u64(&u64v);
      break;
    case 5:
      ok = r.i64(&i64v);
      break;
    case 6:
      ok = r.f32(&f);
      break;
    case 7:
      ok = r.f64(&d);
      break;
    case 8: {
      // varint length-prefixed string; skip oversized copies so the walk
      // stays allocation-cheap under ASan
      uint32_t len = 0;
      ok = r.varint(&len);
      if (ok && len <= kMaxWalkStringLen)
        ok = r.string(&s);
      break;
    }
    case 9: {
      // varint length-prefixed raw bytes
      uint32_t len = 0;
      ok = r.varint(&len);
      if (ok && len <= kMaxWalkStringLen) {
        std::vector<uint8_t> raw;
        ok = r.bytes(len, &raw);
      }
      break;
    }
    default:
      ok = r.varint(&u);
      break;
    }
    if (!ok)
      break;
    if (r.position() <= before)
      __builtin_trap(); // contract: a successful read advances
    if (r.position() > pkt.size())
      __builtin_trap(); // contract: never past the end
  }
}

void exercise_frame_buffer(const std::vector<uint8_t>& input) {
  for (const int t : kThresholds) {
    std::vector<uint8_t> buf = input;
    std::vector<uint8_t> pkt;
    for (size_t i = 0; i < kMaxFramesPerInput; ++i) {
      if (buf.empty())
        break;
      const size_t before = buf.size();
      if (!mc::protocol::consume_frame(buf, t, &pkt))
        break;
      if (buf.size() >= before)
        __builtin_trap(); // a consumed frame must shrink the buffer
      walk_packet(pkt);
    }
  }
}

void exercise_stream_read(const std::vector<uint8_t>& input) {
  for (const int t : kThresholds) {
    MemStream stream(input);
    for (size_t i = 0; i < kMaxFramesPerInput; ++i) {
      auto pkt = mc::protocol::read_frame(stream, t);
      if (!pkt)
        break;
      walk_packet(*pkt);
    }
  }
}

void exercise_varint(const std::vector<uint8_t>& input) {
  for (size_t n = 1; n <= input.size() && n <= 6; ++n) {
    const std::vector<uint8_t> prefix(input.begin(), input.begin() + n);
    size_t pos = 0;
    uint32_t out = 0;
    if (mc::net::decode_varint(prefix, &pos, &out)) {
      if (pos == 0 || pos > prefix.size())
        __builtin_trap(); // contract: success advances within bounds
    } else if (pos != 0) {
      __builtin_trap(); // contract: failure leaves pos untouched
    }
  }
}

void run_one(const std::vector<uint8_t>& input) {
  if (input.empty())
    return;
  exercise_frame_buffer(input);
  exercise_stream_read(input);
  exercise_varint(input);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::vector<uint8_t> input(data, data + size);
  run_one(input);
  return 0;
}

#ifdef PLUSMC_FUZZ_STANDALONE
// Standalone driver: replays corpus files given on the command line, then
// generates deterministic pseudo-random inputs (optionally mutated from the
// corpus) so the parsing layer can be exercised under ASan/UBSan without
// libFuzzer. Usage: fuzz-frame-standalone <corpus-file>... [--iters N]
//                                          [--max-len N] [--seed N]
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

std::vector<uint8_t> mutate(const std::vector<uint8_t>& base, std::mt19937_64& rng) {
  std::vector<uint8_t> out = base;
  const int edits = 1 + static_cast<int>(rng() % 8);
  for (int i = 0; i < edits; ++i) {
    switch (rng() % 4) {
    case 0: // flip a byte
      if (!out.empty())
        out[rng() % out.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
      break;
    case 1: // insert a random byte
      out.insert(out.begin() + static_cast<ptrdiff_t>(rng() % (out.size() + 1)),
                 static_cast<uint8_t>(rng()));
      break;
    case 2: // delete a byte
      if (!out.empty())
        out.erase(out.begin() + static_cast<ptrdiff_t>(rng() % out.size()));
      break;
    case 3: // overwrite a run
      if (!out.empty()) {
        const size_t at = rng() % out.size();
        const size_t len = 1 + static_cast<size_t>(rng() % 16);
        for (size_t j = at; j < out.size() && j < at + len; ++j)
          out[j] = static_cast<uint8_t>(rng());
      }
      break;
    }
  }
  return out;
}

} // namespace

int main(int argc, char** argv) {
  std::vector<std::vector<uint8_t>> corpus;
  size_t iters = 200000;
  size_t max_len = 4096;
  uint64_t seed = 0xC0FFEE;
  bool next_is_iters = false, next_is_maxlen = false, next_is_seed = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (next_is_iters) {
      iters = static_cast<size_t>(std::strtoull(arg.c_str(), nullptr, 10));
      next_is_iters = false;
    } else if (next_is_maxlen) {
      max_len = static_cast<size_t>(std::strtoull(arg.c_str(), nullptr, 10));
      next_is_maxlen = false;
    } else if (next_is_seed) {
      seed = std::strtoull(arg.c_str(), nullptr, 10);
      next_is_seed = false;
    } else if (arg == "--iters") {
      next_is_iters = true;
    } else if (arg == "--max-len") {
      next_is_maxlen = true;
    } else if (arg == "--seed") {
      next_is_seed = true;
    } else {
      corpus.push_back(read_file(arg));
    }
  }

  std::fprintf(stderr, "corpus files: %zu, iters: %zu, max-len: %zu\n", corpus.size(), iters,
               max_len);
  std::mt19937_64 rng(seed);
  for (size_t i = 0; i < iters; ++i) {
    std::vector<uint8_t> input;
    if (!corpus.empty() && (i % 8) == 0) {
      input = mutate(corpus[rng() % corpus.size()], rng);
    } else {
      const size_t len = 1 + static_cast<size_t>(rng() % max_len);
      input.resize(len);
      for (auto& b : input)
        b = static_cast<uint8_t>(rng());
    }
    run_one(input);
  }
  std::fprintf(stderr, "done: %zu inputs, no crashes\n", iters);
  return 0;
}
#endif // PLUSMC_FUZZ_STANDALONE
