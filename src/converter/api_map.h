// SPDX-License-Identifier: GPL-3.0-or-later
//
// `plusmc-convert map`: the second converter slice (docs/PLAN.md §9.3). It takes
// the analyzer's references and maps each Bukkit/Paper class and member to the
// native plugin API's symbol, or records an explicit gap. The table is
// deliberately small and reviewable: every entry is a claim that this ABI has a
// path for that call, or that it does not. Nothing here executes a plugin.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "converter/jar_scan.h"

namespace mc::converter {

enum class MapStatus { Mapped, Partial, Gap };

struct ClassMapping {
  std::string reference;
  std::string native_symbol;
  MapStatus status = MapStatus::Gap;
  std::string note;
};

struct MemberMapping {
  std::string owner;
  std::string name;
  std::string descriptor;
  bool field = false;
  std::string native_symbol;
  MapStatus status = MapStatus::Gap;
  std::string note;
};

struct MappingReport {
  std::string package;
  bool ok = false;
  std::string error;
  Score score = Score::Red;
  std::vector<ClassMapping> classes;
  std::vector<MemberMapping> members;
  std::vector<std::string> notes;
};

MappingReport map_analysis(const Analysis& analysis);
std::string map_status_name(MapStatus status);
std::string map_report_text(const MappingReport& report);
std::string map_report_json(const MappingReport& report);

// The coverage ratio's numerator and denominator, exposed so `--fail-under`
// gates exactly the number the text and JSON reports print.
std::size_t mapping_reference_count(const MappingReport& report);
std::size_t mapping_covered_count(const MappingReport& report);

} // namespace mc::converter
