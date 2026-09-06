# agent_workflow.ps1 - MikanEngine 声明式 Agent 编排器
# ------------------------------------------------------------------
# 让 Agent 用一个受约束的 workflow JSON 串起：脚本生成、构建、场景修改、
# 玩法/渲染测试、状态读取和断言。它是工具层编排器，不是模型本身；模型仍
# 负责提出 workflow，人负责审查高风险操作和最终视觉结果。
#
# 每次执行写入 out\agent_runs\<run-id>\：
#   workflow.input.json / step-*/ / result.json / manifest.json
#
# 退出码：0=成功或 dry-run 计划生成，1=步骤失败，3=workflow/环境错误。
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$WorkflowPath,

    [string]$OutputRoot = "out\agent_runs",

    [string]$RunId = "",

    [switch]$DryRun,

    [switch]$ContinueOnFailure
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
$buildDir = Join-Path $root "out\build\x64-Release"
$script:runDir = $null
$script:resultPath = $null
$script:lastDumpPath = $null
$script:stepRecords = New-Object 'System.Collections.Generic.List[object]'
$script:stepRecordMap = @{}
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
    $json = $Value | ConvertTo-Json -Depth 50
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

function Resolve-ProjectDirectory([string]$Path, [bool]$MustExist = $false) {
    $full = Resolve-ProjectPath $Path $false
    if ($MustExist -and -not (Test-Path -LiteralPath $full -PathType Container)) {
        throw "目录不存在: $full"
    }
    return $full
}

function Get-WorkflowResourceRoot([string]$ProjectFull) {
    $manifestPath = Join-Path $ProjectFull "project.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { return $ProjectFull }
    try {
        $manifest = [System.IO.File]::ReadAllText($manifestPath, $utf8NoBom) | ConvertFrom-Json
        $resourceRoot = [string](Get-PropertyValue $manifest "resourceRoot" ".")
        if ([string]::IsNullOrWhiteSpace($resourceRoot)) { $resourceRoot = "." }
        if ([System.IO.Path]::IsPathRooted($resourceRoot)) { throw "resourceRoot 必须是项目内相对路径" }
        $resourceFull = [System.IO.Path]::GetFullPath((Join-Path $ProjectFull $resourceRoot))
        $projectPrefix = $ProjectFull.TrimEnd('\', '/') + '\'
        if (-not $resourceFull.Equals($ProjectFull, [System.StringComparison]::OrdinalIgnoreCase) -and
            -not $resourceFull.StartsWith($projectPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "resourceRoot 不能逃逸项目目录"
        }
        return $resourceFull
    } catch {
        throw "无法解析项目资源根: $manifestPath；$($_.Exception.Message)"
    }
}

function Resolve-WorkflowScenePath([string]$SceneValue, [string]$ProjectFull = "") {
    if ($ProjectFull -and -not [System.IO.Path]::IsPathRooted($SceneValue)) {
        $clean = $SceneValue.Replace('\', '/').TrimStart('/')
        $base = if ($clean.StartsWith('assets/', [System.StringComparison]::OrdinalIgnoreCase)) { $ProjectFull } else { Get-WorkflowResourceRoot $ProjectFull }
        $candidate = [System.IO.Path]::GetFullPath((Join-Path $base $clean))
        $projectPrefix = $ProjectFull.TrimEnd('\', '/') + '\'
        if (($candidate.Equals($ProjectFull, [System.StringComparison]::OrdinalIgnoreCase) -or $candidate.StartsWith($projectPrefix, [System.StringComparison]::OrdinalIgnoreCase)) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) { return $candidate }
    }
    return Resolve-ProjectPath $SceneValue $true
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
        $full = if ([System.IO.Path]::IsPathRooted($Path)) {
            [System.IO.Path]::GetFullPath($Path)
        } else {
            Resolve-ProjectPath $Path $false
        }
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { return $null }
        $relative = Get-RelativePath $full
        foreach ($existing in $script:artifacts.ToArray()) {
            if ([string]$existing.path -eq $relative) { return $existing }
        }
        $entry = [ordered]@{
            path = $relative
            kind = $Kind
            size = [int64](Get-Item -LiteralPath $full).Length
            sha256 = Get-Sha256 $full
        }
        [void]$script:artifacts.Add([pscustomobject]$entry)
        return [pscustomobject]$entry
    } catch {
        return $null
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

function Get-OutputTail([string]$Text, [int]$TailCount = 80) {
    if ([string]::IsNullOrWhiteSpace($Text)) { return "" }
    $lines = @($Text -split "`r?`n")
    if ($lines.Count -le $TailCount) { return ($lines -join "`n").Trim() }
    return ("... 已省略前 {0} 行 ...`n{1}" -f ($lines.Count - $TailCount), (($lines | Select-Object -Last $TailCount) -join "`n")).Trim()
}

function Get-LogDiagnostics([string]$Text) {
    if ([string]::IsNullOrWhiteSpace($Text)) { return @() }
    return @($Text -split "`r?`n" | Where-Object {
        $_ -match '(?i)(\[ERR\]|\[FTL\]|\[Crash\]|\bERROR\b|\bFatal\b|FAILED:|validation error|exception)'
    } | Select-Object -Last 40)
}

function Invoke-ExternalProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)][string]$StepDir
    )

    $stdoutPath = Join-Path $StepDir "stdout.log"
    $stderrPath = Join-Path $StepDir "stderr.log"
    $argumentString = (($ArgumentList | ForEach-Object { ConvertTo-WindowsCommandLineArg ([string]$_) }) -join " ")
    $started = Get-Date
    $exitCode = $null
    $timedOut = $false
    $launchError = ""
    $stdout = ""
    $stderr = ""
    $process = $null

    New-Item -ItemType Directory -Path $StepDir -Force | Out-Null
    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        $launchError = "可执行文件不存在: $FilePath"
    } elseif (-not (Test-Path -LiteralPath $WorkingDirectory -PathType Container)) {
        $launchError = "工作目录不存在: $WorkingDirectory"
    } else {
        try {
            $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
            $startInfo.FileName = $FilePath
            $startInfo.WorkingDirectory = $WorkingDirectory
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
                    if (Test-Path -LiteralPath $taskkill -PathType Leaf) {
                        & $taskkill /PID $process.Id /T /F 2>$null | Out-Null
                    } else {
                        $process.Kill()
                    }
                } catch { try { $process.Kill() } catch {} }
                [void]$process.WaitForExit(5000)
            } else {
                [void]$process.WaitForExit()
            }
            if ($stdoutTask.IsCompleted) { try { $stdout = $stdoutTask.Result } catch {} }
            else { $stderr += "`n[workflow] stdout stream did not close after termination" }
            if ($stderrTask.IsCompleted) { try { $stderr += $stderrTask.Result } catch {} }
            else { $stderr += "`n[workflow] stderr stream did not close after termination" }
            if ($process.HasExited) { $exitCode = [int]$process.ExitCode }
        } catch {
            $launchError = $_.Exception.Message
            $stderr += "`n[workflow] process launch failure: $launchError"
        }
    }

    [System.IO.File]::WriteAllText($stdoutPath, $stdout, $utf8NoBom)
    [System.IO.File]::WriteAllText($stderrPath, $stderr, $utf8NoBom)
    $status = if ($launchError -or $timedOut -or $null -eq $exitCode -or $exitCode -ne 0) { "failed" } else { "passed" }
    return [pscustomobject][ordered]@{
        name = $Name
        status = $status
        exitCode = $exitCode
        timedOut = $timedOut
        durationMs = [int]((New-TimeSpan -Start $started -End (Get-Date)).TotalMilliseconds)
        stdoutPath = $stdoutPath
        stderrPath = $stderrPath
        stdout = $stdout
        stderr = $stderr
        error = $launchError
    }
}

function Find-LastJsonObject([string]$Text, [string]$ToolName = "") {
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    $lines = @($Text -split "`r?`n")
    for ($index = $lines.Count - 1; $index -ge 0; $index--) {
        $line = $lines[$index].Trim()
        if (-not $line) { continue }
        try {
            $value = $line | ConvertFrom-Json
            if ($null -eq $ToolName -or $ToolName -eq "" -or [string](Get-PropertyValue $value "tool" "") -eq $ToolName) {
                return $value
            }
        } catch {}
    }
    return $null
}

function Get-ResultPathMarker([string]$Text, [string]$Marker = "RESULT_PATH=") {
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    $matches = @($Text -split "`r?`n" | Where-Object { $_.Trim().StartsWith($Marker) })
    if ($matches.Count -eq 0) { return $null }
    return $matches[$matches.Count - 1].Trim().Substring($Marker.Length).Trim()
}

function Get-ActionFailureCategory([string]$Action, [string]$Message = "") {
    if ($Action -eq "assert_state") { return "assertion_failed" }
    if ($Action -in @("build", "compile_games", "create_script")) { return "build_error" }
    if ($Action -in @("validate_scene", "apply_scene_commands")) { return "validation_error" }
    if ($Action -in @("run_gameplay_test", "run_render_test")) { return "runtime_error" }
    if ($Action -eq "capture_frame") { return "visual_capture_error" }
    if ($Action -eq "capture_performance") { return "performance_capture_error" }
    if ($Action -in @("inspect_project", "inspect_scene", "query_assets")) { return "discovery_error" }
    if ($Message -match '(?i)(路径|禁止|危险|allowDestructive|destructive|include)') { return "security_error" }
    return "workflow_error"
}

function Get-NextAction([string]$Action, [bool]$Success, [string]$Message = "") {
    if ($Success) { return "continue_to_next_step" }
    switch ($Action) {
        "create_script" { return "检查脚本源码校验或编译日志；修复后重新执行 create_script" }
        "build" { return "读取该步骤 build.log，修复编译错误后重新 build" }
        "compile_games" { return "读取插件编译日志，确认目标游戏 DLL 生成后重试" }
        "validate_scene" { return "根据校验错误修复场景 Schema、实体引用或资源路径" }
        "apply_scene_commands" { return "检查 commands 和 candidate_validation.log；确认后再提交场景 patch" }
        "run_gameplay_test" { return "读取 stdout.log/stderr.log 和 state dump，定位脚本/物理/插件加载问题" }
        "run_render_test" { return "读取 Vulkan 日志、崩溃日志、dump 和 screenshot metadata；确认设备/资源/交换链问题，必要时检查截图是否黑屏" }
        "capture_frame" { return "读取抓帧步骤的 stdout/stderr 和 result.json；确认 64 位 RenderDoc 已安装且引擎日志出现 ready/triggered" }
        "capture_performance" { return "读取 Nsight stdout/stderr 和 result.json；权限不足时以管理员身份运行或开启 GPU performance counters 访问" }
        "inspect_project" { return "读取 discovery result.json；确认项目元数据、Schema 和 inventory 是否可解析" }
        "inspect_scene" { return "读取 discovery result.json；根据 validation、组件统计和资源引用决定 validate_scene 或 apply_scene_commands" }
        "query_assets" { return "读取 discovery result.json；从受限资产索引中选择下一步需要的场景、模型、纹理或脚本" }
        "read_dump" { return "确认前一步测试生成了有效的 dump JSON" }
        "assert_state" { return "根据断言差异修改脚本或场景，再运行同一测试" }
        "stop_engine" { return "确认 EngineMain/MikanTestRunner 已退出后再继续构建或测试" }
        default { return "读取该步骤日志，修复后重新执行 workflow" }
    }
}

