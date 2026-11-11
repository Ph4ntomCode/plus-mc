#!/usr/bin/env python3
"""End-to-end smoke test for plusmc-vanilla-data.

Builds two tiny fixture jars (a flat jar and a modern bundler jar with a nested
server jar), runs the tool against each, and asserts the extracted output.

Usage: vanilla_data_smoke.py /path/to/plusmc-vanilla-data
"""
import json
import pathlib
import subprocess
import sys
import tempfile
import zipfile

TOOL = pathlib.Path(sys.argv[1]).resolve()

VERSION_JSON = (
    '{\n  "id": "9.9",\n  "name": "9.9",\n  "world_version": 42,\n'
    '  "protocol_version": 999,\n  "stable": true\n}\n'
).encode()
RECIPE = b'{"type": "minecraft:crafting_shapeless", "group": "smoke-test"}\n'


def make_inner_bytes() -> bytes:
    buf = tempfile.SpooledTemporaryFile()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("version.json", VERSION_JSON)
        z.writestr("pack.mcmeta", b'{"pack": {"description": "t", "pack_format": 107}}\n')
        z.writestr("data/minecraft/recipe/smoke_item.json", RECIPE)
    buf.seek(0)
    return buf.read()


def run_tool(jar: pathlib.Path, out: pathlib.Path):
    proc = subprocess.run(
        [str(TOOL), "--jar", str(jar), "--out", str(out)],
        capture_output=True,
        text=True,
        timeout=120,
    )
    if proc.returncode != 0:
        raise SystemExit(f"tool failed ({proc.returncode}):\n{proc.stdout}\n{proc.stderr}")


def assert_file(out: pathlib.Path, rel: str, expected: bytes):
    p = out / rel
    if not p.is_file():
        raise AssertionError(f"missing expected file: {p}")
    if p.read_bytes() != expected:
        raise AssertionError(f"content mismatch: {p}")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="plusmc-smoke-") as td:
        root = pathlib.Path(td)

        # 1) Flat (non-bundler) jar.
        flat_jar = root / "flat.jar"
        with zipfile.ZipFile(flat_jar, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("version.json", VERSION_JSON)
            z.writestr("data/minecraft/recipe/smoke_item.json", RECIPE)
        out_flat = root / "out-flat"
        run_tool(flat_jar, out_flat)
        idx = json.loads((out_flat / "9.9" / "index.json").read_text())
        assert idx["protocol"] == 999 and idx["dataVersion"] == 42, idx
        assert idx["extractedFiles"] == 2, idx
        assert_file(out_flat / "9.9", "version.json", VERSION_JSON)
        assert_file(out_flat / "9.9", "data/minecraft/recipe/smoke_item.json", RECIPE)
        print("flat jar: OK")

        # 2) Bundler jar (26.x style): real server nested under META-INF/versions/.
        inner = make_inner_bytes()
        bundler = root / "server-bundler.jar"
        with zipfile.ZipFile(bundler, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("META-INF/MANIFEST.MF", b"Manifest-Version: 1.0\n")
            z.writestr("META-INF/versions/9.9/server-9.9.jar", inner)
            z.writestr("net/minecraft/bundler/Main.class", b"\xca\xfe\xba\xbe")
        out_bundle = root / "out-bundle"
        run_tool(bundler, out_bundle)
        idx = json.loads((out_bundle / "9.9" / "index.json").read_text())
        assert idx["protocol"] == 999 and idx["dataVersion"] == 42, idx
        # version.json + pack.mcmeta + 1 recipe, extracted from the nested jar.
        assert idx["extractedFiles"] == 3, idx
        assert_file(out_bundle / "9.9", "version.json", VERSION_JSON)
        assert_file(out_bundle / "9.9", "data/minecraft/recipe/smoke_item.json", RECIPE)
        print("bundler jar: OK")

    print("PASS")


if __name__ == "__main__":
    main()
