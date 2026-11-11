#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Live player-menu tracking used by the first-person preview.

The server sends the 46-slot player menu with container_set_content (0x12) at
join, then one container_set_slot (0x14) per mutation. This test builds those
packets directly and checks that LiveInventory replaces, updates and clears
the same menu slots the Java client would show.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

import join_test as JT  # noqa: E402
import player_session as PS  # noqa: E402


def stack(item: int, count: int) -> bytes:
    if count <= 0:
        return b"\x00"
    return (JT.varint(count) + JT.varint(item) +
            JT.varint(0) + JT.varint(0))


def content(slots: list[tuple[int, int]], state: int = 5) -> bytes:
    body = JT.varint(0) + JT.varint(state) + JT.varint(len(slots))
    for item, count in slots:
        body += stack(item, count)
    return body + b"\x00"  # the carried stack


def stack_with_components(item: int, count: int,
                          components: tuple[tuple[int, bytes], ...]) -> bytes:
    out = JT.varint(count) + JT.varint(item)
    out += JT.varint(len(components)) + JT.varint(0)
    for component, value in components:
        out += JT.varint(component) + value
    return out


def slot_update(slot: int, item: int, count: int, state: int = 6,
                components: tuple[tuple[int, bytes], ...] = ()) -> bytes:
    if count <= 0:
        payload = stack(item, 0)
    else:
        payload = stack_with_components(item, count, components)
    return (JT.varint(0) + JT.varint(state) +
            slot.to_bytes(2, "big", signed=True) + payload)


def nbt_text(text: str) -> bytes:
    raw = text.encode()
    return b"\x08" + struct.pack(">H", len(raw)) + raw


def potion_contents(potion: int = 0, name: str | None = None) -> bytes:
    out = b"\x01" + JT.varint(potion) + b"\x00" + JT.varint(0)
    if name is None:
        out += b"\x00"
    else:
        raw = name.encode()
        out += b"\x01" + JT.varint(len(raw)) + raw
    return out


def main() -> int:
    stone = PS.ITEM_IDS["minecraft:stone"]
    log = PS.ITEM_IDS["minecraft:oak_log"]
    empty: list[tuple[int, int]] = [(0, 0)] * 46
    empty[36] = (stone, 7)
    empty[40] = (stone, 64)
    live = PS.LiveInventory()

    assert live.apply_content(content(empty))
    assert live.syncs == 1 and live.updates == 0
    assert live.window == 0 and live.state == 5
    assert live.named()[36]["name"] == "minecraft:stone"
    assert live.named()[36]["count"] == 7
    assert live.named()[40]["count"] == 64
    assert live.hotbar_count() == 2

    assert live.apply_slot(slot_update(36, log, 3))
    assert live.updates == 1 and live.state == 6
    assert live.named()[36]["name"] == "minecraft:oak_log"
    assert live.named()[36]["count"] == 3
    assert live.named()[40]["name"] == "minecraft:stone"

    assert live.apply_slot(slot_update(40, 0, 0))
    assert live.updates == 2
    assert 40 not in live.named()
    assert live.hotbar_count() == 1

    assert live.apply_slot(slot_update(0, 0, 0))
    assert live.updates == 3, "an empty slot update is still a real mutation"
    assert "minecraft:oak_log" in {
        value["name"] for value in live.named().values()
    }

    # The clientbound patch has no per-value lengths. A custom name is network
    # NBT; potion contents carry their own optional display name and effects.
    potion_item = PS.ITEM_IDS["minecraft:potion"]
    components = (
        (6, nbt_text("Bob")),
        (53, potion_contents(0, "Healing")),
    )
    assert live.apply_slot(slot_update(36, potion_item, 1,
                                      components=components))
    entry = live.named()[36]
    assert entry["name"] == "minecraft:potion"
    assert entry["custom_name"] == "Bob"
    assert entry["potion_contents"]["potion"] == 0
    assert entry["potion_contents"]["custom_name"] == "Healing"
    assert entry["potion_contents"]["custom_effects"] == []

    # The preview's seeded hotbar must exercise the new special icons.
    assert {"minecraft:skeleton_skull", "minecraft:red_banner",
            "minecraft:shield", "minecraft:copper_golem_statue"} <= set(PS.SAMPLE_ITEMS)

    # A different container is not the player's inventory and must not replace
    # the menu the preview is drawing.
    other = live.named()
    assert not live.apply_content(JT.varint(1) + JT.varint(9) + JT.varint(0) + b"\x00")
    assert live.named() == other
    print("live inventory: content, slot update, removal and container filtering -> ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
