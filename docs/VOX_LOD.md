# VOX ray tracing LOD

Runtime VOX LOD is **disabled by default**. Without an explicit `MIKAN_VOX_LOD=1`, the engine uses original geometry and skips CPU proxy preparation, proxy BLASes, GPU selection and LOD TLAS updates. Isolated/forced/primary/GPU LOD options do not enable the master switch.

When explicitly enabled for diagnostics, the temporary policy uses two levels for **every nonempty VOX triangle BLAS**:
exact LOD0 and one remesh/simplify proxy LOD1. Triangle count, merge factor,
material type and static/dynamic entity flags do not filter candidates.
The 25% triangle target is a preference: a valid result that misses it is
retained. If remeshing is unsuitable, the exact source is simplified instead;
an irreducible mesh can keep the original triangle count at LOD1.

CPU and GPU selection are limited to LOD0/LOD1. By default they switch outward
at 40 world units and inward below 36, retaining distance hysteresis.
Only one proxy is generated and built. Proxy cache version 2 and a new algorithm
key prevent old three-proxy results from being reused.

Primary hits, indirect lighting, reflections and shadows use the same selected
level. GPU selection updates the per-instance BLAS and hit-table index, then
refits the TLAS. Emissive sources retain an exact metadata-only leaf for light
sampling; that leaf is invisible to camera and ordinary secondary rays.
Proxy triangles carry their projected palette material, preserving mirror,
PBR, glass and emission flags. DDA/AABB/software modes keep their own existing
representations; ordinary non-VOX models are outside this VOX-only policy.

Environment options (set before launch):

| Variable | Default | Meaning |
| --- | --- | --- |
| `MIKAN_VOX_LOD` | `0` | Explicit `1` opts into diagnostic mesh LOD; otherwise all runtime LOD paths remain off |
| `MIKAN_VOX_LOD_PRIMARY` | `1` | Set `0` to keep exact primary hits while retaining secondary LOD |
| `MIKAN_VOX_LOD_GPU` | `1` | GPU distance/hysteresis selection and TLAS update; `0` retains CPU selection for diagnostic comparison |
| `MIKAN_VOX_LOD_ONLY` | unset | Isolated test mode, 0–1; load only this level and disable distance selection |
| `MIKAN_VOX_LOD_DISTANCES` | `40` | Single transition distance; the first value of legacy comma-separated input is accepted |
| `MIKAN_VOX_LOD_FORCE` | `-1` | Force level 0–1; -1 selects automatically |
| `MIKAN_VOX_LOD_MIN_TRIANGLES` | ignored | Candidate filtering is temporarily disabled |
| `MIKAN_VOX_LOD_MAX_MERGE` | ignored | Candidate filtering is temporarily disabled |
| `MIKAN_VOX_LOD_CACHE` | `cache/vox-lod` | CPU proxy cache directory |

Source/proxy preparation is cached per view until geometry, transforms or
material/visibility inputs change. Camera position is excluded from that CPU
cache key in GPU mode. Each fence-complete view slot owns its selection state,
instance buffer, descriptors and TLAS scratch. A stationary camera reuses its
last GPU selection. Transform-only changes upload transforms and run GPU selection
before the TLAS refit. Proxy shading records contain a
float32 flat normal, palette color and material (20 bytes per triangle);
intersection vertices remain in the BLAS and are not fetched again for shading.

CPU meshes are content-keyed, compressed `.vlod` files with a checksum and
algorithm version. Invalid caches regenerate automatically. Proxy BLAS also
uses the existing device-specific BLAS disk cache. Original geometry is kept
resident, so GPU LOD adds memory and is not guaranteed to improve static views.
Isolated `MIKAN_VOX_LOD_ONLY` tests retain their single-level loading behavior
and bypass GPU distance selection.

Build `MikanVoxLodBake` to precompute CPU proxies from a `.vox` file:

```powershell
out/build/x64-release/MikanVoxLodBake.exe source.vox report.json
```

For an alternating primary-only comparison (secondary LOD enabled in both):

```powershell
./tools/benchmark_rt_gpu_tables.ps1 -Gpu '780M' -ComparePrimaryLod -HalfLighting -Warmup 600 -Samples 300 -Pairs 2 -Capture
```

The historical `local=0/1` result field denotes the selected comparison
switch, recorded as `memory_switch` in `summary.json`. For this comparison it
is `MIKAN_VOX_LOD_PRIMARY`. Selection logs include `primaryLOD=0/1`.

For isolated tests, `MIKAN_VOX_LOD_ONLY=0` skips proxy loading. Value 1
skip original BLAS creation and original raster geometry upload, upload only
the requested proxy, and reject an unavailable level instead of falling back.
CPU source surfaces are still read for content identity and cache validation;
only the selected decoded proxy triangle array remains resident. Cold cache
generation produces the reusable LOD1 cache before isolated sampling.
Isolated emissive proxies use their own triangle light list.

The benchmark uses the project's currently configured scene and game camera.
Run the following to measure each isolated level twice in forward/reverse order,
then capture matching images independently of the timing runs:

```powershell
./tools/benchmark_vox_lod_isolated.ps1
python tools/compose_vox_lod_comparison.py out/bench-rt-test/menger-isolated
```

Direct remesh inspection: set `MIKAN_VOX_LOD_DIRECT_REMESH_SCALE=0.25`
and `MIKAN_VOX_LOD_ONLY=1`. The scale applies to the longest grid axis,
rounded to an integer (81 becomes 20 for menger). This diagnostic bypasses
simplification and uploads the remesher output as the isolated level-1 proxy.
Its cache identity is separate from normal simplified proxies. It is not
subject to the former 25% acceptance gate.

For an assembly such as castle, every nonempty component now participates.
The former `-CandidateFiltering` switch is ignored by the isolated benchmark.
Triangle count and merge-factor environment thresholds are also ignored under
this temporary policy. Missing invalid/resource-limited proxies retain LOD0;
valid proxies are not discarded merely for missing the preferred reduction.

Use `benchmark_rt_gpu_tables.ps1 -Gpu 4090 -HalfLighting -CompareVoxLod`
to compare default distance-selected LOD against completely disabled LOD.

## Shader variants with LOD disabled

Normal triangle rendering automatically loads compile-time native shader variants
with proxy decoding, primitive debug and custom traversal removed. Full-resolution
lighting also removes half-resolution sampling/reconstruction. Existing quality
controls switch/cache half/full pipelines; triangle debug loads a generic pipeline
when requested. Enabled LOD and custom traversal continue to use the generic module.

`MIKAN_HWRT_SHADER_VARIANTS=0` forces the generic control for profiling. An explicit
`MIKAN_HWRT_SHADER_OVERRIDE` binary is a diagnostic-only override. Build the
CompileShaders target to regenerate generic/native/native_full modules together.
The generic shader remains the compatibility fallback if native modules are absent.
