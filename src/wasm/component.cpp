// SPDX-License-Identifier: GPL-3.0-or-later
//
// The component layer behind `plusmc-wasm-adapt`. A component's import and
// export sections use the component-model encoding, not a core module's: the
// name is a `nameattributes` prefix plus a length-prefixed UTF-8 string, the
// external type is a component sort, and an export carries an optional
// ascribed type. This file walks exactly that structure, with the same
// bounded-reader discipline as the package gate, and maps each imported
// interface onto the pinned table in `component.h`.
//
// It does not resolve type indices, decode instance types or execute a module.
// A name is evidence of an import, not of a link: canonical-ABI lifting and
// lowering are the runtime's work.

#include "wasm/component.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>
#include <utility>

#include "converter/report_json.h"
#include "wasm/module.h"

namespace mc::wasm {

namespace detail = mc::converter::detail;

namespace {

constexpr std::size_t kMaxModuleBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxSections = 4096;
constexpr std::size_t kMaxEntries = 100000;
constexpr std::size_t kMaxNameBytes = 4096;
constexpr std::size_t kMaxDisplayedEntries = 1000;

class Reader {
public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  bool u8(std::uint8_t* out) {
    if (position_ >= size_)
      return false;
    *out = data_[position_++];
    return true;
  }

  bool peek(std::uint8_t* out) const {
    if (position_ >= size_)
      return false;
    *out = data_[position_];
    return true;
  }

  bool u32(std::uint32_t* out) {
    std::uint32_t result = 0;
    for (unsigned int shift = 0; shift < 35; shift += 7) {
      std::uint8_t byte = 0;
      if (!u8(&byte))
        return false;
      if (shift == 28 && (byte & 0xf0) != 0)
        return false; // more than 32 bits of LEB128
      result |= static_cast<std::uint32_t>(byte & 0x7f) << shift;
      if ((byte & 0x80) == 0) {
        *out = result;
        return true;
      }
    }
    return false;
  }

  bool name(std::string* out) {
    std::uint32_t length = 0;
    if (!u32(&length))
      return false;
    if (length > kMaxNameBytes || length > remaining())
      return false;
    const char* start = reinterpret_cast<const char*>(data_ + position_);
    if (!valid_utf8(start, length))
      return false;
    out->assign(start, length);
    position_ += length;
    return true;
  }

  bool skip(std::size_t count) {
    if (count > remaining())
      return false;
    position_ += count;
    return true;
  }

  std::size_t remaining() const {
    return size_ - position_;
  }

  bool sub(std::uint32_t length, Reader* out) const {
    if (length > remaining())
      return false;
    *out = Reader(data_ + position_, length);
    return true;
  }

