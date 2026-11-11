#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Item textures and inventory icons: coverage and source-texel parity.

This is the item counterpart to ``block_texture_parity_test.py``. It resolves
every item in the generated registry through the client jar's item-model
definitions, renders flat/generated icons, and compares those pixels with the
composited source sprites. Block items are rendered from their model boxes and
every non-transparent icon pixel is checked against the source model texels.
The inventory sheet is composed from the same icons. It is not a screenshot
comparison with a running Java client.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

from block_textures import BlockTextures, find_texture_source  # noqa: E402
from item_textures import ItemTextures  # noqa: E402


def pixels(image: Image.Image):
    if hasattr(image, "get_flattened_data"):
        return list(image.get_flattened_data())
    return list(image.getdata())  # Pillow < 12


def flat_expected(items: ItemTextures, name: str, size: int = 16):
    layers = items.model_layers(name)
    if not layers:
        return None
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    resample = getattr(Image, "Resampling", Image).NEAREST
    for layer in layers:
        out.alpha_composite(layer.resize((size, size), resample))
    return pixels(out)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets", "--jar", dest="source", type=Path,
                        help="prepared asset tree or client jar")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    source = args.source or find_texture_source(repo)
    if source is None or not source.exists():
        print("skip: no client assets for item textures")
        return 77
    blocks = BlockTextures(source)
    items = ItemTextures(blocks)
    names = sorted(f"minecraft:{name}" for name in blocks.definitions("items"))
    assert names, "the client assets carry no item definitions"

    models = 0
    special_icons = 0
    block_icons = 0
    block_compared = 0
    block_matched = 0
    flat_items = 0
    compared = 0
    matched = 0
    missing = []
    for name in names:
        ref = items.model_ref(name)
        if ref is None:
            missing.append(name)
            continue
        models += 1
        if items.special_kind(name):
            icon = items.icon(name, 16)
            if icon is None:
                missing.append(name)
                continue
            special_icons += 1
            continue
        model = blocks.model(ref)
        if model.elements:
            icon = items.icon(name, 16)
            if icon is None:
                missing.append(name)
                continue
            # Every icon texel is a model texel: the isometric ray writes the
            # source RGBA directly, so compare it with the full model palette
            # rather than merely asserting that the icon is non-empty.
            parts = blocks.parts_for_model(ref)
            palette = set()
            for part in parts.elements:
                for layer in part.faces.values():
                    palette.update(pixels(layer.image))
            block_icons += 1
            for pixel in pixels(icon):
                if pixel[3]:
                    block_compared += 1
                    if pixel in palette:
                        block_matched += 1
            continue
        flat_items += 1
        expected = flat_expected(items, name)
        actual = items.icon(name, 16)
        if expected is None or actual is None:
            missing.append(name)
            continue
        for want, got in zip(expected, pixels(actual)):
            compared += 1
            if want == got:
                matched += 1

    slots = {
        36: {"name": "minecraft:stone", "count": 1},
        37: {"name": "minecraft:oak_stairs", "count": 2},
        38: {"name": "minecraft:chest", "count": 1},
        39: {"name": "minecraft:white_shulker_box", "count": 1},
        40: {"name": "minecraft:decorated_pot", "count": 1},
        41: {"name": "minecraft:skeleton_skull", "count": 1},
        42: {"name": "minecraft:red_banner", "count": 1},
        43: {"name": "minecraft:shield", "count": 1},
        44: {"name": "minecraft:copper_golem_statue", "count": 1},
    }
    sheet = items.render_inventory(slots, selected=0)
    assert sheet.size == (176, 166)
    sheet_pixels = sum(1 for p in pixels(sheet) if p[3] and p[3] > 0)
    assert sheet_pixels > 1000, sheet_pixels
    hud = items.render_hotbar(slots, selected=2)
    assert hud.size == (182, 34)
    assert sum(1 for p in pixels(hud) if p[3]) > 1000
    for name in ("minecraft:chest", "minecraft:white_shulker_box",
                 "minecraft:decorated_pot", "minecraft:skeleton_skull",
                 "minecraft:player_head", "minecraft:red_banner",
                 "minecraft:shield", "minecraft:copper_golem_statue"):
        assert items.special_kind(name) is not None, name
        icon = items.icon(name, 16)
        assert icon is not None and any(p[3] for p in pixels(icon)), name

    model_score = models / len(names)
    pixel_score = matched / compared if compared else 0.0
    block_score = block_matched / block_compared if block_compared else 0.0
    print(f"client assets: {source}")
    print(f"items: {len(names)}; models resolved: {models} ({model_score:.4%}); "
          f"missing: {len(missing)}")
    print(f"special item icons: {special_icons}; flat item icons: {flat_items}; "
          f"source texels compared: {compared}; matched: {matched} ({pixel_score:.4%})")
    print(f"block item icons: {block_icons}; source texels compared: {block_compared}; "
          f"matched: {block_matched} ({block_score:.4%}); HUD: {hud.width}x{hud.height}; "
          f"inventory sheet: {sheet.width}x{sheet.height}, {sheet_pixels} opaque pixels")
    if missing:
        print("first missing items:", ", ".join(missing[:10]))
    expected_missing = ["minecraft:air"] if "minecraft:air" in names else []
    assert missing == expected_missing, f"unexpected missing items: {missing[:10]}"
    if model_score < 0.90:
        raise AssertionError(f"item model coverage {model_score:.2%} is below 90%")
    if pixel_score < 0.90:
        raise AssertionError(f"flat item pixel parity {pixel_score:.2%} is below 90%")
    if block_score < 0.90:
        raise AssertionError(f"block item pixel parity {block_score:.2%} is below 90%")
    print("ok: item textures and inventory preview meet the threshold")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
