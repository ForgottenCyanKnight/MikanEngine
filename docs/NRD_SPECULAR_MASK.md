# Specular NRD ROI experiment

## Whole-frame empty fast path

`MIKAN_HWRT_NRD_SKIP_EMPTY_SPEC=1` is enabled by default. It uses a standalone
specular instance; `MIKAN_HWRT_NRD_JOINT=1` explicitly retains the joint backend
instead. `MIKAN_HWRT_NRD_SKIP_EMPTY_SPEC=0` disables the empty fast path.

A GPU reduction checks actual material enable/reflectance, copying one uint to
the current fenced slot. It does not wait synchronously for the GPU. The prior
result is reused only with identical scene/material/transform revision, camera,
projection and jitter. Empty jitter phases are cached separately (maximum 64).
Camera or scene changes discard the cache. Therefore moving views still run
normal denoising until a matching empty result is known. Skipped output is
cleared, and the next actual denoiser invocation restarts history.

The producer previously overwrote specular-material alpha with 1 at the end of
lighting, even on diffuse-only surfaces. This was corrected to use
`surface.specularEnabled`. Earlier ROI timings below used that overly broad mask
and are not representative of a corrected sparse reflection mask.

After the correction, a diffuse-only cube test on AMD 780M at 960x540 primary /
480x270 denoising measured 0.942 ms specular NRD without skipping and 0.008 ms
with skipping (180 measured frames). A reflective room still ran specular NRD
and exited normally. Brick and triangle startup regressions also passed after
a complete EXE/Game/Editor rebuild.

The build helper no longer forces English MSVC output against CMake's localized
`/showIncludes` prefix. Ninja now records the shared headers as valid dependencies.

## Spatial ROI prototype (opt-in)

`MIKAN_HWRT_NRD_SPEC_MASK=1` selects separate diffuse/specular instances and
enables native REBLUR tile rejection outside a conservative specular ROI.
The default remains the joint instance when both signals have matching extents.

The classifier reads reflection material enable/reflectance, not noisy radiance.
Using a noisy black sample as proof that no reflection exists would incorrectly
reject paths that happen to miss a bright emitter in the current frame.

Each 16x16 denoiser tile scans its source pixels, including all four pixels when
the signal uses stride 2. Active tiles scatter into a dilated mask. The margin is
128 denoiser pixels by default, plus the tile's maximum screen-space motion and
one extra tile. `MIKAN_HWRT_NRD_SPEC_MASK_MARGIN` accepts 128..512 pixels.
Current and previous masks are unioned; history resets also clear mask history.

Only the specular adapter marks inactive depth as out of denoising range.
NRD's native classifier/early outs then reject it. Packed output is cleared
every frame, and NRD's depth history rejects reappearing surfaces. Diffuse guides
are unaffected. No SDK shader modification is required.

Initial AMD 780M / brick / half-resolution static comparison, 180 samples:

| Path | NRD time |
|---|---:|
| Separate diffuse | 0.920 ms |
| Separate specular | 0.827 ms |
| Joint diffuse/specular | 1.436 ms |
| Masked specular, including classification | 1.015 ms |

This conservative prototype is not enabled by default because the measured
specular cost increased. Dynamic boundary/disocclusion validation and mask
coverage instrumentation are still needed. A black displayed specular texture
does not establish a small material mask. Whole-frame times varied substantially
and should not be used to attribute a speedup to this prototype.
