# AI Native Agent 修复闭环

`agent_task.ps1` 是建立在 `agent_workflow.ps1` 之上的任务层入口，解决的是“工具已经能执行，但失败后如何让 Agent 有证据地继续”的问题。

## 能力边界

任务由四部分组成：基础 workflow、候选修复、失败匹配条件和最大尝试次数。

```text
task JSON
  -> preview 基础 workflow
  -> execute 基础 workflow
  -> 读取失败步骤 / failureCategory / diagnostics
  -> 选择匹配 candidate
  -> 应用受限 args.<field> 补丁
  -> 重跑完整 workflow
  -> 保留每次尝试、日志、结果和 manifest
```

它不是模型本身，也不会从编译器文本中凭空生成可靠 patch。模型或人工负责提出候选，任务执行器负责校验候选、调用已有工具、判断通过与否并留下可回放证据。

## 运行方式

先预览：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent_task.ps1 `
  -TaskPath tools\agent_task.example.json `
  -Mode preview
```

确认候选补丁后执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent_task.ps1 `
  -TaskPath tools\agent_task.example.json `
  -Mode execute
```

示例会让 `validate_scene` 第一次使用不存在的场景路径，得到 `validation_error`；候选 `fix_scene_path` 只修改 `args.scenePath`，之后由完整 workflow 重新完成场景校验、构建、插件编译、玩法测试、dump 读取和状态断言。

## 候选补丁规则

- 只支持 `op=set`。
- 路径只能是 `args.<field>`，不能修改 workflow 的步骤、依赖、断言结构或执行器代码。
- 字段按 action 白名单限制：例如 `validate_scene` 可改 `scenePath/checkAssets`，测试可改场景、帧数和 dump 路径。
- `assert_state` 不允许作为候选 patch 目标，因此不能通过修改 expected 来掩盖失败。
- `create_script.overwrite` 和 `apply_scene_commands.inPlace` 不在白名单中；高风险修改仍由 workflow 的 `allowDestructive` 闸门负责。
- 单任务最多 5 次尝试、32 个候选；默认遇到没有匹配候选就停止。
- 每次重跑使用独立目录，不覆盖原始 workflow 或前一次尝试。

## 机器可读结果

每次运行写入 `out\agent_tasks\<run-id>\`：

- `task.input.json`：原始任务输入。
- `attempt-*/`：每次基础/候选尝试的 workflow、stdout、stderr 和嵌套 workflow 运行目录。
- `result.json`：任务级 success、尝试记录、失败分类、选中的候选和下一步建议。
- `manifest.json`：任务目录内文件的大小和 SHA-256。

MCP 对应工具是 `run_agent_task`，默认 `mode=preview`；需要实际运行时显式传 `mode=execute`。

## 已接入 Planner / Evidence

`agent_plan.ps1` 将目标、task 执行和证据采集串成单一入口；`agent_evidence.ps1` 会把失败历史、日志诊断、平台状态和视觉证据的可用性写成机器可读结果。这样模型下一轮读取的是结构化观察，而不是只看终端最后一行。

当前没有自动调用模型 API，也没有把“没有 Android 设备/截图”包装成成功。Planner 仍只输出 `agent_task.schema.json` 允许的候选补丁。

## 下一步演进

当前已具备确定性的修复执行器。后续可以接入模型 planner、编译器错误摘要、场景差异和截图/MRT 证据，但 planner 仍应只输出本任务 schema 允许的候选补丁，并经过 preview、策略校验和必要的人工确认。
