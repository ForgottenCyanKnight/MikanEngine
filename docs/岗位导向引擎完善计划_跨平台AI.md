# 岗位导向引擎完善计划：跨平台 Graphics × AI Agent

> 适用项目：MikanEngine / vulkan engine
> 基线日期：2026-09-03
> 目标方向：游戏引擎技术工程师（自研引擎、AI Native、AI Agent 工具链、移动端 Graphics）
> 计划性质：在不牺牲引擎真实质量的前提下，补齐最能证明岗位能力的工程闭环。

---

## 0. 计划目标

本计划不以继续堆叠渲染效果数量为主要目标，而是把现有引擎收敛成一个可以被稳定验证、被 AI Agent 可靠调用、并能在桌面与移动端展示的工程样例。

计划结束时，希望能够用一个完整演示回答以下问题：

1. 引擎如何在 Windows 与 Android 上共享运行时和渲染架构？
2. 面对不同 GPU、资源系统和平台 IO 差异，如何设计能力查询、降级和错误诊断？
3. AI Agent 如何通过受控工具创建场景、修改组件、构建项目、运行测试和分析渲染结果？
4. 如何证明这些能力不是 Demo，而是可重复、可回归、可维护的工程系统？

最终对外定位：

> 一个以 C++/Vulkan 为核心、支持 Windows/Android 的个人游戏引擎；
> 以结构化场景数据、MCP 工具和确定性测试为基础，探索 AI Agent 驱动的游戏开发工作流。

当前应避免的过度表述：

- 没有完整 Metal/WebGL 后端时，不写“支持 Vulkan/Metal/WebGL 多后端”。
- 只有 MCP 工具而没有自主规划、执行和评估闭环时，不写“已经实现 AI 自动开发游戏”。
- Hi-Z、SSGI、SMAA、CMAA 等部分链路仍处于关闭或实验状态时，不写“所有高级渲染特性均已稳定”。
- 只使用过 Codex/Cursor 辅助写代码时，不等同于“AI Native 引擎”。

---

## 1. 目标岗位能力矩阵

| 岗位能力 | 当前项目基础 | 需要补强的证据 |
|---|---|---|
| C++ 与引擎架构 | C++17、ECS、场景系统、Game/Editor/Plugin 分层 | 用架构文档、接口边界和独立构建证明可维护性 |
| Vulkan / Graphics | MRT、z-prepass、后处理、阴影、GPU 蒙皮、Cluster culling | 收敛 RenderPass/资源同步，使用 RenderDoc + Nsight GPU Trace 形成可复测性能证据 |
| 移动端落地 | Android arm64、SDL 资源读取、Adreno 独立合成路径 | 建立能力报告、设备矩阵、resize/恢复/安装测试 |
| 数据结构与性能 | BVH、四叉树、Chunk、LOD、实例合批、队列 | 增加固定基准，记录 CPU/GPU 帧时间和内存 |
| 多线程 | 体素 Chunk 网格生成存在 worker/写回路径 | 明确线程边界、资源所有权和渲染线程约束，不夸大为全引擎并行化 |
| AI Native | 场景 JSON、组件元数据、Schema、状态 dump | 为场景/项目/资产增加版本、诊断和稳定命令接口 |
| Agent / Tool-use | MCP 已暴露构建、校验、玩法/渲染测试、dump/断言、RenderDoc 抓帧和 Nsight 性能采集 | 增加场景编辑、资产查询、设备能力和 Android 执行适配 |
| JavaScript / Web 工具 | 当前 JS/TS 不是主链路 | 用 Node/TypeScript 做轻量 Agent CLI 或调试面板，不贸然重写渲染后端 |
| 工程质量 | 已有脚本、日志、headless 和项目发布基础 | 统一 test 入口、结构化结果、CI 和清洁发布包 |
| 技术表达 | 有路线图、Android 排障记录和代码规模 | 形成 3 个可演示案例和一份可读的技术文章/项目说明 |

---

## 2. 当前基线

### 2.1 已具备的能力

- EngineMain.exe：完整 SDL/Vulkan 宿主，可加载编辑器或以纯游戏模式运行。
- Game.dll：运行时核心，包含 ECS、场景、渲染、物理、音频、UI 和世界系统。
- Editor.dll：ImGui docking 编辑器，包含层级、属性、资源、场景/游戏视图、材质、预览和撤销。
- Game<name>.dll：玩法插件，桌面端可独立编译和热重载。
- MikanTestRunner.exe：不初始化 SDL Video/Vulkan 的玩法层测试宿主。
- ECS 组件元数据可用于 Inspector、场景序列化和 Schema。
- 场景支持实体层级、Prefab、脚本参数和 2D/3D 组件。
- Vulkan 渲染链包含离屏目标、MRT、后处理、时序历史、阴影、地形、水体和体素路径。
- Android 端已存在与桌面运行时基本对齐的 CMake 源码清单和真机排障记录。
- MCP 服务已有构建、场景校验、玩法测试、渲染测试、状态 dump、断言、RenderDoc 抓帧和 Nsight 桌面性能采集能力。

参考：

- [项目架构说明](../PROJECT_CONTEXT.md)
- [现有开发路线图](开发路线图_2026H2.md)
- [工业化开发计划](工业化开发计划.md)
- [引擎 README](../README.md)

### 2.2 关键缺口

