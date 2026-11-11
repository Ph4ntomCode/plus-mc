#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prove the positive place-back frame assertion rejects changed reports/frames."""

from __future__ import annotations

import pathlib
import sys
import tempfile

from PIL import Image

TESTS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS))

from player_session_smoke_test import (  # noqa: E402
    SmokeFailure, check_restored_frame,
)

MATCHING_REPORT = "frame vs the pre-break frame after placing: 0 pixels differ\n"
NONZERO_REPORT = "frame vs the pre-break frame after placing: 1 pixels differ\n"
MISSING_REPORT = "no restored-frame report\n"


def write_frames(root: pathlib.Path, *, changed: bool) -> None:
    before = Image.new("RGB", (4, 4), (12, 34, 56))
    restored = before.copy()
    if changed:
        restored.putpixel((0, 0), (56, 34, 12))
    before.save(root / "before_break.png")
    restored.save(root / "restored.png")


def expect_rejected(root: pathlib.Path, report: str, expected: str,
                    label: str) -> bool:
    try:
        check_restored_frame(root, report, "control")
    except SmokeFailure as exc:
        if expected not in str(exc):
            print(f"FAIL: {label}: wrong rejection: {exc}", file=sys.stderr)
            return False
        print(f"{label}: rejected", flush=True)
        return True
    print(f"FAIL: {label}: was accepted", file=sys.stderr)
    return False


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="plusmc-frame-check-") as temp:
        root = pathlib.Path(temp)

        write_frames(root, changed=False)
        try:
            check_restored_frame(root, MATCHING_REPORT, "control")
        except SmokeFailure as exc:
            print(f"FAIL: identical frames were rejected: {exc}", file=sys.stderr)
            return 1
        print("identical restored frame accepted", flush=True)

        write_frames(root, changed=True)
        if not expect_rejected(
                root, MATCHING_REPORT,
                "restored.png differs from before_break.png by 1 pixels",
                "one changed restored pixel"):
            return 1

        write_frames(root, changed=False)
        if not expect_rejected(
                root, NONZERO_REPORT,
                "control: place-back left 1 pixels changed",
                "nonzero report with identical images"):
            return 1

        if not expect_rejected(
                root, MISSING_REPORT,
                "control: no restored-frame report",
                "missing restored-frame report"):
            return 1
        return 0


if __name__ == "__main__":
    sys.exit(main())
