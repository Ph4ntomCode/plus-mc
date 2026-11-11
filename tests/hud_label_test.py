#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pixel regression for the selected-stack name in the preview HUD.

Every 3D frame composites the same 182x34 hotbar overlay. The custom potion's
label is drawn above that overlay, so the rendered label pixels can be compared
against a mask built with the same PIL font and placement as `render_hotbar`.
A missing HUD, a fallback to the plain item name, or a shifted label all lower
the match score below the threshold.
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

LABEL = "Healing Draught"
FRAME_SIZE = (400, 225)
HUD_SIZE = (182, 34)
LABEL_HEIGHT = 12
ORIGIN = ((FRAME_SIZE[0] - HUD_SIZE[0]) // 2,
          FRAME_SIZE[1] - HUD_SIZE[1])
FRAMES = (
    "view_yaw0.png", "view_yaw90.png", "view_yaw180.png", "view_yaw270.png",
    "spawn.png", "walked_horizon.png", "before_break.png", "after_break.png",
    "restored.png",
)


def label_mask(text: str) -> Image.Image:
    """The white glyphs of ItemTextures.render_hotbar's label."""
    image = Image.new("RGBA", (HUD_SIZE[0], LABEL_HEIGHT), (0, 0, 0, 0))
    font = ImageFont.load_default()
    draw = ImageDraw.Draw(image)
    width = int(draw.textlength(text, font=font))
    x = (HUD_SIZE[0] - width) // 2
    draw.text((x, 0), text, fill=(255, 255, 255, 255), font=font)
    return image


def match_score(frame: Image.Image, mask: Image.Image) -> float:
    """Fraction of core glyph pixels that are bright in the rendered frame.

    The label is composited with alpha, so even a fully covered glyph pixel
    lands just below #ffffff over the hotbar. Comparing its luminance rather
    than one exact colour still pins down the shapes and their placement.
    """
    matched = total = 0
    for y in range(mask.height):
        for x in range(mask.width):
            alpha = mask.getpixel((x, y))[3]
            if alpha < 128:
                continue
            total += 1
            r, g, b = frame.getpixel((ORIGIN[0] + x, ORIGIN[1] + y))
            if 0.2126 * r + 0.7152 * g + 0.0722 * b >= 150:
                matched += 1
    return matched / total if total else 0.0


def check_directory(directory: Path) -> None:
    custom = label_mask(LABEL)
    fallback = label_mask("Potion")
    for name in FRAMES:
        path = directory / name
        assert path.is_file(), f"missing frame {name}"
        with Image.open(path) as source:
            frame = source.convert("RGB")
        assert frame.size == FRAME_SIZE, f"{name}: {frame.size} != {FRAME_SIZE}"
        custom_score = match_score(frame, custom)
        fallback_score = match_score(frame, fallback)
        assert custom_score >= 0.90, (
            f"{name}: custom label matched {custom_score:.1%} of its pixels "
            f"(fallback {fallback_score:.1%})")
        assert custom_score > fallback_score + 0.20, (
            f"{name}: custom label is not more likely than the plain name "
            f"({custom_score:.1%} vs {fallback_score:.1%})")
    print(f"hud label: {LABEL!r} present in all {len(FRAMES)} frames -> ok")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} FRAMES_DIR", file=sys.stderr)
        return 2
    try:
        check_directory(Path(sys.argv[1]))
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
