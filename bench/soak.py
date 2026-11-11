#!/usr/bin/env python3
"""Soak / load harness for +MC — Phase 5 (`docs/PLAN.md` §11, §8).

The plan's performance targets are numbers a *server* has to produce: "steady
20.0 TPS", "mspt never sustained above ~45 ms", "200+ players simulated", "stable
long-run RSS". None of them can be read off a unit test, so this harness drives
the real binary with N real protocol clients and reports what the engine's own
metrics line said while they were connected.

What it measures, and where each number comes from:

* **startup** — process spawn to the `listening on` line on stdout. §8's target
  is <5 s including the vanilla-data load.
* **join** — the wall time until every bot has reached play (the last
  `Synchronize Player Position`). This is the connection-thread path, not the
  sim, so it is reported separately from the tick numbers.
* **tick metrics** — the `metrics: tps=… work_avg_ms=… work_peak_ms=…
  busy_pct=… resync=… players=… rss_bytes=…` lines the loop emits (see
  `core/metrics.h`). The harness parses them; the idle window (before the bots
  arrive) and the loaded window are summarised separately, because an average
  across both says nothing about either.
* **RSS** — first and last sample, so a leak creep over a long run is visible.
* **latency** — only with `--probe-period S`: every bot sends a
  `ServerboundPingRequest` every S seconds, the server echoes it
  (`handle_ping_request`), and the time until the echo comes back is summarised
  as percentiles over the loaded window. It answers §8's "player packet ->
  response within one tick (<= 50 ms)", which no tick metric can: work per tick
  is the *sim's* cost, while this is one packet's round trip. The flag is off by
  default on purpose, so the runs recorded in `bench/baseline/` stay comparable
  (a probe adds a packet and nothing else).

* **per-tier latency** — `--proxy bungeecord|velocity` starts
  `tests/proxy_live.py` in front of the listening server and points the bots at
  it, so the same probe measures a second real TCP hop; the server is started
  with the matching forwarding flag and the record's `proxy` block names the
  mode and both ports (`docs/PHASE5.md` §4, ordered item 6).

A run stopped with Ctrl-C is not discarded: the measured window up to the signal
is summarised and written (`"interrupted": true`, `"interrupted_after_s"`), the
server log is still written, and the process exits 130. A signal before the
first window opens writes a record with no loaded window, which is honest about
what was measured rather than pretending the run completed.

Two honest bounds on what this proves, both of which the printed summary says:

1. It runs on one machine, so the sim and the bots share the CPU. A loaded
   result is therefore a *lower* bound on the server's capability, and the
   `--bots` count is bounded by the harness's own CPU as much as by the server.
2. It is not a cross-host or quiet-box rig. 1000 clients is reachable with
   `--bot-processes 10` on one four-core box (`docs/PHASE5.md` §4, the
   1000-player record), but the workers share the server's cores, so loaded
   figures stay lower bounds stamped with the ambient load. 1000 *movers* is a
   different cell: the per-packet Python cost is still what would cap it, and
   PLAN §8's claim is measured for a stationary 1000-player population only.

Two bot engines, both real protocol clients:

* **`--engine loop` (default)** — `bench/botnet.py`: one thread, one
  `selectors` loop, N non-blocking sockets. Use this for numbers.
* **`--engine threads`** — one Python thread per bot (the original engine). It
  is kept because it is what produced the first baseline, and because the two
  engines disagreeing is itself the measurement: at 60 bots the threaded engine
  reported 12.87 ms of work per tick against the loop engine's 0.8 ms, which is
  the harness competing with the sim rather than the sim's cost.

Usage:
    bench/soak.py [--bin PATH] [--bots N] [--seconds S] [--generated SEED]
                  [--world DIR] [--data DIR] [--mobs] [--view-distance N]
                  [--json FILE] [--log FILE] [--engine loop|threads]
                  [--probe-period S] [--quiet]

Defaults: the tree's Release build (`build/src/plusmc`), 20 bots, 10 s of
loaded time, the void world, natural spawning off (so two runs are comparable).
"""
import argparse
import hashlib
import json
import math
import os
import pathlib
import re
import signal
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tests"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wire_client  # noqa: E402  (the shared test client: transport, varint helpers)
import botnet  # noqa: E402  (the cheap non-blocking bot driver)

try:
    import resource  # POSIX only: the CPU-time denominator for a generation rate.
except ImportError:  # pragma: no cover - the harness is a POSIX tool (PLAN §3.5)
    resource = None

PROTO = 777
HOST = "127.0.0.1"
# Serverbound play ids (777, the packet table the wire tests use too).
P_POSITION = 0x1E
P_POSLOOK = 0x1F
P_LOOK = 0x20
# Clientbound play ids.
CB_SYNC_POS = 0x49
CB_KEEPALIVE = 0x2D

METRICS = re.compile(r"metrics: (.*)$")
LISTENING = re.compile(r"listening on [\d.]+:(\d+)")
# The write path's shutdown line (docs/WRITE_PATH.md): how many chunk sends
# this process made and how many of them reused a body or a whole compressed
# frame another connection had built. Parsed into the record because it is the
# one number on that path wall-clock time on a shared box cannot move.
CHUNK_SHARING = re.compile(
    r"chunk sharing: (\d+) sends, (\d+) bodies reused \(([\d.]+)%\), "
    r"(\d+) frames reused \(([\d.]+)%\)")
# The generation pool's shutdown line (world/chunk_generation.h): what it built
# ahead of the view, how many asks were already satisfied, and how much the
# queue held when the bots left. Recorded because `generation` above is the
# *store's* view — stored columns per second — while this is the work the pool
# did to produce them, and the two are what an inline-vs-pool A/B differ in.
GENERATION_POOL = re.compile(
    r"chunk generation: (\d+) columns generated by (\d+) thread\(s\), "
    r"(\d+) requested, (\d+) already stored, (\d+) dropped, (\d+) failed, "
    r"(\d+) queued")
# The biome memo's shutdown line (world/worldgen_biome.cpp): hits against cold
# `Climate.RTree` searches, entries held, and how the entries are spread over
# the threads that asked. This is the instrument that attributes a generation
# speedup to the memo rather than to a cold walk getting cheaper — a profile
# shows the searches that ran and cannot show the ones that did not.
BIOME_MEMO = re.compile(
    r"biome memo: (\d+) hits, (\d+) misses \(([\d.]+)% hit rate\), "
    r"(\d+) entr\(ies\) across (\d+) thread\(s\) \(most (\d+)\), "
    r"(\d+) refill\(s\)")
# The natural spawn pass's shutdown line (Phase 4 milestone C,
# src/server/session.cpp): the mobs the pass created, the passes that walked
# candidates, and the candidate chunks asked of the world — tickable against
# refused. The pair of chunk counters is what a stored-column A/B is read from:
# the geometric square a player watches against the part of it the store will
# tick (docs/PHASE5.md). Silent for a run whose spawning never walked a
# candidate, which is what `PLUSMC_MOB_SPAWNING=0` produces.
NATURAL_SPAWN = re.compile(
    r"natural spawn: (\d+) mob\(s\) from (\d+) pass\(es\), "
    r"(\d+) candidate chunk\(s\): (\d+) tickable, (\d+) refused")
# The mob relay's shutdown line (docs/MOB_VISIBILITY.md): payloads the tick
# built against the client writes they became, the relationships that started,
# and the largest viewer set a mob held. Same reason as CHUNK_SHARING — it is
# the load-independent statement of what the tracking gate did.
MOB_TRACKING = re.compile(
    r"mob tracking: (\d+) payloads built, (\d+) client writes \(([\d.]+) per payload\), "
    r"(\d+) pairings, peak (\d+) viewers")
# The item relay's, same shape (docs/MOB_VISIBILITY.md). Silent for a run whose
# bots never drop or break anything.
ITEM_TRACKING = re.compile(
    r"item tracking: (\d+) payloads built, (\d+) client writes \(([\d.]+) per payload\), "
    r"(\d+) pairings, peak (\d+) viewers")
# The four later relays on the same map (docs/MOB_VISIBILITY.md): experience
# orbs, primed TNT, arrows/thrown items and thrown potions (with the clouds a
# lingering one leaves). One regex because the kind is in the line; silent for
# a run whose bots never earn, prime, fire or throw any of them.
TRANSIENT_TRACKING = re.compile(
    r"(orb|tnt|projectile|potion|cloud) tracking: (\d+) payloads built, "
    r"(\d+) client writes \(([\d.]+) per payload\), (\d+) pairings, peak (\d+) viewers")


def load1():
    """The box's 1-minute load average, or None where the platform has none.

    Recorded in every run because this harness shares the machine with whatever
    else is running (docs/PHASE5.md §5). A record taken while a sibling session is
    compiling or soaking is not comparable with one taken on a quiet box, and
    nothing but this number says which one it is — a result is a *lower* bound on
    the server either way, but a contended record cannot be re-derived later.
    """
    try:
        return round(os.getloadavg()[0], 2)
    except (OSError, AttributeError):
        return None


# ---- the quiet-box gate ------------------------------------------------------
#
# A cell is only comparable with another cell taken on an equally idle box, and
# the 1-minute load average cannot say whether this one is: a sibling soak is a
# duty cycle, so the average dips between its cells while the CPU is still busy,
# and a compile disturbs the memory system at a load the average barely moves.
# (Six cells were once labelled "quiet" at load 50-187 that way.) So the gate
# reads the kernel's own idle share, and separately refuses to start beside the
# two things that make a benchmark cell unusable even at a low average: a
# compiler in the process table and another soak harness or server running.
#
# It lives here rather than in the shell script that drove one experiment, so
# every future A/B inherits it: `--wait-quiet` is the switch, `--quiet-idle`,
# `--quiet-load` and `--quiet-hold` are its thresholds, and a record that did
# start while contended says so in `busy`/`busy_reasons` and `ambient`.
BUILD_TOOL_NAMES = {
    "cc1", "cc1plus", "cc1obj", "clang", "clang++", "gcc", "g++", "cc", "c++",
    "ld", "ld64", "ld.lld", "lld", "collect2", "ninja", "cmake", "make", "gmake",
    "xcodebuild", "swift-frontend", "swiftc", "rustc", "cargo", "ar",
}


def cpu_idle_share(sample_seconds=1.0):
    """The CPU's own idle share over one sample, in percent, or None.

    macOS answers with `top -l 2` (the first sample is since boot, the second is
    the interval — the second is the one a gate can use) and Linux with two
    reads of `/proc/stat`. None where neither exists, so the gate falls back to
    its other checks instead of passing on a guess.
    """
    if sys.platform == "darwin":
        try:
            out = subprocess.run(["top", "-l", "2", "-n", "0", "-s", f"{sample_seconds:g}"],
                                 capture_output=True, text=True, timeout=30).stdout
        except (OSError, subprocess.SubprocessError):
            return None
        idle = None
        for line in out.splitlines():
            if line.startswith("CPU usage:"):
                hit = re.search(r"([\d.]+)%\s+idle", line)
                if hit:
                    idle = float(hit.group(1))
        return idle
    try:
        def read_cpu_line():
            fields = pathlib.Path("/proc/stat").read_text().splitlines()[0].split()[1:]
            return [int(v) for v in fields]
        first = read_cpu_line()
        time.sleep(sample_seconds)
        second = read_cpu_line()
    except (OSError, ValueError, IndexError):
        return None
    total = sum(second) - sum(first)
    # user nice system idle iowait irq softirq steal: idle is fields 3 and 4.
    idle_ticks = (second[3] + second[4]) - (first[3] + first[4])
    if total <= 0 or idle_ticks < 0:
        return None
    return round(100.0 * idle_ticks / total, 1)


