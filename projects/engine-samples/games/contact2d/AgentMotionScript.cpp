// Generated through MCP create_script: ECS::ScriptContext v1
#include "ECS/ScriptContext.h"
#include "ECS/ScriptSystem.h"

#include <algorithm>
#include <glm/glm.hpp>
#include <string>

class AgentMotionScript final : public ECS::IScriptBehaviour {
public:
    const char* GetScriptName() const override { return "AgentMotionScript"; }

    void OnStart(ECS::Entity entity) override {
        m_Context.SetSelf(entity);
        m_Context.Log("started");
        m_Context.SetPosition(m_Context.Position() + offset);
        m_Context.SetComponentField("render", "visible", "true");
        std::string visible;
        if (m_Context.GetComponentField("render", "visible", visible)) {
            m_Context.Log("render.visible=" + visible);
        }
    }

    void OnUpdate(float deltaTime) override {
        if (!m_Context.IsAlive()) return;
        const float horizontal = m_Context.Axis("MoveLeft", "MoveRight");
        if (horizontal != 0.0f) {
            glm::vec3 position = m_Context.Position();
            position.x += horizontal * speed * std::max(0.0f, deltaTime);
            m_Context.SetPosition(position);
        }
    }

    void OnDestroy() override {
        m_Context.SetSelf(ECS::INVALID_ENTITY);
    }

    const ECS::FieldMeta* GetParamFields(int& outCount) const override {
        outCount = 2;
        return s_Fields;
    }

    float speed = 2.5f;
    glm::vec3 offset = glm::vec3(1.0f, 2.0f, 3.0f);

private:
    static const ECS::FieldMeta s_Fields[];
    ECS::ScriptContext m_Context;
};

const ECS::FieldMeta AgentMotionScript::s_Fields[] = {
    SCRIPT_FIELD(AgentMotionScript, speed, Float, "移动速度"),
    SCRIPT_FIELD(AgentMotionScript, offset, Vec3, "起始偏移"),
};

REGISTER_SCRIPT(AgentMotionScript, "AgentMotionScript");
