# MikanEngine

<p align="center">
  <img src="android/app/src/main/res/drawable-nodpi/mikan_engine_icon.png" alt="MikanEngine logo" width="180">
</p>

<p align="center">A C++20, Vulkan and SDL3 2D/3D game engine with project management, a runtime, desktop editor, physics, animation, C++ gameplay plugins and automated validation tools.</p>

<p align="right"><a href="README.md">简体中文</a> · English</p>

Windows is the main platform for editor and rendering development. The repository also provides an Android arm64 runtime build and APK asset-packaging path. Feature availability depends on the platform, GPU extensions and build options.

Project content is separate from the engine. Local projects under `projects/` are temporarily excluded from this repository. Create or import your own project, then select it with `--project` or `-ProjectPath`.

## Technology and modules

| Area | Current implementation |
|---|---|
| Language and build | C++20, CMake, Ninja, MSVC; separately compiled gameplay plugins |
| Graphics and windowing | Vulkan, SDL3, GLSL/SPIR-V |
| Editor and UI | Dear ImGui, ImGuizmo; runtime Canvas, sprites, text and buttons |
| Scenes and assets | ECS, JSON scenes and Prefabs, Assimp/glTF, VOX, project asset manifests |
| Physics and animation | Jolt 3D, Box2D 2D, skeletal animation, PMX/VMD and MMD physics integration |
| Audio and fonts | miniaudio, stb_vorbis, msdfgen |
| Reconstruction | NRD REBLUR; optional NVIDIA DLSS SR/RR and Streamline Frame Generation |

```text
MikanEngine.exe            SDL/Vulkan desktop host, main loop and project startup
├── Game.dll               Shared runtime: rendering, ECS, scenes, physics, animation, audio
├── Editor.dll             Optional desktop editor
└── Game<name>.dll          Project gameplay plugin: public interfaces and independent hot reload

MikanTestRunner.exe        CPU-only gameplay test host
└── Game.dll / GameplayRuntime
```

The editor and gameplay plugins reuse the runtime in `Game.dll`. `MikanTestRunner.exe` executes scene, script, animation and physics logic without creating a window or initializing SDL Video/Vulkan. Rendering validation uses the full host's headless path.

## Current capabilities

### Rendering and reconstruction

- Vulkan model/PBR rendering, sky and atmosphere, shadows, terrain, voxel/VOX geometry, particles, water and 2D rendering.
- glTF skeletal animation, GPU skinning with a CPU fallback, PMX models and VMD animation playback.
- Desktop hardware ray tracing for supported model and VOX geometry, including direct lighting, indirect lighting and reflections. NEE/MIS is the default estimator, with an optional ReSTIR Enhanced path.
- NRD REBLUR diffuse/specular denoising, plus optional DLSS Super Resolution, Ray Reconstruction and Streamline Frame Generation on NVIDIA devices. These paths require suitable GPU, driver and SDK support.
- Configurable TAA, GTAO, SSGI, Bloom, SMAA, CMAA2 and tonemapping; Hi-Z, spatial culling and indirect-draw paths.
- Shader hot reload, raw/reconstructed signal capture, GPU timestamps and RenderDoc/Nsight integration tools.

Scene data passes through RenderWorld snapshots, per-view preparation and culling, geometry/lighting and post-processing. Scene View and Game View maintain separate cameras and view histories. Configuration and device capabilities determine the active post-processing and ray-tracing paths.

### Runtime, editor and tools

- ECS, transform hierarchies, JSON scene saving/loading, Prefabs, project asset-path resolution and asset hot reload.
- Jolt 3D rigid bodies/collision, Box2D 2D physics, character/camera control, audio, 2D Canvas/UI and tilemaps.
- C++ script components, project gameplay DLL compilation and hot reload, plus MMD animation and Jolt physics playback.
- Desktop Scene/Game Views, hierarchy and properties, transform gizmos, asset browsing/previews, material and tilemap editing, a console, logs and rendering debug views.
- Offline scene-schema validation, fixed-step CPU gameplay tests, Vulkan rendering tests and JSON state export.
- MCP, structured scene commands, GameSpec/TestSpec and external-agent workflow interfaces in `tools/`. Model clients submit specifications through these interfaces; execution records logs and evidence.

## Basic project structure

Projects can live outside the engine repository. A regular new project creates `project.json`, `scenes/main.json` and `games/`; add asset directories as needed:

```text
MyProject/
├── project.json
├── scenes/
│   └── main.json
├── games/                 Optional C++ gameplay modules, usually one directory per plugin
├── models/                Project assets added as needed
├── textures/
└── audio/
```

A minimal project manifest:

```json
{
  "formatVersion": 1,
  "name": "MyProject",
  "scene": "scenes/main.json",
  "game": "",
  "resourceRoot": ".",
  "codeRoot": "games",
  "editorPostProcessChain": "",
  "assets": ["scenes/main.json"]
}
```

`scene` selects the default scene. An empty `game` selects no gameplay plugin; otherwise it names the module. `resourceRoot` defines the base for scene asset references, `codeRoot` selects the gameplay source directory, and `assets` lists project resources. `engine/...` references built-in engine assets separately from project resources. An empty scene can use `{"entities": []}`; supported components and fields are defined in the [scene schema](tools/scene_schema.json).

## Project-manager templates

