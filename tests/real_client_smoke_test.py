#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in smoke test with a supplied 26.1/26.2 Java client.

The socket tests prove the wire bytes decode; this test proves a real vanilla
client accepts the translated login, configuration and initial play packets.
It is opt-in because the client jar and its Mojang libraries are not part of
the repository. Supply the jar with ``--client-jar`` or
``PLUSMC_CLIENT_JAR``; with no jar the test skips with exit code 77.

A supplied jar is not treated as optional input: an unreadable jar, a release
outside 26.1-26.2, missing libraries, or a launch that dies before reaching play
is a failure. The client is pointed at ``--quickPlayMultiplayer`` and the test
waits for the server's join line, then keeps both processes alive for a settle
period so a packet the client rejects still fails the run.

It also passes ``--test-t0-menus``: the server force-opens the translated
enchanting-table and anvil screens after the client enters play, and the test
requires the client to stay connected through both screens and the close. That
is what proves a real client accepts the new menu packets, not just the
synthetic wire client.

Supplying a 26.3 jar through ``--canonical-jar`` / ``PLUSMC_CANONICAL_CLIENT_JAR``
switches to the two-client arm: a real 26.3 client joins alongside the T0
client, the server runs ``--test-t0-visibility``, and the test asserts the
logged player pair in both directions across a tracking-range exit and re-entry
while both clients stay connected.

Library lookup defaults to the repository's vanilla cache::

    data/vanilla/_cache/real-client/<version>/libraries
    data/vanilla/_cache/real-client/<version>/natives

Use ``--libraries``/``--natives`` for an installation elsewhere. When no
extracted natives directory exists, the native classifier jars under the
library tree are unpacked into a temporary directory.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import selectors
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SUPPORTED_PROTOCOLS = {775: "26.1", 776: "26.2"}
JOIN_MARKERS = ("joined the game", "entered play")
FAILURE_MARKERS = (
    "Internal Exception",
    "Failed to decode packet",
    "DecoderException",
    "Connection lost",
    "Disconnected from",
    "Client disconnected with reason",
    "Timed out while waiting for the client to load chunks",
    "Exception Connecting",
)
MENU_MARKERS = (
    "t0 menu smoke: enchanting table opened",
    "t0 menu smoke: anvil opened",
    "t0 menu smoke: closed",
)
CANONICAL_PROTOCOL = 777
CANONICAL_USERNAME = "RealCanon"
VISIBILITY_MARKERS = ("near", "far", "back")
VISIBILITY_RE = re.compile(
    r"t0 visibility: (near|far|back) canonical=(\d+) "
    r"canonical_sees_t0=(\d+) t0_sees_canonical=(\d+)")


def skip(message: str) -> int:
    print(f"SKIP: {message}", file=sys.stderr)
    return 77


def fail(message: str, *, server_log: str = "", client_log: str = "") -> int:
    print(f"FAIL: {message}", file=sys.stderr)
    for title, text in (("server", server_log), ("client", client_log)):
        if text:
            print(f"--- {title} log (tail) ---", file=sys.stderr)
            for line in text.splitlines()[-80:]:
                print(line, file=sys.stderr)
    return 1


def read_jar_version(jar: Path) -> dict:
    with zipfile.ZipFile(jar) as archive:
        if "version.json" not in archive.namelist():
            raise ValueError("jar has no version.json")
        return json.loads(archive.read("version.json"))


def native_classifier() -> str:
    system = platform.system().lower()
    machine = platform.machine().lower()
    if system == "darwin":
        return "natives-macos-arm64" if machine in ("arm64", "aarch64") else "natives-macos"
    if system == "windows":
        return "natives-windows-arm64" if machine in ("arm64", "aarch64") else "natives-windows"
    if machine in ("arm64", "aarch64"):
        return "natives-linux-arm64"
    return "natives-linux"


