# MikanEngine 项目上下文与代码地图

> 快照日期：2026-08-21  
> 工程根目录：`D:\Engine project\vulkan engine`  
> 用途：供新对话或更换模型后快速建立上下文，减少重复全盘查询。  
> 更新规则：目录结构、模块职责、构建入口或关键系统发生变化时同步更新本文。

## 0. 新对话读取顺序

1. 先读本文，建立项目边界和入口认知。
2. 再读 `AGENTS.md`，获取构建、编码、备份和场景约定。
3. 只有任务涉及具体模块时，读取下文列出的入口文件及其直接依赖。
4. 构建命令查 `编译命令.md`；发展方向查 `docs/开发路线图_2026H2.md`。
5. 不要为了“了解项目”递归扫描第三方、备份、构建产物或大资源目录。

## 1. 项目一句话定位

MikanEngine 是个人开发的 C++17 Vulkan 游戏引擎，包含运行时、ImGui 编辑器、ECS 场景、2D/3D 物理、游戏插件、项目管理、序列化、动画、音频、体素世界、现代后处理和 Android 支持。

当前架构：

```text
EngineMain.exe（薄宿主，src/HostMain.cpp）
├─ Game.dll（运行时：Core / Rendering / ECS / UI / World）
├─ Editor.dll（编辑器，可选动态加载）
└─ Game<name>.dll（games/<name>，玩法插件，可热重载）

MikanTestRunner.exe（玩法测试薄宿主，src/Core/GameplayTestHost.cpp）
└─ Game.dll → GameplayRuntime（无窗口、无 SDL Video/Vulkan 初始化）
```

编辑器存在时进入编辑器模式；`--no-editor` 或 headless 使用纯游戏模式。自动测试按运行层拆分：玩法层走 MikanTestRunner，渲染层走 EngineMain。玩法逻辑应放在 `games/` 插件，不应重新塞回 `src/Game/`。

## 2. `.gitignore` 审计结果与扫描边界

### 2.1 `.gitignore` 明确忽略

- 构建：`out/`、`build/`、Android 各级 `build/`、`.cxx/`。
- IDE：`.vs/`、`.idea/`、`.trae/`、`*.user`、`*.suo`。
- 日志/临时：`*.log`、`crash_log.txt`、`imgui.ini`、`t.json`。
- 缓存：`bvh/`、顶层 `models/`、`*_bvh.txt`、所有 `*.spv`。
- 备份：`*.bak`、顶层 `_backup_*` 和 `_*_bak*`。
- 大资产：`assets/models/`，以及 `projects/` 下的 FBX/KTX2/TGA/DDS/HDR/OBJ。
- Android 临时项：`.gradle/`、`.idea/`、`temp_sdl/`、`local.properties`、日志、APK/AAB。

### 2.2 默认上下文扫描区

通常只扫描：

```text
CMakeLists.txt
src/
include/
games/
tools/                 （排除 tools/glslang 的第三方内容）
engine/shaders/glsl/
engine/postprocess_chain.json
assets/*.json          （仅任务相关场景）
projects.json
android/app/src/main/cpp/
android/app/src/main/java/com/mikanengine/
README.md
AGENTS.md
编译命令.md
docs/开发路线图_2026H2.md
```

### 2.3 默认禁止扫描区

除非任务直接相关，不读取：

```text
out/                    构建产物
backup/                 多份历史源码镜像，会污染搜索结果
_backup_*/              顶层快照
.vs/                    IDE 数据
log/                    历史日志
dependencies/           第三方源码，日常只通过接口使用
third_party/             第三方库
tools/glslang/           第三方 Shader 工具
lib/、dll/               二进制依赖
assets/models/           大体量 glTF/模型样本
android/**/build/        Android 构建产物
android/_sdl_aar/        SDL 上游内容
engine/shaders/spv/      可再生成 Shader 产物
```

注意：`dependencies/`、`third_party/`、`lib/`、`dll/` 按 `.gitignore` 决策可能进入版本控制，但它们仍不属于常规模型上下文。只有编译错误或第三方接口问题才深入检查。

当前目录已初始化为 Git 仓库，`main` 已推送到远程；文件清单应以 `.gitignore + git ls-files` 为权威来源。构建产物和备份文件仍保留在本地，但不会进入提交。

