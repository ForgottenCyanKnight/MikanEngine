# AI Native 桌面端 RenderDoc 抓帧

## 目标

把“让 Agent 检查渲染结果”从普通窗口截图提升为可回放的 RenderDoc capture：Agent 指定场景和帧号，工具启动桌面版 `EngineMain`，RenderDoc 注入 Vulkan 调用，引擎在指定 Present 前触发一次抓帧，最终输出 `.rdc`、缩略图、日志和哈希 manifest。

这条链路只负责桌面端。Android/Adreno 的等价能力仍需要独立的设备 target、截图或移动端 GPU 调试方案，不能把桌面 `.rdc` 结果当成移动端验证。

## 调用链

```text
MCP capture_frame
    -> tools/renderdoc_capture.ps1
    -> renderdoccmd capture --wait-for-exit
    -> 注入 renderdoc.dll
    -> EngineMain --renderdoc-capture-frame N
    -> Present 前调用 RenderDoc TriggerCapture()
    -> out/renderdoc_captures/<run-id>/*.rdc
    -> renderdoccmd thumb
    -> result.json + manifest.json
    -> collect_agent_evidence
```

引擎只在 RenderDoc 已注入时通过动态 API 获取 `renderdoc.dll` 中的 `RENDERDOC_GetAPI`，不会自行 `LoadLibrary`。这样普通启动不依赖 RenderDoc，抓帧失败时也不会把“已配置”误报成“已捕获”。

## 手工调用

先构建 `EngineMain`，再执行：

```powershell
& "D:\Engine project\vulkan engine\tools\build.ps1" -Target EngineMain -ConfigureIfMissing -KillEngine

& "D:\Engine project\vulkan engine\tools\renderdoc_capture.ps1" `
  -TargetPath "D:\Engine project\vulkan engine\out\build\x64-Release\EngineMain.exe" `
  -WorkingDirectory "D:\Engine project\vulkan engine" `
  -TargetArguments @("--headless", "--frames", "90", "--fixed-dt", "0.016666667", "--no-project-manager", "--no-editor", "--scene", "D:\Engine project\vulkan engine\assets\baka3d.json", "--game", "baka3d") `
  -CaptureFrame 60
```

同一 PowerShell 会话中用 `-TargetArguments @(...)` 最直观；如果由另一个 PowerShell 进程启动脚本，使用 `TargetArgumentsJson` 可能受到 Windows 引号处理影响，因此 MCP 内部改用 UTF-8 Base64 传递同一组参数，避免 `--frames`、`--scene` 被解析成抓帧脚本自己的参数。

执行前需要安装与目标位数匹配的 64 位 RenderDoc，并满足以下任一条件：

- `renderdoccmd.exe` 已加入 PATH；
- 设置 `RENDERDOC_CMD_PATH`；
- 传入 `-RenderDocCmdPath "<absolute path>\renderdoccmd.exe"`。

`-Preview` 可以只生成 `command.json`、`result.json` 和 `manifest.json` 来检查参数，不会启动目标程序。

## MCP 调用

```json
{
  "name": "capture_frame",
  "arguments": {
    "scene": "assets/baka3d.json",
    "game": "baka3d",
    "captureFrame": 60,
    "frames": 90,
    "fixedDeltaSeconds": 0.016666667,
    "apiValidation": false,
    "captureCallstacks": false
  }
}
```

工具会自动加入 `--headless`、`--no-editor`、`--renderdoc-capture-frame`、`--renderdoc-capture-path` 和独立的 `--crash-log`。`extraArgs` 不能覆盖场景、帧数、dump、崩溃日志和 RenderDoc 保留参数。

在声明式 Agent 流程中可直接使用 `capture_frame` action；可复制示例是 `tools/agent_workflow.renderdoc.example.json`，任务层示例是 `tools/agent_task.renderdoc.example.json`。如果没有安装 RenderDoc，workflow 会在抓帧步骤失败并保留结构化结果，后续 Agent 可据 `failureCategory=visual_capture_error` 和 `nextAction` 生成受限候选，而不会把环境缺失误判成引擎渲染错误。

## 结果约定

每次调用写入独立目录：

```text
out/renderdoc_captures/<run-id>/
├─ result.json
├─ manifest.json
├─ command.json
├─ *.rdc
├─ *.png                 # 默认由 renderdoccmd thumb 生成
├─ stdout.log
├─ stderr.log
└─ crash_log.txt
```

关键状态：

- `preview`：只完成参数检查和命令落盘。
- `unavailable`：没有找到 `renderdoccmd.exe`，目标程序不会启动。
- `captured`：目标正常退出，至少一个 `.rdc` 已落盘。
- `target_failed` / `target_timeout`：目标进程失败或超时，应先看 `stdout.log`、`stderr.log` 和 `crash_log.txt`。
- `capture_failed`：目标退出但没有找到 `.rdc`，重点检查引擎是否重新构建，以及日志中是否出现 `ready`、`triggered`。

`collect_agent_evidence` 会把 `.rdc` 识别为 `renderdoc_capture`，把缩略图识别为 `renderdoc_thumbnail`。没有 `.rdc` 时，Evidence 的视觉证据仍然是不可用；普通窗口位图不能替代 RenderDoc capture。

## 当前验证结果

- `RenderDocCapture.cpp` 已纳入 CMake，`EngineMain` 增量构建通过。
- 直接运行引擎时已验证新参数能进入初始化路径，并打印 `requested frame 1, but renderdoc.dll is not injected`。
- 脚本 Preview、无 RenderDoc execute、MCP `capture_frame` 调用均已验证；无 RenderDoc环境按设计返回 `status=unavailable`、退出码 3。
- 当前机器没有发现 `renderdoccmd.exe`、`qrenderdoc.exe` 或 PATH 中的 `renderdoc.dll`，因此尚未生成真实 `.rdc`。安装 RenderDoc 后，用上面的 execute 命令完成最后一次端到端验收。
