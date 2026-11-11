#!/usr/bin/env python3
"""Phase 5 (ops) — world backups: snapshot, rotation, and the restore drill.

`docs/PHASE5.md` §6 item 8 is the operational half of a save path that is
already atomic per chunk (Phase 2): a snapshot a host can point at, rotation so
the snapshots do not grow without bound, and a restore that has actually been
*run*. §3 of that file names the restore path as the part that had never been
exercised, so this test is deliberately end to end — it kills a world, restores
it, and checks the bytes on disk rather than a log line alone.

What it walks, all against a real `--world` directory:

  A. **the snapshot.** A pass flushes first (`save_all` + the two stores) and
     copies the world into `<world>/backups/<stamp>/` (level.dat, the
     dimensions/ tree, mobs.dat, advancements.dat, a manifest). The server keeps
     serving while the copy happens; a half-written snapshot never appears
     (`.partial` is renamed into place, and never listed).
  B. **the accident, and the drill.** A block is *changed* after the snapshot,
     the server is shut down (flush), and the live region file is shown to
     differ from the snapshot's copy — then the server is started again with
     `--restore-backup <name>`, and the live file is byte-identical to the
     snapshot's copy again. That is the restore, measured.
  C. **rotation.** With `--backup-keep 2`, more than two snapshots are created
     and only the newest two survive — and only snapshots are ever deleted.
  D. **the refusals.** A name that is not there, and a directory that is not one
     of this server's snapshots, are both refused *before* the world opens
     (nonzero exit, no half-restore).

The world is the void world on disk: the test lays its own floor, like
`tier-b-gate`, so block writes are what create the region file the drill
compares.

Run: backup_test.py /path/to/plusmc
"""
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import craft_test as ct  # noqa: E402
import wire_client  # noqa: E402
from craft_test import Bot, drain, rd_varint  # noqa: E402

CB_BLOCK_UPDATE = 0x08
SB_CHAT_COMMAND = 0x07
GROUND_Y = 3
STONE = "minecraft:stone"
AIR = "minecraft:air"
# The cell the drill changes: inside chunk (0,0), so it lands in r.0.0.mca.
CELL = (1, GROUND_Y, 1)
STEP_TIMEOUT = 30.0


def command(bot, text):
    """`/command`, with no read of the reply (the polls below do that)."""
    assert text.startswith("/")
    bot.send(SB_CHAT_COMMAND, ct.varint(len(text) - 1) + text[1:].encode())


def block_updates(packets):
    return {tuple(wire_client.unpack_block_pos(struct.unpack(">q", b[0:8])[0])): rd_varint(b, 8)[0]
            for p, b in packets if p == CB_BLOCK_UPDATE}


def poll(bot, predicate, timeout=STEP_TIMEOUT):
    packets = []
    deadline = time.time() + timeout
    while True:
        packets += drain(bot, 0.05)
        result = predicate(packets)
        if result or time.time() >= deadline:
            return result, packets


def setblock(bot, cell, state):
    command(bot, f"/setblock {cell[0]} {cell[1]} {cell[2]} {state}")
    ok, packets = poll(bot, lambda p: cell in block_updates(p))
    assert ok, f"/setblock {state} at {cell} must answer a block update: " \
               f"{sorted(block_updates(packets))}"


def region_file(world_dir):
    """The overworld region file holding chunk (0,0)."""
    path = os.path.join(world_dir, "dimensions", "minecraft", "overworld", "region", "r.0.0.mca")
    assert os.path.isfile(path), f"the world must have written {path}"
    return path


def snapshots(world_dir):
    """Every *snapshot* directory, newest first (the stamp names sort that way).

    `.partial` is excluded on purpose: it is a copy in flight, not a snapshot,
    and `list_backups` does not list one either (`server/backup.h` says so). A
    poll that lands inside the copy window must not be read as a snapshot that
    exists — it is renamed into place a moment later, so a name taken from that
    window points at a directory that is gone by the time it is read. On a
    loaded box that window is wide enough to hit reliably (see the note in
    docs/PHASE5.md §5).
    """
    root = os.path.join(world_dir, "backups")
    if not os.path.isdir(root):
        return []
    return sorted([d for d in os.listdir(root)
                   if os.path.isdir(os.path.join(root, d)) and not d.endswith(".partial")],
                  reverse=True)


def partial_dirs(world_dir):
    """Snapshot directories that are still being copied (`<stamp>.partial`)."""
    root = os.path.join(world_dir, "backups")
    if not os.path.isdir(root):
        return []
    return [d for d in os.listdir(root)
            if os.path.isdir(os.path.join(root, d)) and d.endswith(".partial")]


def is_snapshot(world_dir, name):
    """A usable snapshot: level.dat, a manifest and at least one region file."""
    root = os.path.join(world_dir, "backups", name)
    manifest = os.path.join(root, "backup.json")
    if not (os.path.isfile(os.path.join(root, "level.dat")) and os.path.isfile(manifest)):
        return False
    with open(manifest, "r", encoding="utf-8") as fh:
        if "plusmc-backup" not in fh.read():
            return False
    for dirpath, _dirs, files in os.walk(root):
        if any(f.endswith(".mca") for f in files):
            return True
    return False


