#!/usr/bin/env python3
"""Phase 5 (ops) — `plusmc-restore`: the host-facing restore command.

`docs/PHASE5.md` §6 item 8's last piece. The drill itself landed with the
backup slice (`--backup-interval` / `--backup-keep` / `--restore-backup`, pinned
by `tests/backup_test.py`), but every one of those is a *startup flag*: to list
snapshots or to ask whether a restore would work, a host had to start the server
— which is exactly when a server that fails its restore never serves. This test
pins the command that answers those questions without starting anything, and
that calls the drill's own validator so the two cannot disagree.

It is deliberately server-free and fast: no listener, no world open, no vanilla
data, no bot. The fixture is a world and a snapshot written by hand (the
snapshot is a directory with a `level.dat`, a region file and the manifest the
engine writes), which is what makes the cases below cheap enough to be exact.

What it walks:

  A. **the listing.** One tab-separated line per snapshot, newest first, with
     the byte/file counts the tree on disk actually has and the manifest's own
     `created_unix` as UTC — and *nothing created* by looking (the whole tree is
     compared before and after the run). A backup directory that does not exist
     is an empty listing and exit 0, not a new directory.
  B. **the restore.** After the live region file is changed, `--restore <name>`
     makes it byte-identical to the snapshot's copy again, says so on stdout,
     and does not copy the manifest back (it is metadata about the snapshot, not
     world state).
  C. **the dry run.** `--restore <name> --dry-run` reports the same snapshot and
     writes nothing: the live world is byte-identical after it.
  D. **the refusals.** A name that is not there, a name that is a path, a
     directory with a `level.dat` but no manifest, a plain file, and the
     degenerate restore-into-the-snapshot case are each refused with exit 1,
     and the live world is byte-identical after every one of them.
  E. **the arguments.** Bad argv (no `--world`, an unknown flag, two actions at
     once, `--dry-run` without `--restore`, a flag with no value) exits 2 —
     the 0/1/2 convention `--validate-config` established — and `--help` exits 0.

Run: restore_cli_test.py /path/to/plusmc-restore
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

REGION_REL = os.path.join("dimensions", "minecraft", "overworld", "region", "r.0.0.mca")
STAMP = "20260101T000000Z"
# Newer than STAMP, so a copy in flight that *were* listed would sort first and
# be impossible to miss.
PARTIAL_STAMP = "20260102T000000Z"
CREATED_UNIX = 1767225600  # 2026-01-01T00:00:00Z
SNAPSHOT_LEVEL = b"+MC level (snapshot)\n"
LIVE_LEVEL = b"+MC level (live)\n"
SNAPSHOT_REGION = b"snapshot region file bytes \x00\x01\x02"
LIVE_REGION = b"live region file bytes \x00\xff\xfe"


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)


def make_world(path, region):
    """A world directory with a level and one region file."""
    write(os.path.join(path, "level.dat"), LIVE_LEVEL)
    write(os.path.join(path, REGION_REL), region)
    return path


def make_snapshot(path):
    """A snapshot this engine would recognise: level.dat + region + manifest."""
    write(os.path.join(path, "level.dat"), SNAPSHOT_LEVEL)
    write(os.path.join(path, REGION_REL), SNAPSHOT_REGION)
    manifest = {"format": "plusmc-backup", "version": 1, "created_unix": CREATED_UNIX,
                "world": "world", "files": 2, "bytes": 0}
    with open(os.path.join(path, "backup.json"), "w", encoding="utf-8") as fh:
        json.dump(manifest, fh)
    return path


def tree_state(path):
    """Every file's (relative path, size, sha256), plus the set of directories.

    The comparison `a == b` over this is what pins "the command wrote nothing" /
    "the refused restore left the world exactly as it was" without trusting a
    log line.
    """
    files = {}
    dirs = set()
    if not os.path.exists(path):
        return {"files": files, "dirs": dirs, "exists": False}
    for dirpath, dirnames, filenames in os.walk(path):
        for name in dirnames:
            dirs.add(os.path.relpath(os.path.join(dirpath, name), path))
        for name in filenames:
            full = os.path.join(dirpath, name)
            with open(full, "rb") as fh:
                blob = fh.read()
            files[os.path.relpath(full, path)] = (len(blob), hashlib.sha256(blob).hexdigest())
    return {"files": files, "dirs": dirs, "exists": True}


def run(binary, args):
    proc = subprocess.run([binary] + args, capture_output=True, text=True, timeout=30)
    return proc.returncode, proc.stdout, proc.stderr


def listing_lines(stdout):
    return [line for line in stdout.splitlines() if line.strip()]


def read(path):
    with open(path, "rb") as fh:
        return fh.read()


def snapshot_bytes(snapshot_dir):
    """What the listing should say: the snapshot tree's own size and file count."""
    total, count = 0, 0
    for dirpath, _dirs, files in os.walk(snapshot_dir):
        for name in files:
            total += os.path.getsize(os.path.join(dirpath, name))
            count += 1
    return total, count


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc-restore", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1])
    root = tempfile.mkdtemp(prefix="plusmc-restore-")
    try:
        # --- the fixture -------------------------------------------------------
        # Two layouts, because both are real: the default (snapshots inside the
        # world, <world>/backups) and an explicit --backup-dir outside the world.
        world = make_world(os.path.join(root, "world"), LIVE_REGION)
        snaps = make_snapshot(os.path.join(root, "snaps", STAMP))
        world2 = make_world(os.path.join(root, "world2"), LIVE_REGION)
        inside = make_snapshot(os.path.join(world2, "backups", STAMP))
        snap_bytes, snap_files = snapshot_bytes(inside)
        stamp_utc = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(CREATED_UNIX))

        # --- A. the listing ----------------------------------------------------
        before = tree_state(os.path.join(root, "world2"))
        code, out, err = run(binary, ["--world", world2])
        assert code == 0, f"a listing must exit 0: {code} {err}"
        assert tree_state(os.path.join(root, "world2")) == before, \
            "listing must not create, delete or rewrite anything"
        lines = listing_lines(out)
        assert len(lines) == 1, f"one snapshot must be one line: {lines}"
        fields = lines[0].split("\t")
        assert fields[0] == STAMP, f"the name is the directory name: {fields}"
        assert int(fields[1]) == snap_bytes and int(fields[2]) == snap_files, \
            f"the listing must report the tree's own size/files: {fields} against " \
            f"({snap_bytes}, {snap_files})"
        assert fields[3] == stamp_utc, f"created_unix as UTC: {fields[3]} != {stamp_utc}"
        assert os.path.join(world2, "backups") in err, \
            f"the summary names the backup directory it read: {err!r}"
        print(f"[1] list: {STAMP} ({snap_bytes} bytes, {snap_files} files, {stamp_utc})",
              flush=True)

        # A `.partial` directory is a snapshot *in flight*: by the time it is
        # renamed into place it already holds a level.dat, a manifest and a
        # region file, so it can pass every "is this a snapshot?" test — and
        # `list_backups` still does not list it (server/backup.h says so, and
        # the engine's own rotation must not count it either). Written here on
        # purpose rather than waited for, so the check is deterministic.
        partial = os.path.join(world2, "backups", PARTIAL_STAMP + ".partial")
        make_snapshot(partial)
        code, out, err = run(binary, ["--world", world2, "--list"])
        assert code == 0, f"a listing with a copy in flight still exits 0: {err!r}"
        names = [line.split("\t")[0] for line in listing_lines(out)]
        assert names == [STAMP], f"a copy in flight must not be listed: {names}"
        assert os.path.isdir(partial), "and a listing must not delete it"
        print("[1b] a .partial copy in flight is not a snapshot: not listed, kept",
              flush=True)

        # An explicit --backup-dir outside the world, and no <world>/backups is
        # created by looking at a world that does not have one.
        code, out, err = run(binary, ["--world", world, "--backup-dir", os.path.join(root, "snaps")])
        assert code == 0 and listing_lines(out)[0].split("\t")[0] == STAMP, \
            f"--backup-dir must list the same snapshot: {out!r} {err!r}"
        assert not os.path.exists(os.path.join(world, "backups")), \
            "listing must not create the world's default backup directory"
        code, out, err = run(binary, ["--world", os.path.join(root, "empty-world"), "--list"])
        assert code == 0 and listing_lines(out) == [], \
            f"a world with no snapshots lists nothing and exits 0: {out!r}"
        assert "no snapshots" in err, f"and says so: {err!r}"
        assert not os.path.exists(os.path.join(root, "empty-world", "backups")), \
            "a listing of a missing backup directory must not create it"
        print("[2] list: an external --backup-dir works; a missing one is empty, exit 0",
              flush=True)

        # --- B. the restore ----------------------------------------------------
        live_region = os.path.join(world2, REGION_REL)
        write(live_region, LIVE_REGION + b"changed after the snapshot")
        assert read(live_region) != read(os.path.join(inside, REGION_REL))
        code, out, err = run(binary, ["--world", world2, "--restore", STAMP])
        assert code == 0, f"a usable snapshot must restore: {code} {err}"
        assert read(live_region) == SNAPSHOT_REGION, \
            "the restored region file must be byte-identical to the snapshot's copy"
        assert read(os.path.join(world2, "level.dat")) == SNAPSHOT_LEVEL
        assert f"restored {STAMP} -> " in out, f"the command must say what it did: {out!r}"
        assert not os.path.exists(os.path.join(world2, "backup.json")), \
            "the manifest is metadata about the snapshot and must not travel back"
        print(f"[3] restore: {STAMP} -> the live region file is the snapshot's bytes again",
              flush=True)

        # --- C. the dry run ----------------------------------------------------
        write(live_region, LIVE_REGION + b"changed again, after a dry run's call")
        before = tree_state(world2)
        code, out, err = run(binary, ["--world", world2, "--restore", STAMP, "--dry-run"])
        assert code == 0, f"a dry run of a usable snapshot exits 0: {code} {err}"
        assert f"would restore {STAMP} -> " in out, f"the dry run must report the plan: {out!r}"
        assert tree_state(world2) == before, "a dry run must write nothing at all"
        print("[4] dry run: reported the plan, wrote nothing", flush=True)

        # --- D. the refusals ---------------------------------------------------
        # Each of these must exit 1 *and* leave the world byte-identical. The
        # hand-made directory is the interesting one: a level.dat with no
        # manifest is a stranger's dump, not one of this server's snapshots.
        manual = os.path.join(world2, "backups", "manual")
        write(os.path.join(manual, "level.dat"), b"not a real level\n")
        write(os.path.join(world2, "backups", "a-file"), b"not a directory\n")
        refusals = [
            (["--world", world2, "--restore", "nope"], "not a usable backup"),
            (["--world", world2, "--restore", "../escape"], "invalid backup name"),
            (["--world", world2, "--restore", "manual"], "is not a +MC backup"),
            (["--world", world2, "--restore", "a-file"], "not a usable backup"),
            # The degenerate layout: the world *is* the snapshot. Either
            # direction of that copies a directory into itself, so it is refused.
            (["--world", os.path.join(world2, "backups", STAMP),
              "--backup-dir", os.path.join(world2, "backups"), "--restore", STAMP],
             "refusing to restore"),
        ]
        for args, expected in refusals:
            before = tree_state(world2)
            code, out, err = run(binary, args)
            assert code == 1, f"{args} must be refused with exit 1 (got {code}): {err!r}"
            assert expected in err, f"{args} must say why ({expected!r}): {err!r}"
            assert listing_lines(out) == [], f"a refusal is not a result: {out!r}"
            assert tree_state(world2) == before, \
                f"a refused restore must leave the world exactly as it was: {args}"
        print(f"[5] refusals: {len(refusals)} cases, exit 1 each, the world untouched",
              flush=True)

        # --- E. the arguments --------------------------------------------------
        bad_argv = [
            ([], "--world"),
            (["--world"], "--world"),
            (["--world", world2, "--nope"], "unknown argument"),
            (["--world", world2, "--list", "--restore", STAMP], "--list and --restore"),
            (["--world", world2, "--dry-run"], "--dry-run needs --restore"),
            (["--world", world2, "--restore"], "needs a value"),
        ]
        for args, expected in bad_argv:
            code, out, err = run(binary, args)
            assert code == 2, f"{args} must exit 2 (got {code}): {err!r}"
            assert expected in err, f"{args} must name the problem ({expected!r}): {err!r}"
            assert listing_lines(out) == [], f"a usage error writes no result: {out!r}"
        code, out, err = run(binary, ["--help"])
        assert code == 0 and "--restore" in out and "--dry-run" in out, \
            f"--help must exit 0 and describe the actions: {out!r}"
        print(f"[6] arguments: {len(bad_argv)} bad argv cases exit 2, --help exits 0", flush=True)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("\nPASS: plusmc-restore — list, restore, dry run, and the refusals it shares "
          "with the server's own drill")
    return 0


if __name__ == "__main__":
    sys.exit(main())
