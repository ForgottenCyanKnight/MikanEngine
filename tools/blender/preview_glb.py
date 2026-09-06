"""Render one baked GLB animation frame for quick retargeting inspection."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import bpy
from mathutils import Vector


def parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--action", default="Idle_Loop.001")
    parser.add_argument("--frame", type=float, default=1.0)
    parser.add_argument("--distance", type=float, default=1.6)
    return parser.parse_args(argv)


def main() -> None:
    args = parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(Path(args.input).resolve()))

    armatures = [obj for obj in bpy.data.objects if obj.type == "ARMATURE"]
    if not armatures:
        raise RuntimeError("The GLB contains no armature")
    armature = armatures[0]
    actions = list(bpy.data.actions)
    action = bpy.data.actions.get(args.action)
    if action is None:
        prefix = args.action.split(".", 1)[0]
        action = next((candidate for candidate in actions if candidate.name.startswith(prefix)), None)
    if action is None:
        raise RuntimeError(f"Animation action not found: {args.action}; available={len(actions)}")

    armature.animation_data_create()
    armature.animation_data.action = action
    scene = bpy.context.scene
    scene.frame_set(int(args.frame), subframe=args.frame - int(args.frame))
    bpy.context.view_layer.update()

    mesh_objects = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    corners = [obj.matrix_world @ Vector(corner) for obj in mesh_objects for corner in obj.bound_box]
    if not corners:
        raise RuntimeError("The GLB contains no mesh")
    minimum = Vector((min(point.x for point in corners), min(point.y for point in corners), min(point.z for point in corners)))
    maximum = Vector((max(point.x for point in corners), max(point.y for point in corners), max(point.z for point in corners)))
    center = (minimum + maximum) * 0.5
    size = maximum - minimum
    radius = max(size.length * 0.5, 0.5)

    camera_offset = Vector((radius * 0.75, -radius * args.distance, radius * 0.20))
    bpy.ops.object.camera_add(location=center + camera_offset)
    camera = bpy.context.object
    camera.data.lens = 58.0
    camera.rotation_euler = (center - camera.location).to_track_quat("-Z", "Y").to_euler()
    scene.camera = camera

    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.studio_light = "paint.sl"
    scene.display.shading.color_type = "MATERIAL"
    scene.display.shading.show_shadows = True
    scene.display.shading.show_cavity = True
    scene.display.shading.cavity_type = "WORLD"
    scene.display.shading.curvature_ridge_factor = 1.5
    scene.display.shading.curvature_valley_factor = 1.5
    scene.display.shading.background_type = "WORLD"
    scene.display.shading.background_color = (0.055, 0.075, 0.10)
    scene.render.resolution_x = 700
    scene.render.resolution_y = 900
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = str(Path(args.output).resolve())
    bpy.ops.render.render(write_still=True)
    print(f"[preview_glb] action={action.name} frame={args.frame} output={scene.render.filepath}")


if __name__ == "__main__":
    main()
