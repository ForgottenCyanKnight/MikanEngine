# Vulkan VOX DDA benchmark

`MikanVoxDdaPreview` runs the engine's production `traceVoxelDDA` shader in a
headless Vulkan compute pass. It outputs VOX palette albedo as RGBA8, so each
variant can be checked for both hit coverage and color. The preview uses one
center ray per pixel at 640×480, warms up twice, and reports the median of five
dispatch/submit/readback trials. VOX loading, mip construction and upload,
pipeline creation, and optional CPU screen-bound projection are outside the
timed region.

The variants are:

| Option | DDA data and traversal |
|---|---|
| default | Production 3D `R8_UINT` occupancy texture and mip levels |
| `--optimized-dda` | Linear SSBO occupancy mips and vectorized stepping |
| `--bitset-dda` | Bit-packed SSBO occupancy mips and vectorized stepping |
| `--morton-dda` | Morton-indexed bitset mip levels and vectorized stepping |
| `--morton-rays` | Bitset mips with Morton-ordered pixel workgroups |
| `--screen-bounds` | Bitset mips plus conservative projected model-bounds rejection |

The screen-bounds variant projects all eight VOX grid corners on the CPU and
skips DDA for pixels outside the padded screen rectangle. It adds a four-pixel
margin and disables rejection if any corner is at or behind the camera plane.
This is a primary-ray benchmark; it does not cover lighting, shadows, GI,
denoising, or runtime frame scheduling. Do not infer a GPU frame-rate gain from
the Lavapipe numbers alone.

## Build and run

Configure the Linux software Vulkan targets using the setup in
[`SOFTWARE_VULKAN.md`](SOFTWARE_VULKAN.md), then build and run a model:

```sh
cmake --build out/build/linux-cpu --target MikanVoxDdaPreview --parallel 2
out/build/linux-cpu/MikanVoxDdaPreview tests/fixtures/vox-mit/menger.vox --screen-bounds
```

The preview selects a CPU Vulkan device by default. Pass `--allow-gpu` to permit
a hardware Vulkan device for testing on a development machine. Supply the Vulkan
ICD, layer, and runtime environment required by that machine, as described in
the software Vulkan setup.

Each run writes `dda-report.json`, `knight-albedo.rgba`, and `knight-mask.raw`
under `out/dda-knight-primary/`; copy the report after each variant because the
next run replaces it. The report records the selected device, variant, hit
count, five trial timings, and Vulkan validation counts.

## Full engine integration (Windows / GPU)

`MIKAN_HWRT_DDA_SCREEN_BOUNDS=1` enables whole-volume DDA with bitset SSBO
occupancy mips and conservative primary-ray screen rectangles in the full
lighting/denoising pipeline. `MIKAN_HWRT_DDA_BITSET=1` enables the same bitsets
without screen rejection. Both imply `MIKAN_HWRT_VOXEL_DDA=1`; leave all three
unset for the default hardware triangle path. Brick and software/procedural
quad traversal switches cannot be combined with bitset DDA.

Level-zero palette indices remain in the original R8 texture. Bitsets are built
once per registered asset, packed once per unique grid in each cached scene
buffer, and referenced by the otherwise-unused record word 23. Transformed
instances retain separate matrices, shading identities and shadow masks.
View-local rectangles are updated from the current camera and instance bounds,
with four pixels of margin. Camera-plane crossings disable rejection. Only
primary rays use rectangles by default. The separate opt-in
`MIKAN_HWRT_DDA_SECONDARY_SCREEN_BOUNDS=1` tests secondary ray segments against
the projected conservative cone, failing open for camera-plane crossings.
Secondary lighting, emission, RR guide decoding and NRD/DLSS
selection retain the existing DDA surface contract.

`tests/VoxDdaOptimizationTests.cpp` checks packed mip bits against brute-force
occupancy for non-cubic volumes and checks conservative projected bounds.

## NAADF-inspired experimental backend

`MIKAN_HWRT_DDA_NAADF=1` selects a separate software traversal backend at
startup. It implies VOX DDA; `MIKAN_HWRT_DDA_SCREEN_BOUNDS=1` may additionally
enable the same primary rectangle broad phase used by the bitset comparison.
Do not combine it with hardware brick, procedural quad or software quad modes.
Unset the NAADF flag to restore the original bitset/texture path.

The design follows [NAADF](https://github.com/cg-tuwien/NAADF) and its
[paper](https://doi.org/10.1111/cgf.70413), with an independent implementation:
4^3 voxels form a block; 4^3 blocks form a chunk. Mixed cells reference the
next layer; uniformly full cells store a palette id. Each empty cell stores
six extents defining an entirely empty cuboid, expanded by testing complete
slabs. These are not independent axis-to-obstacle distances. Chunk extents
use 5 bits per direction; block and voxel extents use 2. Voxel entries are
packed in 16 bits. The ray jumps to the cuboid exit, advancing tied axes.

`VoxNaadf.h` builds immutable per-asset data synchronously at registration;
instances share the payload through the existing DDA scene buffer. It does
not implement block hashing or the paper's background incremental updates.
Asset re-registration rebuilds the payload. Existing inside-solid exit,
face, material, emitter and instance contracts remain in the shared DDA
traversal. All ray types use this backend; NEE/MIS and NRD remain unchanged.
No new descriptor binding or class data member is introduced.

`tests/VoxNaadfTests.cpp` checks decoded materials and every empty cuboid
against source occupancy, then compares 100000 rays against uniform stepping
across empty, full, sparse, block-pattern, odd and anisotropic volumes.
GPU equivalence and timing are separate checks; CPU tests do not prove either.

## Historical Lavapipe sample comparison

One 640×480 run per variant used `menger.vox` (160,000 occupied voxels,
128³ padded grid), with two warm-up dispatches and five timed samples per
variant. All paths reported 74,586 hit pixels, byte-identical RGBA output and
hit masks, and zero Vulkan validation errors or warnings.

| Variant | Median dispatch / submit / readback |
|---|---:|
| Production texture mips | 12.224 ms |
| Vectorized SSBO mips | 11.944 ms |
| Bitset SSBO mips | 11.389 ms |
| Morton-indexed mip bitsets | 11.794 ms |
| Morton ray scheduling | 10.646 ms |
| Bitset mips + screen bounds | **9.026 ms** |

Screen bounds was about 26% faster than the production texture-mip path in this
sample. This is one model on CPU Lavapipe, not a general GPU result; repeat the
same comparison on the target hardware before choosing a production path.
