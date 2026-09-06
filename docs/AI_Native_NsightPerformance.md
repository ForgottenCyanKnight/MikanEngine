# AI Native Nsight 性能证据

## 目标

`capture_performance` 是桌面端性能闭环的受控入口。它把一次 EngineMain 运行和 Nsight Graphics 的采集过程收敛为：

```text
场景 + 固定时间步
        ↓
Nsight Graphics CLI
        ↓
.ngfx-gputrace / .ngfx-capture
        ↓
FRAME.xls / GPUTRACE_FRAME.xls / iteration_times.csv
        ↓
result.json + manifest.json
        ↓
agent_evidence.performance
```

它解决的是“Agent 能否拿到可审查的性能证据”，不是“Agent 自动宣布某个优化有效”。所有采集均保留原始报告、日志和 SHA-256 manifest。

## 两种采集类型

| 类型 | CLI | 产物 | 适合回答的问题 |
|---|---|---|---|
| `gpu_trace` | `ngfx.exe` | `.ngfx-gputrace`、`FRAME.xls`、`GPUTRACE_FRAME.xls`、`REPRO_INFO.xls` | GPU frame time、Graphics Engine 活跃度、SM/L1TEX/DRAM/PCIe 吞吐、draw/dispatch 数量和 warp 活跃度 |
| `graphics_capture` | `ngfx-capture.exe` + `ngfx-replay.exe` | `.ngfx-capture`、`iteration_times.csv`、replay 图片 | 保存可复现的图形帧，并做同机同配置的 replay 计时对比 |

默认优先使用 `gpu_trace`。如果只是想先打通“抓帧文件 → replay 指标”链路，可使用 `graphics_capture`。

## 直接执行

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\nsight_capture.ps1" `
  -TargetPath "D:\Engine project\vulkan engine\out\build\x64-Release\EngineMain.exe" `
  -WorkingDirectory "D:\Engine project\vulkan engine" `
  -TargetArgumentsJson '["--headless","--frames","180","--fixed-dt","0.016666667","--no-project-manager","--no-editor","--scene","assets/baka3d.json","--game","baka3d"]' `
  -CaptureType gpu_trace `
  -CaptureFrame 60 `
  -FrameCount 1 `
  -MaxDurationMilliseconds 5000 `
  -TraceTimeoutSeconds 240 `
  -NsightPath "D:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.1.0\host\windows-desktop-nomad-x64\ngfx-ui.exe"
```

`NsightPath` 可以传 `ngfx-ui.exe`、`ngfx.exe` 或它们所在目录。也可以配置 `NSIGHT_GRAPHICS_PATH` / `NSIGHT_GRAPHICS_BIN`，或把 `ngfx.exe`、`ngfx-capture.exe`、`ngfx-replay.exe` 放入 PATH。

## Workflow / MCP

workflow 里可以直接使用：

```json
{
  "id": "capture_performance",
  "action": "capture_performance",
  "dependsOn": ["build_engine"],
  "args": {
    "scene": "assets/baka3d.json",
    "game": "baka3d",
    "captureType": "gpu_trace",
    "captureFrame": 60,
    "frames": 180,
    "fixedDeltaSeconds": 0.016666667,
    "frameCount": 1,
    "maxDurationMilliseconds": 5000,
    "traceTimeoutSeconds": 240,
    "setGpuClocks": "unaltered"
  }
}
```

对应 MCP 工具名是 `capture_performance`。常用参数：

- `captureType`：`gpu_trace` 或 `graphics_capture`；
- `captureFrame` / `frames`：采集帧和目标总帧数，采集帧必须早于目标退出；
- `frameCount`：连续采集帧数，通常保持 1；
- `maxDurationMilliseconds`：GPU Trace 的采样窗口上限；
- `replayLoops` / `skipReplay`：Graphics Capture 的 replay 配置；
- `nsightPath`：Nsight CLI 或其目录；
- `setGpuClocks`：`unaltered`、`base` 或 `maximum`。个人项目的可比性优先，默认 `unaltered`。

示例文件：`tools/agent_workflow.nsight.example.json`、`tools/agent_task.nsight.example.json`。

## 权限和失败状态

脚本不会自动提权，也不会修改系统或 NVIDIA Control Panel 设置。GPU Trace 如果遇到 GPU performance counters 权限问题，会返回：

```json
{
  "success": false,
  "status": "permission_denied",
  "nsight": {
    "available": true,
    "isAdministrator": false
  },
  "nextAction": "以管理员身份运行 capture_performance，或开启 GPU performance counters 访问后重试"
}
```

这类结果仍然会保存 stdout/stderr 和 manifest，便于 Agent 判断是权限问题而不是引擎渲染失败。通常的解决办法是使用管理员权限启动 Nsight/Agent，或者按 NVIDIA 的 GPU counter permission 说明开启开发者访问。

## 指标语义边界

`gpu_trace` 的结构化指标包含：

- `gpuFrameTimeMs`：Nsight 导出的 GPU frame time；
- `graphicsEngineActivePct`、`smThroughputPct`、`l1texThroughputPct`、`dramThroughputPct`、`pcieThroughputPct`；
- `drawCount`、`dispatchCount`；
- `pixelShaderActiveWarpsPct`、`computeShaderActiveWarpsPct`、`inactiveSmIdleWarpsPct`。

`graphics_capture` 的 `iteration_times.csv` 会汇总 `replayTotalFps`、`replayAdjustedFps`、`msSubmitTime`、`msFinishTime`、`msFrameTime` 和 `msGpuTime`。如果 `msGpuTime=-1`，只能说明本次 replay 没有真实 GPU 时间字段；`msFrameTime` 是 replay 计时，不能直接当作应用原生 GPU frame time。

GPU Trace 有 profiling/采样开销，脚本因此明确写出 `baselineReady=false` 和 `baselineNote`。正确的比较方式是同一台机器、同一驱动、同一场景、同一采集参数下比较优化前后，并结合 RenderDoc、渲染日志和实际设备 FPS 判断。

## Evidence 输出

对 workflow 或 task 结果运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Engine project\vulkan engine\tools\agent_evidence.ps1" `
  -SourceResultPath "out\agent_runs\<run-id>\result.json"
```

`evidence.json` 新增 `performance`：

- `status`：`ready`、`permission_denied`、`failed` 或 `unavailable`；
- `captureCount`、`gpuTraceCount`、`graphicsCaptureCount`、`metricFileCount`；
- `captures[]`：每个性能步骤的类型、状态、权限、结构化指标和下一步；
- `nextAction`：给下一轮 planner 的可执行建议。

当 `performance.available=false` 时，Planner 不应根据普通日志或主观 FPS 宣称 GPU 优化完成，应优先补充 `capture_performance` 或处理权限/设备适配问题。

## 当前边界与下一步

当前覆盖 Windows + NVIDIA 桌面端，且已经验证了 `ngfx.exe` GPU Trace、自动导出和 replay 产物链路。仍需补齐：

1. 固定测试场景和相机基线，记录优化前/后成对结果；
2. 把关键 render pass / material / shader 名称与引擎内部 marker 对齐；
3. Android 设备上的安装、运行、logcat、截图和 GPU profiler 适配；
4. 将 Nsight 指标阈值变成可配置断言，而不是只提供观察值；
5. 让 planner 根据 `performance` 选择“复测、抓 RenderDoc、检查 shader 或切换移动端设备”的下一步。
