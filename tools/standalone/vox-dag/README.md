# VOX DAG experiments

These standalone tools measure repeated octree-subtree sharing and encode compiled VOX surfaces as a sparse-octree DAG. This is an offline storage experiment; the engine runtime does not load VMDA, and the result is not a Vulkan acceleration structure. It does not measure GPU memory or ray-tracing performance.

## Tools

- `VoxSvdDagStats.py` reads MagicaVoxel `.vox` files and compares expanded sparse-octree node counts with position-independent DAG node counts. Its estimated byte count excludes material tables and alignment.
- `VoxMeshDagPack.py` packs engine-generated v5 `.voxmesh` surfaces into `.vmda`. Unpacking restores the same geometry and attributes; canonical serialization may produce different bytes from the original file.

## Reproduce

Run the subtree statistics self-test and inspect the Menger model:

```sh
python3 tools/standalone/vox-dag/VoxSvdDagStats.py --self-test
python3 tools/standalone/vox-dag/VoxSvdDagStats.py tests/fixtures/vox-mit/menger.vox
```

First cook a `.voxmesh` with the Linux validation host. Use absolute input and output paths with `--cook`:

```sh
out/build/linux-runtime/MikanVoxRTValidate --cook \
  "$PWD/tests/fixtures/vox-mit/menger.vox" \
  "$PWD/out/menger.voxmesh"
```

Then pack, validate, and unpack it:

```sh
python3 tools/standalone/vox-dag/VoxMeshDagPack.py pack \
  out/menger.voxmesh out/menger.vmda \
  --roundtrip out/menger.roundtrip.voxmesh
python3 tools/standalone/vox-dag/VoxMeshDagPack.py unpack \
  out/menger.vmda out/menger.unpacked.voxmesh
```

The VMDA codec is DAG-specific. Applying zlib to the VMDA package is an optional second step and was measured separately from direct zlib compression of `.voxmesh`.

## Reference sizes

The following files were cooked by the Linux validation host from `tests/fixtures/vox-mit/` with the default static-contact backfill setting. zlib used level 9. Sizes are bytes; DAG round-trips passed geometry and attribute validation.

| Model | `.voxmesh` | Direct zlib | DAG + zlib | DAG minus direct zlib |
| --- | ---: | ---: | ---: | ---: |
| Knight | 3,832 | 1,550 | 1,366 | -184 |
| Teapot | 170,352 | 56,793 | 55,536 | -1,257 |
| Dragon | 251,128 | 87,279 | 88,333 | +1,054 |
| Menger | 1,106,000 | 67,274 | 66,879 | -395 |

These results show file-size behavior only. They do not imply the same reduction in a triangle BLAS. The current engine still expands surfaces into triangles before Vulkan ray tracing.
