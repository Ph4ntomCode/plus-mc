// SPDX-License-Identifier: GPL-3.0-or-later
//
// +MC vanilla-data — Phase 0 tool.
// Downloads a Minecraft server version (or reads a local jar) and extracts the
// vanilla data that the engine will be built on: version.json (protocol + data
// version), the datapack tree (data/minecraft/**), pack.mcmeta, and an index
// summary. Any version can be extracted and any two can be diffed later, which
// is the backbone of the version-support plan (docs/PLAN.md §5, §7).
//
// Dependencies: zlib (inflate). Downloads use libcurl when available, else the
// `curl` CLI. No other external deps (JSON + zip readers are in this repo).

#include "json.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* kManifestUrl =
    "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
constexpr const char* kUserAgent = "plusmc-vanilla-data/0.1 (+MC)";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

[[noreturn]] void fail(const std::string& msg) {
  std::cerr << "error: " << msg << "\n";
  std::exit(1);
}

void print_usage() {
  std::cout << "plusmc-vanilla-data — extract Mojang's vanilla data for a Minecraft version\n"
            << "\n"
            << "Usage:\n"
            << "  plusmc-vanilla-data [--version <id|latest>] [--out <dir>] [options]\n"
            << "  plusmc-vanilla-data --jar <path> [--out <dir>]\n"
            << "  plusmc-vanilla-data --list-versions\n"
            << "\n"
            << "Options:\n"
            << "  --version <id>     Minecraft version id (default: latest stable release)\n"
            << "  --out <dir>        Output root (default: data/vanilla). Version data is\n"
            << "                     written to <dir>/<version-id>/\n"
            << "  --jar <path>       Use a local server jar instead of downloading\n"
            << "  --keep-jar         Keep the downloaded jar (default: delete after extract)\n"
            << "  --refresh          Re-fetch the version manifest and per-version metadata\n"
            << "                     (cached under <out>/_cache); the jar cache is kept\n"
            << "  --list-versions    Print the latest releases known to Mojang and exit\n"
            << "  -h, --help         Show this help\n";
}

bool safe_path(const std::string& p) {
  // Guard against shell metacharacters (curl CLI fallback) and traversal. A
  // Windows path legitimately carries a drive colon and backslashes; both are
  // inert inside the fallback's quoted destination.
  for (char c : p) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '/' || c == '.' || c == '-' || c == '_';
#ifdef _WIN32
    ok = ok || c == ':' || c == '\\';
#endif
    if (!ok)
      return false;
  }
  return p.find("..") == std::string::npos;
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    fail("cannot read " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_file(const fs::path& path, const std::string& data) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    fail("cannot write " + path.string());
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!out)
    fail("write failed: " + path.string());
}

std::string now_utc() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