- 尚无统一的 tools/test.ps1，现有测试入口还没有形成完整回归矩阵。
- 资产加载、缓存、释放和热更新尚未统一到 Asset Registry/Lifecycle。
- 场景、Prefab、项目数据缺少正式的版本迁移机制。
- PostProcessChain 已经是轻量 pass graph，但整个 Vulkan 帧流程仍由多个全局状态和手工调度组成。
- Hi-Z culling、部分高级抗锯齿和 SSGI 仍需按设备与稳定性逐项验证。
- Android 已有适配成果，但仍需要把历史设备丢失、资源读取和恢复问题整理成可重复测试。
- 已有声明式 AI Agent 工具编排、受限候选修复、RenderDoc/Nsight 采集和 evidence 汇总层；尚无模型 API planner、资产查询、MRT 采集和移动设备执行闭环。
- 当前没有 Metal/WebGL 后端，不把实现完整多后端列为近期目标。

---

## 3. 目标架构：让引擎成为 Agent 可操作的系统

    用户自然语言 / Agent
              │
              ▼
    Node/TypeScript Agent CLI 或 MCP Client
              │
              ▼
    MCP Tool Gateway
      ├─ Project / Scene / Component API
      ├─ Asset / Shader API
      ├─ Build / Gameplay Test / Render Test
      ├─ Capture / MRT Debug / State Dump
      └─ Device Capability / Android Test
              │
              ▼
    Engine Service Boundary
      ├─ ProjectManager
      ├─ SceneSerializer + Scene Schema
      ├─ ECS Command Layer
      ├─ Asset Registry
      ├─ Renderer / Render Graph
      └─ Platform Capability Layer
              │
              ├─ Windows Vulkan
              └─ Android Vulkan

### 3.1 边界原则

1. Agent 只调用领域工具，不直接操作任意 Vulkan handle、进程或文件系统路径。
2. 场景修改优先使用 SceneCommand 或结构化 patch，避免 Agent 直接重写完整 JSON。
3. 每个工具都返回稳定字段：ok、tool、runId、diagnostics、artifacts、nextAction。
4. 工具必须支持重复执行、失败重试和 dry-run；破坏性操作需要明确确认。
5. 引擎内部错误与平台错误分层返回，例如 scene.schema_error、shader.compile_error、vulkan.feature_missing、android.install_error。
6. AI 负责提出和执行候选修改，人负责架构决策、代码审查和最终视觉验收。
7. 材质 albedo 只表示材质颜色/纹理数据；曝光、环境、反射和画面表现放在合成/后处理层。

---

## 4. 分阶段开发计划

### Phase 0：基线、测试和证据整理

**建议周期：1 周｜优先级：P0**

### 目标

把目前分散的脚本、场景和日志整理成一个可重复的基线，后续所有 AI 和跨平台工作都依赖它。

### 任务

- [ ] 创建 tools/test.ps1，统一执行构建、场景校验、CPU-only 玩法测试、Vulkan headless 渲染测试、状态 dump 和断言。
- [ ] 建立独立测试输出目录：out/test_runs/<run-id>/。
- [ ] 固定最小回归场景：
  - [ ] 空场景。
  - [ ] 2D 物理与 UI 场景。
  - [ ] 3D 模型、材质、相机和阴影场景。
  - [ ] 骨骼动画场景。
  - [ ] 地形/水体场景。
  - [ ] baka3d 或第三人称导航场景。
- [ ] 所有测试结果使用 JSON 汇总，不依赖日志文本匹配。
- [ ] 记录桌面 GPU、驱动、Vulkan API、编译器和 Android 设备信息。
- [ ] 为当前源码和文档状态建立 Git tag 或明确基线提交。

### 验收标准

- [ ] 一条命令可以完成构建、校验、玩法测试和渲染测试。
- [ ] 连续运行 3 次结果一致。
- [ ] 场景加载失败、断言失败、崩溃或 Validation error 返回非零退出码。
- [ ] 每次测试都能定位到独立日志、dump 和截图。

### 岗位证明

能够展示“引擎具备工程质量底座”，而不是只展示一张渲染截图。

---

### Phase 1：跨平台 Graphics 基础收敛

**建议周期：2～3 周｜优先级：P0**

### 目标

让同一个场景和同一套运行时接口可以在 Windows Vulkan 与 Android Vulkan 上运行，并能解释平台差异。

### 任务

- [ ] 建立 PlatformCapabilities / RenderCapabilities：
  - [ ] Vulkan API 和扩展版本。
  - [ ] descriptor、dynamic UBO、独立深度采样等能力。
  - [ ] MRT、HDR、input attachment、compute 支持。
  - [ ] 最大纹理尺寸、uniform/storage buffer 限制。
  - [ ] Android/Adreno/NVIDIA/AMD/集显设备标识。
- [ ] 统一平台资源访问：桌面文件系统、Android APK assets、Shader、场景、字体、纹理、模型和后处理配置。
- [ ] 统一窗口和 Surface 生命周期：首次创建、resize、pause/resume、swapchain 重建和设备丢失报告。
- [ ] 建立桌面/移动端渲染变体表：
  - [ ] Desktop MRT 三 subpass。
  - [ ] Android 单几何 pass + 独立 composite pass。
  - [ ] HDR 格式和采样 fallback。
  - [ ] depth sampler 和 postprocess shader 变体。
- [ ] 检查所有新增源码是否同时纳入 Android CMake。
- [ ] 为每个平台生成 capabilities.json 和启动日志摘要。
- [ ] 不在本阶段实现完整 Metal/WebGL 后端；先保证 Scene/ECS/RenderFrame 数据接口不绑定单个平台。

### 验收标准

- [ ] 同一个第三人称或模型场景在 Windows headless 和 Android 真机可启动。
- [ ] Android 资源读取不依赖 std::ifstream/fopen。
- [ ] 连续 resize、切后台、恢复和重复启动不出现黑屏或未诊断崩溃。
- [ ] 缺失硬件能力时走明确 fallback，并在日志中说明原因。
- [ ] 生成一份桌面与 Android 的能力差异报告。

