# AI Native 设备能力门

`get_device_capabilities` 是外部 Agent 在桌面渲染或性能测试前调用的只读上下文工具。它回答“当前机器能做什么”，不替代真正的 RenderDoc 抓帧、Nsight GPU Trace 或 Android 测试。

## MCP 调用

```json
{
  "name": "get_device_capabilities",
  "arguments": {
    "nsightPath": "D:/Program Files/NVIDIA Corporation/Nsight Graphics 2026.1.0/host/windows-desktop-nomad-x64/ngfx-ui.exe",
    "outputRoot": "out/agent_device"
  }
}
```

`renderDocPath` 可以传 `qrenderdoc.exe` 或 RenderDoc 目录；`nsightPath` 可以传 `ngfx-ui.exe` 或 Nsight host 目录；`vulkanInfoPath` 只能指向 `vulkaninfo.exe`。省略路径时工具先使用 PATH 和 Windows 常见安装位置。

## 返回内容

- `host`：Windows 版本、进程架构、64 位状态、CPU 线程数和 PowerShell 版本。
- `vulkan`：loader 是否存在、`vulkaninfo` 版本和探测状态、实例版本、GPU 名称/厂商/驱动/API 版本。
- `renderDoc`：UI 和 `renderdoccmd.exe` 是否存在；只有 `captureReady=true` 才能进入桌面 RenderDoc 证据链。
- `nsight`：UI、GPU Trace、Graphics Capture、Replay 工具及版本；`permission` 在真正采集前固定为 `not_tested`。
- `engine`：`EngineMain.exe`、`MikanTestRunner.exe`、`Game.dll` 是否存在，并记录大小和 SHA-256，便于判断是否先构建。
- `mobile`：当前为 `deferred`，不会把 Android 安装、运行、logcat 或截图能力虚报为已完成。
- `recommendations`：只给出下一步建议，不自动提权、不改驱动设置、不执行性能采集。

## 状态解释

| 状态 | 含义 |
|---|---|
| `ready_for_desktop_probe` | Vulkan 设备可以枚举，可以继续选择桌面渲染/性能工具 |
| `partial` | 能力信息不完整，先处理 `recommendations` |
| `available_not_profiled` | Nsight 工具已发现，但性能权限还没有通过真实采集验证 |
| `installed_but_capture_command_missing` | RenderDoc UI 存在，但缺少命令行捕获器，不能产出 `.rdc` |
| `unsupported_for_detected_gpu` | 工具存在，但当前枚举到的 GPU 不适用，例如没有 NVIDIA 设备时不能做 Nsight GPU Trace |
| `permission_denied` | 只应由真实性能采集工具返回，能力查询不会提前伪造这个状态 |

## 推荐顺序

```text
get_project_context
  -> get_device_capabilities
  -> inspect_scene / query_assets
  -> run_render_test
  -> capture_frame / capture_performance
  -> collect_agent_evidence
  -> evaluate_agent_result
```

设备能力是约束条件，不是交付证据。Agent 必须继续读取实际采集的 `.rdc`、`.ngfx-gputrace`、`.ngfx-capture`、指标文件和 `permission_denied` 诊断。
