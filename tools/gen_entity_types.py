#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Dumps every *living* entity type's wire id, dimensions, spawn category and
default attributes out of the vanilla server jar.

Mob dimensions and attributes are **code**, not data: `EntityTypes.java` builds
each type with `EntityType.Builder.of(X::new, MobCategory.Y).sized(w, h)` and
each entity class carries a `createAttributes()` method whose numbers only exist
compiled into the jar. There is no data-generator report for either, which is
why `src/world/mob_table.h` used to be hand-transcribed.

This tool removes the transcription: it compiles a few lines of Java against the
server jar, bootstraps the vanilla registries and asks the game itself. The
attributes come from `DefaultAttributes.getSupplier(type)` — the exact
`AttributeSupplier` a freshly created mob is given — so health, movement speed,
attack damage, armour, follow range and knockback are the game's own numbers,
not a reading of a decompiler's rendering of them.

Rows are every `EntityType` that `DefaultAttributes` has a supplier for, i.e.
every living entity. The player, armour stands and the 26.2 mannequin are living
but not mobs; `tools/gen_mob_table.py` drops them by name (the choice is made
there, where the rest of the row rules live, not hidden here).

Output columns (tab-separated, one header line):

    id              registry id == protocol id (EntityTypes registration order)
    name            "minecraft:zombie"
    category        MobCategory.getSerializedName() ("monster", "creature", ...)
    width, height   EntityType.getDimensions()
    fire_immune     EntityType.fireImmune()
    max_health      Attributes.MAX_HEALTH
    movement_speed  Attributes.MOVEMENT_SPEED
    has_attack_damage  whether the supplier carries ATTACK_DAMAGE at all (a
                       skeleton does not: its damage lives in its arrows)
    attack_damage   Attributes.ATTACK_DAMAGE (0 when absent)
    armor           Attributes.ARMOR
    follow_range    Attributes.FOLLOW_RANGE
    attack_knockback     Attributes.ATTACK_KNOCKBACK (0 when absent)
    knockback_resistance Attributes.KNOCKBACK_RESISTANCE (0 when absent)
    jump_strength        Attributes.JUMP_STRENGTH (0 when absent)
    safe_fall_distance   Attributes.SAFE_FALL_DISTANCE (0 when absent)

Usage:
    gen_entity_types.py <server.jar> <out.tsv> [--libraries DIR]...

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
import net.minecraft.core.Holder;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.ai.attributes.Attribute;
import net.minecraft.world.entity.ai.attributes.AttributeSupplier;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.entity.ai.attributes.DefaultAttributes;

public class DumpEntityTypes {
  static double value(AttributeSupplier supplier, Holder<Attribute> attribute) {
    return supplier.hasAttribute(attribute) ? supplier.getValue(attribute) : 0.0;
  }

  @SuppressWarnings("unchecked")
  public static void main(String[] args) throws Exception {
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
      out.println("id\\tname\\tcategory\\twidth\\theight\\tfire_immune\\tmax_health"
          + "\\tmovement_speed\\thas_attack_damage\\tattack_damage\\tarmor\\tfollow_range"
          + "\\tattack_knockback\\tknockback_resistance\\tjump_strength\\tsafe_fall_distance");
      for (EntityType<?> raw : BuiltInRegistries.ENTITY_TYPE) {
        if (!DefaultAttributes.hasSupplier(raw)) {
          continue;
        }
        EntityType<? extends LivingEntity> type = (EntityType<? extends LivingEntity>) raw;
        AttributeSupplier supplier = DefaultAttributes.getSupplier(type);
        int id = BuiltInRegistries.ENTITY_TYPE.getId(raw);
        Identifier key = BuiltInRegistries.ENTITY_TYPE.getKey(raw);
        out.println(id + "\\t" + key + "\\t" + type.getCategory().getSerializedName()
            + "\\t" + type.getDimensions().width() + "\\t" + type.getDimensions().height()
            + "\\t" + type.fireImmune()
            + "\\t" + value(supplier, Attributes.MAX_HEALTH)
            + "\\t" + value(supplier, Attributes.MOVEMENT_SPEED)
            + "\\t" + supplier.hasAttribute(Attributes.ATTACK_DAMAGE)
            + "\\t" + value(supplier, Attributes.ATTACK_DAMAGE)
            + "\\t" + value(supplier, Attributes.ARMOR)
            + "\\t" + value(supplier, Attributes.FOLLOW_RANGE)
            + "\\t" + value(supplier, Attributes.ATTACK_KNOCKBACK)
            + "\\t" + value(supplier, Attributes.KNOCKBACK_RESISTANCE)
            + "\\t" + value(supplier, Attributes.JUMP_STRENGTH)
            + "\\t" + value(supplier, Attributes.SAFE_FALL_DISTANCE));
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
    parser.add_argument("out", type=pathlib.Path, help="output TSV file")
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
        src = tmpdir / "DumpEntityTypes.java"
        src.write_text(_DUMPER)
        compiled = subprocess.run([javac, "-cp", cp, "-d", str(tmpdir), str(src)],
                                  capture_output=True, text=True)
        if compiled.returncode != 0:
            print("javac failed:\n" + compiled.stderr.strip()[:2000], file=sys.stderr)
            return 1

        dumped = tmpdir / "entity_types.tsv"
        ran = subprocess.run([java, "-cp", f"{tmpdir}:{cp}", "DumpEntityTypes", str(dumped)],
                             capture_output=True, text=True)
        if ran.returncode != 0 or not dumped.exists():
            print("running the dumper failed:\n" + (ran.stderr or ran.stdout).strip()[:2000],
                  file=sys.stderr)
            return 1
        text = dumped.read_text()

    rows = [line for line in text.splitlines() if line.strip()]
    if len(rows) < 2:
        print("dumper produced no entity rows", file=sys.stderr)
        return 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(rows) + "\n")
    print(f"wrote {args.out} ({len(rows) - 1} living entity types)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
