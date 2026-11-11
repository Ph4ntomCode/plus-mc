#!/usr/bin/env python3
"""A soak stopped with Ctrl-C still writes its record.

`bench/soak.py` is the harness every long-run number in `docs/PHASE5.md` comes
from. A multi-hour cell is expensive to repeat, so a Ctrl-C must not throw the
window away: the harness catches the signal, summarises the metrics and driver
counters it already has, writes the JSON record and the server log, and exits
130 (the shell's "terminated by SIGINT") after doing so.

The test pins the three parts that can regress independently:

  * **the record is written**, and names itself `"interrupted": true` with the
    measured window it actually got (`"interrupted_after_s"`), not the requested
    `seconds`;
  * **the window is real** — at least one metrics sample falls inside it, so a
    record that says "interrupted" but carries no loaded window is not accepted
    as the partial-window path;
  * **the exit code is 130**, which is how a person or a CI wrapper tells a
    stopped soak from a completed one.

The child is started with `-u` so its "holding for ..." line is readable before
the window opens; that is the synchronisation point, not a sleep guessed from
startup time. The settle is shortened to half a second and the test signals two
and a half seconds later, so the signal lands in the measured window on a busy
box as well.

Before interrupting, a short uninterrupted join preflight runs through the same
data root. `bench/soak.py` prints its holding line even when the join deadline
expires, so a zero-bot interrupt record is not evidence that the interrupt path
works. The preflight proves that the server and bot protocol can join; if the
box is busy, one retry is allowed, while a quiet-box failure is reported as a
real join regression. Only a passing preflight is followed by the interrupt.

The server needs client-config assets before it will let a client join, and
these are gitignored captured data no CI runner has. This test does not measure
the world, only the interrupt path, so it gives the server a minimal data root
of its own: a version.json and an empty `clientconfig/`. The protocol-level bots
join through that (the server warns that the registry listing is incomplete and
sends the void fallback for chunks, neither of which this test reads).

Usage: soak_interrupt_test.py /path/to/plusmc
"""
import json
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOAK = ROOT / "bench" / "soak.py"
BOTS = 2
SECONDS = 30.0
# The bots in `bench/botnet.py` speak the canonical version's protocol; the
# advertising version only has to agree with it for the join below.
DATA_VERSION = "26.3"
PROTOCOL = 777
# Time from "holding for" to the signal: the 0.5 s settle plus ~2 s of window,
# which is two one-second metrics samples. The window is 30 s, so this cannot
# reach the end of the run even on a slow box.
SIGNAL_AFTER = 2.5
LINE_TIMEOUT = 60.0
PREFLIGHT_SECONDS = 1.0
PREFLIGHT_SETTLE = 0.25
PREFLIGHT_ATTEMPTS = 2
PREFLIGHT_TIMEOUT = 60.0


def make_data_root(work: pathlib.Path) -> pathlib.Path:
    """The minimal assets the server needs to accept a protocol client.

    `load_client_config` only requires a version directory with version.json;
    an empty `clientconfig/` directory is enough for `ok()`. The bots are not a
    vanilla client, so the registry listing and tags payload are not read.
    """
    root = work / "vanilla"
    version_dir = root / DATA_VERSION
    (version_dir / "clientconfig").mkdir(parents=True)
    (version_dir / "version.json").write_text(json.dumps({
        "id": DATA_VERSION,
        "name": DATA_VERSION,
        "world_version": 1,
        "protocol_version": PROTOCOL,
        "stable": True,
    }) + "\n")
    return root


def soak_argv(binary: pathlib.Path, data_root: pathlib.Path, seconds: float,
              record: pathlib.Path, log: pathlib.Path, settle: float) -> list[str]:
    return [sys.executable, "-u", str(SOAK), "--bin", str(binary),
            "--bots", str(BOTS), "--spread", "64", "--view-distance", "2",
            "--settle-after-join", f"{settle:g}", "--seconds", f"{seconds:g}",
            "--data", str(data_root), "--json", str(record), "--log", str(log)]


