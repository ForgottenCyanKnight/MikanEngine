"""Minimal editor import: evaluated Blender scene -> self-contained GLB."""
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Matrix


def apply_object_material_variants():
    options_path = Path(bpy.data.filepath + ".import.json")
    options = json.loads(options_path.read_text(encoding="utf-8-sig")) if options_path.is_file() else {}
    variants = {}
    for name, variant in options.get("objectMaterialVariants", {}).items():
        obj = bpy.data.objects.get(name)
        if not obj:
            continue
        for slot in obj.material_slots:
            original = slot.material
            if not original:
                continue
            key = (original, variant)
            if key not in variants:
                variants[key] = original.copy()
                variants[key].name = variant
            # Override at object level so a shared mesh does not modify doors.
            slot.link = "OBJECT"
            slot.material = variants[key]
            print(f"[BlendImport] material variant: {name} -> {variants[key].name}")


def realize_collection_instances():
    hidden_support_data = {obj.data for obj in bpy.context.scene.objects
                           if obj.type == "MESH" and obj.instance_type in {"FACES", "VERTS"}
                           and not obj.show_instancer_for_render}
    # Collection instances may otherwise export as empty nodes when visible
    # filtering hides their source objects. Materialize only in this temporary
    # Blender process, retaining the evaluated placement and hierarchy.
    instances = [obj for obj in bpy.context.scene.objects
                 if obj.instance_type == "COLLECTION" and obj.instance_collection
                 and obj.visible_get() and not obj.hide_render]
    for obj in instances:
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.duplicates_make_real(use_base_parent=True, use_hierarchy=True)
        print(f"[BlendImport] realized collection instance: {obj.name}")
    # glTF may export a face/vertex instancer's support mesh despite Blender's
    # Show Instancer being disabled. Realize its contents before removing ONLY
    # that support geometry; hiding its parent could also lose the instances.
    helpers = [obj for obj in bpy.context.scene.objects
               if obj.type == "MESH" and obj.instance_type in {"FACES", "VERTS"}
               and not obj.show_instancer_for_render and obj.visible_get() and not obj.hide_render]
    for obj in helpers:
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.duplicates_make_real(use_base_parent=True, use_hierarchy=True)
        obj.data = bpy.data.meshes.new(f"{obj.name}_hidden_instancer")
        print(f"[BlendImport] removed hidden instancer support mesh: {obj.name}")
    # Collection realization resets instance_type on copies. Their shared source
    # mesh identity still identifies hidden support geometry (e.g. Plane.033),
    # so do not mistake these copies for visible, unmaterialed scene geometry.
    for obj in list(bpy.context.scene.objects):
        if obj.type == "MESH" and obj.data in hidden_support_data:
            obj.data = bpy.data.meshes.new(f"{obj.name}_hidden_instancer_copy")
            print(f"[BlendImport] removed hidden instancer support copy: {obj.name}")


def texture_constant_coverage():
    options_path = Path(bpy.data.filepath + ".import.json")
    options = json.loads(options_path.read_text(encoding="utf-8-sig")) if options_path.is_file() else {}
    overrides = options.get("materialOverrides", {})
    # The current model runtime consumes texture alpha, not baseColorFactor.a.
    # Encode constant alpha without changing its authored value. Keep the RGB
    # input unchanged; only an opaque white texture's alpha drives coverage.
    for material in bpy.data.materials:
        if not material.use_nodes:
            continue
        tree = material.node_tree
        for node in list(tree.nodes):
            if node.type != "BSDF_PRINCIPLED":
                continue
            alpha = node.inputs.get("Alpha")
            rule = overrides.get(material.name, {})
            if alpha and "alpha" in rule:
                for link in list(alpha.links):
                    tree.links.remove(link)
                alpha.default_value = max(0.0, min(1.0, float(rule["alpha"])))
            if not alpha or alpha.is_linked or alpha.default_value >= 1.0:
                continue
            color = node.inputs["Base Color"]
            if color.is_linked:
                continue
            image = bpy.data.images.new(f"{material.name}_coverage", width=1, height=1, alpha=True)
            texture = tree.nodes.new("ShaderNodeTexImage")
            texture.image = image
            # glTF needs a base-color texture to carry the alpha channel.
            tint = tuple(color.default_value)
            encoded = [12.92 * v if v <= 0.0031308 else 1.055 * v ** (1.0 / 2.4) - 0.055
                       for v in tint[:3]]
            image.pixels[:] = (*encoded, alpha.default_value)
            image.pack()
            tree.links.new(texture.outputs["Color"], color)
            tree.links.new(texture.outputs["Alpha"], alpha)
            print(f"[BlendImport] encoded constant coverage: {material.name}")