def extract_natives(library_jars: list[Path], destination: Path) -> None:
    """Unpack the host classifier jars (and plain jtracy native jars)."""
    classifier = native_classifier()
    destination.mkdir(parents=True, exist_ok=True)
    unpacked = 0
    for jar in library_jars:
        name = jar.name
        if "natives" not in name:
            continue
        host_jar = f"-{classifier}.jar" in name
        # jtracy publishes `natives-macos` as the x64 file and a separate
        # `natives-macos-arm64`; older layouts have no classifier in the name.
        if not host_jar and "jtracy" in name and "natives" in name:
            host_jar = ("arm64" in name) == (platform.machine().lower() in ("arm64", "aarch64"))
        if not host_jar:
            continue
        with zipfile.ZipFile(jar) as archive:
            for member in archive.namelist():
                if member.endswith("/") or member.startswith("META-INF/"):
                    continue
                archive.extract(member, destination)
                unpacked += 1
    if unpacked == 0:
        raise ValueError(f"no {classifier} native jars under {library_jars[0].parent}")


def wait_for_join(server: subprocess.Popen, client: subprocess.Popen,
                  timeout: float) -> tuple[str | None, str]:
    """Drain the server pipe until the client joins or either process exits.

    Canonical sessions log ``joined the game``; translated T0 sessions log
    ``translated <protocol> <name> entered play``. Both mean the play state
    has started.
    """
    assert server.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(server.stdout, selectors.EVENT_READ)
    lines: list[str] = []
    deadline = time.monotonic() + timeout
    join_line = None
    while time.monotonic() < deadline:
        if client.poll() is not None:
            break
        if server.poll() is not None:
            break
        for _ in selector.select(0.2):
            line = server.stdout.readline()
            if not line:
                continue
            lines.append(line)
            if any(marker in line for marker in JOIN_MARKERS):
                join_line = line
                break
        if join_line is not None:
            break
    selector.close()
    return join_line, "".join(lines)


def wait_for_markers(server: subprocess.Popen, client: subprocess.Popen,
                     markers: tuple[str, ...], timeout: float) -> tuple[set[str], str]:
    """Drain the server pipe until every forced-menu step has been logged."""
    assert server.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(server.stdout, selectors.EVENT_READ)
    lines: list[str] = []
    seen: set[str] = set()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and len(seen) < len(markers):
        if client.poll() is not None or server.poll() is not None:
            break
        for _ in selector.select(0.2):
            line = server.stdout.readline()
            if not line:
                continue
            lines.append(line)
            seen.update(marker for marker in markers if marker in line)
    selector.close()
    return seen, "".join(lines)


def listen(server: subprocess.Popen, timeout: float) -> tuple[int | None, str]:
    assert server.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(server.stdout, selectors.EVENT_READ)
    lines: list[str] = []
    deadline = time.monotonic() + timeout
    port = None
    while time.monotonic() < deadline:
        if server.poll() is not None:
            break
        for _ in selector.select(0.2):
            line = server.stdout.readline()
            if not line:
                continue
            lines.append(line)
            match = re.search(r"listening on [\d.]+:(\d+)", line)
            if match:
                port = int(match.group(1))
                break
        if port is not None:
            break
    selector.close()
    return port, "".join(lines)


def read_log(path: Path) -> str:
    try:
        return path.read_text(errors="replace")
    except OSError:
        return ""


def drain_server(server: subprocess.Popen, clients: list[subprocess.Popen],
                 seconds: float) -> str:
    """Keep the server pipe flowing through the settle period."""
    assert server.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(server.stdout, selectors.EVENT_READ)
    lines: list[str] = []
    deadline = time.monotonic() + seconds
    while (time.monotonic() < deadline and server.poll() is None and
           not any(client.poll() is not None for client in clients)):
        for _ in selector.select(0.2):
            line = server.stdout.readline()
            if line:
                lines.append(line)
    selector.close()
    return "".join(lines)


