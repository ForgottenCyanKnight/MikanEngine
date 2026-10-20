#!/usr/bin/env python3
"""Measure exact repeated-subtree sharing in MagicaVoxel models.

This compares an ordinary pruned sparse voxel octree with a bottom-up DAG that
interns identical, position-independent subtrees. It measures storage structure,
not ray-tracing time or triangle reduction.
"""

import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import sys


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load_vox_models(path):
    data = Path(path).read_bytes()
    require(len(data) >= 20 and data[:4] == b"VOX ", f"{path}: bad VOX header")
    require(struct.unpack_from("<I", data, 4)[0] >= 150, f"{path}: unsupported VOX version")

    chunk_id, content_size, children_size = struct.unpack_from("<4sII", data, 8)
    require(chunk_id == b"MAIN", f"{path}: missing MAIN chunk")
    content_begin = 20
    content_end = content_begin + content_size
    main_end = content_end + children_size
    require(content_size == 0 and main_end <= len(data), f"{path}: invalid MAIN bounds")

    models = []
    pending_size = None
    offset = content_end
    while offset < main_end:
        require(offset + 12 <= main_end, f"{path}: truncated chunk header")
        name, size, child_size = struct.unpack_from("<4sII", data, offset)
        begin = offset + 12
        end = begin + size
        chunk_end = end + child_size
        require(chunk_end <= main_end, f"{path}: chunk exceeds MAIN bounds")

        if name == b"SIZE":
            require(size == 12, f"{path}: invalid SIZE chunk")
            pending_size = struct.unpack_from("<III", data, begin)
            require(all(dim > 0 for dim in pending_size), f"{path}: empty SIZE chunk")
        elif name == b"XYZI":
            require(pending_size is not None and size >= 4, f"{path}: XYZI without SIZE")
            count, = struct.unpack_from("<I", data, begin)
            require(size == 4 + count * 4, f"{path}: invalid XYZI size")
            voxels = [tuple(data[p:p + 4]) for p in range(begin + 4, end, 4)]
            require(len(voxels) == count, f"{path}: invalid voxel count")
            occupied = set()
            for x, y, z, color in voxels:
                require(x < pending_size[0] and y < pending_size[1] and z < pending_size[2],
                        f"{path}: voxel outside SIZE bounds")
                key = (x, y, z)
                require(key not in occupied, f"{path}: duplicate voxel coordinate {key}")
                occupied.add(key)
            models.append((pending_size, voxels))
            pending_size = None
        offset = chunk_end

    require(offset == main_end and models, f"{path}: no complete SIZE/XYZI models")
    return data, models


def next_power_of_two(value):
    return 1 << (value - 1).bit_length()


def build_stats(dims, voxels, colored):
    """Build expanded and interned octrees; child refs 1..256 encode leaf color."""
    side = next_power_of_two(max(dims))
    interned = {}
    next_node_id = 257  # 0 is empty; 1..256 are material-aware voxel leaves.
    expanded_by_level = Counter()
    unique_by_level = Counter()
    expanded_edges = 0
    unique_edges = 0

    def build(points, ox, oy, oz, size):
        nonlocal next_node_id, expanded_edges, unique_edges
        if not points:
            return 0
        if size == 1:
            require(len(points) == 1, "duplicate voxel reached an octree leaf")
            return points[0][3] + 1 if colored else 1

        half = size // 2
        groups = [[] for _ in range(8)]
        for point in points:
            x, y, z, _ = point
            child = ((x >= ox + half) << 2) | ((y >= oy + half) << 1) | (z >= oz + half)
            groups[child].append(point)

        children = []
        for child, group in enumerate(groups):
            cx = ox + (half if child & 4 else 0)
            cy = oy + (half if child & 2 else 0)
            cz = oz + (half if child & 1 else 0)
            children.append(build(group, cx, cy, cz, half))

        level = size.bit_length() - 1
        children = tuple(children)
        expanded_by_level[level] += 1
        expanded_edges += sum(child != 0 for child in children)
        key = (level, children)
        node_id = interned.get(key)
        if node_id is None:
            node_id = next_node_id
            next_node_id += 1
            interned[key] = node_id
            unique_by_level[level] += 1
            unique_edges += sum(child != 0 for child in children)
        return node_id

    root = build(voxels, 0, 0, 0, side)
    expanded_nodes = sum(expanded_by_level.values())
    unique_nodes = len(interned)

    # Packed reference layout estimate: 1-byte child mask + 4-byte ref-array
    # offset per internal node, then one 32-bit ref for each nonempty child.
    # This is the same layout for the tree and DAG; allocator/alignment overhead,
    # palette/material tables, and the root ref are excluded from the ratio.
    expanded_bytes = expanded_nodes * 5 + expanded_edges * 4
    dag_bytes = unique_nodes * 5 + unique_edges * 4

    return {
        "mode": "color_index" if colored else "occupancy_only",
        "root_ref": root,
        "expanded_internal_nodes": expanded_nodes,
        "unique_internal_nodes": unique_nodes,
        "internal_node_reduction_percent": percent_reduction(expanded_nodes, unique_nodes),
        "expanded_child_refs": expanded_edges,
        "unique_child_refs": unique_edges,
        "child_ref_reduction_percent": percent_reduction(expanded_edges, unique_edges),
        "expanded_estimated_bytes": expanded_bytes,
        "dag_estimated_bytes": dag_bytes,
        "estimated_byte_reduction_percent": percent_reduction(expanded_bytes, dag_bytes),
        "expanded_nodes_by_level": dict(sorted(expanded_by_level.items())),
        "unique_nodes_by_level": dict(sorted(unique_by_level.items())),
    }


