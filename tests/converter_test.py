#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The +MC Converter analyzer (Phase 7, slice 1).

The analyzer is a decompile-free scan: it reads a plugin JAR's manifest and
every class's constant pool, names what the plugin references, and scores how
much of that surface maps onto the native API. This test builds real class
files and JARs (no Java toolchain, no fixture binaries) and pins the report,
the score and every refusal:

  * a plugin whose references are only the mapped plugin/event/command/
    scheduler/config surface scores green;
  * a wider Bukkit API reference (an inventory) scores yellow and is listed
    under "needs mapping";
  * an NMS class or a reflective lookup scores red and names the reference;
  * a malformed class, a truncated class and a traversing entry are refused
    with the entry named, rather than crashing or half-reporting;
  * `--json` carries the same decision for a catalog to consume.

Run: converter_test.py /path/to/plusmc-convert
"""
from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

TIMEOUT = 60


def run(cli, args, expect=0):
    proc = subprocess.run(
        [str(cli), *[str(arg) for arg in args]],
        text=True,
        capture_output=True,
        timeout=TIMEOUT,
    )
    if expect is not None and proc.returncode != expect:
        raise AssertionError(
            f"plusmc-convert exited {proc.returncode}, expected {expect}: {proc.args}\n"
            f"stdout:\n{proc.stdout[-4000:]}\nstderr:\n{proc.stderr[-4000:]}"
        )
    return proc


class Pool:
    """A minimal constant pool: Utf8, Class, String, NameAndType and members."""

    def __init__(self):
        self.entries = []
        self.keys = {}

    def _add(self, key, tag, payload):
        if key in self.keys:
            return self.keys[key]
        index = len(self.entries) + 1
        self.entries.append((tag, payload))
        self.keys[key] = index
        return index

    def utf8(self, value):
        raw = value.encode("utf-8")
        assert len(raw) <= 0xFFFF
        return self._add(("utf8", value), 1, struct.pack(">H", len(raw)) + raw)

    def cls(self, name):
        return self._add(("class", name), 7, struct.pack(">H", self.utf8(name)))

    def string(self, value):
        return self._add(("string", value), 8, struct.pack(">H", self.utf8(value)))

    def name_and_type(self, name, descriptor):
        payload = struct.pack(">HH", self.utf8(name), self.utf8(descriptor))
        return self._add(("nat", name, descriptor), 12, payload)

    def method(self, owner, name, descriptor):
        payload = struct.pack(">HH", self.cls(owner), self.name_and_type(name, descriptor))
        return self._add(("method", owner, name, descriptor), 10, payload)

    def field(self, owner, name, descriptor):
        payload = struct.pack(">HH", self.cls(owner), self.name_and_type(name, descriptor))
        return self._add(("field", owner, name, descriptor), 9, payload)

    def encode(self):
        out = bytearray()
        for tag, payload in self.entries:
            out.append(tag)
            out += payload
        return bytes(out)


def class_bytes(
    name,
    *,
    major=61,
    super_name="java/lang/Object",
    interfaces=(),
    classes=(),
    methods=(),
    fields=(),
    strings=(),
):
    """A structurally valid class file whose constant pool holds the references.

    No methods or fields are emitted: the analyzer reads references from the
    constant pool, which is exactly the claim the slice makes. That keeps this
    writer small enough to read and means a test cannot pass by the analyzer
    secretly walking bytecode it does not parse.
    """
    pool = Pool()
    this_class = pool.cls(name)
    super_class = pool.cls(super_name)
    interface_indices = [pool.cls(interface) for interface in interfaces]
    for owner in classes:
        pool.cls(owner)
    for owner, member, descriptor in methods:
        pool.method(owner, member, descriptor)
    for owner, member, descriptor in fields:
        pool.field(owner, member, descriptor)
    for value in strings:
        pool.string(value)

    out = bytearray(struct.pack(">IHHH", 0xCAFEBABE, 0, major, len(pool.entries) + 1))
    out += pool.encode()
    out += struct.pack(">HHH", 0x0021, this_class, super_class)
    out += struct.pack(">H", len(interface_indices))
    for index in interface_indices:
        out += struct.pack(">H", index)
    out += struct.pack(">H", 0)  # fields
    out += struct.pack(">H", 0)  # methods
    out += struct.pack(">H", 0)  # class attributes
    return bytes(out)


def make_jar(path, classes, manifest=None, extra=None):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        if manifest is not None:
            archive.writestr("plugin.yml", manifest)
        for name, data in classes.items():
            archive.writestr(name, data)
        for name, data in (extra or {}).items():
            archive.writestr(name, data)
    return path


MANIFEST = """name: Example
version: 1.2.3
main: com.example.Plugin
api-version: '1.20'
author: Tester
authors: [One, Two]
depend: [Core]
softdepend: [Optional]
commands:
  greet:
    description: Say hi
    permission: example.greet
permissions:
  example.greet:
    default: true
