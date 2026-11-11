#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prove an Archil disk keeps a file between two *separate* execs.

The disk is the only thing that survives an exec — each one is a fresh container,
where files on the mount persist but container state does not. So this writes one
unique line in a first exec and reads it back with a second, independent `cat`,
and exits non-zero unless the second exec's stdout carries exactly what the first
wrote:

  * the disk is `ARCHIL_DISK_ID`, or a newly created one when that is unset (its
    id is then saved into the repo's `.env`, never its one-time mount token);
  * the line goes into a fresh per-job subdirectory (`plusmc-jobs/<utc stamp>-<hex>`)
    because exec mounts are shared and each job has to claim its own directory;
  * the read-back prints the disk name and id, the region and the line, so a
    passing run says *what* persisted *where*.

Usage:
    python3 tools/check_archil_persistence.py
    python3 tools/check_archil_persistence.py --disk-name plusmc-persist

It needs `ARCHIL_API_KEY` (a `key-…` API key, not a disk/mount token) in the
environment or in `.env`; see `.env.example` and `tools/archil_storage.py`.
"""

from __future__ import annotations

import argparse
import shlex
import sys
import time
import uuid

import archil_storage

# The statuses are the caller's: 2 is "the environment is not ready", 1 is "the
# disk answered but the line did not survive". A shell and CI both read non-zero
# as a failure; only the message distinguishes them.
EXIT_NOT_READY = 2
EXIT_NO_PERSISTENCE = 1


def stamp() -> str:
    """A UTC timestamp, filename- and sort-safe."""
    return time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())


def describe(disk_obj) -> tuple[str, str]:
    """`(name, id)` of an SDK disk handle, tolerating the field the SDK uses."""
    name = getattr(disk_obj, "name", None) or "(unnamed)"
    ident = (getattr(disk_obj, "id", None) or getattr(disk_obj, "disk_id", None)
             or getattr(disk_obj, "name", None) or "(unknown)")
    return str(name), str(ident)


def explain(exc: Exception) -> str:
    """An SDK failure with the hint for the credential mistakes it usually is."""
    text = str(exc)
    if "401" in text or "Unauthorized" in text or "unauthorized" in text:
        return (f"{text}\n"
                "  ARCHIL_API_KEY is being refused (401). The usual causes: the "
                "value is a disk/mount token rather than an API key (keys start "
                "with `key-`; tokens often `adt_…`), or the key was made in a "
                "different region than ARCHIL_REGION — keys are region-scoped. "
                "Copy a key- key from https://console.archil.com/api-keys.")
    return text


def open_disk(name: str | None):
    """The disk to check: `ARCHIL_DISK_ID`, else a new one saved into `.env`."""
    target = archil_storage.disk_id()
    if target:
        return archil_storage.disk(target)

    disk_name = name or f"plusmc-persist-{stamp()}"
    created = archil_storage.create_disk(disk_name)
    _, ident = describe(created)
    if ident == "(unknown)":
        print(f"[archil-check] created disk {disk_name!r}; could not read its id "
              "back, so ARCHIL_DISK_ID was not saved", file=sys.stderr)
        return created
    saved = archil_storage.remember_disk_id(ident)
    if saved is not None:
        print(f"[archil-check] created disk {disk_name!r} ({ident}); "
              f"saved it as ARCHIL_DISK_ID in {saved.name}")
    else:
        print(f"[archil-check] created disk {disk_name!r} ({ident}); could not "
              f"write {archil_storage.ENV_FILE.name}, so set "
              f"ARCHIL_DISK_ID={ident} to reuse it", file=sys.stderr)
    return created


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Write a line to an Archil disk in one exec and read it back "
                    "in a second one.")
    ap.add_argument("--disk-name", default=None,
                    help="name for the disk created when ARCHIL_DISK_ID is unset "
                         "(default: plusmc-persist-<utc stamp>)")
    args = ap.parse_args()

    try:
        archil_storage.api_key()
    except archil_storage.ArchilConfigError as exc:
        print(f"[archil-check] FAILED: {exc}", file=sys.stderr)
        return EXIT_NOT_READY
    where = archil_storage.region()

    try:
        disk = open_disk(args.disk_name)
    except archil_storage.ArchilConfigError as exc:
        print(f"[archil-check] FAILED: {exc}", file=sys.stderr)
        return EXIT_NOT_READY
    except Exception as exc:  # noqa: BLE001 - the SDK's error type is its own
        print(f"[archil-check] FAILED: {explain(exc)}", file=sys.stderr)
        return EXIT_NOT_READY

    disk_name, disk_ident = describe(disk)
    print(f"[archil-check] disk {disk_name!r} ({disk_ident}), region {where}")

    # A fresh per-job directory: the mount is shared, and a job that writes into
    # its own new directory is the one that owns it.
    job = f"{archil_storage.JOB_ROOT}/{stamp()}-{uuid.uuid4().hex[:8]}"
    line = f"plusmc persist check {uuid.uuid4().hex} {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}"
    path = f"{job}/line.txt"

    try:
        # 1. Write, in an exec of its own.
        wrote = disk.exec(
            f"mkdir -p {shlex.quote(job)} && "
            f"printf '%s\\n' {shlex.quote(line)} > {shlex.quote(path)}")
        if wrote.exit_code != 0:
            print(f"[archil-check] FAILED: the write exec exited {wrote.exit_code}\n"
                  f"  stderr: {archil_storage.stderr_text(wrote).strip()}",
                  file=sys.stderr)
            return EXIT_NO_PERSISTENCE

        # 2. Read it back, in a second exec — a new container, so anything not on
        #    the disk is gone by now.
        read = disk.exec(f"cat {shlex.quote(path)}")
    except Exception as exc:  # noqa: BLE001 - the SDK's error type is its own
        print(f"[archil-check] FAILED: {explain(exc)}", file=sys.stderr)
        return EXIT_NOT_READY

    # The second exec's stdout is the evidence: it has to carry the exact line the
    # first exec wrote (a `cat` that finds nothing exits non-zero and is caught by
    # the same condition).
    out = read.stdout
    got = out.decode("utf-8", "replace") if isinstance(out, bytes) else (out or "")
    if read.exit_code != 0 or line not in got:
        print(f"[archil-check] FAILED: the line written in the first exec did not "
              f"come back from the second\n"
              f"  wrote:   {line!r}\n"
              f"  read:    {got.strip()!r} (exit {read.exit_code})\n"
              f"  path:    {path}\n"
              f"  stderr:  {archil_storage.stderr_text(read).strip()}",
              file=sys.stderr)
        return EXIT_NO_PERSISTENCE

    print(f"[archil-check] wrote {path} in exec 1, read back in exec 2")
    print(f"[archil-check] read-back: {got.strip()}")
    print(f"[archil-check] OK: {line!r} survived between two execs on "
          f"{disk_name!r} ({disk_ident}) in {where}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
