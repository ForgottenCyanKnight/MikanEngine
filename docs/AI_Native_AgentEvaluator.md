# AI Native Agent Evaluator

`agent_evaluate.ps1` 是 AI Native 链路的交付门禁。它读取 `agent_evidence` 产出的 `evidence.json`，根据一个受限的 evaluation contract 做机器可判定检查，并生成新的 `result.json`/`manifest.json`。

## 调用方式

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent_evaluate.ps1 `
  -EvidencePath out\agent_evidence\<run-id>\evidence.json `
  -ContractPath tools\agent_evaluation.example.json
```

MCP 对应工具是 `evaluate_agent_result`。它支持项目内 `contractPath` 或内联 `contract`，结构约束见 [`tools/agent_evaluation.schema.json`](../tools/agent_evaluation.schema.json)；省略 contract 时默认只要求 `evidence.source.targetSuccess=true`。

## Contract 能力

```json
{
  "schemaVersion": 1,
  "requireTargetSuccess": true,
  "requiredActions": [
    { "action": "run_gameplay_test", "status": "passed", "minCount": 1 }
  ],
  "discovery": {
    "required": true,
    "minProjectQueries": 1,
    "minSceneQueries": 1,
    "minAssetQueries": 1
  },
  "visual": {
    "required": true,
    "minRenderDocCaptures": 1
  },
  "performance": {
    "required": true,
    "maxGpuFrameTimeMs": 16.67,
    "maxDrawCount": 500,
    "maxGraphicsEngineActivePct": 95
  }
}
```

- `requiredActions` 检查 evidence 中成功的 workflow step；
- `discovery` 检查成功的项目/场景/资产查询次数；
- `visual` 检查截图、MRT 和 RenderDoc 捕获数量；
- `performance` 检查最新成功 Nsight 性能采集的硬件指标；缺失指标直接失败；
- `requireBaselineReady` 只有在确实有无采集开销基线时才应开启。GPU Trace 的 profiling 证据默认 `baselineReady=false`，不能拿它伪装成游戏原生 FPS 基线。

所有检查都会写入 `evaluation.checks[]`，包括 `id`、`status`、`expected`、`actual` 和说明。退出码 `0` 表示门禁通过，`1` 表示至少一项不满足，`3` 表示输入/环境错误。失败时 Agent 应修复实现或补充证据，不能通过降低 contract 门槛绕过失败。

## AI Native 闭环

```text
inspect_project / inspect_scene / query_assets
                    ↓
          create_script / scene commands
                    ↓
               build + validate
                    ↓
         gameplay/render + capture
                    ↓
              agent_evidence
                    ↓
           evaluate_agent_result
                    ↓
          通过交付 / 生成下一轮任务
```

当前 evaluator 是模型无关的规则执行器，不在本地调用 OpenAI、Claude 或 Cursor。它先把“完成”定义成可验证的工程事实，后续再接入模型 API 或 Agent CLI；移动端执行仍暂缓。
