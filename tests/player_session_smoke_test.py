#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the real player session in every dimension and assert its output.

This is deliberately an integration smoke test: it starts the server, joins as
a player, receives and renders the world, and checks every frame plus the
inventory sheet. The overworld case also opens the chest GUI. The wall-clock
budget is a fixed outer timeout so a hung server or renderer fails instead of
blocking CTest indefinitely.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image

REPO = pathlib.Path(__file__).resolve().parent.parent
TOOLS = REPO / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(REPO / "tests"))

from block_textures import find_texture_source  # noqa: E402
from player_session import (  # noqa: E402
    SAMPLE_COUNTS, SAMPLE_DISPLAY_NAMES, SAMPLE_ITEMS, pixels_differing,
)

BUDGET_SECONDS = 180
SKIPPED_FRAME_MIN_PIXELS = 100
CASES = (
    ("overworld", "minecraft:overworld", True),
    ("nether", "minecraft:nether", False),
    ("end", "minecraft:the_end", False),
)
COMMON_FRAMES = (
    "inventory.png",
    "view_yaw0.png",
    "view_yaw90.png",
    "view_yaw180.png",
    "view_yaw270.png",
    "spawn.png",
    "walked_horizon.png",
    "before_break.png",
    "after_break.png",
    "restored.png",
)


class SmokeFailure(AssertionError):
    pass


def check_image(path: pathlib.Path, size: tuple[int, int]) -> None:
    if not path.is_file():
        raise SmokeFailure(f"missing {path.name}")
    if path.stat().st_size <= 500:
        raise SmokeFailure(f"empty {path.name}")
    with Image.open(path) as image:
        if image.size != size:
            raise SmokeFailure(f"{path.name}: {image.size} != {size}")
        colors = image.convert("RGB").getcolors(maxcolors=1 << 20)
    if not colors or len(colors) <= 4:
        raise SmokeFailure(f"{path.name} is blank")


def check_placement(out_dir: pathlib.Path, output: str, dimension: str) -> None:
    """Require the server log to confirm the exact block and coordinates."""
    broke = re.search(
        r"breaking (minecraft:[^\s]+) at \((-?\d+), (-?\d+), (-?\d+)\)",
        output)
    if broke is None:
        raise SmokeFailure(f"{dimension}: no broken-block report")
    block = broke.group(1).split("[", 1)[0]
    x, y, z = (int(broke.group(i)) for i in (2, 3, 4))
    log_path = out_dir / "server.log"
    if not log_path.is_file():
        raise SmokeFailure(f"{dimension}: missing server.log")
    placement = f"placed {block} at ({x}, {y}, {z})"
    if placement not in log_path.read_text(errors="replace"):
        raise SmokeFailure(
            f"{dimension}: server log has no exact {placement!r}")


def check_skipped_frame(out_dir: pathlib.Path, output: str) -> int:
    """Require no restore: the skipped frame must equal the after-break frame."""
    report = re.search(
        r"^frame vs the pre-break frame after skipping place-back: "
        r"(\d+) pixels differ$", output, re.MULTILINE)
    if report is None:
        raise SmokeFailure("negative: no skipped-frame difference report")
    reported = int(report.group(1))
    if reported < SKIPPED_FRAME_MIN_PIXELS:
        raise SmokeFailure(
            f"negative: skipped restore changed only {reported} pixels")

    before_path = out_dir / "before_break.png"
    after_path = out_dir / "after_break.png"
    restored_path = out_dir / "restored.png"
    for path in (before_path, after_path, restored_path):
        if not path.is_file():
            raise SmokeFailure(f"negative: missing {path.name}")
    with Image.open(before_path) as before_image, \
            Image.open(after_path) as after_image, \
            Image.open(restored_path) as restored_image:
        before_frame = before_image.convert("RGB")
        after_frame = after_image.convert("RGB")
        restored_frame = restored_image.convert("RGB")
        actual = pixels_differing(before_frame, restored_frame)
        unrepaired = pixels_differing(after_frame, restored_frame)
    if actual != reported:
        raise SmokeFailure(
            f"negative: frame report says {reported}, images differ by {actual}")
    if unrepaired:
        raise SmokeFailure(
            f"negative: restored frame differs from after_break by "
            f"{unrepaired} pixels; place-back was not skipped")
    return actual


