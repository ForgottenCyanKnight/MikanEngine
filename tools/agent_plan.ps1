# agent_plan.ps1 - MikanEngine 模型计划入口
# ------------------------------------------------------------------
# 接受模型/人工生成的 plan JSON，调用受控 agent_task，再调用 agent_evidence，
# 输出一个可交给下一轮 planner 的任务结果和证据索引。它不调用模型 API，也不
# 允许 plan 直接执行 PowerShell；真正的动作仍受 agent_task/workflow 白名单约束。
# 默认 preview，必须显式 execute 才会执行任务。
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PlanPath,

    [string]$OutputRoot = "out\agent_plans",

    [string]$RunId = "",

    [string]$Mode = "",

    [int]$MaxAttempts = 0
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
$script:runDir = $null
$script:resultPath = $null
$script:startedAt = Get-Date
$script:artifacts = New-Object 'System.Collections.Generic.List[object]'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

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

function Write-JsonFile([string]$Path, $Value) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [System.IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 60), $utf8NoBom)
}

function Resolve-ProjectPath([string]$Path, [bool]$MustExist = $false) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "路径不能为空" }
    $candidate = if ([System.IO.Path]::IsPathRooted($Path)) { $Path } else { Join-Path $root $Path }
    $full = [System.IO.Path]::GetFullPath($candidate)
    $prefix = $root.TrimEnd('\', '/') + '\'
    if (-not $full.Equals($root, [System.StringComparison]::OrdinalIgnoreCase) -and -not $full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) { throw "路径必须位于项目目录内: $Path" }
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

function Add-Artifact([string]$Path, [string]$Kind = "artifact") {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    try {
        $full = Resolve-ProjectPath $Path $false
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { return $null }
        $relative = Get-RelativePath $full
        foreach ($existing in $script:artifacts.ToArray()) { if ([string]$existing.path -eq $relative) { return $existing } }
        $entry = [ordered]@{ path = $relative; kind = $Kind; size = [int64](Get-Item -LiteralPath $full).Length; sha256 = Get-Sha256 $full }
        [void]$script:artifacts.Add([pscustomobject]$entry)
        return [pscustomobject]$entry
    } catch { return $null }
}

function ConvertTo-WindowsCommandLineArg([string]$Value) {
    if ($null -eq $Value) { $Value = "" }
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($ch in $Value.ToCharArray()) {
        if ($ch -eq '\') { $slashes++; continue }
        if ($ch -eq '"') {
            if ($slashes -gt 0) { [void]$builder.Append(('\' * ($slashes * 2))) }
            [void]$builder.Append('\"')
        } else {
            if ($slashes -gt 0) { [void]$builder.Append(('\' * $slashes)) }
            [void]$builder.Append($ch)
        }
        $slashes = 0
    }
    if ($slashes -gt 0) { [void]$builder.Append(('\' * ($slashes * 2))) }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Invoke-ChildProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$LogDir,
        [Parameter(Mandatory = $true)][string]$Marker,
        [int]$TimeoutSeconds = 7200
    )
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
    $stdoutPath = Join-Path $LogDir "stdout.log"
    $stderrPath = Join-Path $LogDir "stderr.log"
    $started = Get-Date
    $stdout = ""
    $stderr = ""
    $exitCode = $null
    $timedOut = $false
    $errorText = ""
    try {
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $FilePath
        $startInfo.WorkingDirectory = $WorkingDirectory
        $startInfo.Arguments = (($ArgumentList | ForEach-Object { ConvertTo-WindowsCommandLineArg ([string]$_) }) -join " ")
        $startInfo.UseShellExecute = $false
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.CreateNoWindow = $true
        $process = [System.Diagnostics.Process]::Start($startInfo)
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            $taskkill = Join-Path $env:SystemRoot "System32\taskkill.exe"
            try { if (Test-Path -LiteralPath $taskkill -PathType Leaf) { & $taskkill /PID $process.Id /T /F 2>$null | Out-Null } else { $process.Kill() } } catch { try { $process.Kill() } catch {} }
            [void]$process.WaitForExit(5000)
        } else { [void]$process.WaitForExit() }
        if ($stdoutTask.IsCompleted) { try { $stdout = $stdoutTask.Result } catch {} }
        if ($stderrTask.IsCompleted) { try { $stderr = $stderrTask.Result } catch {} }
        if ($process.HasExited) { $exitCode = [int]$process.ExitCode }
    } catch { $errorText = $_.Exception.Message; $stderr = $stderr + "`n${FilePath}: $errorText" }
    [System.IO.File]::WriteAllText($stdoutPath, $stdout, $utf8NoBom)
    [System.IO.File]::WriteAllText($stderrPath, $stderr, $utf8NoBom)
    $markerLines = @($stdout -split "\r?\n" | Where-Object { $_.Trim().StartsWith($Marker) } | Select-Object -Last 1)
    $resultPath = $null
    if ($markerLines.Count -gt 0) { $resultPath = $markerLines[0].Trim().Substring($Marker.Length).Trim() }
    $structured = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        try { $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json } catch { $errorText = "结果 JSON 解析失败: $($_.Exception.Message)" }
    }
    return [pscustomobject][ordered]@{
        success = ($null -ne $structured -and [bool](Get-PropertyValue $structured "success" $false) -and -not $timedOut -and -not $errorText)
        exitCode = $exitCode
        timedOut = $timedOut
        durationMs = [int]((New-TimeSpan -Start $started -End (Get-Date)).TotalMilliseconds)
        stdoutPath = $stdoutPath
        stderrPath = $stderrPath
        resultPath = $resultPath
        result = $structured
        error = $errorText
    }
}

function Save-PlanResult([bool]$Success, [string]$ErrorText = "", [string]$NextAction = "") {
    if ($null -eq $script:resultPath) { return }
    $endedAt = Get-Date
    $taskResult = $script:planContext.taskProcess.result
    $evidenceResult = $script:planContext.evidenceProcess.result
    $evaluationResult = if ($null -ne $script:planContext.evaluationProcess) { $script:planContext.evaluationProcess.result } else { $null }
    $summary = [ordered]@{
        schemaVersion = 1
        tool = "agent_plan"
        apiVersion = 1
        success = $Success
        mode = [string]$script:planContext.mode
        preview = ([string]$script:planContext.mode -eq "preview")
        planName = [string]$script:planContext.planName
        goal = [string]$script:planContext.goal
        runId = [string]$script:planContext.runId
        runDir = Get-RelativePath $script:runDir
        planPath = Get-RelativePath $script:planContext.planPath
        taskResultPath = if ($script:planContext.taskProcess.resultPath) { Get-RelativePath $script:planContext.taskProcess.resultPath } else { $null }
        evidenceResultPath = if ($script:planContext.evidenceProcess.resultPath) { Get-RelativePath $script:planContext.evidenceProcess.resultPath } else { $null }
        evaluationContractPath = if ($script:planContext.evaluationContractPath) { Get-RelativePath $script:planContext.evaluationContractPath } else { $null }
        evaluationResultPath = if ($script:planContext.evaluationProcess -and $script:planContext.evaluationProcess.resultPath) { Get-RelativePath $script:planContext.evaluationProcess.resultPath } else { $null }
        startedAt = $script:startedAt.ToString("o")
        endedAt = $endedAt.ToString("o")
        durationMs = [int](($endedAt - $script:startedAt).TotalMilliseconds)
        task = if ($null -ne $taskResult) { [ordered]@{ success = [bool](Get-PropertyValue $taskResult "success" $false); tool = [string](Get-PropertyValue $taskResult "tool" ""); counts = Get-PropertyValue $taskResult "counts" $null; selectedCandidate = Get-PropertyValue $taskResult "selectedCandidate" $null; failure = Get-PropertyValue $taskResult "failure" $null } } else { [ordered]@{ success = $false; error = [string]$script:planContext.taskProcess.error } }
        evidence = if ($null -ne $evidenceResult) { [ordered]@{ success = [bool](Get-PropertyValue $evidenceResult "success" $false); targetSuccess = [bool](Get-PropertyValue (Get-PropertyValue $evidenceResult "source" $null) "targetSuccess" $false); android = [string](Get-PropertyValue (Get-PropertyValue (Get-PropertyValue $evidenceResult "platforms" $null) "android" $null) "status" ""); visualAvailable = [bool](Get-PropertyValue (Get-PropertyValue $evidenceResult "visual" $null) "available" $false) } } else { [ordered]@{ success = $false; error = [string]$script:planContext.evidenceProcess.error } }
        evaluation = if ($script:planContext.evaluationRequested) { if ($null -ne $evaluationResult) { [ordered]@{ requested = $true; success = [bool](Get-PropertyValue $evaluationResult "success" $false); checkCount = [int](Get-PropertyValue (Get-PropertyValue $evaluationResult "evaluation" $null) "checkCount" 0); failedCount = [int](Get-PropertyValue (Get-PropertyValue $evaluationResult "evaluation" $null) "failedCount" 0); status = if ([bool](Get-PropertyValue $evaluationResult "success" $false)) { "passed" } else { "failed" } } } else { [ordered]@{ requested = $true; success = $false; checkCount = 0; failedCount = 0; status = if ([string]$script:planContext.mode -eq "preview") { "planned" } else { "unavailable" } } } } else { [ordered]@{ requested = $false; success = $true; checkCount = 0; failedCount = 0; status = "not_requested" } }
        artifacts = $script:artifacts.ToArray()
        review = [ordered]@{ explicitExecuteRequired = $true; modelApiCalled = $false; note = if ($script:planContext.mode -eq "preview") { "当前只校验计划、任务和证据索引" } else { "模型输出只通过受控 task/workflow action 执行" } }
        error = $ErrorText
        nextAction = if ($NextAction) { $NextAction } elseif ($Success -and $script:planContext.mode -eq "preview") { "人工审查 plan 和 task 后，以 mode=execute 重跑" } elseif ($Success) { "agent_plan_completed" } else { "读取 evidenceResultPath 的 failures/diagnostics 后生成下一份候选 task" }
    }
    Write-JsonFile $script:resultPath $summary
}

function Write-PlanManifest {
    $manifestPath = Join-Path $script:runDir "manifest.json"
    $entries = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse | Where-Object { $_.Name -notin @("manifest.json", "result.json") })) { $entries += [ordered]@{ path = Get-RelativePath $file.FullName; size = [int64]$file.Length; sha256 = Get-Sha256 $file.FullName } }
    $manifest = [ordered]@{ schemaVersion = 1; operation = "agent_plan"; runId = [string]$script:planContext.runId; createdAt = (Get-Date).ToString("o"); files = $entries }
    Write-JsonFile $manifestPath $manifest
    foreach ($entry in @($manifest.files)) { $full = Resolve-ProjectPath ([string]$entry.path) $true; if ((Get-Sha256 $full) -ne [string]$entry.sha256) { throw "plan manifest 校验失败: $($entry.path)" } }
    return $manifestPath
}

$fatalError = ""
try {
    $planFull = Resolve-ProjectPath $PlanPath $true
    $plan = [System.IO.File]::ReadAllText($planFull, $utf8NoBom) | ConvertFrom-Json
    if ($null -eq $plan -or [int](Get-PropertyValue $plan "schemaVersion" 0) -ne 1) { throw "plan schemaVersion 必须为 1" }
    $goal = [string](Get-PropertyValue $plan "goal" "")
    if ([string]::IsNullOrWhiteSpace($goal) -or $goal.Length -gt 4000) { throw "plan.goal 必须存在且不超过 4000 字符" }
    $taskInline = Get-PropertyValue $plan "task" $null
    $taskPathValue = [string](Get-PropertyValue $plan "taskPath" "")
    if (($null -eq $taskInline) -eq ([string]::IsNullOrWhiteSpace($taskPathValue))) { throw "plan 必须且只能提供 task 或 taskPath" }
    $planMode = [string](Get-PropertyValue $plan "mode" "preview")
    if ($Mode) { $planMode = $Mode }
    if ($planMode -notin @("preview", "execute")) { throw "mode 只支持 preview 或 execute" }
    $planAttempts = [int](Get-PropertyValue $plan "maxAttempts" 0)
    if ($MaxAttempts -gt 0) { $planAttempts = $MaxAttempts }
    if ($planAttempts -ne 0 -and ($planAttempts -lt 1 -or $planAttempts -gt 5)) { throw "maxAttempts 必须在 1..5" }
    $priorEvidenceValue = [string](Get-PropertyValue $plan "evidencePath" "")
    if ($priorEvidenceValue) { $priorEvidenceFull = Resolve-ProjectPath $priorEvidenceValue $true }
    $evaluationContractPathValue = [string](Get-PropertyValue $plan "evaluationContractPath" "")
    $evaluationContractInline = Get-PropertyValue $plan "evaluationContract" $null
    if ($evaluationContractPathValue -and $null -ne $evaluationContractInline) { throw "evaluationContractPath 与 evaluationContract 只能二选一" }
    $evaluationContractFull = $null
    if ($evaluationContractPathValue) { $evaluationContractFull = Resolve-ProjectPath $evaluationContractPathValue $true }

    $outputRootFull = Resolve-ProjectPath $OutputRoot $false
    if (-not $RunId) { $RunId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8)) }
    if ($RunId -notmatch '^[A-Za-z0-9_.-]{1,80}$') { throw "RunId 只能包含字母、数字、下划线、点和短横线" }
    $runDir = Join-Path $outputRootFull $RunId
    if (Test-Path -LiteralPath $runDir) { throw "plan run 目录已存在，为避免覆盖请换 RunId: $RunId" }
    New-Item -ItemType Directory -Path $runDir -Force | Out-Null
    $script:runDir = $runDir
    $script:resultPath = Join-Path $runDir "result.json"
    $taskPath = $null
    if ($taskPathValue) { $taskPath = Resolve-ProjectPath $taskPathValue $true }
    else { $taskPath = Join-Path $runDir "task.input.json"; Write-JsonFile $taskPath $taskInline }
    [System.IO.File]::Copy($planFull, (Join-Path $runDir "plan.input.json"), $true)
    [void](Add-Artifact (Join-Path $runDir "plan.input.json") "plan_input")
    [void](Add-Artifact $taskPath "task_input")
    if ($priorEvidenceValue) { [void](Add-Artifact $priorEvidenceFull "prior_evidence") }
    if ($evaluationContractFull) { [void](Add-Artifact $evaluationContractFull "evaluation_contract") }
    $planName = [string](Get-PropertyValue $plan "name" ([System.IO.Path]::GetFileNameWithoutExtension($planFull)))
    $script:planContext = [pscustomobject][ordered]@{
        runId = $RunId; runDir = $runDir; planPath = $planFull; planName = $planName; goal = $goal; mode = $planMode; maxAttempts = $planAttempts
        taskProcess = $null; evidenceProcess = $null; evaluationProcess = $null
        evaluationRequested = ($null -ne $evaluationContractInline -or $null -ne $evaluationContractFull)
        evaluationContractPath = $evaluationContractFull
        evaluationContract = $evaluationContractInline
    }
    $powershellPath = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    if (-not (Test-Path -LiteralPath $powershellPath -PathType Leaf)) { $powershellPath = "powershell.exe" }
    $taskScript = Join-Path $PSScriptRoot "agent_task.ps1"
    $taskArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $taskScript, "-TaskPath", $taskPath, "-OutputRoot", (Join-Path $runDir "task_runs"))
    if ($planMode) { $taskArgs += @("-Mode", $planMode) }
    if ($planAttempts -gt 0) { $taskArgs += @("-MaxAttempts", [string]$planAttempts) }
    $script:planContext.taskProcess = Invoke-ChildProcess $powershellPath $taskArgs $root (Join-Path $runDir "task_process") "TASK_RESULT_PATH=" 7200
    [void](Add-Artifact $script:planContext.taskProcess.stdoutPath "task_process_stdout")
    [void](Add-Artifact $script:planContext.taskProcess.stderrPath "task_process_stderr")
    if ($script:planContext.taskProcess.resultPath) { [void](Add-Artifact $script:planContext.taskProcess.resultPath "task_result") }

    $script:planContext.evidenceProcess = [pscustomobject][ordered]@{ success = $false; resultPath = $null; result = $null; error = "未生成 task result"; stdoutPath = $null; stderrPath = $null }
    if ($script:planContext.taskProcess.resultPath) {
        $evidenceScript = Join-Path $PSScriptRoot "agent_evidence.ps1"
        $evidenceArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $evidenceScript, "-SourceResultPath", $script:planContext.taskProcess.resultPath, "-OutputRoot", (Join-Path $runDir "evidence_runs"))
        $script:planContext.evidenceProcess = Invoke-ChildProcess $powershellPath $evidenceArgs $root (Join-Path $runDir "evidence_process") "EVIDENCE_RESULT_PATH=" 7200
        [void](Add-Artifact $script:planContext.evidenceProcess.stdoutPath "evidence_process_stdout")
        [void](Add-Artifact $script:planContext.evidenceProcess.stderrPath "evidence_process_stderr")
        if ($script:planContext.evidenceProcess.resultPath) { [void](Add-Artifact $script:planContext.evidenceProcess.resultPath "evidence_result") }
    }
    if ($script:planContext.evaluationRequested) {
        $script:planContext.evaluationProcess = [pscustomobject][ordered]@{ success = $false; resultPath = $null; result = $null; error = if ($planMode -eq "preview") { "preview 不执行 evaluation" } else { "未生成 evidence result" }; stdoutPath = $null; stderrPath = $null }
        if ($planMode -eq "execute" -and $script:planContext.evidenceProcess.resultPath) {
            $evaluationScript = Join-Path $PSScriptRoot "agent_evaluate.ps1"
            $evaluationArgs = @(
                "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $evaluationScript,
                "-EvidencePath", $script:planContext.evidenceProcess.resultPath,
                "-OutputRoot", (Join-Path $runDir "evaluation_runs")
            )
            if ($evaluationContractFull) {
                $evaluationArgs += @("-ContractPath", $evaluationContractFull)
            } elseif ($null -ne $evaluationContractInline) {
                $evaluationJson = $evaluationContractInline | ConvertTo-Json -Depth 30 -Compress
                $evaluationBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes([string]$evaluationJson))
                $evaluationArgs += @("-ContractJsonBase64", $evaluationBase64)
            }
            $script:planContext.evaluationProcess = Invoke-ChildProcess (Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe") $evaluationArgs $root (Join-Path $runDir "evaluation_process") "EVALUATION_RESULT_PATH=" 7200
            [void](Add-Artifact $script:planContext.evaluationProcess.stdoutPath "evaluation_process_stdout")
            [void](Add-Artifact $script:planContext.evaluationProcess.stderrPath "evaluation_process_stderr")
            if ($script:planContext.evaluationProcess.resultPath) { [void](Add-Artifact $script:planContext.evaluationProcess.resultPath "evaluation_result") }
        }
    }
    $taskSuccess = [bool](Get-PropertyValue $script:planContext.taskProcess.result "success" $false)
    $evidenceSuccess = [bool](Get-PropertyValue $script:planContext.evidenceProcess.result "success" $false)
    $evaluationSuccess = (-not $script:planContext.evaluationRequested -or $planMode -eq "preview" -or [bool](Get-PropertyValue $script:planContext.evaluationProcess.result "success" $false))
    $finalSuccess = ($taskSuccess -and $evidenceSuccess -and $evaluationSuccess)
    $nextAction = if ($finalSuccess -and $planMode -eq "preview") { "人工审查 plan、task、evidence 和 evaluation contract 后，以 mode=execute 重跑" } elseif (-not $evaluationSuccess) { "读取 evaluationResultPath 的 failed checks，修复实现或补充证据后重试" } elseif ($finalSuccess) { "Planner 已完成任务、证据和交付门禁" } else { "读取 evidenceResultPath 的 failures/diagnostics，生成下一份候选 task" }
    $manifestPath = Write-PlanManifest
    [void](Add-Artifact $manifestPath "plan_manifest")
    Save-PlanResult $finalSuccess "" $nextAction
    Write-Output "PLAN_RESULT_PATH=$script:resultPath"
    Write-Output "SUCCESS=$finalSuccess"
    if ($finalSuccess) { exit 0 } else { exit 1 }
} catch {
    $fatalError = $_.Exception.Message
    if ($null -ne $script:runDir) {
        if ($null -eq $script:planContext) { $script:planContext = [pscustomobject][ordered]@{ runId = $RunId; runDir = $script:runDir; planPath = $PlanPath; planName = "invalid"; goal = ""; mode = if ($Mode) { $Mode } else { "preview" }; taskProcess = [pscustomobject]@{ result = $null; resultPath = $null; error = $fatalError }; evidenceProcess = [pscustomobject]@{ result = $null; resultPath = $null; error = "" } } }
        Save-PlanResult $false $fatalError "修复 plan 输入或执行环境后重试"
        Write-Output "PLAN_RESULT_PATH=$script:resultPath"
    }
    Write-Output "ERROR=$fatalError"
    Write-Output "SUCCESS=False"
    exit 3
}
