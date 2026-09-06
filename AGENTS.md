# AGENTS.md — MikanEngine 工作指引

> **新对话快速入口：先读 `PROJECT_CONTEXT.md`。** 其中包含项目大纲、活跃代码树、关键入口、模块关系和默认扫描边界；只有任务涉及具体模块时再读取对应源码，禁止把 `D:\Engine project\backup`、`out/`、第三方依赖和大资源目录作为常规上下文扫描。

## 项目

- 位置：`D:\Engine project\vulkan engine`（路径含空格，命令中须引号）
- 自研 Vulkan 游戏引擎：C++17 + SDL3 + Vulkan + ImGui + Jolt + Box2D，构建 CMake + Ninja + MSVC，产物 `out\build\x64-Release\`
- **备份目录**：部分项目备份/历史副本位于 `D:\Engine project\backup`。该目录不属于当前源码树；默认不扫描、不编译、不提交、不移动或删除其中内容。需要恢复或引用时，必须使用绝对路径并先确认具体子目录/文件。
- **完整文档：先读 `README.md`**（项目大纲/编译测试/MCP/陷阱四部分）；命令细节见 `编译命令.md`；规划见 `docs/工业化开发计划.md`、`docs/开发路线图_2026H2.md`、`docs/待办问题.md`
- 架构：`Game.dll`(共享运行时) / `Editor.dll`(编辑器,可选) / `EngineMain.exe`(完整 SDL/Vulkan 宿主) / `MikanTestRunner.exe`(CPU-only 玩法测试宿主) / `games/<name>`(游戏插件 DLL，独立编译+热重载，改玩法不碰 Game.dll)

## 构建/测试（用现成工具，勿手拼长命令）

- **构建**：`powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\build.ps1" [-Target EngineMain|MikanTestRunner|Editor|Game|CompileShaders] [-KillEngine] [-CleanFirst] [-ConfigureIfMissing]`
  - 退出码：0 成功 / 1 失败 / 2 引擎在运行（加 `-KillEngine` 自动关闭）/ 3 环境错误
  - 改 Editor 代码无需单独 target（EngineMain 连带构建 Editor.dll）
- **游戏插件 DLL（`games/<name>`）独立编译**：`tools\compile_games.ps1`——引擎未运行时直接输出 `out\build\x64-Release\Game<name>.dll`（首次构建/发布形态开箱即用）；引擎运行时输出 `.tmp` 等编辑器热重载重命名（运行中 DLL 被锁定）。**若游戏不运行（如模型不转），先确认 `Game<name>.dll` 存在**（`[GameManager] LoadPlugin: no DLL for '<name>'` = 插件缺失）
- **分层自动测试**：玩法逻辑优先 `MikanTestRunner.exe --frames N --fixed-dt 0.016666667 --dump-state <path> --scene <绝对路径> --game x`；它不创建窗口、不初始化 SDL Video/Vulkan，覆盖场景/插件/脚本/物理/动画。渲染回归使用 `EngineMain.exe --headless --frames N ...`，覆盖 SDL Video/Vulkan/FrameRender/Present
  - 两层 dump 共用结构并带 `runtime_layer`：玩法=`gameplay-cpu`，渲染=`render-vulkan`；退出码 0 正常跑完 / 2 初始化或场景/插件加载失败 / 3 dump 失败
  - 旧 `EngineMain --headless-no-render` 仍可用，但会初始化 SDL/Vulkan，不再作为玩法层首选
  - `MikanTestRunner` 使用 `Game.dll` 中共享运行时代码，但通过 `RuntimeCapabilities` 禁止设备型渲染/音频/输入路径；它是 CPU-only 执行路径，不是单独复制一套 ECS/物理
