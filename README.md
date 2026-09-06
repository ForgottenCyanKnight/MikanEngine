# MikanEngine

使用 C++17、Vulkan 和 SDL3 构建的自研 2D/3D 游戏引擎，包含运行时、ImGui 编辑器、ECS 场景系统、物理、脚本插件、资源序列化和测试工具。

## 技术栈

| 类别 | 技术 |
|---|---|
| 语言与构建 | C++17、CMake、Ninja、MSVC |
| 图形与窗口 | Vulkan、SDL3、GLSL/SPIR-V |
| 编辑器与 UI | Dear ImGui |
| 场景与资源 | ECS、JSON、Assimp、glTF、Prefab |
| 物理 | Jolt Physics、Box2D |
| 其他 | msdfgen、RenderDoc/Nsight 调试工作流 |
| 平台 | Windows；包含 Android arm64 构建支持 |

## 架构

```text
EngineMain.exe                 桌面宿主：SDL/Vulkan 初始化、主循环和渲染运行
├── Game.dll                   运行时核心：Rendering / ECS / Scene / Physics / Audio
├── Editor.dll                 可选编辑器：ImGui 场景编辑、资产浏览、预览和调试
└── Game<name>.dll             游戏插件：具体玩法、脚本和项目逻辑，支持热重载

MikanTestRunner.exe            玩法测试宿主：不创建窗口，不初始化 SDL Video/Vulkan
└── Game.dll → GameplayRuntime 固定步长推进 ECS、物理、脚本和场景
```

引擎运行时、编辑器和游戏玩法分别组织。游戏插件通过 `Game.lib` 使用引擎公开接口，玩法代码可以独立编译和重新加载；编辑器作为可选动态模块加载。`MikanTestRunner.exe` 复用 `Game.dll` 的 GameplayRuntime，在没有窗口和 Vulkan 上下文的情况下运行玩法测试。

## 功能

### 渲染

- Vulkan 2D/3D 渲染，支持模型、材质、天空、阴影、体素世界和 2D 图元。
- glTF 骨骼动画、GPU skinning 和 CPU skinning 回退路径。
- TAA、GTAO、SSGI、Bloom、SMAA、CMAA2、Tonemap 等可配置后处理路径。
- Hi-Z 深度纹理、BVH/四叉树遮挡剔除和 GPU 间接绘制相关路径。
- GLSL/SPIR-V Shader 热重载，便于着色器迭代和调试。

渲染流程大致为：

```text
VulkanManager
    └── SceneRenderer
        └── SceneCollector
            └── Model / Voxel / Shadow / Sky / 2D Renderers
                └── PostProcessChain
                    └── Game View / Scene View / Swapchain
```

### 运行时与场景

- ECS 实体、组件注册和系统更新。
- 场景层级、Transform、Prefab、JSON 场景序列化和项目资源管理。
- C++ 脚本组件、游戏插件、脚本热重载和运行时状态导出。
- 模型、纹理、材质、动画、音频和场景资源加载。

### 编辑器

- ImGui 场景视图和运行时调试。
- 资产浏览、模型/纹理预览和材质编辑。
- 场景实体、组件、Transform 和资源引用编辑。

### 物理与测试

- Jolt Physics 3D 物理和 Box2D 2D 物理。
- CPU-only 玩法测试路径，不初始化 SDL Video 和 Vulkan。
- Vulkan headless 渲染测试路径，用于交换链、渲染管线和资源集成验证。
- 场景 JSON 离线校验、固定步长运行、状态 JSON 导出和自动化测试结果记录。

## 构建

### 环境要求

- Windows
- Visual Studio/MSVC x64 工具链
- CMake 3.21 或更高版本
- Ninja
- Vulkan SDK
- PowerShell

建议使用仓库提供的构建脚本。脚本会探测 Visual Studio 环境、检查引擎进程占用并将日志写入 `out/build/build.log`。

在项目根目录执行：

```powershell
# 默认构建 EngineMain（连带 Game、Editor 和 Shader）
powershell -NoProfile -ExecutionPolicy Bypass -File ./tools/build.ps1 -ConfigureIfMissing

# 按目标构建
powershell -NoProfile -ExecutionPolicy Bypass -File ./tools/build.ps1 -Target Game
powershell -NoProfile -ExecutionPolicy Bypass -File ./tools/build.ps1 -Target Editor
powershell -NoProfile -ExecutionPolicy Bypass -File ./tools/build.ps1 -Target MikanTestRunner
powershell -NoProfile -ExecutionPolicy Bypass -File ./tools/build.ps1 -Target CompileShaders
```

也可以在 Visual Studio Developer PowerShell 中使用 CMake Preset：

```powershell
cmake --preset x64-release
cmake --build --preset x64-release --target EngineMain
```

Windows 构建中，Jolt、msdfgen 和 Box2D 作为独立的 `STATIC` 目标管理，再由 `Game` 链接。构建输出位于 `out/build/x64-Release/`。

## 运行与测试

```powershell
# 启动引擎
./out/build/x64-Release/EngineMain.exe

# 关闭编辑器模式
./out/build/x64-Release/EngineMain.exe --no-editor

# 运行统一测试入口
powershell -NoProfile -ExecutionPolicy Bypass -File ./tools/test.ps1 -Layer gameplay
powershell -NoProfile -ExecutionPolicy Bypass -File ./tools/test.ps1 -Layer render

# 离线校验场景 JSON
powershell -NoProfile -ExecutionPolicy Bypass -File ./tools/validate_scene.ps1 ./assets/contact2d.json
```

`tools/test.ps1` 支持 `validate`、`gameplay`、`render` 和 `all` 测试层，也支持通过 `-Case` 选择测试场景。玩法层使用 `MikanTestRunner.exe`，渲染层使用 `EngineMain.exe --headless`。

完整构建、运行和测试参数以 `tools/` 下的脚本帮助和参数定义为准。

## 示例场景

| 文件 | 内容 |
|---|---|
| [`baka3d.json`](assets/baka3d.json) | 3D 模型、材质、脚本和基础场景 |
| [`cesium_man.json`](assets/cesium_man.json) | 骨骼动画和模型渲染 |
| [`voxel_world.json`](assets/voxel_world.json) | 体素世界 |
| [`contact2d.json`](assets/contact2d.json) | Box2D 接触和 2D 物理 |
| [`sponza_demo.json`](assets/sponza_demo.json) | 3D 场景和渲染验证 |

## 目录结构

| 目录 | 内容 |
|---|---|
| `src/`、`include/` | 引擎实现和公开头文件 |
| `games/` | 游戏插件和玩法代码 |
| `projects/` | 自包含项目及其场景、资源和玩法源码 |
| `engine/` | 引擎系统资源，如 Shader、字体和内置纹理 |
| `assets/` | 场景、模型、纹理、音频和地图等项目内容 |
| `tools/` | 构建、测试和场景处理工具 |
| `dependencies/` | Vulkan、SDL3、ImGui、Jolt、Box2D 等第三方依赖 |

## 文档与入口

- 构建配置：[`CMakePresets.json`](CMakePresets.json)
- 构建脚本：[`tools/build.ps1`](tools/build.ps1)
- 测试脚本：[`tools/test.ps1`](tools/test.ps1)
- 场景校验：[`tools/validate_scene.ps1`](tools/validate_scene.ps1)