def prepare_client(label: str, jar_path: str, libraries_path, natives_path,
                   temp_root: Path) -> dict:
    """The jar, version and classpath for one real client.

    Raises ValueError with the message the failure output should carry; the
    caller turns that into the test's exit code.
    """
    jar = Path(jar_path).expanduser().resolve()
    if not jar.is_file():
        raise ValueError(f"{label} client jar does not exist: {jar}")
    try:
        version_info = read_jar_version(jar)
    except (OSError, ValueError, zipfile.BadZipFile, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read {label} client jar {jar}: {exc}") from exc
    version = str(version_info.get("id", ""))
    protocol = version_info.get("protocol_version")
    if not isinstance(protocol, int):
        raise ValueError(f"{label} client jar {jar} has no numeric protocol_version")
    if not version:
        version = {775: "26.1", 776: "26.2", 777: "26.3"}.get(protocol, "")
    libraries_arg = "--canonical-libraries" if label == "canonical" else "--libraries"
    libraries = (Path(libraries_path).expanduser().resolve() if libraries_path
                 else REPO / "data" / "vanilla" / "_cache" / "real-client" / version
                 / "libraries")
    if not libraries.is_dir():
        raise ValueError(f"{label} client libraries not found: {libraries} "
                         f"(pass {libraries_arg})")
    library_jars = sorted(libraries.rglob("*.jar"))
    if not library_jars:
        raise ValueError(f"no library jars under {libraries}")
    natives = natives_path
    if natives is None:
        cached = libraries.parent / "natives"
        natives = cached if cached.is_dir() else temp_root / f"{label.lower()}-natives"
    natives = Path(natives).expanduser().resolve()
    if not natives.is_dir():
        try:
            extract_natives(library_jars, natives)
        except (OSError, ValueError, zipfile.BadZipFile) as exc:
            raise ValueError(f"cannot prepare {label} native libraries: {exc}") from exc
    classpath = os.pathsep.join([str(jar), *(str(path) for path in library_jars)])
    return {"jar": jar, "version": version, "protocol": protocol,
            "classpath": classpath, "natives": natives}


def launch_client(java: str, client: dict, username: str, game_dir: Path,
                  assets_dir: Path, quickplay: Path, port: int):
    """Start one real client; returns its process and its stdout log path."""
    command = [java]
    if platform.system() == "Darwin":
        command.append("-XstartOnFirstThread")
    command.extend([
        f"-Djava.library.path={client['natives']}",
        f"-Dorg.lwjgl.system.SharedLibraryExtractPath={client['natives']}",
        "-cp", client["classpath"],
        "net.minecraft.client.main.Main",
        "--username", username,
        "--version", client["version"],
        "--gameDir", str(game_dir),
        "--assetsDir", str(assets_dir),
        "--uuid", str(uuid.uuid5(uuid.NAMESPACE_DNS, f"plusmc-real-client-{username}")),
        "--accessToken", "0",
        "--versionType", "release",
        "--quickPlayPath", str(quickplay),
        "--quickPlayMultiplayer", f"127.0.0.1:{port}",
    ])
    game_dir.mkdir(parents=True, exist_ok=True)
    # A fresh game directory opens the accessibility onboarding screen, which
    # stops quick play from ever connecting, and the multiplayer warning would
    # need a click. Pre-seed the two options so the client lands on the title
    # screen and processes --quickPlayMultiplayer on its own.
    options = game_dir / "options.txt"
    if not options.exists():
        options.write_text("onboardAccessibility:false\nskipMultiplayerWarning:true\n")
    out_path = game_dir.parent / f"{username}.out"
    with out_path.open("w") as output:
        proc = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                cwd=game_dir,
                                env={**os.environ, "LIBGL_ALWAYS_SOFTWARE": "1"})
    return proc, out_path


def wait_for_line(server: subprocess.Popen, processes: list[subprocess.Popen],
                  marker: str, timeout: float) -> tuple[str | None, str]:
    """Drain the server pipe until a line contains `marker` or a process exits."""
    assert server.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(server.stdout, selectors.EVENT_READ)
    lines: list[str] = []
    found = None
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and found is None:
        if server.poll() is not None or any(p.poll() is not None for p in processes):
            break
        for _ in selector.select(0.2):
            line = server.stdout.readline()
            if not line:
                continue
            lines.append(line)
            if marker in line:
                found = line
                break
    selector.close()
    return found, "".join(lines)


def wait_for_visibility(server: subprocess.Popen,
                        processes: list[subprocess.Popen],
                        timeout: float) -> tuple[dict, str]:
    """Drain the pipe until the three tracking-pair states have been logged."""
    assert server.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(server.stdout, selectors.EVENT_READ)
    states: dict[str, tuple[int, int, int]] = {}
    lines: list[str] = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and len(states) < len(VISIBILITY_MARKERS):
        if server.poll() is not None or any(p.poll() is not None for p in processes):
            break
        for _ in selector.select(0.2):
            line = server.stdout.readline()
            if not line:
                continue
            lines.append(line)
            match = VISIBILITY_RE.search(line)
            if match is not None:
                states.setdefault(match.group(1),
                                  (int(match.group(2)), int(match.group(3)),
                                   int(match.group(4))))
    selector.close()
    return states, "".join(lines)


