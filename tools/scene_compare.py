#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare +MC preview frames with Java client screenshots.

This is the scene-level gate, separate from the model/texel parity tests. It
does not pretend to launch a Java client: supply a directory of reference PNGs
captured from Java at the same viewport, with the same filenames as the +MC
frames (`spawn.png`, `before_break.png`, ...). The score is RGB similarity
(`1 - mean absolute error / 255`); exact-match pixels are reported separately.

Usage:
    scene_compare.py --plusmc premium-server/playtest \
                     --java /path/to/java/screenshots
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image


def compare_images(plusmc_path: Path, java_path: Path) -> dict:
    a = Image.open(plusmc_path).convert("RGB")
    b = Image.open(java_path).convert("RGB")
    if a.size != b.size:
        raise ValueError(f"size mismatch: {plusmc_path} is {a.size}, {java_path} is {b.size}")
    pa, pb = a.tobytes(), b.tobytes()
    total = a.width * a.height
    exact = 0
    error = 0
    for i in range(0, len(pa), 3):
        ca, cb = pa[i:i + 3], pb[i:i + 3]
        if ca == cb:
            exact += 1
        error += abs(ca[0] - cb[0]) + abs(ca[1] - cb[1]) + abs(ca[2] - cb[2])
    similarity = 1.0 - error / (total * 3 * 255)
    return {
        "file": plusmc_path.name,
        "pixels": total,
        "exact": exact,
        "exact_ratio": exact / total,
        "similarity": similarity,
        "mae": error / (total * 3),
    }


def matching_frames(plusmc_dir: Path, java_dir: Path) -> list[tuple[Path, Path]]:
    pairs = []
    for java in sorted(java_dir.glob("*.png")):
        plusmc = plusmc_dir / java.name
        if plusmc.is_file():
            pairs.append((plusmc, java))
    return pairs


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plusmc", type=Path, required=True)
    parser.add_argument("--java", type=Path, required=True)
    parser.add_argument("--min", type=float, default=0.85)
    args = parser.parse_args(argv)
    pairs = matching_frames(args.plusmc, args.java)
    if not pairs:
        print("skip: no matching +MC/Java PNGs")
        return 77
    worst = None
    for plusmc, java in pairs:
        try:
            result = compare_images(plusmc, java)
        except ValueError as exc:
            print(f"FAIL: {exc}", file=sys.stderr)
            return 1
        print(f"{result['file']}: similarity {result['similarity']:.4%}; "
              f"exact {result['exact']}/{result['pixels']} "
              f"({result['exact_ratio']:.2%}); MAE {result['mae']:.2f}")
        if worst is None or result["similarity"] < worst["similarity"]:
            worst = result
    assert worst is not None
    if worst["similarity"] < args.min:
        print(f"FAIL: worst scene {worst['file']} is {worst['similarity']:.2%} "
              f"(below {args.min:.0%})", file=sys.stderr)
        return 1
    print(f"ok: {len(pairs)} scenes, worst similarity {worst['similarity']:.4%}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