"""


def green_classes():
    return {
        "com/example/Plugin.class": class_bytes(
            "com/example/Plugin",
            super_name="org/bukkit/plugin/java/JavaPlugin",
            interfaces=["org/bukkit/event/Listener"],
            methods=[
                ("org/bukkit/command/Command", "execute", "()Z"),
                ("org/bukkit/scheduler/BukkitRunnable", "run", "()V"),
                ("org/bukkit/configuration/ConfigurationSection", "getString", "()Ljava/lang/String;"),
            ],
        )
    }


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: converter_test.py <plusmc-convert>")
    cli = Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="plusmc-converter-test-") as temporary:
        root = Path(temporary)

        green = make_jar(root / "green.jar", green_classes(), MANIFEST)
        text = run(cli, ["analyze", green]).stdout
        assert "manifest: plugin.yml (name Example" in text, text
        assert "api-version 1.20" in text, text
        assert "main: com.example.Plugin" in text, text
        assert "score: green" in text, text
        assert "org/bukkit/inventory/" not in text, text
        print("PASS green plugin")

        yellow_classes = green_classes()
        yellow_classes["com/example/Menu.class"] = class_bytes(
            "com/example/Menu", classes=["org/bukkit/inventory/Inventory"]
        )
        yellow = make_jar(root / "yellow.jar", yellow_classes, MANIFEST)
        text = run(cli, ["analyze", yellow]).stdout
        assert "score: yellow" in text, text
        assert "needs mapping: 1" in text, text
        assert "org/bukkit/inventory/Inventory" in text, text
        print("PASS wider API needs mapping")

        nms_classes = green_classes()
        nms_classes["com/example/Deep.class"] = class_bytes(
            "com/example/Deep", classes=["net/minecraft/server/level/ServerLevel"]
        )
        nms = make_jar(root / "nms.jar", nms_classes, MANIFEST)
        text = run(cli, ["analyze", nms]).stdout
        assert "score: red" in text, text
        assert "server internals: 1" in text, text
        assert "net/minecraft/server/level/ServerLevel" in text, text
        assert "NMS/CraftBukkit" in text, text
        print("PASS NMS refused to green")

        reflection_classes = green_classes()
        reflection_classes["com/example/Hidden.class"] = class_bytes(
            "com/example/Hidden",
            methods=[("java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;")],
        )
        reflection = make_jar(root / "reflection.jar", reflection_classes, MANIFEST)
        text = run(cli, ["analyze", reflection]).stdout
        assert "score: red" in text, text
        assert "reflection members: 1" in text, text
        assert "java/lang/Class#forName" in text, text
        print("PASS reflection refused to green")

        report = json.loads(run(cli, ["analyze", green, "--json"]).stdout)
        assert report["ok"] is True, report
        assert report["score"] == "green", report
        assert report["manifest"]["api_version"] == "1.20", report
        assert report["manifest"]["depend"] == ["Core"], report
        assert report["classes"][0]["name"] == "com/example/Plugin", report
        assert "org/bukkit/plugin/java/JavaPlugin" in report["usage"]["supported"], report
        print("PASS JSON report")

        # A manifest the reader cannot parse is refused with the line, not
        # guessed at: a catalog must never publish a score for a broken jar.
        broken_manifest = make_jar(root / "broken-manifest.jar", green_classes(), "name: [\nnot a pair\n")
        proc = run(cli, ["analyze", broken_manifest], expect=1)
        assert "plugin.yml: line 2" in proc.stderr, proc.stderr
        print("PASS malformed manifest")

        broken_class = make_jar(
            root / "broken-class.jar", {"com/example/Broken.class": b"\xca\xfe\xba\xbe\x00\x00\x00\x3d"}, MANIFEST
        )
        proc = run(cli, ["analyze", broken_class], expect=1)
        assert "com/example/Broken.class:" in proc.stderr, proc.stderr
        assert "shorter than its header" in proc.stderr, proc.stderr

        truncated = make_jar(
            root / "truncated.jar",
            {"com/example/Truncated.class": class_bytes("com/example/Truncated")[:20]},
            MANIFEST,
        )
        proc = run(cli, ["analyze", truncated], expect=1)
        assert "com/example/Truncated.class:" in proc.stderr, proc.stderr
        assert "runs past the end of the class file" in proc.stderr, proc.stderr
        print("PASS malformed class")

        wrong_magic = make_jar(
            root / "wrong-magic.jar", {"com/example/Plugin.class": b"not a class file"}, MANIFEST
        )
        proc = run(cli, ["analyze", wrong_magic], expect=1)
        assert "CAFEBABE" in proc.stderr, proc.stderr
        print("PASS wrong magic")

        traversal = make_jar(root / "traversal.jar", {}, MANIFEST, extra={"../escape.class": b"x"})
        proc = run(cli, ["analyze", traversal], expect=1)
        assert "path traversal" in proc.stderr, proc.stderr
        print("PASS traversing entry")

        empty = make_jar(root / "empty.jar", {}, MANIFEST)
        proc = run(cli, ["analyze", empty], expect=1)
        assert "no class files" in proc.stderr, proc.stderr
        print("PASS no classes")

        # The analyzer reports a missing plugin.yml as a finding, not a crash;
        # an archive with classes can still be inspected before it is scored.
        plain = make_jar(root / "plain.jar", green_classes(), None)
        text = run(cli, ["analyze", plain]).stdout
        assert "manifest: none" in text, text
        assert "no plugin.yml or paper-plugin.yml" in text, text
        assert "score: red" in text, text
        print("PASS no manifest is a finding")

        # A multi-release entry is not decoded as a second copy of the class.
        multi = make_jar(
            root / "multi.jar",
            green_classes(),
            MANIFEST,
            extra={"META-INF/versions/17/com/example/Plugin.class": b"ignored"},
        )
        text = run(cli, ["analyze", multi]).stdout
        assert "classes: 1" in text, text
        assert "multi-release entry skipped" in text, text
        print("PASS multi-release entries")

    print("converter-analyze: all checks passed")


if __name__ == "__main__":
    main()
