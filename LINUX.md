# Linux CPU adaptation

This is an initial Linux **CPU validation** port of MikanEngine, based on upstream
commit `07413151`. It compiles production engine sources and existing regression
tests, without a Vulkan loader, graphics device, window server, Windows DLLs or
the local-only sample projects. It is not yet a Linux editor or the complete
`MikanTestRunner` gameplay host.

## Build and run

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

- Added an isolated CMake path using source-built SDL3, Jolt and Box2D. Vulkan
  **headers** are needed by a CPU geometry header, but no Vulkan library is linked.
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

The Linux workflow has been added but has not been executed on GitHub Actions;
Windows, Android and graphical Linux builds have not been revalidated here.

## Remaining work for a full Linux runtime

The existing `MikanTestRunner` links the full `Game` library, whose globals and
implementation still pull in rendering. It is deliberately not replaced with a
stub or a different scene interpreter in this port. Running arbitrary project
scenes and gameplay plugins on Linux still requires:

1. Separating renderer-owned global objects from the gameplay runtime link graph.
2. Providing platform-neutral runtime/editor/plugin loading and removing remaining
   Windows export/library assumptions from the full build.
3. Building Linux Assimp, SDL_image and other full-runtime dependencies.
4. Adapting Vulkan window/surface management, shader compilation and font services.
5. Exercising actual rendering with a supported GPU or separately configured
   software driver, then validating project loading, fixed-step state dumps and
   plugin lifecycle with the full host.

Passing this suite establishes CPU-module portability on the tested Linux machine;
it does not establish renderer, editor, full scene playback or mobile driver parity.

The optional software Vulkan configuration now also includes headless CPU Ray Query.
All 16 CTest entries passed with both software rendering tests enabled.
See [SOFTWARE_RAY_QUERY.md](SOFTWARE_RAY_QUERY.md) for scope, images and timings.

A 960×540 Knight still with production NEE GI and full four-order atmosphere is
documented in [KNIGHT_540P.md](KNIGHT_540P.md).

The near-eye-level view and RT sky shader data flow are documented in
[SKY_RAY_QUERY.md](SKY_RAY_QUERY.md).

CPU Vulkan 上的 NRD 双信号实测与同帧对照见 [SOFTWARE_NRD.md](SOFTWARE_NRD.md)。