function Resolve-Reference([string]$Token, $WorkflowContext) {
    if ($Token -eq "workflow.runId") { return [string]$WorkflowContext.runId }
    if ($Token -eq "workflow.runDir") { return [string]$WorkflowContext.runDir }
    if ($Token -eq "workflow.root") { return [string]$WorkflowContext.root }
    if ($Token -eq "workflow.outputRoot") { return [string]$WorkflowContext.outputRoot }
    if (-not $Token.StartsWith("steps.", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "未知模板引用: $Token"
    }
    $stepReference = $Token.Substring(6)
    $stepId = $null
    $path = $null
    $longestIdLength = -1
    foreach ($candidate in @($script:stepRecordMap.Keys)) {
        $prefix = ([string]$candidate) + "."
        if ($stepReference.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) -and
            $candidate.Length -gt $longestIdLength) {
            $stepId = [string]$candidate
            $path = $stepReference.Substring($prefix.Length)
            $longestIdLength = $candidate.Length
        }
    }
    if (-not $stepId) { throw "模板引用的步骤尚未完成: $stepReference" }
    if ([string]::IsNullOrWhiteSpace($path)) { throw "模板引用缺少字段路径: $Token" }
    $current = $script:stepRecordMap[$stepId]
    foreach ($segment in $path.Split('.')) {
        if ($null -eq $current) { throw "模板引用不存在: $Token" }
        if ($current -is [System.Collections.IDictionary] -and $current.Contains($segment)) {
            $current = $current[$segment]
        } else {
            $property = $current.PSObject.Properties[$segment]
            if ($null -eq $property) { throw "模板引用不存在: $Token" }
            $current = $property.Value
        }
    }
    return $current
}

function Resolve-TemplateValue($Value, $WorkflowContext) {
    if ($null -eq $Value) { return $null }
    if ($Value -is [string]) {
        $matches = [regex]::Matches([string]$Value, '\$\{([^}]+)\}')
        if ($matches.Count -eq 0) { return [string]$Value }
        if ($matches.Count -eq 1 -and $matches[0].Value -eq [string]$Value) {
            return Resolve-Reference $matches[0].Groups[1].Value $WorkflowContext
        }
        $resolved = [string]$Value
        foreach ($match in $matches) {
            $replacement = [string](Resolve-Reference $match.Groups[1].Value $WorkflowContext)
            $resolved = $resolved.Replace($match.Value, $replacement)
        }
        return $resolved
    }
    if ($Value -is [array]) {
        $items = New-Object 'System.Collections.Generic.List[object]'
        foreach ($item in $Value) { [void]$items.Add((Resolve-TemplateValue $item $WorkflowContext)) }
        return $items.ToArray()
    }
    if ($Value -is [System.Collections.IDictionary]) {
        $map = [ordered]@{}
        foreach ($key in $Value.Keys) { $map[[string]$key] = Resolve-TemplateValue $Value[$key] $WorkflowContext }
        return $map
    }
    if ($Value.PSObject -and $Value.PSObject.Properties.Count -gt 0) {
        $map = [ordered]@{}
        foreach ($property in $Value.PSObject.Properties) {
            $map[[string]$property.Name] = Resolve-TemplateValue $property.Value $WorkflowContext
        }
        return $map
    }
    return $Value
}

function Test-ReservedExtraArgument([string]$Argument) {
    return $Argument -match '^--(headless|headless-no-render|frames|fixed-dt|dump-state|crash-log|scene|game|project|screenshot-frame|screenshot-path|renderdoc-capture-frame|renderdoc-capture-path)(=|$)'
}

function Get-RunningEngineProcesses {
    $result = @()
    foreach ($entry in @(@("EngineMain", (Join-Path $buildDir "EngineMain.exe")), @("MikanTestRunner", (Join-Path $buildDir "MikanTestRunner.exe")))) {
        foreach ($process in @(Get-Process -Name $entry[0] -ErrorAction SilentlyContinue)) {
            try {
                if ($process.Path -and [System.IO.Path]::GetFullPath($process.Path).Equals(
                        [System.IO.Path]::GetFullPath($entry[1]), [System.StringComparison]::OrdinalIgnoreCase)) {
                    $result += $process
                }
            } catch {}
        }
    }
    return @($result)
}

function Invoke-CreateScriptAction($Arguments, [string]$StepDir, [int]$TimeoutSeconds, [bool]$AllowDestructive) {
    $scriptName = [string](Get-PropertyValue $Arguments "scriptName" "")
    $outputPathValue = [string](Get-PropertyValue $Arguments "outputPath" "")
    if (-not $scriptName -or -not $outputPathValue) { throw "create_script 需要 scriptName 和 outputPath" }
    $overwrite = [bool](Get-PropertyValue $Arguments "overwrite" $false)
    if ($overwrite -and -not $AllowDestructive) { throw "create_script overwrite=true 需要 workflow.allowDestructive=true" }
    $scaffold = Join-Path $PSScriptRoot "script_scaffold.ps1"
    if (-not (Test-Path -LiteralPath $scaffold -PathType Leaf)) { throw "脚本生成器不存在: $scaffold" }
    $outputFull = Resolve-ProjectPath $outputPathValue $false
    $command = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $scaffold, "-ScriptName", $scriptName, "-OutputPath", $outputFull)
    $className = [string](Get-PropertyValue $Arguments "className" "")
    if ($className) { $command += @("-ClassName", $className) }
    if (Test-Property $Arguments "fields") {
        $fieldsPath = Join-Path $StepDir "fields.request.json"
        Write-JsonFile $fieldsPath @((Get-PropertyValue $Arguments "fields" @()))
        $command += @("-FieldsPath", $fieldsPath)
    }
    $source = [string](Get-PropertyValue $Arguments "source" "")
    if ($source) {
        $sourcePath = Join-Path $StepDir "source.request.cpp"
        [System.IO.File]::WriteAllText($sourcePath, $source, [System.Text.UTF8Encoding]::new($false))
        $command += @("-SourcePath", $sourcePath)
    }
    $sourcePathValue = [string](Get-PropertyValue $Arguments "sourcePath" "")
    if ($sourcePathValue) { $command += @("-SourcePath", (Resolve-ProjectPath $sourcePathValue $true)) }
    if ($overwrite) { $command += "-Force" }
    $compile = [bool](Get-PropertyValue $Arguments "compile" $false)
    if ($compile) { $command += "-Compile" }

    $process = Invoke-ExternalProcess "create_script" (Get-Command powershell.exe).Source $command $root $TimeoutSeconds $StepDir
    $structured = Find-LastJsonObject ($process.stdout + "`n" + $process.stderr) "script_scaffold"
    $success = ($process.status -eq "passed" -and $null -ne $structured -and [bool](Get-PropertyValue $structured "success" $false))
    $artifacts = @()
    foreach ($path in @($process.stdoutPath, $process.stderrPath, $outputFull, (Get-PropertyValue $structured "backupPath" ""))) {
        $artifact = Add-Artifact $path "script"
        if ($null -ne $artifact) { $artifacts += $artifact }
    }
    return [ordered]@{
        success = $success
        action = "create_script"
        scriptName = $scriptName
        outputPath = if ($null -ne $structured) { [string](Get-PropertyValue $structured "outputPath" (Get-RelativePath $outputFull)) } else { Get-RelativePath $outputFull }
        className = if ($null -ne $structured) { [string](Get-PropertyValue $structured "className" $className) } else { $className }
        generated = if ($null -ne $structured) { [bool](Get-PropertyValue $structured "generated" $false) } else { $false }
        sha256 = if ($null -ne $structured) { [string](Get-PropertyValue $structured "sha256" "") } else { "" }
        backupPath = if ($null -ne $structured) { [string](Get-PropertyValue $structured "backupPath" "") } else { "" }
        compile = $compile
        compileExitCode = if ($null -ne $structured) { Get-PropertyValue $structured "compileExitCode" $null } else { $null }
        logSummary = Get-OutputTail ($process.stdout + "`n" + $process.stderr)
        diagnostics = @(Get-LogDiagnostics ($process.stdout + "`n" + $process.stderr))
        artifacts = @($artifacts)
        process = [ordered]@{ status = $process.status; exitCode = $process.exitCode; timedOut = $process.timedOut; durationMs = $process.durationMs }
    }
}

function Invoke-BuildAction($Arguments, [string]$StepDir, [int]$TimeoutSeconds) {
    $target = [string](Get-PropertyValue $Arguments "target" "EngineMain")
    if ($target -notin @("EngineMain", "MikanTestRunner", "Editor", "Game", "CompileShaders")) { throw "不支持的构建目标: $target" }
    $buildScript = Join-Path $PSScriptRoot "build.ps1"
    $buildLog = Join-Path $StepDir "build.log"
    $command = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $buildScript, "-Target", $target, "-LogPath", $buildLog)
    if ([bool](Get-PropertyValue $Arguments "killEngine" $false)) { $command += "-KillEngine" }
    if ([bool](Get-PropertyValue $Arguments "cleanFirst" $false)) { $command += "-CleanFirst" }
    if ([bool](Get-PropertyValue $Arguments "configureIfMissing" $true)) { $command += "-ConfigureIfMissing" }
    $process = Invoke-ExternalProcess ("build_" + $target) (Get-Command powershell.exe).Source $command $root $TimeoutSeconds $StepDir
    $artifacts = @()
    foreach ($path in @($process.stdoutPath, $process.stderrPath, $buildLog)) {
        $artifact = Add-Artifact $path "build_log"
        if ($null -ne $artifact) { $artifacts += $artifact }
    }
    return [ordered]@{
        success = ($process.status -eq "passed")
        action = "build"
        target = $target
        exitCode = $process.exitCode
        timedOut = $process.timedOut
        log = Get-RelativePath $buildLog
        logSummary = Get-OutputTail ($process.stdout + "`n" + $process.stderr)
        diagnostics = @(Get-LogDiagnostics ($process.stdout + "`n" + $process.stderr))
        artifacts = @($artifacts)
    }
}

