"""Blender -> Mikan Engine offline scene converter.

This script is executed by Blender in background mode.  It deliberately uses
Blender's supported glTF exporter instead of parsing .blend files directly:

    blender.exe --background scene.blend --python export_mikan.py -- \
        --output assets/blend_import/scene --asset-root assets

The first version produces a self-contained GLB for visible render meshes,
then writes a Mikan scene JSON containing the GLB, cameras, lights and the
small set of engine-native water metadata currently supported by the runtime.
Unsupported Blender data is reported in manifest.json rather than silently
being dropped.
"""

from __future__ import annotations

import argparse
import datetime
import json
import math
import os
import sys
import traceback
from pathlib import Path

import bpy
from mathutils import Matrix, Vector


ENGINE_BASIS = Matrix.Rotation(-math.pi * 0.5, 4, "X")
INVALID_ENTITY = 4294967295


def parse_args() -> argparse.Namespace:
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []

    parser = argparse.ArgumentParser(description="Export a Blender scene for Mikan Engine")
    parser.add_argument("--output", required=True, help="Directory for generated assets")
    parser.add_argument("--asset-root", default="", help="Mikan assets directory, used for relative model paths")
    parser.add_argument("--scene-name", default="", help="Output scene name (defaults to the .blend stem)")
    parser.add_argument("--inspect-only", action="store_true", help="Only inspect Blender data; do not export a GLB")
    parser.add_argument("--animation-source", default="", help="Optional GLB/FBX animation library used for retargeting")
    parser.add_argument("--retarget-animations", action="store_true", help="Retarget animation-source clips to the imported armature")
    return parser.parse_args(argv)


def finite_float(value: object, fallback: float = 0.0) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return fallback
    return result if math.isfinite(result) else fallback


def json_safe(value: object) -> object:
    """Convert Blender ID properties and mathutils values into JSON values."""
    if isinstance(value, (str, bool, int)):
        return value
    if isinstance(value, float):
        return finite_float(value)
    if isinstance(value, (Vector, tuple, list)):
        return [json_safe(item) for item in value]
    if isinstance(value, dict):
        return {str(key): json_safe(item) for key, item in value.items()}
    try:
        return [json_safe(item) for item in value]
    except TypeError:
        return str(value)


