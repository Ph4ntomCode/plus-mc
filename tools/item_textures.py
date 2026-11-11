#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Resolve Java item models and render item icons / an inventory sheet.

The client jar's `assets/minecraft/items/<name>.json` is the item-model
definition. Block items point at `block/<name>` and are rendered from their
real model boxes with an orthographic isometric ray; generated and handheld
items use their `layer0` (and further layer) sprites.

This is still a debug preview, not a replacement for Minecraft's item renderer:
display transforms, glint and GUI lighting are not modelled. Entity-backed
items (chests, shulkers, pots, heads, banners, the shield and copper-golem
statues) use their entity textures and small hand-built box assemblies. The
item/texture coverage is measured by `item_texture_parity_test.py` against the
jar's assets.
"""

from __future__ import annotations

import math
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from block_textures import BlockTextures, ElementBox, FaceLayer, ModelPart


def _strip_namespace(name: str) -> str:
    return name[len("minecraft:"):] if name.startswith("minecraft:") else name


def item_table(repo: Path) -> list[tuple[str, int]]:
    """The generated item registry, in id order."""
    text = (repo / "src/world/item_registry.h").read_text()
    rows = re.findall(r'\{"(minecraft:[^"]+)",\s*(\d+),', text)
    return [(name, int(item_id)) for name, item_id in rows]


def _normalize(v):
    n = math.sqrt(sum(c * c for c in v))
    return tuple(c / n for c in v) if n else (0.0, 0.0, 0.0)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


# Vanilla DyeColor RGB values, used to tint the shared white banner cloth.
_DYE_COLORS = {
    "white": (249, 255, 254), "orange": (249, 128, 29),
    "magenta": (199, 78, 189), "light_blue": (58, 179, 218),
    "yellow": (254, 216, 61), "lime": (128, 199, 31),
    "pink": (243, 139, 170), "gray": (71, 79, 82),
    "light_gray": (157, 157, 151), "cyan": (21, 137, 145),
    "purple": (121, 42, 172), "blue": (53, 57, 157),
    "brown": (114, 71, 40), "green": (84, 109, 27),
    "red": (161, 39, 34), "black": (20, 21, 25),
}

_HEAD_TEXTURES = {
    "skeleton": "entity/skeleton/skeleton",
    "wither_skeleton": "entity/skeleton/wither_skeleton",
    "zombie": "entity/zombie/zombie",
    "creeper": "entity/creeper/creeper",
    "dragon": "entity/enderdragon/dragon",
    "piglin": "entity/piglin/piglin",
    "player": "entity/player/wide/steve",
}


class ItemTextures:
    def __init__(self, blocks: BlockTextures):
        self.blocks = blocks
        self._icons: dict[tuple[str, int], Image.Image] = {}
        self._model_refs: dict[str, str | None] = {}

    def _definition(self, name: str) -> dict | None:
        return self.blocks._read_json("items", name)

    @classmethod
    def _first_model(cls, value) -> str | None:
        # Only follow model-bearing branches. A condition/select definition also
        # carries component/property/type strings, which are not model paths.
        if isinstance(value, str):
            return value if "/" in value else None
        if isinstance(value, dict):
            for key in ("model", "base", "models", "entries", "cases",
                        "fallback", "on_false", "on_true"):
                if key not in value:
                    continue
                found = cls._first_model(value[key])
                if found:
                    return found
            return None
        if isinstance(value, list):
            for child in value:
                found = cls._first_model(child)
                if found:
                    return found
        return None

    def model_ref(self, name: str) -> str | None:
        name = _strip_namespace(name)
        if name in self._model_refs:
            return self._model_refs[name]
        doc = self._definition(name)
        ref = self._first_model(doc) if doc is not None else None
        candidates = [ref, f"item/{name}", f"block/{name}"]
        chosen = None
        for candidate in candidates:
            if not isinstance(candidate, str):
                continue
            model = self.blocks.model(candidate)
            if model.elements or model.textures:
                chosen = candidate
                break
        self._model_refs[name] = chosen
        return chosen

    def _special_node(self, name: str) -> dict | None:
        """Follow a modern item definition to its special-model leaf."""
        doc = self._definition(name)
        node = doc.get("model") if isinstance(doc, dict) else None
        for _ in range(16):
            if not isinstance(node, dict):
                return None
            kind = node.get("type")
            if kind == "minecraft:special":
                return node
            if kind == "minecraft:select":
                cases = node.get("cases") or []
                node = node.get("fallback") or (cases[0].get("model") if cases else None)
            elif kind == "minecraft:condition":
                node = node.get("on_false") or node.get("on_true")
            elif kind == "minecraft:range_dispatch":
                entries = node.get("entries") or []
                node = entries[0].get("model") if entries else None
            else:
                return None
        return None

    def special_kind(self, name: str) -> str | None:
        """The special renderer kind, or None for ordinary item models."""
        node = self._special_node(name)
        model = node.get("model") if node else None
        return model.get("type") if isinstance(model, dict) else None

    @staticmethod
    def _entity_face(image: Image.Image, region: tuple[int, int, int, int]) -> FaceLayer:
        x0, y0, x1, y1 = region
        x0 = max(0, min(image.width, x0))
        x1 = max(x0 + 1, min(image.width, x1))
        y0 = max(0, min(image.height, y0))
        y1 = max(y0 + 1, min(image.height, y1))
        uv = (x0 / image.width * 16.0, y0 / image.height * 16.0,
              x1 / image.width * 16.0, y1 / image.height * 16.0)
        return FaceLayer("entity", image, uv)

    @classmethod
    def _entity_box(cls, lo, hi, image: Image.Image,
                    side: tuple[int, int, int, int],
                    cap: tuple[int, int, int, int]) -> ElementBox:
        side_layer = cls._entity_face(image, side)
        cap_layer = cls._entity_face(image, cap)
        return ElementBox(tuple(lo), tuple(hi), {
            "down": cap_layer, "up": cap_layer,
            "north": side_layer, "south": side_layer,
            "west": side_layer, "east": side_layer,
        })

    @staticmethod
    def _tint(image: Image.Image, color: tuple[int, int, int]) -> Image.Image:
        """Multiply an RGBA sprite by a dye colour while preserving alpha."""
        src = image.convert("RGBA")
        out = Image.new("RGBA", src.size)
        for y in range(src.height):
            for x in range(src.width):
                r, g, b, a = src.getpixel((x, y))
                out.putpixel((x, y), (r * color[0] // 255, g * color[1] // 255,
                                      b * color[2] // 255, a))
        return out

    @classmethod
    def _head_box(cls, image: Image.Image, size: int) -> ElementBox:
        """The vanilla head cube's six 8x8 texture faces."""
        regions = {
            "down": (16, 0, 24, 8), "up": (8, 0, 16, 8),
            "north": (8, 8, 16, 16), "south": (24, 8, 32, 16),
            "west": (0, 8, 8, 16), "east": (16, 8, 24, 16),
        }
        return ElementBox((0, 0, 0), (size, size, size), {
            face: cls._entity_face(image, region) for face, region in regions.items()
        })

    def _head_icon(self, spec: dict, kind: str, size: int) -> Image.Image | None:
        if kind == "minecraft:player_head":
            head_kind = "player"
        else:
            head_kind = str(spec.get("kind", "skeleton")).replace("minecraft:", "")
        ref = _HEAD_TEXTURES.get(head_kind)
        if ref is None:
            return None
        image = self.blocks.texture(ref)
        if image is None:
            return None
        if head_kind == "dragon":
            # The dragon sheet is 256x256 and its head is a 16px cube; the
            # ordinary 8px skin layout would sample the neck instead.
            regions = {
                "down": (16, 16, 32, 32), "up": (0, 16, 16, 32),
                "north": (0, 0, 16, 16), "south": (32, 0, 48, 16),
                "west": (16, 0, 32, 16), "east": (48, 0, 64, 16),
            }
            box = ElementBox((0, 0, 0), (16, 16, 16), {
                face: self._entity_face(image, region) for face, region in regions.items()
            })
        else:
            box = self._head_box(image, 8)
        return self._render_parts([ModelPart("special", (box,))], size)

    def _banner_icon(self, spec: dict, size: int) -> Image.Image | None:
        cloth = self.blocks.texture("entity/banner/base")
        wood = self.blocks.texture("entity/banner/banner_base")
        if cloth is None or wood is None:
            return None
        color = _DYE_COLORS.get(str(spec.get("color", "white")), _DYE_COLORS["white"])
        cloth = self._tint(cloth, color)
        cloth_layer = self._entity_face(cloth, (0, 0, 42, 42))
        # The pole is the brown vertical strip in the right half of banner_base.
        wood_layer = self._entity_face(wood, (38, 0, 46, 46))
        boxes = [
            ElementBox((7, 0, 7), (9, 16, 9), {face: wood_layer for face in
                       ("down", "up", "north", "south", "west", "east")}),
            ElementBox((2, 2, 6.5), (14, 14, 7.5), {face: cloth_layer for face in
                       ("down", "up", "north", "south", "west", "east")}),
        ]
        return self._render_parts([ModelPart("special", tuple(boxes))], size)

    def _shield_icon(self, size: int) -> Image.Image | None:
        # `base` is the white pattern mask; the no-pattern sprite carries the
        # vanilla wood face that a plain shield has when it has no banner dye.
        image = self.blocks.texture("entity/shield/shield_base_nopattern")
        if image is None:
            image = self.blocks.texture("entity/shield/base")
        if image is None:
            return None
        layer = self._entity_face(image, (2, 2, 12, 22))
        box = ElementBox((3, 0, 6.5), (13, 16, 7.5), {
            face: layer for face in ("down", "up", "north", "south", "west", "east")
        })
        return self._render_parts([ModelPart("special", (box,))], size)

    def _copper_golem_icon(self, spec: dict, size: int) -> Image.Image | None:
        ref = str(spec.get("texture", "textures/entity/copper_golem/copper_golem.png"))
        image = self.blocks.texture(ref)
        if image is None:
            return None
        side = (0, 0, 64, 36)
        cap = (0, 0, 32, 32)
        boxes = [
            self._entity_box((4, 10, 4), (12, 17, 12), image, side, cap),
            self._entity_box((3, 3, 3), (13, 11, 13), image, side, cap),
        ]
        pose = spec.get("pose", "standing")
        if pose == "sitting":
            boxes += [self._entity_box((3, 0, 3), (7, 3, 7), image, side, cap),
                      self._entity_box((9, 0, 3), (13, 3, 7), image, side, cap)]
        else:
            boxes += [self._entity_box((3, 0, 3), (6, 4, 8), image, side, cap),
                      self._entity_box((10, 0, 3), (13, 4, 8), image, side, cap)]
        if pose == "star":
            boxes += [self._entity_box((1, 10, 3), (4, 17, 7), image, side, cap),
                      self._entity_box((12, 10, 3), (15, 17, 7), image, side, cap)]
        else:
            boxes += [self._entity_box((1, 4, 3), (4, 11, 7), image, side, cap),
                      self._entity_box((12, 4, 3), (15, 11, 7), image, side, cap)]
        boxes.append(self._entity_box((7, 9, 2), (9, 11, 4), image, side, cap))
        return self._render_parts([ModelPart("special", tuple(boxes))], size)

    def _special_icon(self, name: str, size: int) -> Image.Image | None:
        node = self._special_node(name)
        if node is None:
            return None
        spec = node.get("model") or {}
        kind = spec.get("type")
        boxes: list[ElementBox] = []
        if kind == "minecraft:head" or kind == "minecraft:player_head":
            return self._head_icon(spec, kind, size)
        if kind == "minecraft:banner":
            return self._banner_icon(spec, size)
        if kind == "minecraft:shield":
            return self._shield_icon(size)
        if kind == "minecraft:copper_golem_statue":
            return self._copper_golem_icon(spec, size)
        if kind == "minecraft:chest":
            texture = str(spec.get("texture", "normal")).replace("minecraft:", "")
            image = self.blocks.texture(f"entity/chest/{texture}")
            if image is None:
                return None
            boxes = [
                self._entity_box((1, 0, 1), (15, 9, 15), image, (0, 0, 42, 42), (0, 0, 56, 14)),
                self._entity_box((1, 9, 1), (15, 14, 15), image, (0, 0, 42, 42), (0, 0, 56, 14)),
                self._entity_box((7, 6, 0), (9, 10, 2), image, (42, 0, 56, 14), (42, 0, 56, 14)),
            ]
        elif kind == "minecraft:shulker_box":
            texture = str(spec.get("texture", "shulker")).replace("minecraft:", "")
            image = self.blocks.texture(f"entity/shulker/{texture}")
            if image is None:
                return None
            boxes = [
                self._entity_box((2, 0, 2), (14, 10, 14), image, (0, 0, 64, 32), (0, 0, 32, 32)),
                self._entity_box((1, 10, 1), (15, 16, 15), image, (0, 32, 64, 64), (0, 32, 32, 64)),
            ]
        elif kind == "minecraft:decorated_pot":
            base = self.blocks.texture("entity/decorated_pot/decorated_pot_base")
            side = self.blocks.texture("entity/decorated_pot/decorated_pot_side")
            if base is None or side is None:
                return None
            # The base sheet is 32x32; the side sheet is 16x16. Keep the pot
            # shape simple: a foot, a body and a small neck.
            boxes = [
                self._entity_box((3, 0, 3), (13, 2, 13), base, (0, 0, 32, 32), (0, 0, 32, 32)),
                self._entity_box((4, 2, 4), (12, 12, 12), side, (0, 0, 16, 16), (0, 0, 16, 16)),
                self._entity_box((5, 12, 5), (11, 16, 11), side, (0, 0, 16, 16), (0, 0, 16, 16)),
            ]
        else:
            return None
        return self._render_parts([ModelPart("special", tuple(boxes))], size)

    def model_layers(self, name: str) -> list[Image.Image]:
        ref = self.model_ref(name)
        if ref is None:
            return []
        model = self.blocks.model(ref)
        layers = []
        keys = sorted(key for key in model.textures
                      if key == "layer0" or key.startswith("layer"))
        for key in keys:
            texture_ref = self.blocks.resolved_texture(model.textures[key], model.textures)
            image = self.blocks.texture(texture_ref)
            if image is not None:
                layers.append(image)
        return layers

    def _flat_icon(self, name: str, size: int) -> Image.Image | None:
        layers = self.model_layers(name)
        if not layers:
            fallback = self.blocks.texture(f"item/{_strip_namespace(name)}")
            if fallback is None:
                fallback = self.blocks.texture(f"block/{_strip_namespace(name)}")
            layers = [fallback] if fallback is not None else []
        if not layers:
            return None
        out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        resample = getattr(Image, "Resampling", Image).NEAREST
        for layer in layers:
            out.alpha_composite(layer.resize((size, size), resample))
        return out

    def _render_parts(self, parts: list[ModelPart], size: int) -> Image.Image | None:
        elements = [box for part in parts for box in part.elements]
        if not elements:
            return None
        lo = [min(box.lo[i] for box in elements) for i in range(3)]
        hi = [max(box.hi[i] for box in elements) for i in range(3)]
        center = tuple((lo[i] + hi[i]) / 2.0 for i in range(3))
        extent = max(hi[i] - lo[i] for i in range(3)) or 16.0
        half = extent * 0.70

        # Camera above +X/+Z, looking toward the centre. The basis is the
        # standard isometric view used by GUI block icons (not the in-hand
        # display transform, which is intentionally out of scope here).
        direction = _normalize((-1.0, -1.0, -1.0))
        right = _normalize(_cross((0.0, 1.0, 0.0), direction))
        up = _normalize(_cross(direction, right))
        out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        for py in range(size):
            sy = (0.5 - (py + 0.5) / size) * 2.0
            for px in range(size):
                sx = ((px + 0.5) / size - 0.5) * 2.0
                origin = tuple(
                    center[i] - direction[i] * 32.0 +
                    right[i] * sx * half + up[i] * sy * half
                    for i in range(3))
                hit = self.blocks.surface_hit_parts(parts, origin, direction)
                if hit is not None:
                    out.putpixel((px, py), hit.texel)
        return out

    def _model_icon(self, name: str, size: int) -> Image.Image | None:
        ref = self.model_ref(name)
        if ref is None:
            return None
        model = self.blocks.model(ref)
        if not model.elements:
            return None
        parts = self.blocks.parts_for_model(ref)
        if not parts.elements:
            return None
        return self._render_parts([parts], size)

    def icon(self, name: str, size: int = 16) -> Image.Image | None:
        key = (_strip_namespace(name), size)
        if key in self._icons:
            return self._icons[key]
        special_icon = self._special_icon(name, size)
        image = special_icon if special_icon is not None else self._model_icon(name, size)
        if image is None:
            image = self._flat_icon(name, size)
        if image is not None:
            self._icons[key] = image
        return image

    @staticmethod
    def display_name(name: str) -> str:
        text = _strip_namespace(name).replace("_", " ")
        return text.title() if text else ""

    def render_hotbar(self, slots: dict[int, dict | None], selected: int = 0) -> Image.Image:
        """Java hotbar plus the selected stack's name, for the HUD overlay."""
        width, bar_height = 182, 22
        image = Image.new("RGBA", (width, bar_height + 12), (0, 0, 0, 0))
        background = self.blocks.texture("gui/sprites/hud/hotbar")
        if background is not None:
            image.alpha_composite(background.convert("RGBA"), (0, 12))
        else:
            draw = ImageDraw.Draw(image)
            draw.rectangle((0, 12, width - 1, 12 + bar_height - 1),
                           fill=(32, 32, 32, 220), outline=(160, 160, 160, 220))
        try:
            font = ImageFont.load_default()
        except OSError:
            font = None
        draw = ImageDraw.Draw(image)
        for slot in range(9):
            stack = slots.get(36 + slot)
            if stack is None and 0 <= slot < 9:
                stack = slots.get(slot)
            if not stack or not stack.get("name"):
                continue
            icon = self.icon(stack["name"], 16)
            if icon is not None:
                image.alpha_composite(icon, (3 + slot * 20, 15))
            count = int(stack.get("count", 1))
            if count > 1 and font is not None:
                text = str(count)
                tx, ty = 20 + slot * 20 - len(text) * 5, 25
                draw.text((tx + 1, ty + 1), text, fill=(30, 30, 30, 255), font=font)
                draw.text((tx, ty), text, fill=(255, 255, 255, 255), font=font)
        selection = self.blocks.texture("gui/sprites/hud/hotbar_selection")
        if 0 <= selected < 9:
            if selection is not None:
                image.alpha_composite(selection.convert("RGBA"), (selected * 20, 12))
            else:
                draw.rectangle((selected * 20, 12, selected * 20 + 23, 34),
                               outline=(255, 255, 255, 255))
        stack = slots.get(36 + selected) or slots.get(selected)
        if stack and stack.get("name") and font is not None:
            text = stack.get("custom_name") or self.display_name(stack["name"])
            text = re.sub(r"\u00a7.", "", str(text))
            tw = draw.textlength(text, font=font)
            tx = (width - int(tw)) // 2
            draw.text((tx + 1, 1), text, fill=(20, 20, 20, 255), font=font)
            draw.text((tx, 0), text, fill=(255, 255, 255, 255), font=font)
        return image

    def render_inventory(self, slots: dict[int, dict | None], selected: int = 0,
                         size: tuple[int, int] = (176, 166)) -> Image.Image:
        background = self.blocks.texture("gui/container/inventory")
        if background is not None:
            image = background.convert("RGBA")
            if image.size != size:
                image = image.crop((0, 0, *size))
        else:
            image = Image.new("RGBA", size, (198, 198, 198, 255))
            draw = ImageDraw.Draw(image)
            draw.rectangle((0, 0, size[0] - 1, size[1] - 1), outline=(30, 30, 30))
        draw = ImageDraw.Draw(image)
        positions = {}
        for row in range(3):
            for col in range(9):
                positions[9 + row * 9 + col] = (8 + col * 18, 84 + row * 18)
        for col in range(9):
            positions[36 + col] = (8 + col * 18, 142)
        try:
            font = ImageFont.load_default()
        except OSError:
            font = None
        for slot, stack in slots.items():
            if not stack or not stack.get("name"):
                continue
            pos = positions.get(slot)
            if pos is None:
                continue
            icon = self.icon(stack["name"], 16)
            if icon is not None:
                image.alpha_composite(icon, (pos[0] + 1, pos[1] + 1))
            count = int(stack.get("count", 1))
            if count > 1 and font is not None:
                text = str(count)
                tx, ty = pos[0] + 20 - len(text) * 5, pos[1] + 10
                draw.text((tx + 1, ty + 1), text, fill=(30, 30, 30, 255), font=font)
                draw.text((tx, ty), text, fill=(255, 255, 255, 255), font=font)
        if 0 <= selected < 9:
            x, y = positions[36 + selected]
            draw.rectangle((x, y, x + 17, y + 17), outline=(255, 255, 255, 255))
        return image

    def render_container(self, slots: dict[int, dict | None], rows: int = 3,
                         size: tuple[int, int] | None = None) -> Image.Image:
        """Draw a chest-like container screen from real GUI and item textures."""
        if size is None:
            size = (176, 168 if rows == 3 else 222)
        background = self.blocks.texture("gui/container/generic_54")
        if background is not None:
            image = background.convert("RGBA").crop((0, 0, *size))
        else:
            image = Image.new("RGBA", size, (198, 198, 198, 255))
            ImageDraw.Draw(image).rectangle((0, 0, size[0] - 1, size[1] - 1),
                                            outline=(30, 30, 30))
        draw = ImageDraw.Draw(image)
        positions = {}
        for row in range(rows):
            for col in range(9):
                positions[row * 9 + col] = (8 + col * 18, 18 + row * 18)
        player_y = 84 if rows == 3 else 138
        for row in range(3):
            for col in range(9):
                positions[rows * 9 + row * 9 + col] = (8 + col * 18, player_y + row * 18)
        for col in range(9):
            positions[rows * 9 + 27 + col] = (8 + col * 18, player_y + 58)
        try:
            font = ImageFont.load_default()
        except OSError:
            font = None
        for slot, stack in slots.items():
            if not stack or not stack.get("name"):
                continue
            pos = positions.get(slot)
            if pos is None:
                continue
            icon = self.icon(stack["name"], 16)
            if icon is not None:
                image.alpha_composite(icon, (pos[0] + 1, pos[1] + 1))
            count = int(stack.get("count", 1))
            if count > 1 and font is not None:
                text = str(count)
                tx, ty = pos[0] + 20 - len(text) * 5, pos[1] + 10
                draw.text((tx + 1, ty + 1), text, fill=(30, 30, 30, 255), font=font)
                draw.text((tx, ty), text, fill=(255, 255, 255, 255), font=font)
        return image

    def coverage(self, names: list[str]) -> dict[str, int]:
        total = len(names)
        models = 0
        icons = 0
        for name in names:
            if self.model_ref(name):
                models += 1
            if self.icon(name, 16) is not None:
                icons += 1
        return {"total": total, "models": models, "icons": icons}