def run_join_preflight(binary: pathlib.Path, data_root: pathlib.Path,
                       work: pathlib.Path) -> None:
    """Prove the join path works before asking the interrupt path for a window.

    A zero-bot record can mean either a broken protocol/join path or a box so
    loaded that the 30 s join deadline expired. Run a short normal soak first:
    if it joins, a later zero-bot interruption is a real join failure in that
    run. If the preflight itself fails, retry once only when the record says the
    box was busy; two failures are reported as a join failure, not disguised as
    an interruption assertion.
    """
    evidence: list[str] = []
    for attempt in range(1, PREFLIGHT_ATTEMPTS + 1):
        record = work / f"preflight-{attempt}.json"
        log = work / f"preflight-{attempt}.log"
        argv = soak_argv(binary, data_root, PREFLIGHT_SECONDS, record, log,
                         PREFLIGHT_SETTLE)
        try:
            result = subprocess.run(argv, cwd=ROOT, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True,
                                    timeout=PREFLIGHT_TIMEOUT)
        except subprocess.TimeoutExpired:
            evidence.append(f"attempt {attempt}: timed out after {PREFLIGHT_TIMEOUT:g} s")
            continue
        output = result.stdout or ""
        data = {}
        if record.is_file():
            try:
                data = json.loads(record.read_text())
            except (OSError, json.JSONDecodeError) as exc:
                evidence.append(f"attempt {attempt}: unreadable record: {exc}")
        if result.returncode == 0 and data.get("joined") == BOTS:
            print(f"join preflight: {BOTS}/{BOTS} bots joined on attempt {attempt}",
                  flush=True)
            return
        evidence.append(
            f"attempt {attempt}: rc {result.returncode}, joined {data.get('joined')}, "
            f"busy {data.get('ambient', {}).get('busy')}, "
            f"bot_failures {data.get('bot_failures')!r}\n{output[-1200:]}")
        if not data.get("ambient", {}).get("busy"):
            break
    raise AssertionError(
        "join preflight failed before the interrupt test; this is a join failure, "
        "not an interruption result:\n" + "\n".join(evidence))


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = pathlib.Path(sys.argv[1]).resolve()
    if not binary.exists():
        print(f"no server binary at {binary}", file=sys.stderr)
        return 1

    work = pathlib.Path(tempfile.mkdtemp(prefix="plusmc-soak-interrupt-"))
    record = work / "record.json"
    log = work / "server.log"
    data_root = make_data_root(work)
    run_join_preflight(binary, data_root, work)
    argv = soak_argv(binary, data_root, SECONDS, record, log, 0.5)
    print("run: " + " ".join(argv))
    proc = subprocess.Popen(argv, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    try:
        # The harness prints this after the join and just before the settle; a
        # fixed sleep from process start would guess at startup + join on a box
        # that is also compiling.
        deadline = time.time() + LINE_TIMEOUT
        seen_holding = False
        while time.time() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            if "holding for" in line:
                seen_holding = True
                break
        if not seen_holding:
            out, _ = proc.communicate(timeout=5) if proc.poll() is None else (proc.stdout.read(), "")
            raise AssertionError("the soak never reported its loaded window\n" + out[-2000:])

        time.sleep(SIGNAL_AFTER)
        proc.send_signal(signal.SIGINT)
        try:
            out, _ = proc.communicate(timeout=40)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
            raise AssertionError("the soak did not exit after SIGINT\n" + out[-2000:])

        assert proc.returncode == 130, \
            f"expected 130 after SIGINT, got {proc.returncode}\n{out[-2000:]}"
        assert record.exists(), f"no record written\n{out[-2000:]}"
        assert log.exists(), f"no server log written\n{out[-2000:]}"
        data = json.loads(record.read_text())
        assert data["interrupted"] is True, data.get("interrupted")
        assert data["seconds"] == SECONDS, data.get("seconds")
        assert data["loaded_windows"] >= 1, data.get("loaded_windows")
        assert data["interrupted_after_s"] and data["interrupted_after_s"] > 0, \
            data.get("interrupted_after_s")
        if data["joined"] != BOTS:
            raise AssertionError(
                f"interrupted soak joined {data['joined']}/{BOTS} after the join "
                f"preflight passed; bot_failures={data.get('bot_failures')!r}, "
                f"ambient={data.get('ambient')!r}")
        assert "interrupted        yes" in out, "the summary did not name the interruption"
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        shutil.rmtree(work, ignore_errors=True)
    print(f"soak interrupt: {data['loaded_windows']} window(s), "
          f"{data['interrupted_after_s']:g} s measured, record and log written, exit 130")
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
