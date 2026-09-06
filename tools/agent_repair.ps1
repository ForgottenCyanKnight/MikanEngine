# agent_repair.ps1 - MikanEngine AI Native 受控测试修复
# ------------------------------------------------------------------
# 输入：一次失败的 agent_test result + 外部 Agent 提交的 repair spec。
# 只允许修改测试 workflow 的运行参数；禁止场景命令、脚本源码、任意命令
# 和断言期望值变更。实际重跑复用 agent_task 的候选补丁与 evidence 产物。
# 默认 preview；显式 -Mode execute 才启动构建/引擎。
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceResultPath,
    [Parameter(Mandatory = $true)][string]$RepairPath,
    [string]$OutputRoot = "out\agent_repairs",
    [string]$RunId = "",
    [string]$Mode = "",
    [int]$MaxAttempts = 0
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$script:runDir = $null
$script:resultPath = $null
$script:repairContext = $null

function Set-SystemPowerShellModulePath {
    if ([string]::IsNullOrWhiteSpace($env:SystemRoot)) { return }
    $systemRoot = $env:SystemRoot
    $programFiles = if ($env:ProgramFiles) { $env:ProgramFiles } else { "C:\Program Files" }
    $programFilesX86 = if (${env:ProgramFiles(x86)}) { ${env:ProgramFiles(x86)} } else { "C:\Program Files (x86)" }
    $inherited = @($env:PSModulePath -split [System.IO.Path]::PathSeparator | Where-Object { $_ -and $_.ToLowerInvariant().IndexOf("codex-runtimes") -lt 0 })
    $paths = @(
        (Join-Path $systemRoot "System32\WindowsPowerShell\v1.0\Modules"),
        (Join-Path $programFiles "WindowsPowerShell\Modules"),
        (Join-Path $programFilesX86 "WindowsPowerShell\Modules"),
        (Join-Path $programFiles "PowerShell\Modules")
    ) + $inherited
    $env:PSModulePath = @($paths | Where-Object { $_ } | Select-Object -Unique) -join [System.IO.Path]::PathSeparator
}

Set-SystemPowerShellModulePath

function Get-PropertyValue($Object, [string]$Name, $Default = $null) {
    if ($null -eq $Object) { return $Default }
    if ($Object -is [System.Collections.IDictionary] -and $Object.Contains($Name)) { return $Object[$Name] }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $Default }
    return $property.Value
}

function Test-Property($Object, [string]$Name) {
    if ($null -eq $Object) { return $false }
    if ($Object -is [System.Collections.IDictionary]) { return $Object.Contains($Name) }
    return ($null -ne $Object.PSObject.Properties[$Name])
}

function Read-JsonFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "JSON 文件不存在: $Path" }
    try { return [System.IO.File]::ReadAllText($Path, $utf8NoBom) | ConvertFrom-Json }
    catch { throw "JSON 解析失败: $Path；$($_.Exception.Message)" }
}

function Write-JsonFile([string]$Path, $Value) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [System.IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 80), $utf8NoBom)
}

function Resolve-ProjectPath([string]$Path, [bool]$MustExist = $false) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "路径不能为空" }
    $candidate = if ([System.IO.Path]::IsPathRooted($Path)) { $Path } else { Join-Path $root $Path }
    $full = [System.IO.Path]::GetFullPath($candidate)
    $prefix = $root.TrimEnd('\', '/') + '\'
    if (-not $full.Equals($root, [System.StringComparison]::OrdinalIgnoreCase) -and
        -not $full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "路径必须位于项目目录内: $Path"
    }
    if ($MustExist -and -not (Test-Path -LiteralPath $full -PathType Leaf)) { throw "文件不存在: $full" }
    return $full
}

function Get-RelativePath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $prefix = $root.TrimEnd('\', '/') + '\'
    if ($full.Equals($root, [System.StringComparison]::OrdinalIgnoreCase)) { return "" }
    if ($full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) { return $full.Substring($prefix.Length).Replace('\', '/') }
    return $full.Replace('\', '/')
}

function Get-Sha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "" }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $sha.Dispose()
    }
}

