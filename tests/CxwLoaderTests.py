"""CPU parser/mesher tests against the production C++ reader. No GPU needed.
Usage: python tests/CxwLoaderTests.py path/to/MikanCxwInspect.exe
"""
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

RED = 0x8000 | (31 << 10)
GREEN = 0x8000 | (31 << 5)


def region(cells, *, free=(), capacity=None, duplicate_id=False, physics=False):
    by_brick = {}
    for (x, y, z), code in cells.items():
        bi = (x >> 3) + 16 * (y >> 3) + 256 * (z >> 3)
        by_brick.setdefault(bi, [0] * 512)[(x & 7) + 8 * (y & 7) + 64 * (z & 7)] = code
    count = len(by_brick)
    capacity = count + len(free) if capacity is None else capacity
    indices = [-1] * 4096
    blocks = [[0x2000] * 512 for _ in range(capacity)]  # Undefined free data must be ignored.
    for brick_id, (bi, values) in enumerate(by_brick.items()):
        indices[bi] = brick_id
        blocks[brick_id] = values
    if duplicate_id:
        indices[4095] = 0
    raw = struct.pack("<4I4096h", 0x32535856, capacity, count, len(free), *indices)
    raw += struct.pack("<" + "h" * len(free), *free)
    raw += bytes(2 + 8192) + struct.pack("<I", 2 if physics else 1)
    raw += struct.pack("<BH", 0, 2)
    raw += struct.pack("<" + "H" * (capacity * 512), *(v for brick in blocks for v in brick))
    if physics:
        raw += struct.pack("<BH", 1, 1) + bytes([0xFA]) * (capacity * 512)
    compressor = zlib.compressobj(wbits=-15)
    packed = compressor.compress(raw) + compressor.flush()
    return bytes(16384) + struct.pack("<I", len(packed)) + packed


def world(regions, *, version=6, flags=1, rotation=(0, 0, 0, 1), pose=False):
    result = bytearray(64)
    entries = []
    for coord, data in regions:
        entries.append((*coord, len(result), len(data)))
        result += data
    index = len(result)
    for entry in entries:
        result += struct.pack("<iiiQI", *entry)
    table = len(result)
    if version >= 7:
        result += struct.pack("<B7f", int(pose), 1, 2, 3, 0, 0, 0, 1)
    result += struct.pack("<I4I3f4fH", 1, 1, 2, 3, 4, 0, 0, 0, *rotation, 0)
    if version >= 3:
        result += struct.pack("<B", 2)
    if version >= 4:
        result += bytes(24)
    if version >= 5:
        result += bytes(1)
    result += struct.pack("<QI", index, len(entries))
    struct.pack_into("<IHHQ", result, 0, 0x534C5843, version, flags, table)
    return result