## 3. 顶层项目大纲

```text
vulkan engine/
├─ CMakeLists.txt              Windows/Android 共用的主要构建定义
├─ CMakePresets.json           CMake 预设
├─ AGENTS.md                   强制工作约定、构建测试和历史陷阱
├─ README.md                   功能大纲与使用说明
├─ PROJECT_CONTEXT.md          本文：快速上下文和代码地图
├─ 编译命令.md                构建、运行、测试命令
├─ docs/                       开发路线、待办和原型指南
├─ projects.json               已登记项目清单
├─ src/                        引擎实现
├─ include/                    公共接口；游戏插件只能依赖这里
├─ games/                      独立玩法插件源码
├─ engine/                     引擎自有运行时资产
├─ assets/                     旧式/测试项目内容和场景
├─ tools/                      构建、发布、校验、MCP 工具
├─ android/                    Android 包装工程
├─ dependencies/              随仓库第三方源码
├─ third_party/                其他第三方 SDK
├─ lib/、dll/                 预编译库和运行时 DLL
├─ out/                        Windows 构建产物，忽略
└─ backup/、_backup_*/         历史快照，禁止常规扫描
```

## 4. 运行时主流程

```text
src/HostMain.cpp
  → 动态加载 Game.dll
  → 调用 Game 入口
src/Core/EngineMain.cpp
  → 解析命令行 / 初始化 SDL、Vulkan、ECS、物理、音频
  → ProjectManager/SceneManager 加载项目与场景
  → 可选加载 Editor.dll
  → 每帧：输入 → 脚本/游戏插件 → 物理/ECS → 编辑器 → 渲染 → present
  → headless 使用固定时间步，执行完整渲染回归并导出 runtime_layer=render-vulkan

src/Core/GameplayTestHost.cpp
  → 动态链接 Game.dll，调用 MikanGameplayTestMain
src/Core/GameplayRuntime.cpp
  → RuntimeCapabilities::GameplayTest（rendering/audioDevice/inputDevices=false）
  → 初始化 ECS、Jolt/Box2D、场景、插件和脚本，不初始化 SDL Video/Vulkan
  → 固定步执行物理、Camera2D、Tween、SpriteAnimator、插件/脚本
  → 导出 runtime_layer=gameplay-cpu 后确定性退出
```

渲染主链大致为：

```text
EngineMain
  → VulkanManager（设备、交换链、帧资源和顶层帧调度）
  → SceneRenderer（场景准备、2D/3D 路径选择）
  → SceneCollector（从 ECS 收集渲染项）
  → 各专用 Renderer（模型/体素/阴影/天空/2D）
  → PostProcessChain（GTAO/SSGI/TAA/Bloom/SMAA/CMAA2/Tonemap 等）
  → Game View / Scene View / Swapchain
```

场景和玩法主链：

```text
project.json / assets/*.json
  → ProjectManager / SceneManager
  → SceneSerializer
  → SceneECS + ComponentRegistry
  → ScriptSystem + GameManager
  → Game<name>.dll
```

## 5. 活跃代码树

### 5.1 `src/` 实现树

