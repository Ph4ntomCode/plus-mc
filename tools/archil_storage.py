#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Archil disk this repo keeps files on between runs.

A disk outlives the process that writes it: `tools/check_archil_persistence.py`
writes a line in one exec and reads it back in a second, separate one, which is
what "persistent" means here. Nothing is deployed and no server code is involved;
this module is the one place the SDK is configured, so the key stays server-side
and out of every other file.

Three environment variables, all declared (empty) in `.env.example`:

  * `ARCHIL_API_KEY` — an API key from https://console.archil.com/api-keys. It
    starts with `key-`. A disk/mount token (`adt_…`, `ARCHIL_MOUNT_TOKEN`) is a
    *different* credential and gets HTTP 401 from the API.
  * `ARCHIL_REGION` — the key is region-scoped: a key made in `aws-us-east-1`
    fails in `aws-us-west-2`. Defaults to `aws-us-east-1`, one of the three
    regions exec runs in (with `aws-us-west-2` and `aws-eu-west-1`).
  * `ARCHIL_DISK_ID` — the `dsk-…` the callers use. `disks()` lists what the key
    can see; `disk()` returns this one.

The values are read from the environment, and `load_env()` fills any that are
missing from the repo's `.env` (gitignored), so the tools run the same way
whether the key was exported or written into that file.

Exec notes that shape the callers: the disk is mounted at `/mnt/data` and is the
working directory, each exec is a fresh container (files persist, container state
does not), the timeout is 5 minutes and stdout/stderr are capped at 128 KiB each.
The mount is shared, so a command writes into a fresh per-job subdirectory (or
runs `archil checkout <path>` itself) instead of into the mount root.
"""

from __future__ import annotations

import os
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
ENV_FILE = REPO / ".env"
DEFAULT_REGION = "aws-us-east-1"

#: A fresh job directory's prefix; every exec that writes picks its own
#: subdirectory under this so a shared mount's ownership claim is never contested.
JOB_ROOT = "plusmc-jobs"

#: `ARCHIL_API_KEY` must look like an API key. Anything else is the wrong
#: credential — `create_disk`/`exec` answer 401 for a disk or mount token, and the
#: message is much clearer here than from the wire.
_KEY_RE = re.compile(r"^key-\S+$")


class ArchilConfigError(RuntimeError):
    """The environment is not ready to talk to Archil (missing or wrong key)."""


def load_env(path: pathlib.Path = ENV_FILE) -> None:
    """Fill `os.environ` from the repo's `.env`, without overriding what is set.

    A tiny `NAME=value` reader: the file is gitignored and only ever holds the
    three Archil variables, so nothing here needs quotes, escaping or expansion.
    A value already in the environment always wins — the exported key is the one
    the caller meant to use.
    """
    try:
        text = path.read_text()
    except OSError:
        return
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, _, value = line.partition("=")
        name, value = name.strip(), value.strip().strip("'\"")
        if name and value and name not in os.environ:
            os.environ[name] = value


def api_key() -> str:
    """`ARCHIL_API_KEY`, or a `ArchilConfigError` saying what to set instead."""
    load_env()
    key = os.environ.get("ARCHIL_API_KEY", "").strip()
    if not key:
        raise ArchilConfigError(
            "ARCHIL_API_KEY is not set. Put an API key from "
            "https://console.archil.com/api-keys (starts with `key-`) in the "
            "repo's .env as ARCHIL_API_KEY=key-… , or export it for this shell.")
    if not _KEY_RE.match(key):
        raise ArchilConfigError(
            "ARCHIL_API_KEY does not look like an Archil API key (it must start "
            "with `key-`). A disk/mount token (often `adt_…`, ARCHIL_MOUNT_TOKEN "
            "/ ARCHIL_DISK_TOKEN) is a different credential and gets HTTP 401 "
            "from the API; copy a key- key from "
            "https://console.archil.com/api-keys instead.")
    return key


def region() -> str:
    """`ARCHIL_REGION`, defaulting to `aws-us-east-1` (where exec runs)."""
    load_env()
    return os.environ.get("ARCHIL_REGION", "").strip() or DEFAULT_REGION


def disk_id() -> str | None:
    """`ARCHIL_DISK_ID` if it is declared, else None (the caller may create one)."""
    load_env()
    return os.environ.get("ARCHIL_DISK_ID", "").strip() or None


def configure():
    """Configure the SDK from the environment and hand back the module.

    `archil.configure` also falls back to these two variables itself; passing
    them makes `api_key()`'s clearer errors the ones a caller sees.
    """
    import archil  # imported here so `--help` and the config checks need no SDK

    archil.configure(api_key=api_key(), region=region())
    return archil


def disks():
    """Every disk this key can see — the SDK's own handles, listed."""
    return configure().list_disks()