def simplify_constant_shaders():
    # glTF does not reliably reduce a Glass/Transparent Mix Shader, even when
    # its constant factor selects only Transparent. Preserve that exact branch
    # rather than importing an opaque white pane.
    transparent_materials = set()
    for material in bpy.data.materials:
        if not material.use_nodes or not material.node_tree:
            continue
        tree = material.node_tree
        for node in list(tree.nodes):
            if node.type != "MIX_SHADER" or node.inputs[0].is_linked:
                continue
            factor = node.inputs[0].default_value
            if factor not in (0.0, 1.0):
                continue
            branch = node.inputs[1 if factor == 0.0 else 2]
            if branch.is_linked:
                source = branch.links[0].from_socket
                for link in list(node.outputs[0].links):
                    tree.links.new(source, link.to_socket)
        for output in tree.nodes:
            if output.type != "OUTPUT_MATERIAL" or not output.is_active_output:
                continue
            surface = output.inputs["Surface"]
            if not surface.is_linked or surface.links[0].from_node.type != "BSDF_TRANSPARENT":
                continue
            transparent = surface.links[0].from_node
            color = transparent.inputs["Color"]
            if color.is_linked or any(abs(value - 1.0) > 1e-6 for value in color.default_value[:3]):
                continue
            transparent_materials.add(material)
            bsdf = tree.nodes.new("ShaderNodeBsdfPrincipled")
            bsdf.inputs["Alpha"].default_value = 0.0
            # Keep coverage explicit for runtimes that consume texture alpha.
            image = bpy.data.images.new(f"{material.name}_transparent", width=1, height=1, alpha=True)
            image.pixels[:] = (1.0, 1.0, 1.0, 0.0)
            image.pack()
            texture = tree.nodes.new("ShaderNodeTexImage")
            texture.image = image
            tree.links.new(texture.outputs["Color"], bsdf.inputs["Base Color"])
            tree.links.new(texture.outputs["Alpha"], bsdf.inputs["Alpha"])
            tree.links.new(bsdf.outputs[0], surface)
            print(f"[BlendImport] preserved fully transparent surface: {material.name}")
    for obj in bpy.context.scene.objects:
        if obj.type == "MESH" and obj.material_slots and all(
                slot.material in transparent_materials for slot in obj.material_slots):
            # A white, fully transparent surface has no geometry interaction.
            obj.hide_render = True


def convert_toon_materials():
    # Shader-to-RGB is a view/light-dependent Eevee shading trick, not emitted
    # radiance. Use its authored diffuse image as portable PBR input; do not
    # export the display Emission node as a real area light.
    converted = 0
    unsupported = []
    for material in bpy.data.materials:
        if not material.use_nodes or not material.node_tree:
            continue
        tree = material.node_tree
        if not any(node.type == "SHADERTORGB" for node in tree.nodes):
            continue
        source = tree.nodes.get("mmd_base_tex")
        if not source or source.type != "TEX_IMAGE" or not source.image:
            unsupported.append(material.name)
            continue
        image = source.image
        tree.nodes.clear()
        texture = tree.nodes.new("ShaderNodeTexImage")
        texture.image = image
        bsdf = tree.nodes.new("ShaderNodeBsdfPrincipled")
        bsdf.inputs["Metallic"].default_value = 0.0
        bsdf.inputs["Roughness"].default_value = 1.0
        sink = tree.nodes.new("ShaderNodeOutputMaterial")
        tree.links.new(texture.outputs["Color"], bsdf.inputs["Base Color"])
        if image.alpha_mode != "NONE":
            tree.links.new(texture.outputs["Alpha"], bsdf.inputs["Alpha"])
        tree.links.new(bsdf.outputs[0], sink.inputs["Surface"])
        converted += 1
        print(f"[BlendImport] non-emissive PBR fallback for toon material: {material.name}")
    return converted, unsupported


def toon_diffuse_color(tree, node):
    """Authored look of an Eevee toon chain: the diffuse colour feeding it.

    Keeping the colour socket's original subgraph (ramps, noise, images)
    lets Cycles evaluate the authored pattern directly during the bake.
    """
    shader_input = node.inputs["Shader"]
    if shader_input.is_linked and shader_input.links[0].from_node.type == "BSDF_DIFFUSE":
        color = shader_input.links[0].from_node.inputs["Color"]
        if color.is_linked:
            return color.links[0].from_socket
        rgb = tree.nodes.new("ShaderNodeRGB")
        rgb.outputs[0].default_value = color.default_value
        return rgb.outputs[0]
    # Glossy/specular captures are view-dependent highlights, not colour.
    black = tree.nodes.new("ShaderNodeRGB")
    black.outputs[0].default_value = (0, 0, 0, 1)
    return black.outputs[0]