```text
src/
├─ HostMain.cpp                      EXE 薄宿主、DLL 加载入口
├─ StbVorbis.c                       Vorbis 解码实现
├─ Core/
│  ├─ EngineMain.cpp                主循环、参数、headless、自测
│  ├─ VulkanManager.cpp             Vulkan 上下文、交换链、帧调度
│  ├─ EngineGlobals.cpp             跨模块运行时全局状态
│  ├─ EngineAssets.cpp              engine/ 与项目资产路径
│  ├─ ProjectManager.cpp            项目清单和当前项目
│  ├─ SceneManager.cpp              场景切换和生命周期
│  ├─ SceneSerializer.cpp           序列化公共核心/预制体
│  ├─ SceneSerializer_3D.cpp        3D 组件序列化
│  ├─ SceneSerializer_2D.cpp        2D 组件序列化
│  ├─ InputController.cpp           底层输入
│  ├─ InputSystem.cpp               动作映射
│  ├─ PhysicsManager.cpp            Jolt 3D 物理
│  ├─ Physics2DManager.cpp          Box2D 世界管理
│  ├─ Physics2DSystem.cpp           ECS 与 Box2D 同步
│  ├─ AudioManager.cpp              miniaudio
│  ├─ Camera2DSystem.cpp            2D 相机
│  ├─ TilemapSystem.cpp             Tilemap 运行时
│  ├─ TmxLoader.cpp                 TMX 读取
│  ├─ SaveSystem.cpp                游戏保存数据
│  ├─ PaletteManager.cpp            调色板资源
│  ├─ VirtualJoystick.cpp           移动端虚拟摇杆
│  └─ Log.cpp                       日志系统
├─ ECS/
│  ├─ SceneECS.cpp                  场景实体/组件 API 与组件注册
│  ├─ ComponentRegistry.cpp         反射元数据、字段、序列化键
│  ├─ ScriptSystem.cpp              C++ ScriptBehaviour 生命周期
│  ├─ PhysicsSystem.cpp             3D 物理 ECS 系统
│  ├─ ECSStatics.cpp                ECS 静态实例/桥接
│  └─ Systems/
│     ├─ RenderSystem.cpp
│     ├─ WorldSystem.cpp
│     ├─ SpriteAnimatorSystem.cpp
│     └─ AudioSourceSystem.cpp
├─ Rendering/
│  ├─ SceneRenderer.cpp             场景渲染协调、2D/3D 模式
│  ├─ SceneCollector.cpp            ECS → 渲染列表
│  ├─ RendererBase.cpp              VulkanPipeline 等基础封装
│  ├─ RenderTarget.cpp              离屏目标
│  ├─ Camera.cpp
│  ├─ CameraUniformBuffer.cpp
│  ├─ ModelLoader.cpp               Assimp/glTF、动画导入
│  ├─ ModelRenderer.cpp             模型与蒙皮渲染
│  ├─ ModelBVH.cpp                  模型 BVH
│  ├─ TexturePool.cpp
│  ├─ TextureDescriptorCache.cpp
│  ├─ ShaderManager.cpp
│  ├─ ShaderHotReload.cpp
│  ├─ VulkanShader.cpp
│  ├─ PostProcessChain.cpp           后处理编排
│  ├─ PostProcessQuad.cpp
│  ├─ FullscreenQuad.cpp
│  ├─ CMAA2.cpp
│  ├─ ComputeShader.cpp
│  ├─ HiZComputeShader.cpp
│  ├─ OcclusionCulling.cpp
│  ├─ QuadTreeCulling.cpp
│  ├─ CascadeShadowRenderer.cpp
│  ├─ PointShadowRenderer.cpp
│  ├─ SkyboxRenderer.cpp
│  ├─ AtmosphereRenderer.cpp
│  ├─ AtmosphereLUT.cpp
│  ├─ Renderer2D.cpp
│  ├─ FontAtlas.cpp
│  ├─ TextRenderer.cpp
│  ├─ SceneDebugRenderer.cpp
│  ├─ WireframeRenderer.cpp
│  ├─ VoxLoader.cpp
│  ├─ VoxRenderer.cpp
│  ├─ VoxelMeshMultiDrawIndirect.cpp
│  ├─ VoxelTexture3DCache.cpp
│  ├─ VoxelTexture3DManager.cpp
│  ├─ DdsDecoder.cpp
├─ Editor/
│  ├─ EditorDllApi.cpp              Editor.dll 导出入口
│  ├─ EditorManager.cpp             编辑器生命周期
│  ├─ DockingLayout.cpp             Docking 布局
│  ├─ MainMenuBar.cpp
│  ├─ ToolbarWindow.cpp
│  ├─ HierarchyWindow.cpp
│  ├─ PropertiesWindow.cpp
│  ├─ ComponentInspector.cpp
│  ├─ AssetsWindow.cpp
│  ├─ SceneViewWindow.cpp
│  ├─ GameViewWindow.cpp
│  ├─ ProjectManagerWindow.cpp
│  ├─ MaterialEditor.cpp
│  ├─ MaterialEditorWindow.cpp
│  ├─ TilemapEditorWindow.cpp
│  ├─ ControlPanelWindow.cpp
│  ├─ UndoManager.cpp
│  ├─ EntityPresets.cpp
│  ├─ TextureCacheManager.cpp
│  ├─ PreviewGenerator.cpp
│  ├─ PreviewGeneratorHelper.cpp
│  ├─ PreviewModelRenderer.cpp
│  ├─ PreviewVoxRenderer.cpp
│  └─ MRTDebugWindow.cpp
├─ UI/
│  ├─ Canvas2D.cpp                  ECS UI 树渲染与交互
│  └─ TweenSystem.cpp               2D 补间
├─ World/
│  ├─ World.cpp                     无限体素世界
│  ├─ Chunk.cpp
│  ├─ BlockManager.cpp
│  └─ WorldRenderer.cpp
└─ Game/
   └─ GameManager.cpp               游戏插件发现、加载和热重载
```

