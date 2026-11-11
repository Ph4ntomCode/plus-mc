// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 8, slice 1: the WASM plugin package gate.
//
// A `.wasm` file is an untrusted binary from a third party, so nothing here
// interprets or executes it. It reads the binary with the same discipline as
// the protocol framing and the JAR reader — every length bounds-checked, every
// count capped — and answers two questions a server owner actually has:
//
//   * is this a well-formed core WebAssembly module or a component?
//   * can the pinned +MC host surface satisfy every import it declares?
//
// The second answer is the sandbox contract: a module that imports `wasi:*`,
// a memory/table/global, or a host function that is not in the table is
// `blocked` with the exact import named, not "probably fine". The runtime that
// actually calls the module is slice 2; this gate is what stops an unhostable
// module from reaching it.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mc::wasm {

// The pinned host namespace and functions a WASM plugin may import. A name not
// in this table is refused, so a typo cannot link against a host that happens
// to have a symbol with a similar name.
inline constexpr const char* kHostNamespace = "plusmc:host";

// One entry of the pinned host surface. `signature` is the exact C ABI the
// runtime links against, in the same text form `ImportRef::signature` uses.
// There is one table, not two: the gate approves a name from it and the
// runtime provides that name from it, so an approved import can never be one
// the runtime does not implement (or implements with another signature).
struct HostFunctionRef {
  const char* name;
  const char* signature;
};

const std::vector<HostFunctionRef>& host_surface();
bool known_host_function(const std::string& name);

struct ImportRef {
  std::string module;
  std::string name;
  // "function", "table", "memory", "global" or "tag".
  std::string kind;
  // Empty for a non-function import.
  std::string signature;
};

struct ExportRef {
  std::string name;
  std::string kind;
  std::uint32_t index = 0;
};

struct SectionRef {
  std::uint8_t id = 0;
  std::uint64_t size = 0;
};

struct ModuleInfo {
  std::string file;
  std::string sha256;
  std::uint64_t size = 0;
  // A component (the `wasm32-wasip2` shape Pumpkin plugins use) has a different
  // binary layer; slice 1 validates its header and section table only.
  bool component = false;
  std::uint32_t version = 0;
  std::uint32_t type_count = 0;
  std::uint32_t function_count = 0;
  std::uint32_t code_count = 0;
  std::uint32_t table_count = 0;
  std::uint32_t memory_count = 0;
  std::uint32_t global_count = 0;
  std::uint32_t tag_count = 0;
  std::vector<SectionRef> sections;
  std::vector<ImportRef> imports;
  std::vector<ExportRef> exports;
  std::vector<std::string> notes;
};

struct ModuleReport {
  bool ok = false;
  std::string error;
  ModuleInfo module;
};

// Reads one `.wasm` file. A malformed binary (bad magic, unsupported version,
// a section past the end, an unknown opcode-bearing section, a code count that
// disagrees with the function section) is refused with the byte offset named;
// it is never partially reported as hostable.
ModuleReport inspect_module(const std::filesystem::path& path);

struct WasmPackage {
  std::filesystem::path dir;
  std::string name;
  std::string version;
  std::string module;
  unsigned api = 0;
};

struct PackageReport {
  bool ok = false;
  std::string error;
  WasmPackage package;
  ModuleInfo module;
  // "hostable" (every import is provided), "blocked" (a listed import is
  // refused), "component" (a component layer, not hosted by this build) or
  // "unreadable" (the binary or manifest did not parse).
  std::string status;
  std::vector<std::string> reasons;
};

// `path` may be a directory holding `+mc-wasm.toml` plus the module it names,
// or a bare `.wasm` file (then the package fields are empty and the status is
// decided from the module alone).
PackageReport inspect_wasm(const std::filesystem::path& path);

std::string package_text(const PackageReport& report);
std::string package_json(const PackageReport& report);

} // namespace mc::wasm
