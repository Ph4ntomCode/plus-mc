#!/usr/bin/env python3
"""Startup order: the save files are read before the listener (protocol 777).

`load_mob_population` and `load_advancement_state` both document that they "must
run before the listener starts", and a join depends on it: a connection accepted
before the store lands is served by a session whose `adopt_saved_advancements`
finds no row for the player, so a rejoining player's restored tree is silently
missing for the whole session. That is not a theory — `main.cpp` did start the
listener first, and running `advancement-persistence-wire` four copies at once
reproduced the missing rows 6 times in 8, with a server log that shows the
client's login *before* the store load and no adoption line at all.

Nothing on the wire can pin the order directly: the window closes before a client
could connect. `advancement-persistence-wire` pins its consequence (a rejoining
client that sends nothing is still handed its rows, which is the adoption
happening at all), and this test watches the order itself in the server's own log,
where it can be read off exactly. It boots twice, and both halves matter:

* **Unreadable saves.** `mobs.dat` and `advancements.dat` hold bytes that are not
  gzip NBT, so each load fails loudly: it returns false and `main.cpp` logs
  "could not read ...; starting empty" for it right after the load, behind the
  loader's own "is not gzip NBT" line naming the file. All four lines have to be
  in the log *before* the port is advertised. A load that ran after the listener
  would print them after it, and the failure message says which of the two
  happened (after the port, or never) by reading a little further. The boot then
  ends on a SIGTERM that has to exit 0: a save file that cannot be read is
  deliberately not fatal, and that is pinned rather than assumed.
* **A healthy store and population.** The second world carries a real
  `advancements.dat` — one player with one obtained criterion, written here from
  the documented schema (world/advancement_save.h) — and a real `mobs.dat` with
  one mob (world/mob_save.h), so the loads that *succeed* are visible too:
  "advancements: loaded 1 player(s), 1 advancement row(s)", "mobs: loaded 1 from
  ...", and the world the boot prepared ("serving world ...", "world spawn:") all
  have to be in the log before the port as well. Without this half the test could
  pass while the store was read at a moment that never works for a *real* file.

A *missing* file is a third case and is deliberately not used: that is success
with an empty state and logs nothing, so neither half would be able to say
anything about it.

Usage: startup_order_test.py /path/to/plusmc
"""
import gzip
import os
import pathlib
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_client  # noqa: E402  (server_env: the suite's shared server environment)

HOST = "127.0.0.1"
START = time.time()
# What `main.cpp` logs for a load that failed, in the order it loads them: the
# mob population first, then the advancement store.
LOAD_FAILURES = (
    "mobs: could not read the mob population; starting empty",
    "advancements: could not read the store; starting empty",
)
# ...and for the two loads that succeed, plus the world the boot prepared.
LOADED = (
    "advancements: loaded 1 player(s), 1 advancement row(s)",
    "mobs: loaded 1 from",
    "world spawn:",
)
LISTENING = re.compile(rb"listening on [\d.]+:(\d+)")

# The DataVersion both save files are stamped with (world/mob_save.h,
# world/advancement_save.h; the same number level.dat carries). Neither reader
# validates it — the files are read by shape — but a fixture that wrote anything
# else would not be the file a save writes.
SAVE_DATA_VERSION = 5023

# NBT tag ids, from the spec: what the fixture builder below needs and no more.
NBT_INT, NBT_LONG, NBT_DOUBLE, NBT_STRING, NBT_LIST, NBT_COMPOUND = 3, 4, 6, 8, 9, 10


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}", flush=True)


# --- the save files, written from their documented schemas -------------------
#
# Each helper returns `(tag id, payload)`, which is what a compound's field list
# and a list's items are made of. The two documents below are byte-for-byte the
# shape `nbt::serialize` writes: a root tag with an empty name, big-endian, gzip.


def _string_body(text):
    raw = text.encode("utf-8")
    return struct.pack(">H", len(raw)) + raw


def nbt_int(value):
    return NBT_INT, struct.pack(">i", value)


def nbt_long(value):
    return NBT_LONG, struct.pack(">q", value)


def nbt_double(value):
    return NBT_DOUBLE, struct.pack(">d", value)


def nbt_string(value):
    return NBT_STRING, _string_body(value)