| Template entry | Created content | Resource requirements |
|---|---|---|
| Blank 3D scene | Main camera, directional light, default skybox and manifest | Generated by the engine using built-in defaults |
| Blank 2D scene | Camera2D, Canvas and manifest | Generated by the engine |
| Third-person animation | Copies character, animation, camera, physics, scenes and gameplay source | Requires the corresponding template resources installed locally |
| Snake 2D | Starter gameplay with scene UI, directional controls, growth, food and game-over logic | Requires the corresponding template resources and gameplay source installed locally |

The latter two entries are implemented in the editor but depend on local project resources. Those resources are not distributed in this repository; creation reports missing template resources when they are unavailable. Blank 2D/3D templates generate their scenes directly. See [ProjectManagerWindow.cpp](src/Editor/ProjectManagerWindow.cpp) for the implementation.

## Dependencies and build

Windows builds require the Visual Studio 2022 C++ workload, Windows SDK, CMake **3.30+**, Ninja, Vulkan SDK, PowerShell, Git and Git LFS. LFS manages retained binary dependencies; submodules pin upstream dependencies.

From the repository root:

```powershell
git lfs install
git lfs pull
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\initialize_submodules.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build.ps1 -ConfigureIfMissing
```

Dependency initialization pins DLSS v310.9.1 and sparsely checks out the required Windows x64 SDK files instead of the complete multi-platform SDK. SDK files and licenses are hash-checked. The default build produces `MikanEngine.exe`, `Game.dll`, `Editor.dll` and shaders. Select individual targets with `-Target Game|Editor|MikanTestRunner|CompileShaders`. Output is written to `out/build/x64-Release/`.

Windows Jolt, Box2D, msdfgen, ImGui and NRD are managed in a separate PC dependency build directory and reused by the runtime/editor. Visual Studio Developer PowerShell can also run `cmake --preset x64-release` followed by `cmake --build --preset x64-release --target MikanEngine`.

### Android arm64

`android/` provides the SDL3/Vulkan runtime, touch input and APK build path. It requires Android SDK/NDK, CMake 3.22.1 and a compatible JDK. The hardware ray-tracing and NVIDIA reconstruction descriptions above apply to the Windows desktop path. Synchronize your own project explicitly:

```powershell
$ProjectPath = "D:\MikanProjects\MyProject"
powershell -NoProfile -ExecutionPolicy Bypass -File .\android\sync_assets.ps1 -ProjectPath $ProjectPath
cd android
.\gradlew.bat :app:assembleDebug --console=plain
```

`android/app/src/main/assets/` is generated. Android gameplay is compiled statically; integrating a new project's gameplay requires configuring the Android native build entry point.

## Using your own project

Start `MikanEngine.exe` to create a project or import `project.json` in the project manager. Select the project explicitly when running, compiling gameplay or packaging:

```powershell
$ProjectPath = "D:\MikanProjects\MyProject"   # Replace with your project directory
.\out\build\x64-Release\MikanEngine.exe --project $ProjectPath
.\out\build\x64-Release\MikanEngine.exe --project $ProjectPath --no-editor

# Build the project plugin when it contains C++ gameplay
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\compile_games.ps1 -ProjectPath $ProjectPath

# Validate the scene; build MikanTestRunner before CPU gameplay testing
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\validate_scene.ps1 "$ProjectPath\scenes\main.json" -ProjectPath $ProjectPath -CheckAssets
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build.ps1 -Target MikanTestRunner
.\out\build\x64-Release\MikanTestRunner.exe --project $ProjectPath --frames 60 --fixed-dt 0.016666667
.\out\build\x64-Release\MikanEngine.exe --project $ProjectPath --headless --frames 60

# Package into a separate directory; compile any gameplay plugin first
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\publish.ps1 -ProjectPath $ProjectPath -OutDir "D:\MikanBuilds\MyProject"
```

For a blank project without a gameplay module, add `-SkipGames` to the packaging command.

Packaging includes the host, runtime DLLs, selected gameplay plugin, engine/project assets and licenses; it excludes the editor and gameplay source. `tools/test.ps1` runs the `validate`, `gameplay`, `render` and `all` layers with a custom test manifest. Its default manifest references local projects, so supply an available project or your own manifest before using it.

## Repository layout

| Path | Responsibility |
|---|---|
| `src/`, `include/` | Engine implementations and public interfaces organized into Core, Rendering, ECS, Editor, UI, World and other modules |
| `engine/` | Engine shaders, fonts, textures, UI and language resources |
| `cmake/`, `CMakeLists.txt` | Build targets, PC dependencies and optional SDK integration |
| `dependencies/`, `.gitmodules` | Retained third-party files, pinned upstream submodules and provenance manifests |
| `tools/` | Dependency initialization, building, gameplay compilation, validation, packaging and diagnostics |
| `tests/` | Engine-level test sources |
| `android/` | Android project and asset synchronization |
| `lib/` | Prebuilt link/runtime libraries required by the current build |

Local projects, development notes and generated build output are excluded from the source distribution. Entry points include the [build script](tools/build.ps1), [dependency initializer](tools/initialize_submodules.ps1), [test script](tools/test.ps1) and [packaging script](tools/publish.ps1).

## License

The engine's own code is licensed under the [MIT License](LICENSE). Third-party libraries, SDKs, derived code and project assets retain their own terms. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), including the GPL and additional permissions for the optional Prime-derived Enhanced path.