function Invoke-CompileGamesAction($Arguments, [string]$StepDir, [int]$TimeoutSeconds) {
    $compileScript = Join-Path $PSScriptRoot "compile_games.ps1"
    $process = Invoke-ExternalProcess "compile_games" (Get-Command powershell.exe).Source @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $compileScript) $root $TimeoutSeconds $StepDir
    $artifacts = @()
    foreach ($path in @($process.stdoutPath, $process.stderrPath)) {
        $artifact = Add-Artifact $path "build_log"
        if ($null -ne $artifact) { $artifacts += $artifact }
    }
    return [ordered]@{
        success = ($process.status -eq "passed")
        action = "compile_games"
        exitCode = $process.exitCode
        timedOut = $process.timedOut
        logSummary = Get-OutputTail ($process.stdout + "`n" + $process.stderr)
        diagnostics = @(Get-LogDiagnostics ($process.stdout + "`n" + $process.stderr))
        artifacts = @($artifacts)
    }
}

function Invoke-ValidateSceneAction($Arguments, [string]$StepDir, [int]$TimeoutSeconds) {
    $sceneValue = [string](Get-PropertyValue $Arguments "scenePath" (Get-PropertyValue $Arguments "scene" ""))
    if (-not $sceneValue) { throw "validate_scene 需要 scenePath" }
    $projectValue = [string](Get-PropertyValue $Arguments "projectPath" (Get-PropertyValue $Arguments "project" ""))
    $projectFull = if ($projectValue) { Resolve-ProjectDirectory $projectValue $true } else { "" }
    $scene = Resolve-WorkflowScenePath $sceneValue $projectFull
    $validator = Join-Path $PSScriptRoot "validate_scene.ps1"
    $command = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $validator, $scene, "-Schema", (Join-Path $PSScriptRoot "scene_schema.json"))
    if ([bool](Get-PropertyValue $Arguments "checkAssets" $false)) { $command += "-CheckAssets" }
    $process = Invoke-ExternalProcess "validate_scene" (Get-Command powershell.exe).Source $command $root $TimeoutSeconds $StepDir
    $artifacts = @()
    foreach ($path in @($process.stdoutPath, $process.stderrPath)) {
        $artifact = Add-Artifact $path "validation_log"
        if ($null -ne $artifact) { $artifacts += $artifact }
    }
    return [ordered]@{
        success = ($process.status -eq "passed")
        action = "validate_scene"
        projectPath = if ($projectFull) { Get-RelativePath $projectFull } else { "" }
        scenePath = Get-RelativePath $scene
        exitCode = $process.exitCode
        logSummary = Get-OutputTail ($process.stdout + "`n" + $process.stderr)
        diagnostics = @(Get-LogDiagnostics ($process.stdout + "`n" + $process.stderr))
        artifacts = @($artifacts)
    }
}

function Invoke-ApplySceneCommandsAction($Arguments, [string]$StepDir, [int]$TimeoutSeconds, [bool]$AllowDestructive) {
    $sceneValue = [string](Get-PropertyValue $Arguments "scene" (Get-PropertyValue $Arguments "scenePath" ""))
    if (-not $sceneValue) { throw "apply_scene_commands 需要 scene" }
    $projectValue = [string](Get-PropertyValue $Arguments "projectPath" (Get-PropertyValue $Arguments "project" ""))
    $projectFull = if ($projectValue) { Resolve-ProjectDirectory $projectValue $true } else { "" }
    $scene = Resolve-WorkflowScenePath $sceneValue $projectFull
    $hasCommands = Test-Property $Arguments "commands"
    $commandsPathValue = [string](Get-PropertyValue $Arguments "commandsPath" "")
    if ($hasCommands -and $commandsPathValue) { throw "commands 与 commandsPath 只能二选一" }
    if (-not $hasCommands -and -not $commandsPathValue) { throw "必须提供 commands 或 commandsPath" }
    if ($hasCommands) {
        $commandsPath = Join-Path $StepDir "commands.request.json"
        $document = [ordered]@{ schemaVersion = 1; commands = @((Get-PropertyValue $Arguments "commands" @())) }
        Write-JsonFile $commandsPath $document
    } else {
        $commandsPath = Resolve-ProjectPath $commandsPathValue $true
    }
    $inPlace = [bool](Get-PropertyValue $Arguments "inPlace" $false)
    if ($inPlace -and -not $AllowDestructive) { throw "apply_scene_commands inPlace=true 需要 workflow.allowDestructive=true" }
    $outputPathValue = [string](Get-PropertyValue $Arguments "outputPath" "")
    $outputPath = if ($outputPathValue) { Resolve-ProjectPath $outputPathValue $false } else { Join-Path $StepDir "scene.generated.json" }
    $sceneScript = Join-Path $PSScriptRoot "scene_command.ps1"
    $testLayer = [string](Get-PropertyValue $Arguments "testLayer" "none")
    if ($testLayer -notin @("none", "validate", "gameplay", "render", "all")) { throw "不支持的 testLayer: $testLayer" }
    $command = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $sceneScript,
        "-ScenePath", $scene, "-CommandsPath", $commandsPath,
        "-OutputPath", $outputPath, "-TestLayer", $testLayer,
        "-TestTimeoutSeconds", [string][int](Get-PropertyValue $Arguments "testTimeoutSeconds" 120),
        "-BuildTimeoutSeconds", [string][int](Get-PropertyValue $Arguments "buildTimeoutSeconds" 600)
    )
    if ($inPlace) { $command += "-InPlace" }
    if ([bool](Get-PropertyValue $Arguments "checkAssets" $true)) { $command += "-CheckAssets" }
    if ([bool](Get-PropertyValue $Arguments "skipTestBuild" $false)) { $command += "-SkipTestBuild" }
    $process = Invoke-ExternalProcess "apply_scene_commands" (Get-Command powershell.exe).Source $command $root $TimeoutSeconds $StepDir
    $combined = $process.stdout + "`n" + $process.stderr
    $nestedResultValue = Get-ResultPathMarker $combined
    $nestedResult = $null
    $nestedResultPath = $null
    if ($nestedResultValue) {
        try {
            $nestedResultPath = Resolve-ProjectPath $nestedResultValue $true
            $nestedResult = [System.IO.File]::ReadAllText($nestedResultPath, $utf8NoBom) | ConvertFrom-Json
        } catch {}
    }
    $success = ($process.status -eq "passed" -and $null -ne $nestedResult -and [bool](Get-PropertyValue $nestedResult "success" $false))
    $artifacts = @()
    foreach ($path in @($process.stdoutPath, $process.stderrPath, $commandsPath, $nestedResultPath, $outputPath)) {
        $artifact = Add-Artifact $path "scene_workflow"
        if ($null -ne $artifact) { $artifacts += $artifact }
    }
    if ($null -ne $nestedResult) {
        foreach ($propertyName in @("candidateScene", "sourceBackup", "destinationBackup")) {
            $value = Get-PropertyValue $nestedResult $propertyName $null
            if ($value -is [string]) {
                $artifact = Add-Artifact $value "scene_workflow"
                if ($null -ne $artifact) { $artifacts += $artifact }
            } elseif ($null -ne $value) {
                $pathValue = [string](Get-PropertyValue $value "path" "")
                $artifact = Add-Artifact $pathValue "scene_workflow"
                if ($null -ne $artifact) { $artifacts += $artifact }
            }
        }
    }
    return [ordered]@{
        success = $success
        action = "apply_scene_commands"
        projectPath = if ($projectFull) { Get-RelativePath $projectFull } else { "" }
        scenePath = Get-RelativePath $scene
        commandsPath = Get-RelativePath $commandsPath
        outputScene = if ($null -ne $nestedResult) { [string](Get-PropertyValue $nestedResult "outputScene" (Get-RelativePath $outputPath)) } else { Get-RelativePath $outputPath }
        nestedResultPath = if ($null -ne $nestedResultPath) { Get-RelativePath $nestedResultPath } else { "" }
        validation = if ($null -ne $nestedResult) { Get-PropertyValue $nestedResult "validation" $null } else { $null }
        regression = if ($null -ne $nestedResult) { Get-PropertyValue $nestedResult "regression" $null } else { $null }
        logSummary = Get-OutputTail $combined
        diagnostics = @(Get-LogDiagnostics $combined)
        artifacts = @($artifacts)
    }
}

