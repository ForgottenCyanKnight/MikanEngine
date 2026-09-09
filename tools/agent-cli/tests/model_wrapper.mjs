// Deterministic provider fixture for the CLI smoke test.
// A real Codex/Claude/Cursor wrapper should read the prompt from stdin and
// write one GameSpec JSON object to stdout.
process.stdin.resume();
process.stdin.setEncoding("utf8");
let prompt = "";
process.stdin.on("data", (chunk) => { prompt += chunk; });
process.stdin.on("end", () => {
  const goal = prompt.includes("AI Native")
    ? "为第三人称项目生成一个可审查的 AI Native preview"
    : "由外部模型 wrapper 生成的 preview";
  process.stdout.write(JSON.stringify({
    schemaVersion: 1,
    name: "agent-cli-command-wrapper",
    description: "command provider 集成 smoke 的确定性模型响应。",
    goal,
    allowDestructive: false,
    project: { projectPath: "projects/third-person-navigation", scenePath: "scenes/main.json" },
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
