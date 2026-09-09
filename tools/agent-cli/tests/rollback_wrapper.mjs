function source(marker) {
  return [
    '#include "ECS/ScriptContext.h"',
    '#include "ECS/ScriptSystem.h"',
    '',
    'class AutofixRollbackFixture final : public ECS::IScriptBehaviour {',
    'public:',
    '    const char* GetScriptName() const override { return "AutofixRollbackFixture"; }',
    '    void OnStart(ECS::Entity entity) override {',
    '        m_Context.SetSelf(entity);',
    `        m_Context.Log("${marker}");`,
    '    }',
    '    void OnUpdate(float deltaTime) override { (void)deltaTime; }',
    '    void OnDestroy() override { m_Context.SetSelf(ECS::INVALID_ENTITY); }',
    '    const ECS::FieldMeta* GetParamFields(int& outCount) const override {',
    '        outCount = 0;',
    '        return nullptr;',
    '    }',
    'private:',
    '    ECS::ScriptContext m_Context;',
    '};',
    '',
    'REGISTER_SCRIPT(AutofixRollbackFixture, "AutofixRollbackFixture");',
  ].join("\n");
}

process.stdin.resume();
process.stdin.setEncoding("utf8");
let prompt = "";
process.stdin.on("data", (chunk) => { prompt += chunk; });
process.stdin.on("end", () => {
  if (prompt.includes("# MikanEngine AI Native Repair Planner")) {
    process.stdout.write(JSON.stringify({
      schemaVersion: 1,
      kind: "GameSpecRepair",
      action: "repair",
      name: "change-fixture-source",
      diagnosis: "故意修改脚本源码但保留原断言失败，用于验证失败后的代码回滚。",
      confidence: 0.99,
      patches: [{
        op: "set",
        path: "scripts[0].source",
        value: source("repair_fixture"),
      }],
    }, null, 2));
    return;
  }

  process.stdout.write(JSON.stringify({
    schemaVersion: 1,
    name: "agent-cli-rollback-wrapper",
    description: "自动修复失败回滚的确定性夹具。",
    goal: "验证代码修复失败后恢复到修复前源码。",
    allowDestructive: false,
    project: { projectPath: "projects/third-person-navigation", scenePath: "scenes/main.json" },
    assets: { requiredPaths: ["scenes/main.json"] },
    scripts: [{
      scriptName: "AutofixRollbackFixture",
      outputPath: "projects/third-person-navigation/games/cesiumwalk/AgentAutofixRollbackFixture.cpp",
      sourcePath: "tools/agent-cli/tests/rollback_initial_source.cpp",
    }],
    tests: {
      gameplay: { enabled: true, frames: 1, fixedDeltaSeconds: 0.016666667, timeoutMs: 120000 },
      assertions: [{ entityName: "Ground", field: "pos", expected: [0, -0.5, 0], tolerance: 0.001 }],
      render: { enabled: false },
      capture: { enabled: false },
      performance: { enabled: false },
    },
    delivery: { enabled: false, packageZip: false, includeBuildArtifacts: false, includeCaptures: false },
  }, null, 2));
});
