# MikanEngine — 自研 Vulkan 游戏引擎

> 位置：`D:\Engine project\vulkan engine`（路径含空格，命令中须引号）
> 技术栈：C++17 + Vulkan + SDL3 + ImGui（docking）+ Jolt + Box2D + msdfgen + miniaudio
> 构建：CMake + Ninja + MSVC（Release），产物在 `out\build\x64-Release\`
> 支持 Android（arm64，同一 CMakeLists）

---

## 1. 项目大纲

### 架构（三层 DLL 结构）

| 模块 | 说明 |
|---|---|
| `Game.dll` | 运行时：渲染 / 物理(3D Jolt + 2D Box2D) / ECS / 场景序列化 / 2D 核心 / 音频 |
| `Editor.dll` | 编辑器：ImGui 窗口、资产浏览器、预览、undo（可选加载，存在即编辑器模式） |
| `EngineMain.exe` | 薄宿主：检测到 Editor.dll 进入编辑器模式；`--no-editor` 强制纯游戏模式 |
| `games/<name>/` | 游戏插件 DLL：独立编译（cl + Game.lib），F5 热重载，**不碰 Game.dll**（snake/breakout/contact2d/phys2d 全部玩法均插件化，`src/Game/` 只剩 GameManager；**3D 边界已验证**：`baka3d` 插件驱动天空盒场景中的旋转模型，见 `assets/baka3d.json`） |

### 关键目录

- `src/` + `include/` 引擎源码，**镜像分类**：`Core/`（引擎核心）、`Rendering/`（渲染/几何/体素）、`ECS/`（组件系统）、`Editor/`（编辑器窗口/预览）、`UI/`（2D 场景图）、`World/`（体素世界）、`Game/`（内置示例游戏）、`Platform/`（导出宏 MIKAN_API）
  - `.cpp` 在 `src/` 对应目录，公共头在 `include/` 对应目录（**games/ 插件只可见 include/**，是 DLL API 边界）；src/ 根只留 `HostMain.cpp`（入口）+ `StbVorbis.c`
- **玩法头随插件（2026-08）**：各游戏自己的头文件在 `games/<name>/` 目录（与 .cpp 同目录，`#include "SnakeGame.h"` 引号形式直查源目录）；`include/Game/` 只留引擎接口 `GameManager.h`（游戏管理器）与 `IGameModule.h`（插件接口）——开发玩法不碰引擎头，引擎也不依赖玩法头
  - 关键文件：`src/Core/EngineMain.cpp`（主循环/命令行参数）、`src/Core/SceneSerializer.cpp`（79KB 手写 JSON 序列化）、`include/ECS/Components.h`（组件定义）、`include/ECS/SceneECS.h`（场景 API）
- `dependencies/` 第三方（imgui / JoltPhysics / box2d / glm / SDL3 / assimp / glslang 等）
- **职责分离（2026-08）**：`engine/` = 引擎系统资产（shaders/glsl→spv、fonts/、textures/ 引擎纹理+skybox、postprocess_chain.json），`assets/` = 游戏/项目内容（场景 JSON、models/、audio/、maps/、tilesets/ 等）。引擎根探测以 `engine/shaders/spv` 为准（`ProjectManager::DetectEngineRoot`）；引擎资产走 `GetEngineAssetPath`（→ `engine/`），游戏资源走 `ResolveAssetPath`（→ `assets/`）
- **项目化（2026-08，从 baka3d 起步）**：`projects/<项目>/`（引擎根顶层，与 assets/ engine/ 平级）是自包含项目目录——`project.json`（项目清单：name / scene / game / assets[] 资源清单，新增资源须加入清单）、`scenes/`（场景文件=项目工作目录配置，资源引用相对项目根）、`models/ textures/` 等资源、`games/`（玩法源码，compile_games.ps1 支持项目化插件编译）。打开项目（项目管理器 / `--project <dir>`）→ 读清单 → 校验 assets[] → 加载清单 scene → 激活 game；资源区 = 项目目录本身（Unity 式）。总项目清单 = 引擎根 `projects.json`（记录项目路径显示于项目列表）。旧式项目（目录含 `assets/`）完全兼容
- `tools/` 构建/校验/MCP 脚本（见 §2、§3）
- `out/build/x64-Release/` 产物：Game.dll / Editor.dll / EngineMain.exe / Game.pdb

### 场景 JSON 格式

实体含基础键 `id / name / transform / hierarchy` + 组件键（如 `sprite2d`、`camera2d`、`rigidbody2d`）。
组件键与字段白名单见 `tools/scene_schema.json`（由引擎 `--dump-schema` 生成，改组件后重新生成）。
⚠️ 引擎对**未知组件键静默忽略**（不报错）——校验器 `validate_scene` 能发现（历史教训：场景里的 `active` 死键已清理）。