### 5.2 `include/` 公共接口树

`include/` 与 `src/` 按目录镜像。游戏插件只能依赖 `include/`，所以移动或新增公共 API 时必须保持此边界。

```text
include/
├─ Platform/
│  └─ Export.h                      MIKAN_API 导出宏
├─ Game/
│  ├─ IGameModule.h                 游戏插件 ABI
│  └─ GameManager.h
├─ Core/
│  ├─ EngineConfig.h、EngineGlobal.h、EngineAssets.h
│  ├─ VulkanContext.h、VulkanManager.h、RenderGlobals.h
│  ├─ ProjectManager.h、SceneManager.h、SceneSerializer.h
│  ├─ InputController.h、InputSystem.h、InputGlobals.h
│  ├─ PhysicsManager.h、Physics2DManager.h、Physics2DSystem.h、PhysicsGlobals.h
│  ├─ AudioManager.h、SaveSystem.h、Log.h
│  ├─ Camera2DSystem.h、TilemapSystem.h、TmxLoader.h
│  └─ PaletteManager.h、VirtualJoystick.h
├─ ECS/
│  ├─ Types.h、ECS.h、Coordinator.h
│  ├─ EntityManager.h、ComponentManager.h、ComponentArray.h
│  ├─ System.h、SystemManager.h
│  ├─ Components.h                  所有组件定义，改动触发大范围重编
│  ├─ ComponentRegistry.h           反射描述
│  ├─ SceneECS.h                    场景公共 API
│  ├─ ScriptSystem.h、PhysicsSystem.h
│  └─ Systems/                      Render/World/SpriteAnimator/AudioSource
├─ Rendering/
│  ├─ SceneTypes.h、SceneCollector.h、SceneRenderer.h
│  ├─ RendererBase.h、RenderFrameContext.h、RenderTarget.h
│  ├─ Camera.h、CameraUniformBuffer.h
│  ├─ ModelLoader.h、ModelRenderer.h、ModelBVH.h
│  ├─ ShaderManager.h、ShaderHotReload.h、VulkanShader.h
│  ├─ DescriptorSetCache.h、TexturePool.h、TextureDescriptorCache.h
│  ├─ PostProcessChain.h、PostProcessQuad.h、FullscreenQuad.h、CMAA2.h
│  ├─ ComputeShader.h、HiZComputeShader.h、OcclusionCulling.h、QuadTreeCulling.h
│  ├─ CascadeShadowRenderer.h、PointShadowRenderer.h
│  ├─ SkyboxRenderer.h、AtmosphereRenderer.h、AtmosphereLUT.h
│  ├─ Renderer2D.h、FontAtlas.h、TextRenderer.h
│  ├─ VoxLoader.h、VoxRenderer.h、VoxelMeshMultiDrawIndirect.h
│  ├─ VoxelTexture3DCache.h、VoxelTexture3DManager.h
│  └─ AABB.h、JsonLite.h、DdsDecoder.h、SceneDebugRenderer.h、WireframeRenderer.h
├─ Editor/                          与 src/Editor 窗口一一对应的公共声明
├─ UI/
│  ├─ Canvas2D.h
│  └─ TweenSystem.h
└─ World/
   ├─ World.h、WorldTypes.h、WorldGlobals.h
   ├─ Chunk.h、BlockManager.h
   └─ WorldRenderer.h
```

### 5.3 `games/` 游戏插件

```text
games/
├─ snake/
│  ├─ SnakeGame.h
│  ├─ SnakeGame.cpp
│  └─ GameSnakeExports.cpp
├─ breakout/
│  ├─ BreakoutGame.h
│  └─ BreakoutGame.cpp
├─ contact2d/
│  ├─ GameContact2D.h
│  └─ GameContact2D.cpp
├─ phys2d/
│  ├─ GamePhys2D.h
│  └─ GamePhys2D.cpp
└─ cesiumwalk/
   └─ CesiumWalk.cpp
```

