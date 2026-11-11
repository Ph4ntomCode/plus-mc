// SPDX-License-Identifier: GPL-3.0-or-later
//
// plusmc-restore — the *host-facing* half of Phase-5 item 8's backup work.
//
// `docs/PHASE5.md` §6 item 8 lands with a snapshot schedule
// (`--backup-interval`), rotation (`--backup-keep`) and a restore *drill*
// (`--restore-backup <name>`), all of them startup flags: the drill runs
// before the world is opened, because restoring under a live world is a
// half-restore, and the server refusing to start on the world it was told to
// replace is better than serving it. What that shape leaves a host without is
// the command: to list snapshots, to ask "would this restore work?", or to
// actually restore, a host has to start the server — and a server that fails
// its restore never serves, which is exactly when a host wants the answer most.
//
// This tool is that command and nothing else. It links the engine's own backup
// module (`server/backup.{h,cpp}`) and calls the *same*
// `validate_restore`/`list_backups`/`restore_backup` the server does, so it
// cannot drift from the drill: `--dry-run` reports the refusal the copy would
// have made, in the copy's own words. What it deliberately does **not** do is
// start anything — no listener, no EULA gate, no world open, no vanilla data,
// no config file. It is the `--validate-config` argument applied to backups: a
// read-only walk a host can run on a live box, and a write only when asked.
//
// Usage:
//   plusmc-restore --world <dir> [--backup-dir <dir>]              # --list
//   plusmc-restore --world <dir> [--backup-dir <dir>] --list
//   plusmc-restore --world <dir> [--backup-dir <dir>] --restore <name> [--dry-run] [--force]
//   plusmc-restore --help
//
// `--backup-dir` defaults to `<world>/backups`, the server's own default.
// The listing goes to stdout (one snapshot per line, newest first, tab
// separated, so a host can pipe it); everything else — the summary, the
// refusal, the "the world is held" warning — goes to stderr, the convention
// `plusmc` itself uses. A restore now *checks* what the header used to only
// warn about: `<world>/session.lock` is vanilla's `DirectoryLock`, held for as
// long as a server has the level open, so the check is a probe rather than a
// guess (see `world/directory_lock.h`). Exit status: 0 accepted, 1 refused (nothing
// was written), 2 the arguments are wrong (the same 0/1/2 `--validate-config`
// established).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "core/log.h"
#include "server/backup.h"
#include "world/directory_lock.h"

namespace fs = std::filesystem;

namespace {

struct Args {
  fs::path world;      // required: the world a restore writes into
  fs::path backup_dir; // default <world>/backups, the server's own default
  std::string restore; // the snapshot to restore; empty = no restore asked for
  bool list = false;   // asked explicitly (a bare --world lists too)
  bool dry_run = false;
  bool force = false; // restore even while a server holds the level lock
};

void print_usage(std::FILE* out) {
  std::fprintf(out, "usage: plusmc-restore --world <dir> [--backup-dir <dir>] [--list]\n"
                    "       plusmc-restore --world <dir> [--backup-dir <dir>] --restore <name> "
                    "[--dry-run]\n"
                    "\n"
                    "Lists and restores this server's world snapshots without starting the\n"
                    "server. --backup-dir defaults to <world>/backups, where the server keeps\n"
                    "them by default. `--restore` refuses while a server holds the level's\n"
                    "lock (<world>/session.lock, vanilla's DirectoryLock); --force overrides\n"
                    "that for a host that knows the holder is not writing. Nothing is\n"
                    "written unless --restore is given without --dry-run.\n"
                    "\n"
                    "  --list              list snapshots, newest first (the default action)\n"
                    "  --restore <name>    copy snapshot <name> back over the world\n"
                    "  --dry-run           with --restore: check and report, write nothing\n"
                    "  --force             with --restore: proceed even if the world is held\n"
                    "  --help              this text\n"
                    "\n"
                    "exit status: 0 accepted, 1 refused (nothing written), 2 bad arguments\n");
}

// The manifest's `created_unix` as `YYYY-MM-DDTHH:MM:SSZ`, or "-" when the
// snapshot carries no readable stamp (the listing stays one line per snapshot
// rather than dropping the entry).
std::string utc_stamp(uint64_t created_unix) {
  if (created_unix == 0)
    return "-";
  const std::time_t when = static_cast<std::time_t>(created_unix);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &when);
#else
  gmtime_r(&when, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return std::string(buf);
}

} // namespace

