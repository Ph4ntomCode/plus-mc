#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare the client asset tree from the player's own Minecraft copy.

+MC is a server: it sends block states, not textures. This tool exists for the
debug preview and the future companion client. Like Horizon's prepare step, it
ships no Mojang files: it reads the player's client jar and asset store and
writes a derived tree under the git-ignored ``data/vanilla/<version>/assets``.

Two sources are combined:

* ``assets/**`` in the client jar: block/item models, textures, fonts, shaders,
  language files and the other resources embedded in the jar;
* ``assets/indexes/<id>.json`` plus ``assets/objects``: the game's own asset
  store, which is where sounds live (the client jar contains no ``.ogg`` files).

The jar's ``version.json`` names the version. The asset-index id comes from the
launcher's version JSON or from ``data/vanilla/_cache/<version>.meta.json``,
which the vanilla-data tool already caches. Nothing is downloaded; a missing
index is reported instead of guessing a different release's sounds.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parent.parent


def default_minecraft_dir() -> Path | None:
    """The standard Java Edition game directory, when it exists."""
    env = os.environ.get("PLUSMC_MINECRAFT_DIR")
    if env:
        path = Path(env).expanduser()
        return path if path.is_dir() else None
    home = Path.home()
    if sys.platform == "darwin":
        candidates = [home / "Library" / "Application Support" / "minecraft"]
    elif os.name == "nt":
        appdata = os.environ.get("APPDATA")
        candidates = [Path(appdata) / ".minecraft"] if appdata else []
    else:
        candidates = [home / ".minecraft"]
        xdg = os.environ.get("XDG_DATA_HOME")
        if xdg:
            candidates.append(Path(xdg) / "minecraft")
    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    return None


def find_client_jar(repo: Path = REPO, version: str | None = None) -> Path | None:
    """Find a cached client jar, preferring an explicit environment path."""
    env = os.environ.get("PLUSMC_CLIENT_JAR")
    if env:
        path = Path(env).expanduser()
        if path.is_file():
            return path
    cache = repo / "data" / "vanilla" / "_cache"
    if version:
        path = cache / f"client-{version}.jar"
        return path if path.is_file() else None
    candidates = sorted(cache.glob("client-*.jar"))
    return candidates[-1] if candidates else None


def safe_asset_rel(name: str) -> str | None:
    """Accept a relative asset path, refusing traversal and absolute names."""
    if not name or "\x00" in name or "\\" in name or name.startswith("/"):
        return None
    parts = name.split("/")
    if any(part in ("", ".", "..") for part in parts):
        return None
    if ":" in parts[0]:
        return None
    return "/".join(parts)


def _read_json(path: Path | None) -> dict[str, Any] | None:
    if path is None:
        return None
    try:
        doc = json.loads(path.read_text())
    except (OSError, ValueError):
        return None
    return doc if isinstance(doc, dict) else None


def _jar_version(jar: Path | None) -> dict[str, Any]:
    if jar is None:
        return {}
    try:
        with zipfile.ZipFile(jar) as archive:
            doc = json.loads(archive.read("version.json"))
    except (KeyError, OSError, ValueError, zipfile.BadZipFile):
        return {}
    return doc if isinstance(doc, dict) else {}


def _infer_version(jar: Path | None, version_doc: dict[str, Any]) -> str:
    value = version_doc.get("id")
    if isinstance(value, str) and value:
        return value
    if jar is not None and jar.name.startswith("client-") and jar.name.endswith(".jar"):
        return jar.name[len("client-"):-len(".jar")]
    return "unknown"


def _index_id_from_doc(doc: dict[str, Any]) -> str | None:
    asset_index = doc.get("assetIndex")
    if isinstance(asset_index, dict):
        value = asset_index.get("id")
        if isinstance(value, str) and value:
            return value
    if isinstance(asset_index, str) and asset_index:
        return asset_index
    # Older version JSONs spell the id at the top level.
    value = doc.get("assets")
    if isinstance(value, str) and value:
        return value
    return None


def discover_asset_index_id(
    version: str,
    jar_doc: dict[str, Any],
    minecraft_dir: Path | None,
    repo: Path = REPO,
) -> str | None:
    """Find the asset index belonging to this exact version."""
    value = _index_id_from_doc(jar_doc)
    if value:
        return value
    candidates: list[Path] = []
    if minecraft_dir is not None and version != "unknown":
        candidates.extend([
            minecraft_dir / "versions" / version / f"{version}.json",
            minecraft_dir / "versions" / version / "version.json",
        ])
    if version != "unknown":
        candidates.append(repo / "data" / "vanilla" / "_cache" / f"{version}.meta.json")
    for path in candidates:
        value = _index_id_from_doc(_read_json(path) or {})
        if value:
            return value
    return None


def _copy_to(src, dest: Path) -> int:
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    with part.open("wb") as out:
        shutil.copyfileobj(src, out, length=1024 * 1024)
    size = part.stat().st_size
    part.replace(dest)
    return size


def extract_assets(
    *,
    client_jar: Path | None,
    assets_dir: Path | None,
    index_id: str | None,
    out: Path,
    repo: Path = REPO,
    version: str | None = None,
    minecraft_dir: Path | None = None,
) -> dict[str, Any]:
    """Extract jar assets and, when available, the matching asset store."""
    jar = Path(client_jar) if client_jar else None
    if jar is not None and not jar.is_file():
        raise FileNotFoundError(jar)
    version_doc = _jar_version(jar)
    version = version or _infer_version(jar, version_doc)
    if not index_id:
        index_id = discover_asset_index_id(version, version_doc, minecraft_dir, repo)
    index_path = (assets_dir / "indexes" / f"{index_id}.json"
                  if assets_dir is not None and index_id else None)
    index_doc = _read_json(index_path) if index_path is not None else None
    if jar is None and index_doc is None:
        raise FileNotFoundError("no client jar and no readable asset index")

    out.mkdir(parents=True, exist_ok=True)
    sizes: dict[str, int] = {}
    from_jar = 0
    from_index = 0
    missing: list[str] = []
    skipped: list[str] = []

    if jar is not None:
        with zipfile.ZipFile(jar) as archive:
            for info in archive.infolist():
                if info.is_dir() or not info.filename.startswith("assets/"):
                    continue
                rel = safe_asset_rel(info.filename[len("assets/"):])
                if rel is None:
                    skipped.append(info.filename)
                    continue
                with archive.open(info) as src:
                    sizes[rel] = _copy_to(src, out / rel)
                from_jar += 1

    if index_doc is not None:
        objects = index_doc.get("objects")
        if not isinstance(objects, dict):
            raise ValueError(f"{index_path}: no objects map")
        assert assets_dir is not None
        for logical, meta in objects.items():
            logical = str(logical)
            rel = safe_asset_rel(logical)
            if rel is None:
                skipped.append(logical)
                continue
            if not isinstance(meta, dict):
                missing.append(logical)
                continue
            digest = meta.get("hash")
            if not isinstance(digest, str) or len(digest) != 40:
                missing.append(logical)
                continue
            digest = digest.lower()
            if any(c not in "0123456789abcdef" for c in digest):
                missing.append(logical)
                continue
            obj = assets_dir / "objects" / digest[:2] / digest
            if not obj.is_file():
                missing.append(logical)
                continue
            expected = meta.get("size")
            if isinstance(expected, int) and expected >= 0 and obj.stat().st_size != expected:
                missing.append(logical)
                continue
            with obj.open("rb") as src:
                sizes[rel] = _copy_to(src, out / rel)
            from_index += 1

    manifest: dict[str, Any] = {
        "version": version,
        "assetIndex": index_id,
        "clientJar": str(jar) if jar is not None else None,
        "assetStore": str(assets_dir) if assets_dir is not None else None,
        "assetIndexFile": str(index_path) if index_doc is not None else None,
        "indexAvailable": index_doc is not None,
        "files": len(sizes),
        "bytes": sum(sizes.values()),
        "fromJar": from_jar,
        "fromIndex": from_index,
        "missingCount": len(missing),
        "missingObjects": missing[:50],
        "skippedCount": len(skipped),
        "skippedEntries": skipped[:50],
        "generatedAtUtc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    (out / "_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Prepare Minecraft client assets from the player's own copy")
    parser.add_argument("--minecraft-dir", type=Path,
                        help="Java Edition game directory (or PLUSMC_MINECRAFT_DIR)")
    parser.add_argument("--assets-dir", type=Path,
                        help="the game's assets/ directory (contains indexes/ and objects/)")
    parser.add_argument("--client-jar", type=Path,
                        help="client jar (default: data/vanilla/_cache/client-*.jar)")
    parser.add_argument("--index", dest="index_id",
                        help="asset-index id (default: from the version metadata)")
    parser.add_argument("--version",
                        help="version id (default: the client jar's version.json)")
    parser.add_argument("--out", type=Path,
                        help="destination (default: data/vanilla/<version>/assets)")
    parser.add_argument("--strict", action="store_true",
                        help="fail when the asset index or any indexed object is missing")
    parser.add_argument("--json", action="store_true",
                        help="print the manifest as JSON")
    args = parser.parse_args(argv)

    minecraft_dir = (args.minecraft_dir.expanduser()
                     if args.minecraft_dir is not None else default_minecraft_dir())
    assets_dir = (args.assets_dir.expanduser()
                  if args.assets_dir is not None else
                  (minecraft_dir / "assets"
                   if minecraft_dir is not None and (minecraft_dir / "assets").is_dir()
                   else None))
    jar = (args.client_jar.expanduser()
           if args.client_jar is not None else find_client_jar(REPO, args.version))
    if jar is not None and not jar.is_file():
        parser.error(f"client jar not found: {jar}")
    if jar is None and assets_dir is None:
        parser.error("no client jar and no --minecraft-dir/--assets-dir")
    version_doc = _jar_version(jar)
    version = args.version or _infer_version(jar, version_doc)
    if version == "unknown" and args.out is None:
        parser.error("cannot infer the version; pass --version or --out")
    if any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
           for c in version):
        parser.error(f"version id has unsafe characters: {version!r}")
    index_id = args.index_id or discover_asset_index_id(
        version, version_doc, minecraft_dir, REPO)
    out = (args.out.expanduser()
           if args.out is not None else REPO / "data" / "vanilla" / version / "assets")

    result = extract_assets(
        client_jar=jar,
        assets_dir=assets_dir,
        index_id=index_id,
        out=out,
        repo=REPO,
        version=version,
        minecraft_dir=minecraft_dir,
    )

    if not result["indexAvailable"]:
        if result["assetIndex"]:
            print(f"warning: asset index {result['assetIndex']} was not found under "
                  f"{assets_dir / 'indexes' if assets_dir else '(no assets directory)'}; "
                  "store-only assets such as sounds are absent", file=sys.stderr)
        else:
            print("warning: no asset-index id; store-only assets such as sounds are absent",
                  file=sys.stderr)
    if result["missingCount"]:
        print(f"warning: {result['missingCount']} indexed objects are missing from the store "
              f"(first: {result['missingObjects'][0]})", file=sys.stderr)

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        if result["clientJar"]:
            print(f"client jar:   {result['clientJar']} ({result['fromJar']} assets)")
        if result["assetIndex"]:
            state = "available" if result["indexAvailable"] else "missing"
            print(f"asset index: {result['assetIndex']} ({state}; "
                  f"{result['fromIndex']} objects copied)")
        print(f"wrote:       {result['files']} files, "
              f"{result['bytes'] / 1_000_000:.1f} MB -> {out}")

    if args.strict and (result["missingCount"] or not result["indexAvailable"]):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
