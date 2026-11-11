// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/backup.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

#include "core/log.h"
#include "world/directory_lock.h"

namespace mc::server {

namespace fs = std::filesystem;

namespace {

// The manifest is what makes a directory recognisably *ours*: rotation and the
// restore drill both refuse to touch anything without it, so a host that keeps
// its own notes (or another server's dump) in the same directory keeps them.
constexpr const char* kManifestName = "backup.json";
constexpr const char* kManifestMarker = "\"plusmc-backup\"";
// A snapshot in progress. Renamed into place on success, removed on failure;
// `list_backups` skips anything with this suffix.
constexpr const char* kPartialSuffix = ".partial";

std::string path_error(const std::string& what, const fs::path& path, const std::error_code& ec) {
  return what + " " + path.string() + ": " + ec.message();
}

// Recursive size/file count of a directory tree (used for the manifest and for
// `list_backups`). Best-effort: an unreadable file counts as zero bytes rather
// than failing the whole snapshot.
void scan_tree(const fs::path& root, uintmax_t* bytes, uint64_t* files) {
  std::error_code ec;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  while (!ec && it != end) {
    std::error_code item_ec;
    if (it->is_regular_file(item_ec)) {
      const auto size = fs::file_size(it->path(), item_ec);
      if (!item_ec) {
        *bytes += static_cast<uintmax_t>(size);
        ++*files;
      }
    }
    it.increment(ec);
  }
}

// Copies the *contents* of `from` into `to`, recursively. `skip_path` (the live
// backup directory, when it lives inside the world it snapshots) and
// `skip_names` (the manifest when a snapshot is copied back, and the level lock
// in both directions — see the callers) are left out. Files already in `to` are
// overwritten.
bool copy_tree(const fs::path& from, const fs::path& to, const fs::path& skip_path,
               const std::vector<std::string>& skip_names, std::string* error) {
  std::error_code ec;
  fs::create_directories(to, ec);
  if (ec) {
    *error = path_error("cannot create", to, ec);
    return false;
  }
  // Canonical form of the excluded subtree, so a relative `--backup-dir`
  // pointing at a child of the world is still recognised.
  fs::path skip;
  if (!skip_path.empty()) {
    skip = fs::weakly_canonical(skip_path, ec);
    if (ec) {
      ec.clear();
      skip.clear();
    }
  }
  fs::directory_iterator it(from, ec);
  const fs::directory_iterator end;
  while (!ec && it != end) {
    const fs::path src = it->path();
    std::error_code item_ec;
    const bool is_dir = it->is_directory(item_ec);
    if (item_ec) {
      item_ec.clear();
    }
    bool excluded = !skip.empty() && fs::weakly_canonical(src, item_ec) == skip && !item_ec;
    for (const std::string& name : skip_names) {
      if (src.filename() == name) {
        excluded = true;
        break;
      }
    }
    if (!excluded) {
      const fs::path dst = to / src.filename();
      if (is_dir) {
        if (!copy_tree(src, dst, skip_path, skip_names, error))
          return false;
      } else if (fs::is_regular_file(src, item_ec)) {
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, item_ec);
        if (item_ec) {
          *error = path_error("cannot copy", src, item_ec);
          return false;
        }
      }
    }
    it.increment(ec);
  }
  if (ec) {
    *error = path_error("cannot read", from, ec);
    return false;
  }
  return true;
}

bool write_manifest(const fs::path& snapshot, const fs::path& world_dir, uintmax_t bytes,
                    uint64_t files, uint64_t created_unix, std::string* error) {
  std::ofstream out(snapshot / kManifestName, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "cannot write " + (snapshot / kManifestName).string();
    return false;
  }
  // One line of JSON, the same shape tools/ writes elsewhere in the tree; the
  // marker is what `is_backup_dir` looks for.
  out << "{\"format\":\"plusmc-backup\",\"version\":1,\"created_unix\":" << created_unix
      << ",\"world\":\"" << world_dir.filename().string() << "\",\"files\":" << files
      << ",\"bytes\":" << bytes << "}\n";
  out.flush();
  if (!out) {
    *error = "cannot write " + (snapshot / kManifestName).string();
    return false;
  }
  return true;
}

// The manifest's `created_unix`, read back with the same substring approach the
// hosting config readers use (this is a file this server wrote, not input): 0
// when it is missing.
uint64_t manifest_created_unix(const fs::path& snapshot, uintmax_t* bytes, uint64_t* files) {
  std::ifstream in(snapshot / kManifestName, std::ios::binary);
  if (!in)
    return 0;
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes != nullptr || files != nullptr) {
    // The live counts are recomputed from the tree rather than trusted from the
    // manifest: a snapshot a host edited by hand should still list honestly.
    uintmax_t b = 0;
    uint64_t f = 0;
    scan_tree(snapshot, &b, &f);
    if (bytes != nullptr)
      *bytes = b;
    if (files != nullptr)
      *files = f;
  }
  const std::string key = "\"created_unix\":";
  const size_t at = text.find(key);
  if (at == std::string::npos)
    return 0;
  return static_cast<uint64_t>(std::strtoull(text.c_str() + at + key.size(), nullptr, 10));
}

