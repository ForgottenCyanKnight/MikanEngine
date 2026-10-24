# CXW to engine voxmesh import

CXW is a source/import format. The runtime scene uses an ordinary standalone
`.voxmesh` asset produced by the same voxel surface optimizer and serializer
as VOX files.

## Convert and run

```powershell
./tools/build.ps1 -Target MikanCxwInspect
./tools/convert_cxw.ps1
./tools/run_cxw_citadel.ps1
```

Defaults:

* Source: `C:/Users/Forgo/Downloads/epic-citadel.8192.packed.cxw`
* Output: `projects/cxw-citadel/assets/epic-citadel.voxmesh`
* Sample scene: `projects/cxw-citadel/scenes/main.json`

`convert_cxw.ps1` accepts `-Source`, `-Output`, `-UniformMaterials`, and
`-ClearGlass` (explicitly substitute every source glass tint with white
dielectric transmission when the external glass palette is unavailable). The
sample project now references only `assets/epic-citadel.voxmesh`; rendering it
does not read the source CXW. The resulting asset can be moved with the project.

Large conversions automatically write bounded-size compiled assemblies and a
v9 `.voxmesh` entry with relative shard paths. Keep the adjacent
`<entry>.parts-*` directory with the entry file when moving it. Runtime loads
and flattens the compiled assemblies; it does not reread or rebake the CXW.
Shards default to 256 MiB of raw compiled data; `MIKAN_CXW_SHARD_MIB` (1..512)
controls the threshold. Small conversions retain the existing single-file format.
Sharding limits individual file sizes, not total runtime residency: the current
loader loads all parts and their geometry. A successful conversion does not
establish that a very large world fits CPU/GPU memory. Rootfall was converted
but failed to load in the user's runtime and is not a validated sample.

The editor also accepts a CXW in its voxel picker/asset-panel drag-and-drop.
That entry now invokes the same CPU cooker into `cache/cxw/*-v3.voxmesh` and
then calls the normal compiled-surface loader. It has no separate CXW runtime
mesh generator. Native OS file-drop support is not part of this change.

## One canonical optimizer

`CxwLoader` decodes sparse sector/brick data and supplies canonical exterior
faces and occupancy queries to `BakeVoxSurfaces`, the existing VOX baker.
Its previous independent greedy partitioner has been removed.

Adjacent 128-cubed sectors are grouped into 256-cubed conversion tiles, matching
the engine's compact quad-coordinate range. This allows coplanar rectangles
and contact optimization across sector boundaries inside each tile. The shared
baker performs cross-color merging, eight-orientation selection, opaque contact
backfill, real-face/color coverage audits and R8/R16 attribute round-trip checks.

The shared baker now supports larger palettes through R16 indices; old VOX R8
outputs remain supported. Source emission stays uniform per rectangle and an
entity-emission override uses `-UniformMaterials` to disable cross-color merging
and contact backfill. Both the offline importer and VOX cooker use the existing
`EncodeCompiledSurface` and atomic compressed `.voxmesh` writer.

Conversion is CPU-only and uses up to four bounded workers by default. Results
are serialized in a stable order; tests verify byte-identical files with one
and four workers. `MIKAN_CXW_WORKERS` can select 1..16 workers. The world bounding
box is never allocated as one dense grid.

## Supplied Citadel file

* CXW v6, 3837 sectors, 1,095,051 bricks, 71,496,242 nonempty voxels.
* 5712 source packed colors; 5668 colors occur on exterior surfaces.
* Previous separate importer: 3837 components, 31,052,378 quads.
* Canonical voxmesh cooker: **936 components, 29,079,093 quads**.
* Shared-baker baseline: 31,751,480 quads before its contact/D4 selection.
* Real exterior unit faces: 141,620,124. Added hidden contact samples:
  57,961,815, audited to be inside opaque contacts.
* Standalone voxmesh: 256,651,594 bytes. Conversion measured 178.2 seconds.

The final geometry has 6.35% fewer quads than the previous importer and about
75.6% fewer components. The serialized file is slightly larger than the former
253,068,457-byte cache: fewer geometric rectangles and a smaller byte stream are
different objectives, especially when contact backfill adds indexed samples.

Evidence is in `out/cxw/convert-shared.log`, `convert-shared-time.json`, and
`epic-voxmesh.log`. The older importer's screenshots remain available for a
same-camera comparison.

## Scope

The decoder follows the supplied Caelix byte-layout reference, versions 1..8,
including legacy entity records and optional player-pose metadata. Raw deflate,
absolute offsets, brick maps/freelists/slots, duplicates and canonical codes are
validated. Individual decompressed sectors are capped at 64 MiB.

Packed RGB555 opaque colors and RGB444 emission (`0.1 * 8^s`) are supported.
Block-ID mode cannot be detected from the file and is not supported. Glass
requires the external Caelix tint palette and fails explicitly by default;
`-ClearGlass` / `MIKAN_CXW_CLEAR_GLASS=1` permits clear-white substitution. This
Citadel source has no glass or emissive cells. Physics bodies/velocities,
automata and player-camera behavior are not instantiated by a geometry import.

Entity translations and quaternion rotations are retained. Geometry is Y-up
and centered on aggregate source-sector bounds. The serialized transform
cancels the existing MagicaVoxel Z reflection. Use the normal scene transform
for scale/placement; the sample uses scale 0.02.

The validated runtime path is hardware triangles. The source importer does not
create native CXW DDA brick grids. The existing growable BLAS conversion pools
support large compiled voxel assemblies and reuse retired pages safely.

## Tests

```powershell
python tests/CxwLoaderTests.py out/build/x64-release/MikanCxwInspect.exe
python tests/CxwCookTests.py out/build/x64-release/MikanCxwInspect.exe
./tools/build.ps1 -Target MikanVoxRTValidate
./out/build/x64-release/MikanVoxRTValidate.exe --cpu-backfill
```

29 decoder/canonical-baker tests, 8 serialized-cook tests, and 18 original VOX
contact-backfill fixtures pass. The VOX fixtures check external nearest hits
and material values across six directions, windows, boundaries and emissive /
glass contacts. CPU test logs and runtime screenshots are separate from
performance measurements; these are not steady-state FPS benchmarks.
