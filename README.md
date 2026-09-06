# MikanEngine — 自研 Vulkan 游戏引擎

> 位置：`D:\Engine project\vulkan engine`（路径含空格，命令中须引号）
> 技术栈：C++17 + Vulkan + SDL3 + ImGui（docking）+ Jolt + Box2D + msdfgen + miniaudio
> 构建：CMake + Ninja + MSVC（Release），产物在 `out\build\x64-Release\`
> 支持 Android（arm64，同一 CMakeLists）

---

## 1. 项目大纲

### 架构（共享运行时 + 分层宿主）

| 模块 | 说明 |
|---|---|
| `Game.dll` | 运行时：渲染 / 物理(3D Jolt + 2D Box2D) / ECS / 场景序列化 / 2D 核心 / 音频 |
| `Editor.dll` | 编辑器：ImGui 窗口、资产浏览器、预览、undo（可选加载，存在即编辑器模式） |
| `EngineMain.exe` | 完整宿主：SDL Video + Vulkan + 可选 Editor，负责渲染层回归 |
| `MikanTestRunner.exe` | 玩法测试宿主：复用 Game.dll 的 ECS/场景/插件/脚本/物理，但不创建窗口、不初始化 SDL Video/Vulkan |
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
- `out/build/x64-Release/` 产物：Game.dll / Editor.dll / EngineMain.exe / MikanTestRunner.exe / Game.pdb

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
| `docs/工业化开发计划.md` | Phase 0-6 路线图（git/日志/测试/资产/并行渲染/版本化/热重载） |
| `docs/开发路线图_2026H2.md` | 当前引擎与玩法开发路线图 |
| `docs/待办问题.md` | 当前待办与已知问题 |
| `docs/简单3D原型构造快速步骤.md` | 简单 3D 原型的场景、组件、运行和验收步骤 |
| `docs/AI_Native_GameSpec.md` | 结构化 GameSpec：素材、场景、玩法脚本、测试门禁和交付包 |
| `docs/AI_Native_AgentTesting.md` | 外部 Agent 驱动的只测试 TestSpec、Evidence 和 Evaluation 闭环 |
| `tools/agent-cli/README.md` | Node/TypeScript 模型 Planner CLI：Discovery、模型响应、GameSpec 和 MCP 执行 |

---

## 2. 编译与测试（AI 推荐路径）

### 一键构建 `tools/build.ps1`

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\build.ps1" [-Target EngineMain|MikanTestRunner|Editor|Game|CompileShaders] [-KillEngine] [-CleanFirst] [-ConfigureIfMissing]
```

- 自动：vswhere 探测 VsDevCmd、文件锁检测、日志写 `out\build\build.log`、错误摘要和预期产物校验；`-ConfigureIfMissing` 可恢复空 `out`，`-CleanFirst` 可排除陈旧中间产物
- 退出码：`0` 成功 / `1` 构建失败 / `2` 引擎在运行（加 `-KillEngine`）/ `3` 环境错误
- 默认 target `EngineMain` **连带构建 Editor.dll + Game.dll + Shaders**（ninja 依赖）

### 分层自动测试（可断言）

```powershell
$root = "D:\Engine project\vulkan engine"

# 玩法层：无窗口、无 SDL Video/Vulkan 初始化
& "$root\out\build\x64-Release\MikanTestRunner.exe" --frames 60 --fixed-dt 0.016666667 --dump-state "$root\out\gameplay-state.json" --scene "$root\assets\contact2d.json" --game contact2d

# 地形玩法输入：等待落地后自动跳跃，再移动并验证碰撞/输入闭环
& "$root\out\build\x64-Release\MikanTestRunner.exe" --project "$root" --scene "$root\assets\terrain_render_prototype.json" --frames 600 --fixed-dt 0.016666667 --scripted-input --dump-state "$root\out\terrain-gameplay-state.json"

# 渲染层：完整 SDL Video/Vulkan/FrameRender/Present
& "$root\out\build\x64-Release\EngineMain.exe" --headless --frames 1 --fixed-dt 0.016666667 --dump-state "$root\out\render-state.json" --no-project-manager --scene "$root\assets\contact2d.json" --game contact2d
```

