# agent_test.ps1 - MikanEngine AI Native 只测试编排器
# ------------------------------------------------------------------
# TestSpec 是外部 Agent 调用引擎测试能力的窄接口：只允许 Discovery、场景校验、
# 构建、玩法/渲染测试、状态断言、RenderDoc/Nsight 证据和 Evaluation，不接受
# scene.commands、脚本 source、任意 PowerShell 或覆盖开关。
#
# 默认 preview；显式 -Mode execute 才启动引擎/构建/采集。每次运行写入
# out\agent_tests\<run-id>\，并通过 agent_plan 继续生成 evidence/evaluation。
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$TestSpecPath,
    [string]$OutputRoot = "out\agent_tests",
    [string]$RunId = "",
    [string]$Mode = ""
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$script:runDir = $null
$script:resultPath = $null
$script:startedAt = Get-Date
$script:executionMode = "preview"
$script:testContext = $null
$script:steps = New-Object 'System.Collections.Generic.List[object]'
$script:artifacts = New-Object 'System.Collections.Generic.List[object]'

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

function Write-JsonFile([string]$Path, $Value) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [System.IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 80), $utf8NoBom)
}

function Read-JsonFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "JSON 文件不存在: $Path" }
    try { return [System.IO.File]::ReadAllText($Path, $utf8NoBom) | ConvertFrom-Json }
    catch { throw "JSON 解析失败: $Path；$($_.Exception.Message)" }
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

function Resolve-ProjectDirectory([string]$Path, [bool]$MustExist = $false) {
    $full = Resolve-ProjectPath $Path $false
    if ($MustExist -and -not (Test-Path -LiteralPath $full -PathType Container)) { throw "目录不存在: $full" }
    return $full
}