def nbt_list(item_type, items):
    assert all(tag == item_type for tag, _ in items), "a list holds one type"
    return NBT_LIST, bytes([item_type]) + struct.pack(">i", len(items)) + b"".join(
        payload for _, payload in items)


def nbt_compound(fields):
    body = b"".join(bytes([tag]) + _string_body(name) + payload
                    for name, (tag, payload) in fields)
    return NBT_COMPOUND, body + b"\x00"


def nbt_document(root):
    tag, payload = root
    return gzip.compress(bytes([tag]) + _string_body("") + payload)


def store_with_rows(player="StartupHolder", advancement="minecraft:story/mine_stone",
                    criterion="get_stone", obtained_ms=1_700_000_000_000):
    """`<world>/advancements.dat` for one player with one obtained criterion.

    The schema is world/advancement_save.h's:
    root{DataVersion, Players:[{Name, Recipes, Advancements:[{Id,
    Criteria:[{Name, Obtained}]}]}]}. The row is `story/mine_stone` with its real
    criterion (`get_stone`, the `inventory_changed` trigger from the generated
    table), so this is a file a save would write and not merely a shape the
    reader accepts — and a reader that counted it as zero rows would say so.
    """
    criterion_row = nbt_compound((
        ("Name", nbt_string(criterion)),
        ("Obtained", nbt_long(obtained_ms)),
    ))
    advancement_row = nbt_compound((
        ("Id", nbt_string(advancement)),
        ("Criteria", nbt_list(NBT_COMPOUND, [criterion_row])),
    ))
    player_row = nbt_compound((
        ("Name", nbt_string(player)),
        ("Recipes", nbt_list(NBT_STRING, [])),
        ("Advancements", nbt_list(NBT_COMPOUND, [advancement_row])),
    ))
    return nbt_document(nbt_compound((
        ("DataVersion", nbt_int(SAVE_DATA_VERSION)),
        ("Players", nbt_list(NBT_COMPOUND, [player_row])),
    )))


def population_with_one_cow(entity_id=1, x=8.5, y=4.0, z=8.5):
    """`<world>/mobs.dat` with one mob (world/mob_save.h):

    root{DataVersion, NextEntityId, Mobs:[{Id, Type, Pos}]}. Id, Type and Pos are
    the three fields the reader cannot reconstruct a row without (every other
    field defaults), and the type is a real registry name, so the boot's own
    "mobs: loaded 1 from ..." line is the load and not a skipped row.
    """
    mob = nbt_compound((
        ("Id", nbt_int(entity_id)),
        ("Type", nbt_string("minecraft:cow")),
        ("Pos", nbt_list(NBT_DOUBLE, [nbt_double(x), nbt_double(y), nbt_double(z)])),
    ))
    return nbt_document(nbt_compound((
        ("DataVersion", nbt_int(SAVE_DATA_VERSION)),
        ("NextEntityId", nbt_long(entity_id + 1)),
        ("Mobs", nbt_list(NBT_COMPOUND, [mob])),
    )))


# --- reading the server's own boot log --------------------------------------


def read_until_port(proc, timeout):
    """`(port, lines)` — the port from the server's own log line, and every line
    it logged before that one, in order."""
    before = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                raise AssertionError(
                    "plusmc exited before it advertised a port:\n" + "".join(before))
            # An empty read on a live process means the write end is closed: no
            # more lines are coming, so wait out the deadline rather than spin.
            time.sleep(0.05)
            continue
        match = LISTENING.search(line)
        if match:
            return int(match.group(1)), before
        before.append(line.decode("utf-8", "replace"))
    raise AssertionError(
        f"plusmc did not advertise a port within {timeout:g}s:\n" + "".join(before))


def read_more(proc, seconds):
    """Whatever else the server logs within `seconds` — used only to make a
    failure say whether a line came *after* the port or never came at all."""
    out = []
    deadline = time.time() + seconds
    while True:
        remaining = deadline - time.time()
        if remaining <= 0 or not select.select([proc.stdout], [], [], remaining)[0]:
            return out
        line = proc.stdout.readline()
        if not line:
            return out
        out.append(line.decode("utf-8", "replace"))