- 两层共用固定步进和状态 dump，`runtime_layer` 分别为 `gameplay-cpu` / `render-vulkan`
- `MikanTestRunner` 覆盖场景、玩法插件、脚本、3D/2D 物理、Camera2D/Tween/SpriteAnimator；不验证 shader、GPU 资源、交换链和 Present
- `EngineMain --headless` 覆盖完整渲染路径；旧 `--headless-no-render` 仅跳过逐帧渲染，仍初始化 Vulkan
- 退出码：`0` 正常跑完 / `2` 初始化、场景或玩法插件加载失败 / `3` dump 写入失败
- 默认崩溃写工作目录 `log/crash_log.txt`；自动化可用 `--crash-log <path>` 指定本次运行专属日志
- ⚠️ `--scene` 必须配 `--no-project-manager` 或 `--project` 才会加载场景

### 统一回归入口 `tools/test.ps1`

将场景校验、玩法测试、渲染测试和结果归档收敛到同一条命令，测试清单位于 `tools/test_cases.json`：

```powershell
$root = "D:\Engine project\vulkan engine"

# 默认构建所需目标并运行全部测试层
powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\test.ps1" -Layer all

# 只跑某一层或某几个场景；已有构建产物时可跳过构建
powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\test.ps1" -Layer gameplay -Case contact2d,phys2d -SkipBuild
powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\test.ps1" -Layer render -Case cesiumwalk -SkipBuild
```

- `-Layer` 支持 `validate`、`gameplay`、`render`、`all`；`-Case` 支持按清单名称筛选
- 每次运行写入独立的 `out\test_runs\<run-id>\`，包含 `metadata.json`、`result.json`、步骤日志、状态 dump 和崩溃日志
- `result.json` 是后续 Agent 读取的机器可读入口，记录每一步的状态、退出码、耗时和产物路径
- 退出码：`0` 全部通过 / `1` 至少一步失败 / `3` 测试环境或清单错误

### SceneCommand 结构化场景编辑

scene_command.ps1 为 Agent 提供受约束的场景编辑入口，命令文件格式见 tools\scene_command.example.json。支持创建、删除、重命名实体，修改变换和组件，维护父子层级，以及设置场景 game 属性。

    powershell -NoProfile -ExecutionPolicy Bypass -File tools\scene_command.ps1 -ScenePath assets\contact2d.json -CommandsPath tools\scene_command.example.json -CheckAssets -TestLayer gameplay -SkipTestBuild

默认写入 out\scene_commands\<run-id>\scene.generated.json，不覆盖源场景。使用 -OutputPath 可指定副本路径；只有显式使用 -InPlace 才会覆盖源场景，覆盖前会保存 source.before.json。命令执行顺序、别名映射、候选场景、校验结果和回归结果都记录在同一运行目录的 result.json 中。

MCP 客户端可直接调用 apply_scene_commands 并传入 scene、commands、testLayer 等参数；返回内容包含文本摘要和 structuredContent，适合 Agent 继续读取结果或进入断言阶段。

### Agent 玩法脚本 SDK

`ECS::ScriptContext`（`include/ECS/ScriptContext.h`）为 Agent 生成的 C++ 玩法脚本提供稳定入口：实体查询、层级、变换、可见性、运行时创建/延迟销毁、脚本挂载、组件反射字段和语义输入都不需要直接拼接底层单例。组件字段使用场景协议的 `serializeKey`，例如 `render`、`camera`、`rigidBody`。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\script_scaffold.ps1 `
  -ScriptName FollowTargetScript `
  -OutputPath games\cesiumwalk\FollowTargetScript.cpp `
  -FieldsJson '[{"name":"followSpeed","type":"float","label":"跟随速度","default":4.0}]'
