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

## Lavapipe sample comparison

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