function Resolve-TestScenePath([string]$SceneValue, [string]$ProjectFull = "") {
    if ($ProjectFull -and -not [System.IO.Path]::IsPathRooted($SceneValue)) {
        $clean = $SceneValue.Replace('\', '/').TrimStart('/')
        $manifestPath = Join-Path $ProjectFull "project.json"
        $resourceRoot = $ProjectFull
        if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
            try {
                $manifest = [System.IO.File]::ReadAllText($manifestPath, $utf8NoBom) | ConvertFrom-Json
                $resourceValue = [string](Get-PropertyValue $manifest "resourceRoot" ".")
                if ($resourceValue -and -not [System.IO.Path]::IsPathRooted($resourceValue)) {
                    $resourceRoot = [System.IO.Path]::GetFullPath((Join-Path $ProjectFull $resourceValue))
                }
            } catch { throw "无法解析项目 resourceRoot: $manifestPath；$($_.Exception.Message)" }
        }
        $base = if ($clean.StartsWith('assets/', [System.StringComparison]::OrdinalIgnoreCase)) { $ProjectFull } else { $resourceRoot }
        $candidate = [System.IO.Path]::GetFullPath((Join-Path $base $clean))
        $prefix = $ProjectFull.TrimEnd('\', '/') + '\'
        if (($candidate.Equals($ProjectFull, [System.StringComparison]::OrdinalIgnoreCase) -or $candidate.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) { return $candidate }
    }
    return Resolve-ProjectPath $SceneValue $true
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
        $full = Resolve-ProjectPath $Path $true
        $relative = Get-RelativePath $full
        foreach ($existing in $script:artifacts.ToArray()) { if ([string]$existing.path -eq $relative) { return $existing } }
        $item = Get-Item -LiteralPath $full
        $entry = [ordered]@{ path = $relative; kind = $Kind; size = [int64]$item.Length; sha256 = Get-Sha256 $full }
        [void]$script:artifacts.Add([pscustomobject]$entry)
        return [pscustomobject]$entry
    } catch { return $null }
}

function Assert-Name([string]$Value, [string]$Context, [int]$MaxLength = 160) {
    if ([string]::IsNullOrWhiteSpace($Value) -or $Value.Length -gt $MaxLength -or $Value -notmatch '^[A-Za-z][A-Za-z0-9_.-]*$') { throw "$Context 不合法: $Value" }
}

function Assert-KnownProperties($Object, [string[]]$Allowed, [string]$Context) {
    if ($null -eq $Object) { return }
    $unknown = @($Object.PSObject.Properties.Name | Where-Object { $_ -notin $Allowed })
    if ($unknown.Count -gt 0) { throw "$Context 含不支持字段: $($unknown -join ', ')" }
}

function Assert-RuntimeTestShape($Object, [string]$Context) {
    if ($null -eq $Object) { return }
    Assert-KnownProperties $Object @("enabled", "frames", "fixedDeltaSeconds", "timeoutMs", "replayPath", "screenshotFrame", "extraArgs") $Context
    if ($Context -eq "tests.gameplay" -and (Test-Property $Object "screenshotFrame")) {
        throw "tests.gameplay 不支持 screenshotFrame；截图只能由 tests.render 生成"
    }
}

function Assert-EnabledField($Object, [string]$Context) {
    if ($null -eq $Object) { return }
    if (-not (Test-Property $Object "enabled")) { throw "$Context 必须显式提供 enabled" }
}

function Assert-InputVector($Value, [string]$Context) {
    if ($null -eq $Value -or $Value -is [string]) { throw "$Context.move 必须是两个数字" }
    $components = @($Value)
    if ($components.Count -ne 2) { throw "$Context.move 必须包含两个数字" }
    foreach ($component in $components) {
        $number = [double]0
        if (-not [double]::TryParse([string]$component, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$number) -or
            [double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -lt -1.0 -or $number -gt 1.0) {
            throw "$Context.move 的值必须位于 [-1, 1]"
        }
    }
}

function Assert-ReplayInput($Object, [string]$Context, [bool]$RequireInput = $false) {
    if ($null -eq $Object -or $Object -is [string]) { throw "$Context 必须是对象" }
    Assert-KnownProperties $Object @("move", "jump") $Context
    $hasMove = Test-Property $Object "move"
    $hasJump = Test-Property $Object "jump"
    if ($RequireInput -and -not $hasMove -and -not $hasJump) { throw "$Context 必须提供 move 或 jump" }
    if ($hasMove) { Assert-InputVector (Get-PropertyValue $Object "move" $null) $Context }
    if ($hasJump -and (Get-PropertyValue $Object "jump" $null) -isnot [bool]) { throw "$Context.jump 必须是布尔值" }
}

function Read-InputReplay([string]$Path) {
    $replay = Read-JsonFile $Path
    Assert-KnownProperties $replay @("schemaVersion", "name", "frames", "defaultInput", "events") "input replay"
    if ([int](Get-PropertyValue $replay "schemaVersion" 0) -ne 1) { throw "input replay schemaVersion 必须为 1" }
    $replayName = [string](Get-PropertyValue $replay "name" "")
    Assert-Name $replayName "input replay.name"
    $replayFrames = Assert-IntegerRange (Get-PropertyValue $replay "frames" 0) "input replay.frames" 1 1000000
    $defaultInput = Get-PropertyValue $replay "defaultInput" $null
    if ($null -ne $defaultInput) { Assert-ReplayInput $defaultInput "input replay.defaultInput" $false }
    if (-not (Test-Property $replay "events")) { throw "input replay.events 必须存在" }
    $eventsValue = Get-PropertyValue $replay "events" $null
    if ($null -eq $eventsValue -or $eventsValue -is [string]) { throw "input replay.events 必须是数组" }
    $events = @($eventsValue)
    if ($events.Count -gt 4096) { throw "input replay.events 不能超过 4096 项" }
    $previousEnd = 0
    for ($index = 0; $index -lt $events.Count; $index++) {
        $event = $events[$index]
        $context = "input replay.events[$index]"
        Assert-KnownProperties $event @("startFrame", "endFrame", "move", "jump") $context
        $startFrame = Assert-IntegerRange (Get-PropertyValue $event "startFrame" -1) "$context.startFrame" 0 1000000
        $endFrame = Assert-IntegerRange (Get-PropertyValue $event "endFrame" 0) "$context.endFrame" 1 1000000
        if ($endFrame -le $startFrame -or $endFrame -gt $replayFrames) { throw "$context 的帧区间无效" }
        if ($startFrame -lt $previousEnd) { throw "$context 与前一个事件重叠或未按帧排序" }
        $eventInput = [ordered]@{}
        if (Test-Property $event "move") { $eventInput.move = Get-PropertyValue $event "move" $null }
        if (Test-Property $event "jump") { $eventInput.jump = Get-PropertyValue $event "jump" $null }
        Assert-ReplayInput ([pscustomobject]$eventInput) $context $true
        $previousEnd = $endFrame
    }
    return [pscustomobject][ordered]@{ path = $Path; name = $replayName; frames = $replayFrames; eventCount = $events.Count }
}

function Assert-CaptureShape($Object) {
    if ($null -eq $Object) { return }
    Assert-KnownProperties $Object @("enabled", "captureFrame", "frames", "fixedDeltaSeconds", "captureWaitSeconds", "captureTimeoutSeconds", "renderDocCmdPath", "apiValidation", "captureCallstacks", "skipThumbnail", "extraArgs") "tests.capture"
}

function Assert-PerformanceShape($Object) {
    if ($null -eq $Object) { return }
    Assert-KnownProperties $Object @("enabled", "captureType", "captureFrame", "frames", "fixedDeltaSeconds", "frameCount", "maxDurationMilliseconds", "captureTimeoutSeconds", "traceTimeoutSeconds", "replayLoops", "skipReplay", "nsightPath", "setGpuClocks", "extraArgs", "thresholds") "tests.performance"
    $thresholds = Get-PropertyValue $Object "thresholds" $null
    if ($null -ne $thresholds) { Assert-KnownProperties $thresholds @("maxGpuFrameTimeMs", "maxDrawCount", "maxDispatchCount", "maxGraphicsEngineActivePct", "maxSmThroughputPct", "maxL1texThroughputPct", "maxDramThroughputPct", "maxPcieThroughputPct") "tests.performance.thresholds" }
}

function Assert-IntegerRange($Value, [string]$Context, [int]$Min, [int]$Max) {
    $number = 0
    if (-not [int]::TryParse([string]$Value, [ref]$number) -or $number -lt $Min -or $number -gt $Max) { throw "$Context 必须在 $Min..$Max" }
    return $number
}

function Get-OptionalInt($Object, [string]$Name, [int]$Default, [int]$Min, [int]$Max, [string]$Context) {
    if (-not (Test-Property $Object $Name)) { return $Default }
    return Assert-IntegerRange (Get-PropertyValue $Object $Name $Default) "$Context.$Name" $Min $Max
}

function Get-OptionalDouble($Object, [string]$Name, [double]$Default, [double]$Max, [string]$Context) {
    if (-not (Test-Property $Object $Name)) { return $Default }
    $number = [double]0
    if (-not [double]::TryParse([string](Get-PropertyValue $Object $Name $Default), [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$number) -or
        [double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -le 0.0 -or $number -gt $Max) { throw "$Context.$Name 必须位于 (0, $Max]" }
    return $number
}

function Get-Boolean($Object, [string]$Name, [bool]$Default) {
    if (-not (Test-Property $Object $Name)) { return $Default }
    return [bool](Get-PropertyValue $Object $Name $Default)
}

function Assert-AssetType([string]$AssetType, [string]$Context) {
    if ($AssetType -notin @("all", "scenes", "scripts", "shaders", "models", "textures", "audio")) { throw "$Context.assetType 不支持: $AssetType" }
}

function Test-PathUnder([string]$Path, [string]$RelativeDirectory) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $directory = [System.IO.Path]::GetFullPath((Join-Path $root $RelativeDirectory)).TrimEnd('\', '/') + '\'
    return $full.StartsWith($directory, [System.StringComparison]::OrdinalIgnoreCase)
}

function Add-Step([string]$Id, [string]$Action, [string[]]$DependsOn = @(), $StepArgs = $null, [int]$TimeoutSeconds = 600) {
    foreach ($existing in $script:steps.ToArray()) { if ([string]$existing.id -eq $Id) { throw "TestSpec 生成了重复 workflow step: $Id" } }
    $step = [ordered]@{ id = $Id; action = $Action; timeoutSeconds = $TimeoutSeconds }
    $cleanDependencies = @($DependsOn | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) })
    if ($cleanDependencies.Count -gt 0) { $step.dependsOn = $cleanDependencies }
    if ($null -ne $StepArgs) { $step.args = $StepArgs }
    [void]$script:steps.Add([pscustomobject]$step)
    return $step
}

function Convert-CommonTestArgs($Test, [string]$Context, [int]$DefaultFrames = 120, [int]$DefaultTimeout = 60000) {
    $args = [ordered]@{
        frames = Get-OptionalInt $Test "frames" $DefaultFrames 1 1000000 $Context
        fixedDeltaSeconds = Get-OptionalDouble $Test "fixedDeltaSeconds" (1.0 / 60.0) 0.1 $Context
        timeoutMs = Get-OptionalInt $Test "timeoutMs" $DefaultTimeout 1000 3600000 $Context
    }
    if ($Context -eq "tests.render") {
        $args.screenshotFrame = Get-OptionalInt $Test "screenshotFrame" 0 0 1000000 $Context
        if ($args.screenshotFrame -gt $args.frames) { throw "$Context.screenshotFrame 必须不大于 frames" }
    }
    if (Test-Property $Test "extraArgs") { $args.extraArgs = @((Get-PropertyValue $Test "extraArgs" @()) | ForEach-Object { [string]$_ }) }
    return $args
}

function Convert-CaptureArgs($Capture) {
    $captureFrame = Get-OptionalInt $Capture "captureFrame" 60 1 1000000 "tests.capture"
    $frames = Get-OptionalInt $Capture "frames" ([Math]::Max(120, $captureFrame + 30)) $captureFrame 1000000 "tests.capture"
    $args = [ordered]@{
        captureFrame = $captureFrame
        frames = $frames
        fixedDeltaSeconds = Get-OptionalDouble $Capture "fixedDeltaSeconds" (1.0 / 60.0) 0.1 "tests.capture"
        captureWaitSeconds = Get-OptionalInt $Capture "captureWaitSeconds" 10 0 120 "tests.capture"
        captureTimeoutSeconds = Get-OptionalInt $Capture "captureTimeoutSeconds" 180 1 3600 "tests.capture"
    }
    foreach ($field in @("renderDocCmdPath", "extraArgs")) { if (Test-Property $Capture $field) { $args[$field] = Get-PropertyValue $Capture $field $null } }
    foreach ($field in @("apiValidation", "captureCallstacks", "skipThumbnail")) { if (Test-Property $Capture $field) { $args[$field] = [bool](Get-PropertyValue $Capture $field $false) } }
    return $args
}

function Convert-PerformanceArgs($Performance) {
    $captureFrame = Get-OptionalInt $Performance "captureFrame" 60 1 1000000 "tests.performance"
    $frames = Get-OptionalInt $Performance "frames" ([Math]::Max(180, $captureFrame + 30)) ($captureFrame + 1) 1000000 "tests.performance"
    $args = [ordered]@{
        captureType = [string](Get-PropertyValue $Performance "captureType" "gpu_trace")
        captureFrame = $captureFrame
        frames = $frames
        fixedDeltaSeconds = Get-OptionalDouble $Performance "fixedDeltaSeconds" (1.0 / 60.0) 0.1 "tests.performance"
        frameCount = Get-OptionalInt $Performance "frameCount" 1 1 60 "tests.performance"
        maxDurationMilliseconds = Get-OptionalInt $Performance "maxDurationMilliseconds" 5000 1000 600000 "tests.performance"
        captureTimeoutSeconds = Get-OptionalInt $Performance "captureTimeoutSeconds" 180 1 3600 "tests.performance"
        traceTimeoutSeconds = Get-OptionalInt $Performance "traceTimeoutSeconds" 240 1 3600 "tests.performance"
        replayLoops = Get-OptionalInt $Performance "replayLoops" 3 0 100 "tests.performance"
        skipReplay = Get-Boolean $Performance "skipReplay" $false
        setGpuClocks = [string](Get-PropertyValue $Performance "setGpuClocks" "unaltered")
    }
    if ($args.captureType -notin @("gpu_trace", "graphics_capture")) { throw "tests.performance.captureType 不支持: $($args.captureType)" }
    if ($args.setGpuClocks -notin @("unaltered", "base", "maximum")) { throw "tests.performance.setGpuClocks 不支持: $($args.setGpuClocks)" }
    if ($args.captureType -eq "graphics_capture" -and -not $args.skipReplay -and $args.replayLoops -eq 0) { throw "graphics_capture 在不使用 skipReplay 时 replayLoops 必须大于 0" }
    foreach ($field in @("nsightPath", "extraArgs")) { if (Test-Property $Performance $field) { $args[$field] = Get-PropertyValue $Performance $field $null } }
    return $args
}

function New-EvaluationContract($RequiredActions, [int]$RequiredAssetQueries, [bool]$CaptureEnabled, [bool]$ScreenshotEnabled, [bool]$PerformanceEnabled, $Performance) {
    $contract = [ordered]@{
        schemaVersion = 1
        requireTargetSuccess = $true
        requiredActions = @($RequiredActions)
        discovery = [ordered]@{ required = $true; minProjectQueries = 1; minSceneQueries = 1; minAssetQueries = $RequiredAssetQueries }
    }
    if ($CaptureEnabled -or $ScreenshotEnabled) { $contract.visual = [ordered]@{ required = $true; minScreenshots = 1 } }
    if ($PerformanceEnabled) {
        $contract.performance = [ordered]@{ required = $true }
        $thresholds = Get-PropertyValue $Performance "thresholds" $null
        if ($null -ne $thresholds) {
            foreach ($property in $thresholds.PSObject.Properties) { $contract.performance[$property.Name] = [double]$property.Value }
        }
    }
    return $contract
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
    $stdout = ""; $stderr = ""; $exitCode = $null; $timedOut = $false; $errorText = ""
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
            try { $process.Kill() } catch {}
            [void]$process.WaitForExit(5000)
        } else { [void]$process.WaitForExit() }
        if ($stdoutTask.IsCompleted) { try { $stdout = $stdoutTask.Result } catch {} }
        if ($stderrTask.IsCompleted) { try { $stderr = $stderrTask.Result } catch {} }
        if ($process.HasExited) { $exitCode = [int]$process.ExitCode }
    } catch { $errorText = $_.Exception.Message; $stderr = "$stderr`n$FilePath`: $errorText" }
    [System.IO.File]::WriteAllText($stdoutPath, $stdout, $utf8NoBom)
    [System.IO.File]::WriteAllText($stderrPath, $stderr, $utf8NoBom)
    $markerLines = @($stdout -split "\r?\n" | Where-Object { $_.Trim().StartsWith($Marker) } | Select-Object -Last 1)
    $resultPath = if ($markerLines.Count -gt 0) { $markerLines[0].Trim().Substring($Marker.Length).Trim() } else { "" }
    $structured = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) { try { $structured = Read-JsonFile $resultPath } catch { $errorText = "结果 JSON 解析失败: $($_.Exception.Message)" } }
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

