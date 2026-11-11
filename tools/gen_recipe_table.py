#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates src/world/recipe_table.h: every 26.3 recipe, as data.

Why generate instead of parse at runtime: the recipe set is a *datapack* file
tree, and reading 1763 JSON documents (plus item tags) at startup would mean
shipping a JSON parser and paying that cost on every boot. The project already
generates its block/item registries from vanilla's own data, so recipes follow
the same rule: one generator, one committed header, no hand-editing.

The table covers all 1763 recipes, so the milestones that still need them (the
recipe book and stonecutter menu, furnaces, smithing) do not have to regenerate
it. Only the crafting-table families are *matched* by the engine today —
`crafting_shaped`, `crafting_shapeless` and `crafting_transmute`. Everything else
is recorded with its kind, so the table is complete and the matcher can never
mistake an unimplemented `crafting_special_*` for a shapeless recipe.

Ingredient sets matter: an ingredient is a `HolderSet<Item>` (Ingredient.java),
so one entry can be an item, a `#tag`, or a JSON *list* of either — 78 of the
26.3 ingredients are lists. Tags nest, so they are resolved to a
flat sorted item-id set here, once, instead of on every match.

Usage:
    gen_recipe_table.py RECIPE_DIR TAG_DIR ITEM_HEADER OUT_HEADER
"""

import json
import os
import re
import sys

# Cooking times default per subclass: SmeltingRecipe 200, BlastingRecipe 100,
# SmokingRecipe 100, CampfireCookingRecipe 100. All 9 campfire recipes carry an
# explicit `cookingtime` of 600 in the 26.3 data.
COOK_DEFAULTS = {
    "minecraft:smelting": 200,
    "minecraft:blasting": 100,
    "minecraft:smoking": 100,
    "minecraft:campfire_cooking": 100,
}

# Cooking families map to their own RecipeKind; everything else is spelled out.
COOKING_KINDS = {
    "minecraft:smelting": "Smelting",
    "minecraft:blasting": "Blasting",
    "minecraft:smoking": "Smoking",
    "minecraft:campfire_cooking": "Campfire",
}

# The transmute bounds default (TransmuteRecipe.DEFAULT_MATERIAL_COUNT).
MATERIAL_MIN_DEFAULT = 1
MATERIAL_MAX_DEFAULT = 8

# RecipeBookCategory registry ids (RecipeBookCategories.java registration order,
# which is also the id order: the registry is code-registered, not datapack).
# The recipe book's tabs are these categories, and an entry whose category is
# not one the open menu's RecipeBookType lists is dropped without a word — so a
# wrong value here is a silently empty book, not an error.
BOOK_CATEGORIES = {
    "building": 0,              # crafting_building_blocks
    "redstone": 1,              # crafting_redstone
    "equipment": 2,             # crafting_equipment
    "misc_crafting": 3,         # crafting_misc
    "food_furnace": 4,          # furnace_food
    "blocks_furnace": 5,        # furnace_blocks
    "misc_furnace": 6,          # furnace_misc
    "blocks_blasting": 7,       # blast_furnace_blocks
    "misc_blasting": 8,         # blast_furnace_misc
    "food_smoking": 9,          # smoker_food
    "stonecutter": 10,
    "smithing": 11,
    "campfire": 12,
}

# The JSON `category` string -> BOOK_CATEGORIES key, per recipe family, from the
# `recipeBookCategory()` implementations:
#   * CraftingRecipe (shaped/shapeless/transmute): CraftingBookCategory's own
#     order is the registry order for the four crafting tabs, and the field
#     defaults to MISC.
#   * AbstractCookingRecipe's subclasses each map CookingBookCategory onto their
#     own furnace's tabs; SmokingRecipe and CampfireCookingRecipe ignore the
#     category entirely and always use their one tab (SmokingRecipe returns
#     SMOKER_FOOD, CampfireCookingRecipe CAMPFIRE).
#   * BlastingRecipe folds FOOD and MISC into BLAST_FURNACE_MISC.
CATEGORY_BY_KIND = {
    "Shaped": {"building": "building", "redstone": "redstone",
               "equipment": "equipment", "misc": "misc_crafting"},
    "Shapeless": {"building": "building", "redstone": "redstone",
                  "equipment": "equipment", "misc": "misc_crafting"},
    "Transmute": {"building": "building", "redstone": "redstone",
                  "equipment": "equipment", "misc": "misc_crafting"},
    "Smelting": {"food": "food_furnace", "blocks": "blocks_furnace",
                 "misc": "misc_furnace"},
    "Blasting": {"food": "misc_blasting", "blocks": "blocks_blasting",
                 "misc": "misc_blasting"},
}
FIXED_CATEGORY_BY_KIND = {
    "Smoking": "food_smoking",
    "Campfire": "campfire",
    "Stonecutting": "stonecutter",
    "SmithingTransform": "smithing",
    "SmithingTrim": "smithing",
    "Special": "misc_crafting",
}


def book_category(kind, recipe, where):
    """The RecipeBookCategory registry id the recipe's book entry uses."""
    if kind in FIXED_CATEGORY_BY_KIND:
        return BOOK_CATEGORIES[FIXED_CATEGORY_BY_KIND[kind]]
    raw = recipe.get("category", "misc")
    mapping = CATEGORY_BY_KIND.get(kind)
    if mapping is None:
        raise ValueError(f"{where}: no recipe-book category for {kind}")
    if raw not in mapping:
        raise ValueError(f"{where}: unknown category {raw!r} for {kind}")
    return BOOK_CATEGORIES[mapping[raw]]


