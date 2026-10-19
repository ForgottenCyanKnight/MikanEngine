"""Run the production VOX bake and independently verify real-file face counts."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def raw_vox_counts(path):
    # These pinned samples each contain one XYZI model and no scene graph.
    # Count exposed area with 6*N - 2*adjacent_pairs, independently of the
    # renderer's direction-by-direction surface extraction and bake audit.
    data = path.read_bytes()
    require(data[:4] == b"VOX ", "bad VOX signature")
    offset, models = 8, []
    while offset + 12 <= len(data):
        name, size, children = struct.unpack_from("<4sII", data, offset)
        offset += 12
        require(offset + size + children <= len(data), "truncated VOX chunk")
        if name == b"XYZI":
            count, = struct.unpack_from("<I", data, offset)
            require(size == 4 + count * 4, "invalid XYZI size")
            cells = {tuple(data[p:p + 3]) for p in range(offset + 4, offset + size, 4)}
            require(len(cells) == count, "duplicate voxels")
            models.append(cells)
        require(name not in (b"nTRN", b"nSHP", b"nGRP"), "fixture unexpectedly contains a scene graph")
        offset += size  # descend through MAIN children
    require(len(models) == 1, "fixture must contain exactly one model")
    cells = models[0]
    pairs = sum((x + dx, y + dy, z + dz) in cells
                for x, y, z in cells for dx, dy, dz in ((1, 0, 0), (0, 1, 0), (0, 0, 1)))
    return len(cells), 6 * len(cells) - 2 * pairs


def main():
    executable, fixture_dir, output_path = sys.argv[1:]
    root, output = Path(fixture_dir), Path(output_path)
    manifest = json.loads((root / "sources.json").read_text())
    paths = []
    for item in manifest["files"]:
        path = root / item["name"]
        data = path.read_bytes()
        require(len(data) == item["bytes"], "fixture size changed")
        require(hashlib.sha256(data).hexdigest() == item["sha256"], "fixture checksum changed")
        paths.append(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    process = subprocess.run([executable, *map(str, paths), "--output", str(output)],
                             capture_output=True, text=True)
    if process.returncode:
        print(process.stdout)
        print(process.stderr, file=sys.stderr)
        raise RuntimeError("production VOX analysis failed")
    reports = json.loads(output.read_text())["files"]
    require(len(reports) == len(paths), "report omitted a model")
    for item, path, report in zip(manifest["files"], paths, reports):
        voxel_count, faces = raw_vox_counts(path)
        stats = report["stats"]
        require(report["voxel_count"] == voxel_count, "loader voxel count differs from raw file")
        require(stats["exposed_unit_faces"] == faces, "surface area differs from independent adjacency count")
        for key, expected in item["expected"].items():
            actual = report["voxel_count"] if key == "voxel_count" else stats[key]
            require(actual == expected, f"{path.name}: {key} changed from {expected} to {actual}")
        require(stats["triangles"] == 2 * stats["final_quads"], "triangle count mismatch")
        reduction = 1 - stats["final_quads"] / faces
        require(abs(stats["final_reduction_percent"] - reduction * 100) < 1e-10, "reduction ratio mismatch")
        require(report["validation"] == "pass", "surface audit failed")
        print(f"PASS {path.name}: voxels={voxel_count} faces={faces} "
              f"greedy={stats['greedy_quads']} final={stats['final_quads']} reduction={reduction:.2%}")
    print(f"Report: {output}")


if __name__ == "__main__":
    main()