def main():
    exe = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="cxw-tests-") as temp:
        count = 0

        def run(name, data, expected=None, error=None, uniform=False, clear_glass=False):
            nonlocal count
            path = pathlib.Path(temp) / (name + ".cxw")
            path.write_bytes(data)
            environment=dict(os.environ, MIKAN_CXW_CLEAR_GLASS="1" if clear_glass else "0")
            p = subprocess.run([str(exe), str(path)] + (["--uniform"] if uniform else []), capture_output=True, text=True, errors="replace", env=environment)
            if error:
                assert p.returncode != 0 and error in p.stderr, (name, p.returncode, p.stdout, p.stderr)
            else:
                assert p.returncode == 0, (name, p.stdout, p.stderr)
                result = next(json.loads(line) for line in p.stdout.splitlines() if line.startswith("{"))
                for key, value in expected.items():
                    assert (value(result[key]) if callable(value) else result[key] == value), (name, key, result[key], value)
            count += 1

        one = region({(0, 0, 0): RED})
        for version in (1, 2, 3, 4, 5, 7):
            run(f"v{version}-legacy-layout", world([((0, 0, 0), one)], version=version), {"voxels": 1, "version": version})
        run("v6-one", world([((0, 0, 0), one)]), {"voxels": 1, "quads": 6, "exposed_faces": 6, "first_color": 0xFF00})
        run("v8-pose-rotation", world([((0, 0, 0), one)], version=8, pose=True, rotation=(0, .382683432, 0, .923879533)), {"voxels": 1, "has_player_pose": True})
        run("solid-cube", world([((0, 0, 0), region({(x, y, z): RED for x in range(2) for y in range(2) for z in range(2)}))]), {"voxels": 8, "quads": 6, "exposed_faces": 24})
        run("negative-cross-sector-seam", world([((-1, 0, 0), region({(127, 0, 0): RED})), ((0, 0, 0), region({(0, 0, 0): GREEN}))]), {"voxels": 2, "quads": 10, "exposed_faces": 10})
        run("merge-across-sector", world([((0, 0, 0), region({(127, 0, 0): RED})), ((1, 0, 0), region({(0, 0, 0): GREEN}))]), {"voxels": 2, "quads": 6, "exposed_faces": 10, "decoded_colors": 2, "meshes": 1})
        step={(x,y,0):RED for x in range(2) for y in range(2)}
        step[(0,0,1)]=GREEN
        run("shared-contact-backfill", world([((0,0,0),region(step))]), {"exposed_faces":20, "hidden_faces":lambda n:n>0})
        run("large-palette", world([((0, 0, 0), region({((i % 32) * 3, (i // 32) * 3, 3): 0x8000 | i for i in range(300)}))]), {"voxels": 300, "max_palette": 300, "exposed_faces": 1800})
        run("r16-merged-colors", world([((0, 0, 0), region({(i % 20, i // 20, 3): 0x8000 | i for i in range(300)}))]), {"voxels": 300, "quads": 6, "exposed_faces": 670, "decoded_colors": 300, "mixed_quads": 6})
        run("uniform-emitter-override", world([((0, 0, 0), region({(i % 20, i // 20, 3): 0x8000 | i for i in range(300)}))]), {"quads": 670, "decoded_colors": 300, "mixed_quads": 0}, uniform=True)
        code = 0x4000 | (15 << 10) | 2
        material = 2 | (struct.unpack("<H", struct.pack("<e", 6.4))[0] << 16)
        run("emission-scale", world([((0, 0, 0), region({(0, 0, 0): code}))]), {"emissive_quads": 6, "first_material": material})
        run("free-and-physics", world([((0, 0, 0), region({(0, 0, 0): RED}, free=(1, 2), physics=True))]), {"voxels": 1})
        run("duplicate-sector", world([((0, 0, 0), one), ((0, 0, 0), one)]), error="duplicate sector")
        run("duplicate-brick", world([((0, 0, 0), region({(0, 0, 0): RED}, duplicate_id=True))]), error="duplicate brick")
        run("bad-freelist", world([((0, 0, 0), region({(0, 0, 0): RED}, free=(1, 1)))]), error="freelist")
        run("invalid-code", world([((0, 0, 0), region({(0, 0, 0): 0x100}))]), error="noncanonical")
        run("glass-without-palette", world([((0, 0, 0), region({(0, 0, 0): 1}))]), error="glass requires")
        run("clear-white-glass", world([((0, 0, 0), region({(0, 0, 0): 132}))]), {"quads": 6, "first_color": 0xffffff00, "first_material": 8}, clear_glass=True)
        run("glass-opaque-interface", world([((0, 0, 0), region({(0, 0, 0): 1, (1, 0, 0): RED}))]), {"exposed_faces": 11, "decoded_colors": 2}, clear_glass=True)
        run("reserved-glass", world([((0, 0, 0), region({(0, 0, 0): 133}))]), error="reserved glass", clear_glass=True)
        run("unsupported-version", world([((0, 0, 0), one)], version=9), error="unsupported file version")
        run("bad-flags", world([((0, 0, 0), one)], flags=2), error="compression flags")
        broken = world([((0, 0, 0), one)])
        broken[64 + 16388] ^= 0xFF
        run("bad-deflate", broken, error="deflate")
        run("truncated", world([((0, 0, 0), one)])[:-9], error="entity count")
        print(f"CXW reader/mesher: {count} tests PASS")


if __name__ == "__main__":
    main()