### 岗位证明

重点展示“跨平台不是改几处宏，而是能力查询、资源抽象、RenderPass 变体和生命周期管理”。

---

### Phase 2：场景、资产和渲染数据契约

**建议周期：2～3 周｜优先级：P0**

### 目标

把引擎内部数据变成稳定、可诊断、可被 Agent 操作的协议。

### 任务

- [ ] 为 project.json、场景、Prefab 增加 formatVersion。
- [ ] 建立版本迁移函数：当前版本读取、旧版本升级、不支持版本拒绝加载。
- [ ] 未知组件、未知字段、缺失字段返回结构化诊断，不再静默吞掉。
- [ ] 实现规范化 JSON 输出，保证相同 ECS 状态产生稳定顺序。
- [ ] 增加原子保存、临时文件和自动恢复版本。
- [ ] 为 ECS 增加最小命令接口：
  - [ ] create_entity。
  - [ ] delete_entity。
  - [ ] set_component。
  - [ ] remove_component。
  - [ ] set_parent。
  - [ ] set_visible。
  - [ ] save_scene。
- [ ] 建立最小 Asset Registry：规范化路径、类型和状态、缺失/失败原因、引用来源、reload 入口。
- [ ] 为 RenderPass/RenderTarget 补充输入、输出、格式、尺寸和生命周期描述。

### 验收标准

- [ ] 场景可以执行“加载 → 保存 → 重载 → 状态比较”。
- [ ] Agent 或脚本可以通过命令接口修改场景，不需要拼接完整 JSON。
- [ ] 所有场景错误能定位到实体、组件、字段和文件。
- [ ] 资产缺失时有统一错误对象和可见的编辑器提示。
- [ ] 命令接口具备 dry-run 和撤销/恢复能力。

### 岗位证明

这是 AI Native 的真正基础：Agent 能否理解和修改引擎，取决于数据契约是否稳定，而不是取决于接入了哪个大模型。

---

### Phase 3：MCP 与 AI Agent 工具链

**建议周期：2～3 周｜优先级：P0**

### 目标

把现有 MCP 基础设施升级为可演示、可评估、可安全运行的 Agent tool-use 系统。

### 3.0 已完成子阶段：Gameplay Script SDK

本子阶段把 Agent 的代码输出接入现有 C++ 脚本宿主，作为场景命令之后的下一条可验证链路：

- [x] 新增 `ECS::ScriptContext`，收敛实体查询、层级、变换、输入、组件反射和脚本生命周期接口。
- [x] 为 `ScriptSystem` 增加动态挂载/解绑和延迟实体销毁，避免 Agent 生成逻辑在 `OnUpdate` 中修改实例表导致迭代器失效。
- [x] 新增 `tools/script_scaffold.ps1` 和 `tools/script_sdk.json`，限制输出目录、include、危险调用，并返回源码哈希。
- [x] MCP 新增 `create_script`，支持模板/完整源码、覆盖前备份和可选插件编译。
- [x] 用 `AgentMotionScript` 和 `RotateScript` 完成生成脚本、插件编译、场景挂载、固定步进玩法测试闭环。

准确的阶段结论是“AI Agent 可通过受控接口生成并验证 C++ 玩法脚本”，不是“已经实现任意游戏的自主开发”。

### 3.1 已完成子阶段：Agent Workflow Orchestrator

本子阶段把分散的 MCP/PowerShell 工具提升为可复用的声明式执行计划，重点验证“工具能否自己串起来”，而不是把模型能力包装成引擎能力：

- [x] 新增 `tools/agent_workflow.ps1`、`tools/agent_workflow.schema.json` 和可重复运行的示例 workflow。
- [x] 支持步骤依赖、`run-id` 独立目录、每步输入/输出日志、`result.json` 和哈希 `manifest.json`。
- [x] 支持 `${steps.<id>.result.<field>}` 产物引用、默认失败即停止、`continueOnFailure` 和 `-DryRun`。
- [x] 对脚本覆盖和场景原地修改增加 `allowDestructive` 显式闸门，并限制 action 白名单，禁止 workflow 直接执行任意 PowerShell。
- [x] MCP 新增 `run_agent_workflow`，可传入内联 workflow 或项目内 workflow 文件。
- [x] `agent_workflow.example.json` 已完成场景校验、构建、插件编译、玩法测试、dump 读取和状态断言 6/6 闭环。

当前阶段的准确表述是“具备可审查、可回放的 Agent 工具编排层”。下一步是在不扩大任意写入权限的前提下，把失败分类、候选修复和受限重跑做成任务层，再接入模型 planner、资产和设备工具。

### 3.2 已完成子阶段：Agent Repair Loop

本子阶段把 workflow 的单次失败升级为有证据的任务级继续机制。候选修复由模型或人工提出，执行器只负责 schema 校验、失败匹配、受限参数补丁和完整 workflow 重跑：

- [x] 新增 `tools/agent_task.ps1`、`tools/agent_task.schema.json` 和可重复运行的失败修复示例。
- [x] 支持 `preview`/`execute`、最大尝试次数、候选 `when` 匹配和 `failureCategory`/`diagnostics`/`nextAction` 传递。
- [x] 候选 patch 只允许 action 白名单内的 `args.<field>`，禁止修改步骤依赖、断言 expected、任意 PowerShell、`overwrite` 和 `inPlace`。
- [x] 每次基础/候选尝试保留独立 workflow、日志、嵌套结果和任务级 SHA-256 manifest。
- [x] MCP 新增 `run_agent_task`，默认 preview，execute 需要显式选择。
- [x] `agent_task.example.json` 已完成“故意 validation_error → 修正 scenePath → 完整 workflow 通过”的演示闭环。

