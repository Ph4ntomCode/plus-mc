#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Resolve Minecraft Java block models and textures from a client jar.

+MC is a server: it sends block-state ids, not textures. This module is for the
debug preview (`player_session.py`) and its parity test. It reads the vanilla
client jar's blockstate JSON, resolves the model parent chain and samples the
referenced block PNGs, so the preview can draw what a Java client draws instead
of the old flat-colour fallback.

The source is Mojang data and is intentionally not shipped here. It can be a
client jar (`PLUSMC_CLIENT_JAR`, or a `client-*.jar` under
`data/vanilla/_cache/`) or the extracted `data/vanilla/<version>/assets` tree
prepared by `vanilla_assets.py` (`PLUSMC_ASSETS`).
"""

from __future__ import annotations

import io
import json
import math
import re
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

from PIL import Image

FACES = ("down", "up", "north", "south", "west", "east")
_FULL_UV = (0.0, 0.0, 16.0, 16.0)


def _strip_prefix(value: str, prefix: str) -> str:
    return value[len(prefix):] if value.startswith(prefix) else value


def _strip_suffix(value: str, suffix: str) -> str:
    return value[:-len(suffix)] if suffix and value.endswith(suffix) else value


def parse_state(state: str) -> tuple[str, dict[str, str]]:
    """Split ``minecraft:grass_block[snowy=false]`` into name and properties."""
    name, _, rest = state.partition("[")
    name = _strip_prefix(name, "minecraft:")
    props: dict[str, str] = {}
    if rest:
        for item in rest.rstrip("]").split(","):
            key, _, value = item.partition("=")
            if key:
                props[key] = value
    return name, props


def _bare_name(name: str) -> str:
    return _strip_prefix(name, "minecraft:")


@dataclass(frozen=True)
class FaceLayer:
    texture: str
    image: Image.Image
    uv: tuple[float, float, float, float] = _FULL_UV
    rotation: int = 0
    tintindex: int = -1


@dataclass(frozen=True)
class ElementBox:
    lo: tuple[float, float, float]
    hi: tuple[float, float, float]
    faces: dict[str, FaceLayer]


@dataclass(frozen=True)
class ModelPart:
    model: str
    elements: tuple[ElementBox, ...]
    rot_x: float = 0.0
    rot_y: float = 0.0
    uvlock: bool = False


@dataclass(frozen=True)
class SurfaceHit:
    """A ray hit in block-local space; distance is in world units."""

    distance: float
    face: str
    texel: tuple[int, int, int, int]


@dataclass
class Appearance:
    model: str
    faces: dict[str, list[FaceLayer]]
    textures: dict[str, object]
    parts: list[ModelPart] = field(default_factory=list)


@dataclass
class _Model:
    textures: dict[str, object]
    elements: list[dict]


class BlockTextures:
    """A read-only view of a client jar or an extracted asset tree."""

    def __init__(self, source: str | Path):
        self.source = Path(source)
        self._zip: zipfile.ZipFile | None = None
        self._root: Path | None = None
        self._names: set[str] | None = None
        if self.source.is_dir():
            self._root = self.source
            if (self._root / "assets" / "minecraft").is_dir():
                self._root = self._root / "assets"
            if not (self._root / "minecraft").is_dir():
                raise ValueError(f"not a client asset tree: {self.source}")
        elif self.source.is_file():
            self._zip = zipfile.ZipFile(self.source)
            self._names = set(self._zip.namelist())
        else:
            raise FileNotFoundError(self.source)
        self._models: dict[str, _Model] = {}
        self._blockstates: dict[str, dict | None] = {}
        self._appearances: dict[str, Appearance] = {}
        self._textures: dict[str, Image.Image | None] = {}

    def close(self) -> None:
        if self._zip is not None:
            self._zip.close()

    def _asset_path(self, asset: str) -> Path | None:
        assert self._root is not None
        relative = asset[len("assets/"):] if asset.startswith("assets/") else asset
        path = self._root / relative
        return path if path.is_file() else None

    def _has_asset(self, asset: str) -> bool:
        if self._zip is not None:
            assert self._names is not None
            return asset in self._names
        return self._asset_path(asset) is not None

    def _read_asset(self, asset: str) -> bytes:
        if self._zip is not None:
            return self._zip.read(asset)
        path = self._asset_path(asset)
        if path is None:
            raise KeyError(asset)
        return path.read_bytes()

    def _read_json(self, kind: str, ref: str) -> dict | None:
        path = _bare_name(ref)
        path = _strip_suffix(path, ".json") + ".json"
        asset = f"assets/minecraft/{kind}/{path}"
        if not self._has_asset(asset):
            return None
        try:
            return json.loads(self._read_asset(asset))
        except (ValueError, KeyError, OSError):
            return None

    def _blockstate(self, name: str) -> dict | None:
        name = _bare_name(name)
        if name not in self._blockstates:
            self._blockstates[name] = self._read_json("blockstates", name)
        return self._blockstates[name]

    def has_blockstate(self, name: str) -> bool:
        """Whether this source carries the block's state file."""
        return self._blockstate(name) is not None

    def model(self, ref: str) -> _Model:
        ref = _bare_name(ref)
        if ref in self._models:
            return self._models[ref]
        data = self._read_json("models", ref)
        if data is None:
            empty = _Model({}, [])
            self._models[ref] = empty
            return empty
        parent = self.model(data["parent"]) if data.get("parent") else _Model({}, [])
        textures = dict(parent.textures)
        # Texture values can be strings or modern sprite objects; keep them
        # intact until _texture_ref resolves the "#name" indirection.
        textures.update({str(k): v for k, v in (data.get("textures") or {}).items()})
        elements = data.get("elements") if "elements" in data else parent.elements
        model = _Model(textures, elements or [])
        self._models[ref] = model
        return model

    def _texture_ref(self, value: object, textures: dict[str, str]) -> str | None:
        # Modern client models may spell a texture as an object
        # ({"sprite": "minecraft:block/glass"}) rather than a string.
        seen: set[str] = set()
        while True:
            if isinstance(value, dict):
                value = value.get("sprite") or value.get("texture")
            if not isinstance(value, str) or not value:
                return None
            if not value.startswith("#"):
                return _bare_name(value)
            key = value[1:]
            if key in seen or key not in textures:
                return None
            seen.add(key)
            value = textures[key]

    def resolved_texture(self, value: object,
                         textures: dict[str, object]) -> str | None:
        """Resolve a model texture value, including its #name indirection."""
        return self._texture_ref(value, textures)

    def texture(self, ref: str | None) -> Image.Image | None:
        ref = self._texture_ref(ref, {})
        if not ref:
            return None
        if ref in self._textures:
            return self._textures[ref]
        path = _strip_suffix(ref, ".png") + ".png"
        if not path.startswith("textures/"):
            path = "textures/" + path
        asset = "assets/minecraft/" + path
        image = None
        if self._has_asset(asset):
            try:
                image = Image.open(io.BytesIO(self._read_asset(asset))).convert("RGBA")
            except (OSError, ValueError, KeyError):
                image = None
        self._textures[ref] = image
        return image

    @staticmethod
    def _variant_matches(key: str, props: dict[str, str]) -> bool:
        if not key:
            return True
        for item in key.split(","):
            name, _, value = item.partition("=")
            if props.get(name) != value:
                return False
        return True

    @staticmethod
    def _when_matches(when: object, props: dict[str, str]) -> bool:
        if not when:
            return True
        if not isinstance(when, dict):
            return False
        if "OR" in when:
            return any(BlockTextures._when_matches(v, props) for v in when["OR"])
        if "AND" in when:
            return all(BlockTextures._when_matches(v, props) for v in when["AND"])
        for key, values in when.items():
            if key in ("OR", "AND"):
                continue
            if props.get(key) not in str(values).split("|"):
                return False
        return True

    @staticmethod
    def _model_entries(value: object) -> Iterable[dict]:
        if isinstance(value, list):
            for entry in value:
                if isinstance(entry, dict):
                    yield entry
        elif isinstance(value, dict):
            yield value

    def _state_models(self, name: str, props: dict[str, str]) -> list[dict]:
        state = self._blockstate(name)
        if state is None:
            return [{"model": f"block/{name}"}]
        variants = state.get("variants")
        if isinstance(variants, dict):
            matches = [(key, value) for key, value in variants.items()
                       if self._variant_matches(key, props)]
            if not matches:
                # A state whose properties are not listed still needs a model;
                # the empty variant is the conventional catch-all.
                matches = [("", variants[""])] if "" in variants else []
            if not matches:
                return []
            matches.sort(key=lambda item: len(item[0]), reverse=True)
            # A variant value is a weighted list of alternative models: one is
            # chosen per placement, not all of them. The first is deterministic
            # and is how the preview picks a representative appearance.
            entries = list(self._model_entries(matches[0][1]))
            return entries[:1]
        parts = state.get("multipart")
        if isinstance(parts, list):
            chosen: list[dict] = []
            for part in parts:
                if not isinstance(part, dict):
                    continue
                if self._when_matches(part.get("when"), props):
                    chosen.extend(self._model_entries(part.get("apply")))
            return chosen
        return []

    def _texture_layer(self, face_data: object,
                       textures: dict[str, object]) -> FaceLayer | None:
        if not isinstance(face_data, dict):
            return None
        raw_texture = face_data.get("texture")
        if raw_texture is None:
            return None
        texture_ref = self._texture_ref(raw_texture, textures)
        image = self.texture(texture_ref)
        if image is None:
            return None
        uv_data = face_data.get("uv", _FULL_UV)
        if not isinstance(uv_data, (list, tuple)) or len(uv_data) != 4:
            uv = _FULL_UV
        else:
            uv = tuple(float(v) for v in uv_data)
        return FaceLayer(
            texture=texture_ref or "",
            image=image,
            uv=uv,
            rotation=int(face_data.get("rotation", 0) or 0),
            tintindex=int(face_data.get("tintindex", -1) or -1),
        )

    def _build_box(self, element: dict,
                   textures: dict[str, object]) -> ElementBox | None:
        from_, to_ = element.get("from"), element.get("to")
        if not (isinstance(from_, list) and isinstance(to_, list) and
                len(from_) == 3 and len(to_) == 3):
            return None
        try:
            lo = tuple(min(float(from_[i]), float(to_[i])) for i in range(3))
            hi = tuple(max(float(from_[i]), float(to_[i])) for i in range(3))
        except (TypeError, ValueError):
            return None
        faces = {}
        element_faces = element.get("faces") or {}
        for face in FACES:
            layer = self._texture_layer(element_faces.get(face), textures)
            if layer is not None:
                faces[face] = layer
        return ElementBox(lo, hi, faces) if faces else None

    def parts_for_model(self, ref: str, entry: dict | None = None) -> ModelPart:
        """Build the boxes and blockstate rotation for one placed model."""
        entry = entry or {}
        model = self.model(ref)
        boxes = []
        for element in model.elements:
            box = self._build_box(element, model.textures)
            if box is not None:
                boxes.append(box)
        if not boxes and model.textures:
            # Marker/generated models have no elements. A block preview still
            # needs one cube for its texture; item icons deliberately do not
            # call this fallback and stay flat instead.
            raw = (model.textures.get("all") or model.textures.get("side") or
                   model.textures.get("top") or model.textures.get("particle") or
                   next(iter(model.textures.values())))
            layer = self._texture_layer({"texture": raw}, model.textures)
            if layer is not None:
                boxes.append(ElementBox((0, 0, 0), (16, 16, 16),
                                        {face: layer for face in FACES}))
        try:
            rot_x = float(entry.get("x", 0) or 0)
            rot_y = float(entry.get("y", 0) or 0)
        except (TypeError, ValueError):
            rot_x = rot_y = 0.0
        return ModelPart(_bare_name(ref), tuple(boxes), rot_x, rot_y,
                         bool(entry.get("uvlock", False)))

    @staticmethod
    def _rotate_vector(vec: tuple[float, float, float], axis: int,
                       degrees: float, around: tuple[float, float, float] | None = None):
        x, y, z = vec
        if around is not None:
            x, y, z = x - around[0], y - around[1], z - around[2]
        rad = math.radians(degrees)
        c, s = math.cos(rad), math.sin(rad)
        if axis == 0:
            y, z = y * c - z * s, y * s + z * c
        else:
            x, z = x * c + z * s, -x * s + z * c
        if around is not None:
            x, y, z = x + around[0], y + around[1], z + around[2]
        return x, y, z

    @classmethod
    def _inverse_point(cls, point: tuple[float, float, float], part: ModelPart):
        # The placed model is rotated X then Y around the block centre; undo
        # that order so the canonical model boxes can be intersected.
        point = cls._rotate_vector(point, 1, -part.rot_y, (8.0, 8.0, 8.0))
        return cls._rotate_vector(point, 0, -part.rot_x, (8.0, 8.0, 8.0))

    @classmethod
    def _inverse_direction(cls, direction: tuple[float, float, float], part: ModelPart):
        direction = cls._rotate_vector(direction, 1, -part.rot_y)
        return cls._rotate_vector(direction, 0, -part.rot_x)

    @staticmethod
    def _ray_box(origin: tuple[float, float, float],
                 direction: tuple[float, float, float],
                 lo: tuple[float, float, float],
                 hi: tuple[float, float, float]):
        tmin, tmax = -1e30, 1e30
        axis, sign = 0, 1
        for i in range(3):
            if abs(direction[i]) < 1e-9:
                if origin[i] < lo[i] or origin[i] > hi[i]:
                    return None
                continue
            t1 = (lo[i] - origin[i]) / direction[i]
            t2 = (hi[i] - origin[i]) / direction[i]
            if t1 > t2:
                t1, t2 = t2, t1
            if t1 > tmin:
                tmin, axis, sign = t1, i, 1 if direction[i] > 0 else -1
            if t2 < tmax:
                tmax = t2
            if tmin > tmax:
                return None
        t = tmin if tmin >= 0 else tmax
        if t < 0:
            return None
        face = (("west", "east"), ("down", "up"),
                ("north", "south"))[axis][0 if sign > 0 else 1]
        return t, face

    @staticmethod
    def _face_uv(face: str, point: tuple[float, float, float]):
        x, y, z = (max(0.0, min(16.0, v)) / 16.0 for v in point)
        if face == "up":
            return x, 1.0 - z
        if face == "down":
            return x, z
        if face == "north":
            return x, 1.0 - y
        if face == "south":
            return 1.0 - x, 1.0 - y
        if face == "west":
            return z, 1.0 - y
        return 1.0 - z, 1.0 - y

    def surface_hit_parts(self, parts: list[ModelPart],
                          origin: tuple[float, float, float],
                          direction: tuple[float, float, float],
                          tint: tuple[int, int, int] | None = None) -> SurfaceHit | None:
        """Closest non-transparent model texel along a block-local ray."""
        best: SurfaceHit | None = None
        for part in parts:
            local_origin = self._inverse_point(origin, part)
            local_direction = self._inverse_direction(direction, part)
            for box in part.elements:
                hit = self._ray_box(local_origin, local_direction, box.lo, box.hi)
                if hit is None:
                    continue
                t, face = hit
                layer = box.faces.get(face)
                if layer is None:
                    continue
                hit_point = tuple(local_origin[i] + local_direction[i] * t for i in range(3))
                u, v = self._face_uv(face, hit_point)
                use_tint = tint if layer.tintindex >= 0 else None
                texel = self._sample_layer(layer, u, v, use_tint)
                if texel[3] == 0:
                    continue
                distance = t / 16.0
                if best is None or distance < best.distance:
                    best = SurfaceHit(distance, face, texel)
        return best

    def surface_hit(self, state: str, origin: tuple[float, float, float],
                    direction: tuple[float, float, float],
                    tint: tuple[int, int, int] | None = None) -> SurfaceHit | None:
        return self.surface_hit_parts(self.appearance(state).parts, origin, direction, tint)

    def appearance(self, state: str) -> Appearance:
        if state in self._appearances:
            return self._appearances[state]
        name, props = parse_state(state)
        refs = self._state_models(name, props)
        faces: dict[str, list[FaceLayer]] = {face: [] for face in FACES}
        all_textures: dict[str, object] = {}
        parts: list[ModelPart] = []
        model_name = ""
        for entry in refs:
            model_ref = entry.get("model")
            if not isinstance(model_ref, str):
                continue
            model_name = model_name or model_ref
            model = self.model(model_ref)
            all_textures.update(model.textures)
            part = self.parts_for_model(model_ref, entry)
            if part.elements:
                parts.append(part)
                for box in part.elements:
                    for face, layer in box.faces.items():
                        faces[face].append(layer)
        appearance = Appearance(model_name, faces, all_textures, parts)
        self._appearances[state] = appearance
        return appearance

    @staticmethod
    def _tinted(pixel: tuple[int, int, int, int], tint: tuple[int, int, int] | None):
        if tint is None:
            return pixel
        r, g, b, a = pixel
        return (r * tint[0] // 255, g * tint[1] // 255, b * tint[2] // 255, a)

    @staticmethod
    def _sample_layer(layer: FaceLayer, u: float, v: float,
                      tint: tuple[int, int, int] | None) -> tuple[int, int, int, int]:
        u = min(1.0, max(0.0, u))
        v = min(1.0, max(0.0, v))
        u1, v1, u2, v2 = layer.uv
        rot = layer.rotation % 360
        if rot == 90:
            u, v = v, 1.0 - u
        elif rot == 180:
            u, v = 1.0 - u, 1.0 - v
        elif rot == 270:
            u, v = 1.0 - v, u
        x = u1 + (u2 - u1) * u
        y = v1 + (v2 - v1) * v
        width = layer.image.width
        # Animated block textures are a vertical strip of square frames; the
        # preview samples frame 0, so the sample height is the frame width.
        height = width
        px = min(width - 1, max(0, int(x / 16.0 * width)))
        py = min(height - 1, max(0, int(y / 16.0 * height)))
        return BlockTextures._tinted(layer.image.getpixel((px, py)), tint)

    def sample_face(self, state: str, face: str, u: float, v: float,
                    tint: tuple[int, int, int] | None = None) -> tuple[int, int, int, int] | None:
        layers = self.appearance(state).faces.get(face)
        if not layers:
            return None
        out = [0, 0, 0, 0]
        for layer in layers:
            use_tint = tint if layer.tintindex >= 0 else None
            r, g, b, a = self._sample_layer(layer, u, v, use_tint)
            if a == 0:
                continue
            sa = a / 255.0
            da = out[3] / 255.0
            oa = sa + da * (1.0 - sa)
            if oa <= 0.0:
                out = [0, 0, 0, 0]
                continue
            out = [
                int((r * sa + out[0] * da * (1.0 - sa)) / oa),
                int((g * sa + out[1] * da * (1.0 - sa)) / oa),
                int((b * sa + out[2] * da * (1.0 - sa)) / oa),
                int(oa * 255.0),
            ]
        return tuple(out)  # type: ignore[return-value]

    def face_pixels(self, state: str, face: str, size: int = 16,
                    tint: tuple[int, int, int] | None = None) -> list[tuple[int, int, int, int]]:
        return [self.sample_face(state, face, (x + 0.5) / size, (y + 0.5) / size, tint)
                or (0, 0, 0, 0) for y in range(size) for x in range(size)]

    def source_faces(self, state: str) -> dict[str, list[FaceLayer]]:
        return self.appearance(state).faces

    def corpus(self, kind: str) -> list[str]:
        if self._zip is not None:
            assert self._names is not None
            prefix = f"assets/minecraft/textures/{kind}/"
            relative = "assets/minecraft/textures/"
            return sorted(n[len(relative):] for n in self._names
                          if n.startswith(prefix) and n.endswith(".png"))
        assert self._root is not None
        base = self._root / "minecraft" / "textures" / kind
        if not base.is_dir():
            return []
        relative = self._root / "minecraft" / "textures"
        return sorted(path.relative_to(relative).as_posix()
                      for path in base.rglob("*.png") if path.is_file())

    def definitions(self, kind: str) -> list[str]:
        """Relative JSON names under ``assets/minecraft/<kind>``."""
        if self._zip is not None:
            assert self._names is not None
            prefix = f"assets/minecraft/{kind}/"
            return sorted(name[len(prefix):-5] for name in self._names
                          if name.startswith(prefix) and name.endswith(".json"))
        assert self._root is not None
        base = self._root / "minecraft" / kind
        if not base.is_dir():
            return []
        return sorted(path.relative_to(base).with_suffix("").as_posix()
                      for path in base.rglob("*.json") if path.is_file())


def find_client_jar(repo: Path) -> Path | None:
    """Find a client jar without ever requiring one at import time."""
    import os

    env = os.environ.get("PLUSMC_CLIENT_JAR")
    if env:
        path = Path(env).expanduser()
        if path.is_file():
            return path
    candidates = sorted((repo / "data" / "vanilla" / "_cache").glob("client-*.jar"))
    return candidates[-1] if candidates else None


def find_texture_source(repo: Path) -> Path | None:
    """Prefer a prepared asset tree; fall back to a cached client jar."""
    import os

    env = os.environ.get("PLUSMC_ASSETS")
    if env:
        path = Path(env).expanduser()
        if path.is_file() or path.is_dir():
            return path
    version = latest_version_name(repo)
    if version:
        path = repo / "data" / "vanilla" / version / "assets"
        if (path / "minecraft" / "models").is_dir():
            return path
    for path in reversed(sorted((repo / "data" / "vanilla").glob("*/assets"))):
        if ((path / "minecraft" / "blockstates").is_dir() and
                (path / "minecraft" / "models").is_dir()):
            return path
    return find_client_jar(repo)


def latest_version_name(repo: Path) -> str | None:
    versions = sorted((repo / "data" / "vanilla").glob("*/version.json"))
    for path in reversed(versions):
        try:
            return json.loads(path.read_text())["id"]
        except (OSError, ValueError, KeyError):
            continue
    return None