function Invoke-RunTestAction($Arguments, [string]$Action, [string]$StepDir, [int]$TimeoutSeconds) {
    $sceneValue = [string](Get-PropertyValue $Arguments "scene" (Get-PropertyValue $Arguments "scenePath" ""))
    if (-not $sceneValue) { throw "$Action 需要 scene" }
    $projectValue = [string](Get-PropertyValue $Arguments "projectPath" (Get-PropertyValue $Arguments "project" ""))
    $projectFull = if ($projectValue) { Resolve-ProjectDirectory $projectValue $true } else { "" }
    $scene = Resolve-WorkflowScenePath $sceneValue $projectFull
    $game = [string](Get-PropertyValue $Arguments "game" "")
    $frames = [int](Get-PropertyValue $Arguments "frames" 120)
    $fixedDt = [double](Get-PropertyValue $Arguments "fixedDeltaSeconds" (1.0 / 60.0))
    $timeoutMs = [int](Get-PropertyValue $Arguments "timeoutMs" 60000)
    if ($frames -lt 1 -or $frames -gt 1000000) { throw "frames 必须在 1..1000000" }
    if ($fixedDt -le 0.0 -or $fixedDt -gt 0.1) { throw "fixedDeltaSeconds 必须在 (0, 0.1]" }
    if ($timeoutMs -lt 1000 -or $timeoutMs -gt 3600000) { throw "timeoutMs 必须在 1000..3600000" }
    $isRender = ($Action -eq "run_render_test")
    $screenshotFrame = [int](Get-PropertyValue $Arguments "screenshotFrame" 0)
    if ($screenshotFrame -lt 0 -or $screenshotFrame -gt 1000000) { throw "screenshotFrame 必须在 0..1000000" }
    if (-not $isRender -and $screenshotFrame -gt 0) { throw "screenshotFrame 只支持 run_render_test" }
    if ($screenshotFrame -gt $frames) { throw "screenshotFrame 必须不大于 frames" }
    $executable = if ($isRender) { Join-Path $buildDir "EngineMain.exe" } else { Join-Path $buildDir "MikanTestRunner.exe" }
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "测试入口不存在: $executable；先执行对应 build" }
    $running = @(Get-RunningEngineProcesses)
    if ($running.Count -gt 0) { throw "当前项目已有运行时/测试进程（PID $($running.Id -join ',')），请先 stop_engine" }

    $dumpValue = [string](Get-PropertyValue $Arguments "dumpStatePath" "")
    $dumpPath = if ($dumpValue) { Resolve-ProjectPath $dumpValue $false } else { Join-Path $StepDir "state.json" }
    if ((Test-Path -LiteralPath $dumpPath -PathType Leaf) -and -not [bool](Get-PropertyValue $Arguments "overwriteDump" $false)) {
        throw "dumpStatePath 已存在；请换路径或设置 overwriteDump=true: $dumpPath"
    }
    if (Test-Path -LiteralPath $dumpPath -PathType Leaf) { Remove-Item -LiteralPath $dumpPath -Force }
    $dumpParent = Split-Path -Parent $dumpPath
    if ($dumpParent) { New-Item -ItemType Directory -Path $dumpParent -Force | Out-Null }
    $crashPath = Join-Path $StepDir "crash.log"
    $screenshotPath = ""
    $screenshotMetadataPath = ""
    if ($isRender -and $screenshotFrame -gt 0) {
        $screenshotPath = Join-Path $StepDir ("frame-{0:D4}.png" -f $screenshotFrame)
        $screenshotMetadataPath = [System.IO.Path]::ChangeExtension($screenshotPath, ".json")
    }
    $command = New-Object 'System.Collections.Generic.List[string]'
    if ($isRender) { [void]$command.Add("--headless") }
    if ($projectFull) { [void]$command.Add("--project"); [void]$command.Add($projectFull) }
    [void]$command.Add("--frames"); [void]$command.Add([string]$frames)
    [void]$command.Add("--fixed-dt"); [void]$command.Add($fixedDt.ToString("0.#########", [System.Globalization.CultureInfo]::InvariantCulture))
    [void]$command.Add("--dump-state"); [void]$command.Add($dumpPath)
    [void]$command.Add("--crash-log"); [void]$command.Add($crashPath)
    if ($isRender -and $screenshotFrame -gt 0) {
        [void]$command.Add("--screenshot-frame"); [void]$command.Add([string]$screenshotFrame)
        [void]$command.Add("--screenshot-path"); [void]$command.Add($screenshotPath)
    }
    foreach ($extra in @((Get-PropertyValue $Arguments "extraArgs" @()))) {
        if (Test-ReservedExtraArgument ([string]$extra)) { throw "extraArgs 不能覆盖 workflow 保留参数: $extra" }
        [void]$command.Add([string]$extra)
    }
    $hasProjectManagerOverride = @($command.ToArray() | Where-Object {
        [string]$_ -eq "--no-project-manager" -or [string]$_ -eq "--project" -or [string]$_ -like "--project=*"
    }).Count -gt 0
    if ($isRender -and -not $hasProjectManagerOverride) { [void]$command.Add("--no-project-manager") }
    [void]$command.Add("--scene"); [void]$command.Add($scene)
    if ($game) { [void]$command.Add("--game"); [void]$command.Add($game) }
    $processTimeout = [Math]::Max(1, [int][Math]::Ceiling($timeoutMs / 1000.0))
    if ($processTimeout -gt $TimeoutSeconds) { $processTimeout = $TimeoutSeconds }
    $process = Invoke-ExternalProcess $Action $executable $command.ToArray() $buildDir $processTimeout $StepDir
    $dumpExists = Test-Path -LiteralPath $dumpPath -PathType Leaf
    $crashExists = Test-Path -LiteralPath $crashPath -PathType Leaf
    $combined = $process.stdout + "`n" + $process.stderr
    $diagnostics = @(Get-LogDiagnostics $combined)
    $severe = @($combined -split "`r?`n" | Where-Object { $_ -match '(?i)(\[ERR\]|\[FTL\]|\[Crash\]|\bERROR\b|\bFatal\b|validation error)' }).Count -gt 0
    $failed = ($process.status -ne "passed" -or $crashExists -or -not $dumpExists -or $severe)
    $screenshotExists = $false
    $screenshotMetadataExists = $false
    $screenshotSummary = $null
    if ($screenshotFrame -gt 0) {
        $screenshotExists = Test-Path -LiteralPath $screenshotPath -PathType Leaf
        $screenshotMetadataExists = Test-Path -LiteralPath $screenshotMetadataPath -PathType Leaf
        if (-not $screenshotExists) {
            $failed = $true
            $diagnostics += "截图未生成: $screenshotPath"
        }
        if (-not $screenshotMetadataExists) {
            $failed = $true
            $diagnostics += "截图元数据未生成: $screenshotMetadataPath"
        } else {
            try {
                $screenshotSummary = [System.IO.File]::ReadAllText($screenshotMetadataPath, $utf8NoBom) | ConvertFrom-Json
                if (-not [bool](Get-PropertyValue $screenshotSummary "runtimeReady" $false)) {
                    $failed = $true
                    $diagnostics += "截图对应运行时尚未 ready: $([string](Get-PropertyValue $screenshotSummary 'readinessStatus' 'unknown'))"
                }
                $visualStatus = [string](Get-PropertyValue $screenshotSummary "visualStatus" "")
                if ($visualStatus -in @("startup-black-screen", "runtime-ready-but-black")) {
                    $failed = $true
                    $diagnostics += "截图黑屏诊断: visualStatus=$visualStatus"
                }
            } catch {
                $failed = $true
                $diagnostics += "截图元数据 JSON 解析失败: $($_.Exception.Message)"
            }
        }
    }
    $dumpSummary = $null
    if ($dumpExists) {
        try {
            $dump = [System.IO.File]::ReadAllText($dumpPath, $utf8NoBom) | ConvertFrom-Json
            $dumpSummary = [ordered]@{ frames = Get-PropertyValue $dump "frames" $null; entityCount = Get-PropertyValue $dump "entity_count" $null; game = Get-PropertyValue $dump "game" ""; runtimeLayer = Get-PropertyValue $dump "runtime_layer" "" }
            $script:lastDumpPath = $dumpPath
        } catch {
            $failed = $true
            $diagnostics += "状态 Dump JSON 解析失败: $($_.Exception.Message)"
        }
    }
    $artifacts = @()
    foreach ($path in @($process.stdoutPath, $process.stderrPath, $dumpPath, $crashPath)) {
        $artifact = Add-Artifact $path "test_artifact"
        if ($null -ne $artifact) { $artifacts += $artifact }
    }
    if ($screenshotFrame -gt 0) {
        $artifact = Add-Artifact $screenshotPath "engine_screenshot"
        if ($null -ne $artifact) { $artifacts += $artifact }
        $artifact = Add-Artifact $screenshotMetadataPath "engine_screenshot_metadata"
        if ($null -ne $artifact) { $artifacts += $artifact }
    }
    return [ordered]@{
        success = (-not $failed)
        action = $Action
        layer = if ($isRender) { "rendering" } else { "gameplay" }
        runtimeLayer = if ($null -ne $dumpSummary) { [string]$dumpSummary.runtimeLayer } else { "" }
        projectPath = if ($projectFull) { Get-RelativePath $projectFull } else { "" }
        scene = Get-RelativePath $scene
        game = $game
        frames = $frames
        fixedDeltaSeconds = $fixedDt
        exitCode = $process.exitCode
        timedOut = $process.timedOut
        dumpPath = Get-RelativePath $dumpPath
        crashPath = Get-RelativePath $crashPath
        screenshotFrame = $screenshotFrame
        screenshotPath = if ($screenshotExists) { Get-RelativePath $screenshotPath } else { "" }
        screenshotMetadataPath = if ($screenshotMetadataExists) { Get-RelativePath $screenshotMetadataPath } else { "" }
        screenshot = $screenshotSummary
        dump = $dumpSummary
        logSummary = Get-OutputTail $combined
        diagnostics = @($diagnostics)
        artifacts = @($artifacts)
    }
}

function Get-DumpObject([string]$Path) {
    $full = Resolve-ProjectPath $Path $true
    try { return [System.IO.File]::ReadAllText($full, $utf8NoBom) | ConvertFrom-Json }
    catch { throw "dump JSON 无法解析: $full；$($_.Exception.Message)" }
}

function Get-DumpEntityName($Entity) {
    $value = Get-PropertyValue $Entity "name" ""
    if ($value -is [string]) { return [string]$value }
    return [string](Get-PropertyValue $value "name" "")
}

function Invoke-ReadDumpAction($Arguments, [string]$StepDir) {
    $pathValue = [string](Get-PropertyValue $Arguments "path" "")
    if (-not $pathValue) { $pathValue = [string]$script:lastDumpPath }
    if (-not $pathValue) { throw "read_dump 缺少 path，且此前没有成功的测试 dump" }
    $full = Resolve-ProjectPath $pathValue $true
    $dump = Get-DumpObject $full
    $entities = @((Get-PropertyValue $dump "entities" @()))
    $artifact = Add-Artifact $full "state_dump"
    return [ordered]@{
        success = $true
        action = "read_dump"
        dumpPath = Get-RelativePath $full
        runtimeLayer = [string](Get-PropertyValue $dump "runtime_layer" "")
        frames = Get-PropertyValue $dump "frames" $null
        entityCount = Get-PropertyValue $dump "entity_count" $entities.Count
        game = [string](Get-PropertyValue $dump "game" "")
        entityNames = @($entities | ForEach-Object { Get-DumpEntityName $_ } | Where-Object { $_ })
        state = $dump
        artifacts = if ($null -ne $artifact) { @($artifact) } else { @() }
    }
}

function Invoke-AssertStateAction($Arguments, [string]$StepDir) {
    $pathValue = [string](Get-PropertyValue $Arguments "path" "")
    if (-not $pathValue) { $pathValue = [string]$script:lastDumpPath }
    if (-not $pathValue) { throw "assert_state 缺少 path，且此前没有成功的测试 dump" }
    $full = Resolve-ProjectPath $pathValue $true
    $dump = Get-DumpObject $full
    $assertions = @((Get-PropertyValue $Arguments "assertions" @()))
    if ($assertions.Count -eq 0) { throw "assert_state 至少需要一项 assertions" }
    $allowedFields = @("pos", "wpos", "rot_deg", "scale", "visible")
    $checks = New-Object 'System.Collections.Generic.List[object]'
    $failures = 0
    $index = 0
    $entities = @((Get-PropertyValue $dump "entities" @()))
    foreach ($assertion in $assertions) {
        $index++
        $selector = ""
        $selected = @()
        if (Test-Property $assertion "entityId") {
            $id = [int64]$assertion.entityId
            $selector = "id=$id"
            $selected = @($entities | Where-Object { [int64](Get-PropertyValue $_ "id" -1) -eq $id })
        } elseif (Test-Property $assertion "entityName") {
            $name = [string]$assertion.entityName
            $selector = "name='$name'"
            $selected = @($entities | Where-Object { (Get-DumpEntityName $_) -ceq $name })
        } else {
            $failures++
            [void]$checks.Add([ordered]@{ index = $index; status = "failed"; error = "缺少 entityId 或 entityName" })
            continue
        }
        $field = [string](Get-PropertyValue $assertion "field" "")
        if ($selected.Count -ne 1) {
            $failures++
            [void]$checks.Add([ordered]@{ index = $index; selector = $selector; field = $field; status = "failed"; error = "$selector 匹配 $($selected.Count) 个实体（必须唯一）" })
            continue
        }
        if ($field -notin $allowedFields) {
            $failures++
            [void]$checks.Add([ordered]@{ index = $index; selector = $selector; field = $field; status = "failed"; error = "不支持字段 '$field'（允许: $($allowedFields -join ', ')）" })
            continue
        }
        if (-not (Test-Property $assertion "expected")) {
            $failures++
            [void]$checks.Add([ordered]@{ index = $index; selector = $selector; field = $field; status = "failed"; error = "缺少 expected" })
            continue
        }
        $entity = $selected[0]
        $actual = Get-PropertyValue $entity $field $null
        $expected = Get-PropertyValue $assertion "expected" $null
        $tolerance = [double](Get-PropertyValue $assertion "tolerance" 0.001)
        if ($tolerance -lt 0.0) { throw "assertion #$index tolerance 不能为负数" }
        $passed = $true
        if ($field -eq "visible") {
            $passed = ($expected -is [bool] -and [bool]$actual -eq [bool]$expected)
        } else {
            $actualValues = @($actual)
            $expectedValues = @($expected)
            if ($actualValues.Count -ne $expectedValues.Count) { $passed = $false }
            else {
                for ($valueIndex = 0; $valueIndex -lt $actualValues.Count; $valueIndex++) {
                    try {
                        if ([Math]::Abs([double]$actualValues[$valueIndex] - [double]$expectedValues[$valueIndex]) -gt $tolerance) { $passed = $false; break }
                    } catch { $passed = $false; break }
                }
            }
        }
        if (-not $passed) { $failures++ }
        [void]$checks.Add([ordered]@{
            index = $index
            selector = $selector
            field = $field
            status = if ($passed) { "passed" } else { "failed" }
            actual = $actual
            expected = $expected
            tolerance = $tolerance
        })
    }
    $artifact = Add-Artifact $full "state_dump"
    return [ordered]@{
        success = ($failures -eq 0)
        action = "assert_state"
        dumpPath = Get-RelativePath $full
        passed = $assertions.Count - $failures
        failed = $failures
        assertions = $checks.ToArray()
        artifacts = if ($null -ne $artifact) { @($artifact) } else { @() }
    }
}

