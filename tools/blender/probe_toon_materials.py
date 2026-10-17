"""Probe: dump node graph summary for Shader-to-RGB materials in a .blend."""
import sys
import bpy

results = []
for material in bpy.data.materials:
    if not material.use_nodes or not material.node_tree:
        continue
    tree = material.node_tree
    stg = [n for n in tree.nodes if n.type == "SHADERTORGB"]
    if not stg:
        continue
    info = {"name": material.name, "nodes": sorted(n.type for n in tree.nodes),
            "tex_images": [], "shader_to_rgb_inputs": []}
    for node in tree.nodes:
        if node.type == "TEX_IMAGE" and node.image:
            info["tex_images"].append(node.image.name)
    for node in stg:
        for inp in node.inputs:
            for link in inp.links:
                info["shader_to_rgb_inputs"].append(
                    f"{inp.name} <- {link.from_node.type}({link.from_node.name})")
    results.append(info)

out = sys.argv[sys.argv.index("--") + 1]
with open(out, "w", encoding="utf-8") as f:
    for info in results:
        f.write(f"=== {info['name']}\n")
        f.write(f"  nodes: {', '.join(info['nodes'])}\n")
        f.write(f"  tex_images: {info['tex_images'] or '-'}\n")
        f.write(f"  shader-to-rgb inputs: {info['shader_to_rgb_inputs'] or '-'}\n")
print(f"[Probe] dumped {len(results)} toon materials")