function Assert-KnownProperties($Object, [string[]]$Allowed, [string]$Context) {
    if ($null -eq $Object) { return }
    $unknown = @($Object.PSObject.Properties.Name | Where-Object { $_ -notin $Allowed })
    if ($unknown.Count -gt 0) { throw "$Context 含不支持字段: $($unknown -join ', ')" }
}

function Assert-Name([string]$Value, [string]$Context, [int]$MaxLength = 160) {
    if ([string]::IsNullOrWhiteSpace($Value) -or $Value.Length -gt $MaxLength -or $Value -notmatch '^[A-Za-z][A-Za-z0-9_.-]*$') { throw "$Context 不合法: $Value" }
}

function Assert-IntegerRange($Value, [string]$Context, [int]$Min, [int]$Max) {
    $number = 0
    if (-not [int]::TryParse([string]$Value, [ref]$number) -or $number -lt $Min -or $number -gt $Max) { throw "$Context 必须在 $Min..$Max" }
    return $number
}

function Assert-DoubleRange($Value, [string]$Context, [double]$MinExclusive, [double]$Max) {
    $number = [double]0
    if (-not [double]::TryParse([string]$Value, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$number) -or
        [double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -le $MinExclusive -or $number -gt $Max) { throw "$Context 必须位于 ($MinExclusive, $Max]" }
    return $number
}

function Assert-BoolValue($Value, [string]$Context) {
    if ($Value -isnot [bool]) { throw "$Context 必须是布尔值" }
}

function Test-ReservedExtra([string]$Value) {
    return $Value -match '(?i)^--(headless|headless-no-render|frames|fixed-dt|dump-state|crash-log|scene|game|screenshot-frame|screenshot-path|renderdoc-capture-frame|renderdoc-capture-path|input-replay)(=|$)'
}

function Get-Step($Steps, [string]$StepId) {
    foreach ($step in @($Steps)) { if ([string](Get-PropertyValue $step "id" "") -eq $StepId) { return $step } }
    return $null
}

function Get-RepairFields([string]$Action) {
    switch ($Action) {
        "run_gameplay_test" { return @("frames", "fixedDeltaSeconds", "timeoutMs", "extraArgs") }
        "run_render_test" { return @("frames", "fixedDeltaSeconds", "screenshotFrame", "timeoutMs", "extraArgs") }
        "capture_frame" { return @("captureFrame", "frames", "fixedDeltaSeconds", "captureTimeoutSeconds", "captureWaitSeconds", "extraArgs") }
        "capture_performance" { return @("captureFrame", "frames", "fixedDeltaSeconds", "frameCount", "maxDurationMilliseconds", "captureTimeoutSeconds", "traceTimeoutSeconds", "replayLoops", "skipReplay", "extraArgs") }
        default { return @() }
    }
}

function Validate-RepairValue($Value, [string]$Action, [string]$Field, $Step) {
    switch ($Field) {
        "frames" { [void](Assert-IntegerRange $Value "$Action.args.$Field" 1 1000000) }
        "captureFrame" { [void](Assert-IntegerRange $Value "$Action.args.$Field" 1 1000000) }
        "screenshotFrame" {
            [void](Assert-IntegerRange $Value "$Action.args.$Field" 0 1000000)
            $frames = [int](Get-PropertyValue (Get-PropertyValue $Step "args" $null) "frames" 0)
            if ($Value -gt 0 -and $frames -gt 0 -and $Value -gt $frames) { throw "$Action.args.screenshotFrame 必须不大于 frames" }
        }
        "fixedDeltaSeconds" { [void](Assert-DoubleRange $Value "$Action.args.$Field" 0.0 0.1) }
        "timeoutMs" { [void](Assert-IntegerRange $Value "$Action.args.$Field" 1000 3600000) }
        "captureTimeoutSeconds" { [void](Assert-IntegerRange $Value "$Action.args.$Field" 1 3600) }
        "captureWaitSeconds" { [void](Assert-IntegerRange $Value "$Action.args.$Field" 0 120) }
        "frameCount" { [void](Assert-IntegerRange $Value "$Action.args.$Field" 1 60) }
        "maxDurationMilliseconds" { [void](Assert-IntegerRange $Value "$Action.args.$Field" 1000 600000) }
        "traceTimeoutSeconds" { [void](Assert-IntegerRange $Value "$Action.args.$Field" 1 3600) }
        "replayLoops" { [void](Assert-IntegerRange $Value "$Action.args.$Field" 0 100) }
        "skipReplay" { Assert-BoolValue $Value "$Action.args.$Field" }
        "extraArgs" {
            if ($null -eq $Value -or $Value -is [string]) { throw "$Action.args.extraArgs 必须是字符串数组" }
            $items = @($Value)
            if ($items.Count -gt 64) { throw "$Action.args.extraArgs 不能超过 64 项" }
            foreach ($item in $items) {
                if ([string]$item.Length -gt 1024) { throw "$Action.args.extraArgs 单项不能超过 1024 字符" }
                if (Test-ReservedExtra ([string]$item)) { throw "repair 不允许覆盖 workflow 保留参数: $item" }
            }
            if ($Action -eq "run_gameplay_test") {
                $baseArgs = @((Get-PropertyValue (Get-PropertyValue $Step "args" $null) "extraArgs" @()))
                $hasReplay = @($baseArgs | Where-Object { [string]$_ -match '(?i)^--input-replay(=|$)' }).Count -gt 0
                if ($hasReplay) { throw "已有输入回放的 gameplay 不允许通过 repair 替换 extraArgs" }
            }
        }
        default { throw "repair 不支持字段: $Action.args.$Field" }
    }
}

function Validate-Candidate($Candidate, [int]$Index, $Steps) {
    if ($null -eq $Candidate -or $Candidate -is [string]) { throw "candidates[$Index] 必须是对象" }
    Assert-KnownProperties $Candidate @("id", "description", "when", "patches") "candidates[$Index]"
    $id = [string](Get-PropertyValue $Candidate "id" "")
    Assert-Name $id "candidates[$Index].id" 64
    $when = Get-PropertyValue $Candidate "when" $null
    Assert-KnownProperties $when @("stepId", "action", "failureCategory") "candidates[$Index].when"
    $patches = @((Get-PropertyValue $Candidate "patches" @()))
    if ($patches.Count -lt 1 -or $patches.Count -gt 8) { throw "candidates[$Index].patches 数量必须在 1..8" }
    $seen = @{}
    for ($patchIndex = 0; $patchIndex -lt $patches.Count; $patchIndex++) {
        $patch = $patches[$patchIndex]
        Assert-KnownProperties $patch @("op", "stepId", "path", "value") "candidates[$Index].patches[$patchIndex]"
        if ([string](Get-PropertyValue $patch "op" "set") -ne "set") { throw "patch op 只支持 set" }
        $stepId = [string](Get-PropertyValue $patch "stepId" "")
        $step = Get-Step $Steps $stepId
        if ($null -eq $step) { throw "patch 引用了未知 workflow step: $stepId" }
        $action = [string](Get-PropertyValue $step "action" "")
        $allowed = @(Get-RepairFields $action)
        if ($allowed.Count -eq 0) { throw "repair 不允许修改 action=$action" }
        $path = [string](Get-PropertyValue $patch "path" "")
        if ($path -notmatch '^args\.([A-Za-z][A-Za-z0-9_]*)$') { throw "patch.path 只能是 args.<field>" }
        $field = $Matches[1]
        if ($allowed -notcontains $field) { throw "repair 不允许修改 $action.args.$field" }
        $key = "$stepId.$field"
        if ($seen.ContainsKey($key)) { throw "同一 candidate 不能重复修改 $key" }
        $seen[$key] = $true
        if (-not (Test-Property $patch "value")) { throw "patch 缺少 value: $path" }
        Validate-RepairValue (Get-PropertyValue $patch "value" $null) $action $field $step
    }
    return $Candidate
}

function Get-ArtifactList {
    $items = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse | Where-Object { $_.Name -ne "result.json" })) {
        $items += [ordered]@{ path = Get-RelativePath $file.FullName; size = [int64]$file.Length; sha256 = Get-Sha256 $file.FullName }
    }
    return $items
}

