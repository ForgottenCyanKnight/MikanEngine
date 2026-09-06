# AI Native Agent Planner / Evidence

这一阶段把模型与引擎之间的接口收敛成三层：

```text
Planner plan.json
      ↓
agent_plan.ps1
      ↓
agent_task.ps1 → agent_workflow.ps1 → 引擎/场景/测试
       ↓
agent_evidence.ps1
       ↓
agent_evaluate.ps1（可选 contract 门禁）
       ↓
下一轮 planner 的 evidence.json / evaluation result
```

## 运行

先预览计划：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent_plan.ps1 `
  -PlanPath tools\agent_plan.example.json `
  -Mode preview
```

人工审查后执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent_plan.ps1 `
  -PlanPath tools\agent_plan.example.json `
  -Mode execute
```

MCP 对应入口是 `run_agent_plan`。如果只需要重新整理已有结果，可调用 `collect_agent_evidence`；如果已有 evidence 和验收规则，可调用 `evaluate_agent_result`，两者都是只读分析入口。

## Plan 契约

Plan 由模型或人工生成，但只允许描述：

- `goal`：当前目标，供人和下一轮模型理解。
- `taskPath` 或内联 `task`：交给 `agent_task` 的受控任务。
- `mode`：`preview` 或 `execute`。
- `maxAttempts`：候选修复最多尝试次数。
- `evidencePath`：可选的历史证据，作为上下文索引。
- `evaluationContractPath` 或 `evaluationContract`：可选的交付门禁；`execute` 时在 evidence 生成后自动验收，`preview` 只记录为 planned。

Plan 不能直接携带 PowerShell、任意命令行、源码写入路径或设备安装指令；这些都必须经过 task/workflow action 白名单。

## Evidence 内容

`evidence.json` 的重点字段：

- `source`：输入结果路径、哈希、工具类型和最终目标状态。
- `observations`：每个 workflow 的 step、耗时、运行层、dump、场景和诊断。
- `failures`：失败步骤、分类、错误、下一步建议；repair loop 的历史失败会保留用于审计。
- `diagnostics`：从结构化结果和 stdout/stderr 提取的编译、运行、Shader、验证和警告信息。
- `host`：PowerShell、处理器架构、Git HEAD 和主要构建产物哈希。
- `platforms.android`：`ready`、`no_device`、`needs_authorization`、`unavailable` 或 `error`。
- `visual`：已发现的截图/MRT 数量；没有 capture 工具时明确为 `available=false`。
- `performance`：Nsight GPU Trace/Graphics Capture 的状态、报告/指标数量、权限状态和可执行下一步；`available=false` 或 `status=permission_denied` 时不能推断性能已经达标。
- `discovery`：项目/场景/资产发现查询次数、最近场景与资产上下文、Schema 和能力入口；没有 discovery 时下一轮应先调用 `inspect_project`。
- `recommendations`：按失败类型生成的下一步建议。

同时生成 `evidence.md` 方便人工快速浏览，所有文件都有 `manifest.json` 哈希记录。

## Planner 决策规则

1. 先看 `source.targetSuccess`，不要因为 repair loop 的历史失败记录而重复修复已经通过的任务。
2. 若存在 `build_error`，先按文件/行号/错误码归纳，再提出候选 task；不能修改断言来绕过编译失败。
3. 若存在 `assertion_failed`，保持 expected 不变，修复脚本或场景行为。
4. Android 不是 `ready` 时，只能提出准备明确设备 target 的下一步，不能声称已完成真机交付。
5. `visual.available=false` 时，不能根据当前 evidence 下视觉质量结论；应补充 capture_frame/capture_mrt 工具。
6. `performance.available=false` 时，不能根据普通 FPS 或日志宣称 GPU 优化完成；桌面 NVIDIA 优先调用 `capture_performance(captureType="gpu_trace")`，权限不足先处理 `permission_denied`。
7. `discovery.available=false` 时，先调用 `inspect_project`；生成或修改场景前补充 `inspect_scene` 和 `query_assets`，不要让模型猜测组件键或资源路径。
8. 新候选先 `preview`，再由人确认后 `execute`；每次执行都读取最终 `result.json` 和 `evidence.json`。

## 当前边界

本阶段实现的是 discovery + Planner 输入/执行/观察 + evaluator 交付门禁契约，不是模型 API 集成。模型可以通过 MCP 获取项目/场景/资产上下文、提交结构化 plan，并按 contract 验收 evidence；项目不会在本地自动调用 OpenAI、Claude 或 Cursor。桌面端已具备 RenderDoc 视觉抓帧和 Nsight 性能采集，Android 安装运行和移动端 GPU profiler 适配暂缓。下一步重点是接入模型 planner/CLI 和候选 patch diff。