def disk(disk_id_: str | None = None):
    """The disk named by `ARCHIL_DISK_ID` (or `disk_id_`), as the SDK handle."""
    target = disk_id_ or disk_id()
    if not target:
        raise ArchilConfigError(
            "ARCHIL_DISK_ID is not set and no disk id was given. Run "
            "tools/check_archil_persistence.py once: it creates a disk, saves "
            "its id into .env, and every later run reuses it.")
    return configure().get_disk(target)


def create_disk(name: str):
    """Create a disk and return its handle — deliberately dropping the token.

    `create_disk` answers `(disk, token)` where the token is a one-time *mount*
    token for attaching the disk to a machine. Exec does not need it and it must
    not be printed or stored, so it never leaves this function.
    """
    result = configure().create_disk(name=name)
    return result.disk


def remember_disk_id(disk_id_: str, path: pathlib.Path = ENV_FILE) -> pathlib.Path | None:
    """Save `disk_id_` as `ARCHIL_DISK_ID` in `.env` (creating it if needed).

    Used when the check creates a disk, so later runs reuse it instead of making
    another one. Only that one line is touched: the key and the region already in
    the file are left exactly as they are. Returns the file written, or None when
    it could not be written — the caller has the id and can report it to the user
    instead, which is not a reason to fail a check that already passed.
    """
    try:
        text = path.read_text()
    except OSError:
        text = ""
    lines = text.splitlines()
    row = f"ARCHIL_DISK_ID={disk_id_}"
    for n, line in enumerate(lines):
        if line.split("=", 1)[0].strip() == "ARCHIL_DISK_ID":
            lines[n] = row
            break
    else:
        lines.append(row)
    try:
        path.write_text("\n".join(lines) + "\n")
    except OSError:
        return None
    os.environ["ARCHIL_DISK_ID"] = disk_id_
    return path


def run(disk_obj, command: str):
    """Run `command` on the disk; the SDK's result (`.stdout`, `.stderr`,
    `.exit_code`, `.timing`)."""
    return disk_obj.exec(command)


def _decode(value) -> str:
    """A command stream as text (the SDK hands back bytes or str)."""
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return value or ""


def text(result) -> str:
    """A command result's stdout as text."""
    return _decode(getattr(result, "stdout", ""))


def stderr_text(result) -> str:
    """A command result's stderr as text."""
    return _decode(getattr(result, "stderr", ""))


def put_object(disk_obj, key: str, body) -> None:
    """Write `key` on the disk, object-style (`str` is encoded as UTF-8)."""
    disk_obj.put_object(key, body.encode("utf-8") if isinstance(body, str) else body)


def get_object(disk_obj, key: str) -> bytes:
    """Read `key` back; always bytes (the SDK hands back bytes or str)."""
    body = disk_obj.get_object(key)
    return body.encode("utf-8") if isinstance(body, str) else body


def get_text(disk_obj, key: str) -> str:
    """`get_object`, decoded as UTF-8."""
    return get_object(disk_obj, key).decode("utf-8", "replace")


def main() -> int:
    """`python3 tools/archil_storage.py` — say whether the environment is ready."""
    try:
        key, where = api_key(), region()
    except ArchilConfigError as exc:
        print(f"archil: {exc}", file=sys.stderr)
        return 2
    target = disk_id()
    print(f"archil: key {key[:4]}… ready, region {where}, "
          f"disk {target or '(none yet — the check creates one)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
