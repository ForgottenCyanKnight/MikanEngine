import { strict as assert } from "node:assert";
import * as fs from "node:fs/promises";
import * as path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const testDirectory = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(testDirectory, "..", "..", "..");
const cliPath = path.join(projectRoot, "tools", "agent-cli", "src", "cli.ts");
const runId = `cli-smoke-${Date.now()}`;

function runCli(args: string[]): Promise<{ code: number | null; stdout: string; stderr: string }> {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, ["--experimental-strip-types", cliPath, ...args], {
      cwd: projectRoot,
      stdio: ["ignore", "pipe", "pipe"],
      windowsHide: true,
    });
    let stdout = "";
    let stderr = "";
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => { stdout += chunk; });
    child.stderr.on("data", (chunk: string) => { stderr += chunk; });
    child.on("error", reject);
    child.on("close", (code: number | null) => resolve({ code, stdout, stderr }));
  });
}

const result = await runCli([
  "preview",
  "--scene", "assets/contact2d.json",
  "--goal", "为 Contact2D 生成一个可审查的 AI Native preview",
  "--provider", "mock",
  "--run-id", runId,
  "--json",
]);
assert.equal(result.code, 0, result.stderr || result.stdout);
const summary = JSON.parse(result.stdout);
assert.equal(summary.success, true);
assert.equal(summary.mode, "preview");
assert.equal(summary.provider, "mock");
assert.ok(summary.discoveryPath);
assert.ok(summary.promptPath);
assert.ok(summary.specPath);
assert.ok(summary.gameSpecResultPath);
await fs.access(path.join(projectRoot, summary.discoveryPath));
await fs.access(path.join(projectRoot, summary.promptPath));
await fs.access(path.join(projectRoot, summary.specPath));
await fs.access(path.join(projectRoot, summary.gameSpecResultPath));
console.log(`agent-cli smoke passed runId=${runId}`);

const projectRunId = `cli-project-context-${Date.now()}`;
const projectResult = await runCli([
  "discover",
  "--project-path", "projects/third-person-combat",
  "--scene", "scenes/main.json",
  "--query", "base color",
  "--asset-type", "textures",
  "--run-id", projectRunId,
  "--json",
]);
assert.equal(projectResult.code, 0, projectResult.stderr || projectResult.stdout);
const projectSummary = JSON.parse(projectResult.stdout);
assert.equal(projectSummary.success, true);
assert.equal(projectSummary.projectPath, "projects/third-person-combat");
const projectDiscovery = JSON.parse(await fs.readFile(path.join(projectRoot, projectSummary.discoveryPath), "utf8"));
assert.equal(projectDiscovery.projectPath, "projects/third-person-combat");
assert.equal(projectDiscovery.projectContext.projectContext.projectPath, "projects/third-person-combat");
assert.equal(projectDiscovery.projectContext.projectContext.assetIndex.items[0].semanticRole, "material_base_color");
console.log(`agent-cli project context passed runId=${projectRunId}`);

const commandRunId = `cli-command-smoke-${Date.now()}`;
const wrapperPath = path.join(projectRoot, "tools", "agent-cli", "tests", "model_wrapper.mjs");
const commandResult = await runCli([
  "preview",
  "--scene", "assets/contact2d.json",
  "--goal", "由 command provider 生成一个可审查的 AI Native preview",
  "--provider", "command",
  "--model-command", process.execPath,
  "--model-arg", wrapperPath,
  "--run-id", commandRunId,
  "--json",
]);
assert.equal(commandResult.code, 0, commandResult.stderr || commandResult.stdout);
const commandSummary = JSON.parse(commandResult.stdout);
assert.equal(commandSummary.success, true);
assert.match(commandSummary.provider, /^command:/);
await fs.access(path.join(projectRoot, commandSummary.specPath));
console.log(`agent-cli command provider passed runId=${commandRunId}`);

const autofixRunId = `cli-autofix-${Date.now()}`;
const autofixWrapperPath = path.join(projectRoot, "tools", "agent-cli", "tests", "autofix_wrapper.mjs");
const autofixResult = await runCli([
  "execute",
  "--scene", "assets/contact2d.json",
  "--goal", "验证 AI Native 失败反馈与自动修复闭环",
  "--provider", "command",
  "--model-command", process.execPath,
  "--model-arg", autofixWrapperPath,
  "--auto-repair",
  "--max-attempts", "2",
  "--run-id", autofixRunId,
  "--yes",
  "--json",
]);
assert.equal(autofixResult.code, 0, autofixResult.stderr || autofixResult.stdout);
const autofixSummary = JSON.parse(autofixResult.stdout);
assert.equal(autofixSummary.success, true);
assert.equal(autofixSummary.autoRepair.enabled, true);
assert.equal(autofixSummary.attempts.length, 2);
assert.equal(autofixSummary.attempts[0].success, false);
assert.equal(autofixSummary.attempts[1].success, true);
assert.equal(autofixSummary.repairRounds.length, 1);
assert.equal(autofixSummary.repairRounds[0].status, "applied");
assert.equal(autofixSummary.repairRounds[0].patchCount, 1);
assert.ok(autofixSummary.repairRounds[0].repairPath);
assert.equal(autofixSummary.finalSpecPath, autofixSummary.attempts[1].specPath);
await fs.access(path.join(projectRoot, autofixSummary.repairRounds[0].repairPath));
await fs.access(path.join(projectRoot, autofixSummary.attempts[1].specPath));
await fs.access(path.join(projectRoot, autofixSummary.attempts[1].mcpResultPath));
const repairPrompt = await fs.readFile(path.join(projectRoot, autofixSummary.repairRounds[0].promptPath), "utf8");
assert.match(repairPrompt, /MikanEngine AI Native Repair Planner/);
assert.match(repairPrompt, /failure/);
const autofixProgress = JSON.parse(await fs.readFile(path.join(projectRoot, autofixSummary.runDir, "autofix.progress.json"), "utf8"));
assert.equal(autofixProgress.success, true);
assert.equal(autofixProgress.rollback.status, "not_needed");
console.log(`agent-cli autofix loop passed runId=${autofixRunId}`);