def expect_before(proc, before, wanted, what):
    """Every line in `wanted` has to be in `before` — the log up to the port."""
    missing = [text for text in wanted if not any(text in line for line in before)]
    if not missing:
        return
    after = read_more(proc, 2.0)
    why = []
    for text in missing:
        where = ("logged only after the port" if any(text in line for line in after)
                 else "never logged")
        why.append(f"{text!r} was {where}")
    raise AssertionError(
        f"{what}: the server has to be the world it advertises before it listens, but "
        + "; ".join(why)
        + "\n--- the log up to the port ---\n" + "".join(before)
        + ("--- and after it ---\n" + "".join(after) if after else ""))


def boot(binary, data_root, world, live):
    """Start the server on `world`; return `(proc, port, lines before the port)`.

    `live` collects the processes the caller still has to reap, so an assertion
    anywhere below cannot leave a server running.
    """
    proc = wire_client.start_process(
        [binary, "--host", HOST, "--port", "0", "--offline-mode",
         "--data", str(data_root), "--world", str(world)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        env=wire_client.server_env(dict(os.environ)))
    live.append(proc)
    port, before = read_until_port(proc, timeout=30.0)
    return proc, port, before


def stop(proc, live):
    """SIGTERM and require a clean exit — the server must not be left running."""
    proc.send_signal(signal.SIGTERM)
    # A wait that times out leaves the process in `live`, where the caller's
    # cleanup still owns it; only a reaped process is dropped from that list,
    # because killing a pid this process no longer owns is not safe.
    rc = proc.wait(timeout=20)
    tail = proc.stdout.read().decode("utf-8", "replace")
    live.remove(proc)
    assert rc == 0, f"plusmc exited rc={rc} on SIGTERM:\n{tail}"


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1])
    # There is no client and no packet traffic: this test's subject is the
    # server's own startup order, which a transcript (packets, in order) does not
    # hold. Refusing a replay is the honest outcome — passing there would claim
    # the ordering had been re-examined when nothing had.
    wire_client.refuse_replay(
        "its subject is the server's startup order, which a transcript of "
        "packets does not hold — there is no client here at all")
    # Client-config assets live under the repo's data/vanilla; pass an absolute
    # path so the server finds them regardless of the caller's cwd (ctest runs
    # tests from the build directory).
    data_root = pathlib.Path(__file__).resolve().parent.parent / "data" / "vanilla"
    work = pathlib.Path(tempfile.mkdtemp(prefix="plusmc-startup-order-"))
    unreadable = work / "unreadable"
    healthy = work / "healthy"
    live = []
    try:
        # --- 1. both save files unreadable: the loads that fail ---------------
        unreadable.mkdir()
        for name in ("mobs.dat", "advancements.dat"):
            (unreadable / name).write_bytes(b"this is not a gzip NBT document\n")
        step(f"world dir (unreadable): {unreadable}")
        step("-> boot 1: both save files unreadable (not gzip NBT)")
        proc, port, before = boot(binary, data_root, unreadable, live)
        step(f"  OK it advertised port {port} after {len(before)} log line(s)")
        expect_before(proc, before,
                      LOAD_FAILURES + tuple(str(unreadable / name)
                                            for name in ("mobs.dat", "advancements.dat")),
                      "an unreadable save")
        step("  OK both failed loads, and the file each one rejected, are logged before the port")
        step("-> SIGTERM: an unreadable save is not fatal")
        stop(proc, live)
        step("  OK exit 0")

        # --- 2. a real store and population: the loads that succeed -----------
        healthy.mkdir()
        (healthy / "advancements.dat").write_bytes(store_with_rows())
        (healthy / "mobs.dat").write_bytes(population_with_one_cow())
        step(f"world dir (healthy): {healthy}")
        step("-> boot 2: a real store (one player, one obtained criterion) and one mob")
        proc, port, before = boot(binary, data_root, healthy, live)
        step(f"  OK it advertised port {port} after {len(before)} log line(s)")
        expect_before(proc, before,
                      LOADED + (f"serving world {healthy}",
                                str(healthy / "advancements.dat"),
                                str(healthy / "mobs.dat")),
                      "a healthy save")
        step("  OK both loads, and the world the boot prepared, are logged before the port")
        step("-> SIGTERM: a loaded save is not fatal either")
        stop(proc, live)
        step("  OK exit 0")

        print("\nPASS: startup order (both save files are read before the listener — "
              "unreadable and healthy)")
        return 0
    finally:
        for proc in live:
            if proc.poll() is None:
                proc.kill()
                try:
                    proc.wait(timeout=5)
                except Exception:
                    pass
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