当前阶段的准确表述是“具备受限候选修复和可回放重跑能力”，不是“模型已经自动生成正确修复”。下一步是接入模型 planner 和真实编译器/渲染证据，但 planner 只能输出任务 schema 允许的候选。

### 3.3 已完成子阶段：Planner 与 Evidence

本子阶段把“模型提出目标”与“工具执行结果”之间的观察契约补齐：Planner 负责提交目标和 task，执行器负责调用受控任务，Evidence 负责把失败、日志、平台和视觉证据统一给下一轮决策：

- [x] 新增 `tools/agent_plan.ps1`、`tools/agent_plan.schema.json` 和示例 plan。
- [x] 新增只读 `tools/agent_evidence.ps1`，汇总 task/workflow 结果、结构化 diagnostics、日志错误、构建产物和 Git 版本。
- [x] Android 能力以 `adb` 探测结果返回 `ready/no_device/unavailable`，不把未连接真机包装为通过。
- [x] 视觉证据明确返回截图/MRT/RenderDoc `.rdc` 数量和缺口；没有 capture 工具时写出下一步建议。
- [x] 桌面端 `capture_frame`：RenderDoc 注入、引擎指定 Present 触发、`.rdc`/缩略图/日志/manifest 输出，并接入 Evidence artifact 汇总；无 RenderDoc 时返回明确 `unavailable`。
- [x] 桌面端 `capture_performance`：接入 Nsight Graphics `gpu_trace`/`graphics_capture`，保存 `.ngfx-gputrace`、`.ngfx-capture`、导出指标、replay CSV、日志和 SHA-256 manifest。
- [x] `agent_evidence.performance`：汇总 GPU frame time、Graphics Engine/SM/L1TEX/DRAM/PCIe 指标、draw/dispatch 数量、replay 计时和权限状态；GPU Trace profiling 开销与 `msGpuTime=-1` 均明确标注，不伪造 FPS 基线。
- [x] MCP 新增 `run_agent_plan` 和 `collect_agent_evidence`。
- [x] Plan 的 execute 演示完成“失败修复 → 完整 workflow → evidence”闭环。
- [x] 新增 `agent_evaluate.ps1` / `evaluate_agent_result`，按 required actions、Discovery、视觉和 Nsight 指标 contract 判定交付门槛。

 当前阶段的准确表述是“具备 Agent Discovery、结构化 GameSpec、模型可调用的计划、桌面端 RenderDoc/Nsight 采集与 evaluator 交付门禁”，不是“已经接入模型 API 或 Android 自动交付”。移动端暂缓；输入回放、测试模板、项目级上下文和语义资产索引已补齐，下一步转向设备能力查询和跨平台执行适配。

### 3.4 工具接口

 - [x] `inspect_project`：project metadata、构建入口、Schema、组件键和受限 inventory；支持显式 `projectPath`。
 - [x] `get_project_context`：读取项目 manifest、资源根、默认场景引用和带 `semanticRole` 的资产索引。
 - [x] `inspect_scene`：实体层级、组件统计、脚本和资源引用的只读结构摘要；支持项目相对场景路径。
 - [x] `query_assets`：按关键词/类型/语义角色查询场景、脚本、Shader、模型、纹理和音频，并返回哈希。
- [ ] project.list、project.open 的编辑器级语义操作。
- [ ] scene.create、scene.patch、scene.save；已有 `apply_scene_commands` 覆盖受控 patch。
- [ ] entity.create、entity.set_component、entity.set_parent。
- [ ] asset.search、asset.import、asset.reload。
- [ ] shader.compile；[x] build。
- [x] run_gameplay_test、run_render_test。
- [x] capture_frame、capture_performance；[ ] capture_mrt；[x] read_dump、assert_state。
- [x] `evaluate_agent_result`：按 contract 验收行为、Discovery、视觉和桌面性能证据。
- [x] `run_agent_game_spec`：将模型输出的素材、场景命令、玩法脚本、测试和交付字段编译为受控 plan/workflow，并在成功后生成 `delivery.zip`。
- [x] `run_agent_test`：为外部 Agent 提供不涉及内容生成的窄测试入口，统一编排已有场景的校验、玩法/渲染测试、状态断言和桌面证据。
- [x] `run_agent_repair`：接收失败的 `agent_test` 结果，只允许修改测试 workflow 的白名单运行参数并保留候选重跑证据。
- [x] `tests.gameplay.replayPath`：使用结构化帧区间、移动和跳跃输入驱动 CPU-only 玩法回放；禁止脚本、场景或任意命令混入回放文件。
 - [x] `get_device_capabilities`：只读探测 Windows 主机、Vulkan loader/设备、RenderDoc/Nsight 工具和引擎构建产物；权限保持 `not_tested`，不提权、不修改驱动。
- [ ] android.install、android.run、android.logcat。

### 3.5 工具安全与可靠性

- [x] 所有文件操作限制在当前项目根目录。
- [x] 所有运行生成物写入独立 run-id 目录。
- [x] 工具支持超时、取消和进程清理。
- [x] workflow 返回统一 JSON 结果，不把关键状态只写入人类日志。
- [x] Nsight 权限不足返回 `permission_denied`，不把空报告或 replay 计时包装为性能通过。
- [x] 破坏性操作提供 dry-run 和变更预览。
- [x] 场景 patch 在写入前通过 Schema 和资源引用校验。
- [ ] Shader 修改保留旧 SPIR-V；编译失败时继续使用旧管线。
- [ ] Android 安装、卸载、日志抓取和截图使用明确 target/device 参数。
- [x] 禁止 Agent 直接执行任意 PowerShell 或删除项目目录。