def radiance_expression(tree, node, cache):
    """Reduce a closure graph into one colour expression for EMIT baking.

    Emission keeps its radiance, Mix/Add shaders compose recursively, and
    Shader-to-RGB collapses to its diffuse colour input (the authored look).
    Remaining closures (glossy highlights, glass, transparency) shade black:
    view-dependent components a portable bake cannot represent. The colour
    subgraphs feeding those sockets are evaluated natively by Cycles.
    """
    if node in cache:
        return cache[node]
    black = tree.nodes.new("ShaderNodeRGB")
    black.outputs[0].default_value = (0, 0, 0, 1)
    result = black.outputs[0]
    if node.type == "EMISSION":
        multiply = tree.nodes.new("ShaderNodeVectorMath")
        multiply.operation = "SCALE"
        for source, destination in ((node.inputs["Color"], multiply.inputs[0]),
                                    (node.inputs["Strength"], multiply.inputs[3])):
            if source.is_linked:
                tree.links.new(source.links[0].from_socket, destination)
            else:
                value = source.default_value
                destination.default_value = value[:3] if hasattr(value, "__len__") else value
        result = multiply.outputs["Vector"]
    elif node.type in {"MIX_SHADER", "ADD_SHADER"}:
        mix = tree.nodes.new("ShaderNodeMixRGB")
        mix.blend_type = "MIX" if node.type == "MIX_SHADER" else "ADD"
        mix.use_clamp = False
        if node.type == "MIX_SHADER":
            factor = node.inputs[0]
            if factor.is_linked:
                tree.links.new(factor.links[0].from_socket, mix.inputs[0])
            else:
                mix.inputs[0].default_value = factor.default_value
            branches = node.inputs[1:3]
        else:
            mix.inputs[0].default_value = 1.0
            branches = node.inputs[:2]
        for source, destination in zip(branches, mix.inputs[1:3]):
            if source.is_linked:
                tree.links.new(radiance_expression(tree, source.links[0].from_node, cache), destination)
            else:
                destination.default_value = (0, 0, 0, 1)
        result = mix.outputs[0]
    elif node.type == "SHADERTORGB":
        result = toon_diffuse_color(tree, node)
    cache[node] = result
    return result


def bake_material_radiance(obj, slot, width=1024, height=1024):
    """Bake one material slot's reduced radiance into a packed colour image.

    Per-object copies are required even for shared materials: procedural
    textures driven by object coordinates must be evaluated with each
    object's own transform and UV layout. Bakes EMIT only, never scene
    lighting; the engine relights the surface itself.
    """
    scene = bpy.context.scene
    previous_engine = scene.render.engine
    material = slot.material.copy()
    slot.material = material
    tree = material.node_tree
    sink = next(n for n in tree.nodes if n.type == "OUTPUT_MATERIAL" and n.is_active_output)
    cache = {}
    reduced = radiance_expression(tree, sink.inputs["Surface"].links[0].from_node, cache)
    baked_emission = tree.nodes.new("ShaderNodeEmission")
    tree.links.new(reduced, baked_emission.inputs["Color"])
    tree.links.new(baked_emission.outputs[0], sink.inputs["Surface"])
    image = bpy.data.images.new(f"{obj.name}_bake", width=width, height=height,
                                alpha=False, float_buffer=True)
    target = tree.nodes.new("ShaderNodeTexImage")
    target.image = image
    tree.nodes.active = target
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    try:
        scene.render.engine = "CYCLES"
        scene.cycles.device = "CPU"
        scene.cycles.samples = 1
        scene.render.bake.use_selected_to_active = False
        scene.render.bake.margin = 8
        bpy.ops.object.bake(type="EMIT")
    finally:
        scene.render.engine = previous_engine
    image.pack()
    return image


def image_to_u8(image):
    """Re-pack a float HDR bake as an 8-bit image.

    Blender's glTF exporter writes float-buffer images as 16-bit PNG, which
    stb-based loaders (SDL_image) cannot decode - every baked texture then
    falls back to the engine's default material. 8-bit is the portable norm.
    """
    import numpy as np
    pixels = np.empty(len(image.pixels), dtype=np.float32)
    image.pixels.foreach_get(pixels)
    rgba = np.clip(pixels.reshape((-1, 4)), 0.0, 1.0)
    converted = bpy.data.images.new(image.name, width=image.size[0], height=image.size[1],
                                    alpha=False, float_buffer=False)
    converted.pixels.foreach_set(rgba.ravel())
    converted.pack()
    return converted