- **场景校验**：`tools\validate_scene.ps1 <scene.json> [-CheckAssets]`（组件键/字段白名单 + 引用检查；schema 在 `tools\scene_schema.json`）
 - **MCP**（若客户端已接入 `mikanengine`）：`build` / `create_script` / `get_project_context` / `run_agent_workflow` / `run_agent_task` / `run_agent_repair` / `run_agent_plan` / `run_agent_test` / `run_agent_game_spec` / `collect_agent_evidence` / `capture_frame` / `capture_performance` / `validate_scene` / `apply_scene_commands` / `run_gameplay_test` / `run_render_test` / `run_test`(兼容) / `read_dump` / `assert_state` / `engine_status` / `stop_engine`；玩法默认走 CPU-only runner，渲染显式走 EngineMain；`capture_frame` 需要桌面端 64 位 RenderDoc，输出 `.rdc` 后才算视觉证据可用；`capture_performance` 支持 Nsight GPU Trace/Graphics Capture，GPU Trace 权限不足必须记录为 `permission_denied`；每次运行使用独立 `out/build/x64-Release/mcp_runs/<run-id>/`，禁止用旧 `crash_log` 判断新测试

## AI 场景命令层

- MCP 只读设备能力入口是 `get_device_capabilities`；执行桌面渲染/性能测试前先查询 Vulkan、RenderDoc/Nsight 和引擎产物状态。

- scene_command.ps1 是 Agent 编辑场景的首选入口：默认输出副本，按 tools/scene_schema.json 限制组件键和字段，提交前自动运行 validate_scene；可选调用 tools/test.ps1 的 gameplay、render、all 层。
- 支持 create_entity、delete_entity、rename_entity、set_transform、set_parent、set_component、patch_component、remove_component、set_scene_property；新实体可使用 ref 作为后续命令的稳定别名。
- 原地修改必须显式使用 -InPlace，运行目录会保留 commands.applied.json、candidate.json、result.json 和 source.before.json；MCP 对应工具名为 apply_scene_commands。

## AI 玩法脚本层