function Write-RepairManifest {
    $manifestPath = Join-Path $script:runDir "manifest.json"
    $files = Get-ArtifactList
    Write-JsonFile $manifestPath ([ordered]@{ schemaVersion = 1; operation = "agent_repair"; runId = $script:repairContext.runId; createdAt = (Get-Date).ToString("o"); files = $files })
    $loaded = Read-JsonFile $manifestPath
    foreach ($entry in @($loaded.files)) {
        $full = Resolve-ProjectPath ([string]$entry.path) $true
        if ((Get-Sha256 $full) -ne [string]$entry.sha256) { throw "agent_repair manifest 校验失败: $($entry.path)" }
    }
    return $manifestPath
}

function Save-RepairResult([bool]$Success, [string]$ErrorText = "", $TaskResult = $null, [string]$NextAction = "") {
    if ($null -eq $script:resultPath -or $null -eq $script:repairContext) { return }
    $result = [ordered]@{
        schemaVersion = 1
        tool = "agent_repair"
        apiVersion = 1
        success = $Success
        mode = $script:repairContext.mode
        preview = ($script:repairContext.mode -eq "preview")
        name = $script:repairContext.name
        runId = $script:repairContext.runId
        runDir = Get-RelativePath $script:runDir
        sourceResultPath = Get-RelativePath $script:repairContext.sourceResultPath
        workflowPath = Get-RelativePath $script:repairContext.workflowPath
        repairPath = Get-RelativePath $script:repairContext.repairPath
        taskPath = Get-RelativePath $script:repairContext.taskPath
        taskResultPath = if ($TaskResult -and $TaskResult.resultPath) { Get-RelativePath $TaskResult.resultPath } else { $null }
        task = if ($TaskResult -and $TaskResult.result) { $TaskResult.result } else { $null }
        review = [ordered]@{
            explicitExecuteRequired = $true
            sourceSceneOrScriptWriteAllowed = $false
            mutableFields = @("run_gameplay_test.args.frames", "run_gameplay_test.args.fixedDeltaSeconds", "run_gameplay_test.args.timeoutMs", "run_render_test.args.frames", "run_render_test.args.fixedDeltaSeconds", "run_render_test.args.screenshotFrame", "run_render_test.args.timeoutMs", "capture_frame.args.captureFrame", "capture_frame.args.frames", "capture_frame.args.fixedDeltaSeconds", "capture_frame.args.captureTimeoutSeconds", "capture_frame.args.captureWaitSeconds", "capture_performance.args.captureFrame", "capture_performance.args.frames", "capture_performance.args.fixedDeltaSeconds", "capture_performance.args.frameCount", "capture_performance.args.maxDurationMilliseconds", "capture_performance.args.captureTimeoutSeconds", "capture_performance.args.traceTimeoutSeconds", "capture_performance.args.replayLoops", "capture_performance.args.skipReplay")
            assertionExpectedValuesMutable = $false
            note = "仅重跑已有 TestSpec workflow 的受控参数；不会修改场景、脚本或源码"
        }
        artifacts = Get-ArtifactList
        error = $ErrorText
        nextAction = if ($NextAction) { $NextAction } elseif ($Success -and $script:repairContext.mode -eq "preview") { "审查候选参数后，以 mode=execute 重跑" } elseif ($Success) { "读取 task/evidence 结果并保留修复证据" } else { "读取失败步骤 diagnostics 后提交新的 repair candidate" }
    }
    Write-JsonFile $script:resultPath $result
}