def bake_procedural_emission(output):
    """Preserve emission-only procedural backdrops unsupported by glTF.

    Bake EMIT, never diffuse/direct/indirect lighting. Normalize HDR into an
    emissive texture plus its exact strength so the emitted radiance is retained.
    """
    import numpy as np
    scene = bpy.context.scene
    previous_engine = scene.render.engine
    baked = 0
    for obj in list(scene.objects):
        if obj.type != "MESH" or not obj.visible_get() or not obj.data.uv_layers:
            continue
        for slot in obj.material_slots:
            material = slot.material
            if not material or not material.use_nodes:
                continue
            types = {node.type for node in material.node_tree.nodes}
            if "EMISSION" not in types or not types.intersection({"TEX_NOISE", "TEX_VORONOI"}):
                continue
            if any(kind.startswith("BSDF_") and kind != "BSDF_TRANSPARENT" for kind in types):
                continue
            # Cycles' EMIT pass can discard contributions in an Add/Mix graph
            # containing Transparent BSDF. radiance_expression reduces that
            # emission-only closure algebra to a single radiance expression
            # before baking; this retains authored cloud colours without
            # baking scene illumination.
            image = bake_material_radiance(obj, slot)
            pixels = np.empty(len(image.pixels), dtype=np.float32)
            image.pixels.foreach_get(pixels)
            rgba = pixels.reshape((-1, 4))
            peak = max(1.0, float(np.max(rgba[:, :3])))
            rgba[:, :3] /= peak
            rgba[:, 3] = 1.0
            image.pixels.foreach_set(pixels)
            image = image_to_u8(image)
            tree = slot.material.node_tree
            tree.nodes.clear()
            texture = tree.nodes.new("ShaderNodeTexImage")
            texture.image = image
            emission = tree.nodes.new("ShaderNodeEmission")
            emission.inputs["Strength"].default_value = peak
            sink = tree.nodes.new("ShaderNodeOutputMaterial")
            tree.links.new(texture.outputs["Color"], emission.inputs["Color"])
            tree.links.new(emission.outputs[0], sink.inputs["Surface"])
            baked += 1
            print(f"[BlendImport] baked procedural emission: {obj.name}, strength={peak}")
    return baked


def bake_toon_materials(known_toon):
    """Convert Eevee Shader-to-RGB (toon) materials into baked PBR albedo.

    The authored look lives in the diffuse colour subgraph (ramps, noise,
    images); Shader-to-RGB itself is an Eevee-only lighting capture that
    glTF cannot represent and Cycles cannot evaluate. Bake the reduced
    diffuse radiance per object, never scene lighting; the engine relights
    the surface with its own punctual lights.
    """
    scene = bpy.context.scene
    baked = set()
    for obj in list(scene.objects):
        if obj.type != "MESH" or not obj.visible_get() or not obj.data.uv_layers:
            continue
        for slot in obj.material_slots:
            material = slot.material
            if not material or not material.use_nodes:
                continue
            tree = material.node_tree
            if not any(node.type == "SHADERTORGB" for node in tree.nodes):
                continue
            sink = next((n for n in tree.nodes if n.type == "OUTPUT_MATERIAL" and n.is_active_output), None)
            if not sink or not sink.inputs["Surface"].is_linked:
                continue
            original_name = material.name
            image = image_to_u8(bake_material_radiance(obj, slot))
            tree = slot.material.node_tree
            tree.nodes.clear()
            texture = tree.nodes.new("ShaderNodeTexImage")
            texture.image = image
            bsdf = tree.nodes.new("ShaderNodeBsdfPrincipled")
            bsdf.inputs["Metallic"].default_value = 0.0
            bsdf.inputs["Roughness"].default_value = 1.0
            sink = tree.nodes.new("ShaderNodeOutputMaterial")
            tree.links.new(texture.outputs["Color"], bsdf.inputs["Base Color"])
            tree.links.new(bsdf.outputs[0], sink.inputs["Surface"])
            baked.add(original_name)
            print(f"[BlendImport] baked toon albedo: {obj.name} / {original_name}")
    for name in known_toon:
        if name not in baked:
            print(f"[BlendImport] unbaked toon material (no bakeable UV object): {name}")
    return baked


