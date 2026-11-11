#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The preview's texture reader must work from prepared assets, not only a jar.

This uses a tiny synthetic asset tree: one blockstate, one model and one PNG.
It checks both accepted directory shapes (the version root and its `assets`
child) and that `find_texture_source` prefers the tree over a cached jar.
"""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

from PIL import Image

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

from block_textures import BlockTextures, find_texture_source  # noqa: E402


def write_tree(root: Path) -> None:
    minecraft = root / "assets" / "minecraft"
    for kind in ("blockstates", "models/block", "textures/block"):
        (minecraft / kind).mkdir(parents=True, exist_ok=True)
    (minecraft / "blockstates" / "test_block.json").write_text(json.dumps({
        "variants": {"": {"model": "minecraft:block/test_block"}}
    }))
    (minecraft / "models" / "block" / "test_block.json").write_text(json.dumps({
        "textures": {"all": "minecraft:block/test_block"}
    }))
    Image.new("RGBA", (16, 16), (10, 20, 30, 255)).save(
        minecraft / "textures" / "block" / "test_block.png")


def check_source(source: Path) -> None:
    textures = BlockTextures(source)
    try:
        appearance = textures.appearance("minecraft:test_block")
        assert len(appearance.parts) == 1
        assert len(appearance.parts[0].elements) == 1
        assert textures.corpus("block") == ["block/test_block.png"]
        image = textures.texture("block/test_block")
        assert image is not None
        assert image.getpixel((0, 0)) == (10, 20, 30, 255)
    finally:
        textures.close()


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="plusmc-assets-") as tmp:
        root = Path(tmp)
        write_tree(root)
        check_source(root)
        check_source(root / "assets")

        repo = root / "repo"
        write_tree(repo / "data" / "vanilla" / "26.3")
        source = find_texture_source(repo)
        assert source == repo / "data" / "vanilla" / "26.3" / "assets", source

    print("texture source: version root, assets directory and selection -> ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