def load_item_ids(header_path):
    """Parses src/world/item_registry.h into {name: id}."""
    text = open(header_path).read()
    body = text.split("kItems[]", 1)[1].split("};", 1)[0]
    pairs = re.findall(r'\{"([^"]+)", (\d+),', body)
    return {name: int(i) for name, i in pairs}


def load_tags(tag_dir, item_ids):
    """Resolves every item tag to a sorted list of item ids.

    Tags may list `#other_tag`, so resolution is recursive with a cycle guard
    rather than a single pass. A missing tag resolves to the empty set and is
    reported, matching vanilla's "unknown tag = nothing satisfies it" result
    rather than failing the whole generation.
    """
    raw = {}
    # Tags live at the top level *and* in subdirectories (`minecraft:sulfur_
    # cube_archetype/hot` is tags/item/sulfur_cube_archetype/hot.json), so the
    # walk has to recurse: a nested tag read as "missing" would resolve to the
    # empty set, which vanilla reads as "nothing satisfies this ingredient".
    for root, _dirs, files in os.walk(tag_dir):
        for filename in sorted(files):
            if not filename.endswith(".json"):
                continue
            path = os.path.join(root, filename)
            rel = os.path.relpath(path, tag_dir)[:-5].replace(os.sep, "/")
            raw["minecraft:" + rel] = json.load(open(path)).get("values", [])

    resolved = {}
    in_progress = set()

    def resolve(tag):
        if tag in resolved:
            return resolved[tag]
        if tag in in_progress:
            raise ValueError(f"tag cycle through {tag}")
        if tag not in raw:
            print(f"warning: unknown tag {tag}", file=sys.stderr)
            return []
        in_progress.add(tag)
        out = set()
        for entry in raw[tag]:
            if isinstance(entry, dict):
                # {"id": ..., "required": false} — only the id matters.
                entry = entry.get("id", "")
            if entry.startswith("#"):
                out.update(resolve(entry[1:]))
            elif entry in item_ids:
                out.add(item_ids[entry])
            else:
                print(f"warning: tag {tag} names unknown item {entry}", file=sys.stderr)
        in_progress.discard(tag)
        resolved[tag] = sorted(out)
        return resolved[tag]

    for tag in raw:
        resolve(tag)
    return resolved


def ingredient_items(spec, tags, item_ids, where):
    """Resolves one ingredient (a string or a list of them) to sorted item ids."""
    if isinstance(spec, list):
        out = set()
        for entry in spec:
            out.update(ingredient_items(entry, tags, item_ids, where))
        return sorted(out)
    if not isinstance(spec, str):
        raise ValueError(f"{where}: unsupported ingredient {spec!r}")
    if spec.startswith("#"):
        return list(tags.get(spec[1:], ()))
    if spec not in item_ids:
        raise ValueError(f"{where}: unknown item {spec}")
    return [item_ids[spec]]


def shrink(pattern):
    """Vanilla's ShapedRecipePattern.shrink: trims empty rows and columns.

    This runs at *load* time in vanilla, so the stored box is already trimmed —
    and CraftingInput.ofPositioned trims the player's grid the same way, which is
    why a shaped recipe matches wherever it is placed in the table.
    """
    left = min((len(line) - len(line.lstrip(" ")) for line in pattern), default=0)
    right = max((len(line.rstrip(" ")) for line in pattern), default=0)
    rows = [i for i, line in enumerate(pattern) if line.strip(" ")]
    if not rows:
        return []
    return [line[left:right] for line in pattern[rows[0]:rows[-1] + 1]]