def percent_reduction(before, after):
    return (100.0 * (before - after) / before) if before else 0.0


def analyze(path):
    data, models = load_vox_models(path)
    reports = []
    for index, (dims, voxels) in enumerate(models):
        side = next_power_of_two(max(dims))
        reports.append({
            "model_index": index,
            "dimensions": list(dims),
            "octree_side": side,
            "voxel_count": len(voxels),
            "input_vox_file_bytes": len(data),
            "raw_voxel_records_bytes": len(voxels) * 4,
            "dense_occupancy_bitset_bytes": (dims[0] * dims[1] * dims[2] + 7) // 8,
            "layout": "packed 5-byte internal node + 4 bytes per nonempty child ref; excludes palette/material tables and alignment",
            "results": [build_stats(dims, voxels, colored=False), build_stats(dims, voxels, colored=True)],
        })
    return reports


def self_test():
    # Two identical 8^3 motifs translated into opposite root octants.
    motif = [(1, 1, 1, 7), (1, 1, 2, 7), (1, 2, 1, 7), (2, 1, 1, 7)]
    same_color = motif + [(x + 8, y, z, color) for x, y, z, color in motif]
    occupancy = build_stats((16, 8, 8), same_color, colored=False)
    colored = build_stats((16, 8, 8), same_color, colored=True)
    assert occupancy["unique_internal_nodes"] < occupancy["expanded_internal_nodes"]
    assert occupancy["unique_child_refs"] < occupancy["expanded_child_refs"]
    assert colored["unique_internal_nodes"] == occupancy["unique_internal_nodes"]

    different_color = motif + [(x + 8, y, z, 9) for x, y, z, _ in motif]
    occupancy_mixed = build_stats((16, 8, 8), different_color, colored=False)
    colored_mixed = build_stats((16, 8, 8), different_color, colored=True)
    assert occupancy_mixed["unique_internal_nodes"] < colored_mixed["unique_internal_nodes"]
    assert build_stats((8, 8, 8), [], colored=False)["unique_internal_nodes"] == 0
    print("VOX SVDAG subtree-dedup self-test: PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("models", nargs="*", help="MagicaVoxel .vox files")
    parser.add_argument("--self-test", action="store_true", help="run synthetic repeated-subtree checks")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    args = parser.parse_args()

    if args.self_test:
        self_test()
    if not args.models:
        require(args.self_test, "provide one or more .vox files, or use --self-test")
        return

    for path in args.models:
        for model in analyze(path):
            if args.json:
                print(json.dumps({"file": str(path), **model}, sort_keys=True))
                continue
            dims = "x".join(map(str, model["dimensions"]))
            print(f"{path} model={model['model_index']} dims={dims} octree={model['octree_side']}^3 "
                  f"voxels={model['voxel_count']} raw_voxel_records={model['raw_voxel_records_bytes']}B "
                  f"file={model['input_vox_file_bytes']}B")
            for result in model["results"]:
                print(f"  {result['mode']}: internal nodes {result['expanded_internal_nodes']:,} -> "
                      f"{result['unique_internal_nodes']:,} "
                      f"(-{result['internal_node_reduction_percent']:.2f}%), refs "
                      f"{result['expanded_child_refs']:,} -> {result['unique_child_refs']:,} "
                      f"(-{result['child_ref_reduction_percent']:.2f}%), estimated bytes "
                      f"{result['expanded_estimated_bytes']:,} -> {result['dag_estimated_bytes']:,} "
                      f"(-{result['estimated_byte_reduction_percent']:.2f}%)")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
