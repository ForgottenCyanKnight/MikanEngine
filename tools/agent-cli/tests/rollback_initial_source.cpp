#include "ECS/ScriptContext.h"
#include "ECS/ScriptSystem.h"

class AutofixRollbackFixture final : public ECS::IScriptBehaviour {
public:
    const char* GetScriptName() const override { return "AutofixRollbackFixture"; }
    void OnStart(ECS::Entity entity) override {
        m_Context.SetSelf(entity);
        m_Context.Log("initial_fixture");
    }
    void OnUpdate(float deltaTime) override { (void)deltaTime; }
    void OnDestroy() override { m_Context.SetSelf(ECS::INVALID_ENTITY); }
    const ECS::FieldMeta* GetParamFields(int& outCount) const override {
        outCount = 0;
        return nullptr;
    }
private:
    ECS::ScriptContext m_Context;
};

REGISTER_SCRIPT(AutofixRollbackFixture, "AutofixRollbackFixture");
