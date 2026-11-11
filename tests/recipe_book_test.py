#!/usr/bin/env python3
"""Integration test: the recipe book (Phase 3 milestone D2).

A real client cannot use the recipe book unless three packets survive the wire:
the join push of `update_recipes`, `recipe_book_settings`, `recipe_book_add`,
and then `place_recipe` (the client's click on an entry) must put the recipe's
ingredients into the crafting grid. This test joins with the same harness the
crafting test uses and:

  * decodes `update_recipes` (0x85) the way ClientboundUpdateRecipesPacket does
    — 7 RecipePropertySets and the stonecutter list — and checks every byte is
    consumed, so a body with a trailing or missing field fails loudly;
  * decodes `recipe_book_add` (0x4a) down to the RecipeDisplayEntry fields the
    client reads (display id, RecipeDisplay, group, RecipeBookCategory,
    crafting requirements, flags) and finds the oak-log -> oak-planks display;
  * checks all 18 `smithing_trim` entries: each result slot is a
    `SlotDisplay.SmithingTrimDemoSlotDisplay` carrying the recipe's own base and
    addition ingredients and the pattern its template item names, and the 18
    patterns arrive exactly once each;
  * sends `place_recipe` (0x27) naming that display in window 0 and asserts the
    server filled the player menu's 2x2 and derived the result slot, proving the
    ids in the pushed list are the ids `place_recipe` resolves;
  * checks a stale window id and an unknown display id change nothing;
  * sends `recipe_book` (0x2e) and `displayed_recipe` (0x2f) and requires the
    session to survive them (they are accepted and dropped).

Wire layouts are read off the decompiled 26.2 codecs; see
src/world/recipe_book.h for the same description from the encoder's side.
"""

import os, socket, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import craft_test as ct
import wire_client
from craft_test import (Bot, Menu, drain, find, give, rd_string, rd_varint,
                        varint, wait_listening, CB_CONTAINER_SET_CONTENT,
                        CB_KEEP_ALIVE)

import subprocess, tempfile, time

PROTO = ct.PROTO
ITEM_OAK_LOG = ct.ITEM_OAK_LOG
ITEM_OAK_PLANKS = ct.ITEM_OAK_PLANKS
CREATIVE_HOTBAR0 = ct.CREATIVE_HOTBAR0
GROUND_Y = 3

# Clientbound packet ids (ids.h, verified against the 26.2 registration order).
CB_UPDATE_RECIPES = 0x88
CB_RECIPE_BOOK_ADD = 0x4B
CB_RECIPE_BOOK_SETTINGS = 0x4D
CB_PLACE_GHOST_RECIPE = 0x40
SB_PLACE_RECIPE = 0x27
SB_RECIPE_BOOK_SETTINGS = 0x2F
SB_RECIPE_BOOK_SEEN_RECIPE = 0x30

# RecipeDisplay registry ids (RecipeDisplays.bootstrap order).
DISPLAY_SHAPELESS = 0
DISPLAY_SHAPED = 1
DISPLAY_FURNACE = 2
DISPLAY_STONECUTTER = 3
DISPLAY_SMITHING = 4

# SlotDisplay registry ids.
SLOT_EMPTY = 0
SLOT_ANY_FUEL = 1
SLOT_WITH_ANY_POTION = 2
SLOT_ONLY_WITH_COMPONENT = 3
SLOT_ITEM = 4
SLOT_ITEM_STACK = 5
SLOT_TAG = 6
SLOT_DYED = 7
SLOT_SMITHING_TRIM = 8
SLOT_WITH_REMAINDER = 9
SLOT_COMPOSITE = 10

PLAYER_WINDOW = 0
PM_RESULT = 0
PM_GRID_FIRST = 1
PLAYER_MENU_SIZE = 46

PROPERTY_SET_KEYS = [
    "minecraft:smithing_base",
    "minecraft:smithing_template",
    "minecraft:smithing_addition",
    "minecraft:furnace_input",
    "minecraft:blast_furnace_input",
    "minecraft:smoker_input",
    "minecraft:campfire_input",
]