- `include/ECS/ScriptContext.h` 是 Agent 生成 C++ 玩法脚本的稳定 SDK；优先使用 `Position/SetPosition`、`RotationEuler/SetRotationEuler`、`Find`、`SetParent`、`IsDown/IsPressed/Axis` 和组件 `serializeKey` 接口，不要在生成脚本里重复拼接 `SceneECS`/`Coordinator`/`InputSystem` 单例。
- `tools\script_scaffold.ps1` 是受控代码入口，只能写 `games/<plugin>/` 或 `projects/<project>/games/` 下的 `.cpp`；默认不覆盖已有文件，`-Force` 会在 `out\script_backups\` 保存并校验备份。脚本必须包含 `IScriptBehaviour` 和 `REGISTER_SCRIPT(...)`，并遵守 include 白名单。
- MCP `create_script` 支持生成模板或提交完整 `source`，返回源码 SHA-256、备份路径和可选插件编译结果。推荐顺序：`create_script` → `build(Game)`/插件编译 → `apply_scene_commands` 挂载 script 组件 → `run_gameplay_test` → `read_dump`/`assert_state`。
- `ScriptContext::Destroy`、`AttachScript`、`DetachScript` 在 `OnUpdate` 内均为延迟操作，当前 tick 结束后执行，保证脚本实例表遍历安全；`OnDestroy` 先于实体销毁调用。

## AI 工作流编排层

 - `tools\agent_workflow.ps1` 是高层编排入口；允许的 action 只有 `get_project_context`、`create_script`、`build`、`compile_games`、`validate_scene`、`apply_scene_commands`、`run_gameplay_test`、`run_render_test`、`capture_frame`、`capture_performance`、`read_dump`、`assert_state`、`stop_engine`。
- workflow JSON 必须 `schemaVersion=1`，step id 唯一，依赖只能指向前面已经声明的步骤；模板引用使用 `${steps.<id>.result.<field>}`，不要把绝对路径或任意 PowerShell 传给 Agent。
- 默认失败即停止，并在 `out\agent_runs\<run-id>\` 保留每步日志、`result.json` 和 `manifest.json`；使用 `-DryRun` 先检查执行计划。
- `create_script.overwrite=true` 或 `apply_scene_commands.inPlace=true` 必须同时显式设置 workflow `allowDestructive=true`；场景仍优先输出副本。
- 结果中的 `failureCategory`、`diagnostics`、`nextAction` 是 Agent 的继续决策入口；不要只读取人类日志最后一行判断成功。

## AI 任务修复闭环

- `tools\agent_task.ps1` 在 workflow 失败后按 `failureCategory`、step id 和 action 选择候选修复；默认 `preview`，只有显式 `-Mode execute` 才运行基础 workflow 和候选重跑。
- 候选 patch 只允许 `op=set` 和 `args.<field>`，字段由 action 白名单控制；不能改步骤依赖、`assert_state` 的 expected、任意 PowerShell、`overwrite` 或 `inPlace`。
- 每次任务写入独立 `out\agent_tasks\<run-id>\`，保存基础/候选 workflow、stdout/stderr、嵌套 workflow 结果和 SHA-256 manifest；最多 5 次尝试，默认没有匹配候选就停止。
- `run_agent_task` 是 MCP 对应入口，模型可以生成符合 `tools\agent_task.schema.json` 的候选，但“候选被提出”不等于“修复已通过”；必须检查任务级 `result.json` 的 success、attempts、failure 和 artifacts。
- `tools\agent_repair.ps1` / `tools\agent_repair.schema.json` 是面向外部 Agent 的窄修复入口：输入失败的 `agent_test` 结果和候选 repair，只允许修改已有测试 workflow 的白名单运行参数；默认 preview，显式 execute 才重跑。
- `run_agent_repair` 不允许修改场景、脚本、源码、步骤依赖或断言 expected；成功标准是任务结果中出现通过的 candidate attempt，而不是候选被接受。结果写入 `out\agent_repairs\<run-id>\`，并带 source、候选 workflow、日志和 SHA-256 manifest。

## AI Planner 与 Evidence

- `get_device_capabilities` 是只读桌面设备能力门：返回 Windows 主机、Vulkan 设备、RenderDoc/Nsight 安装状态和引擎构建产物；不提权、不修改驱动，`permission` 未探测时保持 `not_tested`，Android 仍标记为 `deferred`。
 - `tools\agent_discovery.ps1` 是只读上下文入口：`inspect_project` 返回项目能力/构建入口/Schema/inventory，`get_project_context` 返回项目 manifest、资源根、默认场景引用和语义资产索引，`inspect_scene` 返回实体层级/组件/脚本/资源引用摘要，`query_assets` 返回受限资产路径、大小、时间、语义角色和 SHA-256。项目化调用显式提供 `projectPath`，避免把项目资源误解析到引擎根目录；它排除 out、backup、依赖和移动端目录，不替代严格 `validate_scene`。
- `tools\agent_evaluate.ps1` 只读读取 `agent_evidence`，按 evaluation contract 检查 target success、required actions、discovery、视觉和 Nsight 阈值；退出码 1 表示门禁失败，不能修改 contract 来绕过失败。
- `tools\agent_plan.ps1` 接收 `goal + task/taskPath`，默认只 preview；显式 execute 后才会调用 `agent_task`，完成后自动调用 `agent_evidence`。
- `tools\agent_evidence.ps1` 只读汇总 task/workflow 结果、编译/运行日志诊断、运行层、构建产物、Git 版本、Android `adb` 状态、截图/MRT/RenderDoc `.rdc` 和 Nsight `.ngfx-gputrace`/`.ngfx-capture` 引用；无设备、无截图/抓帧或 GPU counters 权限不足必须写成 `unavailable/false/permission_denied`，不能伪造性能结论。
- `tools\renderdoc_capture.ps1` 是桌面端抓帧入口：`renderdoccmd capture` 负责注入，`EngineMain` 在指定一基 Present 前调用 RenderDoc in-application API，脚本等待 `.rdc` 落盘、生成缩略图并校验 manifest。没有 `renderdoccmd.exe` 时只返回 `status=unavailable`，不得把普通窗口截图冒充 RenderDoc capture。
- `tools\nsight_capture.ps1` 是桌面端性能入口：`gpu_trace` 调用 `ngfx.exe` 并整理导出硬件指标，`graphics_capture` 调用 `ngfx-capture.exe` 并可用 `ngfx-replay.exe` 生成 replay CSV；脚本不自动提权、不修改驱动设置，结果中的 `baselineReady=false`/`baselineNote` 用于提醒 Agent 区分 profiling/replay 计时与无采集开销基线。
- Planner 的输出仍受 `agent_task.schema.json` 和 `agent_workflow` action 白名单约束；它不能通过 evidence 或 plan 直接执行任意 PowerShell、安装 APK 或改变断言 expected。
- Planner 下一轮应优先读取 `evidence.json` 的 `discovery`、`failures`、`diagnostics`、`platforms`、`visual`、`performance` 和 `recommendations`，再生成候选 task；没有 discovery context 时先调用 `inspect_project`，不要只根据自然语言日志摘要或猜测组件键做修复决定。若存在 contract，则最后调用 `evaluate_agent_result` 作为交付门禁。

## AI Native GameSpec 层

 - `tools\agent_game_spec.ps1` 将模型输出的 `GameSpec` 编译为受控 plan/workflow：统一编排项目上下文、素材前置检查、场景命令、玩法脚本、构建、CPU-only 玩法测试、状态断言、渲染/RenderDoc/Nsight 验收和 delivery manifest。`project.projectPath` 会贯通资源根、项目相对场景和运行时 `--project`。
- GameSpec 默认 `preview`，必须显式 `-Mode execute` 才启动引擎或写入脚本；场景默认生成隔离副本，不覆盖源场景。`allowDestructive=true` 才能允许脚本 `overwrite=true`。
- `tools\agent_game_spec.schema.json` 是模型输出边界；模型不应直接重写场景 JSON 或调用任意 PowerShell，而应生成 `scene.commands`、`scripts`、`tests` 和 `delivery` 字段。
- MCP 对应入口为 `run_agent_game_spec`。执行结果在 `out\agent_game_specs\<run-id>\`，成功执行会生成 `delivery.zip`；真实 OpenAI/Claude API 适配器仍由外部模型客户端负责，当前引擎侧只接收并执行结构化 GameSpec。

## AI Native Model Planner / CLI 层

 - `tools\agent-cli\src\cli.ts` 是 provider-neutral 的 Node/TypeScript 入口：先通过 MCP 获取 `inspect_project`、`get_project_context`、`inspect_scene` 和 `query_assets`，再把只读上下文、GameSpec Schema 和用户目标写入 prompt，最后校验模型 JSON 并调用 `run_agent_game_spec`。
- 支持 `mock`、`file`、`command` 三种 provider。`command` 只启动用户显式指定的模型 wrapper 并通过 stdin/stdout 交换文本；模型输出不能变成任意 PowerShell、批处理或 shell 命令。
- `preview` 是默认安全路径；`execute` 必须显式传 `--yes`，且 GameSpec 中的 `allowDestructive` 和脚本 `overwrite` 会被 CLI 拒绝。每次运行写入 `out\agent_cli\<run-id>\`，保留 discovery、prompt、原始响应、生成规格和 MCP 结果。
- Node 启动 Windows PowerShell 时会过滤 Codex bundled module path，优先使用系统 Windows PowerShell modules，避免独立进程找不到 `Get-FileHash`。

## AI Native 外部 Agent 测试层

 - `tools\agent_test.ps1` 和 `tools\agent_test.schema.json` 是面向 Codex/Claude Code/Cursor 的窄测试接口：TestSpec 只允许 Discovery、项目上下文、场景校验、构建、CPU-only 玩法测试、Vulkan 渲染测试、状态断言、RenderDoc/Nsight 证据和 Evaluation。提供 `projectPath` 后可使用项目相对 `scenePath`，并先生成项目资产语义索引。
- MCP 对应入口是 `run_agent_test`。它会把 TestSpec 编译成受控 workflow/task/plan，再复用 `agent_plan` 生成 evidence/evaluation；不会接收模型密钥，不允许 `scene.commands`、脚本源码、任意 PowerShell、`allowDestructive` 或源场景覆盖。
- TestSpec 的 `tests.gameplay.replayPath` 支持项目内结构化输入回放，只包含固定帧区间、二维移动和跳跃；回放文件不能包含脚本、场景或任意命令。可复用 `tools\agent_replay.example.json` 和 `tools\agent_test.replay.example.json`。
- 默认 `mode=preview`，外部 Agent 应先审查 `workflow.generated.json` 和 `evaluation.generated.json`，确认后再用 `mode=execute`。失败时先读取 `evidenceResultPath` 中的 `failures`、`diagnostics` 和 `recommendations`；若只是测试窗口/采集参数问题，可提交 `run_agent_repair`，否则提交新的 TestSpec，不修改 expected 断言绕过失败。
- 每次运行输出到 `out\agent_tests\<run-id>\`；测试入口会继续生成嵌套的 task/workflow/evidence/evaluation 目录和 SHA-256 manifest。为避免 Windows 深层产物路径，CLI `RunId` 最长 32 个字符。移动端仍暂缓；Nsight GPU Trace 权限不足必须保留 `permission_denied`。

## 硬性约定

- **🚫 备份纪律（2026-08-26 起，防 AI 对话乱放备份）**：源码树内**禁止创建/遗留任何 `*.bak*` 文件**（含 `.bak-codex-*`、`.stripz.bak`、`*_20260824_timestamp.bak` 等一切变体）。备份只有两条路：**git**（项目已 git 化，改前 `git status` 看基线、大改动先提交 checkpoint 或 `git stash`）或**外部目录 `D:\Engine project\backup\`**（带日期子目录，恢复用绝对路径）。**发现源码树里的 `.bak*` 顺手移到 `D:\Engine project\backup\`，不要留在原地**（2026-08-26 一次清理移走 116 个，历史教训见下）。不要用 `Copy-Item <file> <file>.bak` 这种"原地备份"写法，统一临时目录 + 验证通过即删。规则适用一切文件类型（.cpp/.h/.json/.glsl/.ps1/.java/.kt），不止源码。
- **插件若直接调用 SDL API（如 `SDL_GetKeyboardState`），`compile_games.ps1` 已链接 SDL3.lib/SDL3_image.lib**（2026-08 起）；游戏逻辑尽量走 InputSystem 动作映射，跨 DLL 更稳
- **编码红线（强化）：源文件读写必须全部用 .NET 显式编码**——`[System.IO.File]::ReadAllLines/ReadAllText` + `WriteAllLines/WriteAllText` 都传 `[System.Text.UTF8Encoding]::new($false)`；**禁止 `Get-Content`/`Set-Content` 以隐式编码（ANSI）读写含中文的 .cpp/.h**（历史事故三次：PropertiesWindow 全文件损坏、EngineGlobals 吞代码——即使 WriteAllText 用了 UTF-8，ReadAllText 用 ANSI 也会破坏）
- **无限体素世界原型**：场景放空实体 + 插件挂 `WorldComponent`（参考 `games/voxelproto`）；物品栏选中格写入 `g_HandBlockId`（EngineGlobals，MIKAN_API 导出），EngineMain 放置用 `g_HandBlockId`——物品栏 ↔ 方块放置联动靠此全局
- **修改/删除/覆写源码文件前，备份只允许两种方式，禁止第三条**：① git（首选）——改动前先 `git status` 确认基线，大改动先提交 checkpoint 或 `git stash`；② 外部备份目录 `D:\Engine project\backup\`（带日期子目录）。**禁止在源码树内创建/遗留任何 `*.bak*` 文件**（含 `.bak-codex-*` / `.stripz.bak` / `*_timestamp.bak` 等变体）——历史教训：AI 对话习惯性 `Copy-Item <file> <file>.bak` 留在原目录，源码树一度堆了 116 个 bak 污染排查（2026-08-26 已全部移到 `D:\Engine project\backup\_backup_20260826_bak_sweep\`）；另两项历史事故：① 删 `src/Game/BreakoutGame.cpp` 未备份（靠上下文重建 OnRenderUI）② 覆写 `PropertiesWindow.cpp` 未备份（编码事故致 4-8 月定制编辑逻辑无法恢复，只能重写反射驱动版）——这两次的教训是"要备份"，不是"要 .bak 文件"
- **编码红线：绝不用 `Get-Content`/`Set-Content`/`WriteAllText` 以隐式编码读写含中文的 .cpp/.h**（历史事故：`PropertiesWindow.cpp` 被 ANSI 读 + UTF-8 写导致乱码+换行丢失+注释吞代码）；读写源文件一律用专用工具（edit_file/write_file），确需脚本处理时必须先确认源文件 BOM/编码并显式传编码
- **新增组件需 3 处接入（属性面板已反射驱动，无需手写）**：
  ① `SceneECS.cpp` 组件类型注册表 `RegisterComponent<X>()`（漏了 → GetComponent 崩溃）
  ② `ComponentRegistry.cpp` 反射注册 `RegisterComponent<X>("显示名", "分类", ..., fields表, "serializeKey")`（有 fields+serializeKey 时**序列化与属性面板自动生效**）
  ③ `SceneSerializer.cpp` 的 `DeserializeEntity` 已改为**遍历注册表自动分派**（含手写回退表）——新增组件无需改它；只有需要特殊反序列化逻辑的组件才要加手写回退（kHandwritten）
- **编码红线：绝不用 `Get-Content`/`Set-Content` 读写含中文的 .cpp/.h**（历史事故：`PropertiesWindow.cpp` 被 ANSI 读 + UTF-8 写导致全文件中文字符串/换行损坏 + 注释吞代码，4-8 月定制编辑逻辑无法恢复，只能重写为反射驱动版）；读写一律 `[System.IO.File]::ReadAllText/WriteAllText` + 显式 UTF-8，先确认源文件 BOM/编码再动
- **场景模式判定 `g_SceneIs2D` 由 `SceneRenderer::UpdateSceneMode()` 每帧无条件更新（FrameRender 开头）**：不得把判定放进 3D 渲染路径内部（历史 bug：判定曾内联在 PrepareFrame，2D 场景不经过它，2D→3D 切换后 g_SceneIs2D 卡 true，3D 场景无画面）；「X 只在某渲染路径里执行」是切换类 bug 的常见根因
- **Shader 热更新（2026-08 已实现）**：`ShaderHotReload`（`src/Rendering/ShaderHotReload.cpp`）经 FrameRender 轮询（0.5s 限频，在 vkWaitForFences 后、命令录制前——GPU 空闲窗口）检测 `engine/shaders/glsl/` 与 `engine/shaders/spv/` 文件变化；glsl 变化若有 glslangValidator/glslc 则自动重编（找不到打日志提示，手动跑 `build.ps1 -Target CompileShaders` 后靠 spv 扫描触发）；spv 变化 → `VulkanPipeline::ReloadAllPipelines()` 重建全部已登记管线（VulkanPipeline::Create 成功自动登记、Cleanup 注销；Reload 先建新后毁旧，失败保留旧管线）。**接口不变约定：热更新只允许改 shader 内部计算逻辑，不得改 UBO/采样器/push constant 等接口布局**（descriptor set layout 不重建，改了会崩）；compute pipeline（voxel_culling/voxel_hiz）尚未纳入热更新
- **脚本组件系统（Unity 式 C++ 玩法挂载，2026-08 已实现）**：玩法逻辑不再硬编码 FindByName——脚本类（继承 `ECS::IScriptBehaviour`，实现 OnStart/OnUpdate/OnDestroy/GetParamFields）经 `REGISTER_SCRIPT(Class, "Name")` 注册（游戏插件 DLL 加载时注册），场景 JSON 实体挂 `"script": {"scriptName": "...", "params": {...}}` 即绑定挂载关系。`ScriptSystem` 反序列化时创建实例、回填 params（按脚本参数字段表，`SCRIPT_FIELD` 声明，复用 ComponentRegistry 反射）、每帧 OnUpdate（主循环播放态块，先于插件 OnUpdate）、场景卸载 OnDestroy。**属性面板已支持脚本编辑（Unity 式）**：选中实体 → "脚本"块显示脚本类下拉（已注册脚本列表，切换即 RebindScript 重建实例）+ 参数反射面板（写回实时生效，保存场景时序列化刷新 paramsJson）。**脚本宿主 = 游戏插件 DLL**（保持独立编译/热重载）；**改脚本后跑 `compile_games.ps1`**。参考样例：`src/Game/Baka3dProtoScripts.cpp` 的 RotateScript + `projects/baka3d-third-person/scenes/main.json` 的 script 组件。注意：场景加载早于插件加载，`InstantiateAll` 幂等，引擎在 Activate 游戏模块后补齐实例
- **预制体（Unity 式可复用实体模板，2026-08 已实现）**：`SceneSerializer::SavePrefab(root, path)` 保存实体子树（含全部子实体/组件）为 `assets/prefabs/<name>.prefab.json`（实体 id 归一化 0..N、hierarchy 内部引用重映射）；`InstantiatePrefab(path, parent)` 实例化到场景（返回新根实体，自动补齐脚本实例）。编辑器入口：属性面板"保存为预制体"按钮（选中实体）+ 资产窗口双击 `*.prefab.json` 实例化（自动选中新根）。headless 自测：`--prefab-selftest`（场景加载后保存 Baka + 构造父子树验证，退出码 0/1）
- **骨骼动画（glTF 兼容，2026-08 已实现）**：`ModelLoader` 解析 `aiAnimation`（assimp 归一化，glTF/FBX 通用）为 `MeshData::animations`（AnimationClip/BoneChannel/BoneKeyframe）。**两个关键修复**：① **glTF 动画时间轴**——assimp 把 glTF 的秒放大 1000 存成"毫秒"tick，检测换算后时长 >10s 时按毫秒→秒修正（CesiumMan 83s→2s）；② **关键帧时间轴优先 rotation 通道**——assimp 的 position/rotation/scale 三通道各有独立时间轴，骨骼动画通常 rotation 主导、position 可能单帧，若统一用 position 时间轴会被单帧（time=0）覆盖导致动画不动（Fox 的 Survey 等动画曾因此全静止）。`SampleAnimation` 做关键帧插值（pos lerp / rot slerp / scale lerp）+ 骨骼层级 globalTransform 重算。**蒙皮方案**：GPU 蒙皮主路径 = **uniform texel buffer**（骨骼矩阵每骨骼 4 个 vec4 列，`boneTex` texelFetch 组装 mat4）——**绕开 NVIDIA 驱动对 vertex stage SSBO 读取的兼容性问题（数据正确但 GPU 读错致不可见）与 UBO 数组的 device lost**；`--cpu-skinning` 强制 CPU 蒙皮 fallback（逐顶点加权写 host-visible 顶点缓冲）。实测：GPU 蒙皮 CPU 侧 0.005ms/帧 vs CPU 蒙皮 0.17ms/帧（3273 顶点，34×）。`AnimatorComponent`（挂实体，serializeKey "animator"）：clipIndex/speed/loop/playing + time 运行时回写，SceneRenderer 每帧同步到对应 ModelRenderer（按 mesh.modelPath）。测试素材：CesiumMan.glb（1 动画）、Fox.glb（3 动画 Survey/Walk/Run）、场景 `assets/cesium_man.json`
- **2D 严格由全局 ECS 场景树管理，禁止重建/复活独立 Node 子树**：UI = Canvas 实体(canvas2d) + 子级 UI 实体(sprite2d/textComp/button/slice9)；渲染走 `Canvas2D::RenderECSNodes`、交互走 `UpdateCanvasNodeRecursive`。旧 `Node/SpriteNode/RectNode/ButtonNode` 树已删除（2026-08），头文件注释与 `Clear()` 均为空壳——不要因看到旧文档/旧代码片段而复活
- **Renderer2D 顶点缓冲是单实例共享的**：编辑器场景视图与游戏视图同帧渲染必须 pass 隔离（场景视图用 `UseSecondaryBuffer(true)`，见 `RenderSceneToTarget`），且每个离屏 pass 渲染前必须 `ResetFrame()`——否则同帧互相覆盖顶点数据、跨帧累积 `Vertex buffer overflow`（历史教训：场景视口丢 UI 层 + 98286 quads 溢出）
- **排查「全局标志恒为默认值/功能永不执行」时，先检查赋值语句是否被编码损坏的注释吞掉**（历史教训：`EditorDllApi.cpp` 193 行 `g_ShowSceneView` 赋值被 GBK 乱码注释合并进 `//` 行，场景视图永不渲染；**含历史乱码注释的 .cpp（如 EditorDllApi.cpp）改动前先检查 `//` 行里是否藏了代码**，改文件后用 UTF-8 保存）
- **隐藏/显示 UI 用 `SceneECS::SetVisible(entity, bool)`，禁止移出屏幕（-5000,5000）土办法**：2D 组件（Sprite2D/Text/Button/Slice9）与 3D（Render）统一支持可见性，Canvas2D 渲染前检查、父隐藏连带子树；visible 是运行时字段不序列化（snake/breakout 的 UpdateMenuVisibility 是正确样例）
- **原型/玩法文字用场景树实现，不硬编码绘制**：UI 文字 = Canvas 实体（canvas2d）+ 子实体 textComp（isUI=true，屏幕坐标左上原点 y 向下）；动态文字在插件里更新 `TextComponent.text`，**不要用 `TextRenderer::DrawStringMsdf` 直接画**（baka3d 的 InfoText 是正确样例）
- **场景 JSON 约定**（写 3D 场景前必读 `assets/baka3d.json`——已验证的完整 3D 样例）：
  - `rotation` 四元数顺序为 **[w,x,y,z]**（identity = `[1,0,0,0]`；`[0,0,0,1]` 是绕 Z 转 180°，会把相机倒转）
  - **3D 模型实体必须带 `material` 组件**（`albedoPath` + `useAlbedoTexture:true`），只给 `mesh`+`render` 会 fallback 成无纹理纯色
  - **组件遗漏引擎不报错**（静默忽略/fallback）——写完场景用 `tools\validate_scene.ps1` 校验；权威格式对照 = 引擎 Ctrl+S 保存的 `out\build\x64-Release\auto_save.json`（引擎序列化产物，含完整组件字段）
  - 历史教训（baka3d）：相机倒转 = rotation 顺序写错；模型无纹理 = 缺 material 组件；两者均靠对比 auto_save.json 定位
