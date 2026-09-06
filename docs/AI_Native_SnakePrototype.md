# AI Native Snake 色块原型验收样例

这个样例用已有 `assets/snake.json` 和 `Sprite2D` 颜色组件完成 2D 原型，不依赖模型、纹理或真实美术资源。它验证的是 AI Native 的能力链路：资产发现、受控场景修改、插件玩法、固定输入回放、状态断言、headless Vulkan 启动和交付包。

## 运行顺序

在 `D:\Engine project\vulkan engine` 执行：

```powershell
$root = "D:\Engine project\vulkan engine"

# 1. 场景结构和资源引用
powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\validate_scene.ps1" `
  -ScenePath "$root\assets\snake.json" -CheckAssets

# 2. 编译共享运行时和测试宿主，再编译 snake 插件
powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\build.ps1" `
  -Target MikanTestRunner -ConfigureIfMissing
powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\compile_games.ps1"

# 3. AI Agent 只测试闭环：固定回放 + 状态断言
powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\agent_test.ps1" `
  -TestSpecPath "$root\tools\agent_test.snake.example.json" `
  -Mode execute -RunId "snake-agent-test-local"

# 4. GameSpec 闭环：色块场景变体 + 玩法 + Vulkan headless + delivery.zip
powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\agent_game_spec.ps1" `
  -GameSpecPath "$root\tools\agent_game_spec.snake.example.json" `
  -Mode execute -RunId "snake-gamespec-local"
```

## 验收点

- `Snake0.wpos = [96, 96, 0]`：60 帧固定步进中向右、向下各移动 3 格。
- `Food.wpos = [-384, -288, 0]`：测试钩子固定食物位置，避免随机数导致回归抖动。
- `MenuTitle.visible = false`：验证自动开始确实完成了 Menu → Playing 状态切换。
- `apply_scene_commands` 的输出场景只修改色块颜色，不覆盖源场景。
- `run_render_test` 验证 EngineMain 能加载同一份生成场景并走 headless Vulkan 路径。
- `delivery.zip` 应包含生成场景、输入回放、测试报告、`MikanTestRunner.exe`、`EngineMain.exe`、`Game.dll`、`GameSnake.dll` 及运行时 DLL。

这个样例刻意把“模型生成美术资产”排除在验收之外，先证明 AI 可以稳定完成结构化场景搭建、玩法驱动和自动测试；真实素材接入只需要替换资源清单和场景引用，不应改变测试接口。