### 3.6 四条标准 Agent 工作流

#### 工作流 A：自然语言创建场景

    “创建一个带相机、方向光、地形和可移动角色的第三人称场景”
      → scene.create / entity.create / entity.set_component
      → scene.validate
      → run_gameplay_test
      → run_render_test
      → capture_frame / capture_performance

#### 工作流 B：玩法问题定位

    “角色为什么没有落地？”
      → scene.inspect
      → run_gameplay_test
      → read_dump
      → assert_state
      → 生成候选组件/物理修复
      → 再次测试

#### 工作流 C：Shader 迭代

    “让水体在移动端保持稳定并降低带宽”
      → inspect capabilities
      → shader.compile
      → run_render_test
      → capture_mrt
      → 读取 Validation / GPU 日志
      → 保留或回滚 Shader patch

#### 工作流 D：跨平台回归

    同一场景
      → Windows gameplay test
      → Windows Vulkan render test
      → Android build/install/run
      → logcat/capture
      → 对比状态、截图和能力差异

### 3.7 已完成子阶段：AI Native GameSpec

本子阶段把分散的场景命令、Gameplay Script、测试和性能工具收敛到一个模型可生成的结构化规格，解决“模型该输出什么、引擎如何安全执行、什么条件才允许交付”三个问题：

 - [x] 新增 `tools/agent_game_spec.schema.json`，约束 project/assets/scene/scripts/tests/delivery 数据边界；`project.projectPath` 支持项目化执行。
 - [x] 新增 `tools/agent_game_spec.ps1`，将 GameSpec 编译为 `agent_plan`、`agent_task`、`agent_workflow` 和 evaluation contract，并把项目根贯通到上下文、场景和运行时。
- [x] 素材 `requiredPaths` 在执行前做存在性和 SHA-256 记录；查询型素材通过 `query_assets` 提供给 Agent。
- [x] 场景默认写入 `out/agent_game_specs/<run-id>/generated/`，不覆盖源场景；脚本覆盖需要显式 `allowDestructive=true`。
- [x] 统一支持 Gameplay Script、固定步进玩法测试、状态 dump/断言、渲染 smoke、RenderDoc、Nsight 和 delivery manifest。
 - [x] preview smoke 已验证 GameSpec 可生成 9 步 workflow、任务/evidence 计划和交付包；example execute smoke 已完成“场景副本 → 构建 → 玩法测试 → 状态断言 → evaluation → delivery.zip”，11 项检查全部通过。
 - [x] 新增项目化 GameSpec 示例，先执行 `get_project_context`，再使用项目相对场景和运行时 `--project`；避免生成链路退回引擎根路径。

 准确的阶段结论是“模型可以输出可审查的 GameSpec，由引擎执行受控场景/玩法/测试交付链路”，不是“单句提示词可以稳定生成任意商业游戏”。真实模型 planner/CLI、输入回放和玩法模板已补齐；下一步应接入设备能力查询和跨平台执行适配。

### 3.8 JavaScript / TypeScript 补强

- [x] 新建轻量 `tools/agent-cli/`，使用 Node/TypeScript 调用 MCP，并统一 Discovery → prompt → model response → GameSpec → evidence/evaluation 链路。
 - [x] 通过 GameSpec 间接支持 scene patch、scene validate、test gameplay、test render 和 project context；device capabilities、android smoke 保持待办，避免在移动端暂缓期间虚报能力。
- [x] 支持 `mock`、`file`、`command` provider，输出人类可读摘要和机器可读 JSON 两种格式。
- [x] `execute` 必须显式 `--yes`；CLI 拒绝模型打开 `allowDestructive` 或覆盖脚本，并将每次输入、原始响应和结果写入独立 run 目录。
- [ ] 可选增加浏览器调试页面，展示 ECS 实体树、组件字段、测试结果、MRT 截图和平 台能力差异。

### 3.9 已完成子阶段：外部 Agent TestSpec 测试闭环

本子阶段把“已有内容的验证”从 GameSpec 内容生成链路中独立出来，便于外部 Codex/Claude Code/Cursor 通过 MCP 直接调用引擎内部测试工具，不需要向引擎输入模型密钥，也不需要先生成场景或脚本：

- [x] 新增 `tools/agent_test.ps1`、`tools/agent_test.schema.json` 和只测试示例。
- [x] MCP 新增 `run_agent_test`，只接受 Discovery、场景校验、构建、玩法/渲染测试、状态断言、RenderDoc/Nsight 和 Evaluation 相关字段。
- [x] TestSpec 自动编译为受控 workflow/task/plan，复用既有 Evidence/Evaluation，不允许场景命令、脚本源码、任意 PowerShell 或源文件覆盖。
- [x] preview 默认只生成并审查计划；execute 才启动构建、引擎和桌面采集；失败结果保留 `failures`、`diagnostics`、`recommendations` 和 SHA-256 manifest。
- [x] 外部模型 API 和密钥继续由外部客户端负责；引擎侧只接收结构化 TestSpec/MCP 参数。
- [x] 新增 `tools/agent_replay.schema.json` 和回放示例，支持固定帧区间、二维移动、跳跃输入及运行时计数日志。
- [x] 新增 `tools/agent_test.replay.example.json`、`tools/agent_test.repair.failure.example.json` 等可复制测试模板；已用 900 帧回放验证 `water_buoyancy_test` 的完整 Discovery → build → gameplay → evidence → evaluation 链路，Evaluation 11/11 通过。
- [x] 新增 `tools/agent_repair.ps1`、`tools/agent_repair.schema.json` 和 MCP `run_agent_repair`；已验证“超时失败 → preview 候选 → execute 仅放宽 `timeoutMs`/`frames` → candidate 通过”，不写入场景、脚本、源码或断言期望。
- [x] 对测试/repair `RunId` 增加 32 字符上限，避免 Windows 深层证据目录超过路径限制。
- [x] 项目化 TestSpec 支持 `projectPath` + 项目相对 `scenePath`，先生成 `get_project_context`，再把项目根传入场景、测试和桌面采集入口。
- [x] 使用 `projects/third-person-combat` 完成项目化 900 帧 TestSpec execute，Evaluation 12/12 通过；确认资源不再从引擎根目录解析。

