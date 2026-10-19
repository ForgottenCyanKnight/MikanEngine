# Linux builds

Linux has two build profiles based on upstream commit `07413151`: a low-dependency
CPU validation build and a full Vulkan desktop runtime build. The runtime uses
the production engine, renderer and project scene loader, but does not build the
Windows editor. Keep the profiles in separate build directories.

## Full Linux runtime

Requirements: Linux x86_64, GCC 12+ or Clang with C++20 support, CMake 3.21+,
Ninja, Git, Git LFS, glslangValidator, Vulkan loader/development files, Assimp,
X11 development files, and FFmpeg if exporting video. On Debian/Ubuntu:

```sh
sudo apt-get install build-essential cmake ninja-build git git-lfs glslang-tools \
  libvulkan-dev libassimp-dev libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxi-dev libxfixes-dev libxrender-dev libxkbcommon-dev
git submodule update --init --depth 1
git lfs install --local
git lfs pull
cmake --preset linux-runtime
cmake --build --preset linux-runtime --parallel 4
```

The executable is `out/build/linux-runtime/MikanEngine`. Run a project in
standalone game mode with:

```sh
out/build/linux-runtime/MikanEngine --project /path/to/project --no-editor
```

For a fixed-frame headless smoke test, SDL's offscreen driver can run without an
X server when the Vulkan driver provides `VK_EXT_headless_surface`. For Mesa
Lavapipe:

```sh
SDL_VIDEODRIVER=offscreen \
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
out/build/linux-runtime/MikanEngine --project /path/to/project \
  --headless --frames 2 --fixed-dt 0.0333333
```

The first runtime launch precomputes the atmosphere transmittance and
four-order scattering LUTs, then caches their GPU readback for later launches.
The default cache is under SDL's per-user `MikanEngine/Runtime/cache` directory
(with a temporary-directory fallback). Set `MIKAN_ATMOSPHERE_LUT_CACHE` to a
specific file path to choose the cache location. The cache is checked against
the LUT dimensions, formats, iteration count and precompute SPIR-V contents;
changing those inputs triggers one regeneration. The final sky image remains
dynamic and is still updated from scene lighting and camera altitude.

For normal desktop use, launch from an X11 session without the offscreen
environment variable. Linux KTX2 texture loading and dynamic gameplay plugin
loading are not implemented. NRD is disabled by default on Linux, and DLSS is
Windows-only. Fetch real engine textures and fonts with Git LFS; pointer files
pass the current existence check but cannot be decoded at runtime.

## Linux CPU validation build

Requirements: Linux x86_64, C/C++20 compiler (GCC 12+ recommended), CMake 3.21+,
Ninja, Git, pthreads and iconv (included in glibc). Python 3 is optional for the
two mathematical reference tests. A GPU is not required. The ReSTIR reference
test also requires one 768 KiB Git LFS lookup-table fixture; the rest of the
suite does not require LFS assets.

```sh
git submodule update --init --depth 1 \
  dependencies/JoltPhysics dependencies/glm dependencies/box2d \
  dependencies/SDL3 dependencies/vulkan
git lfs install --local
git lfs pull --include='engine/textures/restir/paired-neighbors-3-16.u32.bin' --exclude=''
cmake --preset linux-cpu
cmake --build --preset linux-cpu --parallel 2
ctest --preset linux-cpu --output-junit results.xml
```

The equivalent explicit configuration is:

```sh
cmake -S . -B out/build/linux-cpu -G Ninja \
  -DMIKAN_CPU_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build out/build/linux-cpu --parallel 2
ctest --test-dir out/build/linux-cpu --output-on-failure
```

`MIKAN_CPU_ONLY` defaults to ON on Linux, and OFF elsewhere. Use a separate build
directory for this profile: its SDL is built with window/audio/GPU backends
disabled. Windows and Android build entries are unchanged.

## What runs

Executables are in `out/build/linux-cpu/`:

- `MikanCpuCoreTests`: the engine's job scheduler, ECS entity/component storage,
  stale generation handles, UTF-8 paths, and **Physics2DManager** running a real
  Box2D floor/body simulation for 240 fixed steps. The final stdout line is JSON.
- `MikanMmdTests --interpolation`: existing playback clock, VMD interpolation
  and cached key interval contracts.
- `MikanMmdTests --physics`: existing Jolt/MMD shapes, gravity, collision masks,
  constraints, reset, damping and 120 Hz time stepping contracts; no model asset
  is required. Existing PMX/VMD file arguments remain supported for supplied assets.
- `MikanProjectAssetLinksTests`: real ProjectManager manifest persistence,
  external links, UTF-8 paths, safe unlink and Linux case-sensitive asset identity.
  CTest creates a fresh fixture directory for every run.
- Viewport resolution/docking, VOX reduction statistics, emissive-light indexing
  and CPU ray-tracing vertex deformation regressions. The ImGui tests perform
  CPU layout only; they do not create a GPU context.
