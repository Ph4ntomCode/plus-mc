// SPDX-License-Identifier: GPL-3.0-or-later
//
// `plusmc-convert port`: the third converter slice (docs/PLAN.md §9.3). It takes
// the analyzer's manifest and the mapper's decisions and emits a native +MC
// plugin project: the manifest, a source file whose commands and events are
// registered with working default bodies, a CMakeLists that builds the module, a
// README and a machine-readable list of behavior that still has to be written.
// No bytecode is read, so the defaults log and answer with usage rather than
// inventing the original behavior.

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "converter/jar_scan.h"

namespace mc::converter {

struct PortOptions {
  std::filesystem::path package;
  std::filesystem::path out_dir;
  // Update an existing port in place: rewrite the manifest and work diff but
  // preserve the human-owned source, CMake project and README.
  bool from_port = false;
};

// One line of the port diff. `kind` is command, event, permission, reference
// or manual; `status` is generated, preserved, inserted, mapped, partial, gap or
// manual. `reference` is the
// original name (a command, an event class, a permission node, an API symbol or
// the manifest), and `action` is what a human has to do about it.
struct PortWorkItem {
  std::string kind;
  std::string reference;
  std::string status;
  std::string action;
};

struct PortReport {
  std::string package;
  bool ok = false;
  std::string error;
  std::string plugin_name;
  std::string plugin_version;
  std::string identifier;
  std::string out_dir;
  bool from_port = false;
  // Files left untouched by an update (the source, plus CMake/README when
  // present). Empty for a fresh scaffold.
  std::vector<std::string> preserved;
  Score score = Score::Red;
  std::vector<std::string> files;
  std::size_t commands = 0;
  std::size_t permissions = 0;
  std::size_t events = 0;
  // Handlers inserted into a preserved source by --from-port. Zero for a fresh
  // scaffold and for an update that had nothing new (or no usable marker).
  std::size_t inserted = 0;
  std::size_t mapped = 0;
  std::size_t partial = 0;
  std::size_t gap = 0;
  std::size_t work_truncated = 0;
  std::vector<PortWorkItem> work;
  std::vector<std::string> notes;
};

PortReport port_jar(const PortOptions& options);
std::string port_report_text(const PortReport& report);
std::string port_report_json(const PortReport& report);

} // namespace mc::converter