```

脚本生成器和 MCP `create_script` 只允许写入游戏插件目录，校验 `IScriptBehaviour`/`REGISTER_SCRIPT`、include 白名单和危险调用；覆盖已有脚本时先在 `out\script_backups\` 生成并校验备份。返回的脚本路径、SHA-256 和编译状态可被 Agent 继续传给场景挂载、玩法测试和状态断言。详细接口见 `tools\script_sdk.json` 与 `docs\AI_Native_ScriptSDK.md`。

### Agent Workflow 编排

`tools\agent_workflow.ps1` 将多个受控工具串成一个声明式 workflow。支持 `create_script`、`build`、`compile_games`、`validate_scene`、`apply_scene_commands`、`run_gameplay_test`、`run_render_test`、`capture_frame`、`capture_performance`、`read_dump` 和 `assert_state` 等步骤；步骤可以声明依赖，并用 `${steps.<id>.result.<field>}` 把前一步产物传给后一步。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent_workflow.ps1 `
  -WorkflowPath tools\agent_workflow.example.json
```

每次执行写入独立的 `out\agent_runs\<run-id>\`，保存 workflow 输入、每步 input/stdout/stderr、状态 dump、`result.json` 和哈希 manifest。默认遇到失败停止；`-DryRun` 只生成计划；脚本覆盖和场景原地修改必须在 workflow 中显式打开 `allowDestructive`。MCP 对应入口是 `run_agent_workflow`。接口和边界说明见 `docs\AI_Native_AgentWorkflow.md`。这提供的是 Agent 的工具编排层，不宣称已经替代模型完成自主规划。

### Agent Task 修复闭环

`tools\agent_task.ps1` 在 workflow 之上增加任务级失败诊断和候选修复重跑：先执行基础 workflow，读取失败步骤的 `failureCategory`、`diagnostics` 和 `nextAction`，再从候选列表中选择匹配项，以受限的 `args.<field>` 补丁生成新 workflow 并重跑。默认 `preview`，显式 `-Mode execute` 才会执行；每次尝试保存在独立的 `out\agent_tasks\<run-id>\`，并返回任务级 `result.json`/manifest。候选不能修改断言期望值、步骤依赖、任意 PowerShell 或高风险覆盖开关。示例和边界见 `tools\agent_task.example.json`、`tools\agent_task.schema.json` 与 `docs\AI_Native_AgentRepairLoop.md`。

### Agent Planner / Evidence

`tools\agent_plan.ps1` 是模型可调用的高层入口：接收目标、受约束 task 和可选 evaluation contract，串联 `agent_task` 执行、`agent_evidence` 取证，并在 `execute` 模式下自动调用 `agent_evaluate` 判定交付门槛。证据包含 discovery、失败步骤、编译/运行日志摘要、运行层、构建产物哈希、Git 版本、Android `adb` 状态、截图/MRT 和 Nsight 性能指标；不可用能力会标记为 `unavailable`，不会伪造真机、视觉或性能结论。默认 `preview`，显式 `-Mode execute` 才运行任务。接口见 `tools\agent_plan.schema.json`、`tools\agent_evaluation.schema.json` 和 `docs\AI_Native_AgentPlannerEvidence.md`。

### AI Native GameSpec

`tools\agent_game_spec.ps1` 将模型输出的结构化 GameSpec 编译为受控 plan/workflow，统一串联素材前置检查、场景命令、Gameplay Script、构建、CPU-only 玩法测试、状态断言、渲染/RenderDoc/Nsight 验收和交付打包。默认 `preview`，显式 `-Mode execute` 才启动引擎或写入脚本；成功执行会在 `out\agent_game_specs\<run-id>\` 生成 `delivery.zip`。MCP 对应工具为 `run_agent_game_spec`。示例：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent_game_spec.ps1 `
  -GameSpecPath tools\agent_game_spec.example.json `
  -Mode preview
```

GameSpec 是模型和引擎之间的可审查边界，不代表一句提示词可以稳定生成任意完整游戏；模型应通过结构化 `scene.commands`、`scripts`、`tests` 和 `delivery` 生成目标，具体安全边界由现有 SceneCommand、Script SDK、Evidence 和 Evaluation 负责。