def wait_for_snapshot(bot, world_dir, timeout=STEP_TIMEOUT):
    """The first *usable* snapshot, with the bot drained while it waits."""
    seen = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        drain(bot, 0.05)
        names = snapshots(world_dir)
        seen = sorted(set(seen) | set(names))
        for name in names:
            if is_snapshot(world_dir, name):
                return name, seen
    raise AssertionError(f"no usable snapshot appeared in {timeout:g} s; seen {seen}")


def read_bytes(path):
    with open(path, "rb") as fh:
        return fh.read()


def server_args(binary, data_root, world_dir, extra=()):
    return [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "backup test",
            "--offline-mode", "--gamemode", "1", "--data", data_root,
            "--world", world_dir] + list(extra)


def read_log(path):
    with open(path, "rb") as fh:
        return fh.read().decode("utf-8", "replace")


def wait_for_port(log_path, proc, timeout=STEP_TIMEOUT):
    """The port from the server's own log file.

    The log goes to a file rather than a pipe on purpose: `wait_listening`
    consumes the pipe up to the listening line, which is where the restore drill
    prints *its* line — the one thing this test has to read back afterwards.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = re.search(r"listening on .*:(\d+)", read_log(log_path))
        if m:
            port = int(m.group(1))
            for _ in range(30):
                try:
                    sock = socket.create_connection(("127.0.0.1", port), timeout=1)
                    sock.close()
                    return port
                except OSError:
                    time.sleep(0.1)
        if proc.poll() is not None:
            raise AssertionError(f"the server exited before listening: {read_log(log_path)[-800:]}")
        time.sleep(0.05)
    raise AssertionError(f"the server did not listen in {timeout:g} s: "
                         f"{read_log(log_path)[-800:]}")


def start(binary, data_root, world_dir, extra=(), env=None, log_path=None):
    """`(proc, port, log_path)`: a server whose whole log stays readable."""
    if log_path is None:
        fd, log_path = tempfile.mkstemp(prefix="plusmc-backup-log-", suffix=".txt")
        os.close(fd)
    with open(log_path, "wb") as log:
        proc = wire_client.start_process(server_args(binary, data_root, world_dir, extra), stdout=log,
                                stderr=subprocess.STDOUT, env=wire_client.server_env(env))
    return proc, wait_for_port(log_path, proc), log_path


def stop(proc, log_path):
    """SIGTERM and the shutdown flush, with the log the server wrote."""
    proc.terminate()
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
    return read_log(log_path)


def run_expecting_failure(binary, data_root, world_dir, extra, env):
    """A startup that is supposed to *refuse*: the restore error is fatal before
    the listener opens, so this cannot wait for a port."""
    fd, log_path = tempfile.mkstemp(prefix="plusmc-backup-log-", suffix=".txt")
    os.close(fd)
    with open(log_path, "wb") as log:
        proc = wire_client.start_process(server_args(binary, data_root, world_dir, extra), stdout=log,
                                stderr=subprocess.STDOUT, env=wire_client.server_env(env))
    try:
        code = proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        raise AssertionError(f"the server started despite {extra}")
    return code, read_log(log_path)


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    root = tempfile.mkdtemp(prefix="plusmc-backup-")
    world_dir = os.path.join(root, "world")
    # Only the backup pass flushes this world, so every file the test compares
    # is one the pass itself wrote.
    env = dict(os.environ)
    env["PLUSMC_AUTOSAVE_SECS"] = "0"

    try:
        # --- A. the snapshot --------------------------------------------------
        proc, port, log_path = start(binary, data_root, world_dir,
                                     extra=["--backup-interval", "1", "--backup-keep", "2"], env=env,
                                     log_path=os.path.join(root, "server-a.log"))
        bot = Bot("BackupBot", port)
        bot.join()
        bot.start_reader()
        for x in range(-2, 3):
            for z in range(-2, 3):
                command(bot, f"/setblock {x} {GROUND_Y} {z} {STONE}")
        drain(bot, 1.5)
        bot.move_to(0.5, GROUND_Y + 1.0, 0.5)
        drain(bot, 0.3)
        # The stone is what makes the chunk dirty, and the pass is what flushes.
        setblock(bot, CELL, STONE)
        first, seen = wait_for_snapshot(bot, world_dir)
        print(f"[1] snapshot {first} ({len(seen)} pass(es) seen)", flush=True)
        assert not os.path.exists(os.path.join(world_dir, "backups", first, "backups")), \
            "a snapshot must not contain the backup directory it lives in"
        assert not first.endswith(".partial") and first not in partial_dirs(world_dir), \
            f"a copy in flight is not a snapshot: {first}"
        # The listing rule itself (a `.partial` is never reported as a snapshot,
        # even when it already holds a level.dat, a manifest and a region file)
        # is pinned race-free in tests/restore_cli_test.py, which can write that
        # state on purpose instead of waiting to catch it.
        snapshot_region = os.path.join(world_dir, "backups", first,
                                       "dimensions", "minecraft", "overworld", "region",
                                       "r.0.0.mca")
        assert os.path.isfile(snapshot_region), "the snapshot must hold the region file"
        good = read_bytes(snapshot_region)
        print(f"[2] snapshot holds {len(good)} bytes of region data", flush=True)

        # --- B. the accident, then the drill ----------------------------------
        setblock(bot, CELL, AIR)
        bot.close()
        log = stop(proc, log_path)
        assert "backup: wrote" in log, f"the pass must log its snapshot: {log[-800:]}"
        live = read_bytes(region_file(world_dir))
        assert live != good, ("the changed block must reach disk (otherwise the drill below "
                              "would prove nothing)")
        differing = sum(1 for a, b in zip(live, good) if a != b)
        print(f"[3] the change reached disk: {len(live)} bytes, {differing} differing "
              f"byte(s) against the snapshot", flush=True)

        proc, port, log_path = start(binary, data_root, world_dir,
                                     extra=["--restore-backup", first], env=env,
                                     log_path=os.path.join(root, "server-b.log"))
        restored = read_bytes(region_file(world_dir))
        log = stop(proc, log_path)
        assert f"restore: {first} -> " in log, f"the drill must say what it restored: {log[-800:]}"
        assert restored == good, ("the restored region file must be byte-identical to the "
                                  f"snapshot's copy ({len(restored)} vs {len(good)} bytes)")
        print(f"[4] restored {first}: byte-identical to the snapshot", flush=True)

        # --- C. rotation ------------------------------------------------------
        proc, port, log_path = start(binary, data_root, world_dir,
                                     extra=["--backup-interval", "1", "--backup-keep", "2"], env=env,
                                     log_path=os.path.join(root, "server-c.log"))
        seen = set(snapshots(world_dir))
        deadline = time.time() + STEP_TIMEOUT
        left = snapshots(world_dir)
        while time.time() < deadline:
            time.sleep(0.2)
            seen |= set(snapshots(world_dir))
            left = snapshots(world_dir)
            # A listing can catch the new snapshot's rename before the same
            # pass removes the oldest one (rotation runs right after the copy),
            # so this waits for the count to settle rather than reading the
            # transient. Three distinct names seen *and* two left is what the
            # pair of facts below asserts.
            if len(seen) >= 3 and len(left) <= 2:
                break
        log = stop(proc, log_path)
        # Read the set *after* the server stopped. The loop's last read is a look
        # at a directory a pass may still have been writing into, and the end
        # state is what these assertions are about; the loop's own read is what
        # the `break` above needed. Names the stop itself revealed still count as
        # observed passes, which is why they are unioned in rather than ignored.
        left = snapshots(world_dir)
        seen |= set(left)
        assert len(seen) >= 3, f"three passes must produce three names (with keep 2): {seen}"
        assert len(left) == 2, f"--backup-keep 2 must leave two snapshots, left {left}"
        assert left == sorted(seen, reverse=True)[:2], \
            f"the newest two of {sorted(seen, reverse=True)} must survive, not {left}"
        assert "backup: rotated out" in log, \
            f"rotation must say what it removed: {log[-600:]}"
        for name in left:
            assert is_snapshot(world_dir, name), f"{name} survived as a usable snapshot"
        print(f"[5] rotation: {len(seen)} snapshots seen, the newest 2 kept ({left})", flush=True)

        # --- D. the refusals --------------------------------------------------
        before = read_bytes(region_file(world_dir))
        code, log = run_expecting_failure(binary, data_root, world_dir,
                                         ["--restore-backup", "nope"], env)
        assert code != 0, "a missing snapshot must not start the server"
        assert "not a usable backup" in log, f"the refusal must say why: {log[-800:]}"
        # A hand-made directory with a level.dat but no manifest is *not* a
        # snapshot: the drill refuses it rather than restoring a stranger's dump.
        fake = os.path.join(world_dir, "backups", "manual")
        os.makedirs(fake, exist_ok=True)
        with open(os.path.join(fake, "level.dat"), "wb") as fh:
            fh.write(b"not a real level")
        code, log = run_expecting_failure(binary, data_root, world_dir,
                                         ["--restore-backup", "manual"], env)
        assert code != 0, "a directory without the manifest must not be restored"
        assert "plusmc-backup" in log, f"the refusal must say why: {log[-800:]}"
        assert read_bytes(region_file(world_dir)) == before, \
            "a refused restore must leave the live world exactly as it was"
        print("[6] refusals: a missing name and a foreign directory are both rejected", flush=True)
    finally:
        subprocess.run(["rm", "-rf", root], check=False)
    print("\nPASS: backups — snapshot, rotation, the restore drill and its refusals")
    return 0


if __name__ == "__main__":
    sys.exit(main())