README/AGENTS 中提到的 `baka3d`、`voxelproto` 等目录可能随快照变化；以 `games/` 当前实际内容和 `projects.json` 为准。玩法插件通过 `tools/compile_games.ps1` 独立编译。

### 5.4 `tools/` 工具链

```text
tools/
├─ build.ps1                标准构建入口（自动配置/clean-first/产物校验）
├─ compile_games.ps1        游戏插件构建/热重载输出
├─ compile_shaders.ps1      Shader 编译
├─ compile_shaders.bat
├─ validate_scene.ps1       场景 Schema 与资产引用校验
├─ scene_schema.json        组件和字段白名单
├─ publish.ps1              发布包生成
├─ ktx2_convert.ps1         KTX2 转换
├─ mcp_server.ps1           分离 run_gameplay_test/run_render_test，并提供断言、构建/校验
└─ TestPlugin.cpp           插件测试源
```

### 5.5 `engine/` 与 `assets/`

```text
engine/
├─ shaders/glsl/            Shader 源码：20 compute、60 fragment、15 vertex（快照值）
├─ shaders/spv/             编译产物，默认不读
├─ fonts/                   引擎字体
├─ textures/                默认纹理和天空资源
└─ postprocess_chain.json   后处理链配置

assets/
├─ *.json                   测试/旧式项目场景
├─ audio/、maps/、tilesets/ 项目内容
└─ models/                  大型模型样本，默认不扫描
```

常用验证场景：

- `assets/baka3d.json`：完整 3D、材质和脚本样例。
- `assets/cesium_man.json`、`cesiumwalk.json`：骨骼动画。
- `assets/phys2d.json`、`contact2d.json`：2D 物理。
- `assets/snake.json`、`breakout.json`：2D 游戏插件。
- `assets/voxel_world.json`：体素世界。
- `assets/sponza_demo.json`：较大 3D 场景。

## 6. 关键任务到文件的路由

| 任务 | 首先读取 | 常见联动 |
|---|---|---|
| 主循环/启动/命令行 | `src/Core/EngineMain.cpp` | `src/HostMain.cpp`、`ProjectManager.cpp` |
| Vulkan 初始化/帧同步 | `src/Core/VulkanManager.cpp` | `include/Core/VulkanContext.h`、`RenderFrameContext.h` |
| 场景没有画面 | `SceneRenderer.cpp` | `SceneCollector.cpp`、相应 Renderer、场景 JSON |
| 新增组件 | `include/ECS/Components.h` | `SceneECS.cpp`、`ComponentRegistry.cpp`；特殊序列化才改 Serializer |
| 场景加载/保存 | `SceneSerializer.cpp` | `_2D.cpp`、`_3D.cpp`、`scene_schema.json` |
| 属性面板 | `PropertiesWindow.cpp` | `ComponentInspector.cpp`、`ComponentRegistry.cpp` |
| 实体层级 | `HierarchyWindow.cpp` | `SceneECS.cpp`、Serializer |
| 游戏插件 | `IGameModule.h`、`GameManager.cpp` | `games/<name>/`、`compile_games.ps1` |
| ScriptBehaviour | `ScriptSystem.cpp` | `ComponentRegistry`、对应游戏插件 |
| 2D UI | `Canvas2D.cpp` | `Renderer2D.cpp`、`Components.h` |
| 2D 物理 | `Physics2DManager.cpp`、`Physics2DSystem.cpp` | Box2D 组件、phys2d 场景 |
| 3D 物理 | `PhysicsManager.cpp`、`ECS/PhysicsSystem.cpp` | Jolt 组件 |
| 模型/材质 | `ModelLoader.cpp`、`ModelRenderer.cpp` | `TexturePool`、`ModelBVH`、Shader |
| 骨骼动画 | `ModelLoader.cpp`、`ModelRenderer.cpp` | `AnimatorComponent`、动画测试场景 |
| 后处理 | `PostProcessChain.cpp` | `FullscreenQuad.cpp`、`engine/postprocess_chain.json`、GLSL |
| Shader 热更新 | `ShaderHotReload.cpp` | `RendererBase.cpp`、compile_shaders |
| 阴影 | `CascadeShadowRenderer.cpp` / `PointShadowRenderer.cpp` | model/world shader |
| 体素渲染 | `VoxRenderer.cpp`、`VoxelMeshMultiDrawIndirect.cpp` | GPU culling、texture 3D cache |
| 无限体素世界 | `World.cpp`、`Chunk.cpp` | `WorldRenderer.cpp`、`WorldSystem.cpp` |
| 编辑器视图串扰 | `SceneViewWindow.cpp`、`GameViewWindow.cpp` | `Renderer2D`、`VulkanManager`、RenderTarget |
| 项目/发布 | `ProjectManager.cpp` | `projects.json`、`publish.ps1` |
| Android | `android/app/src/main/cpp/CMakeLists.txt` | Activity、主 CMake、资产复制 |