function Invoke-StopEngineAction {
    $running = @(Get-RunningEngineProcesses)
    if ($running.Count -eq 0) {
        return [ordered]@{ success = $true; action = "stop_engine"; stopped = 0; remaining = 0; artifacts = @() }
    }
    $ids = @($running.Id)
    foreach ($process in $running) { try { Stop-Process -Id $process.Id -Force } catch {} }
    Start-Sleep -Milliseconds 500
    $remaining = @(Get-RunningEngineProcesses)
    return [ordered]@{ success = ($remaining.Count -eq 0); action = "stop_engine"; stopped = $ids.Count; pids = $ids; remaining = $remaining.Count; artifacts = @() }
}

function Invoke-CaptureFrameAction($Arguments, [string]$StepDir, [int]$TimeoutSeconds) {
    $sceneValue = [string](Get-PropertyValue $Arguments "scene" (Get-PropertyValue $Arguments "scenePath" ""))
    if (-not $sceneValue) { throw "capture_frame 需要 scene" }
    $projectValue = [string](Get-PropertyValue $Arguments "projectPath" (Get-PropertyValue $Arguments "project" ""))
    $projectFull = if ($projectValue) { Resolve-ProjectDirectory $projectValue $true } else { "" }
    $scene = Resolve-WorkflowScenePath $sceneValue $projectFull
    $game = [string](Get-PropertyValue $Arguments "game" "")
    $captureFrame = [int](Get-PropertyValue $Arguments "captureFrame" 60)
    $frames = [int](Get-PropertyValue $Arguments "frames" ([Math]::Max(120, $captureFrame + 30)))
    $fixedDt = [double](Get-PropertyValue $Arguments "fixedDeltaSeconds" (1.0 / 60.0))
    $captureTimeoutSeconds = [int](Get-PropertyValue $Arguments "captureTimeoutSeconds" ([Math]::Min(3600, $TimeoutSeconds)))
    $captureWaitSeconds = [int](Get-PropertyValue $Arguments "captureWaitSeconds" 10)
    if ($captureFrame -lt 1 -or $captureFrame -gt 1000000) { throw "captureFrame 必须在 1..1000000" }
    if ($frames -lt $captureFrame -or $frames -gt 1000000) { throw "frames 必须在 captureFrame..1000000" }
    if ($fixedDt -le 0.0 -or $fixedDt -gt 0.1) { throw "fixedDeltaSeconds 必须在 (0, 0.1]" }
    if ($captureTimeoutSeconds -lt 1 -or $captureTimeoutSeconds -gt 3600) { throw "captureTimeoutSeconds 必须在 1..3600" }
    if ($captureWaitSeconds -lt 0 -or $captureWaitSeconds -gt 120) { throw "captureWaitSeconds 必须在 0..120" }

    $executable = Join-Path $buildDir "EngineMain.exe"
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "抓帧入口不存在: $executable；先执行 build target=EngineMain" }
    $running = @(Get-RunningEngineProcesses)
    if ($running.Count -gt 0) { throw "当前项目已有运行时/测试进程（PID $($running.Id -join ',')），请先 stop_engine" }

    $captureScript = Join-Path $PSScriptRoot "renderdoc_capture.ps1"
    if (-not (Test-Path -LiteralPath $captureScript -PathType Leaf)) { throw "RenderDoc 抓帧脚本不存在: $captureScript" }
    $targetArguments = New-Object 'System.Collections.Generic.List[string]'
    [void]$targetArguments.Add("--headless")
    if ($projectFull) { [void]$targetArguments.Add("--project"); [void]$targetArguments.Add($projectFull) }
    [void]$targetArguments.Add("--frames"); [void]$targetArguments.Add([string]$frames)
    [void]$targetArguments.Add("--fixed-dt"); [void]$targetArguments.Add($fixedDt.ToString("0.#########", [System.Globalization.CultureInfo]::InvariantCulture))
    if (-not $projectFull) { [void]$targetArguments.Add("--no-project-manager") }
    [void]$targetArguments.Add("--no-editor")
    foreach ($extra in @((Get-PropertyValue $Arguments "extraArgs" @()))) {
        if (Test-ReservedExtraArgument ([string]$extra)) { throw "extraArgs 不能覆盖 capture_frame 保留参数: $extra" }
        [void]$targetArguments.Add([string]$extra)
    }
    [void]$targetArguments.Add("--scene"); [void]$targetArguments.Add($scene)
    if ($game) { [void]$targetArguments.Add("--game"); [void]$targetArguments.Add($game) }

    $targetArgumentsJson = $targetArguments.ToArray() | ConvertTo-Json -Compress
    $targetArgumentsBase64 = [System.Convert]::ToBase64String(
        [System.Text.Encoding]::UTF8.GetBytes([string]$targetArgumentsJson))
    $command = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $captureScript,
        "-TargetPath", $executable,
        "-TargetArgumentsBase64", $targetArgumentsBase64,
        "-WorkingDirectory", $buildDir,
        "-CaptureFrame", [string]$captureFrame,
        "-TimeoutSeconds", [string]$captureTimeoutSeconds,
        "-CaptureWaitSeconds", [string]$captureWaitSeconds
    )
    $renderDocCmdPath = [string](Get-PropertyValue $Arguments "renderDocCmdPath" "")
    $outputRoot = [string](Get-PropertyValue $Arguments "outputRoot" "")
    if ($renderDocCmdPath) { $command += @("-RenderDocCmdPath", $renderDocCmdPath) }
    if ($outputRoot) { $command += @("-OutputRoot", $outputRoot) }
    if ([bool](Get-PropertyValue $Arguments "apiValidation" $false)) { $command += "-ApiValidation" }
    if ([bool](Get-PropertyValue $Arguments "captureCallstacks" $false)) { $command += "-CaptureCallstacks" }
    if ([bool](Get-PropertyValue $Arguments "skipThumbnail" $false)) { $command += "-SkipThumbnail" }

    $process = Invoke-ExternalProcess "capture_frame" (Get-Command powershell.exe).Source $command $root $TimeoutSeconds $StepDir
    $combined = $process.stdout + "`n" + $process.stderr
    $resultPathValue = Get-ResultPathMarker $combined "RENDERDOC_RESULT_PATH="
    $structured = $null
    $resultPath = $null
    if ($resultPathValue) {
        try {
            $resultPath = Resolve-ProjectPath $resultPathValue $true
            $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json
        } catch {
            $resultPath = $null
        }
    }
    $artifacts = @()
    foreach ($path in @($process.stdoutPath, $process.stderrPath)) {
        $artifact = Add-Artifact $path "capture_log"
        if ($null -ne $artifact) { $artifacts += $artifact }
    }
    if ($null -ne $structured) {
        foreach ($path in @($resultPath, (Get-PropertyValue $structured "manifestPath" ""))) {
            $artifact = Add-Artifact ([string]$path) "capture_result"
            if ($null -ne $artifact) { $artifacts += $artifact }
        }
        foreach ($entry in @((Get-PropertyValue $structured "artifacts" @()))) {
            $artifact = Add-Artifact ([string](Get-PropertyValue $entry "path" "")) ([string](Get-PropertyValue $entry "kind" "capture_artifact"))
            if ($null -ne $artifact) { $artifacts += $artifact }
        }
    }
    $success = ($process.status -eq "passed" -and $null -ne $structured -and [bool](Get-PropertyValue $structured "success" $false))
    $captureStatus = if ($null -ne $structured) { [string](Get-PropertyValue $structured "status" "") } else { "runner_error" }
    $nextAction = if ($null -ne $structured) { [string](Get-PropertyValue $structured "nextAction" "") } else { "读取抓帧步骤 stdout/stderr，确认 RenderDoc 脚本是否启动" }
    return [ordered]@{
        success = $success
        action = "capture_frame"
        projectPath = if ($projectFull) { Get-RelativePath $projectFull } else { "" }
        scene = Get-RelativePath $scene
        game = $game
        captureFrame = $captureFrame
        frames = $frames
        fixedDeltaSeconds = $fixedDt
        status = $captureStatus
        renderdocAvailable = if ($null -ne $structured) { [bool](Get-PropertyValue (Get-PropertyValue $structured "renderdoc" $null) "available" $false) } else { $false }
        resultPath = if ($resultPath) { Get-RelativePath $resultPath } else { "" }
        runDir = if ($null -ne $structured) { [string](Get-PropertyValue $structured "runDir" "") } else { "" }
        exitCode = $process.exitCode
        timedOut = $process.timedOut
        logSummary = Get-OutputTail $combined
        diagnostics = @(Get-LogDiagnostics $combined)
        nextAction = $nextAction
        artifacts = @($artifacts)
    }
}