def hide_volume_only_objects():
    """glTF has no volume shaders. Blender's exporter degrades a volume-only
    material (e.g. a PRINCIPLED_VOLUME halo around a planet backdrop) to an
    opaque unlit white surface, which renders as a giant room-filling blob.
    Objects whose every slot is volume-only are display effects - hide them
    instead of exporting the degenerate surface.
    """
    def has_surface(material):
        output = next((n for n in material.node_tree.nodes
                       if n.type == "OUTPUT_MATERIAL" and n.is_active_output), None)
        return bool(output and output.inputs["Surface"].is_linked)

    hidden = []
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH" or not obj.visible_get() or obj.hide_render:
            continue
        slots = [s.material for s in obj.material_slots]
        if not slots or not all(m and m.use_nodes for m in slots):
            continue
        if all(not has_surface(m) for m in slots):
            obj.hide_render = True
            hidden.append(obj.name)
            print(f"[BlendImport] hid volume-only object: {obj.name}")
    return hidden


def exported_images():
    """Follow visible scene materials upstream from their active outputs.

    A .blend can retain orphan images and unused materials indefinitely. Those
    datablocks (and world nodes, which we do not export) are not GLB dependencies.
    Follow nested shader groups as well; do not overlook textures inside groups.
    """
    images = set()
    visited = set()

    def visit(node):
        if node in visited:
            return
        visited.add(node)
        image = getattr(node, "image", None)
        if image is not None:
            images.add(image)
        if node.type == "GROUP" and node.node_tree:
            for output in node.node_tree.nodes:
                if output.type == "GROUP_OUTPUT" and output.is_active_output:
                    visit(output)
        for socket in node.inputs:
            for link in socket.links:
                visit(link.from_node)

    for obj in bpy.context.scene.objects:
        if not obj.visible_get() or obj.type not in {"MESH", "CURVE", "SURFACE", "FONT", "META"}:
            continue
        for slot in obj.material_slots:
            material = slot.material
            if not material or not material.use_nodes:
                continue
            for node in material.node_tree.nodes:
                if node.type == "OUTPUT_MATERIAL" and node.is_active_output:
                    visit(node)
    return images


def bake_shared_texture_transforms(output):
    """Bake shared glTF texture placement into UV0 for the current runtime.

    Only a common transform across every sampled map can use one UV stream.
    Do not silently apply albedo placement to differently mapped normal/MR maps.
    """
    import struct
    options_path = Path(bpy.data.filepath + ".import.json")
    options = json.loads(options_path.read_text(encoding="utf-8-sig")) if options_path.is_file() else {}
    flip_v = set(options.get("flipVMaterials", []))
    flip_u = set(options.get("flipUMaterials", []))
    roughness_changed = False
    data = output.read_bytes()
    length = struct.unpack_from("<I", data, 12)[0]
    document = json.loads(data[20:20 + length])
    binary = bytearray(data[28 + length:])
    transforms = {}
    for index, material in enumerate(document.get("materials", [])):
        rule = options.get("materialOverrides", {}).get(material.get("name"), {})
        if "metallic" in rule:
            material.setdefault("pbrMetallicRoughness", {})["metallicFactor"] = max(0.0, min(1.0, float(rule["metallic"])))
            roughness_changed = True
        if "roughnessScale" in rule:
            pbr = material.setdefault("pbrMetallicRoughness", {})
            pbr["roughnessFactor"] = max(0.0, min(1.0, pbr.get("roughnessFactor", 1.0) * float(rule["roughnessScale"])))
            roughness_changed = True
        pbr = material.get("pbrMetallicRoughness", {})
        infos = [pbr.get("baseColorTexture"), pbr.get("metallicRoughnessTexture"),
                 material.get("normalTexture"), material.get("occlusionTexture"),
                 material.get("emissiveTexture")]
        infos = [info for info in infos if info is not None]
        placements = []
        for info in infos:
            ext = info.get("extensions", {}).get("KHR_texture_transform", {})
            placements.append((tuple(ext.get("offset", [0, 0])), ext.get("rotation", 0),
                               tuple(ext.get("scale", [1, 1])), ext.get("texCoord", info.get("texCoord", 0))))
        if not placements or len(set(placements)) != 1 or placements[0][3] != 0:
            continue
        if placements[0] == ((0, 0), 0, (1, 1), 0) and material.get("name") not in (flip_v | flip_u):
            continue
        transforms[index] = (placements[0], infos)
    baked_materials = set()
    for mesh in document.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            material = primitive.get("material")
            if material not in transforms or "TEXCOORD_0" not in primitive.get("attributes", {}):
                continue
            accessor = document["accessors"][primitive["attributes"]["TEXCOORD_0"]]
            if accessor.get("componentType") != 5126 or accessor.get("type") != "VEC2" or "sparse" in accessor:
                raise RuntimeError("Cannot bake texture transform for non-float/sparse UV accessor")
            view = document["bufferViews"][accessor["bufferView"]]
            start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
            stride = view.get("byteStride", 8)
            (offset, angle, scale, _), _infos = transforms[material]
            cosine, sine = math.cos(angle), math.sin(angle)
            values = []
            for vertex in range(accessor["count"]):
                u, v = struct.unpack_from("<ff", binary, start + vertex * stride)
                u, v = u * scale[0], v * scale[1]
                mapped_u = offset[0] + cosine * u - sine * v
                mapped_v = offset[1] + sine * u + cosine * v
                if document["materials"][material].get("name") in flip_u:
                    mapped_u = 1.0 - mapped_u
                if document["materials"][material].get("name") in flip_v:
                    mapped_v = 1.0 - mapped_v
                values.extend((mapped_u, mapped_v))
            binary.extend(b"\0" * ((-len(binary)) % 4))
            at = len(binary)
            binary.extend(struct.pack("<" + "f" * len(values), *values))
            new_view = len(document["bufferViews"])
            document["bufferViews"].append(dict(buffer=0, byteOffset=at, byteLength=len(values)*4, target=34962))
            primitive["attributes"]["TEXCOORD_0"] = len(document["accessors"])
            document["accessors"].append(dict(bufferView=new_view, componentType=5126,
                                               count=accessor["count"], type="VEC2"))
            baked_materials.add(material)
    for material in baked_materials:
        for info in transforms[material][1]:
            extensions = info.get("extensions", {})
            extensions.pop("KHR_texture_transform", None)
            if not extensions:
                info.pop("extensions", None)
        print(f"[BlendImport] baked shared texture UV transform: {document['materials'][material]['name']}")
    if not baked_materials and not roughness_changed:
        return
    document["buffers"][0]["byteLength"] = len(binary)
    encoded = json.dumps(document, separators=(",", ":")).encode("utf-8")
    encoded += b" " * ((-len(encoded)) % 4)
    binary.extend(b"\0" * ((-len(binary)) % 4))
    output.write_bytes(struct.pack("<4sIII4s", b"glTF", 2, 28 + len(encoded) + len(binary),
                                   len(encoded), b"JSON") + encoded +
                       struct.pack("<I4s", len(binary), b"BIN\0") + binary)