### AI Native Model Planner / CLI

`tools\agent-cli\src\cli.ts` 把外部模型客户端和引擎执行器接起来：先调用 MCP 的 Discovery 工具，把项目事实、场景摘要、资产查询结果和 GameSpec Schema 组合成 `model.prompt.md`；再读取模型 stdout/响应文件，提取并检查 JSON，最后交给 `run_agent_game_spec`。支持 `mock`（回归）、`file`（手工保存的 Codex/Claude/Cursor 响应）和 `command`（用户配置的模型 wrapper）三种 provider，不把任何厂商 API 密钥放进引擎。

```powershell
node --experimental-strip-types tools/agent-cli/src/cli.ts preview `
  --scene assets/contact2d.json `
  --goal "为 Contact2D 增加一个可验证的移动目标" `
  --provider mock `
  --json
```

真实构建和测试必须显式执行并确认：

```powershell
node --experimental-strip-types tools/agent-cli/src/cli.ts execute `
  --scene assets/contact2d.json `
  --goal "为 Contact2D 增加一个可验证的移动目标" `
  --provider command `
  --model-command "C:\path\to\model-wrapper.exe" `
  --yes
```

每次运行产物位于 `out\agent_cli\<run-id>\`，包含 `discovery.json`、`model.prompt.md`、`model.response.txt`、`gamespec.generated.json` 和 `result.json`。详细 provider 约定见 `tools\agent-cli\README.md`。

CLI 也支持失败反馈与受限自动修复：在 `execute --yes` 后追加 `--auto-repair --max-attempts 3`，外部模型会读取失败 GameSpec 结果、失败步骤和历史尝试，输出 `GameSpecRepair`，通过本地校验后再执行下一轮。修复器只能替换受控 `scene.commands`、修改当前 GameSpec 已声明脚本源码，或调整有限的测试/采集超时；项目/场景/资产边界、断言期望、交付策略和权限开关不可修改。每一轮保存在 `out\agent_cli\<run-id>\attempt-XX\`，根目录的 `autofix.progress.json` 和 `result.json` 记录完整闭环；脚本补丁执行前会生成带 SHA-256 manifest 的外部备份。若发生过修复，`result.finalSpecPath` 指向最终执行规格，`result.gameSpecResultPath` 指向最后一轮引擎结果。模型可以由 Codex、Claude Code、Cursor 或其他 wrapper 提供，密钥不进入引擎。

### AI Native Agent TestSpec

如果外部 Agent 当前只需要验证已有场景，不需要生成场景、脚本或模型，可以调用 `run_agent_test`。它接收 `tools\agent_test.schema.json` 定义的窄 TestSpec，只编排 Discovery、场景校验、构建、玩法/渲染测试、状态断言、RenderDoc/Nsight 证据和 Evaluation，不接收密钥、任意命令或源文件覆盖。玩法测试可通过 `tests.gameplay.replayPath` 接入项目内的结构化输入回放，复用同一条确定性测试时间线。

项目化测试在 `project` 中提供 `projectPath` 后，`scenePath` 可以使用项目相对路径；TestSpec 会先读取 `project.json`、资源根、默认场景和语义资产索引，再把项目根传给运行时的 `--project`，避免 Agent 猜测资源路径。
```powershell
# 先只生成并审查测试计划，不启动引擎
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent_test.ps1 `
  -TestSpecPath tools\agent_test.example.json `
  -Mode preview

# 确认 workflow.generated.json 后，才执行真实测试
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent_test.ps1 `
  -TestSpecPath tools\agent_test.example.json `
  -Mode execute
