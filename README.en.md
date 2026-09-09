# MikanEngine

<p align="center">
  <img src="android/app/src/main/res/drawable-nodpi/mikan_engine_icon.png" alt="MikanEngine logo" width="180">
</p>

<h1 align="center">MikanEngine</h1>

<p align="center">
  A C++20, Vulkan and SDL3 based 2D/3D game engine for Windows and Android, covering runtime, editor, scenes, physics, scripting and packaging.
</p>

<p align="right">
  <a href="README.md">简体中文</a> · English
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus&logoColor=white" alt="C++20">
  <img src="https://img.shields.io/badge/Vulkan-AC162C?style=flat-square&logo=vulkan&logoColor=white" alt="Vulkan">
  <img src="https://img.shields.io/badge/SDL-3-1E90FF?style=flat-square" alt="SDL3">
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Android-555555?style=flat-square" alt="Platforms">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-2ea44f?style=flat-square" alt="MIT License"></a>
</p>

<p align="center">
  <a href="#technology-stack">Technology stack</a> ·
  <a href="#architecture">Architecture</a> ·
  <a href="#features">Features</a> ·
  <a href="#build">Build</a> ·
  <a href="#run-and-test">Run and test</a> ·
  <a href="#license">License</a>
</p>

## Technology stack

| Category | Technology |
|---|---|
| Language and build | C++20, CMake, Ninja, MSVC |
| Graphics and windowing | Vulkan, SDL3, GLSL/SPIR-V |
| Editor and UI | Dear ImGui |
| Scenes and assets | ECS, JSON, Assimp, glTF, Prefab |
| Physics | Jolt Physics, Box2D |
| Other | msdfgen, RenderDoc/Nsight workflows |
| Platforms | Windows; Android arm64 build support |

## Architecture

~~~text
MikanEngine.exe                Desktop host: SDL/Vulkan setup, main loop and runtime rendering
├── Game.dll                   Runtime core: Rendering / ECS / Scene / Physics / Audio
├── Editor.dll                 Optional editor: ImGui scene editing, asset inspection and debugging
└── Game<name>.dll             Game plugin: project gameplay, scripts and project logic with hot reload

MikanTestRunner.exe            Gameplay test host: no window, SDL Video or Vulkan initialization
└── Game.dll → GameplayRuntime fixed-step ECS, physics, scripting and scene execution
~~~

The runtime, editor and project gameplay are organized as separate modules. Game plugins use the public engine interface through `Game.lib` and can be built and reloaded independently; the editor is loaded as an optional dynamic module. `MikanTestRunner.exe` reuses `Game.dll`'s `GameplayRuntime` to run gameplay tests without a window or Vulkan context.

## Projects and assets

Desktop builds use a project-driven startup flow: the project manager only selects a project, and the runtime does not implicitly treat the engine directory or root `assets/` directory as a default project. Import an existing `project.json` from the startup page, or create a project by choosing a parent directory and project name. Standalone runs, tests and gameplay-plugin builds should pass `--project` or `-ProjectPath` explicitly.

A project uses the following basic layout:

~~~text
projects/<project-name>/
├── project.json          Project manifest, default scene, gameplay module and asset allowlist
├── scenes/               Scene files
├── assets/               Optional project asset directory
├── models/, textures/    Other project asset directories
└── games/                Project gameplay source inside the project directory
~~~

`project.json` controls the project asset root through `resourceRoot` and the gameplay source root through `codeRoot` (normally the project-local `games` directory). Relative scene asset paths are resolved from `resourceRoot`; `engine/...` is reserved for built-in engine assets. Gameplay plugins are compiled only from the current project's `codeRoot`; the repository no longer provides a global `games/` compilation entry point. Android still uses the APK `assets/` boundary, while desktop builds do not implicitly read the repository root `assets/` directory. Root-level `assets/` content is not part of a project or release package and old test material can remain local to development machines.

## Features

### Rendering

- Vulkan 2D/3D rendering for models, materials, sky, shadows, voxel worlds and 2D primitives.
- glTF skeletal animation, GPU skinning and a CPU skinning fallback path.
- Configurable TAA, GTAO, SSGI, Bloom, SMAA, CMAA2 and tonemapping post-process paths.
- Hi-Z depth textures, BVH/quadtree occlusion culling and GPU indirect-draw paths.
- GLSL/SPIR-V shader hot reload for shader iteration and debugging.

The rendering flow is organized approximately as follows:

~~~text
VulkanManager
    └── SceneRenderer
        └── SceneCollector
            └── Model / Voxel / Shadow / Sky / 2D Renderers
                └── PostProcessChain
                    └── Game View / Scene View / Swapchain
~~~

### Runtime and scenes

- ECS entities, component registration and system updates.
- Scene hierarchy, transforms, prefabs, JSON scene serialization and project asset management.
- C++ script components, game plugins, script hot reload and runtime state export.
- Loading for models, textures, materials, animation, audio and scene assets.

### Editor

- ImGui scene view and runtime debugging.
- Asset browser, model/texture preview and material editing.
- Editing for scene entities, components, transforms and asset references.

### Physics and testing

- Jolt Physics 3D physics and Box2D 2D physics.
- CPU-only gameplay tests without SDL Video or Vulkan initialization.
- Vulkan headless rendering tests for swapchain, pipeline and resource integration.
- Offline scene JSON validation, fixed-step execution, JSON state export and automated test result recording.