## 7. 核心数据约定

- 场景实体基础数据：`id / name / transform / hierarchy`，其余键为组件。
- `rotation` 四元数顺序为 `[w,x,y,z]`。
- 3D 可见模型通常需要 `mesh + render + material`。
- 未知组件可能被静默忽略，写场景后必须运行 `validate_scene.ps1`。
- 组件注册真相源是 `ComponentRegistry`；`scene_schema.json` 是导出的校验产物。
- 公共组件定义位于 `include/ECS/Components.h`。
- UI 属于全局 ECS 场景树，不恢复旧式独立 Node 树。
- 引擎资产走 `engine/`，游戏/项目资产走 `assets/` 或项目根。
- 游戏插件 ABI 通过 `include/Game/IGameModule.h` 和 `MIKAN_API` 暴露。

## 8. 构建与验证速查

```powershell
# 引擎/编辑器
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\build.ps1" -Target EngineMain

# CPU-only 玩法测试宿主
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\build.ps1" -Target MikanTestRunner

# 引擎正在运行时允许关闭
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\build.ps1" -Target EngineMain -KillEngine

# 游戏插件
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\compile_games.ps1"

# 场景校验
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\validate_scene.ps1" "D:\Engine project\vulkan engine\assets\baka3d.json" -CheckAssets
```

构建产物默认位于 `out/build/x64-Release/`：

```text
EngineMain.exe
MikanTestRunner.exe
Game.dll
Editor.dll
Game<name>.dll
```

完整参数、headless 和 MCP 用法以 `编译命令.md`、`AGENTS.md` 为准。

## 9. 修改前必须知道

- **🚫 备份纪律（2026-08-26 起）**：源码树内禁止创建/遗留任何 `*.bak*` 文件——备份只允许两条路：① git（改前 `git status` 看基线，大改动先提交 checkpoint 或 `git stash`）；② 外部目录 `D:\Engine project\backup\`（带日期子目录）。发现源码树里的 `.bak*` 顺手移到外部目录，不要留在原地（2026-08-26 一次清理移走 116 个，含 `.bak-codex-*` / `.stripz.bak` / `*_timestamp.bak` 变体）。不要用 `Copy-Item <file> <file>.bak` 原地备份写法。
- 修改或覆盖现有源码前先确认有备份（git 基线或外部目录），验证后再清理。
- 中文源文件必须显式以 UTF-8 读取和写入；禁止隐式 PowerShell 编码转换。
- `tools/*.ps1` 要保留 UTF-8 BOM，以兼容 Windows PowerShell 5.1。
- 引擎运行时会锁定 DLL，构建前停止引擎或使用 `-KillEngine`。
- 改公共头文件会触发大范围重编，属于正常现象。
- 改 Shader 接口布局不能依赖现有热更新，必须完整重建相关 layout/pipeline。
- 不要在当前源码搜索中混入 `backup/`；只有恢复或历史对比时才指定具体备份目录。
- 工作区中的其他引擎项目是参考项目，不属于本工程构建。

## 10. 文档可信度顺序

发生冲突时按以下顺序判断：

1. 当前源码和 CMake。
2. `AGENTS.md` 中带日期的硬性约定。
3. 本文的代码地图。
4. `README.md`。
5. 历史开发计划和备份目录。

本文是导航文档，不替代源码。若任务要求确认当前行为，只读取对应入口和直接依赖，不要再次扫描整个仓库。