```

外部 Codex/Claude Code/Cursor 通过同一个 `mikanengine` MCP 配置调用 `run_agent_test`；不需要把 OpenAI/Anthropic 密钥输入引擎。每次结果位于 `out\agent_tests\<run-id>\`，重点读取 `result.json` 的 `evidenceResultPath`、`evaluationResultPath`、`failures`、`diagnostics` 和 `recommendations`。移动端暂不纳入此闭环；Nsight 权限不足会保留 `permission_denied`，不能作为性能通过。

如果 Agent 需要先理解项目，可直接调用 `get_project_context`；它返回 manifest、资源根、默认场景引用和带 `semanticRole` 的资产索引，例如 base color、normal、PBR、emissive、模型、脚本、音频等。
当测试已生成失败结果，但问题只涉及测试窗口或采集参数时，可调用 `run_agent_repair`。它接收失败的 `agent_test` `result.json` 和 `tools\agent_repair.schema.json` 定义的候选，只能修改已有 workflow 的白名单 `args.<field>`，不能改场景、脚本、源码、步骤依赖或断言期望值；默认 `preview`，确认候选后显式 `execute` 重跑。示例：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent_repair.ps1 `
  -SourceResultPath out\agent_tests\<failed-run>\result.json `
  -RepairPath tools\agent_repair.example.json `
  -Mode preview
