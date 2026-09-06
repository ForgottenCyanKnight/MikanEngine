# agent_task.ps1 - MikanEngine Agent 任务执行与候选修复闭环
# ------------------------------------------------------------------
# 在 agent_workflow.ps1 之上提供一层“失败诊断 -> 候选补丁 -> 受限重跑”。
# 候选补丁是模型或人工提交的结构化数据，不是任意 PowerShell；真正的修改和
# 验证仍然由已有 workflow action 完成。默认 preview，必须显式 execute 才会跑。
#
# 每次执行写入 out\agent_tasks\<run-id>\：
#   task.input.json / attempt-*/ / result.json / manifest.json
#
# 退出码：0=任务成功或 preview 计划有效，1=候选耗尽/步骤失败，3=任务输入或环境错误。
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$TaskPath,

    [string]$OutputRoot = "out\agent_tasks",

    [string]$RunId = "",

    [string]$Mode = "",

    [int]$MaxAttempts = 0
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
$script:runDir = $null
$script:resultPath = $null
$script:taskContext = $null
$script:attemptRecords = New-Object 'System.Collections.Generic.List[object]'
$script:artifacts = New-Object 'System.Collections.Generic.List[object]'
$script:startedAt = Get-Date
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Get-PropertyValue($Object, [string]$Name, $Default = $null) {
    if ($null -eq $Object) { return $Default }
    if ($Object -is [System.Collections.IDictionary] -and $Object.Contains($Name)) {
        return $Object[$Name]
    }
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
    $json = $Value | ConvertTo-Json -Depth 60
    [System.IO.File]::WriteAllText($Path, $json, $utf8NoBom)
}

