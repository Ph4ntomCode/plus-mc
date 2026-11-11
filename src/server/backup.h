// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 5 (ops) — online backups, rotation and the restore drill.
//
// The *save* path is Phase 2's and is done: per-chunk dirty tracking, atomic
// temp+rename writes, autosave on a cadence, a shutdown flush (docs/PHASE2.md).
// What a host needs on top of that is the operational half — a snapshot it can
// point at, rotation so the snapshots do not grow without bound, and a restore
// that has actually been *run* rather than assumed. docs/PHASE5.md names the
// restore path as the part that had never been exercised; this file is it.
//
// A snapshot is a plain directory copy of the world's persistent state, taken
// while the server keeps running:
//
//   <backup_dir>/<stamp>/level.dat
//   <backup_dir>/<stamp>/dimensions/**     (region files — and anything else a
//                                           future dimension adds)
//   <backup_dir>/<stamp>/mobs.dat
//   <backup_dir>/<stamp>/advancements.dat
//   <backup_dir>/<stamp>/players/**        (when present)
//   <backup_dir>/<stamp>/backup.json       (the manifest, see below)
//
// The caller is expected to have **flushed first** — `WorldInterface::save_all`
// plus the two store saves, which is exactly what main.cpp's backup pass does —
// so the copy is of quiescent files. A region file being rewritten by a session
// thread *during* the copy is the one thing that could tear a snapshot, and the
// manifest does not pretend otherwise; the copy runs on the tick thread, and
// the per-region writer it races is the same one the flush just used.
//
// `restore_backup` is deliberately a **startup** operation: it overwrites the
// live world's files, and running it before the world is opened is the only
// ordering that cannot half-restore under a running server (main.cpp's
// --restore-backup does exactly that).
#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace mc::server {

// One snapshot on disk, as `list_backups` reports it.
struct BackupEntry {
  std::string name; // the directory name, "20261012T201530Z"
  std::filesystem::path path;
  uintmax_t bytes = 0;
  uint64_t files = 0;
  // The manifest's `created_unix`, or 0 when it is missing/unreadable.
  uint64_t created_unix = 0;
};

struct BackupResult {
  bool ok = false;
  std::string error;          // why not, when !ok (logged by the caller, never sent)
  std::filesystem::path path; // the snapshot written (create) or the source used (restore)
  uintmax_t bytes = 0;
  uint64_t files = 0;
  // How many older snapshots rotation deleted (create only).
  int removed = 0;
};

// The snapshot directory name for `when`: `YYYYMMDDTHHMMSSZ` in UTC — which
// sorts chronologically as a plain string, the property rotation and
// `list_backups` both rely on.
std::string backup_stamp(std::time_t when);

// Copies the world into <backup_dir>/<stamp>.partial, writes the manifest, then
// renames it into place (so a snapshot is never half-visible), and finally
// rotates: at most `keep` snapshots remain (`keep <= 0` means keep every one).
// Only directories that *are* snapshots (see `is_backup_dir`) are ever deleted,
// so a host's own files in the backup directory are left alone.
BackupResult create_backup(const std::filesystem::path& world_dir,
                           const std::filesystem::path& backup_dir, int keep);

// Every snapshot in `backup_dir`, newest first. A missing directory is an empty
// list, not an error. `.partial` leftovers (a crash mid-copy) are not listed.
std::vector<BackupEntry> list_backups(const std::filesystem::path& backup_dir);

// The restore drill: validates that `name` is one of this server's snapshots
// and copies its contents back over `world_dir` (level.dat, the dimension
// tree, the two store files). The **world must not be open** — the caller runs
// this before it is loaded. `name` is a directory name, never a path: a
// separator, "." or ".." is refused.
BackupResult restore_backup(const std::filesystem::path& world_dir,
                            const std::filesystem::path& backup_dir, const std::string& name);

// Whether a restore of `name` into `world_dir` *would* be accepted, without
// touching anything: the name is a directory name and not a path, the snapshot
// passes `is_backup_dir`, and the world does not live inside the snapshot.
// `why` receives the refusal in the same words `restore_backup` reports it, and
// this is the first thing `restore_backup` itself asks — so a host's dry run
// (`plusmc-restore --restore <name> --dry-run`) is the drill's own answer
// rather than a second opinion that can drift from it.
bool validate_restore(const std::filesystem::path& world_dir,
                      const std::filesystem::path& backup_dir, const std::string& name,
                      std::string* why);

// Whether `dir` looks like a snapshot this server wrote: a directory holding a
// `level.dat`, at least one region file (`.mca`, anywhere below it) and the
// manifest marker. `why` (when non-null) receives the reason it is not one — an
// empty backup directory is the common case a restore drill must refuse rather
// than serve.
bool is_backup_dir(const std::filesystem::path& dir, std::string* why);

} // namespace mc::server