**3D 场景要点**（完整样例：`assets/baka3d.json`）：
- `rotation` 四元数顺序 **[w,x,y,z]**，identity = `[1,0,0,0]`（`[0,0,0,1]` 是绕 Z 转 180°，相机倒转）
- 模型实体必须带 **`material` 组件**（`albedoPath` + `useAlbedoTexture:true`），否则渲染 fallback 无纹理
- 组件遗漏引擎不报错——权威格式对照：引擎 Ctrl+S 保存的 `out\build\x64-Release\auto_save.json`

**音频源（类似 Unity AudioSource）**：实体挂 `audioSource` 组件即自动播放——`{"clip": "bgm.wav", "volume": 0.5, "loop": true, "playOnAwake": true}`（clip 为 `assets/audio/` 下文件名）；插件运行时通过 `playRequested`/`stopRequested` 触发播放/停止、改 `volume`/`loop` 实时生效，由 `AudioSourceSystem` 每帧驱动。

### 代码规模（2026-08 统计，C/C++ 行数）

| 类别 | 文件数 | 行数 | 占比 |
|---|---|---|---|
| **引擎自有 C/C++**（src+include+games+tools） | 209 | **≈52,500** | **5%** |
| **第三方 dependencies**（Jolt/SDL3/imgui/box2d/assimp/glm…） | 1,209 | ≈1,004,800 | **95%** |
| GLSL shader（engine/shaders/glsl） | 98 | — | — |
| Android（java/kotlin/cpp） | 29 | 3,638 | — |
| PowerShell 工具脚本（tools/*.ps1） | 6 | ≈850 | — |

引擎自有代码分布（C/C++）：
- `src/Rendering` 20,926 行（**占自有 40%，最大**——VoxRenderer 97KB / VoxelMeshMultiDrawIndirect 164KB 单文件）
- `include/` 9,435（声明面）· `src/Core` 7,921 · `src/Editor` 7,472 · `src/World` 3,009 · `src/ECS` 1,411
- 第三方大头：SDL3 82K / imgui 78K / box2d 72K / Jolt 96K / assimp 43K / glm 36K

> 巨型文件已拆分：`SceneSerializer`（原 79KB/1995 行）已拆为 `SceneSerializer.cpp`（核心/工具，1104 行）+ `SceneSerializer_3D.cpp`（8 个 3D 组件）+ `SceneSerializer_2D.cpp`（6 个 2D 组件），改组件序列化只读对应文件。

> 含义：引擎自有仅 5%，AI 日常改动实际接触面是 src/Rendering + src/Core + src/Editor 约 3.6 万行；
> 第三方代码无需阅读（只经 include 接口使用）。

### 关键文档

| 文件 | 内容 |
|---|---|
| `编译命令.md` | 构建/运行/测试/MCP 命令速查 + 完整陷阱表 |
| `工业化开发计划.md` | Phase 0-6 路线图（git/日志/测试/资产/并行渲染/版本化/热重载） |
| `2D玩法原型补齐计划.md` | 2D 功能清单（多数已完成） |

---

## 2. 编译与测试（AI 推荐路径）

### 一键构建 `tools/build.ps1`

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\build.ps1" [-Target EngineMain|Editor|Game|CompileShaders] [-KillEngine]
```

- 自动：vswhere 探测 VsDevCmd、文件锁检测、日志写 `out\build\build.log`、错误摘要（前 20 条 `error C/LNK/FAILED`）
- 退出码：`0` 成功 / `1` 构建失败 / `2` 引擎在运行（加 `-KillEngine`）/ `3` 环境错误
- 默认 target `EngineMain` **连带构建 Editor.dll + Game.dll + Shaders**（ninja 依赖）

### headless 测试（可断言）

```powershell
& "D:\Engine project\vulkan engine\out\build\x64-Release\EngineMain.exe" --headless --frames 60 --dump-state dump.json --no-voxel-world --no-project-manager --scene assets/contact2d.json --game contact2d
```

- `--headless` 隐藏窗口、自动退出；`--dump-state` 导出实体 `id/name/pos/wpos/rot_deg/scale/visible` JSON
- 退出码：`0` 正常跑完 / `2` 场景加载失败（**不 fallback 默认场景**，避免误判）
- 崩溃写 exe 旁 `crash_log.txt`（异常码/地址/模块偏移，可符号化）
- ⚠️ `--scene` 必须配 `--no-project-manager` 或 `--project` 才会加载场景

### 场景离线校验 `tools/validate_scene.ps1`

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\validate_scene.ps1" <scene.json> [-CheckAssets]
```

- 检查：JSON 可解析、id 唯一、transform 结构、parent 引用、组件键/字段白名单（schema）、资源存在（`-CheckAssets`）
- 退出码：`0` 通过 / `1` 有错

### schema 重新生成（改组件字段后）

```powershell
& "D:\Engine project\vulkan engine\out\build\x64-Release\EngineMain.exe" --dump-schema "D:\Engine project\vulkan engine\tools\scene_schema.json"
```

### 引擎全部命令行参数

`--project <dir>` / `--no-project-manager` / `--scene <path>` / `--game <name>` / `--no-voxel-world` / `--no-editor` / `--headless` / `--frames N` / `--dump-state <path>` / `--dump-schema <path>` / `--phys2d-selftest`（内建 2D 物理自测）
- **纯游戏模式（`--no-editor`）与 `--headless` 自动跳过项目管理器启动页**：编辑器未加载时启动页（`g_ProjectSelectionPending`）无人渲染，等待选择会导致游戏永不运行——引擎自动视为默认运行（加载 `--scene` 指定场景或默认场景，场景顶层 `game` 键/`--game` 激活游戏模块），无需再显式传 `--no-project-manager`

---

## 3. MCP 调用（AI 客户端接入）

**配置**（`.mcp.json` 或客户端设置）：

```json
{
  "mcpServers": {
    "mikanengine": {
      "command": "powershell",
      "args": ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "D:\\Engine project\\vulkan engine\\tools\\mcp_server.ps1"]
    }
  }
}
```

**工具**：

| 工具 | 作用 | 关键参数 |
|---|---|---|
| `build` | 一键构建 | `target`(默认 EngineMain), `killEngine` |
| `validate_scene` | 离线校验场景 | `scenePath`, `checkAssets` |
| `run_test` | headless 运行 + 导出状态 | `scene`(必填), `game`, `frames`(默认120), `extraArgs`, `timeoutMs`(默认60000) |
| `read_dump` | 读 run_test 的 dump JSON | `path` |
| `engine_status` | 进程/崩溃/exe 状态 | — |
| `stop_engine` | 杀引擎释放文件锁 | — |

**AI 标准闭环**：`validate_scene`（写场景后立即纠错）→ `build`（改代码后）→ `run_test`（headless 跑 N 帧）→ `read_dump`（断言实体 `wpos`，如"Box 60 帧后 y 从 360 降到 263"）。
`run_test` 自动附加 `--no-project-manager`；引擎已在运行时会拒绝执行（先 `stop_engine` 或 `build(killEngine=true)`）。

---

## 4. 陷阱要点

### 构建/运行
- **文件锁 LNK1104**：EngineMain 运行中锁 Game.dll/Editor.dll → `build.ps1` 默认拒绝（exit 2），加 `-KillEngine`
- **改 Editor 代码**：构建 EngineMain 已连带 Editor.dll，能生效；无需单独 `--target Editor`（除非只想重编 Editor）
- **空 out 目录**：`cmake --build` 报 `could not load cache` → 先 `cmake -S <root> -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl` 配置
- **改头文件**（如 `ECS/Components.h`）→ 触发大范围重编，属正常
- **改 CMakeLists.txt** → ninja 重新生成，可能触发大量重编/全量重链接
- **增量构建特性**：改单个 src 文件只重编该 obj + 链接 Game.dll（~2.9s），端到端 ~12s；第三方依赖 obj（占 80%）从不重编，**拆 dll 收益低，不必做**

### 引擎/场景
- `--scene` 必须配 `--no-project-manager` 或 `--project`，否则场景不加载（跑项目管理器页）
- 场景未知组件键被**静默忽略**——写场景后用 `validate_scene` 校验
- headless 下场景加载失败 = 退出码 2（不 fallback）
- 组件/字段白名单以 `scene_schema.json` 为准（改 `ComponentRegistry.cpp` 反射表或序列化后需重新 `--dump-schema`）

### PowerShell 脚本（本仓库 `.ps1` 的坑）
- **必须 UTF-8 BOM**：PS 5.1 按 ANSI(GBK) 读无 BOM 文件，含中文的脚本会解析错乱（报 `&&` 无效/字符串未终止）——写/改 .ps1 后加 BOM
- 哈希表键 `default` 是关键字须加引号；键值对用 `;` 分隔（逗号会被当数组）
- `Start-Process -PassThru` 重定向输出时 `ExitCode` 恒为空（PS 5.1 缺陷）→ 用 .NET `ProcessStartInfo` + 异步 `ReadToEndAsync`
- `[Console]::InputEncoding/OutputEncoding` 须显式 UTF-8，且剥首行 BOM（MCP server 已处理）

### 其他
- 崩溃排查：exe 旁 `crash_log.txt`；编译错误看 `build.log` 的 `FAILED/error C/LNK`
- 引擎日志多为 printf/stderr，无分级（Phase 0.2 计划引入 Log 系统）
- 项目尚未 git 化（工业化计划 Phase 0.1 待做）；`out/`、`android/.gradle`、`*.bak` 等应进 `.gitignore`