function Get-RelativePath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $prefix = $root.TrimEnd('\', '/') + '\'
    if ($full.Equals($root, [System.StringComparison]::OrdinalIgnoreCase)) { return "" }
    if ($full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($prefix.Length).Replace('\', '/')
    }
    return $full.Replace('\', '/')
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
    if ($MustExist -and -not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw "文件不存在: $full"
    }
    return $full
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

function Add-Artifact([string]$Path, [string]$Kind = "file") {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    try {
        $full = [System.IO.Path]::GetFullPath($Path)
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { return $null }
        $relative = Get-RelativePath $full
        foreach ($existing in $script:artifacts.ToArray()) {
            if ([string]$existing.path -eq $relative) { return $existing }
        }
        $item = Get-Item -LiteralPath $full
        $entry = [ordered]@{
            path = $relative
            kind = $Kind
            size = [int64]$item.Length
            sha256 = Get-Sha256 $full
        }
        [void]$script:artifacts.Add([pscustomobject]$entry)
        return [pscustomobject]$entry
    } catch { return $null }
}

function Deep-Copy($Value) {
    if ($null -eq $Value) { return $null }
    return ($Value | ConvertTo-Json -Depth 60 | ConvertFrom-Json)
}

function Set-PropertyValue($Object, [string]$Name, $Value) {
    if ($Object -is [System.Collections.IDictionary]) {
        $Object[$Name] = $Value
        return
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) {
        $property.Value = $Value
    } else {
        Add-Member -InputObject $Object -MemberType NoteProperty -Name $Name -Value $Value
    }
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

function Get-AllowedArgumentFields([string]$Action) {
    switch ($Action) {
        "inspect_project" { return @("projectPath", "query", "assetType", "maxResults", "outputRoot") }
        "get_project_context" { return @("projectPath", "query", "assetType", "maxResults", "outputRoot") }
        "inspect_scene" { return @("projectPath", "scene", "scenePath", "maxResults", "maxEntities", "maxAssetReferences", "outputRoot") }
        "query_assets" { return @("projectPath", "query", "assetType", "maxResults", "outputRoot") }
        "create_script" { return @("scriptName", "className", "outputPath", "fields", "source", "sourcePath", "compile") }
        "build" { return @("target", "killEngine", "cleanFirst", "configureIfMissing") }
        "compile_games" { return @() }
        "validate_scene" { return @("projectPath", "scenePath", "checkAssets") }
        "apply_scene_commands" { return @("scene", "commands", "commandsPath", "outputPath", "checkAssets", "testLayer", "skipTestBuild", "testTimeoutSeconds", "buildTimeoutSeconds") }
        "run_gameplay_test" { return @("projectPath", "scene", "game", "frames", "fixedDeltaSeconds", "dumpStatePath", "overwriteDump", "extraArgs", "timeoutMs") }
        "run_render_test" { return @("projectPath", "scene", "game", "frames", "fixedDeltaSeconds", "screenshotFrame", "dumpStatePath", "overwriteDump", "extraArgs", "timeoutMs") }
        "capture_frame" { return @("projectPath", "scene", "game", "captureFrame", "frames", "fixedDeltaSeconds", "captureTimeoutSeconds", "captureWaitSeconds", "renderDocCmdPath", "outputRoot", "extraArgs", "apiValidation", "captureCallstacks", "skipThumbnail") }
        "capture_performance" { return @("projectPath", "scene", "game", "captureType", "captureFrame", "frames", "fixedDeltaSeconds", "frameCount", "maxDurationMilliseconds", "captureTimeoutSeconds", "traceTimeoutSeconds", "replayLoops", "skipReplay", "nsightPath", "setGpuClocks", "outputRoot", "extraArgs") }
        "read_dump" { return @("path") }
        "stop_engine" { return @() }
        default { return @() }
    }
}

function Get-StepById($Steps, [string]$StepId) {
    foreach ($step in @($Steps)) {
        if ([string](Get-PropertyValue $step "id" "") -eq $StepId) { return $step }
    }
    return $null
}

function Get-StepAction($Steps, [string]$StepId) {
    $step = Get-StepById $Steps $StepId
    if ($null -eq $step) { return "" }
    return [string](Get-PropertyValue $step "action" "")
}

function Validate-CandidatePatch($Patch, [int]$CandidateIndex, [int]$PatchIndex, $Steps) {
    if ($null -eq $Patch) { throw "candidate[$CandidateIndex].patches[$PatchIndex] 不能为空" }
    $operation = [string](Get-PropertyValue $Patch "op" "set")
    if ($operation -ne "set") { throw "candidate[$CandidateIndex].patches[$PatchIndex].op 只支持 set" }
    $stepId = [string](Get-PropertyValue $Patch "stepId" "")
    if (-not $stepId) { throw "candidate[$CandidateIndex].patches[$PatchIndex] 缺少 stepId" }
    $step = Get-StepById $Steps $stepId
    if ($null -eq $step) { throw "candidate[$CandidateIndex].patches[$PatchIndex] 引用了未知步骤: $stepId" }
    $path = [string](Get-PropertyValue $Patch "path" "")
    if ($path -notmatch '^args\.([A-Za-z][A-Za-z0-9_]*)$') {
        throw "candidate[$CandidateIndex].patches[$PatchIndex].path 只能是 args.<field>，禁止任意 JSON 路径: $path"
    }
    $field = $Matches[1]
    $action = [string](Get-PropertyValue $step "action" "")
    $allowed = @(Get-AllowedArgumentFields $action)
    if ($allowed -notcontains $field) {
        throw "candidate[$CandidateIndex].patches[$PatchIndex] 不允许修改 $action.args.$field；允许字段: $($allowed -join ', ')"
    }
    if (-not (Test-Property $Patch "value")) { throw "candidate[$CandidateIndex].patches[$PatchIndex] 缺少 value" }
    return [pscustomobject][ordered]@{ stepId = $stepId; action = $action; path = $path; field = $field; op = $operation }
}

function Validate-Candidate($Candidate, [int]$CandidateIndex, $Steps, $KnownActions) {
    if ($null -eq $Candidate) { throw "candidate[$CandidateIndex] 不能为空" }
    $id = [string](Get-PropertyValue $Candidate "id" "")
    if ($id -notmatch '^[A-Za-z][A-Za-z0-9_.-]{0,63}$') { throw "candidate[$CandidateIndex].id 不合法: $id" }
    $description = [string](Get-PropertyValue $Candidate "description" "")
    if ($description.Length -gt 1000) { throw "candidate[$CandidateIndex].description 过长" }
    $when = Get-PropertyValue $Candidate "when" $null
    foreach ($field in @("stepId", "action", "failureCategory")) {
        if (-not (Test-Property $when $field)) { continue }
        $value = [string](Get-PropertyValue $when $field "")
        if (-not $value) { throw "candidate[$CandidateIndex].when.$field 不能为空" }
        if ($field -eq "stepId" -and $null -eq (Get-StepById $Steps $value)) { throw "candidate[$CandidateIndex].when.stepId 未知: $value" }
        if ($field -eq "action" -and $KnownActions -notcontains $value) { throw "candidate[$CandidateIndex].when.action 不支持: $value" }
    }
    $patches = @((Get-PropertyValue $Candidate "patches" @()))
    if ($patches.Count -lt 1 -or $patches.Count -gt 16) { throw "candidate[$CandidateIndex].patches 数量必须在 1..16" }
    $validated = New-Object 'System.Collections.Generic.List[object]'
    for ($index = 0; $index -lt $patches.Count; $index++) {
        [void]$validated.Add((Validate-CandidatePatch $patches[$index] $CandidateIndex $index $Steps))
    }
    return [pscustomobject][ordered]@{ id = $id; description = $description; when = $when; patches = $patches; validatedPatches = $validated.ToArray() }
}

function Test-CandidateMatch($When, $Signal) {
    if ($null -eq $When) { return $true }
    if ($null -eq $Signal) { return $false }
    foreach ($field in @("stepId", "action", "failureCategory")) {
        if (Test-Property $When $field) {
            if ([string](Get-PropertyValue $When $field "") -ne [string](Get-PropertyValue $Signal $field "")) { return $false }
        }
    }
    return $true
}

function Apply-Candidate($Workflow, $Candidate) {
    $patched = Deep-Copy $Workflow
    $steps = @((Get-PropertyValue $patched "steps" @()))
    foreach ($patch in @((Get-PropertyValue $Candidate "patches" @()))) {
        $stepId = [string](Get-PropertyValue $patch "stepId" "")
        $field = ([string](Get-PropertyValue $patch "path" "")).Substring(5)
        $step = Get-StepById $steps $stepId
        $args = Get-PropertyValue $step "args" $null
        if ($null -eq $args) { $args = [pscustomobject]@{} }
        Set-PropertyValue $args $field (Deep-Copy (Get-PropertyValue $patch "value" $null))
        Set-PropertyValue $step "args" $args
    }
    Set-PropertyValue $patched "steps" $steps
    return $patched
}

function Get-FailureSignal($WorkflowResult, $ProcessResult) {
    $failedSteps = @()
    if ($null -ne $WorkflowResult) {
        $failedSteps = @((Get-PropertyValue $WorkflowResult "steps" @()) | Where-Object { [string](Get-PropertyValue $_ "status" "") -eq "failed" })
    }
    if ($failedSteps.Count -gt 0) {
        $step = $failedSteps[0]
        return [ordered]@{
            stepId = [string](Get-PropertyValue $step "id" "")
            action = [string](Get-PropertyValue $step "action" "")
            failureCategory = [string](Get-PropertyValue $step "failureCategory" "workflow_error")
            error = [string](Get-PropertyValue $step "error" "")
            diagnostics = @((Get-PropertyValue (Get-PropertyValue $step "result" $null) "diagnostics" @()))
            nextAction = [string](Get-PropertyValue $step "nextAction" "读取失败步骤日志后重试")
        }
    }
    $errorText = if ($null -ne $WorkflowResult) { [string](Get-PropertyValue $WorkflowResult "error" "") } else { [string](Get-PropertyValue $ProcessResult "error" "") }
    return [ordered]@{
        stepId = ""
        action = ""
        failureCategory = "workflow_error"
        error = $errorText
        diagnostics = if ($errorText) { @($errorText) } else { @() }
        nextAction = "检查 workflow 结果、任务日志和执行环境"
    }
}

function Invoke-WorkflowProcess {
    param(
        [Parameter(Mandatory = $true)][string]$WorkflowPath,
        [Parameter(Mandatory = $true)][string]$AttemptDir,
        [Parameter(Mandatory = $true)][string]$WorkflowOutputRoot,
        [Parameter(Mandatory = $true)][bool]$DryRun,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )
    New-Item -ItemType Directory -Path $AttemptDir -Force | Out-Null
    New-Item -ItemType Directory -Path $WorkflowOutputRoot -Force | Out-Null
    $stdoutPath = Join-Path $AttemptDir "stdout.log"
    $stderrPath = Join-Path $AttemptDir "stderr.log"
    $workflowScript = Join-Path $PSScriptRoot "agent_workflow.ps1"
    $powershellPath = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    if (-not (Test-Path -LiteralPath $powershellPath -PathType Leaf)) { $powershellPath = "powershell.exe" }
    $arguments = New-Object 'System.Collections.Generic.List[string]'
    foreach ($value in @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $workflowScript, "-WorkflowPath", $WorkflowPath, "-OutputRoot", $WorkflowOutputRoot)) {
        [void]$arguments.Add([string]$value)
    }
    if ($DryRun) { [void]$arguments.Add("-DryRun") }
    $argumentString = (($arguments.ToArray() | ForEach-Object { ConvertTo-WindowsCommandLineArg ([string]$_) }) -join " ")
    $stdout = ""
    $stderr = ""
    $exitCode = $null
    $timedOut = $false
    $launchError = ""
    $started = Get-Date
    $process = $null
    try {
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $powershellPath
        $startInfo.WorkingDirectory = $root
        $startInfo.Arguments = $argumentString
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
            try {
                if (Test-Path -LiteralPath $taskkill -PathType Leaf) { & $taskkill /PID $process.Id /T /F 2>$null | Out-Null }
                else { $process.Kill() }
            } catch { try { $process.Kill() } catch {} }
            [void]$process.WaitForExit(5000)
        } else { [void]$process.WaitForExit() }
        if ($stdoutTask.IsCompleted) { try { $stdout = $stdoutTask.Result } catch {} }
        if ($stderrTask.IsCompleted) { try { $stderr = $stderrTask.Result } catch {} }
        if ($process.HasExited) { $exitCode = [int]$process.ExitCode }
    } catch {
        $launchError = $_.Exception.Message
        $stderr = $stderr + "`n[agent_task] workflow process launch failure: $launchError"
    }
    [System.IO.File]::WriteAllText($stdoutPath, $stdout, $utf8NoBom)
    [System.IO.File]::WriteAllText($stderrPath, $stderr, $utf8NoBom)
    $marker = @($stdout -split "\r?\n" | Where-Object { $_.Trim().StartsWith("WORKFLOW_RESULT_PATH=") } | Select-Object -Last 1)
    $resultPath = $null
    if ($marker.Count -gt 0) { $resultPath = $marker[0].Trim().Substring("WORKFLOW_RESULT_PATH=".Length).Trim() }
    $workflowResult = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        try { $workflowResult = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json } catch { $launchError = "workflow result 解析失败: $($_.Exception.Message)" }
    }
    $success = ($null -ne $workflowResult -and [bool](Get-PropertyValue $workflowResult "success" $false) -and -not $timedOut -and -not $launchError)
    return [pscustomobject][ordered]@{
        success = $success
        status = if ($success) { "passed" } else { "failed" }
        exitCode = $exitCode
        timedOut = $timedOut
        durationMs = [int]((New-TimeSpan -Start $started -End (Get-Date)).TotalMilliseconds)
        stdoutPath = $stdoutPath
        stderrPath = $stderrPath
        stdout = $stdout
        stderr = $stderr
        error = $launchError
        resultPath = $resultPath
        result = $workflowResult
    }
}

function New-AttemptRecord($ProcessResult, [int]$Index, [string]$Kind, $Candidate, [string]$WorkflowPath, [string]$AttemptDir, [bool]$DryRun) {
    $workflowResult = $ProcessResult.result
    $signal = if ($ProcessResult.success) { $null } else { Get-FailureSignal $workflowResult $ProcessResult }
    $status = if ($DryRun -and $ProcessResult.success) { "planned" } elseif ($ProcessResult.success) { "passed" } else { "failed" }
    $record = [ordered]@{
        index = $Index
        kind = $Kind
        candidateId = if ($null -ne $Candidate) { [string](Get-PropertyValue $Candidate "id" "") } else { $null }
        description = if ($null -ne $Candidate) { [string](Get-PropertyValue $Candidate "description" "") } else { "基础 workflow" }
        status = $status
        startedAt = (Get-Date).AddMilliseconds(-1 * [int]$ProcessResult.durationMs).ToString("o")
        endedAt = (Get-Date).ToString("o")
        durationMs = [int]$ProcessResult.durationMs
        workflowPath = Get-RelativePath $WorkflowPath
        attemptDir = Get-RelativePath $AttemptDir
        workflowResultPath = if ($ProcessResult.resultPath) { Get-RelativePath $ProcessResult.resultPath } else { $null }
        exitCode = $ProcessResult.exitCode
        timedOut = [bool]$ProcessResult.timedOut
        patches = if ($null -ne $Candidate) { @((Get-PropertyValue $Candidate "patches" @())) } else { @() }
        failureCategory = if ($null -ne $signal) { [string]$signal.failureCategory } else { "" }
        failedStepId = if ($null -ne $signal) { [string]$signal.stepId } else { "" }
        action = if ($null -ne $signal) { [string]$signal.action } else { "" }
        diagnostics = if ($null -ne $signal) { @($signal.diagnostics) } else { @() }
        nextAction = if ($null -ne $signal) { [string]$signal.nextAction } elseif ($DryRun) { "移除 preview 后显式执行任务" } else { "继续后续任务" }
        error = [string]$ProcessResult.error
    }
    [void]$script:attemptRecords.Add([pscustomobject]$record)
    [void](Add-Artifact $ProcessResult.stdoutPath "task_attempt_stdout")
    [void](Add-Artifact $ProcessResult.stderrPath "task_attempt_stderr")
    if ($ProcessResult.resultPath) { [void](Add-Artifact $ProcessResult.resultPath "workflow_result") }
    return [pscustomobject]$record
}

function Write-TaskManifest {
    $manifestPath = Join-Path $script:runDir "manifest.json"
    $entries = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse | Where-Object { $_.Name -notin @("manifest.json", "result.json") })) {
        $entries += [ordered]@{ path = Get-RelativePath $file.FullName; size = [int64]$file.Length; sha256 = Get-Sha256 $file.FullName }
    }
    $manifest = [ordered]@{
        schemaVersion = 1
        operation = "agent_task"
        runId = [string]$script:taskContext.runId
        createdAt = (Get-Date).ToString("o")
        files = $entries
    }
    Write-JsonFile $manifestPath $manifest
    foreach ($entry in @($manifest.files)) {
        $full = Resolve-ProjectPath ([string]$entry.path) $true
        if ((Get-Sha256 $full) -ne [string]$entry.sha256) { throw "agent_task manifest 校验失败: $($entry.path)" }
    }
    return $manifestPath
}