def sanitize_glb_extensions(output):
    """Drop KHR material extensions the engine never reads and that crash the
    vendored assimp glTF importer (observed 0xC0000005 inside assimp on a
    Blender-exported GLB carrying specular/sheen/transmission/clearcoat/ior).
    KHR_materials_emissive_strength is kept: the material system consumes it.
    """
    import struct
    data = output.read_bytes()
    if data[16:20] != b"JSON":
        return
    length = struct.unpack("<I", data[12:16])[0]
    js = json.loads(data[20:20 + length])
    keep = "KHR_materials_emissive_strength"
    removed = set()
    for material in js.get("materials", []):
        extensions = material.get("extensions")
        if not extensions:
            continue
        for key in [k for k in extensions if k != keep]:
            removed.add(key)
            del extensions[key]
        if not extensions:
            del material["extensions"]
    if not removed:
        return
    used = [e for e in js.get("extensionsUsed", []) if e not in removed]
    js["extensionsUsed"] = used
    if "extensionsRequired" in js:
        js["extensionsRequired"] = [e for e in js["extensionsRequired"] if e not in removed]
        if not js["extensionsRequired"]:
            del js["extensionsRequired"]
    new_json = json.dumps(js, separators=(",", ":")).encode("utf-8")
    new_json += b" " * ((4 - len(new_json) % 4) % 4)
    bin_chunk = data[20 + length:]  # BIN chunk incl. its 8-byte header
    total_length = 20 + len(new_json) + len(bin_chunk)
    header = data[:4] + struct.pack("<III", struct.unpack("<I", data[4:8])[0], total_length, len(new_json))
    output.write_bytes(header + b"JSON" + new_json + bin_chunk)
    print(f"[BlendImport] stripped crash-prone KHR extensions: {sorted(removed)}")