function Invoke-CapturePerformanceAction($Arguments, [string]$StepDir, [int]$TimeoutSeconds) {
    $sceneValue = [string](Get-PropertyValue $Arguments "scene" (Get-PropertyValue $Arguments "scenePath" ""))
    if (-not $sceneValue) { throw "capture_performance 需要 scene" }
    $projectValue = [string](Get-PropertyValue $Arguments "projectPath" (Get-PropertyValue $Arguments "project" ""))
    $projectFull = if ($projectValue) { Resolve-ProjectDirectory $projectValue $true } else { "" }
    $scene = Resolve-WorkflowScenePath $sceneValue $projectFull
    $game = [string](Get-PropertyValue $Arguments "game" "")
    $captureType = [string](Get-PropertyValue $Arguments "captureType" "gpu_trace")
    if ($captureType -notin @("gpu_trace", "graphics_capture")) { throw "captureType 只支持 gpu_trace 或 graphics_capture" }
    $captureFrame = [int](Get-PropertyValue $Arguments "captureFrame" 60)
    $frames = [int](Get-PropertyValue $Arguments "frames" ([Math]::Max(180, $captureFrame + 30)))
    $fixedDt = [double](Get-PropertyValue $Arguments "fixedDeltaSeconds" (1.0 / 60.0))
    $frameCount = [int](Get-PropertyValue $Arguments "frameCount" 1)
    $maxDurationMilliseconds = [int](Get-PropertyValue $Arguments "maxDurationMilliseconds" 5000)
    $captureTimeoutSeconds = [int](Get-PropertyValue $Arguments "captureTimeoutSeconds" ([Math]::Min(3600, $TimeoutSeconds)))
    $traceTimeoutSeconds = [int](Get-PropertyValue $Arguments "traceTimeoutSeconds" ([Math]::Min(3600, [Math]::Max(60, $captureTimeoutSeconds))))
    $replayLoops = [int](Get-PropertyValue $Arguments "replayLoops" 3)
    $skipReplay = [bool](Get-PropertyValue $Arguments "skipReplay" $false)
    $setGpuClocks = [string](Get-PropertyValue $Arguments "setGpuClocks" "unaltered")
    if ($captureFrame -lt 1 -or $captureFrame -gt 1000000) { throw "captureFrame 必须在 1..1000000" }
    if ($frames -le $captureFrame -or $frames -gt 1000000) { throw "frames 必须大于 captureFrame 且不超过 1000000" }
    if ($fixedDt -le 0.0 -or $fixedDt -gt 0.1) { throw "fixedDeltaSeconds 必须在 (0, 0.1]" }
    if ($frameCount -lt 1 -or $frameCount -gt 60) { throw "frameCount 必须在 1..60" }
    if ($maxDurationMilliseconds -lt 1000 -or $maxDurationMilliseconds -gt 600000) { throw "maxDurationMilliseconds 必须在 1000..600000" }
    if ($captureTimeoutSeconds -lt 1 -or $captureTimeoutSeconds -gt 3600) { throw "captureTimeoutSeconds 必须在 1..3600" }
    if ($traceTimeoutSeconds -lt 1 -or $traceTimeoutSeconds -gt 3600) { throw "traceTimeoutSeconds 必须在 1..3600" }
    if ($replayLoops -lt 0 -or $replayLoops -gt 100) { throw "replayLoops 必须在 0..100" }
    if ($captureType -eq "graphics_capture" -and -not $skipReplay -and $replayLoops -eq 0) { throw "graphics_capture 在不使用 skipReplay 时 replayLoops 必须大于 0" }
    if ($setGpuClocks -notin @("unaltered", "base", "maximum")) { throw "setGpuClocks 只支持 unaltered、base 或 maximum" }

    $executable = Join-Path $buildDir "EngineMain.exe"
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "性能采集入口不存在: $executable；先执行 build target=EngineMain" }
    $running = @(Get-RunningEngineProcesses)
    if ($running.Count -gt 0) { throw "当前项目已有运行时/测试进程（PID $($running.Id -join ',')），请先 stop_engine" }

    $captureScript = Join-Path $PSScriptRoot "nsight_capture.ps1"
    if (-not (Test-Path -LiteralPath $captureScript -PathType Leaf)) { throw "Nsight 性能采集脚本不存在: $captureScript" }
    $targetArguments = New-Object 'System.Collections.Generic.List[string]'
    [void]$targetArguments.Add("--headless")
    if ($projectFull) { [void]$targetArguments.Add("--project"); [void]$targetArguments.Add($projectFull) }
    [void]$targetArguments.Add("--frames"); [void]$targetArguments.Add([string]$frames)
    [void]$targetArguments.Add("--fixed-dt"); [void]$targetArguments.Add($fixedDt.ToString("0.#########", [System.Globalization.CultureInfo]::InvariantCulture))
    if (-not $projectFull) { [void]$targetArguments.Add("--no-project-manager") }
    [void]$targetArguments.Add("--no-editor")
    foreach ($extra in @((Get-PropertyValue $Arguments "extraArgs" @()))) {
        if (Test-ReservedExtraArgument ([string]$extra)) { throw "extraArgs 不能覆盖 capture_performance 保留参数: $extra" }
        [void]$targetArguments.Add([string]$extra)
    }
    [void]$targetArguments.Add("--scene"); [void]$targetArguments.Add($(if ($projectFull) { $scene } else { Get-RelativePath $scene }))
    if ($game) { [void]$targetArguments.Add("--game"); [void]$targetArguments.Add($game) }
    $targetArgumentsJson = $targetArguments.ToArray() | ConvertTo-Json -Compress
    $targetArgumentsBase64 = [System.Convert]::ToBase64String(
        [System.Text.Encoding]::UTF8.GetBytes([string]$targetArgumentsJson))
    $command = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $captureScript,
        "-TargetPath", $executable,
        "-TargetArgumentsBase64", $targetArgumentsBase64,
        "-WorkingDirectory", $root,
        "-CaptureType", $captureType,
        "-CaptureFrame", [string]$captureFrame,
        "-FrameCount", [string]$frameCount,
        "-MaxDurationMilliseconds", [string]$maxDurationMilliseconds,
        "-TimeoutSeconds", [string]$captureTimeoutSeconds,
        "-TraceTimeoutSeconds", [string]$traceTimeoutSeconds,
        "-ReplayLoops", [string]$replayLoops,
        "-SetGpuClocks", $setGpuClocks
    )
    $nsightPath = [string](Get-PropertyValue $Arguments "nsightPath" "")
    $outputRoot = [string](Get-PropertyValue $Arguments "outputRoot" "")
    if ($nsightPath) { $command += @("-NsightPath", $nsightPath) }
    if ($outputRoot) { $command += @("-OutputRoot", $outputRoot) }
    if ($skipReplay) { $command += "-SkipReplay" }

    $process = Invoke-ExternalProcess "capture_performance" (Get-Command powershell.exe).Source $command $root $TimeoutSeconds $StepDir
    $combined = $process.stdout + "`n" + $process.stderr
    $resultPathValue = Get-ResultPathMarker $combined "NSIGHT_RESULT_PATH="
    $structured = $null
    $resultPath = $null
    if ($resultPathValue) {
        try {
            $resultPath = Resolve-ProjectPath $resultPathValue $true
            $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json
        } catch { $resultPath = $null }
    }
    $artifacts = @()
    foreach ($path in @($process.stdoutPath, $process.stderrPath)) {
        $artifact = Add-Artifact $path "capture_log"
        if ($null -ne $artifact) { $artifacts += $artifact }
    }
    if ($null -ne $structured) {
        foreach ($path in @($resultPath, (Get-PropertyValue $structured "manifestPath" ""))) {
            $artifact = Add-Artifact ([string]$path) "capture_result"
            if ($null -ne $artifact) { $artifacts += $artifact }
        }
        foreach ($entry in @((Get-PropertyValue $structured "artifacts" @()))) {
            $artifact = Add-Artifact ([string](Get-PropertyValue $entry "path" "")) ([string](Get-PropertyValue $entry "kind" "performance_artifact"))
            if ($null -ne $artifact) { $artifacts += $artifact }
        }
    }
    $success = ($process.status -eq "passed" -and $null -ne $structured -and [bool](Get-PropertyValue $structured "success" $false))
    $nsight = if ($null -ne $structured) { Get-PropertyValue $structured "nsight" $null } else { $null }
    return [ordered]@{
        success = $success
        action = "capture_performance"
        projectPath = if ($projectFull) { Get-RelativePath $projectFull } else { "" }
        captureType = $captureType
        scene = Get-RelativePath $scene
        game = $game
        captureFrame = $captureFrame
        frames = $frames
        fixedDeltaSeconds = $fixedDt
        frameCount = $frameCount
        status = if ($null -ne $structured) { [string](Get-PropertyValue $structured "status" "") } else { "runner_error" }
        nsightAvailable = if ($null -ne $nsight) { [bool](Get-PropertyValue $nsight "available" $false) } else { $false }
        nsightIsAdministrator = if ($null -ne $nsight) { [bool](Get-PropertyValue $nsight "isAdministrator" $false) } else { $false }
        resultPath = if ($resultPath) { Get-RelativePath $resultPath } else { "" }
        runDir = if ($null -ne $structured) { [string](Get-PropertyValue $structured "runDir" "") } else { "" }
        performance = if ($null -ne $structured) { Get-PropertyValue $structured "performance" $null } else { $null }
        replay = if ($null -ne $structured) { Get-PropertyValue $structured "replay" $null } else { $null }
        exitCode = $process.exitCode
        timedOut = $process.timedOut
        logSummary = Get-OutputTail $combined
        diagnostics = @(Get-LogDiagnostics $combined)
        nextAction = if ($null -ne $structured) { [string](Get-PropertyValue $structured "nextAction" "") } else { "读取 capture_performance stdout/stderr，确认 Nsight 脚本是否启动" }
        artifacts = @($artifacts)
    }
}

