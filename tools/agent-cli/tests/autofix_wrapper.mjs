// Deterministic provider fixture for the AI Native failure-feedback loop.
// The first response intentionally contains an invalid scene operation. The
// repair response replaces it with a valid, constrained scene command.
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
      name: "replace-invalid-scene-operation",
      diagnosis: "场景命令使用了不存在的操作，替换为受控的顶层 game 属性命令。",
      confidence: 0.99,
      patches: [{
        op: "set",
        path: "scene.commands",
        value: [{ op: "set_scene_property", property: "game", value: "contact2d" }],
        reason: "保留原场景和项目边界，只修复失败的命令。",
      }],
    }, null, 2));
    return;
  }
  process.stdout.write(JSON.stringify({
    schemaVersion: 1,
    name: "agent-cli-autofix-wrapper",
    description: "自动修复闭环的确定性失败输入。",
    goal: "验证 AI Native 失败反馈与自动修复闭环",
    allowDestructive: false,
    project: { projectPath: "projects/engine-samples", scenePath: "scenes/contact2d.json" },
    assets: { requiredPaths: ["scenes/contact2d.json"] },
    scene: {
      commands: [{ op: "unsupported_operation" }],
      checkAssets: true,
    },
    tests: {
      gameplay: { enabled: false },
      assertions: [],
      render: { enabled: false },
      capture: { enabled: false },
      performance: { enabled: false },
    },
    delivery: { enabled: true, packageZip: true, includeBuildArtifacts: false, includeCaptures: false },
  }, null, 2));
});