def play_failure(server: subprocess.Popen, clients: list[subprocess.Popen],
                 server_log: str, client_texts: list[str]) -> str | None:
    """The reason the clients did not stay in play, or None."""
    if server.poll() is not None:
        return "server exited while the real client was in play"
    for client in clients:
        if client.poll() is not None:
            return "a real client left play before the settle period ended"
    combined = server_log + "".join(client_texts)
    for marker in FAILURE_MARKERS:
        if marker in combined:
            return f"client/server reported {marker!r} during play"
    if "left the game" in server_log or "lost connection" in server_log:
        return "server reported the real client leaving play"
    return None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server", help="path to the plusmc binary")
    parser.add_argument("--client-jar", default=os.environ.get("PLUSMC_CLIENT_JAR"),
                        help="26.1/26.2 client jar (or set PLUSMC_CLIENT_JAR)")
    parser.add_argument("--libraries", type=Path,
                        help="directory containing the client's library jars")
    parser.add_argument("--natives", type=Path,
                        help="directory containing extracted client natives")
    parser.add_argument("--canonical-jar",
                        default=os.environ.get("PLUSMC_CANONICAL_CLIENT_JAR"),
                        help="26.3 client jar for the two-client visibility run "
                             "(or set PLUSMC_CANONICAL_CLIENT_JAR)")
    parser.add_argument("--canonical-libraries", type=Path,
                        help="directory containing the 26.3 client's library jars")
    parser.add_argument("--canonical-natives", type=Path,
                        help="directory containing extracted 26.3 client natives")
    parser.add_argument("--java", default="java", help="Java 21+ executable")
    parser.add_argument("--timeout", type=float, default=420.0,
                        help="seconds to reach play")
    parser.add_argument("--settle", type=float, default=20.0,
                        help="seconds to stay in play before declaring success")
    parser.add_argument("--keep", action="store_true",
                        help="keep the temporary game directory and logs")
    args = parser.parse_args(argv)

    if not args.client_jar:
        return skip("no client jar supplied (--client-jar or PLUSMC_CLIENT_JAR)")

    java = shutil.which(args.java)
    if java is None:
        return fail(f"Java executable not found: {args.java}")
    server_path = Path(args.server).resolve()
    if not server_path.is_file():
        return fail(f"server binary does not exist: {server_path}")

    temp_root = Path(tempfile.mkdtemp(prefix="plusmc-real-client-"))
    server: subprocess.Popen | None = None
    clients: list[subprocess.Popen] = []
    server_log = ""
    try:
        try:
            t0 = prepare_client("T0", args.client_jar, args.libraries, args.natives,
                                temp_root)
        except (OSError, ValueError, zipfile.BadZipFile) as exc:
            return fail(str(exc))
        if t0["protocol"] not in SUPPORTED_PROTOCOLS:
            return fail(f"client {t0['version']} uses protocol {t0['protocol']}; "
                        "only 26.1.x/26.2 (775/776) are supported")

        canonical = None
        if args.canonical_jar:
            try:
                canonical = prepare_client("canonical", args.canonical_jar,
                                           args.canonical_libraries,
                                           args.canonical_natives, temp_root)
            except (OSError, ValueError, zipfile.BadZipFile) as exc:
                return fail(str(exc))
            if canonical["protocol"] != CANONICAL_PROTOCOL:
                return fail(f"canonical client {canonical['version']} uses protocol "
                            f"{canonical['protocol']}; only 26.3 (777) is supported")

        assets_dir = temp_root / "assets"
        assets_dir.mkdir()
        server_command = [
            str(server_path), "--host", "127.0.0.1", "--port", "0",
            "--offline-mode", "--worldgen", "42", "--view-distance", "2",
            "--data", str(REPO / "data" / "vanilla"),
        ]
        server_command.append("--test-t0-visibility" if canonical is not None
                              else "--test-t0-menus")
        server = subprocess.Popen(server_command, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1)
        port, server_log = listen(server, min(args.timeout, 30.0))
        if port is None:
            return fail("server did not report a listening port",
                        server_log=server_log)

        t0_game = temp_root / "t0-game"
        t0_proc, t0_out = launch_client(java, t0, "RealClient", t0_game, assets_dir,
                                        temp_root / "t0-quickplay.log", port)
        clients.append(t0_proc)
        join_line, join_log = wait_for_join(server, t0_proc, args.timeout)
        server_log += join_log
        if join_line is None:
            if t0_proc.poll() is None:
                return fail(f"client did not join within {args.timeout:.0f}s",
                            server_log=server_log, client_log=read_log(t0_out))
            return fail("client exited before joining", server_log=server_log,
                        client_log=read_log(t0_out))
        expected = f"translated {t0['protocol']} RealClient entered play"
        if expected not in join_line:
            return fail(f"client reached play without the T0 join line {expected!r}",
                        server_log=server_log,
                        client_log=read_log(t0_game / "logs" / "latest.log"))

        if canonical is None:
            menu_seen, menu_log = wait_for_markers(server, t0_proc, MENU_MARKERS, 45.0)
            server_log += menu_log
            missing = [marker for marker in MENU_MARKERS if marker not in menu_seen]
            if missing:
                return fail(f"client did not accept the forced menus (missing {missing})",
                            server_log=server_log,
                            client_log=read_log(t0_game / "logs" / "latest.log"))
            server_log += drain_server(server, clients, args.settle)
            failure = play_failure(server, clients, server_log,
                                   [read_log(t0_out),
                                    read_log(t0_game / "logs" / "latest.log")])
            if failure is not None:
                return fail(failure, server_log=server_log,
                            client_log=read_log(t0_out))
            print(f"real client {t0['version']} (protocol {t0['protocol']}) joined, "
                  "stayed in play, and accepted the enchanting-table and anvil menus")
            return 0

        canon_game = temp_root / "canonical-game"
        canon_proc, canon_out = launch_client(
            java, canonical, CANONICAL_USERNAME, canon_game, assets_dir,
            temp_root / "canonical-quickplay.log", port)
        clients.append(canon_proc)
        joined, canon_log = wait_for_line(
            server, clients, f"{CANONICAL_USERNAME} joined the game", args.timeout)
        server_log += canon_log
        if joined is None:
            return fail(f"canonical client did not join within {args.timeout:.0f}s",
                        server_log=server_log, client_log=read_log(canon_out))

        states, visibility_log = wait_for_visibility(server, clients, 120.0)
        server_log += visibility_log
        missing = [phase for phase in VISIBILITY_MARKERS if phase not in states]
        if missing:
            return fail(f"tracking visibility did not complete (missing {missing})",
                        server_log=server_log,
                        client_log=read_log(t0_out) + read_log(canon_out))
        expected_states = {"near": (1, 1), "far": (0, 0), "back": (1, 1)}
        for phase in VISIBILITY_MARKERS:
            canonical_count, canonical_sees_t0, t0_sees_canonical = states[phase]
            want_t0, want_canonical = expected_states[phase]
            if canonical_count < 1 or (canonical_sees_t0, t0_sees_canonical) != (
                    want_t0, want_canonical):
                return fail(
                    f"visibility {phase}: canonical={canonical_count} "
                    f"canonical_sees_t0={canonical_sees_t0} "
                    f"t0_sees_canonical={t0_sees_canonical}",
                    server_log=server_log,
                    client_log=read_log(t0_out) + read_log(canon_out))

        server_log += drain_server(server, clients, args.settle)
        failure = play_failure(
            server, clients, server_log,
            [read_log(t0_out), read_log(t0_game / "logs" / "latest.log"),
             read_log(canon_out), read_log(canon_game / "logs" / "latest.log")])
        if failure is not None:
            return fail(failure, server_log=server_log,
                        client_log=read_log(t0_out) + read_log(canon_out))
        print(f"real clients {canonical['version']} (protocol "
              f"{canonical['protocol']}) and {t0['version']} (protocol "
              f"{t0['protocol']}) paired in both directions, respawned after a "
              "tracking-range exit, and stayed in play")
        return 0
    finally:
        for process in [*clients, server]:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        if server is not None and server.stdout is not None:
            server.stdout.close()
        if args.keep:
            (temp_root / "server.log").write_text(server_log)
            print(f"kept real-client artifacts in {temp_root}", file=sys.stderr)
        else:
            shutil.rmtree(temp_root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
