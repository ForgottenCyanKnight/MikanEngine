# VOX ray tracing LOD

Hardware triangle tracing uses four distance levels: exact voxel surfaces,
then meshoptimizer remesh/simplify proxies with budgets of 25%, 5%, and
1% of the original optimized triangle count. A proxy must be smaller than
the previous accepted level. Missing levels fall back to a finer level.

Primary hits, indirect lighting, reflections and shadows select the same
per-instance level from camera distance. Exact source geometry remains
available to metadata/emitter queries. Special voxel materials and dynamic
voxels retain their original geometry. Brick DDA and software paths do not
use these triangle proxies.

Defaults select candidates with at least 10,000 triangles and a greedy merge
factor no greater than 4. Distance is measured to the transformed component
center; thresholds are 40/80/160 world units with 10% hysteresis.

Environment options (set before launch):

| Variable | Default | Meaning |
| --- | --- | --- |
| `MIKAN_VOX_LOD` | `1` | Set `0` to disable all mesh LOD |
| `MIKAN_VOX_LOD_PRIMARY` | `1` | Set `0` to keep exact primary hits while retaining secondary LOD |
| `MIKAN_VOX_LOD_ONLY` | unset | Isolated test mode, 0–3; load only this level and disable distance selection |
| `MIKAN_VOX_LOD_DISTANCES` | `40,80,160` | Camera distance thresholds |
| `MIKAN_VOX_LOD_FORCE` | `-1` | Force level 0–3; -1 selects automatically |
| `MIKAN_VOX_LOD_MIN_TRIANGLES` | `10000` | Candidate triangle threshold |
| `MIKAN_VOX_LOD_MAX_MERGE` | `4` | Candidate merge factor ceiling |
| `MIKAN_VOX_LOD_CACHE` | `cache/vox-lod` | CPU proxy cache directory |

CPU meshes are content-keyed, compressed `.vlod` files with a checksum and
algorithm version. Invalid caches regenerate automatically. Proxy BLAS also
uses the existing device-specific BLAS disk cache. Original geometry is kept
resident, so LOD adds memory and is not guaranteed to improve every scene.

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

For isolated tests, `MIKAN_VOX_LOD_ONLY=0` skips proxy loading. Values 1–3
skip original BLAS creation and original raster geometry upload, upload only
the requested proxy, and reject an unavailable level instead of falling back.
CPU source surfaces are still read for content identity and cache validation;
only the selected decoded proxy triangle array remains resident. Cold cache
generation produces the reusable three-level cache before isolated sampling.
This mode is intended for ordinary static voxel test assets.

The bench project now contains one menger model with a fixed close game camera.
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
Its cache identity is separate from normal simplified proxies. The diagnostic
rejects output exceeding the existing 25% level-1 triangle budget.
