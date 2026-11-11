#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates src/world/signal_source_table.h from the decompiled 26.2 sources:
# the blocks whose `BlockState.isSignalSource()` is true, which is one clause of
# `NaturalSpawner.isValidEmptySpawnBlock` (`world/mob_spawn.cpp`) — no mob may
# spawn inside a redstone component.
#
# Unlike `isValidSpawn` (a `Blocks.java` property predicate), `isSignalSource` is
# a *method override*: `BlockBehaviour.isSignalSource(BlockState)` answers false
# and twelve classes override it. So the resolution here is two steps:
#
#   1. every class in the decomp tree that declares `isSignalSource(BlockState
#      state)` is recorded;
#   2. every `Blocks.java` registration whose block class — or any class up its
#      `extends` chain — is in that set is emitted.
#
# Step 2 is why the `extends` walk matters: `RepeaterBlock extends DiodeBlock`
# and `RedstoneWallTorchBlock extends RedstoneTorchBlock` carry the override
# without declaring it, and `CalibratedSculkSensorBlock extends SculkSensorBlock`
# likewise.
#
# One registration line can stand for a family (`WeatheringCopperCollection
# .registerBlocks` on LIGHTNING_ROD builds all eight weathering stages), so those
# are expanded from the project's own registry by suffix, exactly as
# tools/gen_spawn_rule_table.py expands its collections.
#
# Regenerate with: python3 tools/gen_signal_source_table.py

import argparse
import pathlib
import re
import sys

# A one-line registration that stands for a family of blocks, as in
# tools/gen_spawn_rule_table.py: the constant's suffix is looked up in the
# project registry, with the count that suffix must resolve to.
COLLECTIONS = {
    "LIGHTNING_ROD": ("lightning_rod", 8),
}

# The method *and* its body: `BlockBehaviour` declares the default as
# `protected boolean isSignalSource(BlockState state) { return false; }`, so a
# declaration whose body is exactly `return false;` is the baseline and not an
# override. Matching the body is what keeps every block in the game from
# inheriting `BlockBehaviour` as a signal source.
OVERRIDE = re.compile(r"\bisSignalSource\s*\(\s*BlockState\b[^)]*\)\s*\{\s*([^}]*)\}")
# The declaration keyword has to start a word *and* follow whitespace, so a
# `Foo.class` literal is not mistaken for a class declaration. The optional
# `extends` clause is what the whitespace collapsing in class_graph() exists
# for: the decomp wraps a long declaration over two lines.
CLASS_DECL = re.compile(r"(?:^|\s)(?:public |protected |private |final |abstract |static |sealed )*"
                        r"(?:class|interface|enum)\s+([A-Za-z0-9_]+)(?:\s*<[^>{;]*>)?"
                        r"(?:\s+extends\s+([A-Za-z0-9_.]+))?")
REGISTRATION = re.compile(r"^\s*public static final [A-Za-z0-9_<>,\[\] ]+ ([A-Z_0-9]+) = "
                          r"(?:Blocks|WeatheringCopperCollection)\.register\w*\((.*)\);\s*$")
ID_REF = re.compile(r"BlockItemIds\.([A-Z_0-9]+)|BlockIds\.([A-Z_0-9]+)")
CLASS_REF = re.compile(r"new ([A-Z][A-Za-z0-9_]*)\(|([A-Z][A-Za-z0-9_]*)::new")


def class_graph(decomp: pathlib.Path):
    """(class -> parent) and the set of classes that override isSignalSource."""
    parents = {}
    overrides = set()
    for path in decomp.rglob("*.java"):
        # Whitespace is collapsed first so a declaration the decomp wrapped over
        # two lines (`public class RedStoneWireBlock\n extends Block {`) reads as
        # one, and brace-free regexing stays enough to find it.
        text = re.sub(r"\s+", " ", path.read_text(encoding="utf-8", errors="replace"))
        declarations = [(m.start(), m.group(1), m.group(2)) for m in CLASS_DECL.finditer(text)]
        for _, name, parent in declarations:
            if parent:
                parents.setdefault(name, parent.split("<")[0].split(".")[-1])
        override = None
        for match in OVERRIDE.finditer(text):
            if match.group(1).strip() != "return false;":
                override = match
                break
        if override is None:
            continue
        # The innermost class declared before the override encloses it, which is
        # the class the method belongs to (a decomp file is one top-level class,
        # with any nested ones declared before the members that use them).
        owner = None
        for start, name, _ in declarations:
            if start > override.start():
                break
            owner = name
        if owner:
            overrides.add(owner)
    return parents, overrides


def is_signal_source(cls, parents, overrides):
    seen = set()
    while cls and cls not in seen:
        if cls in overrides:
            return True
        seen.add(cls)
        cls = parents.get(cls)
    return False


def reference_names(path: pathlib.Path):
    """(constant -> block name) for a `BlockIds`/`BlockItemIds` reference class.

    Both spell the string the block is registered under: `BlockIds.X = create(
    "name")`, and `BlockItemIds.X = BlockItemId.create("block", "item")` whose
    *first* string is the block (`REDSTONE_DUST` is `create("redstone_wire",
    "redstone")`) — and whose one-argument form repeats the name. So the item id
    is never what this script names a block by.
    """
    text = path.read_text(encoding="utf-8")
    out = {}
    # `create("x")`, `create("block", "item")` and the collection factories
    # (`createSimpleCopper("copper_bars")`) all name the block in their first
    # string argument.
    for match in re.finditer(r"^\s*(?:public\s+)?static final \w+(?:<[^>]*>)? ([A-Z_0-9]+) = "
                             r"\w+\.create\w*\(\s*\"([a-z0-9_/]+)\"", text, re.M):
        out[match.group(1)] = "minecraft:" + match.group(2)
    return out


