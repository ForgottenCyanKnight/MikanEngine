"""Import the current OHOS gameplay scenes into the desktop project."""

import argparse
import copy
import itertools
import json
import math
import shutil
import struct
import zlib
from functools import cache
from pathlib import Path
from urllib.parse import unquote


ENGINE = Path(__file__).resolve().parents[1]
OHOS_RESOURCES = Path("ohos-project/entry/src/main/resources/rawfile")
PROJECT = ENGINE / "projects/ohos-physics-parkour"
SCENES = ("main", "forest_test", "physics_parkour", "parkour", "workshop", "arena", "terrain_demo")
ROOT = 4294967295
GAME = "ohos_prototype"
ENGINE_CUBE = "engine/models/Base Model/cube.glb"
PLAYER_MODEL = "models/Quaternius/AnimationLibrary_Standard.glb"
GROUND_MODEL = "models/CrackedStonebrick/cube.gltf"
HELMET_MODEL = "models/DamagedHelmet/glTF/DamagedHelmet.gltf"
WEAPON_MODELS = {
    "OHOS Held Pistol": "models/Pistol/pistol.glb",
    "OHOS Held Sword": "models/Sword/sword.glb",
}
PLAYER_ORIGIN = 0.96
GROUND_TOP = -0.94
MARKER_ASSETS = Path("assets/ohos/markers")