def process_table():
    """(pid, %cpu, full command line) for every process, or [] where ps fails."""
    try:
        out = subprocess.run(["ps", "-Ao", "pid=,pcpu=,command="],
                             capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    rows = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) < 3:
            continue
        try:
            rows.append((int(parts[0]), float(parts[1]), parts[2]))
        except ValueError:
            continue
    return rows


def _ancestor_pids():
    """This harness and every process above it.

    Their command lines contain `bench/soak.py` and the server binary's name by
    construction, and the gate must not read its own name as a sibling.
    """
    pids, pid = {os.getpid()}, os.getpid()
    for _ in range(16):
        try:
            out = subprocess.run(["ps", "-o", "ppid=", "-p", str(pid)],
                                 capture_output=True, text=True, timeout=10).stdout.strip()
            parent = int(out)
        except (OSError, ValueError, subprocess.SubprocessError):
            break
        if parent <= 0 or parent in pids:
            break
        pids.add(parent)
        pid = parent
    return pids


def quiet_blockers(procs, binary_name):
    """The processes that make a cell unrunnable, as [{cpu, command}].

    Two families, both at one percent of a core or more: a build toolchain (a
    compile moves the memory system even when it leaves the CPU idle) and
    another harness or generated world (the sibling soak whose duty cycle the
    load average was hiding). An *idle* dev server is not contention and does
    not count, which is why this is a CPU threshold and not a name match.
    """
    mine = _ancestor_pids()
    found = []
    for pid, cpu, command in procs:
        if pid in mine or cpu < 1.0:
            continue
        argv = command.split()
        argv0 = os.path.basename(argv[0]) if argv else ""
        tool = argv0 in BUILD_TOOL_NAMES
        sibling = (binary_name and argv0 == binary_name) \
            or "bench/soak.py" in command or "bench/botnet.py" in command
        if tool or sibling:
            found.append((cpu, command))
    found.sort(key=lambda row: -row[0])
    return [{"cpu": round(cpu, 1), "command": cmd[:140]} for cpu, cmd in found[:6]]


def quiet_state(binary_name):
    """One sample of everything the gate checks, in the record's own shape."""
    return {"load1": load1(), "idle_pct": cpu_idle_share(),
            "blockers": quiet_blockers(process_table(), binary_name)}


def quiet_reasons(state, load_limit, idle_floor):
    """Why `state` is not a quiet box, or [] when it is."""
    reasons = []
    if state["idle_pct"] is not None and idle_floor > 0 and state["idle_pct"] < idle_floor:
        reasons.append(f"CPU {state['idle_pct']:.1f}% idle (floor {idle_floor:g}%)")
    if state["load1"] is not None and load_limit is not None and state["load1"] >= load_limit:
        reasons.append(f"load {state['load1']:.2f} (limit {load_limit:g})")
    for row in state["blockers"]:
        argv = row["command"].split()
        name = os.path.basename(argv[0]) if argv else row["command"]
        reasons.append(f"{row['cpu']:.0f}% {name}")
    return reasons


def wait_for_quiet(args, binary_name):
    """Wait for the gate to hold; returns (state, reasons, held).

    `reasons` is empty only when the box was quiet for `--quiet-hold` consecutive
    samples. A gate that expires is not a failure — the cell runs anyway, and
    the record carries the reasons — but it is never called quiet.
    """
    deadline = time.time() + args.wait_quiet
    hold = max(1, args.quiet_hold)
    held = 0
    state = {"load1": None, "idle_pct": None, "blockers": []}
    while True:
        state = quiet_state(binary_name)
        reasons = quiet_reasons(state, args.quiet_load, args.quiet_idle)
        held = held + 1 if not reasons else 0
        idle = "n/a" if state["idle_pct"] is None else f"{state['idle_pct']:.1f}%"
        load = "n/a" if state["load1"] is None else f"{state['load1']:.2f}"
        if held >= hold:
            print(f"[quiet] idle {idle}, load {load} (held {held}/{hold})", flush=True)
            return state, [], held
        print((f"[wait]  idle {idle}, load {load}: {'; '.join(reasons[:3])}" if reasons else
               f"[gate]  idle {idle}, load {load} (held {held}/{hold})"), flush=True)
        if time.time() >= deadline:
            return state, reasons or [f"the gate did not hold {hold} samples"], held
        time.sleep(max(0.5, args.quiet_interval))


def children_cpu_seconds():
    """CPU seconds this process's reaped children have spent, or None.

    `RUSAGE_CHILDREN` is the accounting a harness can actually read for a server
    it started itself and later waited on. On the loop-engine path the bots run
    inside *this* process (`bench/botnet.py`), so the delta around the server's
    life is the server's own user+sys time — a denominator that survives a shared
    box, because contention inflates wall seconds while it barely moves the CPU
    seconds the work itself needs. None where the platform has no `resource`.
    """
    if resource is None:
        return None
    try:
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    except (AttributeError, OSError):
        return None
    return usage.ru_utime + usage.ru_stime