当前阶段的准确表述是“外部 Agent 可以通过 MCP 驱动引擎完成确定性输入回放、证据验收和受控参数修复”，不是“引擎内置了模型”或“Agent 能自动修复所有失败”。项目级上下文和语义资产索引已经接入 TestSpec/GameSpec；下一步优先做设备能力查询和跨平台执行适配，让生成与验收都能根据真实设备能力收敛。

### 3.10 已完成子阶段：输入回放、测试模板与受控修复

本子阶段把“测试一次能跑”推进到“测试可复现、失败可解释、参数问题可受控重跑”，为外部 Codex、Claude Code、Cursor 等 Agent 提供更窄的闭环：

- [x] 回放协议独立于场景和脚本：输入文件只描述总帧数与不重叠事件区间，运行时统一转换为 synthetic input。
- [x] 回放结果进入既有 `state dump`、Evidence 和 Evaluation 链路；日志明确记录 replay 总帧数、实际应用帧数、移动帧数和跳跃帧数。
- [x] 以 JSON schema + 示例 TestSpec 固化“玩法回放模板”和“超时修复模板”，外部 Agent 不需要自由拼接命令行。
- [x] `run_agent_repair` 仅允许 `frames`、固定步长、超时、采集窗口等白名单参数，失败分类不匹配时停止，不自动修改业务内容。
- [x] 已完成 direct script 与 MCP protocol smoke；MCP `run_agent_repair(mode=preview)` 返回候选计划，`mode=execute` 保留基础失败和候选通过两次尝试。

阶段结论：当前已经具备“外部 Agent 提出目标/候选，引擎负责确定性执行、证据记录和安全重跑”的工程闭环。它仍不是模型服务，也不意味着一句提示词可以稳定生成任意商业游戏。

### 3.11 已完成子阶段：项目级上下文、资源根与语义资产索引

本子阶段解决了 AI Native 链路中的一个基础但关键的问题：Agent 不能只知道“引擎有什么工具”，还必须知道“当前项目的资源根、默认场景和可用素材是什么”。

- [x] `agent_discovery.ps1` 新增 `context` 模式和 `get_project_context` 能力，读取 `project.json`、`resourceRoot`、`assets[]`、默认场景和项目资源引用。
- [x] 资产索引提供项目相对路径、类型、存在性、登记状态、引用来源、大小、哈希和 `semanticRole`；可识别 base color、normal、PBR、emissive、模型、脚本、音频等常见语义。
- [x] `projectPath` 已贯通 MCP、Workflow、TestSpec、GameSpec、场景校验、玩法/渲染测试和 RenderDoc/Nsight 入口，运行时统一使用 `--project`。
- [x] MCP 新增 `get_project_context`；`inspect_project`、`inspect_scene`、`query_assets` 支持项目化参数。
- [x] 已通过 MCP protocol smoke、项目上下文查询、项目相对场景解析、TestSpec preview/execute 和 GameSpec preview 回归。

阶段结论：外部 Codex、Claude Code、Cursor 可以先获取项目事实和语义资产，再生成受控 GameSpec/TestSpec；引擎不会因为工作目录不同而把项目资源解析到错误根目录。该索引仍是规则型语义，不宣称理解任意美术意图；复杂模型仍应由素材清单、命名约定和人工验收约束。

### 3.12 已完成子阶段：桌面设备能力门

本子阶段让 Agent 在选择桌面渲染/性能工具前先获取真实主机事实，避免把“工具已安装”“Vulkan 可用”“性能权限已验证”混成一个状态：

- [x] `get_device_capabilities` 通过 MCP 暴露只读设备探测，记录 Windows 主机、进程架构、Vulkan loader、`vulkaninfo` 版本和 GPU 设备摘要。
- [x] 记录 RenderDoc UI/命令行捕获器、Nsight UI/GPU Trace/Graphics Capture/Replay 的安装状态、路径和版本；性能 `permission` 在实际采集前保持 `not_tested`。
- [x] 记录当前引擎 `EngineMain.exe`、`MikanTestRunner.exe` 和 `Game.dll` 的存在性、大小与 SHA-256，便于 Agent 判断是否先构建。
- [x] 对能力做保守分类：Vulkan 设备可枚举才标记桌面探测可用；Nsight 工具存在但没有对应 NVIDIA 设备时标记为不适用；Android 保持 `deferred`。
- [x] 已通过 direct script 和 MCP protocol smoke；当前主机探测到 Vulkan 1.4.341、NVIDIA GeForce RTX 4090 Laptop GPU 与 AMD Radeon 780M Graphics，Nsight 工具存在但权限尚未实测。

阶段结论：Agent 现在可以依据设备事实选择 `run_render_test`、RenderDoc 或 Nsight；能力查询不等于性能测试，真正的 GPU Trace 仍需单独执行并接受 `permission_denied` 结果。下一阶段再做跨平台/Android 执行适配。