int main(int argc, char** argv) {
  std::vector<std::string> tokens(argv + 1, argv + argc);
  Args args;
  for (size_t i = 0; i < tokens.size(); ++i) {
    std::string arg = tokens[i];
    // `--flag=value` is accepted beside `--flag value`; the server's own
    // parser only takes the latter, and a host typing the former should get a
    // working command rather than a usage error.
    std::string inline_value;
    bool has_inline = false;
    if (arg.size() > 2 && arg.compare(0, 2, "--") == 0) {
      const size_t eq = arg.find('=');
      if (eq != std::string::npos) {
        inline_value = arg.substr(eq + 1);
        arg = arg.substr(0, eq);
        has_inline = true;
      }
    }
    auto value_of = [&](const char* flag, std::string* out) {
      if (has_inline) {
        *out = inline_value;
        return true;
      }
      if (i + 1 >= tokens.size()) {
        std::fprintf(stderr, "error: %s needs a value\n", flag);
        return false;
      }
      *out = tokens[++i];
      return true;
    };
    std::string value;
    if (arg == "--world") {
      if (!value_of("--world", &value))
        return 2;
      args.world = value;
    } else if (arg == "--backup-dir") {
      if (!value_of("--backup-dir", &value))
        return 2;
      args.backup_dir = value;
    } else if (arg == "--restore") {
      if (!value_of("--restore", &value))
        return 2;
      args.restore = value;
    } else if (arg == "--list") {
      args.list = true;
    } else if (arg == "--dry-run") {
      args.dry_run = true;
    } else if (arg == "--force") {
      args.force = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage(stdout);
      return 0;
    } else {
      std::fprintf(stderr, "error: unknown argument '%s'\n", tokens[i].c_str());
      print_usage(stderr);
      return 2;
    }
  }
  if (args.world.empty()) {
    std::fprintf(stderr, "error: --world <dir> is required (which world to read/write)\n");
    print_usage(stderr);
    return 2;
  }
  if (args.backup_dir.empty())
    args.backup_dir = args.world / "backups";
  if (args.list && !args.restore.empty()) {
    std::fprintf(stderr, "error: --list and --restore are different actions\n");
    print_usage(stderr);
    return 2;
  }
  if (args.dry_run && args.restore.empty()) {
    std::fprintf(stderr, "error: --dry-run needs --restore <name> to check\n");
    print_usage(stderr);
    return 2;
  }

  // --- the listing (also the default action, and what a host runs first) -----
  if (args.restore.empty()) {
    const std::vector<mc::server::BackupEntry> snapshots =
        mc::server::list_backups(args.backup_dir);
    for (const mc::server::BackupEntry& entry : snapshots)
      std::printf(
          "%s\t%llu\t%llu\t%s\n", entry.name.c_str(), static_cast<unsigned long long>(entry.bytes),
          static_cast<unsigned long long>(entry.files), utc_stamp(entry.created_unix).c_str());
    if (snapshots.empty())
      mc::log::info("no snapshots in ", args.backup_dir.string());
    else
      mc::log::info(snapshots.size(), " snapshot(s) in ", args.backup_dir.string(),
                    " (newest first)");
    return 0;
  }

  // --- the restore (and its dry run) ----------------------------------------
  // The drill's own validator, so "would this work?" and "did it work?" cannot
  // disagree: the refusal below is the one restore_backup would have produced,
  // before anything has been touched.
  std::string why;
  if (!mc::server::validate_restore(args.world, args.backup_dir, args.restore, &why)) {
    mc::log::error("restore refused: ", why);
    return 1;
  }
  // What the snapshot holds, for the message either path prints. The dry run
  // reports the same numbers the copy then reports.
  uintmax_t bytes = 0;
  uint64_t files = 0;
  for (const mc::server::BackupEntry& entry : mc::server::list_backups(args.backup_dir)) {
    if (entry.name == args.restore) {
      bytes = entry.bytes;
      files = entry.files;
    }
  }

  // The level's own lock (`world/directory_lock.h`): a running server holds
  // <world>/session.lock, and copying over a world it is still writing is the
  // half-restore the startup drill exists to avoid. Vanilla's `isLocked` probe
  // touches nothing; a lock file left by a crash carries no lock and does not
  // block. The dry run reports the contention instead of refusing, because it
  // writes nothing.
  const bool in_use = mc::world::DirectoryLock::is_locked(args.world);
  if (in_use && !args.force && !args.dry_run) {
    mc::log::error("restore refused: ", (args.world / mc::world::kWorldLockFileName).string(),
                   " is held by a running server; stop it first, or pass --force if it is "
                   "certainly not writing");
    return 1;
  }
  if (in_use && args.force)
    mc::log::warn("--force: the world is held by a running server; the restore may be "
                  "overwritten as it runs");

  if (args.dry_run) {
    std::printf("would restore %s -> %s (%llu file(s), %llu bytes)\n", args.restore.c_str(),
                args.world.string().c_str(), static_cast<unsigned long long>(files),
                static_cast<unsigned long long>(bytes));
    if (in_use)
      mc::log::warn("dry run: the world is currently held at ", args.world.string(),
                    "; a real restore would refuse until that process stops");
    mc::log::info("dry run: the snapshot is usable and nothing was written");
    return 0;
  }
  const mc::server::BackupResult result =
      mc::server::restore_backup(args.world, args.backup_dir, args.restore);
  if (!result.ok) {
    mc::log::error("restore failed: ", result.error);
    return 1;
  }
  std::printf("restored %s -> %s (%llu file(s), %llu bytes)\n", args.restore.c_str(),
              args.world.string().c_str(), static_cast<unsigned long long>(result.files),
              static_cast<unsigned long long>(result.bytes));
  return 0;
}
