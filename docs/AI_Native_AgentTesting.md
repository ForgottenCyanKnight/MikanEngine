# AI Native Agent Testing

`run_agent_test` 是给外部 Codex、Claude Code、Cursor 等 Agent 使用的“只测试”入口。它解决的问题是：Agent 不需要先生成场景或代码，也可以用统一、可回放的方式验证已有引擎内容。

## 边界

TestSpec 只描述测试意图和验收范围，不描述实现代码。引擎侧会拒绝以下内容：

- 场景命令、源场景覆盖和 `allowDestructive`。
- C++/脚本源码、任意 PowerShell、批处理或 shell 命令。
- 修改断言 expected 来绕过失败。
- 把 Android、RenderDoc 或 Nsight 不可用包装成通过。

默认模式是 `preview`。只有外部 Agent 或人工明确传入 `mode=execute`，才会构建、启动引擎或执行桌面端采集。

## TestSpec

结构定义见 [`tools/agent_test.schema.json`](../tools/agent_test.schema.json)，最小示例见 [`tools/agent_test.example.json`](../tools/agent_test.example.json)：

```json
{
  "schemaVersion": 1,
  "name": "contact2d-agent-test",
  "goal": "验证 Contact2D 场景和 CPU-only 玩法运行结果，不修改源文件。",
  "mode": "preview",
  "project": { "scenePath": "assets/contact2d.json" },
  "assets": {
    "queries": [
      { "query": "contact2d", "assetType": "scenes", "required": true }
    ]
  },
  "tests": {
    "compileGames": false,
    "gameplay": {
      "enabled": true,
      "frames": 120,
      "fixedDeltaSeconds": 0.016666667,
      "timeoutMs": 120000
    },
    "assertions": [],
    "render": { "enabled": false },
    "capture": { "enabled": false },
    "performance": { "enabled": false }
  }
}
```

`tests.gameplay` 使用 `MikanTestRunner`，不创建窗口；`tests.render` 使用 `EngineMain --headless`；`tests.capture` 使用桌面端 RenderDoc；`tests.performance` 使用桌面端 Nsight Graphics。`assertions` 读取玩法 dump，对实体位置、旋转、缩放和可见性做机器判断。

### 项目上下文与资产语义索引

项目化测试可以在 `project` 中同时声明 `projectPath` 和 `scenePath`：

```json
{
  "project": {
    "projectPath": "projects/third-person-combat",
    "scenePath": "scenes/main.json",
    "game": "cesiumwalk"
  }
}
```

`projectPath` 会贯通 Discovery、校验、玩法/渲染测试和 RenderDoc/Nsight 入口，最终传给运行时的 `--project`；`scenePath` 在提供项目根时可写项目相对路径。也可以直接调用 MCP `get_project_context`，读取 `project.json` 的资源根、默认场景、资源引用和 `assetIndex`。索引中的每项包含项目相对 `path`、`type`、`semanticRole`、标签、是否登记在 `assets[]`、是否存在、场景引用来源和大小；例如 `Lantern_baseColor.png` 会标记为 `material_base_color`。索引只覆盖项目清单、默认场景及其项目资源引用，不扫描 `out/`、`backup/` 或第三方依赖。

## 外部 Agent 调用方式

外部客户端仍使用同一个 `mikanengine` MCP 配置：

```json
{
  "mcpServers": {
    "mikanengine": {
      "command": "powershell",
      "args": [
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        "D:\\Engine project\\vulkan engine\\tools\\mcp_server.ps1"
      ]
    }
  }
}
```

调用 `tools/call` 时选择 `run_agent_test`，可以传项目内的 `testSpecPath`：

```json
{
  "name": "run_agent_test",
  "arguments": {
    "testSpecPath": "tools/agent_test.example.json",
    "mode": "preview",
    "outputRoot": "out/agent_tests"
  }
}
```

通过 preview 后，再用新的、唯一的 `runId` 执行：

```json
{
  "name": "run_agent_test",
  "arguments": {
    "testSpecPath": "tools/agent_test.example.json",
    "mode": "execute",
    "runId": "contact2d-test-001"
  }
}
```

也可以直接传 `testSpec` 对象；服务器会先保存为临时项目内 JSON，再交给同一个执行器。引擎侧不保存、不读取任何 OpenAI、Anthropic 或其他模型密钥。

## 确定性输入回放与测试模板