bool has_manifest(const fs::path& dir) {
  std::ifstream in(dir / kManifestName, std::ios::binary);
  if (!in)
    return false;
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return text.find(kManifestMarker) != std::string::npos;
}

// Whether `dir` is inside `parent` (or is `parent` itself).
bool is_within(const fs::path& dir, const fs::path& parent) {
  std::error_code ec;
  const fs::path a = fs::weakly_canonical(dir, ec);
  if (ec)
    return false;
  const fs::path b = fs::weakly_canonical(parent, ec);
  if (ec)
    return false;
  const std::string as = a.string(), bs = b.string();
  if (as == bs)
    return true;
  if (bs.size() >= as.size())
    return false;
  return as.compare(0, bs.size(), bs) == 0 && as[bs.size()] == '/';
}

} // namespace

std::string backup_stamp(std::time_t when) {
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &when);
#else
  gmtime_r(&when, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return std::string(buf);
}

bool is_backup_dir(const fs::path& dir, std::string* why) {
  auto refuse = [&](const std::string& reason) {
    if (why != nullptr)
      *why = reason;
    return false;
  };
  std::error_code ec;
  if (!fs::is_directory(dir, ec))
    return refuse(dir.string() + " is not a directory");
  if (!fs::is_regular_file(dir / "level.dat", ec))
    return refuse(dir.string() + " has no level.dat");
  if (!has_manifest(dir))
    return refuse(dir.string() + " is not a +MC backup (no " + kManifestName + " marker)");
  // At least one region file: a snapshot without one would restore a world with
  // no chunks at all, which is a mistake (an empty backup directory) rather
  // than a world.
  bool any_region = false;
  fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  while (!ec && it != end) {
    std::error_code item_ec;
    if (it->is_regular_file(item_ec) && it->path().extension() == ".mca")
      any_region = true;
    it.increment(ec);
  }
  if (!any_region)
    return refuse(dir.string() + " has no region (.mca) file");
  return true;
}