const noProgressRunId = `cli-autofix-no-progress-${Date.now()}`;
const noProgressRepairPath = path.join(projectRoot, "tools", "agent-cli", "tests", "no_progress_repair.json");
const noProgressResult = await runCli([
  "execute",
  "--scene", "assets/contact2d.json",
  "--goal", "验证重复 RepairSpec 会停止而不是无界重试",
  "--provider", "command",
  "--model-command", process.execPath,
  "--model-arg", autofixWrapperPath,
  "--repair-response-file", noProgressRepairPath,
  "--auto-repair",
  "--max-attempts", "3",
  "--run-id", noProgressRunId,
  "--yes",
  "--json",
]);
assert.equal(noProgressResult.code, 1, noProgressResult.stderr || noProgressResult.stdout);
const noProgressSummary = JSON.parse(noProgressResult.stdout);
assert.equal(noProgressSummary.success, false);
assert.equal(noProgressSummary.repairRounds.length, 2);
assert.equal(noProgressSummary.repairRounds[0].status, "applied");
assert.equal(noProgressSummary.repairRounds[1].status, "no_progress");
assert.match(noProgressSummary.autoRepair.stopReason, /重复提交相同补丁/);
const noProgressState = JSON.parse(await fs.readFile(path.join(projectRoot, noProgressSummary.runDir, "autofix.progress.json"), "utf8"));
assert.equal(noProgressState.success, false);
assert.equal(noProgressState.rollback.status, "not_needed");
console.log(`agent-cli no-progress guard passed runId=${noProgressRunId}`);

const rollbackFixturePath = path.join(projectRoot, "games", "contact2d", "AgentAutofixRollbackFixture.cpp");
assert.equal(await fs.stat(rollbackFixturePath).then(() => true).catch(() => false), false);
const rollbackRunId = `cli-autofix-rollback-${Date.now()}`;
const rollbackWrapperPath = path.join(projectRoot, "tools", "agent-cli", "tests", "rollback_wrapper.mjs");
const rollbackResult = await runCli([
  "execute",
  "--scene", "assets/contact2d.json",
  "--goal", "验证失败后的自动修复代码会回滚",
  "--provider", "command",
  "--model-command", process.execPath,
  "--model-arg", rollbackWrapperPath,
  "--auto-repair",
  "--max-attempts", "2",
  "--run-id", rollbackRunId,
  "--yes",
  "--json",
]);
assert.equal(rollbackResult.code, 1, rollbackResult.stderr || rollbackResult.stdout);
const rollbackSummary = JSON.parse(rollbackResult.stdout);
assert.equal(rollbackSummary.success, false);
assert.equal(rollbackSummary.attempts.length, 2);
assert.equal(rollbackSummary.repairRounds.length, 1);
assert.equal(rollbackSummary.repairRounds[0].status, "applied");
assert.equal(rollbackSummary.autoRepair.rollback.status, "restored");
const rollbackSource = await fs.readFile(rollbackFixturePath, "utf8");
assert.match(rollbackSource, /initial_fixture/);
assert.doesNotMatch(rollbackSource, /repair_fixture/);
await fs.rm(rollbackFixturePath, { force: true });
console.log(`agent-cli rollback passed runId=${rollbackRunId}`);

const deniedRepairRunId = `cli-autofix-denied-${Date.now()}`;
const deniedRepairResult = await runCli([
  "execute",
  "--scene", "assets/contact2d.json",
  "--goal", "验证 RepairSpec 不得改变场景边界",
  "--provider", "command",
  "--model-command", process.execPath,
  "--model-arg", autofixWrapperPath,
  "--repair-response-file", path.join("tools", "agent-cli", "tests", "invalid_repair.json"),
  "--auto-repair",
  "--max-attempts", "2",
  "--run-id", deniedRepairRunId,
  "--yes",
  "--json",
]);
assert.equal(deniedRepairResult.code, 1, deniedRepairResult.stderr || deniedRepairResult.stdout);
const deniedRepairSummary = JSON.parse(deniedRepairResult.stdout);
assert.equal(deniedRepairSummary.success, false);
assert.equal(deniedRepairSummary.attempts.length, 1);
assert.equal(deniedRepairSummary.repairRounds[0].status, "invalid");
assert.match(deniedRepairSummary.autoRepair.stopReason, /安全校验/);
console.log("agent-cli repair safety guard passed");

const denied = await runCli([
  "execute",
  "--scene", "assets/contact2d.json",
  "--goal", "execute safety guard",
  "--provider", "mock",
  "--json",
]);
assert.equal(denied.code, 1);
const deniedSummary = JSON.parse(denied.stdout);
assert.equal(deniedSummary.success, false);
assert.match(deniedSummary.error, /--yes/);
console.log("agent-cli execute safety guard passed");