def check_restored_frame(out_dir: pathlib.Path, output: str,
                         dimension: str) -> None:
    """Require successful place-back to restore the exact pre-break view."""
    report = re.search(
        r"^frame vs the pre-break frame after placing: (\d+) pixels differ$",
        output, re.MULTILINE)
    if report is None:
        raise SmokeFailure(f"{dimension}: no restored-frame report")
    reported = int(report.group(1))
    if reported:
        raise SmokeFailure(
            f"{dimension}: place-back left {reported} pixels changed")

    before_path = out_dir / "before_break.png"
    restored_path = out_dir / "restored.png"
    for path in (before_path, restored_path):
        if not path.is_file():
            raise SmokeFailure(f"{dimension}: missing {path.name}")
    with Image.open(before_path) as before_image, \
            Image.open(restored_path) as restored_image:
        actual = pixels_differing(
            before_image.convert("RGB"), restored_image.convert("RGB"))
    if actual:
        raise SmokeFailure(
            f"{dimension}: restored.png differs from before_break.png by "
            f"{actual} pixels")


def check_case(out_dir: pathlib.Path, output: str, dimension: str,
               dimension_name: str, expect_container: bool) -> None:
    if f"dimension {dimension_name}" not in output:
        raise SmokeFailure(f"{dimension}: joined a different dimension")
    if "chunks: 81/81 streamed" not in output:
        raise SmokeFailure(f"{dimension}: expected chunk batch did not arrive")
    if "textures:" not in output:
        raise SmokeFailure(f"{dimension}: no texture/fallback report")

    frames = COMMON_FRAMES + (("container.png",) if expect_container else ())
    for name in frames:
        size = (176, 166) if name == "inventory.png" else (400, 225)
        check_image(out_dir / name, size)

    required = (
        "inventory:",
        "9 hotbar stacks",
        "frames: view_yaw0/90/180/270.png + spawn.png",
        "block updates received:",
        "place back on top of",
        "block update received",
        "frame vs the pre-break frame after placing: 0 pixels differ",
    )
    for line in required:
        if line not in output:
            raise SmokeFailure(f"{dimension}: missing output {line!r}")
    check_restored_frame(out_dir, output, dimension)
    if expect_container:
        for line in ("container: window", "63 slots", "9 occupied"):
            if line not in output:
                raise SmokeFailure(f"{dimension}: missing container output {line!r}")

    check_placement(out_dir, output, dimension)


def run_negative_place_back(binary: pathlib.Path, keep: bool) -> int:
    """Run a real session with place-back skipped and require rejection."""
    root = pathlib.Path(tempfile.mkdtemp(prefix="plusmc-smoke-negative-"))
    out_dir = root / "overworld"
    out_dir.mkdir()
    command = [sys.executable, str(TOOLS / "player_session.py"),
               str(binary), str(out_dir), "CTestSmokeNeg",
               "--dimension", "overworld", "--render-scale", "0.2",
               "--selected", "0", "--skip-place-back"]
    print("$ " + " ".join(command), flush=True)

    def fail(message: str) -> int:
        print(f"FAIL: {message}", file=sys.stderr)
        print(f"artifacts kept at {root}", file=sys.stderr)
        return 1

    started = time.monotonic()
    try:
        result = subprocess.run(
            command, cwd=REPO, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=BUDGET_SECONDS)
    except subprocess.TimeoutExpired as exc:
        print((exc.stdout or "")[-6000:], file=sys.stderr)
        return fail(f"negative: player_session exceeded {BUDGET_SECONDS}s")
    elapsed = time.monotonic() - started
    output = result.stdout or ""
    print(output[-12000:], flush=True)
    if result.returncode != 0:
        return fail(f"negative: player_session exited {result.returncode}")
    if "place back skipped: --skip-place-back" not in output:
        return fail("negative: session did not report skipping place-back")
    try:
        frame_diff = check_skipped_frame(out_dir, output)
    except SmokeFailure as exc:
        return fail(str(exc))
    broke = re.search(
        r"breaking (minecraft:[^\s]+) at \((-?\d+), (-?\d+), (-?\d+)\)",
        output)
    if broke is None:
        return fail("negative: no broken-block report")
    block = broke.group(1).split("[", 1)[0]
    x, y, z = (int(broke.group(i)) for i in (2, 3, 4))
    expected = (
        f"server log has no exact 'placed {block} at ({x}, {y}, {z})'")
    try:
        check_placement(out_dir, output, "negative")
    except SmokeFailure as exc:
        if expected not in str(exc):
            return fail(f"negative: unexpected checker failure: {exc}")
    else:
        return fail("negative: exact placement check accepted a skipped place-back")
    print(f"negative smoke: real session skipped place-back; restored frame "
          f"differs by {frame_diff} pixels and matches after_break; exact "
          f"placement rejected in {elapsed:.1f}s", flush=True)
    if not keep:
        shutil.rmtree(root, ignore_errors=True)
    return 0


