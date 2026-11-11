#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Dumps the set of blocks that require the correct tool to drop anything.

Vanilla does not encode "needs a pickaxe/shovel/..." in the loot tables: the
loot table for stone simply drops cobblestone, and the *code-side* flag
`BlockBehaviour.requiresCorrectToolForDrops()` decides whether the drop
happens at all. Nothing in the data files or the generated reports exposes
that flag, so this tool compiles a few lines of Java against the server jar,
bootstraps the vanilla registries and writes every block whose default state
reports the flag.

The output is a plain text list of `minecraft:<block>` names (one per line,
sorted), consumed by tools/gen_block_drops.py to gate drops on the correct
tool (the needs_{stone,iron,diamond}_tool tags alone only encode the *tier*,
not whether a tool is required).

Usage:
    gen_tool_requirements.py <server.jar> <out.txt> [--libraries DIR]...

The server jar is a bundler jar: the game classes live inside it, but the
third-party libraries (brigadier, datafixerupper, log4j, ...) must be on the
classpath. Point --libraries at the bundler's `libraries/` directory (the
vanilla-data tool leaves it under data/vanilla/_boot/libraries, or use the
directory the client extractor wrote). Every *.jar below it is added.

Requires a JDK (javac) on PATH — the same requirement as the vanilla data
generator the other tools use.
"""

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

# Writes to a file rather than stdout: the vanilla Bootstrap installs a log4j
# console appender that swallows System.out.
_DUMPER = """\
import java.io.FileWriter;
import java.io.PrintWriter;
import net.minecraft.SharedConstants;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.block.Block;

public class DumpRequiresTool {
  public static void main(String[] args) throws Exception {
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
      for (Block block : BuiltInRegistries.BLOCK) {
        if (block.defaultBlockState().requiresCorrectToolForDrops()) {
          Identifier id = BuiltInRegistries.BLOCK.getKey(block);
          out.println(id);
        }
      }
    }
  }
}
"""


def library_jars(dirs: list[pathlib.Path]) -> list[pathlib.Path]:
    jars: list[pathlib.Path] = []
    for d in dirs:
        if not d.is_dir():
            print(f"warning: --libraries {d} is not a directory", file=sys.stderr)
            continue
        jars.extend(sorted(d.rglob("*.jar")))
    return jars


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("jar", type=pathlib.Path, help="server jar (bundler)")
    parser.add_argument("out", type=pathlib.Path, help="output text file")
    parser.add_argument("--libraries", type=pathlib.Path, action="append", default=[],
                        help="directory of third-party library jars (repeatable)")
    args = parser.parse_args()

    if not args.jar.is_file():
        print(f"server jar not found: {args.jar}", file=sys.stderr)
        return 1
    javac = shutil.which("javac")
    java = shutil.which("java")
    if not javac or not java:
        print("javac/java not found on PATH (a JDK is required)", file=sys.stderr)
        return 1

    classpath = [str(args.jar)] + [str(j) for j in library_jars(args.libraries)]
    cp = ":".join(classpath)

    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = pathlib.Path(tmp)
        src = tmpdir / "DumpRequiresTool.java"
        src.write_text(_DUMPER)
        compiled = subprocess.run([javac, "-cp", cp, "-d", str(tmpdir), str(src)],
                                  capture_output=True, text=True)
        if compiled.returncode != 0:
            print("javac failed:\n" + compiled.stderr.strip()[:2000], file=sys.stderr)
            return 1

        dumped = tmpdir / "blocks.txt"
        ran = subprocess.run([java, "-cp", f"{tmpdir}:{cp}", "DumpRequiresTool", str(dumped)],
                             capture_output=True, text=True)
        if ran.returncode != 0 or not dumped.exists():
            print("running the dumper failed:\n" + (ran.stderr or ran.stdout).strip()[:2000],
                  file=sys.stderr)
            return 1
        names = sorted({line.strip() for line in dumped.read_text().splitlines() if line.strip()})

    if not names:
        print("dumper produced no blocks", file=sys.stderr)
        return 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(names) + "\n")
    print(f"wrote {args.out} ({len(names)} requires-correct-tool blocks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