- `MikanLinuxVmdEncodingTests`: both production VMD readers decode CP932 names
  to UTF-8 and read a generated VMD file through a UTF-8 Linux path.
- `MikanVoxModelStats`: the production VOX loader and surface bake with
  per-unit-face coverage/material audits. Four pinned MIT models and independent
  Python adjacency counts are included; see [VOX_TESTING.md](VOX_TESTING.md).
- Optional `MIKAN_SOFTWARE_VULKAN=ON`: real CPU Vulkan geometry rendering through
  SDL/X11 and Xvfb, using unchanged production VOX shaders. See
  [SOFTWARE_VULKAN.md](SOFTWARE_VULKAN.md) for scope, dependencies and known pixel differences.
- Python voxel DDA and ReSTIR mathematics tests, when Python is available.

CTest writes detailed diagnostics to
`out/build/linux-cpu/Testing/Temporary/LastTest.log`; the JUnit report can be
collected by CI. `.github/workflows/linux-cpu.yml` provides a Linux CI job.

## Portability changes

- The `linux-cpu` profile uses an isolated CMake path with source-built SDL3,
  Jolt and Box2D. Vulkan **headers** are needed by a CPU geometry header, but
  this profile does not link a Vulkan library.
- Windows case-insensitive asset comparisons are retained on Windows; Linux and
  Android preserve path case, including manifest deduplication and removal.
- Both VMD readers use iconv/CP932 on Linux instead of leaving Japanese names as
  raw CP932 bytes. Windows and Android encoding branches remain unchanged.
- Assertions remain enabled in assert-based test executables even in Release.
  The MMD tests use explicit checks and retain Jolt's matching assertion ABI.

## Verified cloud result

Tested on Debian 13 x86_64, GCC 14.2.0, glibc 2.41 and CMake 4.4.4.
The initial 12 CTest entries passed; the suite now includes two additional
VOX surface/model regressions (14 total). The 240-step
Physics2DManager simulation settled at y = 0.499912, within 0.05 of the expected
0.5. The voxel DDA reference checked 8,000 cases. Dynamic dependency inspection
of `MikanCpuCoreTests` showed only the standard C/C++/math runtime libraries,
with no Vulkan, OpenGL or window-system library dependency.

The GitHub Actions Linux workflow has not been run from this cloud checkout.
The full runtime was also started with Lavapipe through SDL's offscreen driver:
the project manifest, scene, camera and GLB cube loaded; the renderer reported
one model draw group; and a two-frame 640×360 H.264 stream passed `ffprobe`.
The offscreen readback pixels were black, so this smoke test verifies runtime
startup, scene loading and the capture/encoder path, but not visible pixel
correctness. Several checked-in engine images and fonts were Git LFS pointers in
this checkout and logged decode errors. A desktop GPU/X11 render still needs
validation.

## Linux video capture

The final Swapchain video-capture path supports Linux desktop builds with an
installed FFmpeg executable. It streams RGBA/BGRA frames to FFmpeg without a
shell, encodes H.264 MP4, and accepts even output dimensions from 64x64 through
7680x4320. The default is 1920x1080; 640x360 and 854x480 are useful low-resolution
16:9 sizes. Supported output rates are 30, 60 and 120 FPS.

Example for a 4-second gameplay clip with 10 warmup frames:

```sh
MikanEngine --project /path/to/project --headless --frames 130 \
  --video-output /tmp/gameplay.mp4 --video-encoder ffmpeg \
  --video-size 640 360 --video-fps 30 --video-warmup 10
```

`--frames` includes warmup frames, so 130 total frames produce 120 captured
frames (4 seconds at 30 FPS). Video export reads the rendered Vulkan swapchain;
it works with a desktop Vulkan driver or a headless software driver that can
create an SDL Vulkan surface. The `linux-cpu` profile remains validation-only
and does not produce `MikanEngine`.

## Runtime scope

The full Linux profile builds and runs the real `MikanEngine` host, `Game`
library, SDL/Vulkan renderer, physics, project manifest and scene loading. It
does not build the editor or Windows gameplay DLL loader. Headless use still
requires a Vulkan implementation; Lavapipe is suitable for smoke tests but is
much slower than a GPU. Linux KTX2 decoding and DLSS are unavailable, NRD is
disabled by default, and valid Git LFS payloads are needed for engine textures
and fonts. Windows and Android targets are unchanged by this profile.

The optional software Vulkan configuration now also includes headless CPU Ray Query.
All 16 CTest entries passed with both software rendering tests enabled.
See [SOFTWARE_RAY_QUERY.md](SOFTWARE_RAY_QUERY.md) for scope, images and timings.

A 960×540 Knight still with production NEE GI and full four-order atmosphere is
documented in [KNIGHT_540P.md](KNIGHT_540P.md).

The near-eye-level view and RT sky shader data flow are documented in
[SKY_RAY_QUERY.md](SKY_RAY_QUERY.md).

CPU Vulkan 上的 NRD 双信号实测与同帧对照见 [SOFTWARE_NRD.md](SOFTWARE_NRD.md)。
