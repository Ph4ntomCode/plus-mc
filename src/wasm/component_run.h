// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 8, slice 3: executing the component layer.
//
// A component's exported `on-load` is a `canon lift` of a core function, not a
// core function itself. This entry point hosts the linked shape: one nested
// core module whose imports are lowered `plusmc:host` calls (or members of the
// tracked `pumpkin:plugin/*@0.2.0` interfaces), bound back to the module
// through a synthetic instance and its instantiation, with `on-load` /
// `on-enable` / `on-disable` lifted from core aliases of the instance. A UTF-8
// string parameter is a (pointer, length) pair read from the module's memory 0;
// a lifted string result is read from a (pointer, length) return area, recorded
// in the report, and the lift's post-return function is called. A lowered
// string result is written into a return area the host allocates through the
// module's `cabi_realloc`; a lowered list<string> result uses the same path,
// with a canonical (pointer, length) array and an 8-byte pair per string, and a
// lowered two-string record result uses two 8-byte pairs in a 16-byte area.
// Pumpkin v0.2's resource handle, async
// `on-load`/`on-unload` and `task.return` are hosted. A compiler-produced
// component whose lifecycle is buried among linker modules or uses the callback
// `(i32)->i32` shape takes the explicit compiled-core path: it selects the
// lifecycle core module, binds known hosts, and gives every other import a host
// import that traps by name instead of returning a fabricated result.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "wasm/runtime.h"

namespace mc::wasm {

// The linked shape of one component, ready for the runtime to instantiate.
// `bytes` is the nested core module, `calls` is the lifecycle order, and
// `bindings` maps the module's imports back to host functions. A server keeps
// this prepared once and then drives `Instance::create_component`.
struct PreparedComponent {
  std::vector<std::uint8_t> bytes;
  std::vector<ExportCall> calls;
  std::vector<CoreImportBinding> bindings;
  // A compiler-produced `handle-event` with the flat tick-start shape. The
  // component still loads without one; it simply receives no gameplay events.
  bool has_tick_callback = false;
  std::uint32_t tick_callback = 0;
  std::uint32_t tick_event_type = 40;
  std::uint32_t tick_event_case = 40;
  // Pumpkin v0.2's asynchronous `handle-task` callback. A synchronous
  // compiler shape `(i32,i32)->()` is hosted too; a callback ABI that returns
  // one status is called for its effects and the status is ignored.
  bool has_task_callback = false;
  std::uint32_t task_callback = 0;
  bool task_callback_status = false;
  bool task_callback_async = false;
  std::string file;
  std::string sha256;
  std::uint64_t size = 0;
};

// Parses and links `path` without executing it. Returns false with `*error` for
// a gate, parse, shape or link refusal.
bool prepare_component(const std::filesystem::path& path, PreparedComponent* out,
                       std::string* error);

// Runs `path` if it is a component in the hosted shape: exactly one core
// module, imports linked through `canon lower` to `plusmc:host`, and
// `on-load` (required) plus the v0.2 async lifecycle or `on-enable` /
// `on-disable` exported as canon lifts. The report has the same fields as a
// core run, so `run_report_text` and `run_report_json` describe both.
RunReport run_component(const std::filesystem::path& path, const Limits& limits, Host& host);

} // namespace mc::wasm
