# MikanEngine Agent CLI

这是引擎侧和 OpenAI Codex、Claude Code、Cursor Agent 或其他模型客户端之间的轻量适配层。它不在引擎内硬编码某一家模型 API，而是固定以下协议：

```text
自然语言目标
  -> inspect_project / get_project_context / inspect_scene / query_assets
  -> prompt.md + GameSpec Schema + discovery.json
  -> 模型输出 JSON
  -> 本地安全校验
  -> run_agent_game_spec
  -> Evidence / Evaluation / delivery.zip
```

CLI 只允许模型提交 `GameSpec`，不会把模型输出当成 PowerShell、批处理或任意源码执行。真正的构建、场景命令、脚本写入、测试和交付仍由 MCP 白名单和 `agent_game_spec.ps1` 控制。

## 环境

- Node.js 22.6+；项目当前已验证 Node 24。
- 不需要 npm 依赖，CLI 通过标准库启动项目内 `tools/mcp_server.ps1`。
- 模型命令应从 stdin 读取 prompt，并把 GameSpec JSON 写到 stdout；命令本身由用户显式配置。

## 快速验证

在 `tools/agent-cli` 目录执行：

```powershell
npm test
```

这会使用内置 `mock` provider 做一次 preview，不启动引擎，也不修改游戏源码。

## 命令

项目化项目建议显式传入 `--project-path`。它会让 CLI 先读取项目 `project.json`、资源根、默认场景和语义资产索引；`--scene` 可以使用项目相对路径，生成的 GameSpec 会保留 `project.projectPath`，执行时由引擎传给运行时 `--project`。

如果目标涉及 Vulkan、RenderDoc 或 Nsight，可额外传 `--device`；CLI 会把只读设备能力写入 `discovery.json` 和模型 prompt，但不会执行性能采集或请求管理员权限。
从引擎根目录执行：

```powershell
node --experimental-strip-types tools/agent-cli/src/cli.ts discover `
  --scene assets/contact2d.json `
  --query contact2d
```

项目化 Discovery 示例：

```powershell
node --experimental-strip-types tools/agent-cli/src/cli.ts discover `
  --project-path projects/third-person-combat `
  --scene scenes/main.json `
  --query "base color" `
  --asset-type textures `
  --json
```
生成 prompt 并查看模型输入：

```powershell
node --experimental-strip-types tools/agent-cli/src/cli.ts prompt `
  --scene assets/contact2d.json `
  --goal "给 Contact2D 增加一个可验证的移动目标" `
  --query contact2d
```

使用外部模型命令生成并预览：

```powershell
node --experimental-strip-types tools/agent-cli/src/cli.ts preview `
  --scene assets/contact2d.json `
  --goal "给 Contact2D 增加一个可验证的移动目标" `
  --provider command `
  --model-command "C:\\path\\to\\model-wrapper.exe" `
  --json
```

也可以把 Codex、Claude Code 或 Cursor Agent 的输出保存到文件，再通过 `file` provider 接入：

```powershell
node --experimental-strip-types tools/agent-cli/src/cli.ts preview `
  --scene assets/contact2d.json `
  --goal "给 Contact2D 增加一个可验证的移动目标" `
  --provider file `
  --response-file out\\model_response.txt
```

`command` provider 使用用户指定的可执行文件；Windows 下 `.cmd/.bat` 会通过 `ComSpec` 调用，其他可执行文件直接启动。模型命令的参数用重复的 `--model-arg=...` 传入。由于不同版本的 Codex、Claude Code 和 Cursor Agent CLI 参数可能不同，本项目不猜测厂商参数，推荐用一个很薄的 wrapper 统一 stdin/stdout。

执行真实构建和测试必须显式确认：

```powershell
node --experimental-strip-types tools/agent-cli/src/cli.ts execute `
  --scene assets/contact2d.json `
  --goal "给 Contact2D 增加一个可验证的移动目标" `
  --provider command `
  --model-command "C:\\path\\to\\model-wrapper.exe" `
  --yes
```

### 失败反馈与自动修复

在 `execute` 失败后，可让同一个模型 wrapper（或 `--repair-model-command` 指定的 wrapper）读取本轮 GameSpec 结果和历史尝试，生成受限的 `GameSpecRepair`，再自动重试：

```powershell
node --experimental-strip-types tools/agent-cli/src/cli.ts execute `
  --scene assets/contact2d.json `
  --goal "给 Contact2D 增加一个可验证的移动目标" `
  --provider command `
  --model-command "C:\\path\\to\\model-wrapper.exe" `
  --auto-repair `
  --max-attempts 3 `
  --yes
```

Repair wrapper 的输入仍然来自 stdin，但 prompt 会明确标记为 `MikanEngine AI Native Repair Planner`；它只能输出 `tools/agent_game_spec_repair.schema.json` 规定的 JSON。也可以用 `--repair-response-file` 注入预先保存的 RepairSpec，适合离线回归。

每轮都写入 `out/agent_cli/<run-id>/attempt-XX/`：

- `repair.prompt.md` / `repair.response.txt`：失败反馈和模型原始响应；
- `repair.generated.json`：通过安全校验的规范化补丁；
- `gamespec.generated.json`、`mcp.result.json`：本轮输入和引擎结果；
- 根目录 `autofix.progress.json` / `result.json`：尝试序列、停止原因和最终状态。

自动修复只允许修改已声明的 `scene.commands`、已声明脚本的 `scripts[i].source` 和有限的测试/采集超时参数；不能改变项目、场景、资产要求、断言期望、交付策略、步骤依赖或权限开关。脚本源码补丁执行前会在项目外侧 `backup/ai-native-autofix-*/` 生成带 SHA-256 manifest 的备份；备份校验失败会停止重试。`result.success=true` 只表示最终一轮 GameSpec 通过，不表示“补丁已生成”就算成功。

如果发生过修复，`result.specPath` 仍指向初始模型规格，`result.finalSpecPath` 指向最终执行的规格；`result.gameSpecResultPath` 同样指向最后一轮引擎结果。

不传 `--yes` 时，`execute` 在启动 MCP 或模型命令前就会失败。每次运行输出到 `out/agent_cli/<run-id>/`，包括：

- `discovery.json`：只读项目上下文；
- `model.prompt.md`：发送给模型的完整约束；
- `model.response.txt`：原始模型响应；
- `gamespec.generated.json`：通过本地安全检查的 GameSpec；
- `mcp.result.json` / `result.json`：执行器结果和下一步建议。

## 当前边界

CLI 已经解决模型客户端和引擎执行器之间的协议、证据和权限边界，但没有把 OpenAI/Anthropic 等 API 密钥写进引擎，也不会声称模型可以从一句模糊提示词稳定生成任意商业游戏。模型质量仍取决于资产语义、可复用玩法模板、输入回放和 GameSpec 的具体验收条件。