  void advance(std::uint32_t length) {
    position_ += length;
  }

private:
  static bool valid_utf8(const char* bytes, std::size_t length) {
    std::size_t i = 0;
    while (i < length) {
      const unsigned char c = static_cast<unsigned char>(bytes[i]);
      if (c == 0)
        return false;
      std::size_t follow = 0;
      if (c < 0x80) {
        ++i;
        continue;
      } else if ((c & 0xe0) == 0xc0) {
        follow = 1;
        if (c < 0xc2)
          return false; // overlong
      } else if ((c & 0xf0) == 0xe0) {
        follow = 2;
      } else if ((c & 0xf8) == 0xf0) {
        follow = 3;
        if (c > 0xf4)
          return false;
      } else {
        return false;
      }
      if (i + follow >= length)
        return false;
      for (std::size_t j = 1; j <= follow; ++j) {
        if ((static_cast<unsigned char>(bytes[i + j]) & 0xc0) != 0x80)
          return false;
      }
      i += follow + 1;
    }
    return true;
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t position_ = 0;
};

bool read_file(const std::filesystem::path& path, std::size_t cap, std::vector<std::uint8_t>* out,
               std::string* error) {
  std::error_code status_error;
  const std::uintmax_t size = std::filesystem::file_size(path, status_error);
  if (status_error) {
    *error = "cannot read " + path.generic_string() + ": " + status_error.message();
    return false;
  }
  if (size > cap) {
    *error = path.generic_string() + " is larger than the " + std::to_string(cap) + "-byte limit";
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    *error = "cannot read " + path.generic_string();
    return false;
  }
  out->resize(static_cast<std::size_t>(size));
  if (!out->empty())
    file.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(out->size()));
  if (!file && !out->empty()) {
    *error = "cannot read " + path.generic_string();
    return false;
  }
  return true;
}

// The component-model `externname`: a leading byte selecting plain or
// attributed form, a length-prefixed UTF-8 name, and for the attributed form
// an `implements` / `version-suffix` / `external-id` list.
struct ExternName {
  std::string plain;
  std::string implements;
  std::string suffix;
};

bool read_extern_name(Reader* reader, ExternName* out, std::string* error) {
  std::uint8_t lead = 0;
  if (!reader->u8(&lead)) {
    *error = "a component name runs past the section";
    return false;
  }
  if (lead > 0x02) {
    *error = "a component name uses unknown kind " + std::to_string(lead);
    return false;
  }
  if (!reader->name(&out->plain)) {
    *error = "a component name runs past the section";
    return false;
  }
  if (lead != 0x02)
    return true;
  std::uint32_t attributes = 0;
  if (!reader->u32(&attributes) || attributes > 3) {
    *error = "a component name carries an implausible attribute count";
    return false;
  }
  for (std::uint32_t i = 0; i < attributes; ++i) {
    std::uint8_t kind = 0;
    std::string value;
    if (!reader->u8(&kind) || !reader->name(&value)) {
      *error = "a component name attribute runs past the section";
      return false;
    }
    if (kind == 0x00) {
      out->implements = value;
    } else if (kind == 0x01) {
      out->suffix = value;
    } else if (kind != 0x02) {
      *error = "a component name attribute uses unknown kind " + std::to_string(kind);
      return false;
    }
  }
  return true;
}

std::string with_suffix(const std::string& name, const std::string& suffix) {
  if (suffix.empty() || name.find('@') == std::string::npos)
    return name;
  return name + suffix;
}

// An `implements` name is the interface actually imported; the plain name is
// the local binding. The table matches interfaces, so it uses the former.
std::string import_name(const ExternName& name) {
  return name.implements.empty() ? with_suffix(name.plain, name.suffix)
                                 : with_suffix(name.implements, name.suffix);
}

std::string export_name(const ExternName& name) {
  return with_suffix(name.plain, name.suffix);
}

bool read_external_kind(Reader* reader, std::string* kind, std::string* error) {
  std::uint8_t byte = 0;
  if (!reader->u8(&byte)) {
    *error = "a component external kind runs past the section";
    return false;
  }
  switch (byte) {
  case 0x00: {
    std::uint8_t tag = 0;
    std::uint32_t index = 0;
    if (!reader->u8(&tag) || tag != 0x11 || !reader->u32(&index)) {
      *error = "a core-module reference is malformed";
      return false;
    }
    *kind = "module";
    return true;
  }
  case 0x01:
    *kind = "func";
    return true;
  case 0x02:
    *kind = "value";
    return true;
  case 0x03:
    *kind = "type";
    return true;
  case 0x04:
    *kind = "component";
    return true;
  case 0x05:
    *kind = "instance";
    return true;
  default:
    *error = "unknown component external kind " + std::to_string(byte);
    return false;
  }
}

bool skip_valtype(Reader* reader, std::string* error) {
  std::uint8_t byte = 0;
  if (!reader->peek(&byte)) {
    *error = "a component value type runs past the section";
    return false;
  }
  // Component primitive value types are the negative SLEB128 opcodes
  // 0x7f..0x64; everything else in this position is a type index.
  if (byte >= 0x64 && byte <= 0x7f) {
    reader->skip(1);
    return true;
  }
  std::uint32_t index = 0;
  if (!reader->u32(&index)) {
    *error = "a component value type runs past the section";
    return false;
  }
  return true;
}

bool read_externtype(Reader* reader, std::string* kind, std::string* error) {
  std::string sort;
  if (!read_external_kind(reader, &sort, error))
    return false;
  if (sort == "value") {
    if (!skip_valtype(reader, error))
      return false;
  } else if (sort == "type") {
    std::uint8_t bounds = 0;
    if (!reader->u8(&bounds)) {
      *error = "a component type bound runs past the section";
      return false;
    }
    if (bounds == 0x00) {
      std::uint32_t index = 0;
      if (!reader->u32(&index)) {
        *error = "a component type bound runs past the section";
        return false;
      }
    } else if (bounds != 0x01) {
      *error = "a component type bound uses unknown kind " + std::to_string(bounds);
      return false;
    }
  } else {
    std::uint32_t index = 0;
    if (!reader->u32(&index)) {
      *error = "a component external type runs past the section";
      return false;
    }
  }
  *kind = sort;
  return true;
}

bool parse_imports(Reader reader, ComponentReport* report, std::set<std::string>* names,
                   std::string* error) {
  std::uint32_t count = 0;
  if (!reader.u32(&count)) {
    *error = "the component import section has no count";
    return false;
  }
  if (count > kMaxEntries) {
    *error =
        "the component import section claims more than " + std::to_string(kMaxEntries) + " imports";
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    ExternName name;
    if (!read_extern_name(&reader, &name, error))
      return false;
    std::string kind;
    if (!read_externtype(&reader, &kind, error))
      return false;
    ComponentImportRef import;
    import.name = import_name(name);
    import.kind = std::move(kind);
    if (!names->insert(import.name).second) {
      *error = "the import \"" + import.name + "\" is declared twice";
      return false;
    }
    report->imports.push_back(std::move(import));
  }
  return true;
}

bool parse_exports(Reader reader, ComponentReport* report, std::string* error) {
  std::uint32_t count = 0;
  if (!reader.u32(&count)) {
    *error = "the component export section has no count";
    return false;
  }
  if (count > kMaxEntries) {
    *error =
        "the component export section claims more than " + std::to_string(kMaxEntries) + " exports";
    return false;
  }
  std::set<std::string> names;
  for (std::uint32_t index = 0; index < count; ++index) {
    ExternName name;
    if (!read_extern_name(&reader, &name, error))
      return false;
    std::string kind;
    if (!read_external_kind(&reader, &kind, error))
      return false;
    std::uint32_t item_index = 0;
    if (!reader.u32(&item_index)) {
      *error = "a component export runs past the section";
      return false;
    }
    std::uint8_t ascribed = 0;
    if (!reader.u8(&ascribed)) {
      *error = "a component export runs past the section";
      return false;
    }
    if (ascribed == 0x01) {
      std::string ignored;
      if (!read_externtype(&reader, &ignored, error))
        return false;
    } else if (ascribed != 0x00) {
      *error = "a component export uses unknown optional-type kind " + std::to_string(ascribed);
      return false;
    }
    ComponentExportRef export_ref;
    export_ref.name = export_name(name);
    export_ref.kind = std::move(kind);
    export_ref.index = item_index;
    if (!names.insert(export_ref.name).second) {
      *error = "the export \"" + export_ref.name + "\" is declared twice";
      return false;
    }
    report->exports.push_back(std::move(export_ref));
  }
  return true;
}

bool parse_component_layer(const std::vector<std::uint8_t>& bytes, ComponentReport* report,
                           std::string* error) {
  if (bytes.size() < 8 || bytes[0] != 0x00 || bytes[1] != 0x61 || bytes[2] != 0x73 ||
      bytes[3] != 0x6d) {
    *error = "not a WebAssembly binary (bad magic)";
    return false;
  }
  const bool component =
      bytes[4] == 0x0d && bytes[5] == 0x00 && bytes[6] == 0x01 && bytes[7] == 0x00;
  if (!component) {
    *error = "the module is not a component (its header is a core module)";
    return false;
  }

  Reader reader(bytes.data() + 8, bytes.size() - 8);
  std::set<std::string> import_names;
  std::size_t sections = 0;
  while (reader.remaining() != 0) {
    if (++sections > kMaxSections) {
      *error = "more than " + std::to_string(kMaxSections) + " sections";
      return false;
    }
    std::uint8_t id = 0;
    std::uint32_t length = 0;
    if (!reader.u8(&id) || !reader.u32(&length)) {
      *error = "a component section header runs past the file";
      return false;
    }
    if (length > reader.remaining()) {
      *error = "component section " + std::to_string(id) + " claims " + std::to_string(length) +
               " bytes but only " + std::to_string(reader.remaining()) + " remain";
      return false;
    }
    Reader payload(nullptr, 0);
    if (!reader.sub(length, &payload)) {
      *error = "component section " + std::to_string(id) + " runs past the file";
      return false;
    }
    // Component sections are definitions, not a core module's singleton
    // table: any order and repeated ids are legal (several core modules,
    // several import sections). Only the bounds are shared with core modules.
    if (id > 12) {
      *error = "unknown component section id " + std::to_string(id);
      return false;
    }
    if (id == 10) {
      if (!parse_imports(payload, report, &import_names, error))
        return false;
    } else if (id == 11) {
      if (!parse_exports(payload, report, error))
        return false;
    }
    reader.advance(length);
  }
  return true;
}

bool has_export(const ComponentReport& report, const std::string& name, const std::string& kind) {
  return std::any_of(report.exports.begin(), report.exports.end(),
                     [&](const ComponentExportRef& export_ref) {
                       return export_ref.name == name && export_ref.kind == kind;
                     });
}

void classify(ComponentReport* report) {
  bool blocked = false;
  bool partial = false;
  bool generic = false;
  for (ComponentImportRef& import : report->imports) {
    if (import.kind == "type") {
      // A component that uses a type from another interface imports that type
      // directly. It has no call to bind and does not need a host value.
      import.pinned = true;
      continue;
    }
    if (import.kind != "instance") {
      blocked = true;
      report->reasons.push_back("imports " + import.name + " as a " + import.kind +
                                "; the adapter hosts interface instances and types only");
      continue;
    }
    import.pinned = true;
    const PinnedInterfaceRef* pinned = find_pinned_interface(import.name);
    if (pinned == nullptr) {
      // The compiled-core linker binds every core import of an unlisted
      // interface to a trapping host import. It is a real binding, not a
      // guess at Pumpkin semantics, so the interface is mapped rather than
      // refused.
      generic = true;
      // A generic stub makes the component loadable, but no member of this
      // interface has Pumpkin semantics. Coverage is therefore partial, never
      // full: `--require-full` must not certify a stubbed surface.
      partial = true;
      import.backed.push_back("all members -> " + std::string(kHostNamespace) + "/component_stub");
      continue;
    }
    for (std::size_t i = 0; i < pinned->member_count; ++i) {
      const PinnedMemberRef& member = pinned->members[i];
      const std::string declaration = std::string(member.member) + "  " + member.signature;
      if (member.host != nullptr && member.host[0] != '\0') {
        import.backed.push_back(declaration + " -> " + kHostNamespace + "/" + member.host);
      } else {
        import.unbacked.push_back(declaration);
        partial = true;
      }
    }
  }

  if (!has_export(*report, "on-load", "func")) {
    blocked = true;
    report->reasons.push_back(
        "does not export on-load; the host drives a plugin through that lifecycle callback");
  }

  if (has_export(*report, "init-plugin", "func"))
    report->notes.push_back("exports init-plugin (Pumpkin's constructor hook)");
  if (generic)
    report->notes.push_back(
        "unlisted interfaces are linked member-for-member to a trapping host import; "
        "the plugin loads, but calling an unbacked member traps instead of returning data");
  report->notes.push_back(
      "mapped is an interface verdict, not a run: the service must still link and execute the "
      "lifecycle before a plugin is trusted");
  report->notes.push_back(
      "Pumpkin v0.2 exports on-load and on-unload as async func(context: context) -> "
      "result<_, string>; +MC's zero-argument lifecycle lift is a different ABI");
  report->notes.push_back(
      "resource methods live inside an instance type and carry an implicit self handle; the "
      "table records the WIT signature and a capability, not a linked signature");

  if (blocked) {
    report->status = "blocked";
    report->coverage.clear();
  } else {
    report->status = "mapped";
    report->coverage = partial ? "partial" : "full";
  }
}

} // namespace

ComponentReport adapt_component(const std::filesystem::path& path) {
  ComponentReport report;
  report.file = path.generic_string();

  const PackageReport gate = inspect_wasm(path);
  report.package_name = gate.package.name;
  report.package_version = gate.package.version;
  report.package_api = gate.package.api;
  report.file = gate.module.file.empty() ? report.file : gate.module.file;
  report.sha256 = gate.module.sha256;
  report.size = gate.module.size;
  report.version = gate.module.version;
  report.component = gate.module.component;
  if (!gate.ok) {
    report.status = "unreadable";
    report.error = gate.error;
    return report;
  }
  if (!report.component) {
    report.ok = true;
    report.status = "not-component";
    report.reasons.push_back("the file is a core module; `inspect` and `run` are its commands");
    return report;
  }

  std::vector<std::uint8_t> bytes;
  std::string error;
  if (!read_file(report.file, kMaxModuleBytes, &bytes, &error)) {
    report.status = "unreadable";
    report.error = error;
    return report;
  }
  if (!parse_component_layer(bytes, &report, &error)) {
    report.status = "unreadable";
    report.error = report.file + ": " + error;
    report.imports.clear();
    report.exports.clear();
    report.notes.clear();
    report.reasons.clear();
    return report;
  }
  classify(&report);
  report.ok = true;
  return report;
}

std::string adapt_text(const ComponentReport& report) {
  std::string out = "+MC wasm component adapter\n";
  if (!report.package_name.empty()) {
    out += "package: " + report.package_name + " " + report.package_version + ", api " +
           std::to_string(report.package_api) + "\n";
  }
  if (!report.file.empty()) {
    out += "module: " + report.file + "  " + std::to_string(report.size) + " bytes  sha256 " +
           report.sha256 + "\n";
  }
  out += std::string("kind: ") + (report.component ? "component" : "core module") + "\n";
  out += "status: " + (report.status.empty() ? std::string("unreadable") : report.status) + "\n";
  if (!report.coverage.empty())
    out += "coverage: " + report.coverage + "\n";
  if (!report.error.empty())
    out += "error: " + report.error + "\n";

  out += "imports: " + std::to_string(report.imports.size()) + "\n";
  const std::size_t shown_imports = std::min(report.imports.size(), kMaxDisplayedEntries);
  for (std::size_t i = 0; i < shown_imports; ++i) {
    const ComponentImportRef& import = report.imports[i];
    out += "  " + import.name + "  " + import.kind + "  " +
           (import.pinned ? "pinned" : "unpinned") + "\n";
    for (const std::string& backed : import.backed)
      out += "    backed: " + backed + "\n";
    for (const std::string& unbacked : import.unbacked)
      out += "    unbacked: " + unbacked + "\n";
  }
  if (shown_imports < report.imports.size())
    out += "  ... and " + std::to_string(report.imports.size() - shown_imports) + " more\n";

  out += "exports: " + std::to_string(report.exports.size()) + "\n";
  const std::size_t shown_exports = std::min(report.exports.size(), kMaxDisplayedEntries);
  for (std::size_t i = 0; i < shown_exports; ++i) {
    const ComponentExportRef& export_ref = report.exports[i];
    out += "  " + export_ref.name + "  " + export_ref.kind + " #" +
           std::to_string(export_ref.index) + "\n";
  }
  if (shown_exports < report.exports.size())
    out += "  ... and " + std::to_string(report.exports.size() - shown_exports) + " more\n";
  if (report.component) {
    out += std::string("lifecycle: ") +
           (has_export(report, "on-load", "func") ? "on-load exported" : "on-load missing") + "\n";
  }

  if (!report.reasons.empty()) {
    out += "reasons:\n";
    for (const std::string& reason : report.reasons)
      out += "  - " + reason + "\n";
  }
  if (!report.notes.empty()) {
    out += "notes:\n";
    for (const std::string& note : report.notes)
      out += "  - " + note + "\n";
  }
  return out;
}

std::string adapt_json(const ComponentReport& report) {
  std::string out = "{\"tool\":\"plusmc-wasm\",\"schema\":1,\"mode\":\"adapt\",\"ok\":";
  out += report.ok ? "true" : "false";
  out += ",\"status\":";
  out += detail::json_quote(report.status.empty() ? "unreadable" : report.status);
  out += ",\"coverage\":";
  out += detail::json_quote(report.coverage);
  out += ",\"package\":{\"name\":";
  out += detail::json_quote(report.package_name);
  out += ",\"version\":";
  out += detail::json_quote(report.package_version);
  out += ",\"api\":" + std::to_string(report.package_api) + "}";
  out += ",\"module\":{\"file\":";
  out += detail::json_quote(report.file);
  out += ",\"sha256\":";
  out += detail::json_quote(report.sha256);
  out += ",\"size\":" + std::to_string(report.size);
  out += ",\"kind\":";
  out += detail::json_quote(report.component ? "component" : "core");
  out += ",\"version\":" + std::to_string(report.version) + "}";
  out += ",\"imports\":[";
  for (std::size_t i = 0; i < report.imports.size(); ++i) {
    if (i != 0)
      out += ",";
    const ComponentImportRef& import = report.imports[i];
    out += "{\"name\":" + detail::json_quote(import.name);
    out += ",\"kind\":" + detail::json_quote(import.kind);
    out += ",\"pinned\":";
    out += import.pinned ? "true" : "false";
    out += ",\"backed\":";
    detail::json_list(&out, import.backed);
    out += ",\"unbacked\":";
    detail::json_list(&out, import.unbacked);
    out += "}";
  }
  out += "],\"exports\":[";
  for (std::size_t i = 0; i < report.exports.size(); ++i) {
    if (i != 0)
      out += ",";
    out += "{\"name\":" + detail::json_quote(report.exports[i].name);
    out += ",\"kind\":" + detail::json_quote(report.exports[i].kind);
    out += ",\"index\":" + std::to_string(report.exports[i].index) + "}";
  }
  out += "],\"reasons\":";
  detail::json_list(&out, report.reasons);
  out += ",\"notes\":";
  detail::json_list(&out, report.notes);
  if (!report.error.empty())
    out += ",\"error\":" + detail::json_quote(report.error);
  out += "}\n";
  return out;
}

} // namespace mc::wasm