```

输入回放和受控修复的完整边界见 `docs\AI_Native_AgentTesting.md`；可直接复用 `tools\agent_test.replay.example.json`、`tools\agent_test.repair.failure.example.json` 和 `tools\agent_repair.example.json`。`RunId` 建议不超过 32 个字符，以避免 Windows 深层产物路径超出限制。

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

`--project <dir>` / `--no-project-manager` / `--scene <path>` / `--game <name>` / `--no-voxel-world` / `--no-editor` / `--headless` / `--headless-no-render` / `--frames N` / `--fixed-dt seconds` / `--dump-state <path>` / `--crash-log <path>` / `--renderdoc-capture-frame N` / `--renderdoc-capture-path <template>` / `--scripted-input` / `--input-replay <path>` / `--dump-schema <path>` / `--phys2d-selftest`（内建 2D 物理自测）
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
| `inspect_project` | 只读发现项目能力、构建入口、Schema、组件键和受限资产索引 | `projectPath`, `maxResults`, `outputRoot` |
| `get_project_context` | 只读读取项目 manifest、资源根、默认场景和语义资产索引 | `projectPath`, `query`, `assetType`, `maxResults` |
| `get_device_capabilities` | 只读探测 Windows 主机、Vulkan 设备、RenderDoc/Nsight 和引擎构建产物 | `renderDocPath`, `nsightPath`, `vulkanInfoPath`, `outputRoot` |
| `inspect_scene` | 只读摘要实体层级、组件、脚本和资源引用 | `projectPath`, `scene`, `maxEntities`, `maxAssetReferences` |
| `query_assets` | 只读按关键词/类型查询场景、脚本、Shader、模型、纹理和音频 | `projectPath`, `query`, `assetType`, `maxResults` |
| apply_scene_commands | 按结构化命令编辑场景，并可自动校验/回归 | scene, commands, outputPath, inPlace, testLayer |
| `create_script` | 生成/校验 Agent 玩法脚本，可选编译游戏插件 | `scriptName`, `outputPath`, `fields`, `source`, `overwrite`, `compile` |
| `run_agent_workflow` | 按依赖编排脚本、构建、场景、测试、dump 和断言 | `workflow`/`workflowPath`, `dryRun`, `continueOnFailure` |
| `run_agent_task` | 失败诊断后选择受限候选补丁并重跑 workflow | `task`/`taskPath`, `mode`, `maxAttempts` |
| `run_agent_plan` | 以目标为入口执行 task，并生成下一轮 planner 可读的 evidence | `plan`/`planPath`, `mode`, `maxAttempts` |
| `run_agent_game_spec` | 将结构化 GameSpec 编译为受控 workflow，并执行测试、验收和交付 | `gameSpec`/`gameSpecPath`, `mode`, `outputRoot` |
| `run_agent_test` | 只测试已有场景：Discovery、校验、玩法/渲染、断言、视觉/性能证据和 Evaluation | `testSpec`/`testSpecPath`, `mode`, `outputRoot` |
| `collect_agent_evidence` | 只读汇总任务/工作流结果、日志、平台和视觉证据 | `resultPath`, `outputRoot` |
| `evaluate_agent_result` | 按 contract 自动判定 evidence 是否满足行为、Discovery、视觉和性能交付门槛 | `evidencePath`, `contract`/`contractPath`, `outputRoot` |
| `capture_frame` | 桌面端 RenderDoc 指定 Present 抓取 `.rdc`，并生成缩略图/日志/manifest | `projectPath`, `scene`, `captureFrame`, `frames`, `game`, `outputRoot` |
| `capture_performance` | 桌面端 Nsight Graphics GPU Trace 或 Graphics Capture，导出指标/replay CSV/日志/manifest | `projectPath`, `scene`, `captureType`, `captureFrame`, `frames`, `nsightPath`, `outputRoot` |
| `build` | 一键构建/恢复配置/干净构建 | `target`, `killEngine`, `cleanFirst`, `configureIfMissing` |
| `validate_scene` | 离线校验场景 | `projectPath`, `scenePath`, `checkAssets` |
| `run_gameplay_test` | CPU-only 玩法层：MikanTestRunner + 独立日志/dump | `projectPath`, `scene`, `game`, `frames`, `fixedDeltaSeconds`, `extraArgs`, `timeoutMs` |
| `run_render_test` | Vulkan 渲染层：EngineMain + 独立日志/dump | `projectPath`, `scene`, `game`, `frames`, `fixedDeltaSeconds`, `extraArgs`, `timeoutMs` |
| `run_test` | 兼容入口；false=玩法层，true=渲染层 | 同上，另含 `render` |
| `read_dump` | 读取并验证 dump JSON | `path`（可省略，使用本会话最近成功 dump） |
| `assert_state` | 对实体状态做数值/可见性断言 | `path`（可省略）, `assertions[]` |
| `engine_status` | 进程/崩溃/exe 状态 | — |
| `stop_engine` | 杀引擎释放文件锁 | — |

**AI 标准闭环**：项目化任务先调用 `inspect_project` 和 `get_project_context` 获取能力、项目根与语义资产索引；涉及渲染或性能时，再调用 `get_device_capabilities` 确认 Vulkan、RenderDoc/Nsight 和引擎产物状态，然后用 `inspect_scene`/`query_assets` 建立场景上下文。之后用 `run_agent_plan(mode=preview)` 审查目标、task、候选和证据计划，再用 `mode=execute` 进入 `agent_task`。失败时按结构化信号选择候选并重跑，随后由 `collect_agent_evidence` 汇总 discovery、日志、平台、视觉和性能证据，再由 `evaluate_agent_result` 按 contract 判定是否达到交付门槛。核心链路是 `inspect_project` → `get_project_context` → `inspect_scene`/`query_assets` → `create_script`/`apply_scene_commands` → `build/compile_games` → `validate_scene` → `run_gameplay_test` → `read_dump`/`assert_state`，涉及 shader/GPU/交换链时再加入 `run_render_test` → `capture_frame`/`capture_performance`。每次输出到独立 `agent_discovery/<run-id>/`、`mcp_runs/<run-id>/`、`agent_runs/<run-id>/`、`agent_tasks/<run-id>/`、`agent_plans/<run-id>/` 或 `agent_evaluations/<run-id>/`，不会把历史崩溃日志误判为本次失败。入口细节见 [`docs/AI_Native_AgentDiscovery.md`](docs/AI_Native_AgentDiscovery.md)、[`docs/AI_Native_AgentEvaluator.md`](docs/AI_Native_AgentEvaluator.md) 和 [`docs/AI_Native_DeviceCapabilities.md`](docs/AI_Native_DeviceCapabilities.md)。

> 只验证已有场景时，外部 Agent 应优先使用 `run_agent_test`：它把 Discovery、场景校验、玩法/渲染测试、状态断言、桌面采集、Evidence 和 Evaluation 统一成只测试闭环；需要生成内容时再使用 `run_agent_game_spec`。

### 桌面端 RenderDoc 自动抓帧

```powershell
& "D:\Engine project\vulkan engine\tools\renderdoc_capture.ps1" `
  -TargetPath "D:\Engine project\vulkan engine\out\build\x64-Release\EngineMain.exe" `
  -WorkingDirectory "D:\Engine project\vulkan engine" `
  -TargetArguments @("--headless", "--frames", "90", "--fixed-dt", "0.016666667", "--no-project-manager", "--no-editor", "--scene", "D:\Engine project\vulkan engine\assets\baka3d.json", "--game", "baka3d") `
  -CaptureFrame 60