def build(blocks_java: pathlib.Path, parents, overrides, registry: set, ids):
    """(name -> True) for every registered block that is a signal source."""
    names = {}
    unresolved = 0
    for line in blocks_java.read_text(encoding="utf-8").splitlines():
        match = REGISTRATION.match(line)
        if match is None:
            continue
        cls = CLASS_REF.search(match.group(2))
        ref = ID_REF.search(line)
        if cls is None or ref is None or not (ref.group(1) or ref.group(2)):
            unresolved += 1
            continue
        # The id the call passes, resolved through the reference class — *not*
        # the Java constant, whose name can differ from the block's
        # (`POTTED_AZALEA` is registered as `BlockIds.POTTED_AZALEA_BUSH`).
        constant = ref.group(1) or ref.group(2)
        if constant not in ids:
            print(f"UNKNOWN ID REFERENCE: {constant}", file=sys.stderr)
            sys.exit(1)
        # A lambda that constructs more than one class (`new WeatheringLightning
        # RodBlock(...)`) is resolved on the *first* one, which is the plain
        # variant; the weathering twins are added by COLLECTIONS below.
        classes = re.findall(r"new ([A-Z][A-Za-z0-9_]*)\(", match.group(2))
        if not classes:
            classes = [cls.group(1) or cls.group(2)]
        if constant in COLLECTIONS:
            suffix, expected = COLLECTIONS[constant]
            expanded = sorted(n for n in registry if n.endswith(suffix))
            if len(expanded) != expected:
                print(f"{constant} expansion found {len(expanded)} blocks, expected {expected}",
                      file=sys.stderr)
                sys.exit(1)
            family = expanded
        else:
            family = [ids[constant]]
        if not any(is_signal_source(c, parents, overrides) for c in classes):
            continue
        for name in family:
            if name not in registry:
                print(f"NOT IN THE PROJECT REGISTRY: {name}", file=sys.stderr)
                sys.exit(1)
            names[name] = True
    return names, unresolved


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--decomp", type=pathlib.Path,
                        default=root / "build/_decomp/net/minecraft")
    parser.add_argument("--blocks-java", type=pathlib.Path,
                        default=root / "build/_decomp/net/minecraft/world/level/block/Blocks.java")
    parser.add_argument("--registry", type=pathlib.Path,
                        default=root / "src/world/block_states.h")
    parser.add_argument("--references", type=pathlib.Path,
                        default=root / "build/_decomp/net/minecraft/references")
    parser.add_argument("--tsv", type=pathlib.Path,
                        default=root / "data/vanilla/26.2/signal_source_blocks.tsv")
    parser.add_argument("--header", type=pathlib.Path,
                        default=root / "src/world/signal_source_table.h")
    args = parser.parse_args()

    text = args.registry.read_text(encoding="utf-8")
    registry = set(re.findall(r'\{"(minecraft:[a-z0-9_/]+)",', text))
    parents, overrides = class_graph(args.decomp)
    if not overrides:
        print(f"no isSignalSource overrides found under {args.decomp}", file=sys.stderr)
        sys.exit(1)
    ids = {}
    for reference_file in ("BlockItemIds.java", "BlockIds.java"):
        ids.update(reference_names(args.references / reference_file))
    if not ids:
        print(f"no id references found under {args.references}", file=sys.stderr)
        sys.exit(1)
    names, unresolved = build(args.blocks_java, parents, overrides, registry, ids)
    if not names:
        print("no signal sources resolved: is Blocks.java present?", file=sys.stderr)
        sys.exit(1)

    lines = ["block\tsignal_source"]
    lines += [f"{name}\ttrue" for name in sorted(names)]
    args.tsv.write_text("\n".join(lines) + "\n")

    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// clang-format off")
    out.append("//")
    out.append(f"// The blocks whose `BlockState.isSignalSource()` is true ({len(names)} blocks,")
    out.append("// generated — do not hand-edit), read out of the vanilla 26.2 decompiled")
    out.append("// sources by tools/gen_signal_source_table.py. `isSignalSource` is a method")
    out.append("// override rather than a property, so the generator resolves each block's")
    out.append("// class up its `extends` chain against the classes that declare")
    out.append("// `isSignalSource(BlockState)` — which is what carries the answer for")
    out.append("// `RepeaterBlock extends DiodeBlock` and")
    out.append("// `RedstoneWallTorchBlock extends RedstoneTorchBlock`.")
    out.append("//")
    out.append("// This is one clause of `NaturalSpawner.isValidEmptySpawnBlock`: a mob cannot")
    out.append("// spawn inside a redstone component, so a lever or a button in the spawn box")
    out.append("// refuses the spawn (world/mob_spawn.cpp).")
    out.append("//")
    out.append("// Regenerate with tools/gen_signal_source_table.py.")
    out.append("#pragma once")
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("// Sorted by name, so block_is_signal_source can binary-search it.")
    out.append("inline constexpr const char* kSignalSourceBlocks[] = {")
    for name in sorted(names):
        out.append(f'    "{name}",')
    out.append("};")
    out.append(f"inline constexpr int kSignalSourceBlockCount = {len(names)};")
    out.append("")
    out.append("} // namespace mc::world")
    out.append("")
    args.header.write_text("\n".join(out) + "\n")
    print(f"{len(overrides)} classes override isSignalSource; {len(names)} blocks carry it "
          f"({unresolved} registration lines without a class were skipped)")
    print(f"wrote {args.tsv} and {args.header}")


if __name__ == "__main__":
    main()