def save_scene(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def import_asset(raw, project, source):
    preferred = project / source
    # The first desktop experiment still owns its three differently colored GLBs.
    if source.startswith("models/parkour/"):
        preferred = project / "assets/ohos" / source
    elif preferred.is_file() and preferred.read_bytes() != (raw / source).read_bytes():
        raise ValueError(f"An existing project asset differs from OHOS: {preferred}")
    preferred.parent.mkdir(parents=True, exist_ok=True)
    if not preferred.is_file() or preferred.read_bytes() != (raw / source).read_bytes():
        shutil.copyfile(raw / source, preferred)
    return preferred.relative_to(project).as_posix()


@cache
def gltf_data(path):
    if path.suffix == ".gltf":
        return json.loads(path.read_text(encoding="utf-8")), b""
    with path.open("rb") as model:
        magic, version, _ = struct.unpack("<4sII", model.read(12))
        size, kind = struct.unpack("<I4s", model.read(8))
        if (magic, version, kind) != (b"glTF", 2, b"JSON"):
            raise ValueError(f"Unrecognized glTF binary: {path}")
        document = json.loads(model.read(size))
        chunk = model.read(8)
        return document, model.read(struct.unpack("<I4s", chunk)[0]) if chunk else b""


def validate_gltf_dependencies(path, root):
    document, _ = gltf_data(path)
    for resource in document.get("buffers", []) + document.get("images", []):
        uri = resource.get("uri", "")
        if uri and not uri.startswith("data:"):
            dependency = (path.parent / unquote(uri)).resolve()
            if not dependency.is_relative_to(root) or not dependency.is_file():
                raise ValueError(f"Missing glTF resource {uri} in {path}")


def rotate(rotation, vector):
    w, x, y, z = rotation
    length = math.sqrt(w*w + x*x + y*y + z*z)
    w, x, y, z = (v / length for v in rotation)
    a, b, c = vector
    return [
        a * (1 - 2 * (y*y + z*z)) + b * 2 * (x*y - w*z) + c * 2 * (x*z + w*y),
        a * 2 * (x*y + w*z) + b * (1 - 2 * (x*x + z*z)) + c * 2 * (y*z - w*x),
        a * 2 * (x*z - w*y) + b * 2 * (y*z + w*x) + c * (1 - 2 * (x*x + y*y)),
    ]


def transform_vertex(node, point):
    if "matrix" in node:
        values = node["matrix"]
        return [sum(values[row + 4 * col] * point[col] for col in range(3)) + values[row + 12]
                for row in range(3)]
    x, y, z, w = node.get("rotation", [0, 0, 0, 1])
    scaled = [p * s for p, s in zip(point, node.get("scale", [1, 1, 1]))]
    return [v + t for v, t in zip(rotate([w, x, y, z], scaled), node.get("translation", [0, 0, 0]))]


@cache
def static_model_normalization(path):
    # The OHOS static importer applies glTF node transforms and then centers
    # the actual positions in a two-unit box. The desktop GLTF loader retains
    # authored dimensions, so apply that normalization to these scene meshes.
    data, binary = gltf_data(path)
    buffers = [(path.parent / unquote(buffer["uri"])).read_bytes()
               if "uri" in buffer else binary for buffer in data["buffers"]]
    low = [math.inf] * 3
    high = [-math.inf] * 3
    nodes = data.get("nodes", [])

    def visit(index, parents):
        node = nodes[index]
        chain = [node, *parents]
        if "mesh" in node:
            for primitive in data["meshes"][node["mesh"]]["primitives"]:
                accessor = data["accessors"][primitive["attributes"]["POSITION"]]
                view = data["bufferViews"][accessor["bufferView"]]
                if accessor["componentType"] != 5126 or accessor["type"] != "VEC3":
                    raise ValueError(f"Unsupported static model vertex format: {path}")
                offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
                stride = view.get("byteStride", 12)
                positions = buffers[view["buffer"]]
                for vertex in range(accessor["count"]):
                    point = struct.unpack_from("<3f", positions, offset + vertex * stride)
                    for transform in chain:
                        point = transform_vertex(transform, point)
                    for axis, value in enumerate(point):
                        low[axis] = min(low[axis], value)
                        high[axis] = max(high[axis], value)
        for child in node.get("children", []):
            visit(child, chain)

    for index in data["scenes"][data.get("scene", 0)]["nodes"]:
        visit(index, [])
    extent = max(b - a for a, b in zip(low, high))
    return [-(a + b) / extent for a, b in zip(low, high)], 2 / extent


def weapon_assets(raw, project):
    assets = {}
    for name, source in WEAPON_MODELS.items():
        path = raw / source
        offset, factor = static_model_normalization(path)
        data, binary = gltf_data(path)
        data = copy.deepcopy(data)
        nodes = data["nodes"]
        for scene in data["scenes"]:
            children = scene["nodes"]
            scene["nodes"] = [len(nodes)]
            nodes.append({"children": children, "translation": offset, "scale": [factor] * 3})
        content = json.dumps(data, separators=(",", ":")).encode("utf-8")
        content += b" " * (-len(content) % 4)
        # The glTF header is followed by a chunk length and chunk type.
        header = struct.pack("<4sIII4s", b"glTF", 2, 28 + len(content) + len(binary), len(content), b"JSON")
        destination = project / "assets/ohos/weapons" / path.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(header + content + struct.pack("<I4s", len(binary), b"BIN\x00") + binary)
        validate_gltf_dependencies(destination, project)
        assets[name] = destination.relative_to(project).as_posix()
    return assets


def transform(position, scale=(1, 1, 1), rotation=(1, 0, 0, 0)):
    return {"position": list(position), "scale": list(scale), "rotation": list(rotation)}


def entity(entity_id, name, position=(0, 0, 0), scale=(1, 1, 1)):
    return {
        "id": entity_id,
        "name": {"name": name},
        "transform": transform(position, scale),
        "hierarchy": {"parent": ROOT, "children": []},
    }


def rigid_body(body_type, size=(2, 2, 2), offset=(0, 0, 0), shape=0, trigger=False, mass=12):
    return {
        "type": body_type, "shapeType": shape, "size": list(size), "offset": list(offset),
        "mass": mass, "restitution": 0, "useGravity": body_type == 1, "isTrigger": trigger,
        "useOBB": shape == 3, "autoFitToModel": False, "syncWithModel": False,
    }


def render():
    return {"visible": True, "castShadow": True, "receiveShadow": True}


def actor(entity_id, name, center, player=False):
    x, y, z = center
    item = entity(entity_id, name, (x, y - PLAYER_ORIGIN, z))
    item["mesh"] = {"type": 4, "modelPath": PLAYER_MODEL}
    item["render"] = render()
    item["animator"] = {"clipIndex": 9, "speed": 1, "loop": True, "playing": True, "time": 0}
    if player:
        item["rigidBody"] = rigid_body(1, (.7, 1.18, .7), (0, .94, 0), shape=2, mass=70)
        # The prototype module handles movement. Physics needs this tag for swimming.
        item["playerController"] = {
            "enabled": False, "cameraName": "Main Camera", "moveSpeed": 1.3, "jumpSpeed": 5.0,
        }
    return item


def marker_texture(path, color, opacity, save_point):
    width, height = 48, 64
    data = bytearray()
    for row in range(height):
        data.append(0)  # no PNG row filter
        v = row / (height - 1)
        for col in range(width):
            u = col / (width - 1)
            if save_point:
                smooth = v * v * (3 - 2 * v)
                alpha = .52 * (1 - smooth) ** 1.7
                light = (.1, 1.65, 3.8)
            else:
                x, y = u * 2 - 1, v * 2 - 1
                edge = max(abs(x), abs(y)) ** 10
                ripple = .5 + .5 * math.sin(y * 14 + math.sin(x * 8) * .6)
                alpha = min(.8, opacity * (.75 + ripple * .3 + edge * .85))
                luma = max(.05, sum(c * w for c, w in zip(color, (.2126, .7152, .0722))))
                light = tuple(c * (2.4 + edge * .6 + .15 * ripple ** 6) / luma for c in color)
            data.extend(round(255 * (c / (1 + c)) ** (1 / 2.2)) for c in light)
            data.append(round(255 * alpha))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(data)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def marker_model(project, name, color=(0, 0, 0), opacity=0, save_point=False):
    base = project / MARKER_ASSETS / name
    base.parent.mkdir(parents=True, exist_ok=True)
    marker_texture(base.with_suffix(".png"), color, opacity, save_point)
    if save_point:
        x, z = .65, .65
        corners = [(-x, -z), (x, -z), (x, z), (-x, z)]
        planes = [(a, b, 0, 1.55) for a, b in zip(corners, corners[1:] + corners[:1])]
    else:
        planes = [((-.94, 0), (.94, 0), -1.4, 1.4)]
    data = bytearray()
    indices = []
    positions = []
    for (ax, az), (bx, bz), bottom, top in planes:
        length = math.hypot(bx - ax, bz - az)
        normal = ((az - bz) / length, 0, (bx - ax) / length)
        vertices = [(ax, bottom, az), (bx, bottom, bz), (bx, top, bz), (ax, top, az)]
        for point, uv in zip(vertices, ((0, 0), (1, 0), (1, 1), (0, 1))):
            data.extend(struct.pack("<8f", *point, *normal, *uv))
        start = len(positions)
        positions.extend(vertices)
        indices.extend(start + offset for offset in (0, 1, 2, 0, 2, 3))
    index_offset = len(data)
    data.extend(struct.pack(f"<{len(indices)}H", *indices))
    base.with_suffix(".bin").write_bytes(data)
    limits = [list(map(rule, zip(*positions))) for rule in (min, max)]
    geometry = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                                   "indices": 3, "material": 0}]}],
        "buffers": [{"uri": base.with_suffix(".bin").name, "byteLength": len(data)}],
        "bufferViews": [{"buffer": 0, "byteLength": index_offset, "byteStride": 32, "target": 34962},
                        {"buffer": 0, "byteOffset": index_offset, "byteLength": 2 * len(indices), "target": 34963}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions),
             "type": "VEC3", "min": limits[0], "max": limits[1]},
            {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": len(positions), "type": "VEC3"},
            {"bufferView": 0, "byteOffset": 24, "componentType": 5126, "count": len(positions), "type": "VEC2"},
            {"bufferView": 1, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
        "images": [{"uri": base.with_suffix(".png").name}],
        "textures": [{"source": 0}],
        "materials": [{
            "name": "Light surface", "alphaMode": "BLEND", "doubleSided": True,
            "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0, "roughnessFactor": 1},
            "emissiveFactor": [1, 1, 1], "emissiveTexture": {"index": 0},
        }],
    }
    path = base.with_suffix(".gltf")
    save_scene(path, geometry)
    validate_gltf_dependencies(path, project)
    return path.relative_to(project).as_posix()


def portals_in(gameplay):
    portals = gameplay.get("portals", [])
    if gameplay.get("portal"):
        return [*portals, gameplay["portal"]]
    return portals


def light_surfaces(stem, gameplay, next_id, project):
    markers = []
    for index, portal in enumerate(portals_in(gameplay), start=1):
        opacity = portal.get("opacity", .32)
        if opacity <= 0:
            continue
        x, y, z = portal["position"]
        mesh = marker_model(project, f"{stem}_portal_{index}", portal.get("color", (.08, .85, .4)), opacity)
        markers.append((f'OHOS Portal: {portal["label"]}', (x, y + .44, z), mesh))
    if gameplay.get("savePoints"):
        mesh = marker_model(project, "save_point", save_point=True)
        for index, (x, y, z) in enumerate(gameplay["savePoints"], start=1):
            markers.append((f"OHOS Save Point {index}", (x, y - .98, z), mesh))
    for name, position, path in markers:
        item = entity(next(next_id), name, position)
        item["mesh"] = {"type": 4, "modelPath": path}
        item["render"] = {"visible": True, "castShadow": False, "receiveShadow": False}
        yield item


def is_ground_surface(item):
    if item.get("mesh", {}).get("modelPath") != ENGINE_CUBE or item.get("rigidBody", {}).get("type", 0):
        return False
    x, y, z = map(abs, item["transform"]["scale"])
    return x >= 8 and z >= 8 and y <= min(x, z) * .1


def convert_collider(item, built_in_cube):
    if not item.get("render", {}).get("visible", True):
        return
    authored = item.pop("collider", None)
    if authored is None:
        if built_in_cube and item["hierarchy"]["parent"] == ROOT:
            item["rigidBody"] = rigid_body(0)
        return
    shape = authored.get("type", 0)
    if shape == 0 and authored.get("useOBB"):
        shape = 3
    size = authored.get("size", [2, 2, 2])
    offset = authored.get("offset", [0, 0, 0])
    if authored.get("autoFitToModel"):
        size = [2, 2, 2]
    elif not authored.get("syncWithModel", True):
        scale = [max(abs(value), .001) for value in item["transform"]["scale"]]
        size = [value / factor for value, factor in zip(size, scale)]
        offset = [value / factor for value, factor in zip(offset, scale)]
    item["rigidBody"] = rigid_body(0, size, offset, shape, authored.get("isTrigger", False))


def convert_dynamic_body(item, next_id):
    # Baking the box dimensions preserves OHOS's 12 kg mass. Applying the
    # model scale through Jolt SetShape would recompute mass from shape density.
    size = item["transform"]["scale"]
    item["transform"]["scale"] = [1, 1, 1]
    item["rigidBody"] = rigid_body(1, [abs(value) * 2 for value in size])
    name = item["name"]["name"]
    visual = entity(next(next_id), f"{name} Visual", scale=size)
    visual["hierarchy"]["parent"] = item["id"]
    item["hierarchy"]["children"].append(visual["id"])
    for component in ("mesh", "render", "material"):
        if component in item:
            visual[component] = item.pop(component)
    return visual


def convert_scene(stem, source, templates, raw, assets, weapons, project):
    gameplay = source.get("gameplay", {})
    workshop = gameplay.get("workshop", {})
    moving = {entry["entityId"] for entry in workshop.get("movingPlatforms", [])}
    moving.update(workshop[k] for k in ("gateId", "liftId") if k in workshop)
    converted = copy.deepcopy(source["entities"])
    by_id = {item["id"]: item for item in converted}
    if len(by_id) != len(converted):
        raise ValueError(f"Duplicate source scene IDs in {stem}")
    next_id = itertools.count(max(by_id) + 1)
    authored_ground = next((item for item in converted if is_ground_surface(item)), None)
    ground_delta = 0
    if authored_ground:
        position = authored_ground["transform"]["position"]
        ground_delta = GROUND_TOP - position[1] - abs(authored_ground["transform"]["scale"][1])
        converted.remove(authored_ground)

    dynamic_visuals = []
    for item in converted:
        item.setdefault("hierarchy", {"parent": ROOT, "children": []})
        if item["hierarchy"]["parent"] == ROOT:
            item["transform"]["position"][1] += ground_delta
        built_in_cube = item.get("mesh", {}).get("modelPath") == ENGINE_CUBE
        if item.pop("dynamicBody", False) or item["id"] == workshop.get("crateId"):
            item.pop("collider", None)
            dynamic_visuals.append(convert_dynamic_body(item, next_id))
        elif item["id"] in moving:
            item.pop("collider", None)
            item["rigidBody"] = rigid_body(2)
        else:
            convert_collider(item, built_in_cube)

    converted.extend(dynamic_visuals)
    for item in converted:
        motion_parent = item.pop("motionParent", None)
        if motion_parent is not None:
            parent = by_id[motion_parent]
            pose, base = item["transform"], parent["transform"]
            if base["rotation"] != [1, 0, 0, 0]:
                raise ValueError(f"Unexpected rotated motion parent {motion_parent} in {stem}")
            pose["position"] = [(p - b) / s for p, b, s in zip(pose["position"], base["position"], base["scale"])]
            pose["scale"] = [s / b for s, b in zip(pose["scale"], base["scale"])]
            item["hierarchy"]["parent"] = motion_parent
            parent["hierarchy"]["children"].append(item["id"])

        mesh = item.get("mesh", {})
        path = mesh.get("modelPath")
        if path in assets:
            mesh["modelPath"] = assets[path]
        if path == ENGINE_CUBE:
            # OHOS renders its solid cube geometry with the cracked stone material.
            mesh["modelPath"] = GROUND_MODEL
        if path and path.startswith("models/EZTree/"):
            offset, factor = static_model_normalization(raw / path)
            pose = item["transform"]
            scaled_offset = [value * scale for value, scale in zip(offset, pose["scale"])]
            offset = rotate(pose["rotation"], scaled_offset)
            pose["position"] = [p + d for p, d in zip(pose["position"], offset)]
            pose["scale"] = [value * factor for value in pose["scale"]]

    if gameplay.get("groundEnabled", True):
        ground = entity(next(next_id), "OHOS demo stone floor", (0, GROUND_TOP - .2, 0), (20, .2, 20))
        ground["mesh"] = {"type": 4, "modelPath": GROUND_MODEL}
        ground["render"] = render()
        ground["render"]["visible"] = gameplay.get("demoPropsEnabled", True)
        ground["rigidBody"] = rigid_body(0)
        converted.append(ground)

    if gameplay.get("helmetEnabled", False) or gameplay.get("demoPropsEnabled", True):
        labels = ("ALBEDO", "WORLD NORMAL", "PBR: R=METAL G=ROUGH B=AO", "FULL PBR LIGHTING")
        offset, factor = static_model_normalization(raw / HELMET_MODEL)
        yaw = [math.cos(.25), 0, math.sin(.25), 0]
        adjustment = rotate(yaw, [v * .5 for v in offset])
        for index, label in enumerate(labels):
            center = (-8, -.44, 4 - 3 * index)
            position = [v + d for v, d in zip(center, adjustment)]
            helmet = entity(next(next_id), f"OHOS Helmet Display: {label}", position, (factor * .5,) * 3)
            helmet["transform"]["rotation"] = yaw
            helmet["mesh"] = {"type": 4, "modelPath": HELMET_MODEL}
            helmet["render"] = render()
            converted.append(helmet)

    water = source.get("water")
    if water is not None and water.get("enabled", True):
        extent = water.get("size", 128) / 2
        bounds = water.get("halfExtents", [extent, 0, extent])
        center = water.get("center", [0, 0, 0])
        pool = entity(next(next_id), "OHOS Water", (center[0], water.get("height", -2.6), center[2]))
        pool["water"] = {
            "enabled": True, "size": [bounds[0] * 2, bounds[2] * 2],
            "surfaceOffset": 0, "depth": 1.8, "buoyancy": 1.15, "drag": 2,
            "color": water.get("color", [.035, .18, .22]), "roughness": water.get("roughness", .08),
            "affectPlayersOnly": True,
        }
        converted.append(pool)

    for template in templates:
        key = next(key for key in ("camera", "light", "skybox") if key in template)
        if not any(key in item for item in converted):
            fallback = copy.deepcopy(template)
            fallback["id"] = next(next_id)
            converted.append(fallback)
    spawn = gameplay.get("playerSpawn", [0, 0, 0])
    parkour_camera = "parkour" in gameplay
    camera_pitch = .38 if parkour_camera else 0
    camera_distance = 4.2 if parkour_camera else 2.8
    for item in converted:
        camera = item.get("camera", {})
        if camera.get("isMainCamera"):
            camera.update(thirdPersonTargetName="OHOS Player", thirdPersonTargetOffset=[0, 1.68, 0],
                          thirdPersonYaw=0, thirdPersonPitch=math.degrees(camera_pitch),
                          thirdPersonDistance=camera_distance, thirdPersonPositionDamping=8,
                          thirdPersonAimShoulderOffset=.55)
            item["transform"] = transform(
                [spawn[0], spawn[1] + .72 + camera_distance * math.sin(camera_pitch),
                 spawn[2] + camera_distance * math.cos(camera_pitch)],
                rotation=[math.cos(camera_pitch / 2), -math.sin(camera_pitch / 2), 0, 0],
            )

    player = actor(next(next_id), "OHOS Player", spawn, player=True)
    converted.append(player)
    for name, path in weapons.items():
        weapon = entity(next(next_id), name)
        weapon["hierarchy"]["parent"] = player["id"]
        weapon["mesh"] = {"type": 4, "modelPath": path}
        weapon["render"] = render()
        weapon["render"]["visible"] = False
        player["hierarchy"]["children"].append(weapon["id"])
        converted.append(weapon)
    for index, spawn_gate in enumerate(gameplay.get("arena", {}).get("spawnGates", []), start=1):
        enemy = actor(next(next_id), f"OHOS Arena Enemy {index}", spawn_gate)
        enemy["render"]["visible"] = False
        converted.append(enemy)
    converted.extend(light_surfaces(stem, gameplay, next_id, project))
    converted.append(entity(next(next_id), f"OHOS Scene: {stem}"))

    return {"formatVersion": 1, "game": GAME, "entities": converted}


def validate_scene(stem, source, scene, project):
    entities = {item["id"]: item for item in scene["entities"]}
    for item in entities.values():
        hierarchy = item["hierarchy"]
        if hierarchy["parent"] != ROOT and item["id"] not in entities[hierarchy["parent"]]["hierarchy"]["children"]:
            raise ValueError(f"Mismatched hierarchy in {stem}: {item['id']}")
        for child in hierarchy["children"]:
            if entities[child]["hierarchy"]["parent"] != item["id"]:
                raise ValueError(f"Mismatched child in {stem}: {child}")
        for component in ("mesh", "terrain", "material"):
            for key, value in item.get(component, {}).items():
                if key.lower().endswith("path") and value:
                    root = ENGINE if value.startswith("engine/") else project
                    if not (root / value).is_file():
                        raise ValueError(f"Missing {component} asset {value} in {stem}")
        skybox = item.get("skybox", {}).get("textureName")
        if skybox:
            for face in ("right", "left", "top", "bottom", "front", "back"):
                if not (project / skybox / f"{face}.jpg").is_file():
                    raise ValueError(f"Missing skybox face {face} in {stem}")
    gameplay = source.get("gameplay", {})
    for portal in portals_in(gameplay):
        if portal["target"] not in {f"scenes/{stem}.json" for stem in SCENES}:
            raise ValueError(f"Missing portal target {portal['target']} in {stem}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ohos-root", type=Path, default=ENGINE.parent / "ohosEngine")
    parser.add_argument("--project", type=Path, default=PROJECT)
    args = parser.parse_args()
    raw = args.ohos_root.resolve() / OHOS_RESOURCES
    project = args.project.resolve()
    sources = {stem: (raw / "scenes" / (stem + ".json")).read_bytes() for stem in SCENES}
    scenes = {stem: json.loads(data) for stem, data in sources.items()}
    templates = [item for item in scenes["main"]["entities"] if any(k in item for k in ("camera", "light", "skybox"))]
    resources = [path.relative_to(raw).as_posix() for path in raw.rglob("*")
                 if path.is_file() and path.relative_to(raw).parts[0] != "scenes"]
    assets = {resource: import_asset(raw, project, resource) for resource in sorted(resources)}
    for destination in assets.values():
        path = project / destination
        if path.suffix.lower() in (".gltf", ".glb"):
            validate_gltf_dependencies(path, project)
    weapons = weapon_assets(raw, project)

    for stem, source in scenes.items():
        scene = convert_scene(stem, source, templates, raw, assets, weapons, project)
        validate_scene(stem, source, scene, project)
        native_path = project / "scenes/ohos" / (stem + ".json")
        source_path = project / "data/ohos" / (stem + ".json")
        save_scene(native_path, scene)
        source_path.parent.mkdir(parents=True, exist_ok=True)
        source_path.write_bytes(sources[stem])
        print(f"{stem}: {len(source['entities'])} source entities, {len(scene['entities'])} native entities")
    print(f"Verified {len(scenes)} scenes and {len(assets)} copied assets.")


if __name__ == "__main__":
    main()