### 验收标准

- [ ] 5 条标准工作流可以重复运行。
- [ ] 每条工作流至少连续成功 3 次。
- [ ] Agent 能通过工具完成场景修改和测试，不直接改任意源码。
- [x] Agent 能通过 `run_agent_test` 完成已有场景的 Discovery、校验、玩法/渲染测试和证据汇总，不直接改任意源码。
- [ ] 所有失败都有明确分类和下一步建议。
- [ ] 记录工具调用次数、耗时、失败原因和人工介入次数。

### 岗位证明

对外表述为：

> 设计并实现面向 Agent tool-use 的引擎 MCP 接口，将场景编辑、构建、测试、渲染诊断和设备能力查询统一为结构化工具。

如果还没有模型自主规划层，就称为“AI Agent 可接入的工具基础设施”，不要称为“完整自主 Agent”。

---

### Phase 4：渲染架构收敛与移动端稳定性

**建议周期：3～4 周｜优先级：P1**

### 目标

把已有渲染效果变成可解释、可测量、可回归的 Vulkan 系统。

### 任务

- [ ] 绘制并固化当前帧流程：Scene View、Game View、Swapchain、Shadow、G-buffer/composite、Particle、Postprocess、UI。
- [ ] 引入轻量 RenderPassScheduler 或 Render Graph 第一阶段：
  - [ ] 声明资源读写。
  - [ ] 声明 pass 顺序。
  - [ ] 生成必要 barrier。
  - [ ] 检查 layout、format 和尺寸。
  - [ ] 暂不追求完全自动资源分配。
- [ ] 统一临时 RenderTarget 池，减少重复创建。
- [ ] 为 Scene View、Game View、Swapchain 消除隐式全局状态共享。
- [ ] 增加 GPU timestamp，覆盖阴影、主几何、Cluster culling、后处理、UI 和体素。
- [ ] 继续验证 Hi-Z：先保留关闭状态，补 active consumer，在至少两类 GPU 上验证，通过后再默认开启。
- [ ] 将 compute pipeline 纳入 ShaderHotReload。
- [ ] 为 Shader descriptor、push constant 和 UBO 接口增加 layout hash 检查。
- [ ] 保持材质数据、物理光照和画面表现分层：albedo 只保存材质颜色/纹理，环境光、曝光、反射和 tonemap 不写回 albedo。

### 验收标准

- [ ] 标准场景连续运行期间 Vulkan Validation 无 error。
- [ ] Scene View/Game View 同帧渲染不互相污染。
- [ ] 连续 resize、切换场景、切换项目和开关编辑器窗口无崩溃。
- [ ] 每个主要 pass 有 GPU 毫秒数。
- [ ] 新增一个简单后处理 pass 只需修改 pass 描述和对应 shader。
- [ ] Hi-Z 等实验功能有明确的 enabled/disabled 原因和设备矩阵。

### 岗位证明

重点展示“能够读懂并治理 Vulkan 帧图、同步、资源生命周期和移动 GPU 差异”，而不是只展示最终画面。

---

### Phase 5：真实内容闭环与 AI + Graphics Demo

**建议周期：3～4 周｜优先级：P1**

### 目标

用一个小型但完整的可玩项目证明：AI 工具链能服务真实游戏内容和跨平台渲染。

### 推荐样例

优先选择第三人称导航/地形场景，原因是它能同时覆盖：

- 3D 模型、材质、动画。
- 相机和玩家控制。
- 地形/水体。
- Jolt 物理。
- 阴影和后处理。
- UI。
- Windows/Android。
- Agent 创建和回归。

2D breakout 可作为第二个轻量回归样例，用来证明 Box2D、Canvas、Tween、音频和 UI。

### Demo 任务

- [ ] 用户输入自然语言需求。
- [ ] Agent 创建项目或打开已有项目。
- [ ] Agent 创建实体、组件和层级。
- [ ] Agent 绑定模型、材质、场景脚本和相机。
- [ ] Agent 执行场景校验。
- [ ] Agent 执行 CPU-only 玩法测试。
- [ ] Agent 执行 Windows Vulkan 渲染测试。
- [ ] Agent 执行 Android 安装、运行和日志抓取。
- [ ] Agent 读取状态 dump、MRT 截图和平台能力。
- [ ] Agent 汇总结果，并在失败时提出可审查的 patch。

### 验收标准

- [ ] 从空项目到可运行场景不依赖手工修改多个无关文件。
- [ ] Windows 与 Android 使用同一份核心场景数据。
- [ ] 玩法状态和渲染状态都能被独立验证。
- [ ] 发布包不依赖源码目录、构建目录或开发机绝对路径。
- [ ] 录制 3～5 分钟完整演示视频。

---

### Phase 6：性能、质量和可发布性

**建议周期：2～3 周｜优先级：P1**

### 任务

- [ ] 建立三档固定基准：小型功能场景、中型游戏场景、大型模型/体素压力场景。
- [ ] 记录 CPU 帧时间、GPU 帧时间、Draw/Dispatch 数、三角形数、显存估算、首次加载时间、场景切换时间和 Android APK 启动时间。
- [ ] 为 build/test/publish 生成版本信息和 manifest。
- [ ] 发布包只包含引擎运行时、项目资源、必要插件和依赖。
- [ ] 将核心测试接入 CTest，或保持 PowerShell 为唯一调度入口。
- [ ] 将标准回归接入 CI：Windows 构建、CPU-only 玩法测试、场景校验和可用设备上的 Vulkan 冒烟测试。
- [ ] 资产加载失败时显示明确错误，不允许静默黑屏。

