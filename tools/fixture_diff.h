// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shared half of the golden-dump generators (tools/gen_feature_dump.cpp and
// tools/gen_random_tick_dump.cpp): reading a fixture back, and printing the
// patch between what is on disk and what this run would write.
//
// Those generators are regression pins of *this* runtime's own output, so the
// question when one has to be regenerated is "what changed, and is that what I
// meant?" — and a failing `ctest` answers only the first half of it. `--diff`
// writes nothing and prints a reviewable patch instead, so the change can be
// read before the fixture is rewritten.
#pragma once

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <zlib.h>

namespace fixture_diff {

// A text file's lines, with any `\r` stripped. `exists` is false when the file
// is not there at all — a fixture that has never been written, which the caller
// reports as an all-added patch rather than as an error.
struct Lines {
  bool exists = false;
  std::vector<std::string> lines;
};

inline Lines read_lines(const std::string& path) {
  Lines out;
  std::ifstream file(path);
  if (!file)
    return out;
  out.exists = true;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    out.lines.push_back(line);
  }
  return out;
}

// A whole `.bin.gz` cell file, or an empty vector when it is missing or cannot
// be read.
inline std::vector<uint8_t> read_gzip(const std::string& path) {
  std::vector<uint8_t> out;
  gzFile file = gzopen(path.c_str(), "rb");
  if (file == nullptr)
    return out;
  uint8_t buffer[64 * 1024];
  int read = 0;
  while ((read = gzread(file, buffer, sizeof(buffer))) > 0)
    out.insert(out.end(), buffer, buffer + read);
  gzclose(file);
  return out;
}

// The index-aligned patch of two row lists. Everything before the first row that
// differs and after the last is kept as context; the differing middle is printed
// in file order, `-` rows then `+` rows, capped at `max_rows` lines with the
// elided count printed.
//
// Rows are compared as text, so this is a readable patch rather than a minimal
// one: a row inserted early in a trace shows as the whole tail replaced, which
// is exactly the shape a shifted random stream has, and which is why the caller
// should align its rows by their own keys (a tick number, a coordinate) before
// reaching here.
inline void print_row_diff(const std::string& label, const std::vector<std::string>& on_disk,
                           const std::vector<std::string>& regenerated, size_t max_rows = 60) {
  size_t first = 0;
  while (first < on_disk.size() && first < regenerated.size() &&
         on_disk[first] == regenerated[first])
    ++first;
  size_t last_old = on_disk.size();
  size_t last_new = regenerated.size();
  while (last_old > first && last_new > first &&
         on_disk[last_old - 1] == regenerated[last_new - 1]) {
    --last_old;
    --last_new;
  }
  const size_t removed = last_old - first;
  const size_t added = last_new - first;
  if (removed == 0 && added == 0) {
    std::printf("  %s: unchanged (%zu rows)\n", label.c_str(), regenerated.size());
    return;
  }
  std::printf("  --- %s (on disk)\n", label.c_str());
  std::printf("  +++ %s (regenerated)\n", label.c_str());
  std::printf("  @@ %zu changed rows: %zu kept before, %zu after @@\n", removed + added, first,
              on_disk.size() - last_old);
  size_t printed = 0;
  for (size_t i = first; i < last_old && printed < max_rows; ++i, ++printed)
    std::printf("  - %s\n", on_disk[i].c_str());
  for (size_t i = first; i < last_new && printed < max_rows; ++i, ++printed)
    std::printf("  + %s\n", regenerated[i].c_str());
  if (removed + added > printed)
    std::printf("  ... %zu more rows\n", removed + added - printed);
}

} // namespace fixture_diff