bool http_download(const std::string& url, const fs::path& dest) {
#ifdef HAVE_LIBCURL
  CURL* curl = curl_easy_init();
  if (!curl)
    return false;
  // `fs::path::c_str()` is `const wchar_t*` on Windows; the narrow string is
  // the only form `fopen` takes on every platform.
  FILE* f = std::fopen(dest.string().c_str(), "wb");
  if (!f) {
    curl_easy_cleanup(curl);
    return false;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
  CURLcode rc = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  std::fclose(f);
  if (rc != CURLE_OK) {
    std::error_code ec;
    fs::remove(dest, ec);
    return false;
  }
  return true;
#else
  // Fallback: curl CLI. dest has already been validated as shell-safe by callers.
  std::string cmd =
      "curl -fsSL --retry 3 --connect-timeout 20 -o \"" + dest.string() + "\" \"" + url + "\"";
  int rc = std::system(cmd.c_str());
  return rc == 0;
#endif
}

// ---------------------------------------------------------------------------
// Minimal zip reader (central directory + deflate via zlib)
// ---------------------------------------------------------------------------

uint16_t rd16(const std::vector<uint8_t>& b, size_t off) {
  // The | operands promote to int; cast the result back explicitly so no
  // -Wconversion warning (implicit int -> uint16_t narrowing) is emitted.
  return static_cast<uint16_t>(static_cast<uint16_t>(b[off]) |
                               (static_cast<uint16_t>(b[off + 1]) << 8));
}
uint32_t rd32(const std::vector<uint8_t>& b, size_t off) {
  return static_cast<uint32_t>(
      static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
      (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24));
}

class ZipReader {
public:
  struct Entry {
    std::string name;
    uint16_t method = 0;
    uint32_t csize = 0;
    uint32_t usize = 0;
    size_t local_offset = 0;
  };

  bool open(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
      return false;
    data_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (data_.size() < 22)
      return false;
    // Locate End Of Central Directory record (search backwards from the end).
    size_t eocd = static_cast<size_t>(-1);
    size_t lo = data_.size() > 22 + 65535 ? data_.size() - 22 - 65535 : 0;
    for (size_t i = data_.size() - 22; i >= lo; --i) {
      if (rd32(data_, i) == 0x06054b50u) {
        eocd = i;
        break;
      }
      if (i == 0)
        break;
    }
    if (eocd == static_cast<size_t>(-1))
      return false;
    size_t cd_offset = rd32(data_, eocd + 16);
    uint16_t total = rd16(data_, eocd + 10);
    size_t pos = cd_offset;
    for (uint16_t n = 0; n < total; ++n) {
      if (pos + 46 > data_.size() || rd32(data_, pos) != 0x02014b50u)
        return false;
      Entry e;
      e.method = rd16(data_, pos + 10);
      e.csize = rd32(data_, pos + 20);
      e.usize = rd32(data_, pos + 24);
      uint16_t name_len = rd16(data_, pos + 28);
      uint16_t extra_len = rd16(data_, pos + 30);
      uint16_t comment_len = rd16(data_, pos + 32);
      e.local_offset = rd32(data_, pos + 42);
      if (pos + 46 + name_len > data_.size())
        return false;
      e.name.assign(reinterpret_cast<const char*>(&data_[pos + 46]), name_len);
      entries_.push_back(std::move(e));
      pos += 46u + name_len + extra_len + comment_len;
    }
    return true;
  }

  const std::vector<Entry>& entries() const {
    return entries_;
  }

  const Entry* find_exact(const std::string& name) const {
    for (const auto& e : entries_)
      if (e.name == name)
        return &e;
    return nullptr;
  }

  // Extract a single entry's bytes.
  std::vector<uint8_t> extract(const Entry& e) const {
    if (e.local_offset + 30 > data_.size() || rd32(data_, e.local_offset) != 0x04034b50u) {
      throw std::runtime_error("zip: bad local header for " + e.name);
    }
    uint16_t name_len = rd16(data_, e.local_offset + 26);
    uint16_t extra_len = rd16(data_, e.local_offset + 28);
    size_t start = e.local_offset + 30u + name_len + extra_len;
    if (start + e.csize > data_.size())
      throw std::runtime_error("zip: truncated entry " + e.name);
    const uint8_t* src = &data_[start];
    if (e.method == 0) { // stored
      return std::vector<uint8_t>(src, src + e.csize);
    }
    if (e.method == 8) { // deflate — raw DEFLATE inside ZIP, no zlib wrapper
      z_stream zs{};
      if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
        throw std::runtime_error("zip: inflateInit failed for " + e.name);
      std::vector<uint8_t> out(e.usize ? e.usize : 1);
      zs.next_in = const_cast<Bytef*>(src);
      zs.avail_in = static_cast<uInt>(e.csize);
      zs.next_out = out.data();
      zs.avail_out = static_cast<uInt>(out.size());
      int rc = inflate(&zs, Z_FINISH);
      bool ok = (rc == Z_STREAM_END);
      uLong produced = zs.total_out;
      inflateEnd(&zs);
      if (!ok)
        throw std::runtime_error("zip: inflate failed for " + e.name);
      out.resize(produced);
      return out;
    }
    throw std::runtime_error("zip: unsupported method " + std::to_string(e.method) + " for " +
                             e.name);
  }

private:
  std::vector<uint8_t> data_;
  std::vector<Entry> entries_;
};

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------

void extract_jar(const fs::path& jar_path, const fs::path& out_dir, bool keep_jar,
                 bool report_removal = true) {
  ZipReader zip;
  if (!zip.open(jar_path))
    fail("cannot open " + jar_path.string() + " as a zip archive");

  // Modern (26.x+) server downloads are *bundler* jars: the real server jar is
  // nested under META-INF/versions/<version>/server-<version>.jar. If we find
  // one, recurse into it and extract from there.
  const ZipReader::Entry* nested = nullptr;
  for (const auto& e : zip.entries()) {
    if (e.name.rfind("META-INF/versions/", 0) == 0 && e.name.size() > 5 &&
        e.name.compare(e.name.size() - 4, 4, ".jar") == 0) {
      std::string base = e.name.substr(e.name.rfind('/') + 1);
      if (base.rfind("server", 0) == 0) {
        nested = &e;
        break;
      }
    }
  }
  if (nested) {
    std::cout << "Bundler jar detected; using nested " << nested->name << "\n";
    fs::path nested_path = jar_path.parent_path() / (jar_path.stem().string() + "-nested.jar");
    std::vector<uint8_t> nested_bytes = zip.extract(*nested);
    write_file(nested_path, std::string(nested_bytes.begin(), nested_bytes.end()));
    extract_jar(nested_path, out_dir, /*keep_jar=*/false, /*report_removal=*/false);
    std::error_code ec;
    fs::remove(nested_path, ec); // always clean up the nested copy
    return;
  }

  // version.json -> identify version, protocol, data version.
  const ZipReader::Entry* ver_entry = zip.find_exact("version.json");
  if (!ver_entry)
    fail("jar has no version.json (is this a Minecraft server jar?)");
  std::vector<uint8_t> ver_bytes = zip.extract(*ver_entry);
  pj::J ver = pj::J::parse(std::string(ver_bytes.begin(), ver_bytes.end()));

  std::string id =
      ver.find("id") && ver.at("id").is_string() ? ver.at("id").as_string() : "unknown";
  long long protocol = 0, world_version = 0;
  if (ver.find("protocol_version") && ver.at("protocol_version").is_number())
    protocol = ver.at("protocol_version").as_int();
  if (ver.find("world_version") && ver.at("world_version").is_number())
    world_version = ver.at("world_version").as_int();
  else if (ver.find("data_version") && ver.at("data_version").is_number())
    world_version = ver.at("data_version").as_int();

  // Output root: <out>/<version-id>/
  std::string dir_id;
  for (char c : id) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_';
    if (ok)
      dir_id += c;
  }
  if (dir_id.empty())
    dir_id = "unknown";
  fs::path dest = out_dir / dir_id;
  fs::create_directories(dest);

  std::cout << "Extracting " << id << " (protocol " << protocol << ", data version "
            << world_version << ") -> " << dest.string() << "\n";

  size_t file_count = 0;
  size_t total_bytes = 0;
  std::vector<std::string> top_levels;

  auto want = [](const std::string& name) {
    if (name.empty() || name.back() == '/')
      return false; // directory entries
    if (name == "version.json" || name == "pack.mcmeta")
      return true;
    return name.rfind("data/", 0) == 0; // any datapack namespace
  };

  auto sanitize_rel = [](const std::string& name) {
    // Jar names use '/'. Reject traversal/absolutes; keep it simple and safe.
    if (name.find("..") != std::string::npos || name.empty() || name[0] == '/')
      throw std::runtime_error("zip: unsafe entry name '" + name + "'");
    return name;
  };

  for (const auto& e : zip.entries()) {
    std::string rel;
    try {
      rel = sanitize_rel(e.name);
    } catch (const std::exception& ex) {
      std::cerr << "warning: skipping " << ex.what() << "\n";
      continue;
    }
    size_t slash = rel.find('/');
    std::string top = slash == std::string::npos ? rel : rel.substr(0, slash);
    if (std::find(top_levels.begin(), top_levels.end(), top) == top_levels.end())
      top_levels.push_back(top);

    if (!want(rel))
      continue;
    std::vector<uint8_t> bytes = zip.extract(e);
    write_file(dest / rel, std::string(bytes.begin(), bytes.end()));
    ++file_count;
    total_bytes += bytes.size();
  }

  // index.json summary.
  std::string idx = "{\n  \"gameVersion\": \"" + id + "\",\n";
  idx += "  \"protocol\": " + std::to_string(protocol) + ",\n";
  idx += "  \"dataVersion\": " + std::to_string(world_version) + ",\n";
  idx += "  \"extractedFiles\": " + std::to_string(file_count) + ",\n";
  idx += "  \"extractedBytes\": " + std::to_string(total_bytes) + ",\n";
  idx += "  \"generatedAtUtc\": \"" + now_utc() + "\"\n";
  idx += "}\n";
  write_file(dest / "index.json", idx);

  std::cout << "Done: " << file_count << " files (" << total_bytes << " bytes) extracted.\n";
  std::cout << "Top-level jar entries: ";
  for (size_t i = 0; i < top_levels.size(); ++i) {
    if (i)
      std::cout << ", ";
    std::cout << top_levels[i];
  }
  std::cout << "\n";

  if (!keep_jar) {
    std::error_code ec;
    fs::remove(jar_path, ec);
    if (!ec && report_removal)
      std::cout << "Removed downloaded jar (use --keep-jar to keep).\n";
  }
}

} // namespace