std::vector<BackupEntry> list_backups(const fs::path& backup_dir) {
  std::vector<BackupEntry> out;
  std::error_code ec;
  if (!fs::is_directory(backup_dir, ec))
    return out;
  for (fs::directory_iterator it(backup_dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code item_ec;
    if (!it->is_directory(item_ec))
      continue;
    const fs::path path = it->path();
    const std::string name = path.filename().string();
    if (name.size() > strlen(kPartialSuffix) &&
        name.compare(name.size() - strlen(kPartialSuffix), strlen(kPartialSuffix),
                     kPartialSuffix) == 0)
      continue; // a copy that never finished; the next create overwrites it
    if (!has_manifest(path))
      continue;
    BackupEntry entry;
    entry.name = name;
    entry.path = path;
    entry.created_unix = manifest_created_unix(path, &entry.bytes, &entry.files);
    out.push_back(std::move(entry));
  }
  // The stamp names sort chronologically, so this is newest-first.
  std::sort(out.begin(), out.end(),
            [](const BackupEntry& a, const BackupEntry& b) { return a.name > b.name; });
  return out;
}

BackupResult create_backup(const fs::path& world_dir, const fs::path& backup_dir, int keep) {
  BackupResult out;
  if (world_dir.empty()) {
    out.error = "no world directory to back up";
    return out;
  }
  if (backup_dir.empty()) {
    out.error = "no backup directory configured";
    return out;
  }
  std::error_code ec;
  if (!fs::is_directory(world_dir, ec)) {
    out.error = "world directory " + world_dir.string() + " is not a directory";
    return out;
  }
  const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
  // A second-resolution stamp with a numeric suffix when two snapshots land in
  // the same second (a scheduled pass racing a manual one) — the suffix keeps
  // the name sortable-ish (only the tie's order can be arbitrary).
  std::string name = backup_stamp(static_cast<std::time_t>(now));
  for (int n = 1; fs::exists(backup_dir / name, ec); ++n) {
    ec.clear();
    name = backup_stamp(static_cast<std::time_t>(now)) + "-" + std::to_string(n);
  }
  fs::create_directories(backup_dir, ec);
  if (ec) {
    out.error = path_error("cannot create", backup_dir, ec);
    return out;
  }
  const fs::path partial = backup_dir / (name + kPartialSuffix);
  fs::remove_all(partial, ec);
  ec.clear();
  std::string error;
  // The backup directory is excluded by path, so a default `<world>/backups`
  // does not try to copy the snapshots into their own snapshot. The level lock
  // is excluded by name: it is process state, not world state, and a snapshot
  // that carried it would hand a restore the file the live process is holding
  // its lock on.
  if (!copy_tree(world_dir, partial, backup_dir, {mc::world::kWorldLockFileName}, &error)) {
    fs::remove_all(partial, ec);
    out.error = error;
    return out;
  }
  scan_tree(partial, &out.bytes, &out.files);
  if (!write_manifest(partial, world_dir, out.bytes, out.files, now, &error)) {
    fs::remove_all(partial, ec);
    out.error = error;
    return out;
  }
  const fs::path target = backup_dir / name;
  fs::rename(partial, target, ec);
  if (ec) {
    fs::remove_all(partial, ec);
    out.error = path_error("cannot rename", partial, ec) + " -> " + target.string();
    return out;
  }
  out.ok = true;
  out.path = target;
  // Rotation: newest `keep` snapshots stay. Only snapshots are candidates, so
  // anything else a host keeps in the directory survives.
  if (keep > 0) {
    const std::vector<BackupEntry> snapshots = list_backups(backup_dir);
    for (size_t i = static_cast<size_t>(keep); i < snapshots.size(); ++i) {
      std::error_code rm_ec;
      if (fs::remove_all(snapshots[i].path, rm_ec)) {
        ++out.removed;
        log::info("backup: rotated out ", snapshots[i].name);
      } else {
        log::warn("backup: could not remove ", snapshots[i].path.string(), ": ", rm_ec.message());
      }
    }
  }
  return out;
}

bool validate_restore(const fs::path& world_dir, const fs::path& backup_dir,
                      const std::string& name, std::string* why) {
  auto refuse = [&](const std::string& reason) {
    if (why != nullptr)
      *why = reason;
    return false;
  };
  // `name` reaches this from a command line, so it may not name a path at all:
  // a separator, "." or ".." is refused before anything is touched. The
  // snapshot still has to pass `is_backup_dir`.
  if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos)
    return refuse("invalid backup name '" + name + "'");
  if (world_dir.empty())
    return refuse("no world directory to restore into");
  const fs::path source = backup_dir / name;
  // The default layout puts the snapshots *inside* the world directory
  // (<world>/backups), which is fine — the copy walks the snapshot, not the
  // world. What is not fine is the world living inside the snapshot, or the
  // snapshot being the world: either one copies a directory into itself.
  if (source == world_dir || is_within(world_dir, source))
    return refuse("refusing to restore: " + source.string() + " contains " + world_dir.string());
  std::string reason;
  if (!is_backup_dir(source, &reason))
    return refuse("not a usable backup: " + reason);
  return true;
}

BackupResult restore_backup(const fs::path& world_dir, const fs::path& backup_dir,
                            const std::string& name) {
  BackupResult out;
  // One validator for the drill, the startup flag and the CLI's dry run: the
  // refusal a host reads is the one the copy would have made.
  std::string why;
  if (!validate_restore(world_dir, backup_dir, name, &why)) {
    out.error = why;
    return out;
  }
  const fs::path source = backup_dir / name;
  std::error_code ec;
  fs::create_directories(world_dir, ec);
  if (ec) {
    out.error = path_error("cannot create", world_dir, ec);
    return out;
  }
  std::string error;
  // The manifest is metadata about the snapshot, not world state, so it does
  // not travel back. Neither does the level lock: the caller may be holding it
  // (the startup drill restores under it), and replacing the file would swap
  // the inode its lock lives on, letting a second server open the world.
  if (!copy_tree(source, world_dir, fs::path(), {kManifestName, mc::world::kWorldLockFileName},
                 &error)) {
    out.error = error;
    return out;
  }
  for (const BackupEntry& entry : list_backups(backup_dir)) {
    if (entry.name == name) {
      out.bytes = entry.bytes;
      out.files = entry.files;
    }
  }
  out.ok = true;
  out.path = source;
  return out;
}

} // namespace mc::server
