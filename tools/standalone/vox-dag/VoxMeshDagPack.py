#!/usr/bin/env python3
"""Pack compiled VOX surfaces as a position-independent sparse-octree DAG.

The DAG stores quad records at their 3D anchor cells and interns equal spatial
subtrees. Unpacking expands those references back into an ordinary v5 .voxmesh
surface asset; no generic byte-stream compressor is used.
"""

import argparse
from collections import Counter
from dataclasses import dataclass
import json
import math
from pathlib import Path
import struct
import sys
import zlib


PACKAGE_MAGIC = b"VMDA"
PACKAGE_HEADER = struct.Struct("<4s16I")
VOXMESH_MAGIC = 0x534D5856
MAX_FILE_BYTES = 512 * 1024 * 1024
OUTER_VERSION = 4
LEAF_VERSION = 5
STATIC_CONTACT_ALGORITHM = 0x42464C02


def require(condition, message):
    if not condition:
        raise ValueError(message)


def hash_words(words):
    value = 2166136261
    for word in words:
        for shift in (0, 8, 16, 24):
            value = ((value ^ ((word >> shift) & 255)) * 16777619) & 0xFFFFFFFF
    return value


def verify_words(words, label):
    require(len(words) >= 6 and words[0] == VOXMESH_MAGIC, f"{label}: invalid VOX mesh header")
    require(hash_words(words[:-1]) == words[-1], f"{label}: checksum mismatch")
    return words[:-1]


def float_from_word(word):
    return struct.unpack("<f", struct.pack("<I", word))[0]


@dataclass(frozen=True)
class Face:
    x: int
    y: int
    z: int
    direction: int
    width: int
    height: int
    attr_kind: int  # 0 = palette descriptor; 1 = inline R8 texels
    attr_value: object


@dataclass(frozen=True)
class DagNode:
    level: int
    children: tuple = ()
    faces: tuple = ()


