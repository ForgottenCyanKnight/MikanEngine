# MikanEngine

<p align="center">
  <img src="android/app/src/main/res/drawable-nodpi/mikan_engine_icon.png" alt="MikanEngine logo" width="180">
</p>

<p align="center">基于 C++20、Vulkan 和 SDL3 的 2D/3D 游戏引擎，提供项目管理、运行时、桌面编辑器、物理、动画、C++ 玩法插件和自动化验证工具。</p>

<p align="right">简体中文 · <a href="README.en.md">English</a></p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus&logoColor=white" alt="C++20">
  <img src="https://img.shields.io/badge/Vulkan-AC162C?style=flat-square&logo=vulkan&logoColor=white" alt="Vulkan">
  <img src="https://img.shields.io/badge/SDL-3-1E90FF?style=flat-square" alt="SDL3">
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Android-555555?style=flat-square" alt="Platforms">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-2ea44f?style=flat-square" alt="MIT License"></a>
</p>

<p align="center">
  <a href="#技术栈与模块">技术栈</a> ·
  <a href="#架构">架构</a> ·
  <a href="#当前能力">功能</a> ·
  <a href="#获取依赖与构建">构建</a> ·
  <a href="#发布">发布</a> ·
  <a href="#使用自己的项目">运行与测试</a> ·
  <a href="#许可证">许可证</a>
</p>

Windows 桌面端是编辑器与渲染开发的主要入口；仓库同时保留 Android arm64 运行时构建与 APK 资源打包路径。功能可用性取决于平台、GPU 扩展和构建选项。

项目内容与引擎分离。`projects/` 中的本地项目暂不随仓库分发，使用项目管理器创建或导入自己的项目，再通过 `--project` 或 `-ProjectPath` 指定路径。

## 技术栈与模块

| 范围 | 当前实现 |
|---|---|
| 语言与构建 | C++20、CMake、Ninja、MSVC；玩法插件独立编译 |
| 图形与窗口 | Vulkan、SDL3、GLSL/SPIR-V |
| 编辑器与界面 | Dear ImGui、ImGuizmo；运行时 Canvas、精灵、文本和按钮 |
| 场景与资源 | ECS、JSON 场景与 Prefab、Assimp/glTF、VOX、项目资源清单 |
| 物理与动画 | Jolt 3D、Box2D 2D、骨骼动画、PMX/VMD 与 MMD 物理集成 |
| 音频与字体 | miniaudio、stb_vorbis、msdfgen |
| 渲染重建 | NRD REBLUR；可选 NVIDIA DLSS SR/RR、Streamline Frame Generation |

## 架构

```text
MikanEngine.exe            SDL/Vulkan 桌面宿主、主循环与项目启动
├── Game.dll               共享运行时：渲染、ECS、场景、物理、动画、音频
├── Editor.dll             可选桌面编辑器
└── Game<name>.dll          项目玩法插件：通过公开接口独立编译与热重载

MikanTestRunner.exe        CPU-only 玩法测试宿主
└── Game.dll / GameplayRuntime
```

编辑器和玩法插件复用 `Game.dll` 的运行时。`MikanTestRunner.exe` 执行场景、脚本、动画和物理逻辑，不创建窗口或初始化 SDL Video/Vulkan；渲染验证使用完整宿主的 headless 路径。

## 当前能力

### 渲染与重建

- Vulkan 模型与 PBR 材质、天空/大气、阴影、地形、体素/VOX、粒子、水体及 2D 渲染。
- glTF 骨骼动画、GPU skinning 与 CPU 回退；PMX 模型和 VMD 动作播放。
- 桌面硬件光追路径覆盖受支持的模型与 VOX 几何，包含直接光、间接光和反射；默认 NEE/MIS，另有可选的 ReSTIR Enhanced 路径。
- NRD REBLUR 漫反射/镜面去噪，以及 NVIDIA 设备上的可选 DLSS Super Resolution、Ray Reconstruction 和 Streamline Frame Generation。对应路径需要合适的 GPU、驱动和 SDK。
- TAA、GTAO、SSGI、Bloom、SMAA、CMAA2、色调映射等可配置后处理；Hi-Z、空间剔除和间接绘制路径。
- Shader 热重载、原始/重建信号抓取、GPU timestamp，以及 RenderDoc/Nsight 接入工具。

场景数据经 RenderWorld 快照、视图准备与剔除进入几何/光照和后处理阶段。Scene View 与 Game View 使用各自的相机与视图历史。具体后处理和光追模式由配置与设备能力决定。

### 运行时、编辑器与工具

- ECS、父子层级与 Transform、JSON 场景保存/加载、Prefab、项目资源路径解析和资源热更新。
- Jolt 3D 刚体/碰撞、Box2D 2D 物理、角色与相机控制、音频、2D Canvas/UI 和瓦片地图。
- C++ 脚本组件、项目玩法 DLL、插件编译与热重载；MMD 动画及 Jolt 物理播放路径。
- 桌面编辑器提供 Scene/Game View、层级与属性、变换操纵器、资源浏览与预览、材质与瓦片地图编辑、控制台、日志和渲染调试视图。
- 离线场景 Schema 校验、固定步长 CPU 玩法测试、Vulkan 渲染测试和 JSON 状态导出。
- `tools/` 提供 MCP、结构化场景命令、GameSpec/TestSpec 和外部 Agent 工作流入口；模型客户端通过这些接口提交规格，执行结果保留日志与证据。