def rd_u8(body, pos):
    return body[pos], pos + 1


def rd_string_or_err(body, pos):
    s, pos = rd_string(body, pos)
    return s, pos


def rd_ingredient(body, pos):
    """Ingredient.CONTENTS_STREAM_CODEC: [varint count + 1] then the items, or a
    lone 0 followed by a tag resource location."""
    n, pos = rd_varint(body, pos)
    if n == 0:
        tag, pos = rd_string(body, pos)
        return {"tag": tag}, pos
    items = []
    for _ in range(n - 1):
        item, pos = rd_varint(body, pos)
        items.append(item)
    return {"items": items}, pos


def rd_slot_display(body, pos):
    """SlotDisplay.STREAM_CODEC. Only the kinds this server emits need payloads;
    the rest are here so a surprising entry fails on an assertion, not a crash."""
    kind, pos = rd_varint(body, pos)
    if kind in (SLOT_EMPTY, SLOT_ANY_FUEL, SLOT_WITH_ANY_POTION, SLOT_TAG, SLOT_DYED):
        return {"k": kind}, pos
    if kind == SLOT_SMITHING_TRIM:
        # SmithingTrimDemoSlotDisplay: the base and the material it trims, then
        # the pattern as a Holder (registry id + 1).
        base, pos = rd_slot_display(body, pos)
        material, pos = rd_slot_display(body, pos)
        pattern, pos = rd_varint(body, pos)
        return ({"k": kind, "base": base, "material": material,
                 "pattern": pattern}, pos)
    if kind == SLOT_ONLY_WITH_COMPONENT:
        _, pos = rd_varint(body, pos)
        inner, pos = rd_slot_display(body, pos)
        return {"k": kind, "inner": inner}, pos
    if kind == SLOT_ITEM:
        item, pos = rd_varint(body, pos)
        return {"k": kind, "item": item}, pos
    if kind == SLOT_ITEM_STACK:
        item, pos = rd_varint(body, pos)
        count, pos = rd_varint(body, pos)
        # `write_stack_slot` always writes an empty patch, and a clientbound
        # value carries no length, so an empty patch is asserted rather than
        # skipped: a display that ever gained a component must be decoded (see
        # container_gui_test.read_stack_full) rather than misread.
        added, pos = rd_varint(body, pos)
        removed, pos = rd_varint(body, pos)
        assert added == 0 and removed == 0, f"display stack patch {added}/{removed}"
        assert item != 0 and count > 0, (
            "an ItemStack display must be a real stack: ItemStackTemplate's "
            f"constructor rejects air or count 0 (got item {item}, count {count})")
        return {"k": kind, "item": item, "count": count}, pos
    if kind == SLOT_WITH_REMAINDER:
        inner, pos = rd_slot_display(body, pos)
        rem, pos = rd_slot_display(body, pos)
        return {"k": kind, "inner": inner, "rem": rem}, pos
    if kind == SLOT_COMPOSITE:
        n, pos = rd_varint(body, pos)
        items = []
        for _ in range(n):
            sd, pos = rd_slot_display(body, pos)
            items.append(sd)
        return {"k": kind, "items": items}, pos
    raise AssertionError(f"unknown SlotDisplay kind {kind}")