function Invoke-DiscoveryAction([string]$Action, $Arguments, [string]$StepDir, [int]$TimeoutSeconds) {
    $mode = switch ($Action) {
        "inspect_project" { "project"; break }
        "get_project_context" { "context"; break }
        "inspect_scene" { "scene"; break }
        "query_assets" { "assets"; break }
        default { throw "不是 discovery action: $Action" }
    }
    $discoveryScript = Join-Path $PSScriptRoot "agent_discovery.ps1"
    if (-not (Test-Path -LiteralPath $discoveryScript -PathType Leaf)) { throw "Agent discovery 脚本不存在: $discoveryScript" }
    $command = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $discoveryScript,
        "-Mode", $mode,
        "-RunId", ("workflow-discovery-" + [Guid]::NewGuid().ToString("N").Substring(0, 12))
    )
    $outputRoot = [string](Get-PropertyValue $Arguments "outputRoot" "out\agent_discovery")
    $maxResults = [int](Get-PropertyValue $Arguments "maxResults" 200)
    if ($maxResults -lt 1 -or $maxResults -gt 2000) { throw "discovery maxResults 必须在 1..2000" }
    $command += @("-OutputRoot", $outputRoot, "-MaxResults", [string]$maxResults)
    $projectPath = [string](Get-PropertyValue $Arguments "projectPath" (Get-PropertyValue $Arguments "project" ""))
    if ($mode -eq "context" -and -not $projectPath) { throw "get_project_context 需要 projectPath" }
    if ($projectPath) {
        [void](Resolve-ProjectDirectory $projectPath $true)
        $command += @("-ProjectPath", $projectPath)
    }
    if ($mode -eq "scene") {
        $sceneValue = [string](Get-PropertyValue $Arguments "scene" (Get-PropertyValue $Arguments "scenePath" ""))
        if (-not $sceneValue) { throw "inspect_scene 需要 scene 或 scenePath" }
        $projectFull = if ($projectPath) { Resolve-ProjectDirectory $projectPath $true } else { "" }
        [void](Resolve-WorkflowScenePath $sceneValue $projectFull)
        $maxEntities = [int](Get-PropertyValue $Arguments "maxEntities" 500)
        $maxReferences = [int](Get-PropertyValue $Arguments "maxAssetReferences" 200)
        if ($maxEntities -lt 1 -or $maxEntities -gt 5000) { throw "inspect_scene maxEntities 必须在 1..5000" }
        if ($maxReferences -lt 1 -or $maxReferences -gt 5000) { throw "inspect_scene maxAssetReferences 必须在 1..5000" }
        $command += @("-ScenePath", $sceneValue, "-MaxEntities", [string]$maxEntities, "-MaxAssetReferences", [string]$maxReferences)
    } elseif ($mode -eq "assets" -or $mode -eq "context") {
        $query = [string](Get-PropertyValue $Arguments "query" "")
        $assetType = [string](Get-PropertyValue $Arguments "assetType" "all")
        if ($assetType -notin @("all", "scenes", "scripts", "shaders", "models", "textures", "audio")) { throw "query_assets assetType 不支持: $assetType" }
        $command += @("-Query", $query, "-AssetType", $assetType)
    }
    $process = Invoke-ExternalProcess $Action (Get-Command powershell.exe).Source $command $root $TimeoutSeconds $StepDir
    $combined = $process.stdout + "`n" + $process.stderr
    $resultPathValue = Get-ResultPathMarker $combined "DISCOVERY_RESULT_PATH="
    $structured = $null
    $resultPath = $null
    if ($resultPathValue) {
        try {
            $resultPath = Resolve-ProjectPath $resultPathValue $true
            $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json
        } catch { $resultPath = $null }
    }
    $artifacts = @()
    foreach ($path in @($process.stdoutPath, $process.stderrPath)) {
        $artifact = Add-Artifact $path "discovery_log"
        if ($null -ne $artifact) { $artifacts += $artifact }
    }
    if ($null -ne $structured) {
        foreach ($path in @($resultPath, (Get-PropertyValue $structured "manifestPath" ""))) {
            $artifact = Add-Artifact ([string]$path) "discovery_result"
            if ($null -ne $artifact) { $artifacts += $artifact }
        }
        foreach ($entry in @((Get-PropertyValue $structured "artifacts" @()))) {
            $artifact = Add-Artifact ([string](Get-PropertyValue $entry "path" "")) ([string](Get-PropertyValue $entry "kind" "discovery_input"))
            if ($null -ne $artifact) { $artifacts += $artifact }
        }
    }
    $success = ($process.status -eq "passed" -and $null -ne $structured -and [bool](Get-PropertyValue $structured "success" $false))
    $context = if ($null -ne $structured) {
        [ordered]@{
            mode = [string](Get-PropertyValue $structured "mode" $mode)
            project = Get-PropertyValue $structured "project" $null
            projectContext = Get-PropertyValue $structured "projectContext" $null
            scene = Get-PropertyValue $structured "scene" $null
            assets = Get-PropertyValue $structured "assets" $null
            schema = Get-PropertyValue $structured "schema" $null
            capabilities = Get-PropertyValue $structured "capabilities" $null
        }
    } else { $null }
    return [ordered]@{
        success = $success
        action = $Action
        mode = $mode
        status = if ($success) { "ready" } else { "failed" }
        resultPath = if ($resultPath) { Get-RelativePath $resultPath } else { "" }
        context = $context
        exitCode = $process.exitCode
        timedOut = $process.timedOut
        logSummary = Get-OutputTail $combined
        diagnostics = @(Get-LogDiagnostics $combined)
        nextAction = if ($null -ne $structured) { [string](Get-PropertyValue $structured "nextAction" "") } else { "读取 discovery stdout/stderr，确认输入路径和 JSON 解析错误" }
        artifacts = @($artifacts)
    }
}

function Invoke-WorkflowAction([string]$Action, $Arguments, [string]$StepDir, [int]$TimeoutSeconds, [bool]$AllowDestructive) {
    switch ($Action) {
        "inspect_project" { return Invoke-DiscoveryAction $Action $Arguments $StepDir $TimeoutSeconds }
        "get_project_context" { return Invoke-DiscoveryAction $Action $Arguments $StepDir $TimeoutSeconds }
        "inspect_scene" { return Invoke-DiscoveryAction $Action $Arguments $StepDir $TimeoutSeconds }
        "query_assets" { return Invoke-DiscoveryAction $Action $Arguments $StepDir $TimeoutSeconds }
        "create_script" { return Invoke-CreateScriptAction $Arguments $StepDir $TimeoutSeconds $AllowDestructive }
        "build" { return Invoke-BuildAction $Arguments $StepDir $TimeoutSeconds }
        "compile_games" { return Invoke-CompileGamesAction $Arguments $StepDir $TimeoutSeconds }
        "validate_scene" { return Invoke-ValidateSceneAction $Arguments $StepDir $TimeoutSeconds }
        "apply_scene_commands" { return Invoke-ApplySceneCommandsAction $Arguments $StepDir $TimeoutSeconds $AllowDestructive }
        "run_gameplay_test" { return Invoke-RunTestAction $Arguments $Action $StepDir $TimeoutSeconds }
        "run_render_test" { return Invoke-RunTestAction $Arguments $Action $StepDir $TimeoutSeconds }
        "capture_frame" { return Invoke-CaptureFrameAction $Arguments $StepDir $TimeoutSeconds }
        "capture_performance" { return Invoke-CapturePerformanceAction $Arguments $StepDir $TimeoutSeconds }
        "read_dump" { return Invoke-ReadDumpAction $Arguments $StepDir }
        "assert_state" { return Invoke-AssertStateAction $Arguments $StepDir }
        "stop_engine" { return Invoke-StopEngineAction }
        default { throw "不支持的 workflow action: $Action" }
    }
}

function Save-WorkflowResult([bool]$Success, [string]$ErrorText = "", [string]$NextAction = "") {
    if ($null -eq $script:resultPath) { return }
    $endedAt = Get-Date
    $records = $script:stepRecords.ToArray()
    $passed = @($records | Where-Object { $_.status -eq "passed" }).Count
    $failed = @($records | Where-Object { $_.status -eq "failed" }).Count
    $skipped = @($records | Where-Object { $_.status -eq "skipped" }).Count
    $planned = @($records | Where-Object { $_.status -eq "planned" }).Count
    $summary = [ordered]@{
        schemaVersion = 1
        tool = "agent_workflow"
        apiVersion = 1
        success = $Success
        dryRun = [bool]$DryRun
        runId = [string]$script:workflowContext.runId
        runDir = Get-RelativePath $script:runDir
        workflowName = [string]$script:workflowContext.workflowName
        workflowPath = Get-RelativePath $script:workflowContext.workflowPath
        startedAt = $script:startedAt.ToString("o")
        endedAt = $endedAt.ToString("o")
        durationMs = [int](($endedAt - $script:startedAt).TotalMilliseconds)
        counts = [ordered]@{ total = $records.Count; passed = $passed; failed = $failed; skipped = $skipped; planned = $planned }
        continueOnFailure = [bool]$script:workflowContext.continueOnFailure
        allowDestructive = [bool]$script:workflowContext.allowDestructive
        steps = $records
        artifacts = $script:artifacts.ToArray()
        error = $ErrorText
        nextAction = if ($NextAction) { $NextAction } elseif ($Success -and $DryRun) { "移除 dry-run 后执行该 workflow" } elseif ($Success) { "workflow_completed" } else { "读取失败步骤的日志并修复后重试" }
    }
    Write-JsonFile $script:resultPath $summary
}

function Add-SkippedStep([int]$Index, $Spec, [string]$Reason) {
    $id = [string](Get-PropertyValue $Spec "id" ("step_" + $Index))
    $action = [string](Get-PropertyValue $Spec "action" "")
    $record = [ordered]@{
        index = $Index
        id = $id
        action = $action
        status = "skipped"
        reason = $Reason
        startedAt = (Get-Date).ToString("o")
        endedAt = (Get-Date).ToString("o")
        durationMs = 0
        result = $null
        artifacts = @()
        failureCategory = "workflow_error"
        nextAction = "修复阻塞步骤后重新执行 workflow"
    }
    [void]$script:stepRecords.Add([pscustomobject]$record)
    $script:stepRecordMap[$id] = [pscustomobject]$record
}

function Write-RunManifest {
    $manifestPath = Join-Path $script:runDir "manifest.json"
    $entries = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse | Where-Object { $_.Name -notin @("manifest.json", "result.json") })) {
        $entries += [ordered]@{
            path = Get-RelativePath $file.FullName
            size = [int64]$file.Length
            sha256 = Get-Sha256 $file.FullName
        }
    }
    $manifest = [ordered]@{
        schemaVersion = 1
        operation = "agent_workflow"
        runId = [string]$script:workflowContext.runId
        createdAt = (Get-Date).ToString("o")
        files = $entries
    }
    Write-JsonFile $manifestPath $manifest
    foreach ($entry in @($manifest.files)) {
        $full = Resolve-ProjectPath ([string]$entry.path) $true
        if ((Get-Sha256 $full) -ne [string]$entry.sha256) { throw "workflow manifest 校验失败: $($entry.path)" }
    }
    return $manifestPath
}

$script:workflowContext = $null
$abortRemaining = $false
$fatalError = ""
$finalSuccess = $false
$finalNextAction = ""

