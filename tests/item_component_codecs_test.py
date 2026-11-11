#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode every item component the 26.3 item registry can put on the wire.

The preview's clientbound stack reader must consume all of these without a
length prefix; one missed field would shift every following menu slot. The
fixture below mirrors the stream-codec field order of the component classes.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

import join_test as JT  # noqa: E402
import player_session as PS  # noqa: E402


def v(value: int) -> bytes:
    return JT.varint(value)


def f(value: float) -> bytes:
    return struct.pack(">f", value)


def i(value: int) -> bytes:
    return struct.pack(">i", value)


def text(value: str) -> bytes:
    raw = value.encode()
    return v(len(raw)) + raw


def nbt_text(value: str) -> bytes:
    raw = value.encode()
    return b"\x08" + struct.pack(">H", len(raw)) + raw


def potion_contents() -> bytes:
    # PotionContents: present potion 0, no colour, no effects, no name.
    return b"\x01" + v(0) + b"\x00" + v(0) + b"\x00"


def custom_model_data() -> bytes:
    return (v(1) + f(1.5) +              # floats
            v(2) + b"\x01\x00" +          # flags
            v(1) + text("variant") +      # strings
            v(1) + i(0x112233))           # colors (plain int, not varint)


def food() -> bytes:
    return v(5) + f(0.6) + b"\x01"


def tool() -> bytes:
    # Tool: one rule, then default speed, damage per block, creative flag.
    # Rule: HolderSet (count + 1, then holder id + 1), optional speed,
    # optional correct-for-drops.
    return (v(1) +
            v(2) + v(10) + b"\x01" + f(2.0) + b"\x01\x00" +
            f(1.0) + v(1) + b"\x01")


def equippable() -> bytes:
    # Equippable: slot, equip-sound holder, three optionals, five flags, then
    # the shearing-sound holder.
    return (v(4) + v(7) +
            b"\x00" + b"\x01" + text("test:overlay") + b"\x00" +
            b"\x00\x01\x00\x01\x00" + v(8))


def main() -> int:
    stone = PS.ITEM_IDS["minecraft:stone"]
    components = (
        (1, v(16)),                    # max_stack_size
        (2, v(250)),                   # max_damage
        (3, v(7)),                     # damage
        (6, nbt_text("Named Stack")),  # custom_name
        (13, v(1) + v(4) + v(2)),      # enchantments
        (17, custom_model_data()),     # custom_model_data
        (23, food()),                  # food
        (28, tool()),                  # tool
        (32, equippable()),            # equippable
        (53, potion_contents()),       # potion_contents
        (58, v(2) + v(3)),             # trim: material 1, pattern 2
    )
    body = v(1) + v(stone) + v(len(components)) + v(0)
    for component, value in components:
        body += v(component) + value

    stack, pos = PS.read_stack(body, 0)
    assert pos == len(body), (pos, len(body))
    assert stack["item"] == stone and stack["count"] == 1
    assert stack["max_stack_size"] == 16
    assert stack["max_damage"] == 250
    assert stack["damage"] == 7
    assert stack["custom_name"] == "Named Stack"
    assert stack["enchantments"] == {4: 2}
    assert stack["custom_model_data"] == {
        "floats": [1.5], "flags": [True, False],
        "strings": ["variant"], "colors": [0x112233],
    }
    assert stack["food"]["nutrition"] == 5
    assert abs(stack["food"]["saturation"] - 0.6) < 1e-6
    assert stack["food"]["can_always_eat"] is True
    assert stack["tool"]["rules"] == [{
        "blocks": {"holders": [9]}, "speed": 2.0, "correct_for_drops": False,
    }]
    assert stack["tool"]["default_mining_speed"] == 1.0
    assert stack["tool"]["damage_per_block"] == 1
    assert stack["tool"]["can_destroy_blocks_in_creative"] is True
    assert stack["equippable"]["slot"] == 4
    assert stack["equippable"]["equip_sound"] == {"sound": 6}
    assert stack["equippable"]["camera_overlay"] == "test:overlay"
    assert stack["equippable"]["dispensable"] is False
    assert stack["equippable"]["swappable"] is True
    assert stack["equippable"]["damage_on_hurt"] is False
    assert stack["equippable"]["equip_on_interact"] is True
    assert stack["equippable"]["can_be_sheared"] is False
    assert stack["equippable"]["shearing_sound"] == {"sound": 7}
    assert stack["potion_contents"]["potion"] == 0
    assert stack["trim"] == (1, 2)

    # The seeded sample sends a real custom-name/potion patch, and the server
    # echoes unknown components verbatim so the preview can render the label.
    assert PS.SAMPLE_ITEMS[0] == "minecraft:potion"
    assert PS.SAMPLE_DISPLAY_NAMES[0] == "Healing Draught"
    assert [cid for cid, _ in PS.SAMPLE_COMPONENTS[0]] == [6, 53]
    for hotbar, expected in {
            3: 32, 5: 17, 7: 28, 8: 23}.items():
        ids = [cid for cid, _ in PS.SAMPLE_COMPONENTS[hotbar]]
        assert ids == [expected], (hotbar, ids)
    seeded = PS.creative_stack("minecraft:potion", 1, 0)
    assert nbt_text("Healing Draught") in seeded

    print("item components: all 11 clientbound codecs -> ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