### 验收标准

- [ ] 干净目录可以启动发布包。
- [ ] 三次连续测试结果一致。
- [ ] 性能退化超过阈值时自动报警。
- [ ] 每个版本都能回溯源码、Shader、项目数据和测试结果。

---

## 5. 优先级和范围控制

### P0：必须完成

1. 统一 tools/test.ps1。
2. 场景/Prefab/项目数据版本化和结构化诊断。
3. MCP 工具统一返回格式和安全边界。
4. Windows/Android 能力查询与最小回归。
5. 一个 Agent 创建场景并完成测试的 Demo。

### P1：强烈建议完成

1. 最小 Asset Registry。
2. 轻量 RenderPassScheduler。
3. GPU profiler。
4. Compute Shader 热重载。
5. Node/TypeScript Agent CLI。
6. 一个干净的可发布小游戏或第三人称样例。

### P2：有余力再做

1. WebGL/WebGPU 场景预览。
2. Metal API 学习性 POC。
3. 更完整的 AI 资产生成和材质生成。
4. 大规模多线程命令录制。
5. 第二套脚本语言。

### 明确暂缓

- 完整 Metal 后端。
- 完整 WebGL 后端。
- 网络系统。
- 大型编辑器换肤。
- 未被真实样例驱动的更多 GI、抗锯齿和粒子算法。
- 没有测试和资源生命周期支撑的新增渲染特性。

---

## 6. 每周工作方式

每周只维护：

- 一个可演示的阶段目标。
- 最多三个必须完成任务。
- 一个可选任务。

每个任务遵循：

1. 先记录当前行为和复现步骤。
2. 先写测试、Schema 或诊断输出。
3. 再改实现。
4. 在 Windows 和 Android 适用路径上验证。
5. 记录性能或稳定性变化。
6. 独立提交，避免把无关重构混入。
7. 将最终结果补进文档和 Demo 脚本。

---

## 7. 对外成果包

### 7.1 三个核心案例

#### 案例一：Adreno/Vulkan 移动端兼容

问题：移动 GPU 对多 subpass/input attachment、APK assets 和深度采样的行为与桌面不同。
方案：平台资源访问抽象、能力检测、独立 composite pass、格式 fallback 和设备日志。
结果：同一运行时在 Android arm64 上运行，并能解释桌面/移动端的渲染差异。

#### 案例二：AI Agent 驱动的引擎开发闭环

问题：Agent 无法可靠操作大量隐式 ECS/Vulkan 状态。
方案：场景 Schema、ECS command、MCP 工具、固定时间步、状态 dump 和断言。
结果：Agent 可以创建场景、运行测试、读取失败原因并提出可审查修改。

#### 案例三：Vulkan 帧图与时序后处理

问题：MRT、阴影、后处理、TAA 历史和多视口渲染之间存在资源与同步关系。
方案：显式 pass 描述、RenderTarget 生命周期、barrier 检查、GPU profiler 和回归场景。
结果：新增效果可以被测量、开关和回滚。

### 7.2 需要保留的证据

- [ ] 架构图。
- [ ] RenderPass/帧流程图。
- [ ] Windows/Android 同场景截图。
- [ ] MRT/G-buffer 调试截图。
- [ ] MCP 工具调用记录。
- [ ] 测试 JSON、状态 dump 和断言结果。
- [ ] GPU/CPU/Android 性能表。
- [x] 桌面端 Nsight GPU Trace 指标和原始报告样例；[ ] Android/Adreno 性能表。
- [ ] 1 个短视频。
- [ ] 1 篇技术文章或项目深度 README。

### 7.3 推荐项目描述

> MikanEngine 是一套个人自研的跨平台 C++ Vulkan 游戏引擎，覆盖 ECS 场景、编辑器、游戏插件、2D/3D 物理、动画、PBR 渲染、后处理和 Android arm64。项目进一步将场景编辑、构建、玩法测试、Vulkan 渲染测试、RenderDoc 抓帧、Nsight GPU Trace、状态断言和移动端诊断封装为 MCP 工具，为 AI Agent 驱动的游戏开发工作流提供结构化基础设施。

---

## 8. Definition of Done

一个任务只有同时满足以下条件才算完成：

- [ ] 标准构建脚本可以构建。
- [ ] 有自动测试，或记录不可自动化的人工步骤。
- [ ] Vulkan 修改检查过 Validation 和资源生命周期。
- [ ] 新增数据有 Schema、版本和错误处理策略。
- [ ] 新增资源有创建、reload 和释放路径。
- [ ] Windows/Android 适用路径已验证。
- [ ] MCP/Agent 相关工具有稳定输入输出和安全边界。
- [ ] 文档、示例和测试结果同步。
- [ ] 不依赖本机绝对路径。
- [ ] Git 中有独立、可理解、可回退的提交。

---

## 9. 最终岗位定位

完成 P0 和 P1 后，项目应被包装为：

> 具备 C++/Vulkan 图形工程能力、Windows/Android 移动端落地经验，并围绕结构化引擎数据和 MCP 工具探索 AI Agent 游戏开发工作流的个人自研引擎。

最重要的叙事顺序：

    跨平台运行时
      → Vulkan 图形与移动端兼容
      → ECS/场景/资产数据契约
      → MCP/Agent 工具调用
      → 确定性测试与渲染诊断
      → 真实游戏内容和发布包

不要把重点放在“我使用过哪些 AI 工具”，而要证明：

> 我能把一个复杂的游戏引擎拆成 AI 可以理解、调用、验证和回滚的工程接口。