玩法测试可以在 `tests.gameplay.replayPath` 中引用项目内的输入回放文件。格式由 [`tools/agent_replay.schema.json`](../tools/agent_replay.schema.json) 定义，示例见 [`tools/agent_replay.example.json`](../tools/agent_replay.example.json)：

- `frames` 是固定步进总帧数；`events` 使用半开区间 `[startFrame, endFrame)`，必须按帧排序且不能重叠。
- 每个事件只允许 `move: [x, y]` 和 `jump: true/false`，移动分量范围为 `[-1, 1]`。
- 引擎运行时把回放转换为 synthetic input，输出应用帧数、移动帧数和跳跃帧数；回放文件不能携带脚本、场景路径或任意命令。
- TestSpec 可以省略 `tests.gameplay.frames`，此时采用回放文件的 `frames`；如显式填写，则测试窗口必须由 TestSpec 负责约束。

推荐把常见玩法沉淀成 TestSpec + replay 两个模板，而不是让模型每次自由拼接输入。仓库提供 `tools/agent_test.replay.example.json` 和 `tools/agent_test.repair.failure.example.json` 作为可复制的结构样例。

## 失败后的受控修复

当失败只属于测试窗口、固定步长或桌面采集等待参数时，可以把失败的 `agent_test` 结果交给 `run_agent_repair`：

```json
{
  "name": "run_agent_repair",
  "arguments": {
    "sourceResultPath": "out/agent_tests/<failed-run>/result.json",
    "repairPath": "tools/agent_repair.example.json",
    "mode": "preview"
  }
}
```

Repair 候选只能对已有测试 workflow 的白名单字段执行 `op=set`，例如 `run_gameplay_test.args.timeoutMs`、`frames` 或采集等待时间。它不能修改场景、脚本、源码、步骤依赖、`assert_state` 的 expected，也不能注入命令行参数绕过安全边界。preview 只验证基础 workflow 和候选计划；确认后以新的 `runId` 显式执行，只有任务结果中出现通过的 candidate attempt 才算修复成功。

## Agent 测试循环

推荐让外部 Agent 遵循这个循环：

1. `inspect_project`：确认构建入口、组件键和工具能力；项目化任务先提供 `projectPath`。
2. `get_project_context`：读取项目 manifest、资源根、默认场景和语义资产索引。
3. `inspect_scene` / `query_assets`：确认场景和资源事实，不猜路径。
4. `run_agent_test(mode=preview)`：审查生成的 workflow 和 evaluation contract。
5. `run_agent_test(mode=execute)`：执行玩法、渲染或采集测试。
6. 读取 `evidenceResultPath`：优先看 `failures`、`diagnostics`、`platforms`、`visual`、`performance` 和 `recommendations`。
7. 若只是测试参数问题，先走 `run_agent_repair(mode=preview)`，人工审查后再 execute；若是场景、脚本或资源问题，提交新的 TestSpec 或转入 `run_agent_game_spec`，不要改 expected 断言绕过失败。

## 产物和判断

每次运行位于 `out/agent_tests/<run-id>/`，主要文件包括：

- `test.input.json`：本次测试意图的快照。
- `workflow.generated.json`：实际允许执行的测试步骤。
- `evaluation.generated.json`：自动生成的行为、Discovery、视觉和性能门禁。
- `plan.generated.json`：交给 `agent_plan` 的入口。
- `plan_runs/`：task、workflow、evidence 和 evaluation 的嵌套结果。
- `result.json`：本次测试的机器可读摘要和路径索引。
- `manifest.json`：输入、计划、日志和结果引用的 SHA-256 清单。

`result.success=true` 只表示本次 TestSpec 的门禁满足。视觉证据必须有真实 RenderDoc `.rdc`/缩略图；Nsight GPU Trace 权限不足时应保留 `permission_denied`，不能写成性能通过。当前移动端仍暂缓，TestSpec 不会伪造 Android 安装、运行或 Adreno 指标。

受控修复的产物位于 `out/agent_repairs/<run-id>/`，包括原始失败结果、repair 输入、生成的 task、基础/候选 workflow、日志和 `manifest.json`。为避免 Windows 深层目录路径超限，repair/test 的 `RunId` 最长为 32 个字符。

## 当前能力定位

这一层已经能让外部 AI 工具通过 MCP 调用引擎内部测试工具，并在窄范围内完成“回放 → 证据 → 受控参数修复 → 重跑”。它不是本地模型服务，也不是自动修复所有失败的黑盒 Agent：模型负责提出测试目标和候选，引擎负责限制动作、执行测试、保留证据和判定结果。