## 项目基础结构

项目可以放在引擎仓库外。普通新建会生成 `project.json`、`scenes/main.json` 和 `games/`，资源目录可按需增加：

```text
MyProject/
├── project.json
├── scenes/
│   └── main.json
├── games/                 可选的 C++ 玩法模块，通常按插件名分子目录
├── models/                按需添加的项目资源
├── textures/
└── audio/
```

最小项目清单示意：

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

资源面板的“链接文件 / 链接文件夹”登记源路径，不复制素材。已有项目内资源继续使用项目相对路径；新链接保存在 `project.json.assets` 中，可由多个项目共用。移除外部链接不会删除源文件。Blender 转换缓存仍保存在源文件相邻 `.meta/` 中。

发布时，`tools/publish.ps1` 将外部资源和模型依赖收集到包内 `linked/` 目录，并将输出场景的引用改为相对路径；原项目不改写。glTF、PMX、OBJ 的依赖会解析收集，FBX/PMD/DAE 使用相邻贴图和常见纹理目录，复杂外部贴图布局应链接完整素材目录。链接失效或 Blender 尚未生成运行时 GLB 时，发布会报错。CPU 打包回归：`powershell -NoProfile -ExecutionPolicy Bypass -File tools/test_linked_assets.ps1`。

`scene` 指定默认场景；`game` 为空时不选择玩法插件，有玩法时填写模块名。`resourceRoot` 定义场景资源引用的基准，`codeRoot` 定义玩法源码目录；`assets` 记录项目资源。`engine/...` 引用引擎自带资源，与项目资源根分开。空场景可使用 `{"entities": []}`，实际组件与字段以 [场景 Schema](tools/scene_schema.json) 为准。

## 项目管理器支持的模板

| 模板入口 | 创建内容 | 资源条件 |
|---|---|---|
| 空白 3D 场景 | 主相机、方向光、默认天空盒及项目清单 | 由引擎生成，使用引擎默认资源 |
| 空白 2D 场景 | Camera2D、Canvas 及项目清单 | 由引擎生成 |
| 第三人称动画示例 | 复制角色、动画、相机、物理、场景及玩法源码 | 需要本地安装对应模板资源 |
| 贪吃蛇 2D 模板 | 场景 UI、方向控制、增长、食物及结束逻辑的起始项目 | 需要本地安装对应模板资源与玩法源码 |

后两项是编辑器已有的模板入口，其数据依赖本地项目资源；当前仓库不分发这些资源，缺失时创建操作会提示模板资源不足。空白 2D/3D 模板可直接生成。模板实现位于 [ProjectManagerWindow.cpp](src/Editor/ProjectManagerWindow.cpp)。

## 获取依赖与构建

Windows 环境需要 Visual Studio 2022 C++ 工作负载、Windows SDK、CMake **3.30+**、Ninja、Vulkan SDK、PowerShell、Git 和 Git LFS。LFS 用于仓库保留的二进制依赖，子模块用于固定版本上游依赖。

在仓库根目录执行：

```powershell
git lfs install
git lfs pull
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\initialize_submodules.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build.ps1 -ConfigureIfMissing
```

依赖初始化脚本固定 DLSS v310.9.1，并稀疏检出所需的 Windows x64 SDK 文件，避免下载完整多平台 SDK。SDK 和许可证都有哈希校验。默认构建生成 `MikanEngine.exe`、`Game.dll`、`Editor.dll` 和 Shader；可通过 `-Target Game|Editor|MikanTestRunner|CompileShaders` 选择目标，输出位于 `out/build/x64-Release/`。

Windows 的 Jolt、Box2D、msdfgen、ImGui 和 NRD 由独立 PC 依赖构建目录管理；运行时和编辑器复用这些结果。使用 Visual Studio Developer PowerShell 时，也可运行 `cmake --preset x64-release` 和 `cmake --build --preset x64-release --target MikanEngine`。

### Android arm64

`android/` 提供 SDL3/Vulkan 运行时、触控输入及 APK 构建路径，需要 Android SDK/NDK、CMake 3.22.1 和兼容的 JDK。本文的硬件光追与 NVIDIA 重建说明针对 Windows 桌面路径。使用自己的项目显式同步资源：

```powershell
$ProjectPath = "D:\MikanProjects\MyProject"
powershell -NoProfile -ExecutionPolicy Bypass -File .\android\sync_assets.ps1 -ProjectPath $ProjectPath
cd android
.\gradlew.bat :app:assembleDebug --console=plain
```

`android/app/src/main/assets/` 是生成目录。Android 玩法采用静态编译方式；新项目的玩法接入需要配置 Android 原生构建入口。