function Write-RunManifest {
    $manifestPath = Join-Path $script:runDir "manifest.json"
    $entries = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse | Where-Object { $_.Name -notin @("manifest.json", "result.json") })) {
        $entries += [ordered]@{ path = Get-RelativePath $file.FullName; size = [int64]$file.Length; sha256 = Get-Sha256 $file.FullName }
    }
    $manifest = [ordered]@{ schemaVersion = 1; operation = "agent_test"; runId = [string]$script:testContext.runId; createdAt = (Get-Date).ToString("o"); files = $entries }
    Write-JsonFile $manifestPath $manifest
    foreach ($entry in @($manifest.files)) {
        $full = Resolve-ProjectPath ([string]$entry.path) $true
        if ((Get-Sha256 $full) -ne [string]$entry.sha256) { throw "agent_test manifest 校验失败: $($entry.path)" }
    }
    return $manifestPath
}

function Save-TestResult([bool]$Success, [string]$ErrorText = "", [string]$NextAction = "") {
    if ($null -eq $script:resultPath -or $null -eq $script:testContext) { return }
    $planProcess = $script:testContext.planProcess
    $planResult = if ($null -ne $planProcess) { Get-PropertyValue $planProcess "result" $null } else { $null }
    $result = [ordered]@{
        schemaVersion = 1
        tool = "agent_test"
        apiVersion = 1
        success = $Success
        mode = $script:executionMode
        preview = ($script:executionMode -eq "preview")
        name = [string]$script:testContext.name
        goal = [string]$script:testContext.goal
        runId = [string]$script:testContext.runId
        projectPath = [string]$script:testContext.projectPath
        scenePath = [string]$script:testContext.scenePath
        runDir = Get-RelativePath $script:runDir
        specPath = Get-RelativePath $script:testContext.specPath
        workflowPath = Get-RelativePath $script:testContext.workflowPath
        taskPath = Get-RelativePath $script:testContext.taskPath
        evaluationContractPath = Get-RelativePath $script:testContext.contractPath
        planPath = Get-RelativePath $script:testContext.planPath
        planResultPath = if ($planProcess -and $planProcess.resultPath) { Get-RelativePath $planProcess.resultPath } else { $null }
        taskResultPath = if ($planResult -and $planResult.taskResultPath) { [string]$planResult.taskResultPath } else { $null }
        evidenceResultPath = if ($planResult -and $planResult.evidenceResultPath) { [string]$planResult.evidenceResultPath } else { $null }
        evaluationResultPath = if ($planResult -and $planResult.evaluationResultPath) { [string]$planResult.evaluationResultPath } else { $null }
        plan = if ($null -ne $planResult) { $planResult } else { $null }
        artifacts = $script:artifacts.ToArray()
        error = $ErrorText
        nextAction = if ($NextAction) { $NextAction } elseif ($Success -and $script:executionMode -eq "preview") { "审查 workflow 和 evaluation contract 后，以 mode=execute 重跑；失败时读取 evidenceResultPath" } elseif ($Success) { "读取 evidence/evaluation 结果并保留本次测试产物" } else { "读取 plan/evidence 的 failures、diagnostics 和 nextAction 后提交新的 TestSpec" }
    }
    Write-JsonFile $script:resultPath $result
}

