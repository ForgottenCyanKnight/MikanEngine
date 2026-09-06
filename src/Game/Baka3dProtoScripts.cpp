// Baka3dProtoScripts.cpp - baka3d 原型脚本（RotateScript）
// 2026-08-23：baka3d.json 声明的 RotateScript 实现——让 Baka 模型绕 Y 轴旋转。
// 桌面端未实现（games/baka3d/ 为空），此处直接编译进引擎（Android 无插件 DLL 机制，
// 脚本必须随引擎 .so 一起注册）。桌面端也可用（REGISTER_SCRIPT 全局静态注册）。
#include "ECS/ScriptSystem.h"
#include "ECS/ScriptContext.h"
#include "Core/Log.h"

#include <algorithm>

// ===== Unity 式脚本：RotateScript（baka3d 样例：绕 Y 轴旋转）=====
class RotateScript : public ECS::IScriptBehaviour {
public:
    const char* GetScriptName() const override { return "RotateScript"; }

    void OnStart(ECS::Entity entity) override {
        m_Context.SetSelf(entity);
        m_Elapsed = 0.0f;
        m_Context.Log("started");
    }

    void OnUpdate(float deltaTime) override {
        if (!m_Context.IsAlive()) return;
        m_Elapsed += std::max(0.0f, deltaTime);
        // speedDegPerSec 来自场景 JSON params（SCRIPT_FIELD 反射回填）
        const float deg = speedDegPerSec * m_Elapsed;
        m_Context.SetRotationEuler(glm::vec3(0.0f, deg, 0.0f));
    }

    void OnDestroy() override {
        m_Context.SetSelf(ECS::INVALID_ENTITY);
    }

    // 参数字段表：baka3d.json params {"speedDegPerSec":45.0}
    const ECS::FieldMeta* GetParamFields(int& outCount) const override {
        outCount = 1;
        return s_Fields;
    }

    float speedDegPerSec = 45.0f;   // 旋转速度°/s（场景 params 覆盖）

private:
    static const ECS::FieldMeta s_Fields[];
    ECS::ScriptContext m_Context;
    float m_Elapsed = 0.0f;
};

const ECS::FieldMeta RotateScript::s_Fields[] = {
    SCRIPT_FIELD(RotateScript, speedDegPerSec, Float, "旋转速度°/s"),
};
REGISTER_SCRIPT(RotateScript, "RotateScript");