try {
    $workflowFull = Resolve-ProjectPath $WorkflowPath $true
    $rawWorkflow = [System.IO.File]::ReadAllText($workflowFull, $utf8NoBom)
    $workflow = $rawWorkflow | ConvertFrom-Json
    if ($null -eq $workflow) { throw "workflow JSON 为空" }
    $version = [int](Get-PropertyValue $workflow "schemaVersion" 0)
    if ($version -ne 1) { throw "workflow schemaVersion 必须为 1" }
    $stepSpecs = @((Get-PropertyValue $workflow "steps" @()))
    if ($stepSpecs.Count -lt 1 -or $stepSpecs.Count -gt 64) { throw "workflow steps 数量必须在 1..64" }
    $defaults = Get-PropertyValue $workflow "defaults" $null
    $defaultTimeout = [int](Get-PropertyValue $defaults "timeoutSeconds" 600)
    if ($defaultTimeout -lt 1 -or $defaultTimeout -gt 7200) { throw "defaults.timeoutSeconds 必须在 1..7200" }
    $workflowContinue = [bool](Get-PropertyValue $workflow "continueOnFailure" $false)
    if ($ContinueOnFailure) { $workflowContinue = $true }
    $allowDestructive = [bool](Get-PropertyValue $workflow "allowDestructive" $false)
    $outputRootFull = Resolve-ProjectPath $OutputRoot $false
    if ([string]::IsNullOrWhiteSpace($RunId)) {
        $RunId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8))
    }
    if ($RunId -notmatch '^[A-Za-z0-9_.-]{1,80}$') { throw "RunId 只能包含字母、数字、下划线、点和短横线" }
    $runDir = Join-Path $outputRootFull $RunId
    if (Test-Path -LiteralPath $runDir) { throw "workflow run 目录已存在，为避免覆盖请换 RunId: $runDir" }
    New-Item -ItemType Directory -Path $runDir -Force | Out-Null
    $script:runDir = $runDir
    $script:resultPath = Join-Path $runDir "result.json"
    $workflowName = [string](Get-PropertyValue $workflow "name" ([System.IO.Path]::GetFileNameWithoutExtension($workflowFull)))
    $script:workflowContext = [pscustomobject][ordered]@{
        runId = $RunId
        runDir = $runDir
        root = $root
        outputRoot = $outputRootFull
        workflowName = $workflowName
        workflowPath = $workflowFull
        continueOnFailure = $workflowContinue
        allowDestructive = $allowDestructive
    }
    [System.IO.File]::Copy($workflowFull, (Join-Path $runDir "workflow.input.json"), $true)
    [void](Add-Artifact (Join-Path $runDir "workflow.input.json") "workflow_input")

    $knownActions = @("inspect_project", "get_project_context", "inspect_scene", "query_assets", "create_script", "build", "compile_games", "validate_scene", "apply_scene_commands", "run_gameplay_test", "run_render_test", "capture_frame", "capture_performance", "read_dump", "assert_state", "stop_engine")
    $stepIds = @{}
    for ($index = 0; $index -lt $stepSpecs.Count; $index++) {
        $spec = $stepSpecs[$index]
        if ($null -eq $spec) { throw "step[$index] 不能为空" }
        $id = [string](Get-PropertyValue $spec "id" "")
        $action = [string](Get-PropertyValue $spec "action" "")
        if ($id -notmatch '^[A-Za-z][A-Za-z0-9_.-]{0,63}$') { throw "step[$index].id 不合法: $id" }
        if ($stepIds.ContainsKey($id)) { throw "workflow step id 重复: $id" }
        if ($knownActions -notcontains $action) { throw "step[$index] 使用了不支持的 action: $action" }
        $stepIds[$id] = $index
        $depends = @((Get-PropertyValue $spec "dependsOn" @()))
        foreach ($dependency in $depends) {
            $dependencyId = [string]$dependency
            if (-not $stepIds.ContainsKey($dependencyId)) { throw "step[$index] dependsOn 未知或尚未声明步骤: $dependencyId" }
            if ([int]$stepIds[$dependencyId] -ge $index) { throw "step[$index] 的依赖必须出现在它之前: $dependencyId" }
        }
        $stepTimeout = [int](Get-PropertyValue $spec "timeoutSeconds" $defaultTimeout)
        if ($stepTimeout -lt 1 -or $stepTimeout -gt 7200) { throw "step[$index].timeoutSeconds 必须在 1..7200" }
        if ($action -eq "create_script" -and [bool](Get-PropertyValue (Get-PropertyValue $spec "args" $null) "overwrite" $false) -and -not $allowDestructive) {
            throw "step[$index] create_script overwrite=true 需要 workflow.allowDestructive=true"
        }
        if ($action -eq "apply_scene_commands" -and [bool](Get-PropertyValue (Get-PropertyValue $spec "args" $null) "inPlace" $false) -and -not $allowDestructive) {
            throw "step[$index] apply_scene_commands inPlace=true 需要 workflow.allowDestructive=true"
        }
    }

    if ($DryRun) {
        for ($index = 0; $index -lt $stepSpecs.Count; $index++) {
            $spec = $stepSpecs[$index]
            $id = [string](Get-PropertyValue $spec "id" "")
            $action = [string](Get-PropertyValue $spec "action" "")
            $record = [ordered]@{
                index = $index
                id = $id
                action = $action
                status = "planned"
                timeoutSeconds = [int](Get-PropertyValue $spec "timeoutSeconds" $defaultTimeout)
                dependsOn = @((Get-PropertyValue $spec "dependsOn" @()))
                inputPath = Get-RelativePath (Join-Path $runDir ("step-{0:D2}-{1}\input.json" -f $index, $id))
                result = $null
                artifacts = @()
                failureCategory = ""
                nextAction = "执行该步骤"
            }
            $stepDir = Join-Path $runDir ("step-{0:D2}-{1}" -f $index, $id)
            New-Item -ItemType Directory -Path $stepDir -Force | Out-Null
            Write-JsonFile (Join-Path $stepDir "input.json") (Get-PropertyValue $spec "args" ([ordered]@{}))
            [void]$script:stepRecords.Add([pscustomobject]$record)
            $script:stepRecordMap[$id] = [pscustomobject]$record
            Save-WorkflowResult $true
        }
        $manifestPath = Write-RunManifest
        [void](Add-Artifact $manifestPath "workflow_manifest")
        $finalSuccess = $true
        $finalNextAction = "移除 dry-run 后执行该 workflow"
    } else {
        for ($index = 0; $index -lt $stepSpecs.Count; $index++) {
            $spec = $stepSpecs[$index]
            $id = [string](Get-PropertyValue $spec "id" "")
            $action = [string](Get-PropertyValue $spec "action" "")
            if ($abortRemaining) {
                Add-SkippedStep $index $spec "前置步骤失败且未启用 continueOnFailure"
                continue
            }
            $dependenciesPassed = $true
            foreach ($dependency in @((Get-PropertyValue $spec "dependsOn" @()))) {
                $dependencyRecord = $script:stepRecordMap[[string]$dependency]
                if ($null -eq $dependencyRecord -or $dependencyRecord.status -ne "passed") {
                    $dependenciesPassed = $false
                    break
                }
            }
            if (-not $dependenciesPassed) {
                Add-SkippedStep $index $spec "依赖步骤未通过"
                continue
            }
            $stepDir = Join-Path $runDir ("step-{0:D2}-{1}" -f $index, $id)
            New-Item -ItemType Directory -Path $stepDir -Force | Out-Null
            $workflowValues = [pscustomobject][ordered]@{
                runId = $script:workflowContext.runId
                runDir = $script:workflowContext.runDir
                root = $script:workflowContext.root
                outputRoot = $script:workflowContext.outputRoot
            }
            $rawArgs = Get-PropertyValue $spec "args" ([ordered]@{})
            $resolvedArgs = Resolve-TemplateValue $rawArgs $workflowValues
            $inputPath = Join-Path $stepDir "input.json"
            Write-JsonFile $inputPath $resolvedArgs
            $started = Get-Date
            $actionResult = $null
            $errorText = ""
            try {
                $stepTimeout = [int](Get-PropertyValue $spec "timeoutSeconds" $defaultTimeout)
                $actionResult = Invoke-WorkflowAction $action $resolvedArgs $stepDir $stepTimeout $allowDestructive
            } catch {
                $errorText = $_.Exception.Message
                $actionResult = [ordered]@{
                    success = $false
                    action = $action
                    error = $errorText
                    diagnostics = @($errorText)
                    artifacts = @()
                }
            }
            $success = [bool](Get-PropertyValue $actionResult "success" $false)
            $status = if ($success) { "passed" } else { "failed" }
            $actionError = [string](Get-PropertyValue $actionResult "error" $errorText)
            $record = [ordered]@{
                index = $index
                id = $id
                action = $action
                status = $status
                startedAt = $started.ToString("o")
                endedAt = (Get-Date).ToString("o")
                durationMs = [int]((New-TimeSpan -Start $started -End (Get-Date)).TotalMilliseconds)
                timeoutSeconds = [int](Get-PropertyValue $spec "timeoutSeconds" $defaultTimeout)
                dependsOn = @((Get-PropertyValue $spec "dependsOn" @()))
                inputPath = Get-RelativePath $inputPath
                result = $actionResult
                artifacts = @((Get-PropertyValue $actionResult "artifacts" @()))
                error = $actionError
                failureCategory = if ($success) { "" } else { Get-ActionFailureCategory $action $actionError }
                nextAction = Get-NextAction $action $success $actionError
            }
            [void]$script:stepRecords.Add([pscustomobject]$record)
            $script:stepRecordMap[$id] = [pscustomobject]$record
            foreach ($artifact in @((Get-PropertyValue $actionResult "artifacts" @()))) {
                if ($null -ne $artifact -and [string](Get-PropertyValue $artifact "path" "")) {
                    $existing = @($script:artifacts | Where-Object { [string]$_.path -eq [string]$artifact.path })
                    if ($existing.Count -eq 0) { [void]$script:artifacts.Add($artifact) }
                }
            }
            Save-WorkflowResult ($status -ne "failed")
            if (-not $success) {
                $stepContinue = if (Test-Property $spec "continueOnError") { [bool](Get-PropertyValue $spec "continueOnError" $false) } else { $workflowContinue }
                if (-not $stepContinue) {
                    $abortRemaining = $true
                    $finalNextAction = Get-NextAction $action $false $actionError
                }
            }
        }
        $failedCount = @($script:stepRecords | Where-Object { $_.status -eq "failed" }).Count
        $finalSuccess = ($failedCount -eq 0)
        if (-not $finalSuccess -and -not $finalNextAction) { $finalNextAction = "读取失败步骤的日志并修复后重试" }
        $manifestPath = Write-RunManifest
        [void](Add-Artifact $manifestPath "workflow_manifest")
    }
    Save-WorkflowResult $finalSuccess "" $finalNextAction
    Write-Output "WORKFLOW_RESULT_PATH=$script:resultPath"
    Write-Output "SUCCESS=$finalSuccess"
    if ($finalSuccess) { exit 0 } else { exit 1 }
} catch {
    $fatalError = $_.Exception.Message
    if ($null -ne $script:runDir) {
        $script:workflowContext = if ($null -ne $script:workflowContext) { $script:workflowContext } else {
            [pscustomobject][ordered]@{ runId = $RunId; runDir = $script:runDir; root = $root; outputRoot = $OutputRoot; workflowName = "invalid"; workflowPath = $WorkflowPath; continueOnFailure = $false; allowDestructive = $false }
        }
        Save-WorkflowResult $false $fatalError "修复 workflow 输入或环境错误后重试"
        Write-Output "WORKFLOW_RESULT_PATH=$script:resultPath"
    }
    Write-Output "ERROR=$fatalError"
    Write-Output "SUCCESS=False"
    exit 3
}