$fatalError = ""
$finalSuccess = $false
$taskProcess = $null
try {
    $sourceFull = Resolve-ProjectPath $SourceResultPath $true
    $source = Read-JsonFile $sourceFull
    if ([string](Get-PropertyValue $source "tool" "") -ne "agent_test") { throw "SourceResultPath 必须来自 agent_test" }
    if ([bool](Get-PropertyValue $source "success" $false)) { throw "只允许对失败的 agent_test 结果执行 repair" }
    $workflowValue = [string](Get-PropertyValue $source "workflowPath" "")
    if ([string]::IsNullOrWhiteSpace($workflowValue)) { throw "agent_test 结果缺少 workflowPath" }
    $workflowFull = Resolve-ProjectPath $workflowValue $true
    $workflowRelative = Get-RelativePath $workflowFull
    if ($workflowRelative -notmatch '^out/agent_tests/') { throw "workflowPath 必须位于 out/agent_tests/" }
    $workflow = Read-JsonFile $workflowFull
    $steps = @((Get-PropertyValue $workflow "steps" @()))
    if ($steps.Count -lt 1) { throw "基础 workflow 至少需要一个 step" }
    foreach ($step in $steps) {
        if ([string]::IsNullOrWhiteSpace([string](Get-PropertyValue $step "id" ""))) { throw "workflow step 缺少 id" }
        if ([string]::IsNullOrWhiteSpace([string](Get-PropertyValue $step "action" ""))) { throw "workflow step 缺少 action" }
    }

    $repairFull = Resolve-ProjectPath $RepairPath $true
    $repair = Read-JsonFile $repairFull
    Assert-KnownProperties $repair @("schemaVersion", "name", "description", "mode", "maxAttempts", "timeoutSeconds", "candidates") "repair"
    if ([int](Get-PropertyValue $repair "schemaVersion" 0) -ne 1) { throw "repair schemaVersion 必须为 1" }
    $name = [string](Get-PropertyValue $repair "name" "")
    Assert-Name $name "repair.name"
    $specMode = [string](Get-PropertyValue $repair "mode" "preview").ToLowerInvariant()
    if ($specMode -notin @("preview", "execute")) { throw "repair.mode 只支持 preview 或 execute" }
    if ($Mode) { $executionMode = $Mode.ToLowerInvariant() }
    elseif ($specMode -eq "execute") { throw "repair.mode=execute 需要调用方显式传入 -Mode execute" }
    else { $executionMode = "preview" }
    if ($executionMode -notin @("preview", "execute")) { throw "mode 只支持 preview 或 execute" }
    $taskMaxAttempts = Assert-IntegerRange (Get-PropertyValue $repair "maxAttempts" 2) "repair.maxAttempts" 1 5
    if ($MaxAttempts -gt 0) { $taskMaxAttempts = Assert-IntegerRange $MaxAttempts "MaxAttempts" 1 5 }
    $timeoutSeconds = Assert-IntegerRange (Get-PropertyValue $repair "timeoutSeconds" 7200) "repair.timeoutSeconds" 1 7200
    $candidateInputs = @((Get-PropertyValue $repair "candidates" @()))
    if ($candidateInputs.Count -gt 8) { throw "repair.candidates 不能超过 8 项" }
    $candidateIds = @{}
    foreach ($index in 0..($candidateInputs.Count - 1)) {
        if ($candidateInputs.Count -eq 0) { break }
        $candidate = Validate-Candidate $candidateInputs[$index] $index $steps
        $candidateId = [string](Get-PropertyValue $candidate "id" "")
        if ($candidateIds.ContainsKey($candidateId)) { throw "candidate id 重复: $candidateId" }
        $candidateIds[$candidateId] = $true
    }

    if (-not $RunId) { $RunId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8)) }
    if ($RunId -notmatch '^[A-Za-z0-9_.-]{1,32}$') { throw "RunId 只能包含字母、数字、点和短横线，长度不超过 32（避免 Windows 产物路径过深）" }
    $outputRootFull = Resolve-ProjectPath $OutputRoot $false
    $outputRootRelative = Get-RelativePath $outputRootFull
    if (-not $outputRootRelative -or $outputRootRelative -notmatch '^out(?:/|$)') { throw "OutputRoot 必须位于项目 out/ 目录下" }
    $runDir = Join-Path $outputRootFull $RunId
    if (Test-Path -LiteralPath $runDir) { throw "agent_repair run 目录已存在，为避免覆盖请换 RunId: $RunId" }
    New-Item -ItemType Directory -Path $runDir -Force | Out-Null
    $script:runDir = $runDir
    $script:resultPath = Join-Path $runDir "result.json"
    $script:repairContext = [pscustomobject][ordered]@{
        runId = $RunId; name = $name; mode = $executionMode; sourceResultPath = $sourceFull; workflowPath = $workflowFull; repairPath = $repairFull; taskPath = $null
    }
    [System.IO.File]::Copy($sourceFull, (Join-Path $runDir "source.result.json"), $true)
    [System.IO.File]::Copy($repairFull, (Join-Path $runDir "repair.input.json"), $true)

    $task = [ordered]@{
        schemaVersion = 1
        name = "agent-repair-task-$name"
        description = "只执行测试 workflow 的受控参数候选，不修改业务文件。"
        workflowPath = $workflowRelative
        mode = $executionMode
        maxAttempts = $taskMaxAttempts
        timeoutSeconds = $timeoutSeconds
        candidates = $candidateInputs
    }
    $taskPath = Join-Path $runDir "task.generated.json"
    Write-JsonFile $taskPath $task
    $script:repairContext.taskPath = $taskPath

    $powershellPath = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    if (-not (Test-Path -LiteralPath $powershellPath -PathType Leaf)) { $powershellPath = "powershell.exe" }
    $taskScript = Join-Path $PSScriptRoot "agent_task.ps1"
    $taskArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $taskScript, "-TaskPath", $taskPath, "-OutputRoot", (Join-Path $runDir "task_runs"), "-Mode", $executionMode, "-MaxAttempts", [string]$taskMaxAttempts)
    $taskOutput = (& $powershellPath @taskArgs 2>&1 | Out-String).Trim()
    $taskExitCode = [int]$LASTEXITCODE
    $taskStdoutPath = Join-Path $runDir "task_process.stdout.log"
    [System.IO.File]::WriteAllText($taskStdoutPath, $taskOutput, $utf8NoBom)
    $taskMarker = @($taskOutput -split "\r?\n" | Where-Object { $_ -like "TASK_RESULT_PATH=*" } | Select-Object -Last 1)
    $taskResultPath = if ($taskMarker.Count -gt 0) { $taskMarker[0].Substring("TASK_RESULT_PATH=".Length).Trim() } else { "" }
    $taskResult = $null
    if ($taskResultPath -and (Test-Path -LiteralPath $taskResultPath -PathType Leaf)) { $taskResult = Read-JsonFile $taskResultPath }
    $taskProcess = [pscustomobject]@{ exitCode = $taskExitCode; outputPath = $taskStdoutPath; resultPath = $taskResultPath; result = $taskResult }
    $finalSuccess = ($taskExitCode -eq 0 -and $null -ne $taskResult -and [bool](Get-PropertyValue $taskResult "success" $false))
    $manifestPath = Write-RepairManifest
    Save-RepairResult $finalSuccess "" $taskProcess
    Write-Output "REPAIR_RESULT_PATH=$script:resultPath"
    Write-Output "SUCCESS=$finalSuccess"
    if ($finalSuccess) { exit 0 } else { exit 1 }
} catch {
    $fatalError = $_.Exception.Message
    if ($null -ne $script:runDir -and $null -ne $script:repairContext) {
        try { Save-RepairResult $false $fatalError $taskProcess "修复 repair 输入或执行环境后，使用新的 RunId 重试" } catch {}
        Write-Output "REPAIR_RESULT_PATH=$script:resultPath"
    }
    Write-Output "ERROR=$fatalError"
    Write-Output "SUCCESS=False"
    exit 3
}
