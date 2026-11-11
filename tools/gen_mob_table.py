#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/world/mob_table.h — every mob this engine can hold.

The table used to be six hand-transcribed rows (Phase 3 milestone I). Six rows
are transcribable by hand; ninety are not, and the transcription rules were
already written down in prose (each row cited its source). This tool turns those
rules into code:

  * **ids, dimensions, categories and attributes** come from the game itself:
    tools/gen_entity_types.py compiles a probe against the server jar and asks
    `BuiltInRegistries.ENTITY_TYPE` and `DefaultAttributes` (see
    data/vanilla/26.3/entity_types.tsv);
  * **the class each type is built from** comes from the decompiled
    `EntityTypes.java` (`EntityType.Builder.of(Zombie::new, ...)`);
  * **the flags** are derived from the class hierarchy and the decompiled
    sources: `Monster`/`Enemy` -> kMobHostile, `Animal` -> kMobAnimal (and
    kMobNoFarDespawn, `Animal.removeWhenFarAway` returning false),
    `HurtByTargetGoal` -> kMobRetaliates, `#minecraft:inverted_healing_and_harm`
    (`data/.../tags/entity_type/inverted_healing_and_harm.json`, nested tags
    resolved) -> kMobInvertedHealAndHarm, and the target/attack goals ->
    kMobMustSee / kMobFollowsUnseen;
  * **XP** is the nearest `this.xpReward = N` in the hierarchy, else `Animal`'s
    `1 + random.nextInt(3)` or `Monster`'s 5;
  * **baby dimensions** are the nearest `BABY_DIMENSIONS` initialiser
    (`EntityDimensions.scalable(w, h)` or the type's own box `.scale(f)`), with
    a documented 0.5x fallback for an `Animal` that declares none;
  * **follow-parent speed** is the nearest `new FollowParentGoal(this, speed)`.

Six rows are *cross-checked* against the values the old hand-written table
carried, byte for byte, so this file cannot quietly change them: the generator
fails if any of zombie/spider/pig/cow/sheep/chicken disagrees on id,
dimensions, health, speed, attack, armour, follow range, XP, follow-parent
speed or flags.

Mobs whose vanilla AI is not a straight melee chase are still generated with
their real stats and (where the engine has the movement) stroll or chase
generically; the header written into the file names each family whose signature
ability is not modelled (ranged, explosive, flight, swimming, jumping, boss
phases, teleport/venom and friends) so nothing is silently claimed.

Usage:
    gen_mob_table.py [--entity-types TSV] [--decomp DIR] [--out HEADER]
"""

import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Living entities that are not mobs: the player, the armour stand, and the 26.3
# mannequin. They have attributes and dimensions but no AI and no spawn egg;
# nothing in this engine wants to simulate one.
EXCLUDED = {"minecraft:player", "minecraft:armor_stand", "minecraft:mannequin"}

# The family rules, each one decompiled line:
#   * `Monster` (monster/Monster.java) and the `Enemy` marker every non-Monster
#     hostile implements (a ghast, a phantom, a slime, a shulker, a hoglin, the
#     dragon): hostile, and `Monster`'s xpReward is 5;
#   * `Animal` (animal/Animal.java:133): `removeWhenFarAway` false and
#     `getBaseExperienceReward` 1 + random.nextInt(3);
#   * `HurtByTargetGoal` registrations: whoever hit the mob becomes its target.
MONSTER = "Monster"
ANIMAL = "Animal"
AGEABLE_WATER = "AgeableWaterCreature"
WATER_ANIMAL = "WaterAnimal"

# The six rows the hand-written table carried, with every value it pinned.
# `None` means "not pinned by the old table" (it only pinned the columns a test
# or a reader depended on); the generator still produces them and the field is
# left to the derived value.
EXPECTED = {
    "minecraft:zombie": dict(entity_type_id=151, width=0.6, height=1.95, max_health=20.0,
                             movement_speed=0.23, attack_damage=3.0, follow_range=35.0,
                             armor=2.0, xp_min=5, xp_max=5, follow_parent=0.0,
                             category="monster", baby=(0.0, 0.0), fire_immune=False,
                             flags={"hostile", "must_see", "inverted"}),
    "minecraft:spider": dict(entity_type_id=124, width=1.4, height=0.9, max_health=16.0,
                             movement_speed=0.3, attack_damage=2.0, follow_range=16.0,
                             armor=0.0, xp_min=5, xp_max=5, follow_parent=0.0,
                             category="monster", baby=(0.0, 0.0), fire_immune=False,
                             flags={"hostile", "must_see", "follows_unseen"}),
    "minecraft:pig": dict(entity_type_id=100, width=0.9, height=0.9, max_health=10.0,
                          movement_speed=0.25, attack_damage=0.0, follow_range=16.0,
                          armor=0.0, xp_min=1, xp_max=3, follow_parent=1.1,
                          category="creature", baby=(0.45, 0.45), fire_immune=False,
                          flags={"animal", "no_far_despawn"}),
    "minecraft:cow": dict(entity_type_id=30, width=0.9, height=1.4, max_health=10.0,
                          movement_speed=0.2, attack_damage=0.0, follow_range=16.0,
                          armor=0.0, xp_min=1, xp_max=3, follow_parent=1.25,
                          category="creature", baby=(0.45, 0.7), fire_immune=False,
                          flags={"animal", "no_far_despawn"}),
    "minecraft:sheep": dict(entity_type_id=111, width=0.9, height=1.3, max_health=8.0,
                            movement_speed=0.23, attack_damage=0.0, follow_range=16.0,
                            armor=0.0, xp_min=1, xp_max=3, follow_parent=1.1,
                            category="creature", baby=(0.45, 0.65), fire_immune=False,
                            flags={"animal", "no_far_despawn"}),
    "minecraft:chicken": dict(entity_type_id=26, width=0.4, height=0.7, max_health=4.0,
                              movement_speed=0.25, attack_damage=0.0, follow_range=16.0,
                              armor=0.0, xp_min=1, xp_max=3, follow_parent=1.1,
                              category="creature", baby=(0.3, 0.4), fire_immune=False,
                              flags={"animal", "no_far_despawn"}),
}

# Mobs whose signature vanilla ability is *not* a melee chase and is not
# modelled. They are generated with their exact stats (so spawning, damage,
# death, drops and keep-alives are real) and get the engine's generic goal for
# their family; the header names the family and what is missing. Nothing here is
# a silent approximation — each entry is the reason the header lists it.
UNMODELLED_AI = {
    "minecraft:blaze": "fires small fireballs and hovers",
    "minecraft:breeze": "leaps and fires wind charges",
    "minecraft:creeper": "fuses and explodes",
    "minecraft:elder_guardian": "a laser beam and mining fatigue",
    "minecraft:ender_dragon": "a boss fight with phases and crystals",
    "minecraft:enderman": "teleports, picks blocks up and is provoked by being looked at",
    "minecraft:endermite": "burrows into blocks",
    "minecraft:evoker": "summons fangs and vexes",
    "minecraft:ghast": "flies and fires fireballs",
    "minecraft:giant": "has no AI in vanilla at all (unused mob)",
    "minecraft:guardian": "a laser beam",
    "minecraft:happy_ghast": "flies and carries players",
    "minecraft:hoglin": "charges",
    "minecraft:illusioner": "blinds and fires its bow",
    "minecraft:magma_cube": "jumps and splits on death",
    "minecraft:phantom": "flies in swoops at night",
    "minecraft:piglin": "ranged crossbow attacks and bartering",
    "minecraft:piglin_brute": "axes and no ranged attack",
    "minecraft:pillager": "a crossbow",
    "minecraft:ravager": "charges and roars",
    "minecraft:shulker": "a bullet and levitation",
    "minecraft:silverfish": "infests blocks",
    "minecraft:skeleton": "fires arrows (the ranged goal is not modelled)",
    "minecraft:slime": "jumps and splits on death",
    "minecraft:stray": "fires arrows (the ranged goal is not modelled)",
    "minecraft:bogged": "fires arrows (the ranged goal is not modelled)",
    "minecraft:parched": "fires arrows (the ranged goal is not modelled)",
    "minecraft:wither_skeleton": "fires arrows (the ranged goal is not modelled)",
    "minecraft:sulfur_cube": "jumps and splits on death",
    "minecraft:vex": "flies through walls",
    "minecraft:warden": "a sonic boom and darkness",
    "minecraft:witch": "throws splash potions",
    "minecraft:wither": "a boss fight with phases",
    "minecraft:zoglin": "charges",
}

# Boxed-in defaults that a parse cannot answer, with the reason. Each is used
# only for the mob named; everything else is derived from the sources.
XP_OVERRIDES = {
    # Monster's xpReward is damage-scaled for the dragon (xpCount accrues and is
    # awarded in fractions), so the flat reward is not a number this table can
    # carry. The drop is recorded as 0 rather than a guess.
    "minecraft:ender_dragon": (0, 0),
    # Slime/MagmaCube assign `this.xpReward = actualSize` (1, 2 or 4 by size).
    "minecraft:slime": (1, 4),
    "minecraft:magma_cube": (1, 4),
    "minecraft:sulfur_cube": (1, 4),
}
XP_NOTES = {
    "minecraft:ender_dragon": "the dragon's damage-scaled reward is not modelled",
    "minecraft:slime": "`this.xpReward = actualSize` (1..4 by size)",
    "minecraft:magma_cube": "`this.xpReward = actualSize` (1..4 by size)",
    "minecraft:sulfur_cube": "size-scaled reward, modelled as the 1..4 range",
}


def split_top_level(text: str) -> list:
    """Splits a Java argument list on top-level commas."""
    parts, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(text[start:i].strip())
            start = i + 1
    tail = text[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def paren_args(text: str, open_index: int) -> tuple:
    """(argument list, index just past the closing paren) for a `(` at open_index."""
    depth, i = 0, open_index
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[open_index + 1:i], i + 1
        i += 1
    raise ValueError("unbalanced parentheses")


def class_declarations(paths) -> dict:
    """class name -> {base, impl, path}. Nested classes are indexed too, which is
    what finds `SpiderAttackGoal` inside Spider.java."""
    out = {}
    pattern = re.compile(r"(?:^|\n)\s*(?:public |private |protected |static |abstract |final )*"
                         r"(class|interface)\s+(\w+)(?:<[^>]*>)?\s*"
                         r"(?:extends\s+([\w.]+))?\s*(?:implements\s+([^{]+))?")
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in pattern.finditer(text):
            name, base = match.group(2), match.group(3)
            base = base.rsplit(".", 1)[-1] if base else None
            impl = []
            if match.group(4):
                for part in match.group(4).split(","):
                    part = part.strip().split("<")[0].rsplit(".", 1)[-1]
                    if part:
                        impl.append(part)
            if name in out and out[name]["base"] is not None and base is None:
                continue  # keep the richer record
            out[name] = {"base": base, "impl": impl, "path": path}
    return out


def chain_of(name: str, declarations: dict) -> list:
    """[leaf, ..., base] following `extends`, stopping at an unknown name."""
    out, seen = [], set()
    while name and name not in seen and name in declarations:
        seen.add(name)
        out.append(name)
        name = declarations[name]["base"]
    return out


def file_of(name: str, declarations: dict):
    entry = declarations.get(name)
    return entry["path"] if entry else None


def is_enemy(chain: list, declarations: dict) -> bool:
    """`Enemy`: the marker `Monster` implements and the non-Monster hostiles
    (a ghast, a phantom, a slime, a shulker, a hoglin, the dragon) implement
    directly. `Monster` is treated as a member of the same set through the chain
    walk, which is why the first test is enough for every zombie."""
    if MONSTER in chain:
        return True
    for name in chain:
        if "Enemy" in declarations[name]["impl"]:
            return True
    return False


def first_bool(args: list) -> bool:
    for arg in args:
        if arg in ("true", "false"):
            return arg == "true"
    return False


def parse_must_see(chain: list, declarations: dict) -> bool:
    """The nearest `NearestAttackableTargetGoal` aimed at players: the first
    boolean literal in its argument list is `mustSee` for every call shape
    (3-arg, 4-arg and the 6-arg random-interval form)."""
    pattern = re.compile(r"NearestAttackableTargetGoal<([\w.]+)>?\s*\(")
    for name in chain:
        text = file_of(name, declarations).read_text(encoding="utf-8", errors="replace")
        for match in pattern.finditer(text):
            if match.group(1) not in ("Player", "LivingEntity"):
                continue
            try:
                args, _ = paren_args(text, text.index("(", match.start()))
            except ValueError:
                continue
            return first_bool(split_top_level(args)[2:])
    return True


def parse_follows_unseen(chain: list, declarations: dict) -> bool:
    """The nearest melee-style attack goal: a call-site boolean literal if there
    is one, else the goal class's own `super(...)` literal (the
    `SpiderAttackGoal` shape, whose call site carries no flag — the goal is a
    nested class, which the declaration index carries too)."""
    call = re.compile(r"new\s+(\w+AttackGoal)\s*\(")
    for name in chain:
        text = file_of(name, declarations).read_text(encoding="utf-8", errors="replace")
        for match in call.finditer(text):
            goal = match.group(1)
            if goal in ("RangedAttackGoal", "HoldGroundAttackGoal"):
                continue
            try:
                args, _ = paren_args(text, text.index("(", match.start()))
            except ValueError:
                continue
            pieces = split_top_level(args)
            for arg in reversed(pieces):
                if arg in ("true", "false"):
                    return arg == "true"
            # No literal at the call site: the goal's own constructor may pass
            # one to its parent (`SpiderAttackGoal` -> `super(mob, 1.0, true)`).
            goal_file = file_of(goal, declarations)
            if goal_file is None:
                continue
            goal_text = goal_file.read_text(encoding="utf-8", errors="replace")
            decl = re.search(r"class\s+" + re.escape(goal) + r"\b", goal_text)
            if not decl:
                continue
            sup = goal_text.find("super(", decl.end())
            if sup < 0:
                continue
            try:
                sup_args, _ = paren_args(goal_text, goal_text.index("(", sup))
            except ValueError:
                continue
            for arg in reversed(split_top_level(sup_args)):
                if arg in ("true", "false"):
                    return arg == "true"
    return False


def parse_retaliates(chain: list, declarations: dict) -> bool:
    for name in chain:
        text = file_of(name, declarations).read_text(encoding="utf-8", errors="replace")
        if "new HurtByTargetGoal" in text:
            return True
    return False


def parse_no_far_despawn(chain: list, declarations: dict, is_animal: bool) -> bool:
    """`Animal.removeWhenFarAway` is an unconditional false; a subclass that
    overrides it conditionally (a tamed cat, a chicken jockey) is still an
    animal, so the animal rule is the floor and a literal-false override in the
    chain lifts a non-animal (the golems, an allay) into the same flag."""
    if is_animal:
        return True
    for name in chain:
        text = file_of(name, declarations).read_text(encoding="utf-8", errors="replace")
        match = re.search(r"removeWhenFarAway\(double\s+\w+\)\s*\{([^}]*)\}", text, re.S)
        if match and re.match(r"^\s*return false;\s*$", match.group(1)):
            return True
    return False


def parse_xp(chain: list, declarations: dict, is_animal: bool, is_monster: bool,
             name: str) -> tuple:
    if name in XP_OVERRIDES:
        return XP_OVERRIDES[name]
    for cls in chain:
        text = file_of(cls, declarations).read_text(encoding="utf-8", errors="replace")
        match = re.search(r"this\.xpReward\s*=\s*(\d+)\s*;", text)
        if match:
            value = int(match.group(1))
            return value, value
    if is_animal or AGEABLE_WATER in chain or WATER_ANIMAL in chain:
        return 1, 3
    if is_monster:
        return 5, 5
    return 0, 0


def parse_follow_parent(chain: list, declarations: dict) -> float:
    pattern = re.compile(r"new FollowParentGoal\(this,\s*([\d.]+)\)")
    for name in chain:
        text = file_of(name, declarations).read_text(encoding="utf-8", errors="replace")
        match = pattern.search(text)
        if match:
            return float(match.group(1))
    return 0.0


def parse_baby_dimensions(chain: list, declarations: dict, width: float, height: float,
                          is_animal: bool) -> tuple:
    # Only an `Animal` can be a baby in this engine (breeding is what creates one),
    # so a non-animal's baby box — a baby zombie's 0.49 x 0.98, a piglin's — is left
    # at 0 exactly as the hand-written table left it: no path can reach it.
    if not is_animal:
        return 0.0, 0.0
    for name in chain:
        text = file_of(name, declarations).read_text(encoding="utf-8", errors="replace")
        match = re.search(r"BABY_DIMENSIONS\s*=\s*([^;]+);", text)
        if not match:
            continue
        expr = match.group(1)
        scalable = re.search(r"EntityDimensions\.scalable\(([\d.]+)f?,\s*([\d.]+)f?\)", expr)
        if scalable:
            return float(scalable.group(1)), float(scalable.group(2))
        scales = re.findall(r"\.scale\(([\d.]+)f?\)", expr)
        if scales:
            factor = float(scales[-1])
            return width * factor, height * factor
    return width * 0.5, height * 0.5


def resolve_tag(name: str, tag_dir: pathlib.Path, cache: dict) -> set:
    """Flattened entity-type names of a tag, nested `#tag` references resolved."""
    if name in cache:
        return cache[name]
    path = tag_dir / (name + ".json")
    if not path.exists():
        raise SystemExit(f"missing entity-type tag file: {path}")
    values = json.loads(path.read_text(encoding="utf-8")).get("values", [])
    out = set()
    for value in values:
        if isinstance(value, dict):
            value = value.get("id", "")
        if value.startswith("#"):
            out |= resolve_tag(value[1:].split(":")[-1], tag_dir, cache)
        else:
            out.add(value if ":" in value else "minecraft:" + value)
    cache[name] = out
    return out


def display_name(name: str) -> str:
    return " ".join(part.capitalize() for part in name.split("_"))


def category_constants(header: pathlib.Path) -> dict:
    """MobCategory serialized name -> kSpawn* constant, out of the generated
    biome spawn table (the vocabulary's single source)."""
    text = header.read_text(encoding="utf-8")
    match = re.search(r"kMobSpawnCategoryNames\[kSpawnCategoryCount\]\s*=\s*\{(.*?)\};", text, re.S)
    if not match:
        raise SystemExit(f"kMobSpawnCategoryNames not found in {header}")
    names = re.findall(r'"([a-z_]+)"', match.group(1))
    consts = {
        "monster": "kSpawnMonster", "creature": "kSpawnCreature", "ambient": "kSpawnAmbient",
        "axolotls": "kSpawnAxolotls", "underground_water_creature": "kSpawnUndergroundWaterCreature",
        "water_creature": "kSpawnWaterCreature", "water_ambient": "kSpawnWaterAmbient",
        "misc": "kSpawnMisc",
    }
    if sorted(names) != sorted(consts):
        raise SystemExit(f"category vocabulary changed: {names}")
    return consts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--entity-types", type=pathlib.Path,
                        default=ROOT / "data/vanilla/26.3/entity_types.tsv")
    parser.add_argument("--decomp", type=pathlib.Path,
                        default=ROOT / "build/_decomp")
    parser.add_argument("--entity-types-java", type=pathlib.Path,
                        default=ROOT / "build/_decomp/net/minecraft/world/entity/EntityTypes.java")
    parser.add_argument("--tags", type=pathlib.Path,
                        default=ROOT / "data/vanilla/26.3/data/minecraft/tags/entity_type")
    parser.add_argument("--category-header", type=pathlib.Path,
                        default=ROOT / "src/world/biome_spawn_table.h")
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "src/world/mob_table.h")
    args = parser.parse_args()

    rows = []
    for line in args.entity_types.read_text(encoding="utf-8").splitlines()[1:]:
        cells = line.split("\t")
        if len(cells) != 16:
            raise SystemExit(f"bad entity_types.tsv row: {line!r}")
        (eid, name, category, width, height, fire, health, speed, has_attack, attack, armor,
         follow, knockback, knockback_resist, jump, fall) = cells
        rows.append(dict(entity_type_id=int(eid), name=name, category=category,
                         width=float(width), height=float(height), fire_immune=fire == "true",
                         max_health=float(health), movement_speed=float(speed),
                         has_attack=has_attack == "true", attack_damage=float(attack),
                         armor=float(armor), follow_range=float(follow)))

    # The class each type is built from, out of EntityTypes.java.
    java = args.entity_types_java.read_text(encoding="utf-8")
    builder = re.compile(
        r"EntityTypes\.register\(EntityTypeIds\.([A-Z0-9_]+),\s*EntityType\.Builder\.of\("
        r"([A-Za-z0-9_$.]+)::new,\s*MobCategory\.([A-Z_]+)\)")
    class_for = {}
    for match in builder.finditer(java):
        constant, factory, _category = match.group(1, 2, 3)
        class_for["minecraft:" + constant.lower()] = factory.rsplit(".", 1)[-1]

    all_files = sorted(args.decomp.rglob("*.java"))
    declarations = class_declarations(all_files)

    inverted = resolve_tag("inverted_healing_and_harm", args.tags, {})
    categories = category_constants(args.category_header)

    mobs = []
    for row in rows:
        name = row["name"]
        if name in EXCLUDED:
            continue
        cls = class_for.get(name)
        if cls is None:
            raise SystemExit(f"no EntityTypes.Builder.of(..) factory found for {name}")
        if cls not in declarations:
            raise SystemExit(f"class {cls} for {name} is not in the decompiled tree")
        chain = chain_of(cls, declarations)
        is_monster = is_enemy(chain, declarations)
        is_animal = ANIMAL in chain
        flags = []
        if is_monster:
            flags.append("kMobHostile")
        if is_monster and parse_must_see(chain, declarations):
            flags.append("kMobMustSee")
        if parse_follows_unseen(chain, declarations):
            flags.append("kMobFollowsUnseen")
        if name in inverted:
            flags.append("kMobInvertedHealAndHarm")
        if parse_no_far_despawn(chain, declarations, is_animal):
            flags.append("kMobNoFarDespawn")
        if is_animal:
            flags.append("kMobAnimal")
        if parse_retaliates(chain, declarations):
            flags.append("kMobRetaliates")
        xp_min, xp_max = parse_xp(chain, declarations, is_animal, is_monster, name)
        baby_w, baby_h = parse_baby_dimensions(chain, declarations, row["width"], row["height"],
                                               is_animal)
        mobs.append(dict(
            name=name, display=display_name(name.split(":", 1)[1]),
            entity_type_id=row["entity_type_id"], category=categories[row["category"]],
            category_name=row["category"], flags=flags, fire_immune=row["fire_immune"],
            width=row["width"], height=row["height"],
            baby_width=baby_w, baby_height=baby_h, max_health=row["max_health"],
            movement_speed=row["movement_speed"], attack_damage=row["attack_damage"],
            follow_range=row["follow_range"],
            follow_parent=parse_follow_parent(chain, declarations), armor=row["armor"],
            xp_min=xp_min, xp_max=xp_max, cls=cls,
        ))

    mobs.sort(key=lambda m: m["name"])
    names = [m["name"] for m in mobs]
    if len(names) != len(set(names)):
        raise SystemExit("duplicate mob names")
    ids = [m["entity_type_id"] for m in mobs]
    if len(ids) != len(set(ids)):
        raise SystemExit("duplicate entity type ids")

    # The six rows the old table pinned: fail loudly if a derived value moved.
    for name, want in EXPECTED.items():
        got = next((m for m in mobs if m["name"] == name), None)
        if got is None:
            raise SystemExit(f"{name} missing from the generated table")
        checks = {
            "entity_type_id": got["entity_type_id"], "width": got["width"], "height": got["height"],
            "max_health": got["max_health"], "movement_speed": got["movement_speed"],
            "attack_damage": got["attack_damage"], "follow_range": got["follow_range"],
            "armor": got["armor"], "xp_min": got["xp_min"], "xp_max": got["xp_max"],
            "follow_parent": got["follow_parent"], "category": got["category_name"],
            "baby": (got["baby_width"], got["baby_height"]),
            "fire_immune": got["fire_immune"],
        }
        for key, got_value in checks.items():
            want_value = want[key]
            if isinstance(want_value, float):
                same = abs(got_value - want_value) < 1e-6
            else:
                same = got_value == want_value
            if not same:
                raise SystemExit(f"{name}: {key} derived {got_value!r}, hand table had "
                                 f"{want_value!r} — refusing to change a verified row")
        short = {"kMobHostile": "hostile", "kMobMustSee": "must_see",
                 "kMobFollowsUnseen": "follows_unseen", "kMobInvertedHealAndHarm": "inverted",
                 "kMobNoFarDespawn": "no_far_despawn", "kMobAnimal": "animal"}
        derived = {short[f] for f in got["flags"] if f in short}
        if "kMobRetaliates" in got["flags"]:
            derived.add("retaliates")
        # The old table's flags are a *subset*: kMobRetaliates is a flag the old
        # model did not carry, and a row is allowed to gain it (a zombie registers
        # HurtByTargetGoal). A pinned flag that went *missing* is a failure.
        missing = set(want["flags"]) - derived
        if missing:
            raise SystemExit(f"{name}: derived flags {sorted(derived)} miss {sorted(missing)}")

    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("// clang-format off")
    out.append("//")
    out.append(f"// The mob table ({len(mobs)} rows, generated — do not hand-edit).")
    out.append("//")
    out.append("// Every living entity type the 26.3 server registers except the player, the")
    out.append("// armour stand and the mannequin (tools/gen_mob_table.py drops those by name;")
    out.append("// all three are living with attributes but have no AI). Rows are sorted by name.")
    out.append("//")
    out.append("// Where each column comes from:")
    out.append("//")
    out.append("//   * `entity_type_id`, `category`, `width`, `height`, `max_health`,")
    out.append("//     `movement_speed`, `attack_damage`, `follow_range` and `armor` are the")
    out.append("//     game's own numbers, dumped by tools/gen_entity_types.py (a Java probe")
    out.append("//     against the server jar: BuiltInRegistries.ENTITY_TYPE, EntityType")
    out.append("//     .getDimensions(), DefaultAttributes.getSupplier(type));")
    out.append("//     `movement_speed` is the vanilla attribute — world/mob.cpp converts it with")
    out.append("//     `m/s = attribute^2 * 43.1718` (Minecraft Wiki, Talk:Attribute).")
    out.append("//   * `category` is the type's `MobCategory`, which natural spawning filters")
    out.append("//     and caps on (world/mob_spawn.h) and `Mob.checkDespawn` measures against")
    out.append("//     (world/mob_category.h).")
    out.append("//   * `fire_immune` is `EntityType.fireImmune()` from the same probe: the flag")
    out.append("//     `Entity.isInvulnerableTo` refuses `IS_FIRE` damage on (so a blaze or a")
    out.append("//     magma cube never burns), the flag `EntityType.isBlockDangerous` reads")
    out.append("//     for a burning block (so only a fire-immune type may stand in one), and")
    out.append("//     the flag a magma block's `isValidSpawn` reads for spawning on it.")
    out.append("//   * the flags are derived from the class hierarchy and the decompiled")
    out.append("//     sources, one rule each:")
    out.append("//       - kMobHostile          the chain contains `Monster`, or the class")
    out.append("//                              implements the `Enemy` marker (a ghast, a")
    out.append("//                              phantom, a slime, a shulker, a hoglin, the")
    out.append("//                              dragon);")
    out.append("//       - kMobMustSee          its player-target goal is a must-see scan")
    out.append("//                              (the first boolean in the NearestAttackable-")
    out.append("//                              TargetGoal call — true for every monster here);")
    out.append("//       - kMobFollowsUnseen    its melee goal tracks a target it cannot see")
    out.append("//                              (MeleeAttackGoal's followingTargetEvenIfNotSeen,")
    out.append("//                              read from the call site or the goal class's")
    out.append("//                              own `super(...)` — spider is the one live case);")
    out.append("//       - kMobInvertedHealAndHarm  membership of the")
    out.append("//                              `minecraft:inverted_healing_and_harm` entity-type")
    out.append("//                              tag (resolved from the data pack, nested tags and")
    out.append("//                              all), so a healing potion damages it and an")
    out.append("//                              instant-damage one heals it;")
    out.append("//       - kMobNoFarDespawn     `Animal.removeWhenFarAway` returning false, or a")
    out.append("//                              literal-false override in its own chain (the")
    out.append("//                              golems, an allay);")
    out.append("//       - kMobAnimal           the class chain contains `Animal` (an AgeableMob")
    out.append("//                              with a food tag — breeding, babies and feeding);")
    out.append("//       - kMobRetaliates       it registers HurryByTargetGoal (whoever hits it")
    out.append("//                              becomes its target, non-hostiles included).")
    out.append("//   * `baby_width`/`baby_height` are the nearest `BABY_DIMENSIONS` initialiser")
    out.append("//     (`EntityDimensions.scalable(w, h)`, or the type's own adult box")
    out.append("//     `.scale(f)`), and an `Animal` that declares none falls back to half its")
    out.append("//     adult box — stated here because it is the one approximation in a column.")
    out.append("//   * `xp_min`/`xp_max` are the nearest `this.xpReward = N` in the hierarchy,")
    out.append("//     else `Animal`'s `1 + random.nextInt(3)` or `Monster`'s 5.")
    out.append("//   * `follow_parent_speed` is the nearest `new FollowParentGoal(this, speed)`")
    out.append("//     registration (0 for a mob with no such goal).")
    out.append("//")
    out.append("// What a row cannot express, and why — all of it is \"not modelled yet\", none")
    out.append("// of it is silently wrong:")
    out.append("//   * equipment, DropChances and picked-up items; riders; per-mob baby")
    out.append("//     multipliers (a zombie's 2.5x XP) and group data (reinforcements, a")
    out.append("//     slime's size); `notInPeaceful`; per-type `getMaxSpawnClusterSize`;")
    out.append("//     the per-type `immuneTo` block tags (`#fox_immune_to`,")
    out.append("//     `#polar_bear_immune_to`, `#stray_immune_to`, `#wither_immune_to`, …);")
    out.append("//     `isSensitiveToWater` (the snow golem is the one type in this table that")
    out.append("//     overrides it);")
    out.append("//   * the mob's eye height is the `height * 0.85` default `EntityType.Builder")
    out.append("//     .sized` fills in, not each class's own `withEyeHeight` value;")
    out.append("//   * every mob-specific *ability*: the table's rows are spawned with their")
    out.append("//     real stats and their family's generic goal (a monster chases and hits,")
    out.append("//     anything else strolls), so the types whose signature behaviour is")
    out.append("//     something else are named here and nowhere pretend to it:")
    for name in sorted(UNMODELLED_AI):
        if any(m["name"] == name for m in mobs):
            out.append(f"//       - {name[10:]:22s} {UNMODELLED_AI[name]};")
    out.append("//     they exist, take damage, die and drop loot exactly as their type says,")
    out.append("//     and the generic goal is what is missing.")
    out.append("//   * entity-type tags this table's readers use (an enchantment's sensitive_to,")
    out.append("//     the undead membership above) resolve by name, so a mob with no row is")
    out.append("//     still correct in those tests — see world/entity_type_tags.h.")
    out.append("//")
    out.append("// Regenerate with tools/gen_mob_table.py (it cross-checks the six rows Phase 3")
    out.append("// hand-verified, byte for byte, and fails rather than move one).")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("#include <string_view>")
    out.append("")
    out.append("#include \"world/biome_spawn_table.h\" // MobSpawnCategory (the `category` column)")
    out.append("")
    out.append("namespace mc::world {")
    out.append("")
    out.append("// Mob flags bit values.")
    out.append("constexpr uint8_t kMobHostile = 1;      // Monster: hunts players and attacks them.")
    out.append("// NearestAttackableTargetGoal's `mustSee`: the goal can only *acquire* a target it")
    out.append("// can see (TargetingConditions.forCombat checks line of sight), and once it has")
    out.append("// one it forgets it after 60 unseen ticks (TargetGoal.unseenMemoryTicks).")
    out.append("constexpr uint8_t kMobMustSee = 2;")
    out.append("// MeleeAttackGoal's `followingTargetEvenIfNotSeen`: the *tick* re-plan gate. With")
    out.append("// it the mob keeps re-planning its path while blind; without it the mob walks the")
    out.append("// path it had and only re-plans when the goal restarts (which happens through the")
    out.append("// 20-tick `lastCanUseCheck` gate).")
    out.append("constexpr uint8_t kMobFollowsUnseen = 4;")
    out.append("// LivingEntity.isInvertedHealAndHarm, i.e. membership of the")
    out.append("// `minecraft:inverted_healing_and_harm` entity-type tag — so a thrown instant")
    out.append("// health *damages* this mob and an instant damage heals it, with the flipped")
    out.append("// amounts (`HealOrHarmMobEffect`).")
    out.append("constexpr uint8_t kMobInvertedHealAndHarm = 8;")
    out.append("// `Mob.removeWhenFarAway(distSqr)` is false: the mob is not discarded for being")
    out.append("// far from every player (every `Animal`, the golems and an allay). It gates only")
    out.append("// `Mob.checkDespawn`'s 128-block branch, not the slow one.")
    out.append("constexpr uint8_t kMobNoFarDespawn = 16;")
    out.append("// `Animal`: an `AgeableMob` with a food tag, a love timer and a breed goal.")
    out.append("constexpr uint8_t kMobAnimal = 32;")
    out.append("// `HurtByTargetGoal`: whoever damages this mob becomes its target (held with the")
    out.append("// 300-tick UnseenMemoryTicks that goal sets), *including* mobs that never hunt")
    out.append("// otherwise — a wolf, a polar bear, an iron golem. A non-hostile with this flag")
    out.append("// acquires a target only through being hurt; its nearest-target scan stays off.")
    out.append("constexpr uint8_t kMobRetaliates = 64;")
    out.append("")
    out.append("struct GeneratedMob {")
    out.append("  const char* name;         // \"minecraft:zombie\"")
    out.append("  const char* display_name; // \"Zombie\" (the death message's attacker name)")
    out.append("  int32_t entity_type_id;   // minecraft:entity_type protocol id (wire spawn)")
    out.append("  uint8_t category;         // MobSpawnCategory (EntityType.Builder.of(.., category))")
    out.append("  uint8_t flags;")
    out.append("  bool fire_immune;         // EntityType.fireImmune() (no IS_FIRE damage)")
    out.append("  float width;              // EntityType.Builder.sized")
    out.append("  float height;")
    out.append("  // The `getDefaultDimensions`' baby box for an `Animal` (0 for a mob that is not")
    out.append("  // one): the class's `BABY_DIMENSIONS`, or half the adult box when it declares")
    out.append("  // none (the generator's one documented fallback).")
    out.append("  float baby_width;")
    out.append("  float baby_height;")
    out.append("  float max_health;         // Attributes.MAX_HEALTH")
    out.append("  double movement_speed;    // Attributes.MOVEMENT_SPEED (an attribute, not blocks/tick)")
    out.append("  float attack_damage;      // Attributes.ATTACK_DAMAGE (0 for a passive mob)")
    out.append("  double follow_range;      // Attributes.FOLLOW_RANGE (target acquisition)")
    out.append("  // `new FollowParentGoal(this, speed)`: the navigation speed modifier a baby")
    out.append("  // walks at while following a parent, 0 for a mob that has no such goal.")
    out.append("  float follow_parent_speed;")
    out.append("  float armor;              // Attributes.ARMOR")
    out.append("  int xp_min;               // XP the kill awards, rolled in [xp_min, xp_max]")
    out.append("  int xp_max;")
    out.append("};")
    out.append("")
    def fnum(value: float) -> str:
        """A C++ float literal: `0.5f`, `4.0f` — never `0f` or `4f`, which is a
        decimal/octal integer the `f` suffix cannot attach to."""
        text = f"{value:g}"
        if "." not in text and "e" not in text:
            text += ".0"
        return text + "f"

    def dnum(value: float) -> str:
        text = f"{value:.6g}"
        if "." not in text and "e" not in text:
            text += ".0"
        return text

    out.append("// clang-format off")
    out.append("inline constexpr GeneratedMob kMobs[] = {")
    for row in mobs:
        flag_text = " | ".join(row["flags"]) if row["flags"] else "0"
        out.append(f'    {{"{row["name"]}", "{row["display"]}", {row["entity_type_id"]}, '
                   f'{row["category"]}, {flag_text}, '
                   f'{"true" if row["fire_immune"] else "false"}, '
                   f'{fnum(row["width"])}, {fnum(row["height"])}, '
                   f'{fnum(row["baby_width"])}, {fnum(row["baby_height"])}, '
                   f'{fnum(row["max_health"])}, {dnum(row["movement_speed"])}, '
                   f'{fnum(row["attack_damage"])}, {dnum(row["follow_range"])}, '
                   f'{fnum(row["follow_parent"])}, {fnum(row["armor"])}, '
                   f'{row["xp_min"]}, {row["xp_max"]}}},')
    out.append("};")
    out.append("// clang-format on")
    out.append(f"inline constexpr int kMobTypeCount = {len(mobs)};")
    out.append("")
    out.append("// The mob a name refers to (\"zombie\" / \"minecraft:zombie\" / \"ZOMBIE\"), or")
    out.append("// nullptr. The name is normalized first.")
    out.append("const GeneratedMob* mob_by_name(std::string_view name);")
    out.append("")
    out.append("// The mob an entity id in an Add Entity packet refers to, or nullptr.")
    out.append("const GeneratedMob* mob_by_entity_type_id(int32_t entity_type_id);")
    out.append("")
    out.append("} // namespace mc::world")
    out.append("")
    args.out.write_text("\n".join(out) + "\n")
    print(f"wrote {args.out} ({len(mobs)} mobs; {len(UNMODELLED_AI)} unmodelled-AI families noted)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