- **删除/移动源码文件前必须先确认有备份**（git 已跟踪则直接删后用 `git restore` 找回；未跟踪或需留历史副本时先移到 `D:\Engine project\backup\` 或系统临时目录，或直接用 move_file 工具而非裸 Remove-Item）；创建新文件前确认父目录存在（历史教训：删 `src/Game/BreakoutGame.cpp` 未备份，靠会话上下文 + obj 字符串重建了 OnRenderUI）
- 新文件按分类放 `src/` 与 `include/` 同名镜像目录（Core/Rendering/ECS/Editor/UI/World/Game/Platform）；公共头放 `include/`（games/ 插件只可见 include/），实现头跟随 .cpp；**不要放 src/ 或 include/ 根目录**（根只留 HostMain.cpp / StbVorbis.c）
- 改 `tools\*.ps1` 必须保留 **UTF-8 BOM**（Windows PowerShell 5.1 无 BOM 时含中文脚本解析错乱）
- 改组件字段后重新生成 schema：`EngineMain.exe --dump-schema tools\scene_schema.json`
- 场景 JSON 的未知组件键会被引擎**静默忽略**——写完场景务必用 `validate_scene` 校验
- 引擎运行中锁定 Game.dll/Editor.dll：构建前 `-KillEngine` 或 `stop_engine`
- 崩溃排查：默认在 exe 工作目录的 `log/crash_log.txt`；MCP 运行在对应 `mcp_runs/<run-id>/crash_log.txt`（异常码/地址/模块偏移）
- 完整陷阱清单：`README.md` §4、`编译命令.md` 末尾表格