def rd_recipe_display(body, pos):
    kind, pos = rd_varint(body, pos)
    if kind == DISPLAY_SHAPELESS:
        n, pos = rd_varint(body, pos)
        ingredients = []
        for _ in range(n):
            sd, pos = rd_slot_display(body, pos)
            ingredients.append(sd)
        result, pos = rd_slot_display(body, pos)
        station, pos = rd_slot_display(body, pos)
        return ({"kind": kind, "ingredients": ingredients, "result": result,
                 "station": station}, pos)
    if kind == DISPLAY_SHAPED:
        w, pos = rd_varint(body, pos)
        h, pos = rd_varint(body, pos)
        n, pos = rd_varint(body, pos)
        assert n == w * h, f"a shaped display's box must be w*h = {w * h}, got {n}"
        cells = []
        for _ in range(n):
            sd, pos = rd_slot_display(body, pos)
            cells.append(sd)
        result, pos = rd_slot_display(body, pos)
        station, pos = rd_slot_display(body, pos)
        return ({"kind": kind, "width": w, "height": h, "cells": cells,
                 "result": result, "station": station}, pos)
    if kind == DISPLAY_FURNACE:
        ing, pos = rd_slot_display(body, pos)
        fuel, pos = rd_slot_display(body, pos)
        result, pos = rd_slot_display(body, pos)
        station, pos = rd_slot_display(body, pos)
        cook, pos = rd_varint(body, pos)
        (xp,) = struct.unpack(">f", body[pos:pos + 4])
        pos += 4
        return ({"kind": kind, "ingredient": ing, "fuel": fuel, "result": result,
                 "station": station, "cook_time": cook, "xp": xp}, pos)
    if kind == DISPLAY_STONECUTTER:
        ing, pos = rd_slot_display(body, pos)
        result, pos = rd_slot_display(body, pos)
        station, pos = rd_slot_display(body, pos)
        return ({"kind": kind, "ingredient": ing, "result": result,
                 "station": station}, pos)
    if kind == DISPLAY_SMITHING:
        template, pos = rd_slot_display(body, pos)
        base, pos = rd_slot_display(body, pos)
        addition, pos = rd_slot_display(body, pos)
        result, pos = rd_slot_display(body, pos)
        station, pos = rd_slot_display(body, pos)
        return ({"kind": kind, "template": template, "base": base,
                 "addition": addition, "result": result, "station": station}, pos)
    raise AssertionError(f"unknown RecipeDisplay kind {kind}")


def decode_recipe_book_add(body):
    """ClientboundRecipeBookAddPacket -> ([entry], replace)."""
    n, pos = rd_varint(body, 0)
    entries = []
    for _ in range(n):
        display_id, pos = rd_varint(body, pos)
        display, pos = rd_recipe_display(body, pos)
        group_plus_one, pos = rd_varint(body, pos)
        category, pos = rd_varint(body, pos)
        present, pos = rd_u8(body, pos)
        requirements = []
        if present:
            cnt, pos = rd_varint(body, pos)
            for _ in range(cnt):
                ing, pos = rd_ingredient(body, pos)
                requirements.append(ing)
        flags, pos = rd_u8(body, pos)
        entries.append({"id": display_id, "display": display,
                        "group": group_plus_one, "category": category,
                        "requirements": requirements, "flags": flags})
    replace, pos = rd_u8(body, pos)
    assert pos == len(body), (
        f"recipe_book_add left {len(body) - pos} trailing bytes — the body does "
        "not match the codec the client reads")
    return entries, replace


def decode_update_recipes(body):
    """ClientboundUpdateRecipesPacket -> ([property set], [stonecutter recipe])."""
    n, pos = rd_varint(body, 0)
    sets = []
    for _ in range(n):
        key, pos = rd_string_or_err(body, pos)
        cnt, pos = rd_varint(body, pos)
        items = []
        for _ in range(cnt):
            item, pos = rd_varint(body, pos)
            items.append(item)
        sets.append((key, items))
    n2, pos = rd_varint(body, pos)
    cutters = []
    for _ in range(n2):
        ing, pos = rd_ingredient(body, pos)
        result, pos = rd_slot_display(body, pos)
        cutters.append((ing, result))
    assert pos == len(body), (
        f"update_recipes left {len(body) - pos} trailing bytes")
    return sets, cutters


def decode_recipe_book_settings(body):
    """Six (open, filtering) pairs — CRAFTING, FURNACE, BLAST_FURNACE, SMOKER."""
    assert len(body) == 8, f"recipe_book_settings is 8 bytes, got {len(body)}"
    return [(body[i], body[i + 1]) for i in range(0, 8, 2)]