function Save-TaskResult([bool]$Success, [string]$ErrorText = "", $Failure = $null, [string]$NextAction = "") {
    if ($null -eq $script:resultPath) { return }
    $endedAt = Get-Date
    $records = $script:attemptRecords.ToArray()
    $result = [ordered]@{
        schemaVersion = 1
        tool = "agent_task"
        apiVersion = 1
        success = $Success
        mode = [string]$script:taskContext.mode
        preview = ([string]$script:taskContext.mode -eq "preview")
        runId = [string]$script:taskContext.runId
        runDir = Get-RelativePath $script:runDir
        taskName = [string]$script:taskContext.taskName
        taskPath = Get-RelativePath $script:taskContext.taskPath
        workflowPath = Get-RelativePath $script:taskContext.baseWorkflowPath
        startedAt = $script:startedAt.ToString("o")
        endedAt = $endedAt.ToString("o")
        durationMs = [int](($endedAt - $script:startedAt).TotalMilliseconds)
        maxAttempts = [int]$script:taskContext.maxAttempts
        counts = [ordered]@{
            attempts = $records.Count
            passed = @($records | Where-Object { $_.status -eq "passed" }).Count
            planned = @($records | Where-Object { $_.status -eq "planned" }).Count
            failed = @($records | Where-Object { $_.status -eq "failed" }).Count
            candidatesConsidered = @($records | Where-Object { $_.kind -eq "candidate" }).Count
        }
        selectedCandidate = $script:taskContext.selectedCandidate
        attempts = $records
        failure = $Failure
        review = [ordered]@{
            explicitExecuteRequired = $true
            destructiveChangesAllowed = $false
            assertionExpectedValuesMutable = $false
            note = if ($script:taskContext.mode -eq "preview") { "当前只生成并验证计划，没有执行引擎或写入业务文件" } else { "候选补丁已通过 action 白名单和 workflow 安全闸门后执行" }
        }
        artifacts = $script:artifacts.ToArray()
        error = $ErrorText
        nextAction = if ($NextAction) { $NextAction } elseif ($Success -and $script:taskContext.mode -eq "preview") { "人工审查候选补丁后，以 mode=execute 重跑任务" } elseif ($Success) { "agent_task_completed" } elseif ($null -ne $Failure) { [string](Get-PropertyValue $Failure "nextAction" "读取失败步骤日志后修复") } else { "修复任务输入或执行环境后重试" }
    }
    Write-JsonFile $script:resultPath $result
}

