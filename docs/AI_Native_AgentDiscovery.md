# AI Native Agent Discovery

`agent_discovery.ps1` 是 Agent 进入引擎工作流前的只读上下文层。它解决的不是“让模型多看几个文件”，而是把项目的能力边界、场景数据和资产索引收敛成有版本、有大小、有 SHA-256 的机器可读输入。

## 可调用入口

### `inspect_project`

返回：

- 项目登记信息和存在性；
- CMake configure/build preset、默认构建产物和玩法测试入口；
- `tools/scene_schema.json` 的组件键、分类和字段数量；
- 场景、脚本、Shader、模型、纹理、音频和游戏插件的受限索引；
- 当前 workflow action、MCP tool 和 AI Native 闭环能力清单。

### `inspect_scene`

返回：

- 场景文件大小和 SHA-256；
- 实体数量、层级深度、实体名称、父节点和组件列表；
- 组件计数、脚本名称和资源引用；
- 结构摘要级校验结果，以及未知组件键提示。

它是只读摘要，不替代严格的 `validate_scene`。Agent 应先用它理解场景，再根据 `validation.nextAction` 决定是否调用严格校验或 `apply_scene_commands`。

### `query_assets`

按路径/名称关键词和 `scenes`、`scripts`、`shaders`、`models`、`textures`、`audio` 类型查询资产，返回路径、大小、修改时间和 SHA-256。扫描范围明确排除 `out`、`backup`、第三方依赖、构建产物和 Android/HarmonyOS 目录，避免把生成物或移动端历史目录混入 Planner 上下文。

### `get_device_capabilities`

返回 Windows 桌面能力门：Vulkan loader、`vulkaninfo` 版本、实例版本和可枚举 GPU；RenderDoc/Nsight 工具与引擎构建产物的状态；以及 `permission=not_tested`、`mobile=deferred` 等保守状态。

它不提权、不修改驱动、不执行性能采集；只有真实的 `capture_performance` 结果才能判定 `permission_denied` 或性能通过。

## 与工作流的关系

MCP 工具和 `agent_workflow.ps1` 共用同一个 discovery 脚本：

```text
inspect_project / get_project_context / get_device_capabilities / inspect_scene / query_assets
                    ↓
        受限、结构化 discovery context
                    ↓
create_script / apply_scene_commands / build / validate_scene
                    ↓
run_gameplay_test / run_render_test / capture_frame / capture_performance
                    ↓
              agent_evidence
```

工作流示例见 [`tools/agent_workflow.discovery.example.json`](../tools/agent_workflow.discovery.example.json)。执行后，每次调用会在 `out/agent_discovery/<run-id>/` 保存 `request.json`、`result.json` 和 `manifest.json`。`result.json` 通过 `resultPath` 保留完整上下文；workflow step 只携带受限的 `context`，避免把原始项目文件交给模型。

## 当前边界

- 这是模型无关的 Planner 输入契约，不在本地调用 OpenAI、Claude 或 Cursor；
- 发现层不修改场景、源码、Shader 或资产，不自动执行引擎；
- 项目化任务通过 `projectPath` 选择资源根；设备能力查询只给出约束，不替代 RenderDoc/Nsight 真实证据；
- 场景是结构摘要，不是完整 JSON 的无限展开；通过 `maxEntities` 和 `maxAssetReferences` 控制上下文大小；
- 资产查询是路径/类型索引，不等同于语义向量搜索或导入器；
- 移动端执行链路本阶段暂缓，桌面端构建、玩法/渲染测试、RenderDoc 和 Nsight 仍由后续受控 action 负责。

## 下一步

discovery context 已经接入 `evaluate_agent_result`。项目化任务还可以先调用 `get_project_context` 和 `get_device_capabilities`，再让 Agent 提交“目标、允许修改范围、性能/行为阈值和验收断言”，把 `inspect_* → generate/patch → build → test → evidence → evaluate` 变成可判定的成功/失败，而不是只返回自然语言摘要。