def read_voxmesh(path_or_bytes):
    data = path_or_bytes if isinstance(path_or_bytes, bytes) else Path(path_or_bytes).read_bytes()
    require(24 <= len(data) <= MAX_FILE_BYTES and len(data) % 4 == 0, "invalid .voxmesh size")
    all_words = list(struct.unpack(f"<{len(data) // 4}I", data))
    outer = (0, 0)
    words = verify_words(all_words, ".voxmesh")
    if words[1] == OUTER_VERSION:
        require(len(words) >= 11 and words[2] == 2 and words[3] == STATIC_CONTACT_ALGORITHM,
                "unsupported v4 contact envelope")
        count = words[4]
        require(count == len(words) - 5, "invalid v4 payload length")
        outer = (words[2], words[3])
        words = verify_words(words[5:], "v4 payload")
    require(len(words) >= 19 and words[0] == VOXMESH_MAGIC and words[1] == LEAF_VERSION,
            "DAG codec currently requires a v5 compiled-surface leaf")

    flags, voxel_count, quad_count, attr_count = words[2:6]
    require(quad_count > 0 and attr_count >= 4, "empty or invalid compiled surface")
    bounds_words = tuple(words[6:12])
    direction_counts = tuple(words[12:18])
    plane_count = words[18]
    base = 19 + plane_count * 2
    require(0 < plane_count <= quad_count and base + quad_count + attr_count == len(words),
            "invalid plane or attribute table length")

    attrs = words[base + quad_count:]
    palette_end = attrs[1]
    descriptor_start = attrs[2]
    require(attrs[0] == 4 and 4 <= palette_end <= 516 and (palette_end - 4) % 2 == 0,
            "unsupported palette table")
    require(descriptor_start == palette_end + quad_count and descriptor_start <= attr_count,
            "invalid per-quad descriptor table")
    require(attrs[3] == 0, "unsupported VOX mesh attribute flags")
    palette_words = tuple(attrs[4:palette_end])
    texture_pool = b"".join(struct.pack("<I", word) for word in attrs[descriptor_start:])

    planes = [(words[19 + i * 2], words[20 + i * 2]) for i in range(plane_count)]
    faces = []
    next_first = 0
    observed_direction_counts = [0] * 6
    previous_direction = -1
    for plane_index, (first, key) in enumerate(planes):
        end = planes[plane_index + 1][0] if plane_index + 1 < plane_count else quad_count
        direction, plane = key >> 16, key & 0xFFFF
        require(first == next_first and first < end <= quad_count, "invalid plane range")
        require(direction < 6 and plane <= 255 and direction >= previous_direction,
                "invalid plane direction order")
        previous_direction = direction
        observed_direction_counts[direction] += end - first
        for index in range(first, end):
            packed = words[base + index]
            u, v = packed & 255, (packed >> 8) & 255
            width, height = ((packed >> 16) & 255) + 1, (packed >> 24) + 1
            if direction < 2:
                x, y, z = u, v, plane
            elif direction < 4:
                x, y, z = plane, v, u
            else:
                x, y, z = u, plane, v
            descriptor = attrs[palette_end + index]
            if descriptor & 0x80000000:
                offset = descriptor & 0x7FFFFFFF
                size = width * height
                require(offset + size <= len(texture_pool), "R8 descriptor outside attribute pool")
                attr_kind, attr_value = 1, texture_pool[offset:offset + size]
            else:
                require(descriptor < len(palette_words) // 2, "palette descriptor outside palette")
                attr_kind, attr_value = 0, descriptor
            faces.append(Face(x, y, z, direction, width, height, attr_kind, attr_value))
        next_first = end
    require(next_first == quad_count and tuple(observed_direction_counts) == direction_counts,
            "direction counts do not match plane table")
    require(len(faces) == quad_count, "quad count mismatch")

    extents = [float_from_word(bounds_words[i + 3]) - float_from_word(bounds_words[i]) for i in range(3)]
    max_cell = max(max(face.x, face.y, face.z) + 1 for face in faces)
    needed_side = max(max_cell, math.ceil(max(extents)))
    require(0 < needed_side <= 256, "surface exceeds U8 voxel coordinates")
    side = 1 << (needed_side - 1).bit_length()
    return {
        "data": data,
        "outer": outer,
        "flags": flags,
        "voxel_count": voxel_count,
        "bounds_words": bounds_words,
        "palette_words": palette_words,
        "faces": faces,
        "side": side,
    }


def face_payload(face):
    return (face.direction, face.width, face.height, face.attr_kind, face.attr_value)


def build_dag(mesh):
    points = {}
    for face in mesh["faces"]:
        points.setdefault((face.x, face.y, face.z), []).append(face_payload(face))
    entries = [(position, tuple(sorted(payload))) for position, payload in points.items()]
    nodes = []
    interned = {}
    expanded_nodes = 0
    expanded_edges = 0

    def intern(node, key):
        existing = interned.get(key)
        if existing is not None:
            return existing
        node_id = len(nodes) + 1
        nodes.append(node)
        interned[key] = node_id
        return node_id

    def build(subset, origin, size):
        nonlocal expanded_nodes, expanded_edges
        if not subset:
            return 0
        expanded_nodes += 1
        level = size.bit_length() - 1
        if size == 1:
            payload = subset[0][1]
            return intern(DagNode(0, faces=payload), (0, payload))

        half = size // 2
        groups = [[] for _ in range(8)]
        for position, payload in subset:
            x, y, z = position
            child = ((x >= origin[0] + half) << 2) | ((y >= origin[1] + half) << 1) | (z >= origin[2] + half)
            groups[child].append((position, payload))
        children = []
        for child, group in enumerate(groups):
            child_origin = (origin[0] + (half if child & 4 else 0),
                           origin[1] + (half if child & 2 else 0),
                           origin[2] + (half if child & 1 else 0))
            children.append(build(group, child_origin, half) if group else 0)
        expanded_edges += sum(reference != 0 for reference in children)
        child_tuple = tuple(children)
        return intern(DagNode(level, children=child_tuple), (level, child_tuple))

    root = build(entries, (0, 0, 0), mesh["side"])
    return nodes, root, expanded_nodes, expanded_edges


def put_varint(output, value):
    require(value >= 0, "negative varint")
    while value >= 0x80:
        output.append((value & 0x7F) | 0x80)
        value >>= 7
    output.append(value)


def get_varint(data, offset):
    value, shift = 0, 0
    while True:
        require(offset < len(data) and shift <= 35, "truncated or oversized varint")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7


def encode_package(mesh):
    nodes, root, expanded_nodes, expanded_edges = build_dag(mesh)
    output = bytearray(PACKAGE_HEADER.pack(
        PACKAGE_MAGIC, 1, mesh["flags"], mesh["voxel_count"], len(mesh["faces"]),
        *mesh["bounds_words"], mesh["side"], root, len(nodes), len(mesh["palette_words"]),
        mesh["outer"][0], mesh["outer"][1]))
    for word in mesh["palette_words"]:
        output.extend(struct.pack("<I", word))

    for node in nodes:
        output.append(node.level)
        if node.level == 0:
            put_varint(output, len(node.faces))
            for direction, width, height, kind, value in node.faces:
                output.extend((direction, width - 1, height - 1, kind))
                if kind == 0:
                    put_varint(output, value)
                else:
                    require(len(value) == width * height, "bad R8 quad attribute size")
                    output.extend(value)
        else:
            mask = sum((1 << index) for index, child in enumerate(node.children) if child)
            output.append(mask)
            for child in node.children:
                if child:
                    put_varint(output, child)
    output.extend(struct.pack("<I", zlib.crc32(output)))
    stats = {
        "expanded_octree_nodes": expanded_nodes,
        "unique_dag_nodes": len(nodes),
        "octree_node_reduction_percent": 100.0 * (expanded_nodes - len(nodes)) / expanded_nodes,
        "expanded_child_refs": expanded_edges,
        "package_bytes": len(output),
    }
    return bytes(output), stats


def decode_package(data):
    require(len(data) >= PACKAGE_HEADER.size + 4, "truncated VMDA package")
    require(zlib.crc32(data[:-4]) == struct.unpack_from("<I", data, len(data) - 4)[0],
            "VMDA package checksum mismatch")
    header = PACKAGE_HEADER.unpack_from(data)
    require(header[0] == PACKAGE_MAGIC and header[1] == 1, "unsupported VMDA package")
    (_, version, flags, voxel_count, quad_count, *fields) = header
    bounds_words = tuple(fields[:6])
    side, root, node_count, palette_count, outer_flags, outer_algo = fields[6:]
    require(side > 0 and side <= 256 and side & (side - 1) == 0, "invalid DAG root size")
    require(0 < root <= node_count and node_count < quad_count * 16, "invalid DAG node counts")
    require(outer_flags in (0, 2) and ((outer_flags == 0 and outer_algo == 0) or
            (outer_flags == 2 and outer_algo == STATIC_CONTACT_ALGORITHM)), "invalid envelope metadata")

    offset = PACKAGE_HEADER.size
    palette_end = offset + palette_count * 4
    require(palette_end <= len(data) - 4, "truncated DAG palette")
    palette_words = struct.unpack_from(f"<{palette_count}I", data, offset) if palette_count else ()
    offset = palette_end
    nodes = []
    for node_id in range(1, node_count + 1):
        require(offset < len(data) - 4, "truncated DAG node table")
        level = data[offset]
        offset += 1
        if level == 0:
            face_count, offset = get_varint(data, offset)
            require(0 < face_count <= 6, "invalid quad count in DAG leaf")
            faces = []
            for _ in range(face_count):
                require(offset + 4 <= len(data) - 4, "truncated DAG face")
                direction, width_minus_one, height_minus_one, kind = data[offset:offset + 4]
                offset += 4
                width, height = width_minus_one + 1, height_minus_one + 1
                require(direction < 6 and kind in (0, 1), "invalid DAG face descriptor")
                if kind == 0:
                    value, offset = get_varint(data, offset)
                    require(value < palette_count // 2, "DAG palette reference out of range")
                else:
                    size = width * height
                    require(offset + size <= len(data) - 4, "truncated DAG R8 face data")
                    value = data[offset:offset + size]
                    offset += size
                faces.append((direction, width, height, kind, value))
            nodes.append(DagNode(0, faces=tuple(faces)))
        else:
            require(1 <= level <= 8 and offset < len(data) - 4, "invalid DAG internal level")
            mask = data[offset]
            offset += 1
            require(mask != 0, "empty internal DAG node")
            children = [0] * 8
            for child_index in range(8):
                if mask & (1 << child_index):
                    child, offset = get_varint(data, offset)
                    require(0 < child < node_id, "DAG reference is not bottom-up")
                    require(nodes[child - 1].level == level - 1, "DAG child level mismatch")
                    children[child_index] = child
            nodes.append(DagNode(level, children=tuple(children)))
    require(offset == len(data) - 4, "trailing bytes in VMDA package")
    require(nodes[root - 1].level == side.bit_length() - 1, "DAG root level mismatch")

    faces = []

    def visit(node_id, origin, size):
        node = nodes[node_id - 1]
        if size == 1:
            require(node.level == 0, "unexpected internal node at voxel leaf")
            for direction, width, height, kind, value in node.faces:
                faces.append(Face(*origin, direction, width, height, kind, value))
            return
        require(node.level == size.bit_length() - 1, "DAG spatial level mismatch")
        half = size // 2
        for child_index, child_id in enumerate(node.children):
            if not child_id:
                continue
            child_origin = (origin[0] + (half if child_index & 4 else 0),
                            origin[1] + (half if child_index & 2 else 0),
                            origin[2] + (half if child_index & 1 else 0))
            visit(child_id, child_origin, half)

    visit(root, (0, 0, 0), side)
    require(len(faces) == quad_count, "expanded DAG quad count mismatch")
    return {
        "flags": flags,
        "voxel_count": voxel_count,
        "quad_count": quad_count,
        "bounds_words": bounds_words,
        "side": side,
        "palette_words": tuple(palette_words),
        "outer": (outer_flags, outer_algo),
        "faces": faces,
        "node_count": node_count,
    }


def face_sort_key(face):
    if face.direction < 2:
        plane, u, v = face.z, face.x, face.y
    elif face.direction < 4:
        plane, u, v = face.x, face.z, face.y
    else:
        plane, u, v = face.y, face.x, face.z
    return (face.direction, plane, u, v, face.width, face.height,
            face.attr_kind, face.attr_value)


def face_identity(face):
    return (face.x, face.y, face.z, face.direction, face.width, face.height,
            face.attr_kind, face.attr_value)


def words_to_bytes(words):
    return struct.pack(f"<{len(words)}I", *words)


def serialize_mesh(mesh):
    faces = sorted(mesh["faces"], key=face_sort_key)
    quad_count = len(faces)
    palette_words = mesh["palette_words"]
    palette_end = 4 + len(palette_words)
    descriptor_start = palette_end + quad_count
    descriptors = []
    texture_pool = bytearray()
    packed_quads = []
    direction_counts = [0] * 6
    planes = []
    previous_key = None

    for index, face in enumerate(faces):
        direction_counts[face.direction] += 1
        if face.direction < 2:
            plane, u, v = face.z, face.x, face.y
        elif face.direction < 4:
            plane, u, v = face.x, face.z, face.y
        else:
            plane, u, v = face.y, face.x, face.z
        key = plane | (face.direction << 16)
        if key != previous_key:
            planes.append((index, key))
            previous_key = key
        packed_quads.append(u | (v << 8) | ((face.width - 1) << 16) | ((face.height - 1) << 24))
        if face.attr_kind == 0:
            require(face.attr_value < len(palette_words) // 2, "invalid palette index")
            descriptors.append(face.attr_value)
        else:
            require(len(face.attr_value) == face.width * face.height, "invalid R8 texel payload")
            offset = len(texture_pool)
            require(offset < 0x80000000, "R8 texture payload exceeds descriptor range")
            descriptors.append(0x80000000 | offset)
            texture_pool.extend(face.attr_value)

    attributes = [4, palette_end, descriptor_start, 0]
    attributes.extend(palette_words)
    attributes.extend(descriptors)
    texture_pool.extend(b"\0" * ((-len(texture_pool)) & 3))
    for offset in range(0, len(texture_pool), 4):
        attributes.append(struct.unpack_from("<I", texture_pool, offset)[0])

    words = [VOXMESH_MAGIC, LEAF_VERSION, mesh["flags"], mesh["voxel_count"], quad_count,
             len(attributes), *mesh["bounds_words"], *direction_counts, len(planes)]
    for first, key in planes:
        words.extend((first, key))
    words.extend(packed_quads)
    words.extend(attributes)
    words.append(hash_words(words))

    outer_flags, outer_algo = mesh["outer"]
    if outer_flags:
        envelope = [VOXMESH_MAGIC, OUTER_VERSION, outer_flags, outer_algo, len(words), *words]
        envelope.append(hash_words(envelope))
        words = envelope
    result = words_to_bytes(words)
    require(len(result) <= MAX_FILE_BYTES, "expanded .voxmesh exceeds size limit")
    return result


def verify_same_geometry(original, restored):
    require(original["flags"] == restored["flags"] and
            original["voxel_count"] == restored["voxel_count"] and
            original["bounds_words"] == restored["bounds_words"] and
            original["palette_words"] == restored["palette_words"] and
            original["outer"] == restored["outer"], "DAG changed mesh metadata")
    original_faces = Counter(face_identity(face) for face in original["faces"])
    restored_faces = Counter(face_identity(face) for face in restored["faces"])
    require(original_faces == restored_faces, "DAG expansion changed quad geometry or attributes")


def pack(source, package_path=None, roundtrip_path=None):
    original = read_voxmesh(source)
    package, stats = encode_package(original)
    expanded = decode_package(package)
    restored_bytes = serialize_mesh(expanded)
    restored = read_voxmesh(restored_bytes)
    verify_same_geometry(original, restored)
    if package_path:
        Path(package_path).write_bytes(package)
    if roundtrip_path:
        Path(roundtrip_path).write_bytes(restored_bytes)
    stats.update({
        "source": str(source),
        "voxmesh_bytes": len(original["data"]),
        "dag_package_bytes": len(package),
        "bytes_saved": len(original["data"]) - len(package),
        "reduction_percent": 100.0 * (len(original["data"]) - len(package)) / len(original["data"]),
        "size_ratio": len(original["data"]) / len(package),
        "quad_count": len(original["faces"]),
        "triangle_count": len(original["faces"]) * 2,
        "primitive_attributes_identical": True,
        "byte_exact_rebuild": restored_bytes == original["data"],
        "package": str(package_path) if package_path else None,
        "roundtrip": str(roundtrip_path) if roundtrip_path else None,
    })
    return package, restored_bytes, stats


def unpack(package_path, output_path):
    package = Path(package_path).read_bytes()
    mesh = decode_package(package)
    data = serialize_mesh(mesh)
    read_voxmesh(data)
    Path(output_path).write_bytes(data)
    return {"output": str(output_path), "voxmesh_bytes": len(data),
            "quad_count": mesh["quad_count"], "triangle_count": mesh["quad_count"] * 2,
            "dag_nodes": mesh["node_count"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    stats_parser = subparsers.add_parser("stats", help="measure DAG size and verify expansion")
    stats_parser.add_argument("source", type=Path)
    stats_parser.add_argument("--package", type=Path)
    stats_parser.add_argument("--roundtrip", type=Path)
    pack_parser = subparsers.add_parser("pack", help="write a DAG-specific package")
    pack_parser.add_argument("source", type=Path)
    pack_parser.add_argument("package", type=Path)
    pack_parser.add_argument("--roundtrip", type=Path)
    unpack_parser = subparsers.add_parser("unpack", help="expand a DAG package to .voxmesh")
    unpack_parser.add_argument("package", type=Path)
    unpack_parser.add_argument("output", type=Path)
    args = parser.parse_args()

    if args.command == "unpack":
        print(json.dumps(unpack(args.package, args.output), sort_keys=True))
        return
    if args.command == "pack":
        _, _, result = pack(args.source, args.package, args.roundtrip)
        print(json.dumps(result, sort_keys=True))
        return
    _, _, result = pack(args.source, args.package, args.roundtrip)
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