$finalSuccess = $false
$fatalError = ""
$finalFailure = $null
$finalNextAction = ""

try {
    $taskFull = Resolve-ProjectPath $TaskPath $true
    $task = [System.IO.File]::ReadAllText($taskFull, $utf8NoBom) | ConvertFrom-Json
    if ($null -eq $task) { throw "task JSON 为空" }
    if ([int](Get-PropertyValue $task "schemaVersion" 0) -ne 1) { throw "task schemaVersion 必须为 1" }
    $workflowInline = Get-PropertyValue $task "workflow" $null
    $workflowPathValue = [string](Get-PropertyValue $task "workflowPath" "")
    if (($null -eq $workflowInline) -eq ([string]::IsNullOrWhiteSpace($workflowPathValue))) { throw "task 必须且只能提供 workflow 或 workflowPath" }
    $taskMode = [string](Get-PropertyValue $task "mode" "preview")
    if ($Mode) { $taskMode = $Mode }
    if ($taskMode -notin @("preview", "execute")) { throw "mode 只支持 preview 或 execute" }
    $taskMaxAttempts = [int](Get-PropertyValue $task "maxAttempts" 2)
    if ($MaxAttempts -gt 0) { $taskMaxAttempts = $MaxAttempts }
    if ($taskMaxAttempts -lt 1 -or $taskMaxAttempts -gt 5) { throw "maxAttempts 必须在 1..5" }
    $timeoutSeconds = [int](Get-PropertyValue $task "timeoutSeconds" 1800)
    if ($timeoutSeconds -lt 1 -or $timeoutSeconds -gt 7200) { throw "timeoutSeconds 必须在 1..7200" }
    $workflowFull = $null
    $workflow = $null
    if ($workflowPathValue) {
        $workflowFull = Resolve-ProjectPath $workflowPathValue $true
        $workflow = [System.IO.File]::ReadAllText($workflowFull, $utf8NoBom) | ConvertFrom-Json
    } else { $workflow = Deep-Copy $workflowInline }
    if ($null -eq $workflow -or [int](Get-PropertyValue $workflow "schemaVersion" 0) -ne 1) { throw "基础 workflow 必须是 schemaVersion=1" }
    $steps = @((Get-PropertyValue $workflow "steps" @()))
    if ($steps.Count -lt 1) { throw "基础 workflow 至少需要一个 step" }
    $knownActions = @("inspect_project", "get_project_context", "inspect_scene", "query_assets", "create_script", "build", "compile_games", "validate_scene", "apply_scene_commands", "run_gameplay_test", "run_render_test", "capture_frame", "capture_performance", "read_dump", "assert_state", "stop_engine")
    $stepIds = @{}
    foreach ($step in $steps) {
        $id = [string](Get-PropertyValue $step "id" "")
        $action = [string](Get-PropertyValue $step "action" "")
        if ($id -notmatch '^[A-Za-z][A-Za-z0-9_.-]{0,63}$' -or $stepIds.ContainsKey($id)) { throw "基础 workflow step id 不合法或重复: $id" }
        if ($knownActions -notcontains $action) { throw "基础 workflow action 不支持: $action" }
        $stepIds[$id] = $action
    }
    $candidateInputs = @((Get-PropertyValue $task "candidates" @()))
    if ($candidateInputs.Count -gt 32) { throw "candidates 数量不能超过 32" }
    $candidates = New-Object 'System.Collections.Generic.List[object]'
    $candidateIds = @{}
    for ($index = 0; $index -lt $candidateInputs.Count; $index++) {
        $candidate = Validate-Candidate $candidateInputs[$index] $index $steps $knownActions
        if ($candidateIds.ContainsKey($candidate.id)) { throw "candidate id 重复: $($candidate.id)" }
        $candidateIds[$candidate.id] = $true
        [void]$candidates.Add($candidate)
    }

    $outputRootFull = Resolve-ProjectPath $OutputRoot $false
    if (-not $RunId) { $RunId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8)) }
    if ($RunId -notmatch '^[A-Za-z0-9_.-]{1,80}$') { throw "RunId 只能包含字母、数字、下划线、点和短横线" }
    $runDir = Join-Path $outputRootFull $RunId
    if (Test-Path -LiteralPath $runDir) { throw "agent_task run 目录已存在，为避免覆盖请换 RunId: $runDir" }
    New-Item -ItemType Directory -Path $runDir -Force | Out-Null
    $script:runDir = $runDir
    $script:resultPath = Join-Path $runDir "result.json"
    $taskName = [string](Get-PropertyValue $task "name" ([System.IO.Path]::GetFileNameWithoutExtension($taskFull)))
    if ($workflowFull) { $baseWorkflowPath = $workflowFull }
    else {
        $baseWorkflowPath = Join-Path $runDir "base.workflow.json"
        Write-JsonFile $baseWorkflowPath $workflow
    }
    $script:taskContext = [pscustomobject][ordered]@{
        runId = $RunId
        runDir = $runDir
        taskName = $taskName
        taskPath = $taskFull
        baseWorkflowPath = $baseWorkflowPath
        mode = $taskMode
        maxAttempts = $taskMaxAttempts
        timeoutSeconds = $timeoutSeconds
        outputRoot = $outputRootFull
        selectedCandidate = $null
    }
    [System.IO.File]::Copy($taskFull, (Join-Path $runDir "task.input.json"), $true)
    [void](Add-Artifact (Join-Path $runDir "task.input.json") "task_input")
    if (-not $workflowFull) { [void](Add-Artifact $baseWorkflowPath "workflow_input") }

    if ($taskMode -eq "preview") {
        $attemptIndex = 0
        $baseAttemptDir = Join-Path $runDir "attempt-00-base"
        $baseOutputRoot = Join-Path $baseAttemptDir "workflow_runs"
        $baseProcess = Invoke-WorkflowProcess $baseWorkflowPath $baseAttemptDir $baseOutputRoot $true $timeoutSeconds
        $baseRecord = New-AttemptRecord $baseProcess $attemptIndex "base" $null $baseWorkflowPath $baseAttemptDir $true
        if (-not $baseProcess.success) {
            $finalFailure = Get-FailureSignal $baseProcess.result $baseProcess
            $finalNextAction = "修复基础 workflow 校验错误后，再预览候选补丁"
        } else {
            $previewCount = [Math]::Min($candidates.Count, [Math]::Max(0, $taskMaxAttempts - 1))
            for ($candidateIndex = 0; $candidateIndex -lt $previewCount; $candidateIndex++) {
                $candidate = $candidates[$candidateIndex]
                $attemptDir = Join-Path $runDir ("attempt-{0:D2}-candidate-{1}" -f ($candidateIndex + 1), $candidate.id)
                $candidateWorkflowPath = Join-Path $attemptDir "workflow.json"
                $candidateWorkflow = Apply-Candidate $workflow $candidate
                Write-JsonFile $candidateWorkflowPath $candidateWorkflow
                [void](Add-Artifact $candidateWorkflowPath "candidate_workflow")
                $processResult = Invoke-WorkflowProcess $candidateWorkflowPath $attemptDir (Join-Path $attemptDir "workflow_runs") $true $timeoutSeconds
                [void](New-AttemptRecord $processResult ($candidateIndex + 1) "candidate" $candidate $candidateWorkflowPath $attemptDir $true)
            }
            $finalSuccess = $true
            $finalNextAction = "人工审查候选补丁后，以 mode=execute 重跑任务"
        }
    } else {
        $currentWorkflow = $workflow
        $signal = $null
        $usedCandidates = @{}
        for ($attemptIndex = 0; $attemptIndex -lt $taskMaxAttempts; $attemptIndex++) {
            $candidate = $null
            $attemptKind = "base"
            $attemptWorkflowPath = $baseWorkflowPath
            $attemptDir = Join-Path $runDir ("attempt-{0:D2}-base" -f $attemptIndex)
            if ($attemptIndex -gt 0) {
                $candidate = @($candidates | Where-Object {
                    $candidateId = [string](Get-PropertyValue $_ "id" "")
                    (-not $usedCandidates.ContainsKey($candidateId)) -and (Test-CandidateMatch (Get-PropertyValue $_ "when" $null) $signal)
                } | Select-Object -First 1)
                if ($candidate.Count -eq 0) { break }
                $candidate = $candidate[0]
                $usedCandidates[[string]$candidate.id] = $true
                $attemptKind = "candidate"
                $attemptDir = Join-Path $runDir ("attempt-{0:D2}-candidate-{1}" -f $attemptIndex, $candidate.id)
                $attemptWorkflowPath = Join-Path $attemptDir "workflow.json"
                $currentWorkflow = Apply-Candidate $workflow $candidate
                Write-JsonFile $attemptWorkflowPath $currentWorkflow
                [void](Add-Artifact $attemptWorkflowPath "candidate_workflow")
            }
            $processResult = Invoke-WorkflowProcess $attemptWorkflowPath $attemptDir (Join-Path $attemptDir "workflow_runs") $false $timeoutSeconds
            $record = New-AttemptRecord $processResult $attemptIndex $attemptKind $candidate $attemptWorkflowPath $attemptDir $false
            if ($processResult.success) {
                $finalSuccess = $true
                if ($null -ne $candidate) { $script:taskContext.selectedCandidate = [string]$candidate.id }
                $finalNextAction = if ($candidate) { "候选修复通过，保留该候选及 workflow 运行证据" } else { "基础 workflow 已完成" }
                break
            }
            $signal = Get-FailureSignal $processResult.result $processResult
            $finalFailure = $signal
            $finalNextAction = [string]$signal.nextAction
        }
        if (-not $finalSuccess -and $null -eq $finalFailure) {
            $finalFailure = [ordered]@{ stepId = ""; action = ""; failureCategory = "workflow_error"; error = "没有候选补丁匹配当前失败信号，或已达到 maxAttempts"; diagnostics = @(); nextAction = "新增经过审查的候选补丁，或人工修复后重新执行" }
            $finalNextAction = [string]$finalFailure.nextAction
        }
    }

    $manifestPath = Write-TaskManifest
    [void](Add-Artifact $manifestPath "task_manifest")
    Save-TaskResult $finalSuccess "" $finalFailure $finalNextAction
    Write-Output "TASK_RESULT_PATH=$script:resultPath"
    Write-Output "SUCCESS=$finalSuccess"
    if ($finalSuccess) { exit 0 } else { exit 1 }
} catch {
    $fatalError = $_.Exception.Message
    if ($null -ne $script:runDir) {
        if ($null -eq $script:taskContext) {
            $script:taskContext = [pscustomobject][ordered]@{ runId = $RunId; runDir = $script:runDir; taskName = "invalid"; taskPath = $TaskPath; baseWorkflowPath = $TaskPath; mode = if ($Mode) { $Mode } else { "preview" }; maxAttempts = $MaxAttempts; outputRoot = $OutputRoot; selectedCandidate = $null }
        }
        Save-TaskResult $false $fatalError $null "修复 task 输入或执行环境后重试"
        Write-Output "TASK_RESULT_PATH=$script:resultPath"
    }
    Write-Output "ERROR=$fatalError"
    Write-Output "SUCCESS=False"
    exit 3
}