```

执行模式需要 64 位 RenderDoc 的 `renderdoccmd.exe`（可放入 PATH，或传 `-RenderDocCmdPath`）。脚本会让 RenderDoc 注入引擎，由引擎在第 N 个 Present 前调用 `TriggerCapture`，然后在 `out/renderdoc_captures/<run-id>/` 保存 `.rdc`、缩略图、进程日志和 SHA-256 manifest。`-Preview` 只解析参数并生成 command/result，不启动目标程序；当前机器未发现 RenderDoc 时，execute 会返回 `status=unavailable`，不会伪造视觉证据。实现细节见 [`docs/AI_Native_RenderDocCapture.md`](docs/AI_Native_RenderDocCapture.md)。

### 桌面端 Nsight Graphics 自动性能采集

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\nsight_capture.ps1" `
  -TargetPath "D:\Engine project\vulkan engine\out\build\x64-Release\EngineMain.exe" `
  -WorkingDirectory "D:\Engine project\vulkan engine" `
  -TargetArgumentsJson '["--headless","--frames","180","--fixed-dt","0.016666667","--no-project-manager","--no-editor","--scene","assets/baka3d.json","--game","baka3d"]' `
  -CaptureType gpu_trace -CaptureFrame 60 -FrameCount 1 `
  -NsightPath "D:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.1.0\host\windows-desktop-nomad-x64\ngfx-ui.exe"
```

`capture_performance` 支持 `gpu_trace` 和 `graphics_capture`。前者会导出 `.ngfx-gputrace`、`FRAME.xls`/`GPUTRACE_FRAME.xls` 等硬件指标，后者会保存 `.ngfx-capture` 并可生成 replay `iteration_times.csv`。GPU Trace 访问 NVIDIA performance counters 时可能要求管理员权限；权限不足会返回 `permission_denied` 和可执行的下一步，不会把空报告当成性能结论。GPU Trace 包含 profiling 开销，replay 的 `msFrameTime` 也不等于应用原生 GPU frame time，优化对比应使用同机、同场景、同采集参数。详细边界见 [`docs/AI_Native_NsightPerformance.md`](docs/AI_Native_NsightPerformance.md)。

---

## 4. 陷阱要点

### 构建/运行
- **文件锁 LNK1104**：EngineMain 运行中锁 Game.dll/Editor.dll → `build.ps1` 默认拒绝（exit 2），加 `-KillEngine`
- **改 Editor 代码**：构建 EngineMain 已连带 Editor.dll，能生效；无需单独 `--target Editor`（除非只想重编 Editor）
- **空 out 目录**：用 `tools/build.ps1 -ConfigureIfMissing` 自动按 `x64-release` preset 配置；怀疑中间产物陈旧时加 `-CleanFirst`
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
- 崩溃排查：默认看 exe 工作目录下 `log/crash_log.txt`；MCP 看对应 `mcp_runs/<run-id>/crash_log.txt`；编译错误看 `build.log`
- 引擎日志多为 printf/stderr，无分级（Phase 0.2 计划引入 Log 系统）
- 项目已完成 Git 初始化并推送至远程 `main`；`out/`、`android/.gradle`、`*.bak` 等已由 `.gitignore` 排除，备份文件仅保留在本地工作区