def result_of(recipe, where, item_ids):
    """(item id, count) from an ItemStackTemplate-shaped `result`, or None.

    The result may carry a `components` patch (17 recipes do — the 16 suspicious
    stews and one other). Component patches are not modelled on ItemStack yet, so
    the item is recorded without them; the header says so.
    """
    res = recipe.get("result")
    if res is None:
        return None
    if not isinstance(res, dict):
        raise ValueError(f"{where}: unsupported result {res!r}")
    # 26.3's code-driven special recipes (`map_cloning`, `map_extending`) carry
    # an empty result object: the output is the input stack, so there is no
    # item id to record.
    name = res.get("id")
    if name is None:
        return None
    if name not in item_ids:
        raise ValueError(f"{where}: unknown result item {name}")
    return item_ids[name], int(res.get("count", 1))


def parse_recipe(name, recipe, tags, item_ids):
    """One JSON document -> a (kind, result, cells, width, height, extras) tuple."""
    rtype = recipe["type"]
    where = name
    result_of_here = lambda: result_of(recipe, where, item_ids)

    if rtype == "minecraft:crafting_shaped":
        pattern = shrink(recipe["pattern"])
        if not pattern:
            raise ValueError(f"{where}: empty pattern")
        width = max(len(line) for line in pattern)
        if any(len(line) != width for line in pattern):
            raise ValueError(f"{where}: ragged pattern {pattern!r}")
        key = recipe["key"]
        cells = []
        for line in pattern:
            for ch in line:
                if ch == " ":
                    cells.append(())
                else:
                    if ch not in key:
                        raise ValueError(f"{where}: symbol {ch!r} not in key")
                    cells.append(tuple(ingredient_items(key[ch], tags, item_ids, where)))
        return ("Shaped", result_of_here(), cells, width, len(pattern), {})

    if rtype == "minecraft:crafting_shapeless":
        cells = [tuple(ingredient_items(i, tags, item_ids, where))
                 for i in recipe["ingredients"]]
        return ("Shapeless", result_of_here(), cells, 0, 0, {})

    if rtype == "minecraft:crafting_transmute":
        bounds = recipe.get("material_count")
        if isinstance(bounds, int):
            lo = hi = bounds
        elif isinstance(bounds, dict):
            lo = int(bounds.get("min", MATERIAL_MIN_DEFAULT))
            hi = int(bounds.get("max", MATERIAL_MAX_DEFAULT))
        else:
            lo, hi = MATERIAL_MIN_DEFAULT, MATERIAL_MAX_DEFAULT
        if recipe.get("add_material_count_to_result"):
            # The result count is the base count plus the materials used; the
            # engine adds the materials back, so the top bit marks this case.
            hi |= 0x80
        cells = (tuple(ingredient_items(recipe["input"], tags, item_ids, where)),
                 tuple(ingredient_items(recipe["material"], tags, item_ids, where)))
        return ("Transmute", result_of_here(), cells, 0, 0,
                {"material_min": lo, "material_max": hi})

    if rtype in COOK_DEFAULTS:
        cells = (tuple(ingredient_items(recipe["ingredient"], tags, item_ids, where)),)
        return (COOKING_KINDS[rtype], result_of_here(), cells, 0, 0,
                {"cook_time": int(recipe.get("cookingtime", COOK_DEFAULTS[rtype])),
                 "xp": float(recipe.get("experience", 0.0))})

    if rtype == "minecraft:stonecutting":
        cells = (tuple(ingredient_items(recipe["ingredient"], tags, item_ids, where)),)
        return ("Stonecutting", result_of_here(), cells, 0, 0, {})

    if rtype == "minecraft:smithing_transform":
        cells = tuple(tuple(ingredient_items(recipe[f], tags, item_ids, where))
                      for f in ("template", "base", "addition"))
        return ("SmithingTransform", result_of_here(), cells, 0, 0, {})

    if rtype == "minecraft:smithing_trim":
        cells = tuple(tuple(ingredient_items(recipe[f], tags, item_ids, where))
                      for f in ("template", "base", "addition"))
        # No `result`: the trimmed item is derived from the base plus the pattern,
        # which this milestone does not model.
        return ("SmithingTrim", None, cells, 0, 0, {})

    # crafting_special_* (banner copy, fireworks, map extending, shield
    # decoration, ...), crafting_dye, crafting_imbue, crafting_decorated_pot —
    # all code-driven in vanilla. Recorded by name so the table is complete and
    # the matcher knows not to guess.
    return ("Special", None, (), 0, 0, {"source_type": rtype})


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    recipe_dir, tag_dir, item_header, out_path = sys.argv[1:]

    item_ids = load_item_ids(item_header)
    tags = load_tags(tag_dir, item_ids)

    parsed = []
    specials = []
    for filename in sorted(os.listdir(recipe_dir)):
        if not filename.endswith(".json"):
            continue
        name = filename[:-5]
        recipe = json.load(open(os.path.join(recipe_dir, filename)))
        entry = parse_recipe(name, recipe, tags, item_ids)
        entry[5]["book_category"] = book_category(entry[0], recipe, name)
        parsed.append((name,) + entry)
        if entry[0] == "Special":
            specials.append((name, entry[5].get("source_type")))

    # Two pools. Identical ingredient *sets* share one (first_item, item_count)
    # pair, and every recipe's cells are then emitted contiguously, so a recipe
    # is a (first_ing, ing_count) slice of kRecipeIngredients. Sharing the pairs
    # keeps the header small: 1763 recipes expand to a few thousand cells but
    # only 590 distinct sets.
    set_entries = {}
    item_pool = []
    for _, _, _, cells, _, _, _ in parsed:
        for ids in cells:
            if ids in set_entries:
                continue
            if not ids:
                set_entries[ids] = (0, 0)
            else:
                set_entries[ids] = (len(item_pool), len(ids))
                item_pool.extend(ids)

    recipe_rows = []
    cell_rows = []
    for name, kind, result, cells, width, height, extra in parsed:
        first_ing = len(cell_rows)
        for ids in cells:
            cell_rows.append(set_entries[ids])
        result_item, result_count = result if result else (0, 0)
        extra = extra or {}
        mat_hi = extra.get("material_max", 0)
        recipe_rows.append({
            "name": name,
            "kind": kind,
            "result_item": result_item,
            "result_count": result_count,
            "width": width,
            "height": height,
            "first_ing": first_ing,
            "ing_count": len(cells),
            "cook_time": extra.get("cook_time", 0),
            "xp_milli": round(extra.get("xp", 0.0) * 1000),
            "material_min": extra.get("material_min", 0),
            "material_max": mat_hi,
            "book_category": extra.get("book_category", 0),
        })

    # Cross-checks that make a silent data-model mistake impossible.
    for row in recipe_rows:
        if row["kind"] == "Shaped":
            assert row["ing_count"] == row["width"] * row["height"], row
        # A transmute with `result: {}` (26.3 map cloning) computes its output
        # from the input stack; a smithing trim does the same from its base.
        if row["kind"] in ("Shaped", "Shapeless", "Transmute",
                           "SmithingTransform", "SmithingTrim"):
            assert row["result_item"] != 0 or row["kind"] in ("SmithingTrim", "Transmute"), row
        if row["kind"] in ("Smelting", "Blasting", "Smoking", "Campfire",
                           "Stonecutting", "SmithingTransform", "SmithingTrim"):
            assert row["ing_count"] in (1, 3), row
        if row["kind"] == "Transmute":
            assert row["ing_count"] == 2, row
        assert 0 <= row["book_category"] <= 12, row

    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        f"// Every 26.3 recipe ({len(recipe_rows)}, generated — do not hand-edit): the",
        "// crafting-table families the engine matches, plus the rest of the tree",
        "// (cooking, stonecutting, smithing, the code-driven `crafting_special_*`)",
        "// recorded with their kind so the table is complete and the matcher can",
        "// never mistake an unimplemented recipe for a supported one.",
        "//",
        "// Regenerate with tools/gen_recipe_table.py RECIPE_DIR TAG_DIR",
        "// src/world/item_registry.h src/world/recipe_table.h.",
        "//",
        "// Layout notes, all read off the 26.3 codecs:",
        "//  * an *ingredient* is a `HolderSet<Item>` (Ingredient.java), so it is a",
        "//    set of item ids — a `#tag` or a JSON list expands to several. A cell",
        "//    with `item_count == 0` is a shaped pattern's *empty* slot, which",
        "//    vanilla requires to be really empty (Ingredient.testOptionalIngredient",
        "//    is `ingredient.test(stack)` or, for an empty cell, `stack.isEmpty()`).",
        "//  * a shaped pattern is stored **shrunk** — ShapedRecipePattern.shrink",
        "//    trims empty rows and columns at load time — and row-major. Matching",
        "//    compares it against the player's grid only after that grid is trimmed",
        "//    to its own bounding box (CraftingInput.ofPositioned), so a recipe",
        "//    matches wherever it sits in the table.",
        "//  * `xp_milli` is experience x 1000 (the JSON carries a float).",
        "//  * `material_min`/`material_max` are the transmute recipe's material_count",
        "//    bounds; bit 7 of material_max marks `add_material_count_to_result`,",
        "//    where the result count is the base count plus the materials used.",
        "//  * `book_category` is a RecipeBookCategory registry id (the recipe book's",
        "//    tab), from the JSON `category` field through each family's",
        "//    `recipeBookCategory()`. The book's tabs are per *menu type*, and an",
        "//    entry in a category the open menu does not list is dropped in silence.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace mc::world {",
        "",
        "enum class RecipeKind : uint8_t {",
        "  Shaped = 0,",
        "  Shapeless,",
        "  Transmute,",
        "  Smelting,",
        "  Blasting,",
        "  Smoking,",
        "  Campfire,",
        "  Stonecutting,",
        "  SmithingTransform,",
        "  SmithingTrim,",
        "  Special,",
        "};",
        "",
        # The structs below carry column-aligned trailing comments, which
        # clang-format rewrites; without this guard a regeneration would leave
        # the committed header failing the CI format gate.
        "// clang-format off",
        "// One ingredient = a set of item ids. `item_count == 0` is a shaped",
        "// pattern's empty slot: the grid must leave that cell empty.",
        "struct GeneratedIngredient {",
        "  uint32_t first_item; // index into kRecipeItems",
        "  uint16_t item_count; // 0 = the cell must be empty",
        "};",
        "",
        "struct GeneratedRecipe {",
        '  const char* name;     // "oak_planks" (the recipe file\'s name)',
        "  RecipeKind kind;",
        "  uint16_t result_item; // index into kItems (0 when the recipe computes it)",
        "  uint8_t result_count;",
        "  uint8_t width;        // shaped: the pattern box, row-major",
        "  uint8_t height;",
        "  uint16_t first_ing;   // index into kRecipeIngredients",
        "  uint8_t ing_count;    // shaped: width*height cells; else the ingredient list",
        "  uint16_t cook_time;   // cooking ticks (0 otherwise)",
        "  uint16_t xp_milli;    // experience x 1000",
        "  uint8_t material_min; // transmute material_count bounds",
        "  uint8_t material_max; // bit 7 -> add_material_count_to_result",
        "  uint8_t book_category; // RecipeBookCategory registry id (the book tab)",
        "};",
        "",
        "inline constexpr uint16_t kRecipeItems[] = {",
    ]
    for i in range(0, len(item_pool), 16):
        lines.append("    " + " ".join(f"{v}," for v in item_pool[i:i + 16]))
    lines += [
        "};",
        f"inline constexpr uint32_t kRecipeItemCount = {len(item_pool)};",
        "",
        "inline constexpr GeneratedIngredient kRecipeIngredients[] = {",
    ]
    for i in range(0, len(cell_rows), 8):
        chunk = cell_rows[i:i + 8]
        lines.append("    " + " ".join(f"{{{f}, {c}}}," for f, c in chunk))
    lines += [
        "};",
        f"inline constexpr uint32_t kRecipeIngredientCount = {len(cell_rows)};",
        "",
        "inline constexpr GeneratedRecipe kRecipes[] = {",
    ]
    for row in recipe_rows:
        lines.append(
            '    {"%s", RecipeKind::%s, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d},' % (
                row["name"], row["kind"], row["result_item"], row["result_count"],
                row["width"], row["height"], row["first_ing"], row["ing_count"],
                row["cook_time"], row["xp_milli"], row["material_min"],
                row["material_max"], row["book_category"]))
    lines += [
        "};",
        f"inline constexpr uint32_t kRecipeCount = {len(recipe_rows)};",
        "// clang-format on",
        "",
        "} // namespace mc::world",
        "",
    ]
    open(out_path, "w").write("\n".join(lines))
    print(f"wrote {out_path}: {len(recipe_rows)} recipes, {len(cell_rows)} cells "
          f"over {len(set_entries)} distinct ingredient sets, {len(item_pool)} item "
          f"ids, {len(specials)} code-driven specials")
    return 0


if __name__ == "__main__":
    sys.exit(main())