$fatalError = ""
$finalSuccess = $false
try {
    $specFull = Resolve-ProjectPath $TestSpecPath $true
    $spec = Read-JsonFile $specFull
    $knownTop = @("schemaVersion", "name", "description", "goal", "mode", "project", "assets", "tests")
    $unknownTop = @($spec.PSObject.Properties.Name | Where-Object { $_ -notin $knownTop })
    if ($unknownTop.Count -gt 0) { throw "TestSpec 含未知顶层字段: $($unknownTop -join ', ')；只允许测试相关字段" }
    if ([int](Get-PropertyValue $spec "schemaVersion" 0) -ne 1) { throw "TestSpec schemaVersion 必须为 1" }
    $name = [string](Get-PropertyValue $spec "name" "")
    Assert-Name $name "name"
    $goal = [string](Get-PropertyValue $spec "goal" "")
    if ([string]::IsNullOrWhiteSpace($goal) -or $goal.Length -gt 4000) { throw "goal 必须存在且不超过 4000 字符" }
    $specMode = [string](Get-PropertyValue $spec "mode" "preview").ToLowerInvariant()
    if ($specMode -notin @("preview", "execute")) { throw "TestSpec.mode 只支持 preview 或 execute" }
    if ($Mode) {
        $script:executionMode = $Mode.ToLowerInvariant()
    } else {
        if ($specMode -eq "execute") { throw "TestSpec.mode=execute 需要调用方显式传入 -Mode execute" }
        $script:executionMode = "preview"
    }
    if ($script:executionMode -notin @("preview", "execute")) { throw "mode 只支持 preview 或 execute" }

    $project = Get-PropertyValue $spec "project" $null
    if ($null -eq $project) { throw "project 必须存在" }
    Assert-KnownProperties $project @("scenePath", "game", "projectPath") "project"
    $sceneValue = [string](Get-PropertyValue $project "scenePath" "")
    $projectPathValue = [string](Get-PropertyValue $project "projectPath" "")
    $projectFull = if ($projectPathValue) { Resolve-ProjectDirectory $projectPathValue $true } else { "" }
    $sceneFull = Resolve-TestScenePath $sceneValue $projectFull
    $scene = Get-RelativePath $sceneFull
    $projectRelative = if ($projectFull) { Get-RelativePath $projectFull } else { "" }
    $game = [string](Get-PropertyValue $project "game" "")
    if ($game -and $game -notmatch '^[A-Za-z0-9_.-]{1,64}$') { throw "project.game 不合法: $game" }

    $tests = Get-PropertyValue $spec "tests" $null
    if ($null -eq $tests) { throw "tests 必须存在" }
    Assert-KnownProperties $tests @("compileGames", "gameplay", "render", "assertions", "capture", "performance") "tests"
    $gameplay = Get-PropertyValue $tests "gameplay" $null
    $render = Get-PropertyValue $tests "render" $null
    $capture = Get-PropertyValue $tests "capture" $null
    $performance = Get-PropertyValue $tests "performance" $null
    Assert-RuntimeTestShape $gameplay "tests.gameplay"
    Assert-RuntimeTestShape $render "tests.render"
    if ($null -ne $render -and (Test-Property $render "replayPath")) { throw "tests.render 不支持 replayPath；输入回放只能用于 gameplay" }
    Assert-CaptureShape $capture
    Assert-PerformanceShape $performance
    Assert-EnabledField $gameplay "tests.gameplay"
    Assert-EnabledField $render "tests.render"
    Assert-EnabledField $capture "tests.capture"
    Assert-EnabledField $performance "tests.performance"
    $gameplayEnabled = Get-Boolean $gameplay "enabled" $false
    $renderEnabled = Get-Boolean $render "enabled" $false
    $renderScreenshotFrame = if ($renderEnabled) { Get-OptionalInt $render "screenshotFrame" 0 0 1000000 "tests.render" } else { 0 }
    if ($renderScreenshotFrame -gt 0 -and $renderScreenshotFrame -gt (Get-OptionalInt $render "frames" 60 1 1000000 "tests.render")) {
        throw "tests.render.screenshotFrame 必须不大于 frames"
    }
    $captureEnabled = Get-Boolean $capture "enabled" $false
    $performanceEnabled = Get-Boolean $performance "enabled" $false
    $assertions = @((Get-PropertyValue $tests "assertions" @()))
    if ($assertions.Count -gt 0 -and -not $gameplayEnabled) { throw "tests.assertions 需要启用 tests.gameplay" }
    foreach ($assertion in $assertions) {
        if ($null -eq $assertion -or $assertion -is [string]) { throw "tests.assertions 每项必须是对象" }
        Assert-KnownProperties $assertion @("entityId", "entityName", "field", "expected", "tolerance") "tests.assertions"
        if (-not (Test-Property $assertion "entityId") -and -not (Test-Property $assertion "entityName")) { throw "tests.assertions 每项必须提供 entityId 或 entityName" }
        if ([string](Get-PropertyValue $assertion "field" "") -notin @("pos", "wpos", "rot_deg", "scale", "visible")) { throw "tests.assertions.field 不支持" }
        if (-not (Test-Property $assertion "expected")) { throw "tests.assertions 每项必须提供 expected" }
        if (Test-Property $assertion "tolerance") {
            $tolerance = [double]0
            if (-not [double]::TryParse([string](Get-PropertyValue $assertion "tolerance" 0), [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$tolerance) -or [double]::IsNaN($tolerance) -or [double]::IsInfinity($tolerance) -or $tolerance -lt 0.0 -or $tolerance -gt 1000000.0) { throw "tests.assertions.tolerance 必须在 [0, 1000000]" }
        }
    }

    $assets = Get-PropertyValue $spec "assets" $null
    Assert-KnownProperties $assets @("queries") "assets"
    $queries = @((Get-PropertyValue $assets "queries" @()))
    $requiredAssetQueries = 0
    foreach ($querySpec in $queries) {
        if ($null -eq $querySpec -or $querySpec -is [string]) { throw "assets.queries 每项必须是对象" }
        Assert-KnownProperties $querySpec @("query", "assetType", "required", "maxResults") "assets.queries"
        $query = [string](Get-PropertyValue $querySpec "query" "")
        if ($query.Length -gt 256) { throw "assets.queries.query 不能超过 256 字符" }
        $assetType = [string](Get-PropertyValue $querySpec "assetType" "all")
        Assert-AssetType $assetType "assets.queries"
        if (Get-Boolean $querySpec "required" $false) { $requiredAssetQueries++ }
        [void](Get-OptionalInt $querySpec "maxResults" 200 1 2000 "assets.queries")
    }

    if (-not $RunId) { $RunId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8)) }
    if ($RunId -notmatch '^[A-Za-z0-9_.-]{1,32}$') { throw "RunId 只能包含字母、数字、下划线、点和短横线，长度不超过 32（避免 Windows 产物路径过深）" }
    $outputRootFull = Resolve-ProjectPath $OutputRoot $false
    $outputRootRelative = Get-RelativePath $outputRootFull
    if (-not $outputRootRelative -or $outputRootRelative -notmatch '^out(?:/|$)') { throw "OutputRoot 必须位于项目 out/ 目录下" }
    $runDir = Join-Path $outputRootFull $RunId
    if (Test-Path -LiteralPath $runDir) { throw "agent_test run 目录已存在，为避免覆盖请换 RunId: $RunId" }
    New-Item -ItemType Directory -Path $runDir -Force | Out-Null
    $script:runDir = $runDir
    $script:resultPath = Join-Path $runDir "result.json"
    $script:testContext = [pscustomobject][ordered]@{
        runId = $RunId; name = $name; goal = $goal; specPath = $specFull
        projectPath = $projectRelative; scenePath = $scene
        workflowPath = $null; taskPath = $null; contractPath = $null; planPath = $null; planProcess = $null
    }
    [System.IO.File]::Copy($specFull, (Join-Path $runDir "test.input.json"), $true)
    [void](Add-Artifact (Join-Path $runDir "test.input.json") "test_input")

    $contextDependencies = New-Object 'System.Collections.Generic.List[string]'
    if ($projectRelative) {
        Add-Step "get_project_context" "get_project_context" @() ([ordered]@{ projectPath = $projectRelative; maxResults = 200 }) 600 | Out-Null
        [void]$contextDependencies.Add("get_project_context")
    }
    $projectInspectArgs = [ordered]@{ maxResults = 200 }
    if ($projectRelative) { $projectInspectArgs.projectPath = $projectRelative }
    $projectStep = Add-Step "inspect_project" "inspect_project" @($contextDependencies.ToArray()) $projectInspectArgs 600
    $sceneInspectArgs = [ordered]@{ scene = $scene; maxEntities = 500; maxAssetReferences = 200 }
    if ($projectRelative) { $sceneInspectArgs.projectPath = $projectRelative }
    $sceneStep = Add-Step "inspect_scene" "inspect_scene" @("inspect_project") $sceneInspectArgs 600
    $queryIndex = 0
    foreach ($querySpec in $queries) {
        $queryArgs = [ordered]@{
            query = [string](Get-PropertyValue $querySpec "query" "")
            assetType = [string](Get-PropertyValue $querySpec "assetType" "all")
            maxResults = Get-OptionalInt $querySpec "maxResults" 200 1 2000 "assets.queries[$queryIndex]"
        }
        if ($projectRelative) { $queryArgs.projectPath = $projectRelative }
        $required = Get-Boolean $querySpec "required" $false
        Add-Step ("query_assets_{0:D2}" -f $queryIndex) "query_assets" @("inspect_project") $queryArgs 600 | Out-Null
        if (-not $required) { $script:steps[$script:steps.Count - 1] | Add-Member -MemberType NoteProperty -Name continueOnError -Value $true }
        $queryIndex++
    }

    $validateArgs = [ordered]@{ scenePath = $scene; checkAssets = $true }
    if ($projectRelative) { $validateArgs.projectPath = $projectRelative }
    Add-Step "validate_scene" "validate_scene" @("inspect_scene") $validateArgs 600 | Out-Null
    $last = "validate_scene"
    if (Get-Boolean $tests "compileGames" $false) {
        Add-Step "compile_games" "compile_games" @($last) $null 1800 | Out-Null
        $last = "compile_games"
    }

    $requiredActions = New-Object 'System.Collections.Generic.List[object]'
    if ($projectRelative) { [void]$requiredActions.Add([ordered]@{ action = "get_project_context"; status = "passed"; minCount = 1 }) }
    [void]$requiredActions.Add([ordered]@{ action = "inspect_project"; status = "passed"; minCount = 1 })
    [void]$requiredActions.Add([ordered]@{ action = "inspect_scene"; status = "passed"; minCount = 1 })
    [void]$requiredActions.Add([ordered]@{ action = "validate_scene"; status = "passed"; minCount = 1 })
    if (Get-Boolean $tests "compileGames" $false) { [void]$requiredActions.Add([ordered]@{ action = "compile_games"; status = "passed"; minCount = 1 }) }
    if ($queries | Where-Object { Get-Boolean $_ "required" $false }) { [void]$requiredActions.Add([ordered]@{ action = "query_assets"; status = "passed"; minCount = $requiredAssetQueries }) }

    if ($gameplayEnabled) {
        $runnerArgs = [ordered]@{ target = "MikanTestRunner"; configureIfMissing = $true }
        Add-Step "build_runner" "build" @($last) $runnerArgs 1800 | Out-Null
        $gameplayArgs = Convert-CommonTestArgs $gameplay "tests.gameplay" 120 60000
        $gameplayArgs.scene = $scene
        if ($projectRelative) { $gameplayArgs.projectPath = $projectRelative }
        if ($game) { $gameplayArgs.game = $game }
        $replayPathValue = [string](Get-PropertyValue $gameplay "replayPath" "")
        if ($replayPathValue) {
            $replayFull = Resolve-ProjectPath $replayPathValue $true
            $replayInfo = Read-InputReplay $replayFull
            if (-not (Test-Property $gameplay "frames")) { $gameplayArgs.frames = $replayInfo.frames }
            foreach ($extra in @((Get-PropertyValue $gameplay "extraArgs" @()))) {
                if ([string]$extra -match '(?i)^--input-replay(=|$)') {
                    throw "tests.gameplay.extraArgs 不能重复提供 --input-replay；请使用 replayPath"
                }
            }
            $gameplayArgs.extraArgs = @("--input-replay", $replayFull) + @($gameplayArgs.extraArgs)
            [void](Add-Artifact $replayFull "input_replay")
        }
        Add-Step "gameplay" "run_gameplay_test" @("build_runner") $gameplayArgs 1800 | Out-Null
        Add-Step "read_gameplay_state" "read_dump" @("gameplay") ([ordered]@{ path = '${steps.gameplay.result.dumpPath}' }) 600 | Out-Null
        $last = "read_gameplay_state"
        [void]$requiredActions.Add([ordered]@{ action = "build"; status = "passed"; minCount = 1 })
        [void]$requiredActions.Add([ordered]@{ action = "run_gameplay_test"; status = "passed"; minCount = 1 })
        if ($assertions.Count -gt 0) {
            Add-Step "assert_gameplay_state" "assert_state" @("read_gameplay_state") ([ordered]@{ path = '${steps.gameplay.result.dumpPath}'; assertions = $assertions }) 600 | Out-Null
            $last = "assert_gameplay_state"
            [void]$requiredActions.Add([ordered]@{ action = "assert_state"; status = "passed"; minCount = 1 })
        }
    }

    $needsEngine = ($renderEnabled -or $captureEnabled -or $performanceEnabled)
    if ($needsEngine) {
        Add-Step "build_engine" "build" @($last) ([ordered]@{ target = "EngineMain"; configureIfMissing = $true; killEngine = $true }) 2400 | Out-Null
        $last = "build_engine"
        [void]$requiredActions.Add([ordered]@{ action = "build"; status = "passed"; minCount = 1 })
    }
    if ($renderEnabled) {
        $renderArgs = Convert-CommonTestArgs $render "tests.render" 60 120000
        $renderArgs.scene = $scene
        if ($projectRelative) { $renderArgs.projectPath = $projectRelative }
        if ($game) { $renderArgs.game = $game }
        Add-Step "render" "run_render_test" @($last) $renderArgs 1800 | Out-Null
        $last = "render"
        [void]$requiredActions.Add([ordered]@{ action = "run_render_test"; status = "passed"; minCount = 1 })
    }
    if ($captureEnabled) {
        $captureArgs = Convert-CaptureArgs $capture
        $captureArgs.scene = $scene
        if ($projectRelative) { $captureArgs.projectPath = $projectRelative }
        if ($game) { $captureArgs.game = $game }
        Add-Step "capture_frame" "capture_frame" @($last) $captureArgs 2400 | Out-Null
        $last = "capture_frame"
        [void]$requiredActions.Add([ordered]@{ action = "capture_frame"; status = "passed"; minCount = 1 })
    }
    if ($performanceEnabled) {
        $performanceArgs = Convert-PerformanceArgs $performance
        $performanceArgs.scene = $scene
        if ($projectRelative) { $performanceArgs.projectPath = $projectRelative }
        if ($game) { $performanceArgs.game = $game }
        Add-Step "capture_performance" "capture_performance" @($last) $performanceArgs 3600 | Out-Null
        [void]$requiredActions.Add([ordered]@{ action = "capture_performance"; status = "passed"; minCount = 1 })
    }

    $workflow = [ordered]@{
        schemaVersion = 1
        name = "agent-test-$name"
        description = [string](Get-PropertyValue $spec "description" $goal)
        allowDestructive = $false
        defaults = [ordered]@{ timeoutSeconds = 600 }
        steps = $script:steps.ToArray()
    }
    $workflowPath = Join-Path $runDir "workflow.generated.json"
    Write-JsonFile $workflowPath $workflow
    $script:testContext.workflowPath = $workflowPath

    $task = [ordered]@{
        schemaVersion = 1
        name = "agent-test-task-$name"
        description = "只测试任务；失败后由外部 Agent 读取 evidence 并提交新的 TestSpec。"
        workflowPath = Get-RelativePath $workflowPath
        mode = $script:executionMode
        maxAttempts = 1
        timeoutSeconds = 7200
        candidates = @()
    }
    $taskPath = Join-Path $runDir "task.generated.json"
    Write-JsonFile $taskPath $task
    $script:testContext.taskPath = $taskPath

    $contract = New-EvaluationContract $requiredActions.ToArray() $requiredAssetQueries $captureEnabled ($renderEnabled -and $renderScreenshotFrame -gt 0) $performanceEnabled $performance
    $contractPath = Join-Path $runDir "evaluation.generated.json"
    Write-JsonFile $contractPath $contract
    $script:testContext.contractPath = $contractPath

    $plan = [ordered]@{
        schemaVersion = 1
        name = "agent-test-plan-$name"
        goal = $goal
        taskPath = Get-RelativePath $taskPath
        evaluationContractPath = Get-RelativePath $contractPath
        mode = $script:executionMode
        maxAttempts = 1
    }
    $planPath = Join-Path $runDir "plan.generated.json"
    Write-JsonFile $planPath $plan
    $script:testContext.planPath = $planPath
    foreach ($generatedPath in @($workflowPath, $taskPath, $contractPath, $planPath)) { [void](Add-Artifact $generatedPath "generated_test_spec") }

    $powershellPath = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    if (-not (Test-Path -LiteralPath $powershellPath -PathType Leaf)) { $powershellPath = "powershell.exe" }
    $planScript = Join-Path $PSScriptRoot "agent_plan.ps1"
    $planArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $planScript, "-PlanPath", $planPath, "-OutputRoot", (Join-Path $runDir "plan_runs"), "-Mode", $script:executionMode, "-MaxAttempts", "1")
    $script:testContext.planProcess = Invoke-ChildProcess $powershellPath $planArgs $root (Join-Path $runDir "plan_process") "PLAN_RESULT_PATH=" 7200
    [void](Add-Artifact $script:testContext.planProcess.stdoutPath "plan_process_stdout")
    [void](Add-Artifact $script:testContext.planProcess.stderrPath "plan_process_stderr")
    if ($script:testContext.planProcess.resultPath) { [void](Add-Artifact $script:testContext.planProcess.resultPath "plan_result") }
    $finalSuccess = [bool]$script:testContext.planProcess.success
    $manifestPath = Write-RunManifest
    [void](Add-Artifact $manifestPath "test_manifest")
    $testNextAction = "读取 plan/evidence 的 failures、diagnostics 和 nextAction 后提交新的 TestSpec"
    if ($finalSuccess) { $testNextAction = "agent_test 已完成；根据 mode 读取 plan/evidence/evaluation 结果" }
    Save-TestResult $finalSuccess "" $testNextAction
    Write-Output "TEST_RESULT_PATH=$script:resultPath"
    Write-Output "SUCCESS=$finalSuccess"
    if ($finalSuccess) { exit 0 } else { exit 1 }
} catch {
    $fatalError = $_.Exception.Message
    if ($null -ne $script:runDir -and $null -ne $script:testContext) {
        try { Save-TestResult $false $fatalError "修复 TestSpec 输入或执行环境后，使用新的 RunId 重试" } catch {}
        Write-Output "TEST_RESULT_PATH=$script:resultPath"
    }
    Write-Output "ERROR=$fatalError"
    Write-Output "SUCCESS=False"
    exit 3
}
