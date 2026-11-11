#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The debug preview must intersect the block model, not the full cube.

These checks use the same Java blockstate/model data as the texture parity
test. They are deliberately geometric: a bottom slab must let a ray through
its upper half, and a bottom stair's back step must be lower than a cube's
top. A machine without the gitignored client jar skips (77).
"""

from __future__ import annotations

import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

from block_textures import BlockTextures, find_texture_source  # noqa: E402

STONE = "minecraft:stone"
BOTTOM_SLAB = "minecraft:oak_slab[type=bottom,waterlogged=false]"
TOP_SLAB = "minecraft:oak_slab[type=top,waterlogged=false]"
BOTTOM_STAIR = "minecraft:oak_stairs[facing=east,half=bottom,shape=straight]"
LOG_X = "minecraft:oak_log[axis=x]"


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    source = find_texture_source(repo)
    if source is None:
        print("skip: no client assets for block geometry")
        return 77
    blocks = BlockTextures(source)

    stone = blocks.appearance(STONE)
    assert len(stone.parts) == 1
    assert len(stone.parts[0].elements) == 1
    assert stone.parts[0].elements[0].lo == (0.0, 0.0, 0.0)
    assert stone.parts[0].elements[0].hi == (16.0, 16.0, 16.0)
    hit = blocks.surface_hit(STONE, (-16.0, 8.0, 8.0), (1.0, 0.0, 0.0))
    assert hit is not None and hit.face == "west" and abs(hit.distance - 1.0) < 1e-6
    assert blocks.surface_hit(STONE, (8.0, 32.0, 8.0), (0.0, -1.0, 0.0)) is not None

    bottom = blocks.appearance(BOTTOM_SLAB)
    assert len(bottom.parts) == 1
    assert bottom.parts[0].elements[0].hi[1] == 8.0
    assert blocks.surface_hit(BOTTOM_SLAB, (-16.0, 4.0, 8.0), (1.0, 0.0, 0.0)) is not None
    assert blocks.surface_hit(BOTTOM_SLAB, (-16.0, 12.0, 8.0), (1.0, 0.0, 0.0)) is None
    top = blocks.appearance(TOP_SLAB)
    assert top.parts[0].elements[0].lo[1] == 8.0
    assert blocks.surface_hit(TOP_SLAB, (-16.0, 4.0, 8.0), (1.0, 0.0, 0.0)) is None
    assert blocks.surface_hit(TOP_SLAB, (-16.0, 12.0, 8.0), (1.0, 0.0, 0.0)) is not None

    stair = blocks.appearance(BOTTOM_STAIR)
    assert len(stair.parts) == 1
    assert len(stair.parts[0].elements) == 2
    # The upper back step is a real step: a ray from above at x=4 reaches the
    # lower base, while at x=12 it reaches the raised step.
    front = blocks.surface_hit(BOTTOM_STAIR, (4.0, 32.0, 8.0), (0.0, -1.0, 0.0))
    back = blocks.surface_hit(BOTTOM_STAIR, (12.0, 32.0, 8.0), (0.0, -1.0, 0.0))
    assert front is not None and back is not None
    assert front.distance > back.distance, (front, back)

    log_parts = blocks.appearance(LOG_X)
    assert len(log_parts.parts) == 1
    assert log_parts.parts[0].rot_x == 90.0
    assert log_parts.parts[0].rot_y == 90.0
    assert blocks.surface_hit(LOG_X, (8.0, 32.0, 8.0), (0.0, -1.0, 0.0)) is not None

    print(f"block geometry: {STONE}, slabs, stairs, and rotated logs -> ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