## 使用自己的项目

启动 `MikanEngine.exe` 后，可在项目管理器创建项目或导入 `project.json`。从命令行运行、编译和发布时显式指定项目：

```powershell
$ProjectPath = "D:\MikanProjects\MyProject"   # 替换为自己的项目目录
.\out\build\x64-Release\MikanEngine.exe --project $ProjectPath
.\out\build\x64-Release\MikanEngine.exe --project $ProjectPath --no-editor

# 有 C++ 玩法模块时，独立编译项目插件
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\compile_games.ps1 -ProjectPath $ProjectPath

# 校验场景；CPU 玩法测试需先构建 MikanTestRunner
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\validate_scene.ps1 "$ProjectPath\scenes\main.json" -ProjectPath $ProjectPath -CheckAssets
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build.ps1 -Target MikanTestRunner
.\out\build\x64-Release\MikanTestRunner.exe --project $ProjectPath --frames 60 --fixed-dt 0.016666667
.\out\build\x64-Release\MikanEngine.exe --project $ProjectPath --headless --frames 60

```

### 发布

```powershell
# 发布到独立目录；有玩法时先完成插件编译
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\publish.ps1 -ProjectPath $ProjectPath -OutDir "D:\MikanBuilds\MyProject"
```

没有玩法模块的空白项目，发布命令需追加 `-SkipGames`。

发布工具打包宿主、运行时 DLL、所选项目插件、引擎/项目资源和许可证，排除编辑器与玩法源码。`tools/test.ps1` 可按自定义测试清单运行 `validate`、`gameplay`、`render`、`all` 层；仓库默认清单引用本地项目，使用前需提供自己的可用项目或清单。

## 仓库结构

| 路径 | 职责 |
|---|---|
| `src/`、`include/` | 引擎实现与公开接口，按 Core、Rendering、ECS、Editor、UI、World 等模块组织 |
| `engine/` | 引擎 Shader、字体、纹理、UI 和语言资源 |
| `cmake/`、`CMakeLists.txt` | 构建目标、PC 依赖及可选 SDK 集成 |
| `dependencies/`、`.gitmodules` | 保留的第三方文件、固定上游子模块与来源清单 |
| `tools/` | 依赖初始化、构建、玩法编译、验证、发布和诊断工具 |
| `tests/` | 引擎级测试源码 |
| `android/` | Android 工程与资源同步入口 |
| `lib/` | 当前构建所需的预编译链接/运行库 |

本地项目、开发文档和构建产物不属于源码分发内容。入口见 [构建脚本](tools/build.ps1)、[依赖初始化](tools/initialize_submodules.ps1)、[测试脚本](tools/test.ps1) 和 [发布脚本](tools/publish.ps1)。

### Blender 模型导入

Windows 编辑器支持将 .blend 作为模型资源导入：在资源面板导入文件，然后拖入 Scene View，或在模型路径选择器选择文件。需要安装 Blender，可通过 BLENDER_EXECUTABLE 环境变量指定 blender.exe，设置后重新启动编辑器。首次转换同步执行，可能短暂停顿。

导入会保留源文件，在源文件相邻的 .meta/<文件名>.blend/ 下固定生成 model.glb、缓存索引及 import.log；重新转换覆盖旧结果，失败保留上次成功的模型，不积累随机版本目录。源文件、导出脚本或已记录的外部纹理/链接库变化时会重新转换；重新拖入或重新选择模型使用新版本。场景保存 GLB 引用，发布和运行无需 Blender。导入文件按钮只复制所选文件；有外部贴图或链接库时，请先在 Blender 中打包资源，或导入完整文件夹并保持相对路径有效。缺失外部资源会拒绝转换，错误写入 import.log。发布排除 .blend 源文件，保留 GLB 及相邻导入元数据。

导入范围为 Blender glTF 导出器支持的可见且允许渲染的网格、PBR 材质、骨骼和动画。拖入 Scene View 时，还会把点光源、聚光灯和平行光创建为模型的子实体；不转换相机；面光源目前按点光源近似导入。Blender 发光材质通过相邻的 model.glb.import.json 标记辐射单位，RT 不套用体素材质的历史发光增益。RT 点光源支持距离衰减、聚光锥和透明裁剪阴影。

Blender 节点图不能完整映射到 glTF：包含 mmd_base_tex 的 Shader-to-RGB 材质使用原始漫反射贴图转换为非发光 PBR，保留贴图透明度；纯程序发光材质自动烘焙 EMIT，不烘焙照明。完全透明的白色表面保留穿透语义。其他程序材质、世界背景、依赖光程的节点和复杂绑定仍需检查转换结果；RT 对 BLEND 使用随机透明覆盖，需时域积累以减轻颗粒，不能保证与 Eevee 完全相同。

## 许可证

引擎自身代码使用 [MIT License](LICENSE)。第三方库、SDK、衍生代码和项目素材保留各自许可；Enhanced ReSTIR 的 Falcor BSD-3-Clause 来源及其他依赖条款见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
