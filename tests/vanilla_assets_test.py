#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The vanilla-assets prepare step, without requiring Mojang data.

A synthetic client jar and a synthetic asset index/object store exercise the
whole path: version detection, jar extraction (including non-minecraft
namespaces), index objects (sounds), jar/index overwrite order, missing-object
reporting and traversal rejection. The real extraction is opt-in because the
player's client jar and asset store are not committed.
"""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import zipfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

from vanilla_assets import extract_assets, safe_asset_rel  # noqa: E402


def make_jar(path: Path) -> None:
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("version.json", json.dumps({"id": "test-1"}))
        archive.writestr("assets/minecraft/textures/block/stone.png", b"jar-stone")
        archive.writestr("assets/minecraft/models/block/stone.json",
                         b'{"parent":"block/cube_all"}')
        archive.writestr("assets/minecraft/lang/en_us.json", b'{"stone":"Stone"}')
        archive.writestr("assets/realms/lang/en_us.json", b'{"realms":"Realms"}')
        # A malicious name must be skipped, not written outside the tree.
        archive.writestr("assets/../escape.txt", b"bad")


def add_object(root: Path, logical: str, data: bytes) -> dict:
    digest = hashlib.sha1(data).hexdigest()
    path = root / "objects" / digest[:2] / digest
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return logical, {"hash": digest, "size": len(data)}


def make_assets(root: Path) -> None:
    objects = {}
    for logical, entry in (
        add_object(root, "minecraft/sounds/random/click.ogg", b"OggS-click"),
        # The store is authoritative: this overrides the jar's stone texture.
        add_object(root, "minecraft/textures/block/stone.png", b"store-stone"),
        add_object(root, "icons/icon_16x16.png", b"icon"),
    ):
        objects[logical] = entry
    objects["minecraft/sounds/missing.ogg"] = {"hash": "0" * 40, "size": 10}
    (root / "indexes").mkdir(parents=True)
    (root / "indexes" / "7.json").write_text(json.dumps({"objects": objects}))


def main() -> int:
    assert safe_asset_rel("minecraft/sounds/a.ogg") == "minecraft/sounds/a.ogg"
    assert safe_asset_rel("../escape") is None
    assert safe_asset_rel("minecraft/../escape") is None
    assert safe_asset_rel("/absolute") is None
    assert safe_asset_rel("minecraft\\escape") is None

    with tempfile.TemporaryDirectory(prefix="plusmc-assets-") as tmp:
        root = Path(tmp)
        jar = root / "client-test.jar"
        make_jar(jar)
        game_assets = root / "game-assets"
        make_assets(game_assets)
        out = root / "out"

        result = extract_assets(
            client_jar=jar,
            assets_dir=game_assets,
            index_id="7",
            out=out,
            repo=root,
        )
        assert result["version"] == "test-1", result
        assert result["assetIndex"] == "7", result
        assert result["indexAvailable"] is True, result
        assert result["fromJar"] == 4, result
        assert result["fromIndex"] == 3, result
        assert result["files"] == 6, result
        assert result["missingCount"] == 1, result
        assert result["missingObjects"][0] == "minecraft/sounds/missing.ogg", result
        assert result["skippedCount"] == 1, result

        assert (out / "minecraft/textures/block/stone.png").read_bytes() == b"store-stone"
        assert (out / "minecraft/sounds/random/click.ogg").read_bytes() == b"OggS-click"
        assert (out / "realms/lang/en_us.json").read_bytes() == b'{"realms":"Realms"}'
        assert not (out / "escape.txt").exists()
        manifest = json.loads((out / "_manifest.json").read_text())
        assert manifest["files"] == 6 and manifest["missingCount"] == 1, manifest

        jar_only = extract_assets(
            client_jar=jar,
            assets_dir=None,
            index_id=None,
            out=root / "jar-only",
            repo=root,
        )
        assert jar_only["indexAvailable"] is False, jar_only
        assert jar_only["files"] == 4, jar_only
        assert (root / "jar-only" / "minecraft/lang/en_us.json").is_file()

    print("ok: jar + index extraction, precedence, missing object and traversal checks")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