## Build

### Requirements

- Windows
- Visual Studio/MSVC x64 toolchain
- CMake 3.21 or newer
- Ninja
- Vulkan SDK
- PowerShell

The repository build scripts are recommended. The desktop root CMake project, Android native CMake project and Windows gameplay-plugin build scripts all use C++20. The scripts detect the Visual Studio environment, check for engine process locks and write build logs to `out/build/build.log`.

From the project root:

~~~powershell
# Build MikanEngine by default (including Game, Editor and shaders)
powershell -NoProfile -ExecutionPolicy Bypass -File ./tools/build.ps1 -ConfigureIfMissing

# Build individual targets
powershell -NoProfile -ExecutionPolicy Bypass -File ./tools/build.ps1 -Target Game
powershell -NoProfile -ExecutionPolicy Bypass -File ./tools/build.ps1 -Target Editor
powershell -NoProfile -ExecutionPolicy Bypass -File ./tools/build.ps1 -Target MikanTestRunner
powershell -NoProfile -ExecutionPolicy Bypass -File ./tools/build.ps1 -Target CompileShaders
~~~

Visual Studio Developer PowerShell can also use the CMake preset:

~~~powershell
cmake --preset x64-release
cmake --build --preset x64-release --target MikanEngine
~~~

For Windows builds, Jolt, msdfgen and Box2D are managed as independent `STATIC` targets and linked into `Game`. Build output is written to `out/build/x64-Release/`.

### Android arm64

The Android project is located in `android/`. It builds the engine runtime as `mikanengine.so` and packages an `arm64-v8a` APK. Android SDK, NDK, CMake 3.22.1 and a JDK compatible with the Android Gradle Plugin are required; the Gradle wrapper downloads Gradle from its official distribution endpoint.

Before the first build, synchronize engine assets and the selected project into the temporary APK `assets/` directory, then run the Gradle build. The default project is `projects/third-person-navigation`; use `-ProjectPath` to select another project:

~~~powershell
cd android
powershell -NoProfile -ExecutionPolicy Bypass -File .\sync_assets.ps1 -ProjectPath ..\projects\third-person-navigation
.\gradlew.bat :app:assembleDebug --console=plain
~~~

`android/app/src/main/assets/` is a generated synchronization directory and should not be committed. Android SDL AARs and the arm64 Assimp runtime are stored in `android/app/libs/`; the larger Assimp binary is managed with Git LFS, so install and enable Git LFS before cloning the repository.

## Run and test

~~~powershell
# Start the project manager; it does not select a default project automatically
.\out\build\x64-Release\MikanEngine.exe

# Open a project explicitly (editor mode)
.\out\build\x64-Release\MikanEngine.exe --project .\projects\third-person-navigation

# Run a project without the editor
.\out\build\x64-Release\MikanEngine.exe --project .\projects\third-person-navigation --no-editor

# Run a fixed number of headless frames; --scene defaults to project.json.scene
.\out\build\x64-Release\MikanEngine.exe --project .\projects\third-person-navigation --headless --frames 60 --no-voxel-world

# Build a gameplay plugin for a specific project
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\compile_games.ps1 -ProjectPath .\projects\third-person-navigation

# Run the unified test entry point
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test.ps1 -Layer gameplay
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test.ps1 -Layer render

# Validate a project's scene JSON offline
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\validate_scene.ps1 .\projects\third-person-navigation\scenes\main.json -ProjectPath .\projects\third-person-navigation -CheckAssets
~~~

`tools/test.ps1` supports the `validate`, `gameplay`, `render` and `all` test layers, as well as selecting a test scene with `-Case`. The gameplay layer uses `MikanTestRunner.exe`; the render layer uses `MikanEngine.exe --headless`. Each test case explicitly binds a project path and builds the corresponding project plugin.

Refer to the help output and parameter definitions in `tools/` for the complete build, run and test options.

## Example scenes

| File | Content |
|---|---|
| [`projects/third-person-navigation/scenes/main.json`](projects/third-person-navigation/scenes/main.json) | Project-based third-person scene, materials and gameplay source |
| [`projects/third-person-navigation/scenes/terrain.json`](projects/third-person-navigation/scenes/terrain.json) | Terrain scene in the same third-person project |

## Directory layout

| Directory | Content |
|---|---|
| `src/`, `include/` | Engine implementation and public headers |
| `projects/*/games/` | Gameplay plugin source for each project |
| `projects/` | Self-contained projects with scenes, assets and gameplay source |
| `engine/` | Built-in engine assets such as shaders, fonts and textures |
| `tools/` | Build, test, scene-processing tools and bundled tool packages |
| `tools/ktx/` | KTX-Software runtime components and KTX2 conversion tools |
| `dependencies/` | Third-party dependencies such as Vulkan, SDL3, ImGui, Jolt and Box2D |

## Documentation and entry points

- Build configuration: [`CMakePresets.json`](CMakePresets.json)
- Build script: [`tools/build.ps1`](tools/build.ps1)
- Test script: [`tools/test.ps1`](tools/test.ps1)
- Scene validation: [`tools/validate_scene.ps1`](tools/validate_scene.ps1)

## License

MikanEngine's own code is licensed under the [MIT License](LICENSE). Third-party dependencies and project assets remain subject to their respective licenses, copyright notices and source requirements.
