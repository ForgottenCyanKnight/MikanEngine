# AI Native Agent Workflow

## 定位

`tools/agent_workflow.ps1` 是 MikanEngine 的工具层编排器。它把已有的脚本 SDK、场景命令、构建和确定性测试组合成可回放的执行计划，让模型可以一次提交一个结构化 workflow，再根据每一步的结果继续决策。

它不负责调用哪一个大模型，也不把“使用过 Codex/Cursor/Claude Code”本身当成引擎能力。模型负责提出步骤和候选修改；引擎负责限制边界、执行工具、保存证据和报告失败。

## 支持的 action

| action | 用途 |
|---|---|
| `inspect_project` | 只读发现项目能力、构建入口、Schema 和受限资产索引 |
| `inspect_scene` | 只读摘要实体层级、组件、脚本和资源引用 |
| `query_assets` | 只读按关键词和类型查询项目资产 |
| `create_script` | 生成或校验 `ECS::ScriptContext` C++ 玩法脚本，可编译游戏插件 |
| `build` / `compile_games` | 构建引擎目标或独立游戏插件 |
| `validate_scene` | 离线检查场景 Schema、实体引用和资源路径 |
| `apply_scene_commands` | 用结构化命令生成场景副本或提交显式原地修改 |
| `run_gameplay_test` | CPU-only 固定时间步玩法测试 |
| `run_render_test` | Windows Vulkan headless 渲染测试 |
| `capture_frame` | 桌面端 RenderDoc 指定 Present 抓取 `.rdc`，生成视觉证据 |
| `capture_performance` | 桌面端 Nsight Graphics GPU Trace/Graphics Capture，生成性能报告和结构化指标 |
| `read_dump` / `assert_state` | 读取运行状态并做可审查断言 |
| `stop_engine` | 清理当前项目的测试/引擎进程，释放构建文件锁 |

workflow 文件必须是 `schemaVersion=1`，步骤 id 唯一，依赖只能指向前置步骤：

```json
{
  "schemaVersion": 1,
  "name": "contact2d-smoke",
  "defaults": { "timeoutSeconds": 600 },
  "steps": [
    {
      "id": "validate",
      "action": "validate_scene",
      "args": { "scenePath": "assets/contact2d.json", "checkAssets": true }
    },
    {
      "id": "gameplay",
      "action": "run_gameplay_test",
      "dependsOn": ["validate"],
      "args": {
        "scene": "assets/contact2d.json",
        "game": "contact2d",
        "frames": 60,
        "fixedDeltaSeconds": 0.016666667
      }
    },
    {
      "id": "assert",
      "action": "assert_state",
      "dependsOn": ["gameplay"],
      "args": {
        "path": "${steps.gameplay.result.dumpPath}",
        "assertions": [
          { "entityName": "Bg", "field": "pos", "expected": [-1500, -1000, 0] }
        ]
      }
    }
  ]
}
```

## 执行与产物

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent_workflow.ps1 `
  -WorkflowPath tools\agent_workflow.example.json
```

每次执行写入 `out/agent_runs/<run-id>/`：

- `workflow.input.json`：原始计划；
- `step-*/input.json`、`stdout.log`、`stderr.log`：步骤级审计材料；
- `state.json`、`crash.log`、构建日志和场景候选文件：实际产物；
- `result.json`：步骤状态、退出码、诊断、失败分类、下一步建议和 artifact 路径；
- `manifest.json`：运行材料的大小与 SHA-256。

默认遇到失败停止。`-DryRun` 只创建计划和输入材料，不调用外部工具；`continueOnFailure` 只适合互不依赖的诊断步骤。模板引用只解析 workflow 结果，不允许引用任意环境变量或执行表达式。

脚本覆盖和场景原地修改属于高风险操作，必须同时满足：

```json
{ "allowDestructive": true }
```

并在对应步骤显式使用 `overwrite=true` 或 `inPlace=true`。场景命令自身仍会保留修改前备份，脚本生成器也会保留覆盖前 SHA-256。

## MCP 接入

MCP 的 `run_agent_workflow` 支持 `workflow` 内联对象或 `workflowPath` 项目内文件，返回 `structuredContent`，因此模型可以直接读取：

1. `steps[*].status`：`passed`、`failed`、`skipped`、`planned`；
2. `steps[*].result`：例如 `dumpPath`、`outputScene`、`compileExitCode`；
3. `failureCategory`：`validation_error`、`build_error`、`runtime_error`、`assertion_failed`、`visual_capture_error`、`performance_capture_error` 等；
4. `nextAction`：面向下一次工具调用的修复建议。

## 当前边界

这一阶段实现了“声明式、可回放、可审查的工具编排”，并已补充 Agent Discovery 上下文层、受限 task repair loop、Planner 入口、RenderDoc 视觉抓帧、Nsight 桌面性能证据和 `evaluate_agent_result` 交付门禁；还没有实现模型 API 自主规划、资产语义向量搜索、Android 真机安装运行或移动端 GPU profiler 适配。下一步应接入真实模型 planner/CLI 和候选 patch diff，让模型消费 context 并生成受控任务。