def write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(payload, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def clean_name(name: str, fallback: str = "Entity") -> str:
    value = str(name or "").strip()
    return value if value else fallback


def custom_value(obj: bpy.types.Object, key: str, default: object = None) -> object:
    try:
        return obj.get(key, default)
    except (AttributeError, TypeError):
        return default


def custom_bool(obj: bpy.types.Object, key: str, default: bool = False) -> bool:
    value = custom_value(obj, key, default)
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes", "on"}
    return bool(value)


def custom_string(obj: bpy.types.Object, key: str, default: str = "") -> str:
    value = custom_value(obj, key, default)
    return str(value) if value is not None else default


def object_type_tag(obj: bpy.types.Object) -> str:
    return custom_string(obj, "mikan_type", "").strip().lower()


def collection_tags(obj: bpy.types.Object) -> set[str]:
    tags: set[str] = set()
    for collection in getattr(obj, "users_collection", []):
        tags.add(collection.name.strip().lower())
    return tags


def is_water_object(obj: bpy.types.Object) -> bool:
    tag = object_type_tag(obj)
    if tag in {"water", "water_volume", "watervolume"}:
        return True
    if custom_bool(obj, "mikan_water", False):
        return True
    tags = collection_tags(obj)
    return any(tag in {"water", "waters", "water_volume", "water volumes"} for tag in tags)


def is_marker_object(obj: bpy.types.Object) -> bool:
    return object_type_tag(obj) in {"spawn", "player_spawn", "marker", "empty"}


def is_collision_only_object(obj: bpy.types.Object) -> bool:
    tag = object_type_tag(obj)
    return tag in {"collision", "collider", "collision_only", "physics"} or custom_bool(
        obj, "mikan_collision_only", False
    )


def is_visible_object(obj: bpy.types.Object) -> bool:
    if bool(getattr(obj, "hide_render", False)) or bool(getattr(obj, "hide_viewport", False)):
        return False
    try:
        if obj.hide_get():
            return False
    except (AttributeError, RuntimeError):
        pass
    for collection in getattr(obj, "users_collection", []):
        if bool(getattr(collection, "hide_render", False)):
            return False
    return True


def is_render_mesh(obj: bpy.types.Object) -> bool:
    if obj.type != "MESH" or not is_visible_object(obj):
        return False
    if is_water_object(obj) or is_marker_object(obj) or is_collision_only_object(obj):
        return False
    if custom_bool(obj, "mikan_exclude", False) or custom_bool(obj, "mikan_export", True) is False:
        return False
    return bool(getattr(obj.data, "polygons", []))


def quat_list(quaternion) -> list[float]:
    q = quaternion.normalized()
    return [finite_float(q.w, 1.0), finite_float(q.x), finite_float(q.y), finite_float(q.z)]


def transform_from_matrix(matrix: Matrix, include_scale: bool = True) -> dict[str, list[float]]:
    """Convert Blender Z-up world coordinates to Mikan/glTF Y-up coordinates."""
    engine_matrix = ENGINE_BASIS @ matrix @ ENGINE_BASIS.inverted()
    location, rotation, scale = engine_matrix.decompose()
    result = {
        "position": [finite_float(location.x), finite_float(location.y), finite_float(location.z)],
        "rotation": quat_list(rotation),
        "scale": [finite_float(scale.x, 1.0), finite_float(scale.y, 1.0), finite_float(scale.z, 1.0)]
        if include_scale
        else [1.0, 1.0, 1.0],
    }
    return result


def transform_from_location_rotation(location: Vector, rotation) -> dict[str, list[float]]:
    matrix = Matrix.Translation(location) @ rotation.to_matrix().to_4x4()
    return transform_from_matrix(matrix)


def transform_from_engine_pose(location: Vector, rotation) -> dict[str, list[float]]:
    """Build a transform for a pose that is already in Mikan Y-up space."""
    return {
        "position": [finite_float(location.x), finite_float(location.y), finite_float(location.z)],
        "rotation": quat_list(rotation),
        "scale": [1.0, 1.0, 1.0],
    }


def make_entity(entity_id: int, name: str, transform: dict[str, list[float]]) -> dict[str, object]:
    return {
        "id": entity_id,
        "name": {"name": clean_name(name)},
        "transform": transform,
        "hierarchy": {"parent": INVALID_ENTITY, "children": []},
    }


def material_component() -> dict[str, object]:
    # ModelRenderer exposes glTF material factors per submesh.  The negative
    # scalar values are the engine's fallback sentinel, while the use* flags
    # allow embedded GLB textures to be sampled when the submesh has them.
    return {
        "albedoPath": "",
        "normalPath": "",
        "roughnessPath": "",
        "metallicPath": "",
        "aoPath": "",
        "emissivePath": "",
        "albedoColor": [1.0, 1.0, 1.0],
        "metallic": -1.0,
        "roughness": -1.0,
        "ao": -1.0,
        "emissiveIntensity": 0.0,
        "useAlbedoTexture": True,
        "useNormalTexture": True,
        "useRoughnessTexture": True,
        "useMetallicTexture": True,
        "useAOTexture": True,
        "useEmissiveTexture": True,
        "albedoSamplerType": 0,
        "normalSamplerType": 0,
        "roughnessSamplerType": 0,
        "metallicSamplerType": 0,
        "aoSamplerType": 0,
        "emissiveSamplerType": 0,
    }


def camera_component(obj: bpy.types.Object, is_main: bool) -> dict[str, object]:
    # Real Blender camera objects expose .data.  The fallback camera created
    # by the converter passes a small camera-data-like object directly.
    data = getattr(obj, "data", obj)
    camera = {
        "fov": max(1.0, min(179.0, math.degrees(finite_float(getattr(data, "angle", math.radians(60.0)), math.radians(60.0))))),
        "nearPlane": max(0.001, finite_float(getattr(data, "clip_start", 0.1), 0.1)),
        "farPlane": max(1.0, finite_float(getattr(data, "clip_end", 1000.0), 1000.0)),
        "isMainCamera": bool(is_main),
        "isOrthographic": getattr(data, "type", "PERSP") == "ORTHO",
        "orthographicSize": max(0.001, finite_float(getattr(data, "ortho_scale", 5.0), 5.0) * 0.5),
        "enableFrustumCulling": True,
        "showFrustumWireframe": False,
        "useSubMeshCulling": False,
        "showBVHWireframe": False,
        "showCollisionWireframe": False,
        "useBVHCulling": False,
        "thirdPersonEnabled": False,
        "thirdPersonTargetName": "",
        "thirdPersonTargetOffset": [0.0, 1.5, 0.0],
        "thirdPersonDistance": 5.0,
        "thirdPersonMinDistance": 2.0,
        "thirdPersonMaxDistance": 12.0,
        "thirdPersonYaw": 180.0,
        "thirdPersonPitch": 15.0,
        "thirdPersonMinPitch": -25.0,
        "thirdPersonMaxPitch": 70.0,
        "thirdPersonOrbitSensitivity": 0.12,
        "thirdPersonZoomSensitivity": 1.0,
        "thirdPersonPositionDamping": 12.0,
        "thirdPersonRotationDamping": 16.0,
        "thirdPersonCaptureMouse": False,
        "thirdPersonPreserveDistanceWhenOccluded": True,
        "thirdPersonCollisionEnabled": True,
        "thirdPersonCollisionRadius": 0.2,
        "thirdPersonCollisionBuffer": 0.08,
        "thirdPersonCollisionMinDistance": 0.75,
        "thirdPersonCollisionDampingIn": 24.0,
        "thirdPersonCollisionDampingOut": 6.0,
        "thirdPersonCollisionSmoothingTime": 0.12,
        "thirdPersonAimEnabled": False,
        "thirdPersonAimShoulderOffset": 0.75,
        "thirdPersonAimFov": 50.0,
        "thirdPersonAimSensitivity": 0.08,
        "thirdPersonAimPositionDamping": 18.0,
        "thirdPersonAimRotationDamping": 20.0,
        "thirdPersonLockOnEnabled": False,
        "thirdPersonLockTargetName": "",
        "thirdPersonLockOnMaxDistance": 25.0,
        "thirdPersonLockOnLookAtBlend": 0.5,
    }
    if getattr(data, "type", "PERSP") == "ORTHO":
        camera["fov"] = 60.0
    return camera


def light_component(obj: bpy.types.Object) -> tuple[dict[str, object] | None, str | None]:
    data = obj.data
    light_type = getattr(data, "type", "")
    type_map = {"SUN": 0, "POINT": 1, "SPOT": 2}
    if light_type not in type_map:
        return None, f"Unsupported Blender light '{obj.name}' of type '{light_type}' (Area lights are not exported)."

    energy = max(0.01, finite_float(getattr(data, "energy", 1.0), 1.0))
    if light_type == "SUN":
        intensity = min(32.0, energy)
        light_range = 1000.0
    else:
        # Mikan's current point-light shader uses an engine-scale intensity;
        # Blender watt values are kept in the manifest and converted gently.
        intensity = min(100.0, max(0.01, energy / 100.0))
        light_range = max(0.1, finite_float(getattr(data, "cutoff_distance", 25.0), 25.0))

    color = getattr(data, "color", (1.0, 1.0, 1.0))
    component = {
        "type": type_map[light_type],
        "color": [finite_float(color[0], 1.0), finite_float(color[1], 1.0), finite_float(color[2], 1.0)],
        "intensity": intensity,
        "range": light_range,
        "spotAngle": max(1.0, min(179.0, math.degrees(finite_float(getattr(data, "spot_size", math.radians(45.0)), math.radians(45.0))))),
        "castShadow": bool(getattr(data, "use_shadow", True)),
    }
    return component, None


def water_component(obj: bpy.types.Object) -> dict[str, object]:
    dimensions = getattr(obj, "dimensions", Vector((32.0, 32.0, 0.0)))
    size_x = max(0.1, abs(finite_float(dimensions.x, 32.0)))
    size_z = max(0.1, abs(finite_float(dimensions.y, 32.0)))
    color = custom_value(obj, "mikan_color", (0.035, 0.22, 0.32))
    if not isinstance(color, (list, tuple, Vector)) or len(color) < 3:
        color = (0.035, 0.22, 0.32)
    return {
        "enabled": custom_bool(obj, "mikan_enabled", True),
        "size": [size_x, size_z],
        "surfaceOffset": finite_float(custom_value(obj, "mikan_surface_offset", 0.0)),
        "depth": max(0.0, finite_float(custom_value(obj, "mikan_depth", 20.0), 20.0)),
        "buoyancy": max(0.0, finite_float(custom_value(obj, "mikan_buoyancy", 1.15), 1.15)),
        "drag": max(0.0, finite_float(custom_value(obj, "mikan_drag", 2.0), 2.0)),
        "color": [finite_float(color[0], 0.035), finite_float(color[1], 0.22), finite_float(color[2], 0.32)],
        "roughness": max(0.0, min(1.0, finite_float(custom_value(obj, "mikan_roughness", 0.12), 0.12))),
        "affectPlayersOnly": custom_bool(obj, "mikan_affect_players_only", True),
    }


def look_at_rotation(position: Vector, target: Vector) -> object:
    forward = target - position
    if forward.length < 1.0e-5:
        forward = Vector((0.0, 0.0, -1.0))
    forward.normalize()
    up_reference = Vector((0.0, 1.0, 0.0))
    if abs(forward.dot(up_reference)) > 0.99:
        up_reference = Vector((0.0, 0.0, 1.0))
    right = forward.cross(up_reference).normalized()
    up = right.cross(forward).normalized()
    rotation_matrix = Matrix((right, up, -forward)).transposed()
    return rotation_matrix.to_quaternion()


def bounds_for_objects(objects: list[bpy.types.Object]) -> tuple[Vector, Vector] | None:
    if not objects:
        return None
    minimum = Vector((float("inf"), float("inf"), float("inf")))
    maximum = Vector((float("-inf"), float("-inf"), float("-inf")))
    for obj in objects:
        for corner in obj.bound_box:
            blender_world = obj.matrix_world @ Vector(corner)
            engine_world = ENGINE_BASIS @ blender_world.to_4d()
            point = Vector((engine_world.x, engine_world.y, engine_world.z))
            minimum.x = min(minimum.x, point.x)
            minimum.y = min(minimum.y, point.y)
            minimum.z = min(minimum.z, point.z)
            maximum.x = max(maximum.x, point.x)
            maximum.y = max(maximum.y, point.y)
            maximum.z = max(maximum.z, point.z)
    if any(not math.isfinite(value) for value in (*minimum, *maximum)):
        return None
    return minimum, maximum


def relative_asset_path(path: Path, asset_root: Path) -> str:
    try:
        return path.resolve().relative_to(asset_root.resolve()).as_posix()
    except ValueError:
        parts = list(path.resolve().parts)
        for index, part in enumerate(parts):
            if part.lower() == "assets" and index + 1 < len(parts):
                return "/".join(parts[index + 1 :])
        return path.name


def related_armatures(render_objects: list[bpy.types.Object]) -> list[bpy.types.Object]:
    """Find armatures that actually skin one of the exported render meshes."""
    result: list[bpy.types.Object] = []
    seen: set[int] = set()
    for obj in render_objects:
        candidates = []
        if obj.parent and obj.parent.type == "ARMATURE":
            candidates.append(obj.parent)
        for modifier in obj.modifiers:
            if modifier.type == "ARMATURE" and modifier.object and modifier.object.type == "ARMATURE":
                candidates.append(modifier.object)
        for armature in candidates:
            if id(armature) not in seen:
                seen.add(id(armature))
                result.append(armature)
    return sorted(result, key=lambda value: value.name.lower())


RETARGET_BONE_PAIRS = {
    "root": "Root",
    "DEF-hips": "Bip001 Pelvis",
    "DEF-spine.001": "Bip001 Spine",
    "DEF-spine.002": "Bip001 Spine1",
    "DEF-spine.003": "Bip001 Spine2",
    "DEF-neck": "Bip001 Neck",
    "DEF-head": "Bip001 Head",
    "DEF-shoulder.L": "Bip001 L Clavicle",
    "DEF-upper_arm.L": "Bip001 L UpperArm",
    "DEF-forearm.L": "Bip001 L Forearm",
    "DEF-hand.L": "Bip001 L Hand",
    "DEF-shoulder.R": "Bip001 R Clavicle",
    "DEF-upper_arm.R": "Bip001 R UpperArm",
    "DEF-forearm.R": "Bip001 R Forearm",
    "DEF-hand.R": "Bip001 R Hand",
    "DEF-thigh.L": "Bip001 L Thigh",
    "DEF-shin.L": "Bip001 L Calf",
    "DEF-foot.L": "Bip001 L Foot",
    "DEF-toe.L": "Bip001 L Toe0",
    "DEF-thigh.R": "Bip001 R Thigh",
    "DEF-shin.R": "Bip001 R Calf",
    "DEF-foot.R": "Bip001 R Foot",
    "DEF-toe.R": "Bip001 R Toe0",
}


def add_finger_retarget_pairs() -> dict[str, str]:
    pairs = dict(RETARGET_BONE_PAIRS)
    for side in ("L", "R"):
        for source_prefix, target_prefix in (
            ("index", "Finger1"),
            ("middle", "Finger2"),
            ("ring", "Finger3"),
            ("pinky", "Finger4"),
        ):
            for segment in ("01", "02", "03"):
                source = f"DEF-f_{source_prefix}.{segment}.{side}"
                target_segment = {"01": "", "02": "1", "03": "2"}[segment]
                pairs[source] = f"Bip001 {side} {target_prefix}{target_segment}"
        for segment in ("01", "02", "03"):
            target_segment = {"01": "", "02": "1", "03": "2"}[segment]
            pairs[f"DEF-thumb.{segment}.{side}"] = f"Bip001 {side} Finger0{target_segment}"
    return pairs


RETARGET_BONE_PAIRS = add_finger_retarget_pairs()


def bone_depth(bone) -> int:
    depth = 0
    parent = bone.parent
    while parent is not None:
        depth += 1
        parent = parent.parent
    return depth


def reset_pose_to_rest(armature: bpy.types.Object) -> None:
    for pose_bone in armature.pose.bones:
        pose_bone.rotation_mode = "QUATERNION"
        pose_bone.matrix_basis = Matrix.Identity(4)
    bpy.context.view_layer.update()


def rest_relative_matrix(pose_bone) -> Matrix:
    """Return a bone's rest transform relative to its rest parent."""
    if pose_bone.bone.parent is None:
        return pose_bone.bone.matrix_local.copy()
    return pose_bone.bone.parent.matrix_local.inverted() @ pose_bone.bone.matrix_local


def pose_relative_matrix(pose_bone) -> Matrix:
    """Return a posed bone transform relative to its currently posed parent."""
    if pose_bone.parent is None:
        return pose_bone.matrix.copy()
    return pose_bone.parent.matrix.inverted() @ pose_bone.matrix


def rotation_only(matrix: Matrix) -> Matrix:
    """Return a normalized 4x4 rotation matrix without translation or scale."""
    _, rotation, _ = matrix.decompose()
    return rotation.to_matrix().to_4x4()


def retarget_animation_library(
    target_armatures: list[bpy.types.Object], animation_source: Path, warnings: list[str]
) -> dict[str, object]:
    """Import a standard humanoid animation library and bake it onto the target rig.

    MMD/Bip001-style armatures may differ from the target rig's animation
    skeleton and clip layout.  The retarget is done
    in rest-pose rotation offsets, then baked back to target local
    pose channels while preserving the target rig's local joint translations.
    That preservation is important for MMD twist bones and child heads that
    do not sit exactly on parent tails.  This keeps the engine's clip-index
    based AnimatorComponent compatible with the prototype while allowing the
    target mesh to retain its own skin weights and extra hair/cloth bones.
    """
    result: dict[str, object] = {
        "enabled": True,
        "source": str(animation_source),
        "targetArmature": "",
        "retargetMethod": "local_rest_rotation_delta",
        "bonePairs": 0,
        "clips": [],
        "warnings": [],
    }
    if not target_armatures:
        message = "Animation retarget requested, but no armature is bound to the exported meshes."
        warnings.append(message)
        result["warnings"].append(message)
        return result
    if not animation_source.is_file():
        message = f"Animation source does not exist: {animation_source}"
        warnings.append(message)
        result["warnings"].append(message)
        return result

    target_armature = target_armatures[0]
    result["targetArmature"] = target_armature.name
    before_objects = {id(obj) for obj in bpy.data.objects}
    original_actions = list(bpy.data.actions)
    before_actions = {id(action) for action in original_actions}
    try:
        bpy.ops.import_scene.gltf(filepath=str(animation_source))
    except Exception as exc:
        message = f"Could not import animation source '{animation_source}': {exc}"
        warnings.append(message)
        result["warnings"].append(message)
        return result

    imported_objects = [obj for obj in bpy.data.objects if id(obj) not in before_objects]
    source_armatures = [obj for obj in imported_objects if obj.type == "ARMATURE"]
    source_actions = [action for action in bpy.data.actions if id(action) not in before_actions]
    if not source_armatures or not source_actions:
        message = "Animation source imported, but no armature/actions were found for retargeting."
        warnings.append(message)
        result["warnings"].append(message)
        for obj in imported_objects:
            bpy.data.objects.remove(obj, do_unlink=True)
        return result

    source_armature = source_armatures[0]
    source_pose_bones = source_armature.pose.bones
    target_pose_bones = target_armature.pose.bones
    bone_pairs = []
    for source_name, target_name in RETARGET_BONE_PAIRS.items():
        source_pose = source_pose_bones.get(source_name)
        target_pose = target_pose_bones.get(target_name)
        if source_pose is not None and target_pose is not None:
            bone_pairs.append((source_pose, target_pose))
    bone_pairs.sort(key=lambda pair: bone_depth(pair[1].bone))
    result["bonePairs"] = len(bone_pairs)
    if len(bone_pairs) < 8:
        message = f"Only {len(bone_pairs)} retarget bone pairs matched; animation quality may be incomplete."
        warnings.append(message)
        result["warnings"].append(message)

    scene = bpy.context.scene
    original_frame = scene.frame_current
    original_start = scene.frame_start
    original_end = scene.frame_end
    target_armature.animation_data_create()
    target_armature.animation_data.action = None
    source_armature.animation_data_create()

    # The source rig contains IK/look-at constraints intended for interactive
    # authoring.  Retargeting writes explicit local pose keys, so keeping those
    # constraints active would modify the baked result and make the glTF
    # exporter resample every clip.  Remove them after baking; the source .blend
    # is never saved, and the exported GLB only needs the explicit keys.
    removed_constraints = 0
    for pose_bone in target_armature.pose.bones:
        while pose_bone.constraints:
            pose_bone.constraints.remove(pose_bone.constraints[0])
            removed_constraints += 1
    result["removedConstraints"] = removed_constraints

    retargeted_actions = []
    try:
        for source_action in source_actions:
            target_action = bpy.data.actions.new(name=source_action.name)
            target_action.use_fake_user = True
            target_armature.animation_data.action = target_action
            source_armature.animation_data.action = source_action

            start = float(source_action.frame_range[0])
            end = float(source_action.frame_range[1])
            first_frame = int(math.floor(start))
            last_frame = int(math.ceil(end))
            frame_values = [float(frame) for frame in range(first_frame, last_frame + 1)]
            if not frame_values:
                frame_values = [start]
            if abs(frame_values[-1] - end) > 1.0e-4:
                frame_values.append(end)

            for frame in frame_values:
                whole_frame = int(math.floor(frame))
                scene.frame_set(whole_frame, subframe=float(frame - whole_frame))
                reset_pose_to_rest(target_armature)
                bpy.context.view_layer.update()

                # Retarget the *local* rotation delta, not an absolute/world
                # rotation.  Both armatures have different rest orientations
                # and different parent chains; applying a world-space source
                # rotation here double-applies parent rotations and produces
                # the characteristic crossed legs / twisted torso seen in the
                # prototype.  The local rest-to-pose delta is independent of
                # those hierarchy differences and is then re-applied to the
                # target bone's own rest transform.
                for source_pose, target_pose in bone_pairs:
                    target_parent_pose = (
                        target_pose.parent.matrix.copy()
                        if target_pose.parent is not None
                        else Matrix.Identity(4)
                    )

                    source_rest_relative = rest_relative_matrix(source_pose)
                    source_pose_relative = pose_relative_matrix(source_pose)
                    source_rest_rotation = rotation_only(source_rest_relative)
                    source_pose_rotation = rotation_only(source_pose_relative)
                    source_delta_rotation = source_rest_rotation.inverted() @ source_pose_rotation

                    target_rest_relative = rest_relative_matrix(target_pose)
                    target_local = target_rest_relative @ source_delta_rotation

                    # Only root/hips are allowed to carry animation
                    # translation.  Ordinary limb bones must retain the
                    # target rig's joint locations, otherwise a small source
                    # skeleton difference turns into visible gaps or bent
                    # knees.  Keep the target rest translation as the base and
                    # add the source local displacement for these two bones.
                    source_delta = source_rest_relative.inverted() @ source_pose_relative
                    if source_pose.name in {"root", "DEF-hips"}:
                        target_local.translation = target_rest_relative.translation + source_delta.translation

                    target_pose.matrix = target_parent_pose @ target_local
                bpy.context.view_layer.update()
                for _, target_pose in bone_pairs:
                    target_pose.keyframe_insert(data_path="location", frame=frame, group=target_pose.name)
                    target_pose.keyframe_insert(data_path="rotation_quaternion", frame=frame, group=target_pose.name)
                    target_pose.keyframe_insert(data_path="scale", frame=frame, group=target_pose.name)

            retargeted_actions.append(target_action)
            result["clips"].append(
                {
                    "index": len(result["clips"]),
                    "name": target_action.name,
                    "sourceName": source_action.name,
                    "frameStart": start,
                    "frameEnd": end,
                    "frameCount": len(frame_values),
                }
            )
    finally:
        scene.frame_start = original_start
        scene.frame_end = original_end
        scene.frame_set(original_frame)
        # export_apply must see the original bind/rest deformation.  The
        # retarget baking leaves the last sampled pose active, so clear it
        # before the GLB exporter captures mesh and inverse-bind data.
        target_armature.animation_data.action = None
        reset_pose_to_rest(target_armature)
        bpy.context.view_layer.update()
        target_armature.animation_data.action = retargeted_actions[0] if retargeted_actions else None

    # Remove imported source objects and source actions.  The target actions
    # are now independent and are the only animations that the GLB exporter
    # needs to see on the selected target armature.
    for obj in imported_objects:
        bpy.data.objects.remove(obj, do_unlink=True)
    for action in source_actions:
        if action.name in {created.name for created in retargeted_actions}:
            continue
        try:
            bpy.data.actions.remove(action)
        except (RuntimeError, ReferenceError):
            pass
    created_action_ids = {id(action) for action in retargeted_actions}
    for action in original_actions:
        if id(action) in created_action_ids:
            continue
        try:
            bpy.data.actions.remove(action)
        except (RuntimeError, ReferenceError):
            pass
    return result


def export_glb(
    render_objects: list[bpy.types.Object], armatures: list[bpy.types.Object], output_path: Path
) -> None:
    for obj in bpy.context.selected_objects:
        obj.select_set(False)
    for obj in render_objects:
        obj.select_set(True)
    for armature in armatures:
        armature.select_set(True)
    if render_objects:
        bpy.context.view_layer.objects.active = render_objects[0]
    elif armatures:
        bpy.context.view_layer.objects.active = armatures[0]

    # Several MMD outline node groups in the source file produce no exportable
    # geometry in Blender's background glTF exporter.  Their base meshes are
    # valid and already contain the renderable character, so export those base
    # meshes while leaving the source scene untouched in memory.
    muted_geometry_nodes = []
    for obj in render_objects:
        for modifier in obj.modifiers:
            if modifier.type != "NODES":
                continue
            muted_geometry_nodes.append((modifier, modifier.show_viewport, modifier.show_render))
            modifier.show_viewport = False
            modifier.show_render = False
    bpy.context.view_layer.update()

    output_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        bpy.ops.export_scene.gltf(
            filepath=str(output_path),
            export_format="GLB",
            use_selection=True,
            use_visible=False,
            use_active_scene=True,
            export_yup=True,
            export_apply=True,
            export_animations=True,
            export_animation_mode="ACTIONS",
            export_bake_animation=False,
            export_skins=True,
            export_def_bones=True,
            export_morph=True,
            export_materials="EXPORT",
            export_image_format="AUTO",
            export_texcoords=True,
            export_normals=True,
            export_tangents=True,
            export_cameras=False,
            export_lights=False,
            export_extras=True,
        )
    finally:
        for modifier, show_viewport, show_render in muted_geometry_nodes:
            modifier.show_viewport = show_viewport
            modifier.show_render = show_render
        bpy.context.view_layer.update()


def inspect_scene() -> dict[str, object]:
    objects = list(bpy.data.objects)
    render_objects = [obj for obj in objects if is_render_mesh(obj)]
    water_objects = [obj for obj in objects if obj.type == "MESH" and is_water_object(obj)]
    camera_objects = [obj for obj in objects if obj.type == "CAMERA" and is_visible_object(obj)]
    light_objects = [obj for obj in objects if obj.type == "LIGHT" and is_visible_object(obj)]
    unsupported = []
    for obj in objects:
        if not is_visible_object(obj):
            continue
        if obj.type in {"MESH", "CAMERA", "LIGHT", "EMPTY"}:
            continue
        unsupported.append({"name": obj.name, "type": obj.type})
    counts: dict[str, int] = {}
    for obj in objects:
        counts[obj.type] = counts.get(obj.type, 0) + 1
    return {
        "objectsTotal": len(objects),
        "objectsByType": counts,
        "renderMeshes": len(render_objects),
        "waterObjects": len(water_objects),
        "cameras": len(camera_objects),
        "lights": len(light_objects),
        "unsupportedVisibleObjects": unsupported,
        "renderMeshNames": [obj.name for obj in render_objects],
        "waterNames": [obj.name for obj in water_objects],
        "cameraNames": [obj.name for obj in camera_objects],
        "lightNames": [obj.name for obj in light_objects],
    }


def build_scene(
    scene_name: str,
    render_objects: list[bpy.types.Object],
    water_objects: list[bpy.types.Object],
    camera_objects: list[bpy.types.Object],
    light_objects: list[bpy.types.Object],
    model_path: str,
    warnings: list[str],
) -> tuple[dict[str, object], list[dict[str, object]]]:
    entities: list[dict[str, object]] = []
    entity_id = 0

    if render_objects:
        render_entity = make_entity(entity_id, f"{scene_name} Render", transform_from_matrix(Matrix.Identity(4)))
        render_entity["mesh"] = {"type": 4, "modelPath": model_path}
        render_entity["render"] = {
            "visible": True,
            "castShadow": True,
            "receiveShadow": True,
            "showAABB": False,
            "showOBB": False,
            "doubleSided": False,
            "wireframe": False,
        }
        render_entity["material"] = material_component()
        entities.append(render_entity)
        entity_id += 1
    else:
        warnings.append("No visible render mesh was found; no mesh entity was generated.")

    main_camera = bpy.context.scene.camera if bpy.context.scene else None
    visible_cameras = [obj for obj in camera_objects if obj.data]
    for obj in visible_cameras:
        entity = make_entity(entity_id, obj.name, transform_from_matrix(obj.matrix_world))
        entity["camera"] = camera_component(obj, obj == main_camera or (main_camera is None and entity_id == 0))
        entities.append(entity)
        entity_id += 1

    if not visible_cameras:
        bounds = bounds_for_objects(render_objects)
        if bounds:
            minimum, maximum = bounds
            center = (minimum + maximum) * 0.5
            extent = max((maximum - minimum).length, 1.0)
            position = center + Vector((extent * 1.25, extent * 0.8, extent * 1.25))
            rotation = look_at_rotation(position, center)
            transform = transform_from_engine_pose(position, rotation)
            default_camera = make_entity(entity_id, "Mikan Default Camera", transform)
            camera = camera_component(type("CameraData", (), {"type": "PERSP", "angle": math.radians(60.0), "clip_start": max(0.01, extent / 1000.0), "clip_end": max(1000.0, extent * 10.0)})(), True)
            default_camera["camera"] = camera
            entities.append(default_camera)
            entity_id += 1
            warnings.append("Blender scene has no camera; generated Mikan Default Camera from mesh bounds.")
        else:
            warnings.append("Blender scene has no camera and no render bounds; no main camera was generated.")

    supported_light_count = 0
    for obj in light_objects:
        component, warning = light_component(obj)
        if warning:
            warnings.append(warning)
        if component is None:
            continue
        entity = make_entity(entity_id, obj.name, transform_from_matrix(obj.matrix_world))
        entity["light"] = component
        entities.append(entity)
        entity_id += 1
        supported_light_count += 1

    if supported_light_count == 0:
        default_light = make_entity(
            entity_id,
            "Mikan Default Sun",
            transform_from_engine_pose(Vector((0.0, 10.0, 0.0)), look_at_rotation(Vector((0.0, 10.0, 0.0)), Vector((0.0, 0.0, 0.0)))),
        )
        default_light["light"] = {
            "type": 0,
            "color": [1.0, 1.0, 1.0],
            "intensity": 1.0,
            "range": 1000.0,
            "spotAngle": 45.0,
            "castShadow": True,
        }
        entities.append(default_light)
        warnings.append("No supported SUN/POINT/SPOT light was found; generated Mikan Default Sun.")

    for obj in water_objects:
        entity = make_entity(entity_id, obj.name, transform_from_matrix(obj.matrix_world, include_scale=False))
        entity["water"] = water_component(obj)
        entities.append(entity)
        entity_id += 1
        if custom_value(obj, "mikan_mask", None) is not None or custom_value(obj, "water_mask", None) is not None:
            warnings.append(
                f"Water object '{obj.name}' declares a mask, but the current Mikan WaterComponent has no maskPath field; kept it in manifest metadata."
            )

    scene = {"game": "", "entities": entities}
    return scene, entities


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output).expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    source_path = Path(bpy.data.filepath).expanduser().resolve()
    scene_name = clean_name(args.scene_name or source_path.stem, "Blender Scene")
    asset_root = Path(args.asset_root).expanduser().resolve() if args.asset_root else output_dir

    inspection = inspect_scene()
    render_objects = [obj for obj in bpy.data.objects if is_render_mesh(obj)]
    armatures = related_armatures(render_objects)
    water_objects = [obj for obj in bpy.data.objects if obj.type == "MESH" and is_water_object(obj) and is_visible_object(obj)]
    camera_objects = [obj for obj in bpy.data.objects if obj.type == "CAMERA" and is_visible_object(obj)]
    light_objects = [obj for obj in bpy.data.objects if obj.type == "LIGHT" and is_visible_object(obj)]

    inspection["skinnedMeshes"] = [obj.name for obj in render_objects if any(
        modifier.type == "ARMATURE" and modifier.object for modifier in obj.modifiers
    )]
    inspection["armatures"] = [
        {"name": armature.name, "bones": len(armature.data.bones)} for armature in armatures
    ]

    warnings: list[str] = []
    related_armature_names = {armature.name for armature in armatures}
    for item in inspection["unsupportedVisibleObjects"]:
        if item["type"] == "ARMATURE" and item["name"] in related_armature_names:
            continue
        warnings.append(f"Unsupported visible Blender object '{item['name']}' of type '{item['type']}' was not exported.")

    glb_path = output_dir / f"{source_path.stem}.glb"
    scene_path = output_dir / "scene.mikan.json"
    manifest_path = output_dir / "manifest.json"
    inspect_path = output_dir / "inspect.json"

    write_json(inspect_path, inspection)
    manifest: dict[str, object] = {
        "format": "mikan-blend-import-manifest-v1",
        "status": "inspect-only" if args.inspect_only else "converted",
        "generatedAtUtc": datetime.datetime.utcnow().isoformat(timespec="seconds") + "Z",
        "sourceBlend": str(source_path),
        "sceneName": scene_name,
        "blenderVersion": bpy.app.version_string,
        "inspection": inspection,
        "skeleton": {
            "armatures": inspection["armatures"],
            "skinnedMeshes": inspection["skinnedMeshes"],
        },
        "output": {
            "glb": glb_path.name if not args.inspect_only else "",
            "scene": scene_path.name if not args.inspect_only else "",
            "inspect": inspect_path.name,
        },
        "warnings": warnings,
        "customProperties": {},
    }
    animation_info: dict[str, object] = {"enabled": False, "clips": [], "warnings": []}

    # Keep custom properties in the manifest for future importer iterations;
    # scene JSON only contains fields currently understood by Mikan.
    custom_properties: dict[str, object] = {}
    for obj in bpy.data.objects:
        if len(obj.keys()) == 0:
            continue
        try:
            custom_properties[obj.name] = {str(key): json_safe(obj[key]) for key in obj.keys()}
        except Exception:
            custom_properties[obj.name] = {"warning": "Could not serialize one or more custom properties."}
    manifest["customProperties"] = custom_properties

    if not args.inspect_only:
        if args.retarget_animations:
            if not args.animation_source:
                message = "Retarget requested but --animation-source was not supplied; exporting the target rig without retargeted clips."
                warnings.append(message)
                animation_info["warnings"].append(message)
            else:
                animation_info = retarget_animation_library(armatures, Path(args.animation_source).expanduser().resolve(), warnings)
        manifest["animationRetarget"] = animation_info
        if not render_objects:
            warnings.append("GLB export skipped because the scene has no visible render mesh.")
        else:
            export_glb(render_objects, armatures, glb_path)
            if not glb_path.is_file() or glb_path.stat().st_size < 20:
                raise RuntimeError(f"Blender did not produce a valid GLB at {glb_path}")

        model_path = relative_asset_path(glb_path, asset_root) if render_objects else ""
        scene, entities = build_scene(scene_name, render_objects, water_objects, camera_objects, light_objects, model_path, warnings)
        manifest["warnings"] = warnings
        manifest["output"]["entityCount"] = len(entities)
        write_json(scene_path, scene)

    manifest["warnings"] = warnings
    write_json(manifest_path, manifest)
    print(f"[mikan-blend] source: {source_path}")
    animation_count = len(animation_info.get("clips", []))
    print(f"[mikan-blend] objects: {inspection['objectsTotal']} (render meshes={inspection['renderMeshes']}, skinned={len(inspection['skinnedMeshes'])}, armatures={len(armatures)}, cameras={inspection['cameras']}, lights={inspection['lights']}, water={inspection['waterObjects']}, clips={animation_count})")
    print(f"[mikan-blend] output: {output_dir}")
    if warnings:
        print(f"[mikan-blend] warnings: {len(warnings)}")
        for warning in warnings:
            print(f"[mikan-blend][warn] {warning}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"[mikan-blend][error] {exc}", file=sys.stderr)
        traceback.print_exc()
        raise
