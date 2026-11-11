#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pixel parity for the block textures used by the debug preview.

This is deliberately *not* a test of the +MC server: the server never renders a
texture. It measures the preview's texture path against the Java Edition assets
it claims to draw, one texel at a time.

Inputs are Mojang data and are never committed. The test exits 77 (CTest's
"skip") when neither prepared assets nor a ``client-*.jar`` is available; set
``PLUSMC_ASSETS`` for an extracted tree or ``PLUSMC_CLIENT_JAR`` for a jar.
When a source is present it:

* resolves every state in ``src/world/block_states.h`` through blockstate JSON,
  model parents, multipart conditions, model textures and PNGs;
* samples all six faces of every resolved state and compares the sampled texels
  with the source PNG (full-UV, unrotated layers only);
* loads every block and item texture in the jar and reports the corpus size;
* fails below 90% state resolution or below 90% matching pixels.

Usage:
    block_texture_parity_test.py [--repo PATH] [--assets PATH] [--min 0.90]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from PIL import Image

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

from block_textures import FACES, BlockTextures, find_texture_source  # noqa: E402


def state_rows(repo: Path, textures: BlockTextures | None = None) -> list[str]:
    text = (repo / "src/world/block_states.h").read_text()
    rows = []
    for name, props, _sid in re.findall(r'\{"([^"]+)", "([^"]*)", (\d+)\}', text):
        if textures is not None and not textures.has_blockstate(name):
            continue
        rows.append(f"{name}[{props}]" if props else name)
    return rows


def source_pixels(layers, size: int):
    """Composite the model's full-UV source layers at ``size`` x ``size``."""
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    resample = getattr(Image, "Resampling", Image).NEAREST
    for layer in layers:
        if layer.rotation % 360 != 0 or tuple(layer.uv) != (0.0, 0.0, 16.0, 16.0):
            return None
        frame = layer.image.crop((0, 0, layer.image.width, layer.image.width))
        out.alpha_composite(frame.resize((size, size), resample))
    if hasattr(out, "get_flattened_data"):
        return list(out.get_flattened_data())
    return list(out.getdata())  # Pillow < 12


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--assets", "--jar", dest="source", type=Path,
                        help="prepared asset tree or client jar")
    parser.add_argument("--min", type=float, default=0.90)
    args = parser.parse_args()

    source = args.source or find_texture_source(args.repo)
    if source is None or not source.exists():
        print("skip: no client assets; run tools/vanilla_assets.py, set "
              "PLUSMC_ASSETS, or put client-*.jar under data/vanilla/_cache/")
        return 77

    textures = BlockTextures(source)
    states = state_rows(args.repo, textures)
    if not states:
        raise AssertionError(f"no block states parsed from {args.repo}")

    air = {"minecraft:air", "minecraft:cave_air", "minecraft:void_air"}
    resolved = 0
    unresolved: list[str] = []
    compared = 0
    matched = 0
    sample_size = 4
    face_cache: dict[tuple, tuple[int, int] | None] = {}

    for index, state in enumerate(states):
        if index and index % 5000 == 0:
            print(f"  resolved {index}/{len(states)} states...", flush=True)
        try:
            appearance = textures.appearance(state)
        except Exception as exc:  # noqa: BLE001 - report the bad state, not a traceback
            unresolved.append(f"{state}: {type(exc).__name__}: {exc}")
            continue
        faces = {face: layers for face, layers in appearance.faces.items() if layers}
        if faces:
            resolved += 1
        elif state.split("[")[0] not in air:
            unresolved.append(state)
            continue
        for face, layers in faces.items():
            key = (face, tuple((layer.texture, layer.uv, layer.rotation, layer.tintindex)
                               for layer in layers))
            if key not in face_cache:
                expected = source_pixels(layers, sample_size)
                if expected is None:
                    face_cache[key] = None
                else:
                    actual = textures.face_pixels(state, face, sample_size)
                    face_cache[key] = (sum(1 for want, got in zip(expected, actual)
                                           if want == got), len(expected))
            result = face_cache[key]
            if result is not None:
                matched += result[0]
                compared += result[1]

    block_pngs = textures.corpus("block")
    item_pngs = textures.corpus("item")
    loaded_block = 0
    for ref in block_pngs:
        try:
            if textures.texture(ref) is not None:
                loaded_block += 1
        except Exception:  # noqa: BLE001 - the coverage count below is the result
            pass
    loaded_item = 0
    for ref in item_pngs:
        try:
            if textures.texture(ref) is not None:
                loaded_item += 1
        except Exception:  # noqa: BLE001 - the coverage count below is the result
            pass

    resolution = resolved / len(states)
    pixel_score = matched / compared if compared else 0.0
    print(f"client assets: {source}")
    print(f"block states: {len(states)}; resolved: {resolved} ({resolution:.4%}); "
          f"unresolved: {len(unresolved)}")
    print(f"source pixels compared: {compared}; matched: {matched} ({pixel_score:.4%})")
    print(f"block textures loaded: {loaded_block}/{len(block_pngs)}; "
          f"item textures loaded: {loaded_item}/{len(item_pngs)}")
    if unresolved:
        print("first unresolved states:")
        for state in unresolved[:10]:
            print(f"  {state}")

    if resolution < args.min:
        raise AssertionError(
            f"block-state texture resolution {resolution:.2%} is below {args.min:.0%}")
    if pixel_score < args.min:
        raise AssertionError(
            f"texture pixel parity {pixel_score:.2%} is below {args.min:.0%}")
    if loaded_block != len(block_pngs):
        raise AssertionError("not every block PNG in the client jar loaded")
    if loaded_item != len(item_pngs):
        raise AssertionError("not every item PNG in the client jar loaded")
    print("ok: block-texture parity meets the threshold")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
