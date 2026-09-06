# AI Native Script SDK（Phase 3）

本阶段把“Agent 可以修改场景”推进为“Agent 可以生成、挂载、测试玩法脚本”。核心不是再增加一套脚本语言，而是把现有 C++ 脚本宿主整理成一个可被模型稳定理解的窄接口。

## 目标边界

当前脚本宿主仍然是 C++ 插件和 `REGISTER_SCRIPT` 注册表。`ECS::ScriptContext` 解决的是生成代码的依赖收敛和运行时安全，不宣称已经具备完整的自主规划 Agent 或任意 C++ 沙箱。

这层 SDK 负责：

- 场景实体查询、重命名、层级和常用变换；
- 运行时创建实体、延迟销毁和脚本挂载/解绑；
- 以场景 `serializeKey` 访问组件和反射字段；
- 使用统一动作名读取键盘、触控映射和 CPU 测试输入；
- 输出带实体上下文的日志，方便从测试结果定位行为。

## 最小脚本示例

```cpp
#include "ECS/ScriptContext.h"
#include "ECS/ScriptSystem.h"

class FollowTargetScript final : public ECS::IScriptBehaviour {
public:
    const char* GetScriptName() const override { return "FollowTargetScript"; }

    void OnStart(ECS::Entity entity) override {
        m_ctx.SetSelf(entity);
        m_target = m_ctx.Find("Player");
        m_ctx.Log("started");
    }

    void OnUpdate(float dt) override {
        if (!m_ctx.IsAlive() || !m_ctx.IsAlive(m_target)) return;
        const glm::vec3 current = m_ctx.Position();
        const glm::vec3 target = m_ctx.WorldPosition(m_target);
        const glm::vec3 next = glm::mix(current, target, glm::clamp(dt * followSpeed, 0.0f, 1.0f));
        m_ctx.SetPosition(next);
    }

    void OnDestroy() override {}

    float followSpeed = 4.0f;

private:
    ECS::ScriptContext m_ctx;
    ECS::Entity m_target = ECS::INVALID_ENTITY;
};

REGISTER_SCRIPT(FollowTargetScript, "FollowTargetScript");
```

生成脚本时，Agent 不需要自己拼出 `Coordinator`、`SceneECS` 和 `InputSystem` 的单例调用。变换写入必须优先使用 `SetPosition`、`SetRotationEuler`、`SetScale`，因为这些入口会让世界矩阵缓存失效。

## 组件字段接口

组件字段使用场景协议的 `serializeKey`，不使用 MSVC 的 `typeid` 名称：

```cpp
ctx.SetComponentField("render", "visible", "false");
ctx.SetComponentField("camera", "fov", "55.0");
ctx.SetComponentField("camera", "thirdPersonTargetOffset", "[0, 1.5, 0]");

std::string fov;
if (ctx.GetComponentField("camera", "fov", fov)) {
    ctx.Log("camera fov=" + fov);
}
```

`SetComponentField` 适合通用玩法参数和 Agent 生成逻辑。涉及变换、脚本自身生命周期或需要物理系统重建的操作，应使用专用 API、场景命令或显式的系统接口，不要直接拿裸组件指针改内存。

## 生命周期语义

- `OnStart` 在脚本实例创建后调用一次；场景加载和 `AttachScript` 都遵守这一点。
- `OnUpdate` 使用引擎传入的秒级 `deltaTime`；玩法测试可以使用固定时间步。
- `Destroy` 不在当前 `OnUpdate` 中立即擦除实例表，而是排队到 tick 结束，先执行 `OnDestroy` 再销毁实体。
- 在 `OnUpdate` 中调用 `AttachScript` 或 `DetachScript` 也会延迟到当前 tick 结束，避免遍历实例表时迭代器失效。
- `OnDestroy` 应释放脚本自己持有的资源，不应继续使用已被销毁的实体。

## Agent 代码入口

### PowerShell

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/script_scaffold.ps1 `
  -ScriptName FollowTargetScript `
  -OutputPath games/cesiumwalk/FollowTargetScript.cpp `
  -FieldsJson '[{"name":"followSpeed","type":"float","label":"跟随速度","default":4.0}]'
```

脚本生成器具备以下约束：

- 只能写入 `games/<plugin>/` 或 `projects/<project>/games/`；
- 默认拒绝覆盖已有文件；`-Force` 会先在 `out/script_backups/` 保存并校验备份；
- 源码最多 200000 字符；必须包含 `IScriptBehaviour` 和 `REGISTER_SCRIPT(...)`；
- 限制 include 白名单并拒绝常见进程启动、命令执行和本地文件系统入口；
- 返回脚本路径、字段数量、字节数和 SHA-256，便于 Agent 记录交付物。

### MCP

MCP 新增 `create_script`：

1. 无 `source` 时生成标准模板；
2. 有 `source` 时按同一套路径/include/危险调用规则校验后写入；
3. `compile=true` 时编译当前 `games/` 插件；
4. 返回 `structuredContent`，包含源码哈希、备份路径和编译结果。

它和 `apply_scene_commands`、`run_gameplay_test` 配合后，最小闭环是：

```text
create_script
  -> compile plugin
  -> apply_scene_commands（挂载 script 组件）
  -> run_gameplay_test
  -> read_dump / assert_state
  -> run_render_test（需要视觉验收时）
```

## 验收证据

本阶段至少保留以下证据：

- `tools/script_sdk.json`：机器可读的 SDK 合约；
- `tools/script_scaffold.ps1`：受控代码生成入口；
- `games/contact2d/AgentMotionScript.cpp`：实际生成并可参与插件编译的样例；
- `ScriptContext` 编译集成和 `RotateScript` 重构：证明旧脚本可以迁移到 SDK；
- MCP `tools/list` 中的 `create_script` 与一次成功 `tools/call` 记录；
- 玩法测试的固定时间步、状态 dump 和断言结果。

## 对外表达

准确表述：

> 设计面向 Agent 生成代码的 C++ Gameplay Script SDK，将实体查询、变换、层级、组件反射、输入和生命周期封装为稳定接口；通过受控脚本生成器和 MCP 工具完成脚本生成、插件编译、场景挂载和确定性测试。

暂不表述为：

> AI 已经可以无限制地自主编写并交付任意游戏。

后续阶段再补 Agent 编排、脚本语义评估、资产工具和跨平台设备执行，才能形成更完整的自动交付闭环。