def run_case(binary: pathlib.Path, root: pathlib.Path, dimension: str,
             dimension_name: str, expect_container: bool) -> None:
    out_dir = root / dimension
    out_dir.mkdir()
    command = [sys.executable, str(TOOLS / "player_session.py"),
               str(binary), str(out_dir), "CTestSmoke",
               "--dimension", dimension, "--render-scale", "0.2",
               "--selected", "0"]
    if expect_container:
        command.append("--container")
    print("$ " + " ".join(command), flush=True)
    started = time.monotonic()
    try:
        result = subprocess.run(
            command, cwd=REPO, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=BUDGET_SECONDS)
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        print(output[-6000:], file=sys.stderr)
        raise SmokeFailure(
            f"{dimension}: player_session exceeded {BUDGET_SECONDS}s") from exc
    elapsed = time.monotonic() - started
    output = result.stdout or ""
    print(output[-12000:], flush=True)
    if result.returncode != 0:
        raise SmokeFailure(f"{dimension}: player_session exited {result.returncode}")
    if elapsed > BUDGET_SECONDS:
        raise SmokeFailure(
            f"{dimension}: player_session took {elapsed:.1f}s > {BUDGET_SECONDS}s")
    check_case(out_dir, output, dimension, dimension_name, expect_container)
    print(f"{dimension}: {len(COMMON_FRAMES) + expect_container} outputs "
          f"in {elapsed:.1f}s", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", nargs="?", type=pathlib.Path,
                        default=REPO / "build" / "src" / "plusmc")
    parser.add_argument("--keep", action="store_true",
                        help="keep the temporary output directory")
    parser.add_argument("--negative-place-back", action="store_true",
                        help="run a real session with place-back skipped and "
                             "require the checker to reject it")
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        print(f"FAIL: missing server binary {binary}", file=sys.stderr)
        return 1
    source = find_texture_source(REPO)
    if source is None:
        print("SKIP: no client assets (run tools/vanilla_assets.py or set PLUSMC_ASSETS)")
        return 77
    if args.negative_place_back:
        return run_negative_place_back(binary, args.keep)

    root = pathlib.Path(tempfile.mkdtemp(prefix="plusmc-smoke-"))
    print(f"working directory: {root}", flush=True)
    try:
        for dimension, dimension_name, expect_container in CASES:
            run_case(binary, root, dimension, dimension_name,
                     expect_container)
    except SmokeFailure as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        print(f"artifacts kept at {root}", file=sys.stderr)
        return 1

    assert SAMPLE_ITEMS[0] == "minecraft:potion"
    assert SAMPLE_DISPLAY_NAMES[0] == "Healing Draught"
    assert SAMPLE_COUNTS[0] == 1
    print("player session smoke: overworld + container, nether and end -> ok")
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
