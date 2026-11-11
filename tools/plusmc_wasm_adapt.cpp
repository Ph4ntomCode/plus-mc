// SPDX-License-Identifier: GPL-3.0-or-later
//
// `plusmc-wasm-adapt` is the Phase 8 slice-3 adapter probe: it reads a
// component's import and export sections, maps every imported interface onto
// the pinned `pumpkin:plugin@0.2.0` table, and reports what the host can and
// cannot back. It never links, instantiates or executes the component.
//
// The command is a separate executable while the slice-2 runtime owns the
// `plusmc-wasm` driver's command set; the two land without editing one file.

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "wasm/component.h"

namespace {

void usage() {
  std::cout << "usage:\n"
            << "  plusmc-wasm-adapt <module.wasm | package-dir> [--json]\n"
            << "                    [--require-mapped] [--require-full]\n";
}

} // namespace

int main(int argc, char** argv) {
  std::string target;
  bool json = false;
  bool require_mapped = false;
  bool require_full = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--json") {
      json = true;
    } else if (argument == "--require-mapped") {
      require_mapped = true;
    } else if (argument == "--require-full") {
      require_full = true;
      require_mapped = true;
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return 0;
    } else if (!argument.empty() && argument.front() == '-') {
      std::cerr << "plusmc-wasm-adapt: unknown option " << argument << "\n";
      return 2;
    } else if (target.empty()) {
      target = argument;
    } else {
      std::cerr << "plusmc-wasm-adapt: this command takes one module or package\n";
      return 2;
    }
  }
  if (target.empty()) {
    usage();
    return 2;
  }

  const mc::wasm::ComponentReport report = mc::wasm::adapt_component(target);
  std::cout << (json ? mc::wasm::adapt_json(report) : mc::wasm::adapt_text(report));
  if (!report.ok) {
    std::cerr << "plusmc-wasm-adapt: " << report.error << "\n";
    return 1;
  }
  if (require_mapped && report.status != "mapped") {
    std::cerr << "plusmc-wasm-adapt: " << target << " is " << report.status << ", not mapped\n";
    return 1;
  }
  if (require_full && report.coverage != "full") {
    std::cerr << "plusmc-wasm-adapt: " << target << " has " << report.coverage
              << " coverage, not full\n";
    return 1;
  }
  return 0;
}