def read_cpu_seconds(pid):
    """One process's cumulative CPU time, from the OS, or None.

    It is sampled twice a second for a server that is also being measured: a
    whole-run total cannot say what the *generation* cost, because the idle tail
    after every view is full is most of a soak's life. Sampling it beside the
    metrics lines lets the fill's own CPU seconds be taken as a difference, and
    the window the sampler's last reading brackets is what `generation_summary`
    divides by.

    Two readers, because neither is enough alone: `/proc/<pid>/stat` is exact to
    the clock tick on Linux but absent on macOS, and `ps -o time=` is the only
    one macOS has — while on Linux its procps prints whole seconds, so a
    three-second generation fill reads as zero CPU and the rate it divides
    vanishes into a null. The exact reader is therefore preferred where it
    exists, and `ps` is the fallback.
    """
    from_proc = read_cpu_seconds_proc(pid)
    if from_proc is not None:
        return from_proc
    try:
        out = subprocess.run(["ps", "-o", "time=", "-p", str(pid)],
                             capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    if not out:
        return None
    try:
        # `[[HH:]MM:]SS.ss` — the fields are positional, not fixed-width.
        total = 0.0
        for part in out.split(":"):
            total = total * 60.0 + float(part)
    except ValueError:
        return None
    return total


def read_cpu_seconds_proc(pid):
    """Linux's `/proc/<pid>/stat`, the same reading without procps.

    utime and stime are fields 14 and 15 of the line -- the first two after the
    parenthesised `comm`, which may itself contain spaces and a `)`, hence the
    split at the *last* `)`.
    """
    try:
        stat = pathlib.Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return None
    fields = stat[stat.rfind(")") + 2:].split()
    try:
        ticks = int(fields[11]) + int(fields[12])
        hz = os.sysconf("SC_CLK_TCK")
    except (IndexError, ValueError, AttributeError, OSError):
        return None
    if not hz:
        return None
    return ticks / float(hz)


def parse_metrics(line):
    """`metrics: tps=20.00 work_avg_ms=0.4 …` -> dict of numbers."""
    m = METRICS.search(line)
    if not m:
        return None
    out = {}
    for pair in m.group(1).split():
        if "=" not in pair:
            continue
        key, value = pair.split("=", 1)
        try:
            out[key] = float(value)
        except ValueError:
            continue
    return out


def percentile(values, p):
    """The p-th percentile of `values` by nearest rank, or None for no samples.
    Nearest rank (not interpolated) on purpose: with a window's worth of probes
    it is the sample a reader could find again in the record."""
    if not values:
        return None
    ordered = sorted(values)
    k = min(len(ordered) - 1, int(round(p / 100.0 * (len(ordered) - 1))))
    return ordered[k]


def latency_summary(samples, target_ms=50.0):
    """`Fleet.drain_rtts()` output -> the numbers §8's latency target is stated
    in. `samples` is `(answered_at, rtt_ms)`, oldest first.

    The share inside `target_ms` is reported next to the percentiles because
    that share *is* the plan's target ("within one tick", i.e. <= 50 ms); a p99
    alone would leave the reader to decide how much of the tail is allowed.
    """
    rtts = [rtt for _, rtt in samples]
    if not rtts:
        return None
    return {
        "n": len(rtts),
        "min_ms": round(min(rtts), 3),
        "p50_ms": round(percentile(rtts, 50), 3),
        "p90_ms": round(percentile(rtts, 90), 3),
        "p99_ms": round(percentile(rtts, 99), 3),
        "max_ms": round(max(rtts), 3),
        "target_ms": target_ms,
        "within_target_pct": round(100.0 * sum(1 for r in rtts if r <= target_ms) / len(rtts), 1),
    }


class Bot:
    """One offline client: handshake -> login -> configuration -> play, then
    movement at 20 Hz with keep-alives answered."""

    def __init__(self, port, name, tag):
        self.name = name
        self.tag = tag
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.threshold = -1
        self.buf = b""
        self.sent = 0
        self.keepalives = 0

    # --- framing ---------------------------------------------------------
    def send_pkt(self, pid, body=b""):
        payload = wire_client.varint(pid) + body
        if self.threshold < 0:
            frame = wire_client.varint(len(payload)) + payload
        else:
            inner = wire_client.varint(0) + payload
            frame = wire_client.varint(len(inner)) + inner
        self.sock.sendall(frame)
        self.sent += 1

    def read_pkt(self):
        while True:
            pid, body, consumed = self._parse()
            if pid is not None:
                self.buf = self.buf[consumed:]
                return pid, body
            chunk = self.sock.recv(65536)
            if not chunk:
                raise AssertionError(f"{self.name}: unexpected EOF")
            self.buf += chunk

    def _parse(self):
        buf = self.buf
        if not buf:
            return None, None, 0
        length, nbytes = wire_client.rd_varint(buf, 0)
        if nbytes <= 0 or len(buf) < nbytes + length:
            return None, None, 0
        body = buf[nbytes:nbytes + length]
        consumed = nbytes + length
        if self.threshold < 0:
            pid, off = wire_client.rd_varint(body)
            return pid, body[off:], consumed
        import zlib
        declared, dn = wire_client.rd_varint(body, 0)
        data = body[dn:]
        if declared != 0:
            data = zlib.decompress(data)
        pid, off = wire_client.rd_varint(data)
        return pid, data[off:], consumed

    # --- the join --------------------------------------------------------
    def join(self):
        handshake = (wire_client.varint(PROTO) + wire_client.varint(len(HOST)) +
                     HOST.encode() + struct.pack(">H", 0) + wire_client.varint(2))
        self.send_pkt(0, handshake)
        self.send_pkt(0x00, wire_client.varint(len(self.name)) + self.name.encode() + b"\x00" * 16)
        while True:
            pid, body = self.read_pkt()
            if pid == 0x03:
                self.threshold, _ = wire_client.rd_varint(body)
            elif pid == 0x02:
                self.send_pkt(0x03)  # login acknowledged
                break
            # Encryption request never arrives in offline mode.
        while True:
            pid, body = self.read_pkt()
            if pid == 0x0E:  # known packs -> echo them
                count, i = wire_client.rd_varint(body)
                packs = []
                for _ in range(count):
                    ns, i = self._string(body, i)
                    pk, i = self._string(body, i)
                    ver, i = self._string(body, i)
                    packs.append((ns, pk, ver))
                out = wire_client.varint(len(packs))
                for ns, pk, ver in packs:
                    out += (wire_client.varint(len(ns)) + ns.encode() +
                            wire_client.varint(len(pk)) + pk.encode() +
                            wire_client.varint(len(ver)) + ver.encode())
                self.send_pkt(0x07, out)
            elif pid == 0x03:  # finish configuration
                self.send_pkt(0x03)
                break
        while True:
            pid, _ = self.read_pkt()
            if pid == CB_SYNC_POS:
                return

    @staticmethod
    def _string(body, i):
        length, i = wire_client.rd_varint(body, i)
        return body[i:i + length].decode("utf-8"), i + length

    # --- the load --------------------------------------------------------
    def stream(self, until, period=0.05):
        x = 8.0 + (self.tag % 8) * 0.5
        z = 8.0 + (self.tag % 5) * 0.5
        self.sock.settimeout(0.02)
        sent_at = time.time()
        while time.time() < until:
            self.send_pkt(P_POSITION, struct.pack(">ddd", x, 100.0, z) + b"\x01")
            self.send_pkt(P_POSLOOK, struct.pack(">ddd", x, 100.2, z) +
                          struct.pack(">ff", 45.0 * self.tag, 0.0) + b"\x01")
            self.send_pkt(P_LOOK, struct.pack(">ff", 90.0 * self.tag, 10.0) + b"\x01")
            # Drain whatever arrived (keep-alives must be answered: an
            # unanswered one drops the bot after the reply window, and the
            # socket's backstop is derived from that cadence — 30 s at the
            # shipped 10 s + 15 s, see docs/PHASE1.md).
            while True:
                try:
                    pid, body = self.read_pkt()
                except (socket.timeout, TimeoutError):
                    break
                except AssertionError:
                    return
                if pid == CB_KEEPALIVE:
                    payload, _ = wire_client.rd_varint(body)
                    self.send_pkt(0x1C, wire_client.varint(payload))
                    self.keepalives += 1
            time.sleep(max(0.0, sent_at + period - time.time()))
            sent_at += period

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def file_sha256(path):
    """The build a record is *about*, not the path it names.

    Several sessions share this machine and `build/` is rebuilt between runs, so
    a record that says only `build/src/plusmc` cannot be reproduced or compared:
    the same path was a different binary an hour earlier. Hashing the executable
    is what ties a number to the bytes that produced it (docs/PHASE5.md). Six
    megabytes of SHA-256 is a few milliseconds against a run of seconds, and a
    missing or unreadable binary yields None rather than failing the run.
    """
    try:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for block in iter(lambda: f.read(1 << 20), b""):
                h.update(block)
        return h.hexdigest()
    except OSError:
        return None


def bot_thread(port, name, tag, until_box, failures, joined):
    try:
        bot = Bot(port, name, tag)
        bot.join()
        joined.append(tag)
        bot.stream(until_box[0])
        bot.close()
    except Exception as exc:  # noqa: BLE001 - the harness must report, not hide
        failures.append(f"{name}: {exc!r}")


# Counters that mean the same thing summed over K driver processes: each bot's
# frames reach exactly one of them. `chunks_distinct` is deliberately not here —
# a chunk delivered to two processes is still one chunk — so the K key sets are
# unioned instead, and both the union and the sum are carried in the record
# (see merge_worker_snapshots).
ADDITIVE_SNAPSHOT_KEYS = ("bots", "connected", "joined", "keepalives", "packets_sent",
                         "chunks_received", "chunk_body_bytes", "packets_received",
                         "received_bytes", "probes_sent", "probes_answered",
                         "probes_pending", "probes_orphan")


def merge_worker_snapshots(snaps):
    """K workers' `Fleet.snapshot()`s of the same instant -> one snapshot.

    `chunks_distinct` is a set *union*, and a union over processes is not a sum.
    Every worker hands its own key pairs over (`bench/botnet.py --keys`) and the
    union is taken here, so the number is the same one a single fleet of the same
    population reports.

    Counting could not do this. An earlier version tried: `max` for a cluster
    (every worker's bots stand on the same cells) and `sum` for a spread
    population (different cells, so disjoint sets). The second half is false, and
    the reason is not the layout. A joining client is streamed the chunks around
    the **world spawn** before its own position packet has arrived, so every
    worker's set contains that block on top of its own cells — at view distance 2
    a bot placed 512 blocks from spawn is handed 50 chunks, its own 5x5 plus
    spawn's 5x5 — and the K sets overlap by that much however far apart the cells
    are (`bench/botnet.py`'s `Fleet.snapshot`). Cells that are close enough to
    overlap at a deep view distance add their own overlap on top (`--spread 256`
    is 16 chunks, and view distance 8 is 17 chunks wide). Both effects are
    invisible to a count and free to a set.

    `chunks_distinct_sum_bound` keeps what the count would have said, so a record
    still shows how much the layouts' sets overlap.
    """
    merged = {key: sum(s[key] for s in snaps) for key in ADDITIVE_SNAPSHOT_KEYS}
    if all("chunk_keys" in s for s in snaps):
        keys = set()
        for s in snaps:
            keys.update((x, z) for x, z in s["chunk_keys"])
        merged["chunks_distinct"] = len(keys)
        merged["chunks_distinct_rule"] = "exact union of the K workers' key pairs"
    else:
        # Only reachable if a worker's record was written by a build without
        # `--keys`. Say so rather than quietly publishing a bound as the count.
        merged["chunks_distinct"] = max(s["chunks_distinct"] for s in snaps)
        merged["chunks_distinct_rule"] = "largest worker's set: an UPPER-BOUND bug, " \
                                         "the workers were not told to carry their keys"
    merged["chunks_distinct_sum_bound"] = sum(s["chunks_distinct"] for s in snaps)
    merged["chunks_distinct_per_worker"] = [s["chunks_distinct"] for s in snaps]
    # The split itself, so a record says one fleet of 200 was four workers of 50:
    # `chunks_distinct_per_worker` alone cannot be read as a bot count.
    merged["workers"] = len(snaps)
    merged["bots_per_worker"] = [s["bots"] for s in snaps]
    merged["chunk_keys_max"] = max(s["chunk_keys_max"] for s in snaps)
    merged["stationary"] = snaps[0]["stationary"]
    merged["spread"] = snaps[0]["spread"]
    merged["failures"] = [f for s in snaps for f in s["failures"]]
    return merged


def drive_workers(args, port, quiet):
    """The multi-process form of the loop engine: K `bench/botnet.py` workers
    driving one server over one wall-clock window (docs/PHASE5.md §6 item 1).

    The single-process driver varint-decodes and zlib-decompresses every
    clientbound frame in one Python thread, so past a couple of hundred movers it
    — and not the server — is what caps the wire. K processes divide exactly
    that. They have to measure *one* interval, so the window is an absolute
    stamp computed here once and handed to every worker through its `--go` file:
    each joins its share of the population, touches its ready file, and then runs
    the same plan. The population also stays one population: the spread grid's
    origin and width are global, so K workers lay their bots out on the cells the
    single-fleet run would have used.

    Returns `(join_s, joined, bot_stats, traffic_marks, phases_seen, latency,
    failures)` — the same locals the single-process branch of `main` leaves.
    """
    workdir = pathlib.Path(tempfile.mkdtemp(prefix="plusmc-workers-"))
    per = args.bots // args.bot_processes
    counts = [per] * args.bot_processes
    for i in range(args.bots - per * args.bot_processes):
        counts[i] += 1
    grid_cols = max(1, int(math.ceil(math.sqrt(args.bots)))) if args.spread else None

    procs, gos, outs, readies = [], [], [], []
    offset = 0
    for i, count in enumerate(counts):
        ready = workdir / f"ready{i}"
        go = workdir / f"go{i}"
        out = workdir / f"out{i}.json"
        argv = [sys.executable, str(pathlib.Path(botnet.__file__)), str(port),
                "--bots", str(count), "--seconds", str(args.seconds),
                "--grid-offset", str(offset), "--ready", str(ready),
                "--go", str(go), "--json", str(out)]
        if args.stationary:
            argv.append("--stationary")
        if args.spread:
            argv += ["--spread", str(args.spread), "--grid-cols", str(grid_cols)]
        if args.probe_period:
            argv += ["--probe-period", str(args.probe_period)]
        if args.relayout is not None:
            argv += ["--relayout", str(args.relayout)]
            if args.prewarm:
                argv.append("--prewarm")
        if quiet:
            argv.append("--quiet")
        argv.append("--keys")  # the parent unions the pairs; counts cannot be summed
        procs.append(subprocess.Popen(argv, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, text=True))
        readies.append(ready)
        gos.append(go)
        outs.append(out)
        offset += count
    if not quiet:
        print(f"soak: {args.bot_processes} bot process(es) x {counts[0]} bot(s)"
              + (f" (grid {grid_cols} wide)" if grid_cols else ""))

    join_started = time.time()
    try:
        ready_deadline = time.time() + 300
        while time.time() < ready_deadline:
            if (all(r.exists() for r in readies)
                    or any(p.poll() is not None for p in procs)):
                break
            time.sleep(0.1)
        join_s = time.time() - join_started

        # The plan. `relayout_at` gives the paired arm the same settle the
        # single-process path gives it, because the teleport streams a region
        # per bot and that cost must not land in either measured window.
        at = time.time() + max(1.0, args.settle_after_join)
        plan = {"at": at}
        if args.relayout is not None:
            plan["relayout_at"] = at + args.seconds + args.settle_after_join
        for go in gos:
            go.write_text(json.dumps(plan))

        budget = (300.0 + join_s + 4.0 * args.seconds
                  + (args.settle_after_join if args.relayout is not None else 0.0))
        stop_by = time.time() + budget
        for proc in procs:
            try:
                proc.wait(timeout=max(30.0, stop_by - time.time()))
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    except BaseException:
        # A Ctrl-C (or anything else) must not leave K bot processes writing to
        # the server after the harness has decided the run is over.
        for proc in procs:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        raise

    records = []
    for i, out in enumerate(outs):
        try:
            records.append(json.loads(out.read_text()))
        except (OSError, ValueError) as exc:  # a worker that died left no record
            records.append(None)
            if not quiet:
                print(f"soak: worker {i} wrote no record ({exc!r})")

    live = [r for r in records if r]
    joined = list(range(sum(r["joined"] for r in live)))
    failures = [f for r in live for f in r.get("failures", [])]
    failures += [f"worker {i}: no record" for i, r in enumerate(records) if not r]

    # The windows. Bounds come from the plan every worker was given, so the
    # server's own metrics are read for exactly the interval the drivers ran.
    bounds = [(plan["at"], plan["at"] + args.seconds)]
    labels = [f"spread{args.spread:.0f}" if args.spread else "clustered"]
    if args.relayout is not None:
        bounds.append((plan["relayout_at"], plan["relayout_at"] + args.seconds))
        labels.append(f"spread{args.relayout:.0f}" if args.relayout else "clustered")
    traffic_marks, phases_seen = [], []
    rtts = []
    for i, (start, end) in enumerate(bounds):
        before = [r["windows"][i]["before"] for r in live if len(r["windows"]) > i]
        after = [r["windows"][i]["after"] for r in live if len(r["windows"]) > i]
        if before and after:
            traffic_marks.append((start, end, merge_worker_snapshots(before),
                                  merge_worker_snapshots(after)))
        phases_seen.append((labels[i], start, end))
        for r in live:
            if len(r["windows"]) > i:
                rtts += r["windows"][i]["rtts_ms"]

    bot_stats = None
    last = [r["windows"][-1]["after"] for r in live if r["windows"]]
    if last:
        bot_stats = merge_worker_snapshots(last)
        bot_stats["failures"] = failures
    latency = latency_summary([(0.0, value) for value in rtts]) if rtts else None
    return join_s, joined, bot_stats, traffic_marks, phases_seen, latency, failures


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=str(REPO / "build" / "src" / "plusmc"),
                    help="the server binary (default: build/src/plusmc)")
    ap.add_argument("--bots", type=int, default=20)
    ap.add_argument("--seconds", type=float, default=10.0,
                    help="how long the bots stay connected (default 10)")
    ap.add_argument("--generated", type=int, default=None, metavar="SEED",
                    help="generate the world from SEED instead of the void world")
    ap.add_argument("--world", default=None, help="serve an existing Anvil world")
    ap.add_argument("--data", default=str(REPO / "data" / "vanilla"),
                    help="vanilla data root the server advertises and loads its client "
                         "config from (default: data/vanilla). A test that only needs "
                         "clients to join can pass a minimal version.json + clientconfig "
                         "root instead of the full extract.")
    ap.add_argument("--mobs", action="store_true",
                    help="leave natural spawning on (off by default: two runs must compare)")
    ap.add_argument("--stationary", action="store_true",
                    help="bots join and stand still: separates entity-tick cost from the "
                         "chunk streaming a moving player causes (loop engine only)")
    ap.add_argument("--spread", type=float, default=0.0, metavar="BLOCKS",
                    help="place the bots on a square grid BLOCKS apart instead of one "
                         "cluster, so every mover is outside every other's view (256 "
                         "clears the 128-block visibility radius with room to spare). "
                         "With --stationary it is the control for the extra chunks a "
                         "spread population loads, so (moving - stationary) isolates the "
                         "relay. Loop engine only (default 0 = the original cluster).")
    ap.add_argument("--view-distance", type=int, default=8)
    ap.add_argument("--settle", type=float, default=1.5,
                    help="seconds to let the server tick alone before the bots arrive")
    ap.add_argument("--settle-after-join", type=float, default=3.0,
                    help="seconds to keep the bots connected *without* counting "
                         "them: a join storm leaves the server streaming chunks "
                         "for a while, and folding that transient into the "
                         "steady-state window is how a per-player cost gets "
                         "reported several times too high (default 3)")
    ap.add_argument("--json", default=None, help="where to write the run record")
    ap.add_argument("--log", default=None, help="where to keep the server's own log")
    ap.add_argument("--engine", choices=("loop", "threads"), default="loop",
                    help="bot engine: 'loop' is one thread with N non-blocking sockets "
                         "(default, use it for numbers); 'threads' is one Python thread "
                         "per bot, the original engine, which competes with the sim")
    ap.add_argument("--probe-period", type=float, default=0.0, metavar="SECONDS",
                    help="send a latency probe every SECONDS per bot and report the "
                         "round trip (0 = off, the default, so a run without it is the "
                         "run the recorded baseline was measured with). It answers "
                         "§8's 'player packet -> response within one tick': the pong is "
                         "written by the session's own handler, so the number covers "
                         "the kernel, both threads and this driver's select loop. "
                         "Loop engine only.")
    ap.add_argument("--proxy", choices=("bungeecord", "velocity"), default=None,
                    help="put every bot behind tests/proxy_live.py's forwarding proxy, "
                         "so the probe's round trip crosses a second real TCP hop (the "
                         "per-tier latency table, docs/PHASE5.md ordered item 6). The "
                         "server is started with the matching forwarding flag and the "
                         "proxy derives a distinct UUID per connection, so the bot "
                         "fleet stays a fleet.")
    ap.add_argument("--proxy-secret", default="soak-secret",
                    help="velocity: the shared secret both ends sign with "
                         "(default: soak-secret)")
    ap.add_argument("--relay-loops", type=int, default=1, metavar="N",
                    help="proxy only: split the relayed sessions over N selector "
                         "loops in tests/proxy_live.py (default 1, the layout the "
                         "earlier per-tier records used). The loops share the GIL, "
                         "so this bounds each loop's ready-set queue rather than "
                         "adding CPU; the arm is recorded in `proxy`.")
    ap.add_argument("--relayout", type=float, default=None, metavar="BLOCKS",
                    help="the *paired* layout experiment: the bots join and hold one "
                         "window, are then re-placed BLOCKS apart, and hold a second "
                         "window in the same process. Two windows, one population: the "
                         "ambient load and drift a shared box has land on both arms "
                         "instead of being compared across two runs. The first window "
                         "is the join layout (clustered, or spread with --spread); "
                         "--relayout 0 re-clusters, which is how the arms are run in "
                         "the opposite order to check for an order effect. Loop engine "
                         "only (absent = one window, as before)")
    ap.add_argument("--prewarm", action="store_true",
                    help="with --relayout, visit the second layout *before* either "
                         "measured window and come back: a layout change is a "
                         "teleport that streams a whole region per player, and if "
                         "that runs between the windows its cost lands in the second "
                         "one. The per-session chunk set is cumulative, so the return "
                         "trip for the real second window then streams nothing new")
    ap.add_argument("--wait-quiet", type=float, default=0.0, metavar="SECONDS",
                    help="wait up to SECONDS for the quiet-box gate before starting, so "
                         "a record is taken on a box that is not also compiling or "
                         "soaking (0 = start now). The gate is the CPU's own idle share "
                         "(--quiet-idle), the 1-minute load average (--quiet-load) and a "
                         "process-table check for build tools and sibling soaks, held "
                         "for --quiet-hold samples. Several sessions share this machine "
                         "(docs/PHASE5.md §5).")
    ap.add_argument("--quiet-load", type=float, default=2.0, metavar="LOAD",
                    help="the 1-minute load average the gate requires (default 2.0; "
                         "the box this harness was calibrated on has four cores)")
    ap.add_argument("--quiet-idle", type=float, default=85.0, metavar="PCT",
                    help="the CPU idle share, in percent, the gate requires (default "
                         "85; 0 disables this check). This, not the load average, is "
                         "what a sibling soak's duty cycle cannot fool")
    ap.add_argument("--quiet-hold", type=int, default=3, metavar="N",
                    help="how many consecutive gate samples must be quiet before the "
                         "cell starts (default 3): one dip between a sibling's cells is "
                         "not a quiet box")
    ap.add_argument("--quiet-interval", type=float, default=5.0, metavar="SECONDS",
                    help="seconds between gate samples (default 5)")
    ap.add_argument("--max-rss", type=float, default=0.0, metavar="MB",
                    help="watch the server's RSS line and SIGKILL the server if it "
                         "passes MB: a view-distance-8 spread population can ask for "
                         "gigabytes of chunks, and a ceiling this harness enforces is "
                         "what keeps the machine (and the record) finite. The record "
                         "says it tripped, and the bots' EOFs are then the run's "
                         "failures rather than a mystery (0 = off, the default, so a "
                         "run without it is unchanged)")
    ap.add_argument("--leave", type=float, default=0.0, metavar="SECONDS",
                    help="after the measured window, close every bot and keep the server "
                         "ticking alone for SECONDS while the RSS line keeps being "
                         "sampled: the difference between a view that is a working set "
                         "and one the process never gives back (0 = off, the default, so "
                         "a run without it is unchanged)")
    ap.add_argument("--bot-processes", type=int, default=1, metavar="K",
                    help="split the loop engine's population over K separate "
                         "bench/botnet.py processes. One process decodes and "
                         "decompresses every clientbound frame in a single Python "
                         "thread, so past a couple of hundred movers it — not the "
                         "server — is what caps the wire; K divides that cost. They "
                         "share one absolute window and one global spread grid, so "
                         "they measure one population and not K clusters. 1 (the "
                         "default) is the single-process path, unchanged.")
    ap.add_argument("--quiet", action="store_true", help="only the summary")
    args = ap.parse_args()

    if args.bot_processes < 1:
        raise SystemExit("--bot-processes must be at least 1")
    if args.bot_processes > 1 and args.engine != "loop":
        raise SystemExit("--bot-processes needs the loop engine (only it is "
                         "implemented by bench/botnet.py)")
    if args.bot_processes > args.bots:
        raise SystemExit(f"--bot-processes {args.bot_processes} exceeds --bots "
                         f"{args.bots}: every process needs at least one bot")
    if args.relay_loops < 1:
        raise SystemExit("--relay-loops must be at least 1")
    if args.relay_loops != 1 and not args.proxy:
        raise SystemExit("--relay-loops needs --proxy: it is the proxy's relay")

    if args.prewarm and not args.relayout:
        raise SystemExit("--prewarm needs a nonzero --relayout: with --relayout 0 the "
                         "second window is the clustered one, which the join already "
                         "streamed")
    if args.engine == "threads" and args.relayout is not None:
        raise SystemExit("--relayout is implemented by bench/botnet.py (the loop engine)")
    if args.engine == "threads" and args.spread:
        raise SystemExit("--spread is implemented by bench/botnet.py (the loop engine); "
                         "the threaded engine places its own bots")
    if args.engine == "threads" and args.probe_period:
        raise SystemExit("--probe-period is implemented by bench/botnet.py (the loop "
                         "engine); the threaded engine's bots never send a probe")

    binary = pathlib.Path(args.bin).resolve()
    if not binary.exists():
        raise SystemExit(f"no server binary at {binary} (build it, or pass --bin)")
    binary_sha = file_sha256(binary)

    # `--max-players` has to be raised on purpose: the server's default is 20
    # (vanilla's), and a 60-bot run against the default silently turns into a
    # 20-bot run with forty "unexpected EOF" failures — the 21st join is refused.
    # The harness measures the sim, not the cap, so it leaves headroom.
    argv = [str(binary), "--host", HOST, "--port", "0", "--offline-mode",
            "--data", str(pathlib.Path(args.data).resolve()), "--motd", "soak",
            "--max-players", str(max(100, args.bots * 2)),
            "--view-distance", str(args.view_distance), "--metrics-interval", "1"]
    if args.generated is not None:
        argv += ["--worldgen", str(args.generated), "--no-flat-world"]
    if args.world:
        argv += ["--world", str(pathlib.Path(args.world).resolve()), "--no-flat-world"]
    if args.proxy == "bungeecord":
        argv.append("--bungeecord")
    elif args.proxy == "velocity":
        argv += ["--velocity-forwarding", "--velocity-secret", args.proxy_secret]
    env = dict(os.environ)
    if not args.mobs:
        env["PLUSMC_MOB_SPAWNING"] = "0"

    log_path = pathlib.Path(args.log) if args.log else None
    if args.wait_quiet > 0.0:
        # A contended record is not wrong so much as unusable: docs/PHASE5.md §5 says
        # every loaded number here is a lower bound, and a sibling build turns the
        # bound into noise. Waiting is cheap next to re-measuring a long window.
        ambient, quiet_verdict, _held = wait_for_quiet(args, binary.name)
        if quiet_verdict:
            print("[!] starting contended anyway: " + "; ".join(quiet_verdict[:3]),
                  flush=True)
    else:
        # Even a cell that did not wait says what the box looked like when it
        # started: one sample, the same gate, so `busy` is not a second rule.
        ambient = quiet_state(binary.name)
        quiet_verdict = quiet_reasons(ambient, args.quiet_load, args.quiet_idle)
    ambient_load = ambient["load1"]
    # The server's own CPU time, taken across its life (see children_cpu_seconds):
    # the only denominator a generation rate can be divided by on a box whose
    # wall clock is somebody else's compile.
    cpu_before = children_cpu_seconds()
    started = time.time()
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1, env=env)
    lines = []
    metrics = []
    listening = {}

    def pump():
        assert proc.stdout is not None
        for line in proc.stdout:
            lines.append(line)
            if not listening and LISTENING.search(line):
                listening["port"] = int(LISTENING.search(line).group(1))
                listening["at"] = time.time()
            parsed = parse_metrics(line)
            if parsed:
                parsed["at"] = time.time()
                metrics.append(parsed)

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()

    # The server's CPU time, sampled beside the metrics lines (see
    # read_cpu_seconds): the rate a generation fill runs at is only comparable
    # across arms as columns per CPU second on a box with neighbours.
    cpu_series = []

    def sample_cpu():
        while proc.poll() is None:
            value = read_cpu_seconds(proc.pid)
            if value is not None:
                cpu_series.append((time.time(), value))
            time.sleep(0.5)

    threading.Thread(target=sample_cpu, daemon=True).start()

    # The RSS ceiling, enforced rather than watched. It reads the same metrics
    # lines the record is built from, so its number is the server's own reading;
    # the poll is four times a second because at a few thousand chunks a second
    # this process grows hundreds of megabytes between two of them. SIGKILL and
    # not SIGINT: a ceiling a graceful shutdown can outlive is not a ceiling.
    rss_guard = {"max_mb": args.max_rss} if args.max_rss > 0 else None
    if rss_guard is not None:
        def watch_rss():
            while proc.poll() is None:
                time.sleep(0.25)
                vals = [m["rss_bytes"] for m in metrics if "rss_bytes" in m]
                if vals and vals[-1] > rss_guard["max_mb"] * 1e6:
                    rss_guard["killed_mb"] = vals[-1] / 1e6
                    rss_guard["at"] = time.time()
                    proc.kill()
                    return
        threading.Thread(target=watch_rss, daemon=True).start()

    deadline = time.time() + 30
    while "port" not in listening and time.time() < deadline and proc.poll() is None:
        time.sleep(0.05)
    if "port" not in listening:
        proc.kill()
        raise SystemExit("the server never reported a listening port")
    startup_s = listening["at"] - started
    port = listening["port"]
    if not args.quiet:
        print(f"soak: server up in {startup_s:.2f} s on port {port} ({len(argv)} arg(s))")

    # The tier's second hop. `tests/proxy_live.py` is the same proxy the
    # proxy-forward test runs the server behind; here it is in this process so
    # the bots' port and the record's are one number, and `uuid_seed` makes each
    # of the bots its own forwarded player.
    proxy = None
    bot_port = port
    if args.proxy:
        import proxy_live
        proxy = proxy_live.Proxy((HOST, port), mode=args.proxy, uuid_seed="soak",
                                 secret=(args.proxy_secret
                                         if args.proxy == "velocity" else None),
                                 relay_loops=args.relay_loops)
        bot_port = proxy.start()
        if not args.quiet:
            print(f"soak: {args.proxy} proxy on port {bot_port} -> {port}" +
                  (f" ({args.relay_loops} relay loops)"
                   if args.relay_loops > 1 else ""))

    time.sleep(args.settle)
    idle_samples = list(metrics)

    join_started = time.time()
    joined, failures = [], []
    bot_stats = {}
    # (start, end, snapshot before, snapshot after) per measured window: the
    # clientbound traffic *inside* the window, which is the only place the
    # movement relay's fan-out shows up in `plusmc`'s metrics (it moves no world
    # state). A cumulative total would fold the join burst and the streaming
    # backlog into the number, so each window carries its own delta.
    traffic_marks = []
    # Filled by the loop engine only (the threaded engine's bots never probe).
    latency = None
    phases_seen = []
    # A Ctrl-C is not a failed run: everything measured up to the signal is
    # still a window, and a two-hour cell stopped at 1:59 is worth keeping. The
    # partial window is recorded (`interrupted`, `interrupted_after_s`) and the
    # process exits 130, but the record and the server log are written first.
    interrupted = False
    join_s = 0.0
    loaded_start = loaded_end = None
    before_window = None
    first_label = f"spread{args.spread:.0f}" if args.spread else "clustered"
    fleet = None
    until_box = None

    def drive_window(driven, seconds, label, before, drain_after=0.0):
        """One measured window; a Ctrl-C keeps what the window had measured.

        The drain is the probe arm's short turn after the deadline (in-flight
        probes are answered after the window's last flush); it is part of the
        mark, as it was before this function existed.  A signal during either
        part closes the mark at the signal instead of discarding it.
        """
        nonlocal interrupted
        start = time.time()
        try:
            driven.run(time.monotonic() + seconds)
            if drain_after > 0.0 and not interrupted:
                driven.run(time.monotonic() + drain_after)
        except KeyboardInterrupt:
            interrupted = True
        finally:
            # Even a signal in the last instant of the run keeps the window's
            # start and the partial counters; the mark is not conditional on a
            # clean return.
            end = time.time()
            traffic_marks.append((start, end, before, driven.snapshot()))
            phases_seen.append((label, start, end))

    if args.engine == "loop" and args.bot_processes > 1:
        # K processes, one population, one wall-clock window: the driver's
        # per-packet Python cost stops being what caps a 200+ mover run.
        try:
            (join_s, joined, bot_stats, traffic_marks, phases_seen, latency,
             failures) = drive_workers(args, bot_port, args.quiet)
            # `loaded` is the first window, like the single-process path: the
            # window the workers' plan named, not "the wall clock since they
            # finished".
            loaded_start, loaded_end = phases_seen[0][1], phases_seen[0][2]
            if not args.quiet:
                print(f"soak: {len(joined)}/{args.bots} bots in play after "
                      f"{join_s:.2f} s; holding for {args.seconds:.0f} s x "
                      f"{len(traffic_marks)} window(s)")
        except KeyboardInterrupt:
            # drive_workers kills its workers on the way out; there is no bot
            # snapshot to merge, so the record is the server's metrics up to
            # the signal and says so.
            interrupted = True
            join_s = time.time() - join_started
        if not interrupted:
            time.sleep(0.5)
    elif args.engine == "loop":
        # The cheap engine: one thread, N non-blocking sockets (bench/botnet.py).
        fleet = botnet.Fleet(HOST, bot_port, args.bots, stationary=args.stationary,
                             spread=args.spread, probe_period=args.probe_period)
        try:
            # Inside the try as well: a 200-bot join is long enough to be a
            # place a person stops a run, and the record should still say so.
            fleet.connect_all()
            # The driver's clock is `time.monotonic` (a tick budget must not
            # move when the system clock is), so its deadlines are built from
            # that one clock — mixing the two makes `until` an epoch stamp that
            # monotonic time can never reach.
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                in_play = sum(1 for b in fleet.bots if b.joined_at is not None)
                if in_play + len(fleet.failures) >= args.bots:
                    break
                fleet.run(min(time.monotonic() + 0.05, deadline))
            join_s = time.time() - join_started
            joined = list(range(sum(1 for b in fleet.bots if b.joined_at is not None)))
            if not args.quiet:
                print(f"soak: {len(joined)}/{args.bots} bots in play after "
                      f"{join_s:.2f} s; holding for {args.seconds:.0f} s")
            fleet.run(time.monotonic() + args.settle_after_join)
            if args.probe_period > 0:
                # The join storm's probes are not the measurement: drain them
                # here so the p99 below is the loaded window's and not the
                # transient's.
                fleet.drain_rtts()
            if args.prewarm and not args.spread:
                # Stream the second layout's regions *now*, then come back, so
                # that neither measured window contains a teleport's streaming
                # burst. The chunk set is cumulative per session, so the real
                # second window's move to the same cells sends nothing new —
                # which is what makes the two windows comparable at all
                # (docs/PHASE5.md).
                fleet.relayout(args.relayout)
                fleet.run(time.monotonic() + args.settle_after_join)
                fleet.relayout(0.0)
                fleet.run(time.monotonic() + args.settle_after_join)
            before_window = fleet.snapshot()
            drive_window(fleet, args.seconds, first_label, before_window,
                         drain_after=0.25 if args.probe_period > 0 else 0.0)
            if traffic_marks:
                loaded_start, loaded_end = traffic_marks[0][0], traffic_marks[0][1]
            if not interrupted and args.relayout is not None:
                # The paired arm. Re-placement costs a teleport and (unless it
                # was pre-warmed) one streamed region per bot, so the same
                # settle the join got runs again before the second window
                # opens; that is the whole point of doing it in one process
                # (docs/PHASE5.md).
                fleet.relayout(args.relayout)
                fleet.run(time.monotonic() + args.settle_after_join)
                if args.probe_period > 0:
                    fleet.drain_rtts()
                before_second = fleet.snapshot()
                label = (f"spread{args.relayout:.0f}" if args.relayout
                         else "clustered")
                drive_window(fleet, args.seconds, label, before_second)
        except KeyboardInterrupt:
            # The signal landed in the join, the settle or the prewarm, before
            # a measured window opened. Keep the population snapshot and let
            # the record say there are no loaded windows.
            interrupted = True
            if join_s <= 0.0:
                join_s = time.time() - join_started
        bot_stats = fleet.snapshot()
        latency = latency_summary(fleet.drain_rtts()) if args.probe_period > 0 else None
        failures = bot_stats["failures"]
        fleet.close_all()
        if not interrupted:
            time.sleep(0.5)
    else:
        until_box = [time.time() + args.seconds]  # one shared end, so the window is square
        threads = [threading.Thread(target=bot_thread,
                                   args=(bot_port, f"B{i}", i, until_box, failures, joined),
                                   daemon=True)
                   for i in range(args.bots)]
        try:
            for th in threads:
                th.start()
            # Wait for the population to be complete before the loaded window
            # starts.
            wait_deadline = time.time() + 30
            while len(joined) + len(failures) < args.bots and time.time() < wait_deadline:
                time.sleep(0.05)
            join_s = time.time() - join_started
            # The settle window is the same idea as the loop engine's: the bots
            # stay connected, the streaming backlog is drained, and only then
            # does the measured window open.
            until_box[0] = time.time() + args.settle_after_join + args.seconds
            time.sleep(args.settle_after_join)
            loaded_start = time.time()
            if not args.quiet:
                print(f"soak: {len(joined)}/{args.bots} bots in play after "
                      f"{join_s:.2f} s; holding for {args.seconds:.0f} s")

            for th in threads:
                th.join(timeout=args.seconds + 30)
            loaded_end = time.time()
        except KeyboardInterrupt:
            # The bots are daemon threads and their own deadline is fixed when
            # `stream` starts, so this closes the window at the signal and lets
            # the record be written; the server shutdown below ends their
            # sockets, and the process exits after the summary.
            interrupted = True
            loaded_end = time.time()
            if join_s <= 0.0:
                join_s = time.time() - join_started
        for th in threads:
            th.join(timeout=1.0)
        if not interrupted:
            time.sleep(0.5)

    # The *release* question, which a run that ends with the bots still connected
    # cannot see: is a streamed view memory only for as long as somebody is
    # looking at it, or for as long as the process lives? The bots are closed at
    # this point and the server keeps ticking alone for `--leave` seconds, so the
    # metrics lines after the close are the answer — an RSS that falls back to
    # the idle floor is a working set, one that stays is a world the process
    # keeps (docs/PHASE5.md, item 4).
    leave_rss = None
    if args.leave > 0 and not interrupted:
        closed_at = time.time()
        at_close = [m["rss_bytes"] for m in metrics if "rss_bytes" in m]
        try:
            time.sleep(args.leave)
        except KeyboardInterrupt:
            # The measured window is already complete; a stop during the
            # release wait keeps whatever post-close samples arrived.
            interrupted = True
        after = [m["rss_bytes"] for m in metrics
                 if "rss_bytes" in m and m.get("at", 0.0) > closed_at]
        leave_rss = {"seconds": args.leave,
                     "at_close": at_close[-1] if at_close else None,
                     "after": after[-1] if after else None,
                     "samples": len(after)}

    # What is left is teardown and the record, and the measurement is already in
    # memory. From here a second Ctrl-C must not throw it away: the shutdown is
    # bounded (`wait(timeout=20)` then kill) and the write is immediate.
    signal.signal(signal.SIGINT, signal.SIG_IGN)

    # The proxy has no state worth keeping after the population has left; close
    # it before the server is signalled so its relay threads are not left
    # draining sockets during the shutdown.
    if proxy is not None:
        proxy.close()

    proc.send_signal(signal.SIGINT)
    # One more reading while the process is still alive but already shutting down:
    # a generated world's last column can land in the final second, and a sample
    # series that stops a second short of it cannot bracket the fill's own CPU
    # share (see read_cpu_seconds). Taken here, before the wait reaps it.
    shutdown_cpu = read_cpu_seconds(proc.pid)
    if shutdown_cpu is not None:
        cpu_series.append((time.time(), shutdown_cpu))
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    reader.join(timeout=5)
    rc = proc.returncode
    cpu_after = children_cpu_seconds()
    server_cpu_s = (round(cpu_after - cpu_before, 3)
                    if cpu_before is not None and cpu_after is not None else None)
    end_load = load1()

    if log_path:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text("".join(lines))

    idle = [m for m in idle_samples]
    # With --relayout, `loaded` stays the *first* window (one layout, one steady
    # state) and `phases` carries both: a union of two layouts averaged into one
    # number would describe neither. An interrupt before the first window opened
    # leaves both bounds unset and `loaded` empty rather than inventing a window.
    loaded = ([m for m in metrics if loaded_start <= m["at"] <= loaded_end]
              if loaded_start is not None and loaded_end is not None else [])

    def col(rows, key):
        return [r[key] for r in rows if key in r]

    def stat(rows, key):
        values = col(rows, key)
        if not values:
            return None
        return {"avg": round(statistics.fmean(values), 3), "min": round(min(values), 3),
                "max": round(max(values), 3), "n": len(values)}

    def counter_stat(rows, key):
        """A store counter's window series, as integers.

        `world_chunks` / `world_bytes` / `world_written_chunks` are the store's
        own O(1) accounting on the metrics line (src/core/metrics.h). RSS alone
        cannot say whether a peak is the store, the join transient or allocator
        slack, so the gauge is summarised beside it: the last value, the max, and
        how many windows sat at that max (a policy that pins an occupancy has a
        `windows_at_max` that is most of `n`). `world_chunks_stored` is the same
        shape for the one cumulative counter -- its `last` is the run's total, and
        it is kept in this block because it is store accounting too. None when
        the server under test predates the keys -- an old record and a new binary
        both read here.
        """
        values = [int(r[key]) for r in rows if key in r]
        if not values:
            return None
        top = max(values)
        return {"first": values[0], "last": values[-1], "max": top,
                "windows_at_max": sum(1 for v in values if v == top),
                "n": len(values)}

    def generation_summary():
        """How fast the world store filled, per second, off the metrics series.

        The counter behind this is the store's cumulative `world_chunks_stored`
        (src/core/metrics.h): the difference between two consecutive one-second
        samples is exactly the columns stored in that second. The resident
        `world_chunks` gauge cannot be used when the store has a budget, because
        eviction holds it at its ceiling while the fill continues -- a cell that
        stored 1,500 columns against a 1,024-column ceiling would read 1,024, and
        one that evicts every second would read a fill of zero. Servers that
        predate the key (and every record filed before it) fall back to the
        gauge, which is exact when nothing is evicted; `counter` in the result
        says which was read.

        That rate is the number a join time cannot give: a cell that finishes
        sooner because it generated less reads as an improvement, so the rate is
        the throughput and the join time is an effect of it.

        Two means are reported because they answer different questions: over the
        fill (first sample to the last column, what a player waits through) and
        over the seconds that actually generated (the same work with the idle tail
        after every view is full excluded). `tps_min` is the tick rate while it
        was happening — generation competes with the tick for one core — and
        `columns_per_cpu_s` is the fill divided by the server's own CPU time
        (`children_cpu_seconds`), the form that survives a shared box.
        """
        counter = ("world_chunks_stored"
                   if any("world_chunks_stored" in m for m in metrics)
                   else "world_chunks")
        rows = [(m["at"], int(m[counter])) for m in metrics if counter in m]
        if len(rows) < 3:
            return None
        span_s = rows[-1][0] - rows[0][0]
        if span_s <= 0:
            return None
        pairs = list(zip(rows, rows[1:]))
        gains = [(later[1] - earlier[1], later[0] - earlier[0]) for earlier, later in pairs
                 if later[1] > earlier[1] and later[0] > earlier[0]]
        if not gains:
            return None
        gained = sum(columns for columns, _ in gains)
        generating_s = sum(seconds for _, seconds in gains)
        last_gain = max(later[0] for earlier, later in pairs if later[1] > earlier[1])
        first_gain = min(later[0] for earlier, later in pairs if later[1] > earlier[1])
        fill_s = last_gain - rows[0][0]
        busy = [m for m in metrics if first_gain <= m["at"] <= last_gain]
        tps = [m["tps"] for m in busy if "tps" in m]

        def cpu_at(when):
            """The server's CPU seconds at `when`, between two samples."""
            if len(cpu_series) < 2:
                return None
            before = [s for s in cpu_series if s[0] <= when]
            after = [s for s in cpu_series if s[0] >= when]
            if not before or not after:
                return None
            (t0, c0), (t1, c1) = before[-1], after[0]
            if t1 <= t0:
                return c1
            return c0 + (c1 - c0) * (when - t0) / (t1 - t0)

        cpu_fill = None
        start_cpu, end_cpu = cpu_at(first_gain), cpu_at(last_gain)
        if start_cpu is not None and end_cpu is not None and end_cpu > start_cpu:
            cpu_fill = end_cpu - start_cpu
        return {
            # Which metrics series this block read: the cumulative counter on a
            # server that has it, the resident gauge on one that predates it.
            "counter": counter,
            "columns": gained,
            "fill_seconds": round(fill_s, 2),
            "columns_per_s": round(gained / fill_s, 2) if fill_s > 0 else None,
            "generating_seconds": round(generating_s, 2),
            "while_generating_per_s": (round(gained / generating_s, 2)
                                       if generating_s > 0 else None),
            "peak_per_s": round(max(columns / seconds for columns, seconds in gains), 1),
            "tps_min": round(min(tps), 2) if tps else None,
            # How long the run went on after its last new column: a fill that
            # finished with time to spare has a rate that is a plateau, one that
            # was still going when the bots left is a lower bound.
            "since_last_column_s": round(rows[-1][0] - last_gain, 2),
            "cpu_s": server_cpu_s,
            # How many one-second samples the fill's CPU share was read from: a
            # zero here means the platform's `ps` never answered (see
            # read_cpu_seconds), not that generation cost nothing.
            "cpu_samples": len(cpu_series),
            # The fill's own CPU seconds, taken between the two samples that
            # bracket its first and last columns, rather than the run's total: a
            # 45-second cell spends most of its life ticking a full view with
            # nothing left to generate, and dividing by that would report the
            # generation as cheaper the longer the bots were left standing.
            "cpu_fill_s": round(cpu_fill, 3) if cpu_fill is not None else None,
            "columns_per_cpu_s": (round(gained / cpu_fill, 2)
                                  if cpu_fill else None),
            # The series itself, so a record carries the per-second shape and not
            # just its mean: [seconds since the first sample, columns generated
            # in that second]. A generation that stalls for two seconds and then
            # bursts has the same mean as a steady one and is a different run.
            "per_second": [[round(later[0] - rows[0][0], 2), later[1] - earlier[1]]
                           for earlier, later in pairs],
        }

    def phase_stat(label, start, end):
        rows = [m for m in metrics if start <= m["at"] <= end]
        return {"label": label, "seconds": round(end - start, 2),
                **{k: stat(rows, k) for k in ("tps", "work_avg_ms", "work_peak_ms",
                                              "busy_pct", "tick_pct", "resync", "players")}}

    rss = col(metrics, "rss_bytes")

    def traffic_window(start, end, before, after):
        """The clientbound traffic *inside* one measured window, per bot per
        second.

        The counters are cumulative and read off the wire by the driver
        (bench/botnet.py), so a window's delta is what actually reached the
        viewers during it: the join burst and the streaming backlog before the
        window are excluded, and so is everything after it. This is the number a
        distance gate moves — a stance relayed to N viewers is N writes to N
        sockets, and no world metric changes when it is withheld.
        """
        bots = len(joined) or args.bots
        seconds = end - start
        if not bots or seconds <= 0 or not before or not after:
            return None
        packets = after["packets_received"] - before["packets_received"]
        nbytes = after["received_bytes"] - before["received_bytes"]
        return {
            "seconds": round(seconds, 2),
            "packets_in": packets,
            # Uncompressed frame sizes, like the chunk counter: the wire bytes are
            # a compression ratio away, not the work the server did.
            "bytes_in": nbytes,
            "packets_per_bot_s": round(packets / bots / seconds, 1),
            "kib_per_bot_s": round(nbytes / bots / seconds / 1024.0, 1),
        }

    traffic = [traffic_window(*mark) for mark in traffic_marks]

    # The write path's counter, off the shutdown line (docs/WRITE_PATH.md). The
    # line is printed once, on the way out, and only when something was sent;
    # a run whose server was killed before it printed it records None rather
    # than a zero that would read as "no sharing happened".
    chunk_sharing = None
    for line in reversed(lines):
        hit = CHUNK_SHARING.search(line)
        if hit:
            sends, bodies, body_pct, frames, frame_pct = hit.groups()
            chunk_sharing = {"sends": int(sends), "bodies_reused": int(bodies),
                             "body_pct": float(body_pct), "frames_reused": int(frames),
                             "frame_pct": float(frame_pct)}
            break

    # ...and the generation pool's, plus the biome memo's own answer to "was the
    # win the hits?". Both are silent when they did nothing (no pool installed,
    # or no biome question asked), so a run without them records None.
    generation_pool = None
    for line in reversed(lines):
        hit = GENERATION_POOL.search(line)
        if hit:
            generated, threads, requested, already, dropped, failed, queued = hit.groups()
            generation_pool = {"generated": int(generated), "threads": int(threads),
                              "requested": int(requested), "already": int(already),
                              "dropped": int(dropped), "failed": int(failed),
                              "queued": int(queued)}
            break
    biome_memo = None
    for line in reversed(lines):
        hit = BIOME_MEMO.search(line)
        if hit:
            hits, misses, rate, entries, threads, most, refills = hit.groups()
            biome_memo = {"hits": int(hits), "misses": int(misses), "hit_pct": float(rate),
                          "entries": int(entries), "threads": int(threads),
                          "most_entries": int(most), "refills": int(refills)}
            break

    # The natural spawn pass's own account, silent for a run that never walked a
    # candidate. `refused` is the stored-column predicate's count: the candidate
    # chunks the world answered `false` for (docs/PHASE5.md).
    natural_spawn = None
    for line in reversed(lines):
        hit = NATURAL_SPAWN.search(line)
        if hit:
            mobs, passes, candidates, tickable, refused = hit.groups()
            natural_spawn = {"mobs": int(mobs), "passes": int(passes),
                             "candidates": int(candidates), "tickable": int(tickable),
                             "refused": int(refused)}
            break

    # ...and the entity relays', silent for a run that built no such payload.
    mob_tracking = None
    for line in reversed(lines):
        hit = MOB_TRACKING.search(line)
        if hit:
            payloads, writes, per, pairings, peak = hit.groups()
            mob_tracking = {"payloads": int(payloads), "writes": int(writes),
                            "writes_per_payload": float(per), "pairings": int(pairings),
                            "peak_viewers": int(peak)}
            break
    item_tracking = None
    for line in reversed(lines):
        hit = ITEM_TRACKING.search(line)
        if hit:
            payloads, writes, per, pairings, peak = hit.groups()
            item_tracking = {"payloads": int(payloads), "writes": int(writes),
                             "writes_per_payload": float(per), "pairings": int(pairings),
                             "peak_viewers": int(peak)}
            break
    entity_relays = {}
    for line in lines:
        hit = TRANSIENT_TRACKING.search(line)
        if not hit:
            continue
        kind, payloads, writes, per, pairings, peak = hit.groups()
        entity_relays[kind] = {"payloads": int(payloads), "writes": int(writes),
                               "writes_per_payload": float(per),
                               "pairings": int(pairings), "peak_viewers": int(peak)}

    summary = {
        "binary": str(binary),
        "binary_sha256": binary_sha,
        "argv": argv,
        # The environment the server was started with: two arms that differ only
        # by a knob (PLUSMC_CHUNK_BUDGET_MB, PLUSMC_CHUNK_FLOOR_COLUMNS) are
        # otherwise indistinguishable records.
        "server_env": {k: v for k, v in sorted(env.items()) if k.startswith("PLUSMC_")},
        "bots": args.bots,
        "seconds": args.seconds,
        # A run stopped with Ctrl-C still writes this record.
        # `interrupted_after_s` is the measured time it actually got, summed
        # over the windows (None if the signal landed before one opened); the
        # requested `seconds` is not rewritten. The threaded engine has no
        # per-window traffic marks, so its single loaded window is the fallback.
        "interrupted": interrupted,
        "interrupted_after_s": (
            round(sum(end - start
                      for start, end, _before, _after in traffic_marks), 2)
            if interrupted and traffic_marks else
            round(loaded_end - loaded_start, 2)
            if interrupted and loaded_start is not None and loaded_end is not None else None),
        "generated_seed": args.generated,
        "mobs": args.mobs,
        "view_distance": args.view_distance,
        "startup_s": round(startup_s, 2),
        "join_s": round(join_s, 2),
        "load_avg_1m": {"before": ambient_load, "after": end_load},
        # The box-busy caveat as a field rather than only a stdout line (the
        # harness prints `[!] the box was busy ...` at or above this threshold):
        # a record taken on a busy box is not comparable with a quiet-box one,
        # and nothing else in the file says which it is (docs/PHASE5.md §5).
        "quiet_load": args.quiet_load,
        # The gate's whole verdict, not only the load half: the CPU's idle share
        # at start, the processes that made it unusable, and why. `busy` is
        # exactly `busy_reasons != []`, so a record cannot claim to be quiet on
        # the load average while a sibling soak's duty cycle was filling the box.
        "quiet_idle": args.quiet_idle,
        "quiet_hold": args.quiet_hold,
        "ambient": ambient,
        "busy": bool(quiet_verdict),
        "busy_reasons": quiet_verdict,
        "engine": args.engine,
        # The second TCP hop, when one was asked for: which proxy mode listened
        # on which port in front of which backend. None on the direct arm.
        "proxy": ({"mode": args.proxy, "port": bot_port, "backend_port": port,
                   "relay_loops": args.relay_loops}
                  if args.proxy else None),
        # How many driver processes ran the population. Without it a K-worker
        # record is indistinguishable from a single-process one — and the point
        # of item 1 is exactly that K is what lifts the driver's read cap
        # (docs/PHASE5.md §6 item 1). 1 is the single-process path.
        "bot_processes": args.bot_processes,
        "stationary": args.stationary,
        "spread": args.spread,
        "relayout_blocks": args.relayout,
        "prewarm": args.prewarm,
        "phases": [phase_stat(*p) for p in phases_seen] if len(phases_seen) > 1 else [],
        "probe_period": args.probe_period,
        "latency": latency,
        "joined": len(joined),
        "bot_stats": bot_stats,
        "bot_failures": failures,
        "server_rc": rc,
        # The server's own user+sys seconds. A tick time on this box is a lower
        # bound moved by whatever else is running; the CPU the work itself needs
        # is the number that compares two arms, and it is free here already
        # (`children_cpu_seconds`, also reported in the generation block).
        "server_cpu_s": server_cpu_s,
        "idle": {k: stat(idle, k) for k in ("tps", "work_avg_ms", "work_peak_ms", "busy_pct",
                                            "tick_pct", "resync")},
        "loaded": {k: stat(loaded, k) for k in ("tps", "work_avg_ms", "work_peak_ms", "busy_pct",
                                                "tick_pct", "resync", "players")},
        "rss_bytes": {"first": rss[0] if rss else None, "last": rss[-1] if rss else None,
                       "peak": max(rss) if rss else None},
        # The store's own gauge over every metrics window, idle and loaded, like
        # `rss_bytes` above (see counter_stat): the part of a memory figure that
        # is the store rather than the allocator.
        "world": {name: counter_stat(metrics, name) for name in
                  ("world_chunks", "world_bytes", "world_written_chunks",
                   "world_chunks_stored")},
        # Generation throughput, per second (see generation_summary): the store
        # fills as views are served, and the rate it fills at is the workload's
        # own cost in a way `join_s` is not.
        "generation": generation_summary(),
        # Sampled *after* every bot has left (--leave): the release half of the
        # memory question, next to the peak that includes the join.
        "rss_after_leave": leave_rss,
        # Set when --max-rss tripped: the run is truncated by construction, and
        # the numbers after this point describe the ceiling, not the workload.
        "rss_guard": rss_guard,
            # The write path's deterministic read (docs/WRITE_PATH.md), from the
        # server's shutdown line: sends, and how many reused a body or a whole
        # frame another connection had already built. None for a server that
        # never sent a chunk (the line is silent then) or predates it.
        "chunk_sharing": chunk_sharing,
        # The pool's own count (world/chunk_generation.h) and the biome memo's
        # hit rate (world/worldgen_biome.cpp): the first says what built the
        # columns `generation` above counted, the second says whether it was the
        # memo's hits that made it cheap. Both None when the line never printed.
        "generation_pool": generation_pool,
        "biome_memo": biome_memo,
        # The natural spawn pass's deterministic read (src/server/session.cpp):
        # mobs created, passes, and the candidate square against what the world
        # would tick. None for a run whose spawning never walked a candidate.
        "natural_spawn": natural_spawn,
        # The mob relay's deterministic read (docs/MOB_VISIBILITY.md): the
        # payloads the tick built and the writes they were fanned out to. None
        # for a run with no mobs, or a server killed before it printed the line.
        "mob_tracking": mob_tracking,
        # ...and the item relay's (docs/MOB_VISIBILITY.md), None without drops.
        "item_tracking": item_tracking,
        # ...and the four later relays, keyed by kind (orb/tnt/projectile/potion/
        # cloud). Empty (and printed only when present) for a scene that never
        # produced one.
        "entity_relays": entity_relays or None,
        # The per-view memory experiment's denominator, counted off the wire
        # (bench/botnet.py's chunk counter) rather than assumed from the view
        # distance: the server's chunk sends are paced, and the world store keeps
        # every chunk it has generated for the process's life.
        "chunks": {"received": (bot_stats or {}).get("chunks_received"),
                   "body_bytes": (bot_stats or {}).get("chunk_body_bytes"),
                   # The set behind the count: a re-stream and a cluster both
                   # inflate `received` (8x for a spawn cluster of 8), so the
                   # union over bots is the denominator an RSS number can be
                   # divided by (bench/botnet.py).
                   "distinct": (bot_stats or {}).get("chunks_distinct"),
                   # How the distinct count was arrived at when K processes drove
                   # the population: `bench/soak.py --bot-processes` unions their
                   # key pairs (None on the single-process path, which needs no
                   # merge). A record that paraphrased a union as a sum would be
                   # a bigger denominator than the run had.
                   "distinct_rule": (bot_stats or {}).get("chunks_distinct_rule"),
                   # What summing the K workers' counts would have said: the excess
                   # over `distinct` is the set overlap (spawn's stream, and cells
                   # that overlap at a deep view distance).
                   "distinct_sum_bound": (bot_stats or {}).get("chunks_distinct_sum_bound"),
                   "per_bot_max": (bot_stats or {}).get("chunk_keys_max")},
        # Clientbound traffic inside the loaded window (and, with --relayout, the
        # paired one): the movement relay's audience, counted off the wire.
        "traffic": traffic[0] if traffic else None,
        "traffic_phases": traffic if len(traffic) > 1 else [],
        "loaded_windows": len(loaded),
        "idle_windows": len(idle),
    }
    if args.json:
        path = pathlib.Path(args.json)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    print()
    if interrupted:
        print("  interrupted        yes: the record is the window up to the signal")
    print(f"  startup            {summary['startup_s']:.2f} s")
    print(f"  join ({args.bots} bots)  {summary['join_s']:.2f} s"
          + (f"  ({len(failures)} failed)" if failures else "")
          + f"  [{args.engine} engine]"
          + (f"  {bot_stats['packets_sent']} packets out" if bot_stats else ""))
    print(f"  layout             {'stationary' if args.stationary else 'moving'}"
          + (f", {args.spread:.0f} blocks apart" if args.spread else ", clustered at spawn"))
    def fmt(v):
        return "n/a" if v is None else f"{v:.2f}"

    ambient_idle = summary.get("ambient", {}).get("idle_pct")
    print(f"  ambient load       {fmt(ambient_load)} -> {fmt(end_load)} (1 min, before -> after)"
          f"  CPU idle {('n/a' if ambient_idle is None else f'{ambient_idle:.1f}%')}"
          + ("   [!] the box was busy: this record is not comparable with a quiet one"
             if summary["busy"] else ""))
    for why in summary.get("busy_reasons", [])[:3]:
        print(f"                     busy: {why}")
    for name, rows in (("idle", idle), ("loaded", loaded)):
        s = summary[name]
        if not s["tps"]:
            print(f"  {name:<7}          (no windows)")
            continue
        players = s.get("players")
        print(f"  {name:<7} tps      {s['tps']['min']:.2f} .. {s['tps']['max']:.2f}"
              f"  (avg {s['tps']['avg']:.2f})")
        print(f"          work     avg {s['work_avg_ms']['avg']:.3f} ms"
              f"  peak {s['work_peak_ms']['max']:.3f} ms")
        # Busy is the average of the window and the peak is named separately: a
        # table that quoted the max busy next to the avg work would report a
        # number that happened in one 50 ms tick as if it were the norm.
        print(f"          load     busy {s['busy_pct']['avg']:.1f}% of a core"
              f" (max {s['busy_pct']['max']:.1f}%)"
              f"  tick {s['tick_pct']['avg']:.1f}% full"
              f"  resync {int(s['resync']['max']) if s['resync'] else 0}"
              + (f"  players {players['max']:.0f}" if players else ""))
        print(f"          windows  {s['tps']['n']}")
    for ph in summary["phases"]:
        if ph["work_avg_ms"]:
            print(f"  phase {ph['label']:<10} work     avg {ph['work_avg_ms']['avg']:.3f} ms"
                  f"  peak {ph['work_peak_ms']['max']:.3f} ms"
                  f"  busy {ph['busy_pct']['avg']:.1f}%"
                  + (f"  players {ph['players']['max']:.0f}" if ph.get("players") else ""))
    if latency:
        print(f"  latency (probe {args.probe_period:g}s)  "
              f"p50 {latency['p50_ms']:.1f} ms  p90 {latency['p90_ms']:.1f} ms  "
              f"p99 {latency['p99_ms']:.1f} ms  max {latency['max_ms']:.1f} ms")
        print(f"           {latency['within_target_pct']:.1f}% of {latency['n']} probes "
              f"<= {latency['target_ms']:.0f} ms (one tick)"
              + (f", {bot_stats['probes_pending']} unanswered at close"
                 if bot_stats and bot_stats.get("probes_pending") else ""))
    gen = summary["generation"]
    if gen:
        print(f"  generation         {gen['columns']} columns in {gen['fill_seconds']:g} s"
              f"  {gen['columns_per_s']:.2f} columns/s (peak {gen['peak_per_s']:.1f})"
              f"  {gen['while_generating_per_s']:.2f}/s while generating"
              + (f"  {gen['columns_per_cpu_s']:.2f}/cpu-s ({gen['cpu_fill_s']:g} s CPU generating)"
                 if gen["columns_per_cpu_s"] is not None else "")
              + (f"  tps min {gen['tps_min']:.2f}" if gen["tps_min"] is not None else ""))
    if rss:
        print(f"  rss                {rss[0] / 1e6:.1f} MB -> {rss[-1] / 1e6:.1f} MB"
              f"  (peak {max(rss) / 1e6:.1f} MB)")
    if bot_stats and bot_stats.get("chunks_received"):
        chunks = bot_stats["chunks_received"]
        bodies = bot_stats["chunk_body_bytes"]
        distinct = bot_stats.get("chunks_distinct") or 0
        peak = max(rss) if rss else None
        # Both denominators are printed because they are different questions:
        # `received` counts packets (what the wire carried), `distinct` counts
        # chunks (what the process is holding). The second number includes the
        # process's base RSS and every player's, so it is an upper bound on the
        # per-chunk cost; the slope comes from subtracting two cells.
        # KB here is 1000 bytes, like the MB in the same line and everywhere else
        # in this file's output: mixing the two is how a 460 KB slope turns into a
        # number nobody can compare with the one a doc quotes.
        print(f"  chunks             {chunks} streamed"
              + (f", {distinct} distinct ({chunks / distinct:.1f}x reused)" if distinct else "")
              + f", {bodies / 1e6:.1f} MB of bodies"
              + (f"  peak {peak / 1e6:.1f} MB = {peak / distinct / 1e3:.0f} KB"
                 f" of RSS per distinct chunk" if peak is not None and distinct else ""))
    if summary["rss_guard"] and summary["rss_guard"].get("killed_mb"):
        g = summary["rss_guard"]
        print(f"  rss guard          TRIPPED at {g['killed_mb']:.0f} MB (ceiling "
              f"{g['max_mb']:.0f} MB): the server was killed, so this run is truncated")
    if summary["rss_after_leave"] and summary["rss_after_leave"]["after"]:
        lv = summary["rss_after_leave"]
        print(f"  rss after leave    {lv['at_close'] / 1e6:.1f} MB when the last bot left"
              f" -> {lv['after'] / 1e6:.1f} MB {lv['seconds']:g} s later"
              f"  ({lv['samples']} sample(s) since)")
    if summary["traffic"]:
        t = summary["traffic"]
        print(f"  traffic (loaded)   in {t['packets_in']} pkts / {t['bytes_in'] / 1e6:.1f} MB"
              f" over {t['seconds']:g} s  ({t['packets_per_bot_s']:.1f} pkts/s/bot,"
              f" {t['kib_per_bot_s']:.1f} KiB/s/bot)")
    for ph, t in zip(summary["phases"], summary["traffic_phases"]):
        if t:
            print(f"  phase {ph['label']:<10} traffic  in {t['packets_in']} pkts"
                  f"  ({t['packets_per_bot_s']:.1f} pkts/s/bot, {t['kib_per_bot_s']:.1f} KiB/s/bot)")
    if summary["chunk_sharing"]:
        cs = summary["chunk_sharing"]
        print(f"  chunk sharing      {cs['sends']} sends, {cs['bodies_reused']} bodies reused"
              f" ({cs['body_pct']:.1f}%), {cs['frames_reused']} frames reused"
              f" ({cs['frame_pct']:.1f}%)")
    if summary["generation_pool"]:
        gp = summary["generation_pool"]
        print(f"  generation pool    {gp['generated']} columns by {gp['threads']} thread(s),"
              f" {gp['requested']} requested, {gp['already']} already stored,"
              f" {gp['dropped']} dropped, {gp['failed']} failed, {gp['queued']} queued")
    if summary["biome_memo"]:
        bm = summary["biome_memo"]
        print(f"  biome memo         {bm['hits']} hits, {bm['misses']} misses"
              f" ({bm['hit_pct']:.1f}% hit rate), {bm['entries']} entries across"
              f" {bm['threads']} thread(s) (most {bm['most_entries']}), {bm['refills']} refills")
    if summary["natural_spawn"]:
        ns = summary["natural_spawn"]
        print(f"  natural spawn      {ns['mobs']} mob(s) from {ns['passes']} pass(es), "
              f"{ns['candidates']} candidate chunk(s): {ns['tickable']} tickable, "
              f"{ns['refused']} refused")
    if summary["mob_tracking"]:
        mt = summary["mob_tracking"]
        print(f"  mob tracking       {mt['payloads']} payloads, {mt['writes']} writes"
              f" ({mt['writes_per_payload']:.2f} per payload), {mt['pairings']} pairings,"
              f" peak {mt['peak_viewers']} viewers")
    if summary["item_tracking"]:
        it = summary["item_tracking"]
        print(f"  item tracking      {it['payloads']} payloads, {it['writes']} writes"
              f" ({it['writes_per_payload']:.2f} per payload), {it['pairings']} pairings,"
              f" peak {it['peak_viewers']} viewers")
    for kind, rt in sorted((summary.get("entity_relays") or {}).items()):
        print(f"  {kind + ' tracking':<18} {rt['payloads']} payloads, {rt['writes']} writes"
              f" ({rt['writes_per_payload']:.2f} per payload), {rt['pairings']} pairings,"
              f" peak {rt['peak_viewers']} viewers")
    if failures:
        print("  failures:")
        for f in failures[:5]:
            print(f"    {f}")
    if args.json:
        print(f"  record             {args.json}")

    if interrupted:
        # 130 is the shell's "terminated by SIGINT"; the record was written
        # first, so a harness or a person can still read the partial window.
        return 130
    if failures or rc not in (0, -signal.SIGINT):
        return 1
    if summary["loaded"]["tps"] and summary["loaded"]["tps"]["min"] < 15.0:
        print("\n  NOTE: the loaded window fell below 15 TPS — the sim is the limit on "
              "this machine at this bot count (see docs/PHASE5.md).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