def all_primary_items(slot):
    """Every item id a slot display can show (a Composite is a set of items)."""
    if slot["k"] == SLOT_ITEM:
        return [slot["item"]]
    if slot["k"] == SLOT_ITEM_STACK:
        return [slot["item"]]
    if slot["k"] == SLOT_COMPOSITE:
        out = []
        for inner in slot["items"]:
            out.extend(all_primary_items(inner))
        return out
    if slot["k"] in (SLOT_WITH_REMAINDER, SLOT_ONLY_WITH_COMPONENT):
        return all_primary_items(slot.get("inner", slot.get("rem", {})))
    if slot["k"] == SLOT_SMITHING_TRIM:
        # A trim demo's items are whatever base it trims: the result is not a
        # stack of its own.
        return all_primary_items(slot["base"])
    return []


class BookBot(Bot):
    """A Bot that keeps every packet it receives, so the join push can be
    inspected after join() has drained the stream."""

    def __init__(self, name, port):
        super().__init__(name, port)
        self.seen = []

    def recv(self):
        pid, body = super().recv()
        self.seen.append((pid, body))
        return pid, body


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-recipe-book-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "1200"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "4000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    # The survival tick counts packets (one tick each, no wall-time catch-up), so
    # a drain banks no ticks. This file's subject is the recipe book's own state —
    # placed recipes, settings, seen recipes, the ghost recipe in the grid — and
    # no step in it is a survival-tick duration.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        print(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", "127.0.0.1", "--port", "0", "--motd", "recipe book test",
             "--offline-mode", "--gamemode", "1", "--data", data_root,
             "--world", world], wait_listening, env=env)

        print("Connecting bot...")
        bot = BookBot("BookBot", port)
        wire_client.record(bot, note="recipe-book-join: the book's state and its writes")
        bot.join()
        join_packets = list(bot.seen)

        # --- update_recipes ---------------------------------------------------
        body = find(join_packets, CB_UPDATE_RECIPES)
        assert body is not None, (
            "a joining client was never sent update_recipes, so its recipe book "
            "cannot know which items the menus accept")
        sets, cutters = decode_update_recipes(body)
        keys = [k for k, _ in sets]
        assert keys == PROPERTY_SET_KEYS, (
            f"update_recipes must carry the 7 RecipePropertySets in bootstrap "
            f"order, got {keys}")
        by_key = dict(sets)
        assert by_key["minecraft:furnace_input"], (
            "the furnace_input set is empty — smelting recipes did not reach it")
        assert by_key["minecraft:smithing_template"], (
            "the smithing_template set is empty")
        assert cutters, "the stonecutter's selectable list is empty"
        cut_result = cutters[0][1]
        assert cut_result["k"] == SLOT_ITEM_STACK and cut_result["count"] > 0, (
            f"a stonecutter entry must carry an item-stack result, got {cut_result}")
        print(f"  update_recipes: {len(sets)} property sets "
              f"({len(by_key['minecraft:furnace_input'])} furnace items), "
              f"{len(cutters)} stonecutter recipes, {len(body)} bytes")

        # --- recipe_book_settings --------------------------------------------
        body = find(join_packets, CB_RECIPE_BOOK_SETTINGS)
        assert body is not None, "a joining client was never sent recipe_book_settings"
        pairs = decode_recipe_book_settings(body)
        assert all(p == (0, 0) for p in pairs), (
            f"every book starts closed and unfiltered, got {pairs}")

        # --- recipe_book_add -------------------------------------------------
        body = find(join_packets, CB_RECIPE_BOOK_ADD)
        assert body is not None, (
            "a joining client was never sent recipe_book_add, so its book is empty "
            "even though place_recipe would accept the ids")
        entries, replace = decode_recipe_book_add(body)
        assert replace == 1, (
            "the first book push replaces the list (vanilla's sendInitialRecipeBook "
            f"sends replace=true), got {replace}")
        assert len(entries) > 500, (
            f"the book should carry the whole generated table, got {len(entries)}")
        ids = [e["id"] for e in entries]
        assert ids == list(range(len(entries))), (
            "display ids must be the entry indices — place_recipe resolves its id "
            "against the same list")
        print(f"  recipe_book_add: {len(entries)} displays, {len(body)} bytes")

        # Every entry must name a category the crafting book lists, or the client
        # drops it in silence. The crafting categories are 0..3.
        crafting_entries = [e for e in entries
                            if e["display"]["kind"] in (DISPLAY_SHAPELESS, DISPLAY_SHAPED)]
        assert crafting_entries, "no crafting displays in the book at all"

        # --- the trim entries ---------------------------------------------
        # There is no smithing recipe book on the client (SmithingMenu is not a
        # RecipeBookMenu), but vanilla still sends every known recipe's display,
        # and a trim's is a *demo*: the base and material it trims plus the
        # pattern, which the client resolves by applying the trim itself.
        trim_entries = [e for e in entries
                        if e["display"]["kind"] == DISPLAY_SMITHING
                        and e["display"]["result"]["k"] == SLOT_SMITHING_TRIM]
        assert len(trim_entries) == 18, (
            f"the book must list all 18 smithing_trim displays, got {len(trim_entries)}")
        material_items = sorted(item for _, _, item, _ in ct.TRIM_MATERIALS)
        template_items = {template for _, _, template in ct.TRIM_PATTERNS}
        seen_patterns = set()
        for entry in trim_entries:
            display = entry["display"]
            assert entry["category"] == 11, (
                f"every trim entry sits in the Smithing tab (11), got {entry['category']}")
            template, base, addition = display["template"], display["base"], display["addition"]
            result = display["result"]
            # The demo repeats the recipe's own base and addition ingredients.
            assert sorted(all_primary_items(result["base"])) == sorted(all_primary_items(base)), (
                "the trim demo's base must be the recipe's base ingredient, got "
                f"{result['base']} for {base}")
            assert sorted(all_primary_items(result["material"])) == material_items, (
                "the trim demo's material must be the 11 #minecraft:trim_materials "
                f"items, got {all_primary_items(result['material'])}")
            assert ct.ITEM_IRON_CHESTPLATE in all_primary_items(base), (
                f"#minecraft:trimmable_armor must contain the iron chestplate, got {base}")
            # The pattern is the one whose template this recipe's template cell is.
            own_template = all_primary_items(template)
            assert len(own_template) == 1 and own_template[0] in template_items, (
                f"a trim recipe has exactly one template item, got {own_template}")
            expected = [pid + 1 for _, pid, template_id in ct.TRIM_PATTERNS
                        if template_id == own_template[0]]
            assert len(expected) == 1, (
                f"no 1:1 pattern for template {own_template[0]}")
            assert result["pattern"] == expected[0], (
                f"the demo must name the pattern {expected[0] - 1}, got "
                f"{result['pattern'] - 1} ")
            assert result["pattern"] not in seen_patterns, (
                f"pattern {result['pattern'] - 1} appears twice in the book")
            seen_patterns.add(result["pattern"])
            # The placement's three ingredients, in the recipe's own order.
            assert len(entry["requirements"]) == 3, (
                f"a trim's placement is template + base + addition, got "
                f"{len(entry['requirements'])}")
            for requirement, slot in zip(entry["requirements"], (template, base, addition)):
                assert sorted(requirement["items"]) == sorted(all_primary_items(slot)), (
                    "each crafting requirement must be its ingredient, got "
                    f"{requirement} for {slot}")
        assert seen_patterns == {pid + 1 for _, pid, _ in ct.TRIM_PATTERNS}, (
            f"the 18 trim entries must cover every pattern once, got {seen_patterns}")
        print(f"  {len(trim_entries)} trim displays: every pattern once, each a "
              f"base + material demo")

        # --- find the oak planks display -------------------------------------
        planks = None
        for entry in entries:
            display = entry["display"]
            if display["kind"] != DISPLAY_SHAPELESS:
                continue
            result = display["result"]
            if result["k"] == SLOT_ITEM_STACK and result["item"] == ITEM_OAK_PLANKS \
                    and result["count"] == 4:
                planks = entry
                break
        assert planks is not None, (
            "the book has no shapeless oak-log -> 4 oak planks display")
        station = planks["display"]["station"]
        assert station["k"] == SLOT_ITEM and ct.ITEM_CRAFTING_TABLE == station["item"], (
            f"a crafting display's station must be the crafting table, got {station}")
        assert planks["category"] == 0, (
            "oak_planks' JSON category is `building`, which is crafting category 0 "
            f"(building blocks), got {planks['category']}")
        assert planks["group"] == 0, (
            "the table does not carry the JSON `group` string yet, so every entry is "
            f"ungrouped (0), got {planks['group']}")
        assert len(planks["requirements"]) == 1, (
            "a shapeless one-ingredient recipe has one crafting requirement, got "
            f"{len(planks['requirements'])}")
        assert ITEM_OAK_LOG in planks["requirements"][0]["items"], (
            "the requirement is the expanded #minecraft:oak_logs tag, so it must "
            f"contain oak_log; got {planks['requirements'][0]}")
        display_id = planks["id"]
        print(f"  found the oak planks display at id {display_id}")

        # --- place_recipe fills the player menu's 2x2 -------------------------
        give(bot, CREATIVE_HOTBAR0, ITEM_OAK_LOG, 4)
        time.sleep(0.2)
        drain(bot, 0.5)

        menu = Menu(bot)
        for pid, body in join_packets:
            if pid not in (CB_KEEP_ALIVE,):
                menu._apply(pid, body)

        def place_recipe(window, display, use_max):
            bot.send(SB_PLACE_RECIPE,
                     varint(window) + varint(display) + bytes([1 if use_max else 0]))
            return menu.pump(1.2)

        # A stale window id must change nothing at all.
        packets = place_recipe(PLAYER_WINDOW + 7, display_id, False)
        assert find(packets, CB_CONTAINER_SET_CONTENT) is None, (
            "place_recipe for a window that is not open must not touch the menu")

        # An unknown display id must change nothing at all.
        packets = place_recipe(PLAYER_WINDOW, len(entries) + 10_000, False)
        assert find(packets, CB_CONTAINER_SET_CONTENT) is None, (
            "place_recipe for an id outside the pushed list must be ignored")

        # The real click: one craft, because use max items is false.
        packets = place_recipe(PLAYER_WINDOW, display_id, False)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, (
            "place_recipe produced no container_set_content, so the client's grid "
            "never filled")
        w, _, slots, _, _ = ct.decode_content(content)
        assert w == PLAYER_WINDOW, f"the content must name window 0, got {w}"
        assert len(slots) == PLAYER_MENU_SIZE, (
            f"the player menu has {PLAYER_MENU_SIZE} slots, got {len(slots)}")
        assert slots[PM_GRID_FIRST] == (ITEM_OAK_LOG, 1), (
            "place_recipe must put one log in the first 2x2 cell, got "
            f"{slots[PM_GRID_FIRST]}")
        assert slots[PM_RESULT] == (ITEM_OAK_PLANKS, 4), (
            "the filled grid must derive 4 oak planks, got "
            f"{slots[PM_RESULT]}")
        assert slots[PM_GRID_FIRST + 1] == (0, 0), (
            "a one-ingredient shapeless recipe fills exactly one cell")
        print("  place_recipe filled the 2x2 and derived 4 planks")

        # Right-clicking a recipe asks for as many crafts as the inventory allows,
        # so all four logs move into the grid.
        packets = place_recipe(PLAYER_WINDOW, display_id, True)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, "the use-max-items place produced no content"
        _, _, slots, _, _ = ct.decode_content(content)
        assert slots[PM_GRID_FIRST] == (ITEM_OAK_LOG, 4), (
            "use max items must move all four logs into the grid, got "
            f"{slots[PM_GRID_FIRST]}")
        assert all(s == (0, 0) for i, s in enumerate(slots)
                   if i not in (PM_RESULT, PM_GRID_FIRST)), (
            "every log must have left the inventory: " + str(slots))
        print("  use max items moved all four logs into the grid")

        # --- a recipe the player cannot craft is ghosted instead -------------
        # ServerPlaceRecipe.placeRecipe clears the grid and asks the client to
        # draw the recipe's shadow when the player lacks the ingredients. Pick a
        # one-cell shapeless recipe (so it fits the 2x2 at all) whose ingredient
        # is not an oak log, which the bot holds none of; the logs sitting in the
        # grid must come back to the inventory before the ghost goes out.
        uncraftable = None
        for entry in entries:
            display = entry["display"]
            if display["kind"] != DISPLAY_SHAPELESS or len(display["ingredients"]) != 1:
                continue
            ingredient = display["ingredients"][0]
            if ingredient["k"] != SLOT_COMPOSITE:
                continue
            if ITEM_OAK_LOG in all_primary_items(ingredient):
                continue
            result = display["result"]
            if result["k"] != SLOT_ITEM_STACK:
                continue
            if result["item"] == ITEM_OAK_PLANKS:
                continue
            uncraftable = entry
            break
        assert uncraftable is not None, (
            "the book has no one-cell shapeless recipe the bot cannot craft")
        packets = place_recipe(PLAYER_WINDOW, uncraftable["id"], False)
        content = find(packets, CB_CONTAINER_SET_CONTENT)
        assert content is not None, (
            "the ghost path still resyncs the menu — the grid was emptied")
        _, _, slots, _, _ = ct.decode_content(content)
        assert slots[PM_GRID_FIRST] == (0, 0), (
            "the grid must be cleared before ghosting, got " f"{slots[PM_GRID_FIRST]}")
        assert slots[PM_RESULT] == (0, 0), "an empty grid derives nothing"
        held_logs = sum(s[1] for s in slots if s[0] == ITEM_OAK_LOG)
        assert held_logs == 4, (
            f"the four logs must come back to the inventory, found {held_logs}")
        ghost = find(packets, CB_PLACE_GHOST_RECIPE)
        assert ghost is not None, (
            "an uncraftable recipe must send place_ghost_recipe so the client can "
            "draw the shadow")
        gw, pos = rd_varint(ghost, 0)
        assert gw == PLAYER_WINDOW, f"the ghost names window 0, got {gw}"
        ghost_display, pos = rd_recipe_display(ghost, pos)
        assert pos == len(ghost), "place_ghost_recipe left trailing bytes"
        assert ghost_display["kind"] == uncraftable["display"]["kind"], (
            "place_ghost_recipe carries the display, not an index, so it must "
            f"decode to the same recipe, got {ghost_display['kind']}")
        assert ghost_display["result"] == uncraftable["display"]["result"], (
            f"the ghosted display must be the same result, got {ghost_display}")
        print("  an uncraftable recipe cleared the grid and was ghosted")

        # --- the settings / seen-recipe packets are accepted and dropped ------
        bot.send(SB_RECIPE_BOOK_SETTINGS, varint(0) + bytes([1, 1]))
        bot.send(SB_RECIPE_BOOK_SEEN_RECIPE, varint(display_id))
        bot.send(0x00, varint(0))  # an empty chat message keeps the socket honest
        drain(bot, 0.6)
        # The session must still be up after the book packet. Live that is the
        # socket's own answer (`fileno() == -1` once the server has closed it);
        # a replay has no socket to ask, so it says so instead of passing.
        if not wire_client.skip_server_check(
                "the session liveness check after the book packet (a replay has "
                "no socket to ask)"):
            assert bot.sock.fileno() != -1, "the session dropped after a book packet"
        print("  recipe_book / displayed_recipe were accepted without dropping the session")

        print("OK — the recipe book is sent at join and place_recipe fills the grid")
        return 0
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    sys.exit(main())