int main(int argc, char** argv) {
  std::string version = "latest";
  std::string out_str = "data/vanilla";
  std::optional<fs::path> jar;
  bool list_versions = false;
  bool keep_jar = false;
  bool refresh = false;

  std::vector<std::string> args(argv + 1, argv + argc);
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= args.size())
        fail(std::string(what) + " requires a value");
      return args[++i];
    };
    if (a == "--version") {
      version = next("--version");
    } else if (a == "--out") {
      out_str = next("--out");
    } else if (a == "--jar") {
      jar = fs::path(next("--jar"));
    } else if (a == "--keep-jar") {
      keep_jar = true;
    } else if (a == "--refresh") {
      refresh = true;
    } else if (a == "--list-versions") {
      list_versions = true;
    } else if (a == "-h" || a == "--help") {
      print_usage();
      return 0;
    } else {
      fail("unknown argument '" + a + "' (see --help)");
    }
  }

  if (!safe_path(out_str))
    fail("--out path contains unsafe characters: '" + out_str + "'");
  fs::path out_root(out_str);

  // ---- Local jar mode -----------------------------------------------------
  if (jar) {
    if (!fs::exists(*jar))
      fail("jar not found: " + jar->string());
    extract_jar(*jar, out_root, /*keep_jar=*/true);
    return 0;
  }

  // ---- Manifest -----------------------------------------------------------
  fs::path cache_dir = out_root / "_cache";
  fs::create_directories(cache_dir);
  fs::path manifest_path = cache_dir / "version_manifest_v2.json";
  if (refresh) {
    std::error_code ec;
    fs::remove(manifest_path, ec); // manifest drives what "latest" means
  }
  if (!fs::exists(manifest_path)) {
    std::cout << "Fetching Mojang version manifest...\n";
    if (!http_download(kManifestUrl, manifest_path))
      fail("failed to download the Mojang version manifest (network down? use --jar <local.jar>)");
  }
  pj::J manifest = pj::J::parse(read_file(manifest_path));

  if (list_versions) {
    const pj::J* latest = manifest.find("latest");
    if (latest) {
      std::cout << "Latest release:  "
                << (latest->find("release") ? latest->at("release").as_string() : "?") << "\n";
      std::cout << "Latest snapshot: "
                << (latest->find("snapshot") ? latest->at("snapshot").as_string() : "?") << "\n";
    }
    const pj::J& versions = manifest.at("versions");
    std::cout << "\nMost recent versions (newest first):\n";
    size_t shown = 0;
    for (const auto& v : versions.array_items()) {
      if (shown >= 15)
        break;
      std::string id = v.at("id").as_string();
      std::string type = v.at("type").as_string();
      std::cout << "  " << id << "  (" << type << ")\n";
      ++shown;
    }
    return 0;
  }

  // ---- Resolve requested version ------------------------------------------
  std::string want = version;
  if (want == "latest" || want == "latest-release") {
    want = manifest.at("latest").at("release").as_string();
  }
  const pj::J* chosen = nullptr;
  for (const auto& v : manifest.at("versions").array_items()) {
    if (v.at("id").as_string() == want) {
      chosen = &v;
      break;
    }
  }
  if (!chosen)
    fail("unknown version '" + want + "' (see --list-versions)");

  std::string id = chosen->at("id").as_string();
  std::string dir_id;
  for (char c : id)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
        c == '-' || c == '_')
      dir_id += c;
  if (dir_id.empty())
    dir_id = "unknown";

  // Manifest entries no longer embed downloads; each points at a per-version
  // metadata JSON (piston-meta) that holds the download URLs and checksums.
  std::string meta_url = chosen->at("url").as_string();
  fs::path meta_path = cache_dir / (dir_id + ".meta.json");
  if (refresh) {
    std::error_code ec;
    fs::remove(meta_path, ec); // metadata may drift (new build of same id)
  }
  if (!fs::exists(meta_path)) {
    std::cout << "Fetching version metadata for " << id << "...\n";
    if (!http_download(meta_url, meta_path))
      fail("failed to download metadata for " + id);
  }
  pj::J meta = pj::J::parse(read_file(meta_path));
  if (!meta.find("downloads") || !meta.at("downloads").find("server"))
    fail("metadata for " + id + " has no server download (unsupported old/odd version?)");
  const pj::J& dl = meta.at("downloads").at("server");
  std::string url = dl.at("url").as_string();
  std::string sha1 = dl.has("sha1") ? dl.at("sha1").as_string() : "";
  if (meta.find("protocol_version") && meta.at("protocol_version").is_number())
    std::cout << "Mojang metadata: protocol " << meta.at("protocol_version").as_int() << "\n";

  fs::path jar_path = cache_dir / (dir_id + ".jar");
  if (!fs::exists(jar_path)) {
    std::cout << "Downloading " << id << " server jar...\n";
    std::cout << "  sha1: " << sha1 << "\n";
    if (!http_download(url, jar_path))
      fail("download failed for " + id);
  } else {
    std::cout << "Using cached jar: " << jar_path.string() << "\n";
  }

  extract_jar(jar_path, out_root, keep_jar);
  return 0;
}
