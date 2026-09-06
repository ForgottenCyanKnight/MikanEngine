# AI Native GameSpec（Phase 6）

`GameSpec` 是模型和 MikanEngine 之间的结构化边界。模型负责把自然语言目标转换成 JSON；引擎侧负责校验、编译和执行受控 workflow。这样模型不需要直接重写场景文件，也不能绕过构建、测试和性能门禁。

## 能力范围

GameSpec 当前可以统一描述：

- 项目入口：场景和游戏插件；
- 素材前置条件：必须存在的路径和只读资源查询；
- 场景变更：`scene_command.ps1` 支持的结构化命令；
- 玩法脚本：通过 `ScriptContext` SDK 生成/写入 C++ 插件脚本；
- 验收：CPU-only 玩法测试、状态 dump/断言、渲染 smoke、RenderDoc 抓帧和 Nsight 性能阈值；
- 交付：生成场景、脚本、spec、workflow、测试证据、evaluation 结果和 `delivery.zip`。

它是“受约束的自动搭建和交付”，不是凭一句提示词生成任意商业游戏。脚本仍必须通过 `script_scaffold` 的 include/危险调用检查，场景仍必须通过 Schema 和引用检查。

## 运行方式

先预览模型输出：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent_game_spec.ps1 `
  -GameSpecPath tools\agent_game_spec.example.json `
  -Mode preview
```

人工检查生成的 `workflow.generated.json`、`evaluation.generated.json` 和路径后执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\agent_game_spec.ps1 `
  -GameSpecPath tools\agent_game_spec.example.json `
  -Mode execute
```

MCP 对应入口是 `run_agent_game_spec`，支持 `gameSpec` 内联对象或 `gameSpecPath`。默认 `preview`；只有显式传 `mode=execute` 才会启动引擎、写入脚本或生成交付场景。

## Agent CLI

Node/TypeScript 客户端 `tools/agent-cli/src/cli.ts` 会先生成带 Discovery 上下文的 `model.prompt.md`，再接收 `mock`、`file` 或用户配置的 `command` provider 响应并调用 `run_agent_game_spec`。

`npm test --prefix tools/agent-cli` 会使用确定性 mock provider 做一次 preview 集成 smoke，不启动引擎。

CLI 的 provider 只负责模型输入输出，不负责绕过引擎安全边界。`command` provider 的 wrapper 从 stdin 读取 prompt、向 stdout 输出一个 GameSpec JSON；Codex、Claude Code、Cursor Agent 的具体命令行参数由用户 wrapper 决定，避免把易变的厂商 CLI 参数硬编码进引擎。

## 推荐的模型输出顺序

```text
inspect_project / inspect_scene / query_assets
  -> GameSpec
  -> preview
  -> apply_scene_commands / create_script
  -> compile / gameplay test
  -> state assertion / RenderDoc / Nsight
  -> evaluation contract
  -> delivery.zip
```

`GameSpec` 的 `requiredPaths` 会在执行前做 SHA-256 记录；场景默认写入本次 run 的隔离目录，不覆盖源场景。只有显式 `allowDestructive=true` 且脚本声明 `overwrite=true` 时才允许覆盖已有脚本，仍会由底层脚本工具保存备份。

## 当前限制

- 还没有在引擎内直接调用 OpenAI/Claude API；模型可以通过 MCP/CLI 生成 GameSpec；
- 玩法质量取决于 `ScriptContext`、场景命令和可复用行为模板，不能从模糊描述推断复杂系统；
- 交付包默认不包含大型构建产物、RenderDoc `.rdc` 或 Nsight 原始文件，可按 spec 显式开启；
- `performance` 采集需要 Nsight 权限，权限不足会作为失败/不可用证据保留，不能伪造通过。

## 对外表达

准确表述：

> 设计 AI Native GameSpec 协议，将模型输出转换为受控场景命令、Gameplay Script、构建测试和性能验收 workflow；通过 preview、Evidence、Evaluation 和 manifest 生成可回放交付包。

暂不表述为：

> 输入一句自然语言即可自动生成任意完整游戏并保证可玩性。