def main():
    output = Path(sys.argv[sys.argv.index("--") + 1])
    apply_object_material_variants()
    realize_collection_instances()
    dependencies = set()
    for image in exported_images():
        if image.filepath and not image.packed_file and image.source == "FILE":
            dependencies.add(str(Path(bpy.path.abspath(image.filepath, library=image.library)).resolve()))
    for library in bpy.data.libraries:
        dependencies.add(str(Path(bpy.path.abspath(library.filepath)).resolve()))
    missing = [path for path in dependencies if not Path(path).is_file()]
    if missing:
        raise RuntimeError("Missing external resources; pack them in Blender before importing: " + ", ".join(missing))
    report = {}
    simplify_constant_shaders()
    converted, unsupported_toon = convert_toon_materials()
    report["toonTextureFallback"] = converted
    # Blender's Alpha=None means the image node outputs opaque coverage, even
    # if the packed PNG has an alpha channel. The glTF exporter can otherwise
    # reconnect that raw channel and turn opaque MMD surfaces transparent.
    # Material edits are in-memory only; never save over the authoring .blend.
    trees = [m.node_tree for m in bpy.data.materials if m.use_nodes]
    trees += list(bpy.data.node_groups)
    ignored_alpha_links = 0
    for tree in trees:
        for node in list(tree.nodes):
            if node.type != "TEX_IMAGE" or not node.image or node.image.alpha_mode != "NONE":
                continue
            alpha = node.outputs.get("Alpha")
            for link in list(alpha.links) if alpha else []:
                destination = link.to_socket
                tree.links.remove(link)
                if destination.name == "Alpha":
                    destination.default_value = 1.0
                else:
                    value = tree.nodes.new("ShaderNodeValue")
                    value.outputs[0].default_value = 1.0
                    tree.links.new(value.outputs[0], destination)
                ignored_alpha_links += 1
    print(f"[BlendImport] preserved ignored image alpha on {ignored_alpha_links} links")
    report["volumeOnlyHidden"] = len(hide_volume_only_objects())
    report["proceduralEmissionBaked"] = bake_procedural_emission(output)
    report["toonBaked"] = len(bake_toon_materials(unsupported_toon))
    # Alpha-cutout marking. Blender 4.5's glTF exporter derives alphaMode from
    # the NODE GRAPH (gather_alpha_info/detect_alpha_clip in io_scene_gltf2),
    # not from blend_method: only an Alpha socket fed through a Math ROUND (or
    # X>cutoff compare) node exports as MASK. Plain alpha-textured materials
    # (all HASHED here, e.g. the plants) export as BLEND instead, and the
    # engine renders BLEND unsorted with no depth-prepass and no shadows —
    # broken foliage (user report). Fix: mark HASHED materials whose Alpha is
    # texture-driven with a Math ROUND node (a pure export marker; MASK then
    # discards at alphaCutoff 0.5 on the engine side). Constant-alpha HASHED
    # materials keep BLEND, matching authored semi-coverage.
    hashed_to_clip = 0
    alpha_test_marked = 0
    skipped_emissive = 0

    def material_emits(mat):
        """True when the authored material glows (fake volumetrics, lamp shades,
        baked toon/emission). Such alpha-gradient materials must keep BLEND:
        MASK would binarize the soft silhouette at cutoff 0.5 (user report:
        light-shaft planes render as solid white sheets)."""
        for node in mat.node_tree.nodes:
            if node.type == "EMISSION":
                return True
            if node.type == "BSDF_PRINCIPLED":
                strength = node.inputs.get("Emission Strength")
                if strength:
                    if strength.is_linked:
                        return True
                    try:
                        if float(strength.default_value) > 0.0:
                            return True
                    except (TypeError, ValueError):
                        pass
        return False

    for mat in bpy.data.materials:
        if not (mat and mat.use_nodes) or getattr(mat, "blend_method", "") != "HASHED":
            continue
        mat.blend_method = "CLIP"
        hashed_to_clip += 1
        bsdf = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
        alpha_in = bsdf.inputs.get("Alpha") if bsdf else None
        if bsdf and alpha_in and alpha_in.is_linked:
            if material_emits(mat):
                # Glowing soft-gradient silhouette: keep BLEND. In the RT path
                # the engine renders BLEND with stochastic alpha coverage.
                skipped_emissive += 1
                continue
            src = alpha_in.links[0].from_socket
            rnd = mat.node_tree.nodes.new("ShaderNodeMath")
            rnd.operation = "ROUND"
            mat.node_tree.links.new(src, rnd.inputs[0])
            mat.node_tree.links.new(rnd.outputs[0], alpha_in)
            alpha_test_marked += 1
    report["hashedToClip"] = hashed_to_clip
    report["alphaTestMarked"] = alpha_test_marked
    report["emissiveKeptBlend"] = skipped_emissive
    print(f"[BlendImport] HASHED→CLIP {hashed_to_clip}, alpha-test marked {alpha_test_marked}, "
          f"emissive gradients kept BLEND {skipped_emissive}")
    texture_constant_coverage()
    bpy.ops.export_scene.gltf(
        filepath=str(output), export_format="GLB", use_selection=False,
        use_visible=True, use_renderable=True, use_active_scene=True,
        export_yup=True, export_apply=True, export_animations=True,
        export_cameras=False, export_lights=False,
        # The vendored assimp build corrupts control flow while importing GLB
        # morph targets (observed 0xC0000005 mid-padding on a Blender-exported
        # shape key), and the engine has no morph animation support anyway.
        export_morph=False,
    )
    bake_shared_texture_transforms(output)
    sanitize_glb_extensions(output)
    output.with_suffix(".dependencies.json").write_text(
        json.dumps(sorted(dependencies), ensure_ascii=False), encoding="utf-8")
    basis = Matrix.Rotation(-math.pi * 0.5, 4, "X")
    # Engine calibration (evidence: engine/shaders/glsl/fullscreen.frag):
    # - punctual lights attenuate by (1 - d/range)^2 (NOT 1/d^2) and the whole
    #   term is multiplied by scene exposure pc.sunDir.w = 10.
    # - directional: sunLight = (SOLAR_IRRADIANCE/π ≈ 0.6)·transmittance·I·10.
    # The previous energy/(4π) + range=1000 mapping therefore rendered rooms
    # with ~no falloff and ~10x over-brightness (user: "亮得失真").
    # Match Blender irradiance at d0 = range/2:
    #   I·(1-d0/r)²·10 = P/(4π·d0²)  →  I = P/(2.5π·r²)
    k_default_range_point = 6.0   # indoor ceiling/desk lamp throw
    k_default_range_spot = 8.0    # window/curtain throw across the room
    k_scene_exposure = 10.0
    def punctual_intensity(energy, light_range):
        return energy / (2.5 * math.pi * light_range * light_range)
    lights = []
    area_lights = 0
    for obj in bpy.context.scene.objects:
        if obj.type != "LIGHT" or obj.hide_render or not obj.visible_get():
            continue
        data = obj.data
        if data.type in {"POINT", "SPOT", "SUN"}:
            light_type = {"SUN": 0, "POINT": 1, "SPOT": 2}[data.type]
            if data.type == "SUN":
                # Solve sunLight ≈ 0.54·I·k_scene_exposure = E (W/m²).
                intensity = data.energy / (0.54 * k_scene_exposure)
            else:
                light_range = (data.cutoff_distance
                               if data.use_custom_distance and data.cutoff_distance > 0.0
                               else (k_default_range_spot if data.type == "SPOT" else k_default_range_point))
                intensity = punctual_intensity(data.energy, light_range)
        elif data.type == "AREA":
            # glTF and the engine consume punctual lights only. Approximate an
            # area light by an isotropic point light so its contribution
            # survives the round trip; sourceType marks the approximation for
            # tools that want to revisit it.
            light_type = 1
            light_range = (data.cutoff_distance
                           if data.use_custom_distance and data.cutoff_distance > 0.0
                           else k_default_range_point)
            intensity = punctual_intensity(data.energy, light_range)
            area_lights += 1
            print(f"[BlendImport] approximated AREA light as point: {obj.name}")
        else:
            print(f"[BlendImport] unsupported light shape: {obj.name} ({data.type})")
            continue
        world = basis @ obj.matrix_world
        rotation = world.to_quaternion()
        light = dict(name=obj.name, type=light_type,
                     position=list(world.translation), rotation=list(rotation), color=list(data.color),
                     intensity=intensity, sourceEnergy=data.energy,
                     range=(data.cutoff_distance if data.use_custom_distance else 1000.0) if data.type == "SUN"
                           else light_range,
                     spotAngle=math.degrees(data.spot_size) if data.type == "SPOT" else 45.0,
                     # Engine point-light cube shadows are not stable yet
                     # (user report); keep them off at the import boundary.
                     castShadow=data.use_shadow and data.type != "POINT")
        if data.type == "AREA":
            light["sourceType"] = "AREA"
        lights.append(light)
    report["areaLightsApproximated"] = area_lights
    output.with_suffix(".lights.json").write_text(json.dumps(dict(formatVersion=1, lights=lights),
                                                           ensure_ascii=False, indent=2), encoding="utf-8")
    Path(str(output) + ".import.json").write_text(json.dumps(dict(formatVersion=1, emissionUnits="radiometric",
                                                                  report=report),
                                                           ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[BlendImport] exported {len(lights)} punctual lights")
    print(f"[BlendImport] report: {json.dumps(report, ensure_ascii=False)}")


if __name__ == "__main__":
    main()
