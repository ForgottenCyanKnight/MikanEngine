# agent_evidence.ps1 - MikanEngine Agent 模型可读证据采集器
# ------------------------------------------------------------------
# 将 agent_workflow / agent_task 的结果、日志、运行层、构建产物、主机环境和
# Android adb 状态收敛为 evidence.json。该工具只读项目结果和外部能力，不执行
# 引擎、不安装 APK、不修改源码；缺失的截图、MRT 或设备能力会明确标为 unavailable。
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceResultPath,

    [string]$OutputRoot = "out\agent_evidence",

    [string]$RunId = ""
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

function Write-TextFile([string]$Path, [string]$Text) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
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

function Add-ArtifactEvidence([string]$Path, [string]$Kind = "artifact") {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    try {
        $full = Resolve-ProjectPath $Path $false
        $exists = Test-Path -LiteralPath $full -PathType Leaf
        $relative = Get-RelativePath $full
        foreach ($existing in $script:artifacts.ToArray()) { if ([string]$existing.path -eq $relative) { return $existing } }
        $size = if ($exists) { [int64](Get-Item -LiteralPath $full).Length } else { 0 }
        $entry = [ordered]@{ path = $relative; kind = $Kind; exists = $exists; size = $size; sha256 = if ($exists) { Get-Sha256 $full } else { "" } }
        [void]$script:artifacts.Add([pscustomobject]$entry)
        return [pscustomobject]$entry
    } catch {
        return [pscustomobject][ordered]@{ path = [string]$Path; kind = $Kind; exists = $false; size = 0; sha256 = ""; error = $_.Exception.Message }
    }
}

function Read-JsonFile([string]$Path) {
    $full = Resolve-ProjectPath $Path $true
    try { return [System.IO.File]::ReadAllText($full, $utf8NoBom) | ConvertFrom-Json }
    catch { throw "JSON 解析失败: $full；$($_.Exception.Message)" }
}

function Read-TextFileSafe([string]$Path, [int]$MaxChars = 200000) {
    try {
        $full = Resolve-ProjectPath $Path $true
        $text = [System.IO.File]::ReadAllText($full, $utf8NoBom)
        if ($text.Length -gt $MaxChars) { return $text.Substring($text.Length - $MaxChars) }
        return $text
    } catch { return "" }
}

function Get-OutputTail([string]$Text, [int]$TailLines = 80) {
    if ([string]::IsNullOrWhiteSpace($Text)) { return "" }
    $lines = @($Text -split "\r?\n")
    if ($lines.Count -le $TailLines) { return ($lines -join "`n").Trim() }
    return ("... 已省略前 {0} 行 ...`n{1}" -f ($lines.Count - $TailLines), (($lines | Select-Object -Last $TailLines) -join "`n")).Trim()
}

function Get-DiagnosticClass([string]$Line) {
    if ($Line -match '(?i)(fatal error C\d+|error C\d+|LNK\d+|undefined reference|ninja: error|FAILED:)') { return "build" }
    if ($Line -match '(?i)(VK_ERROR|device lost|validation error|\[ERR\]|\[FTL\]|\[Crash\]|\bFatal\b|FATAL EXCEPTION|AndroidRuntime)') { return "runtime" }
    if ($Line -match '(?i)(glslang|spir-v|shader.*(error|fail)|compile.*shader)') { return "shader" }
    if ($Line -match '(?i)(\[WARN\]|warning|可能不存在|not found|missing)') { return "warning" }
    return "other"
}

function Get-LogDiagnostics([string]$Text) {
    if ([string]::IsNullOrWhiteSpace($Text)) { return @() }
    $items = New-Object 'System.Collections.Generic.List[object]'
    foreach ($line in @($Text -split "\r?\n")) {
        $trimmed = $line.Trim()
        if (-not $trimmed) { continue }
        if ($trimmed -match '(?i)(error|fatal|failed|crash|device lost|validation|warning|missing|not found|VK_ERROR|FATAL EXCEPTION|AndroidRuntime|LNK\d+|C\d{4})') {
            [void]$items.Add([ordered]@{ class = Get-DiagnosticClass $trimmed; text = $trimmed })
        }
    }
    return @($items | Select-Object -Last 80)
}

function Get-ResultPathValue($Value) {
    if ($null -eq $Value) { return $null }
    $path = [string]$Value
    if ([string]::IsNullOrWhiteSpace($path)) { return $null }
    try { return Resolve-ProjectPath $path $true } catch { return $null }
}

function Get-WorkflowResults($RootResult, [string]$RootPath) {
    $results = New-Object 'System.Collections.Generic.List[object]'
    $tool = [string](Get-PropertyValue $RootResult "tool" "")
    if ($tool -eq "agent_workflow") { [void]$results.Add([pscustomobject][ordered]@{ path = $RootPath; value = $RootResult; source = "workflow" }) }
    elseif ($tool -eq "agent_task") {
        foreach ($attempt in @((Get-PropertyValue $RootResult "attempts" @()))) {
            $candidatePath = Get-ResultPathValue (Get-PropertyValue $attempt "workflowResultPath" "")
            if ($candidatePath) {
                try { [void]$results.Add([pscustomobject][ordered]@{ path = $candidatePath; value = Read-JsonFile $candidatePath; source = [string](Get-PropertyValue $attempt "kind" "attempt") }) } catch {}
            }
        }
    }
    return $results.ToArray()
}

function Collect-ReferencedArtifacts($Object) {
    if ($null -eq $Object) { return }
    foreach ($artifact in @((Get-PropertyValue $Object "artifacts" @()))) {
        $path = [string](Get-PropertyValue $artifact "path" "")
        if ($path) { [void](Add-ArtifactEvidence $path ([string](Get-PropertyValue $artifact "kind" "artifact"))) }
    }
    foreach ($step in @((Get-PropertyValue $Object "steps" @()))) {
        foreach ($artifact in @((Get-PropertyValue $step "artifacts" @()))) {
            $path = [string](Get-PropertyValue $artifact "path" "")
            if ($path) { [void](Add-ArtifactEvidence $path ([string](Get-PropertyValue $artifact "kind" "step_artifact"))) }
        }
        $stepResult = Get-PropertyValue $step "result" $null
        foreach ($artifact in @((Get-PropertyValue $stepResult "artifacts" @()))) {
            $path = [string](Get-PropertyValue $artifact "path" "")
            if ($path) { [void](Add-ArtifactEvidence $path ([string](Get-PropertyValue $artifact "kind" "step_artifact"))) }
        }
    }
}

function Invoke-CapturedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int]$TimeoutSeconds = 10
    )
    $result = [ordered]@{ available = $true; exitCode = $null; timedOut = $false; stdout = ""; stderr = ""; error = "" }
    try {
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $FilePath
        $startInfo.WorkingDirectory = $root
        $startInfo.Arguments = (($ArgumentList | ForEach-Object { '"' + ([string]$_).Replace('"', '\"') + '"' }) -join " ")
        $startInfo.UseShellExecute = $false
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.CreateNoWindow = $true
        $process = [System.Diagnostics.Process]::Start($startInfo)
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $result.timedOut = $true
            try { $process.Kill() } catch {}
            [void]$process.WaitForExit(2000)
        }
        if ($stdoutTask.IsCompleted) { try { $result.stdout = $stdoutTask.Result } catch {} }
        if ($stderrTask.IsCompleted) { try { $result.stderr = $stderrTask.Result } catch {} }
        if ($process.HasExited) { $result.exitCode = [int]$process.ExitCode }
    } catch { $result.error = $_.Exception.Message }
    return [pscustomobject]$result
}

function Get-HostEvidence {
    $gitHead = ""
    try { $gitHead = ((& git -C $root rev-parse HEAD 2>$null) | Out-String).Trim() } catch {}
    $gitStatusCount = 0
    try { $gitStatusCount = @(& git -C $root status --porcelain 2>$null).Count } catch {}
    $files = New-Object 'System.Collections.Generic.List[object]'
    foreach ($relative in @("out\build\x64-Release\MikanEngine.exe", "out\build\x64-Release\MikanTestRunner.exe", "out\build\x64-Release\Game.dll", "out\build\x64-Release\Gamecontact2d.dll")) {
        $full = Join-Path $root $relative
        $exists = Test-Path -LiteralPath $full -PathType Leaf
        [void]$files.Add([ordered]@{ path = $relative.Replace('\', '/'); exists = $exists; size = if ($exists) { [int64](Get-Item -LiteralPath $full).Length } else { 0 }; sha256 = if ($exists) { Get-Sha256 $full } else { "" } })
    }
    return [ordered]@{
        os = [string]$env:OS
        processorArchitecture = [string]$env:PROCESSOR_ARCHITECTURE
        powershell = [string]$PSVersionTable.PSVersion
        gitHead = $gitHead
        gitDirtyEntryCount = $gitStatusCount
        buildArtifacts = $files.ToArray()
    }
}

function Get-AndroidEvidence {
    $adbCommand = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($null -eq $adbCommand) {
        return [ordered]@{ status = "unavailable"; adbPresent = $false; devices = @(); nextAction = "安装 Android SDK platform-tools 或在目标机器配置 adb 后再运行移动端 smoke" }
    }
    $devicesResult = Invoke-CapturedProcess $adbCommand.Source @("devices", "-l") 10
    if ($devicesResult.error -or $devicesResult.timedOut) {
        return [ordered]@{ status = "error"; adbPresent = $true; devices = @(); diagnostics = @($devicesResult.error); nextAction = "检查 adb server 和 USB/网络调试连接" }
    }
    $devices = New-Object 'System.Collections.Generic.List[object]'
    foreach ($line in @($devicesResult.stdout -split "\r?\n")) {
        $trimmed = $line.Trim()
        if (-not $trimmed -or $trimmed -like "List of devices attached*") { continue }
        $parts = @($trimmed -split '\s+')
        if ($parts.Count -lt 2) { continue }
        $serial = [string]$parts[0]
        $state = [string]$parts[1]
        $device = [ordered]@{ serial = $serial; state = $state; raw = $trimmed }
        if ($state -eq "device") {
            foreach ($property in @(@("model", "ro.product.model"), @("abi", "ro.product.cpu.abi"), @("androidVersion", "ro.build.version.release"), @("vulkan", "ro.hardware.vulkan"))) {
                $propertyResult = Invoke-CapturedProcess $adbCommand.Source @("-s", $serial, "shell", "getprop", $property[1]) 5
                $device[$property[0]] = ([string]$propertyResult.stdout).Trim()
            }
        }
        [void]$devices.Add($device)
    }
    $status = if ($devices.Count -eq 0) { "no_device" } elseif (@($devices | Where-Object { $_.state -eq "device" }).Count -gt 0) { "ready" } else { "needs_authorization" }
    return [ordered]@{ status = $status; adbPresent = $true; devices = $devices.ToArray(); nextAction = if ($status -eq "ready") { "可在明确 serial 后接入 Android 安装/运行 smoke" } else { "连接并授权一个 Android Vulkan 设备" } }
}

function Get-WorkflowObservations($WorkflowResults) {
    $observations = New-Object 'System.Collections.Generic.List[object]'
    $failures = New-Object 'System.Collections.Generic.List[object]'
    $diagnostics = New-Object 'System.Collections.Generic.List[object]'
    foreach ($entry in @($WorkflowResults)) {
        $workflow = $entry.value
        $stepSummaries = New-Object 'System.Collections.Generic.List[object]'
        foreach ($step in @((Get-PropertyValue $workflow "steps" @()))) {
            $stepResult = Get-PropertyValue $step "result" $null
            $stepDiagnostics = @((Get-PropertyValue $stepResult "diagnostics" @()))
            foreach ($diagnostic in $stepDiagnostics) { [void]$diagnostics.Add([ordered]@{ source = [string]$step.id; class = "structured"; text = [string]$diagnostic }) }
            $summary = [ordered]@{
                id = [string](Get-PropertyValue $step "id" "")
                action = [string](Get-PropertyValue $step "action" "")
                status = [string](Get-PropertyValue $step "status" "")
                durationMs = Get-PropertyValue $step "durationMs" $null
                failureCategory = [string](Get-PropertyValue $step "failureCategory" "")
                nextAction = [string](Get-PropertyValue $step "nextAction" "")
                runtimeLayer = [string](Get-PropertyValue $stepResult "runtimeLayer" "")
                dumpPath = [string](Get-PropertyValue $stepResult "dumpPath" "")
                frames = Get-PropertyValue $stepResult "frames" $null
                scene = [string](Get-PropertyValue $stepResult "scene" (Get-PropertyValue $stepResult "scenePath" ""))
                captureType = [string](Get-PropertyValue $stepResult "captureType" "")
                performanceStatus = [string](Get-PropertyValue $stepResult "status" "")
                performance = Get-PropertyValue $stepResult "performance" $null
                discoveryMode = [string](Get-PropertyValue $stepResult "mode" "")
                discoveryContext = Get-PropertyValue $stepResult "context" $null
                diagnostics = $stepDiagnostics
            }
            [void]$stepSummaries.Add($summary)
            if ($summary.status -eq "failed") {
                [void]$failures.Add([ordered]@{ source = [string]$entry.path; stepId = $summary.id; action = $summary.action; failureCategory = $summary.failureCategory; nextAction = $summary.nextAction; error = [string](Get-PropertyValue $step "error" ""); diagnostics = $stepDiagnostics })
            }
        }
        [void]$observations.Add([ordered]@{
            source = [string]$entry.path
            sourceKind = [string]$entry.source
            tool = [string](Get-PropertyValue $workflow "tool" "")
            success = [bool](Get-PropertyValue $workflow "success" $false)
            dryRun = [bool](Get-PropertyValue $workflow "dryRun" $false)
            workflowName = [string](Get-PropertyValue $workflow "workflowName" "")
            runId = [string](Get-PropertyValue $workflow "runId" "")
            counts = Get-PropertyValue $workflow "counts" $null
            steps = $stepSummaries.ToArray()
        })
    }
    return [pscustomobject][ordered]@{ observations = $observations.ToArray(); failures = $failures.ToArray(); diagnostics = $diagnostics.ToArray() }
}

function Get-VisualEvidence {
    $screenshots = @($script:artifacts | Where-Object { $_.exists -and $_.path -match '(?i)\.(png|jpg|jpeg|bmp|exr)$' -and $_.path -match '(?i)(capture|screenshot|frame|mrt)' })
    $mrt = @($script:artifacts | Where-Object { $_.exists -and $_.path -match '(?i)(mrt|gbuffer|depth)' })
    $renderdocCaptures = @($script:artifacts | Where-Object { $_.exists -and $_.path -match '(?i)\.rdc$' })
    $engineScreenshots = @($screenshots | Where-Object { [string]$_.kind -eq "engine_screenshot" })
    $screenshotMetadata = @($script:artifacts | Where-Object { $_.exists -and [string]$_.kind -eq "engine_screenshot_metadata" })
    $readiness = @()
    foreach ($metadataArtifact in $screenshotMetadata) {
        try {
            $metadata = Read-JsonFile ([string]$metadataArtifact.path)
            $readiness += [ordered]@{
                metadataPath = [string]$metadataArtifact.path
                image = [string](Get-PropertyValue $metadata "image" "")
                frame = Get-PropertyValue $metadata "frame" $null
                runtimeReady = [bool](Get-PropertyValue $metadata "runtimeReady" $false)
                readinessStatus = [string](Get-PropertyValue $metadata "readinessStatus" "")
                visualStatus = [string](Get-PropertyValue $metadata "visualStatus" "")
                nonBlackRatio = [double](Get-PropertyValue $metadata "nonBlackRatio" 0.0)
            }
        } catch {
            $readiness += [ordered]@{
                metadataPath = [string]$metadataArtifact.path
                image = ""
                frame = $null
                runtimeReady = $false
                readinessStatus = "metadata-parse-failed"
                visualStatus = "metadata-parse-failed"
                nonBlackRatio = 0.0
            }
        }
    }
    $invalidReadiness = @($readiness | Where-Object {
        -not $_.runtimeReady -or $_.visualStatus -in @("startup-black-screen", "runtime-ready-but-black", "metadata-parse-failed")
    })
    $validEngineScreenshotCount = @($readiness | Where-Object { $_.runtimeReady -and $_.visualStatus -eq "ready" }).Count
    $untrackedScreenshotCount = @($screenshots | Where-Object { [string]$_.kind -ne "engine_screenshot" }).Count
    $usableScreenshotCount = $validEngineScreenshotCount + $untrackedScreenshotCount
    $available = ($screenshots.Count -gt 0 -or $mrt.Count -gt 0 -or $renderdocCaptures.Count -gt 0)
    return [ordered]@{
        screenshotCount = $screenshots.Count
        screenshots = @($screenshots)
        engineScreenshotCount = $engineScreenshots.Count
        screenshotMetadataCount = $screenshotMetadata.Count
        screenshotReadiness = @($readiness)
        usableScreenshotCount = $usableScreenshotCount
        invalidScreenshotCount = $invalidReadiness.Count
        blackScreenCount = @($readiness | Where-Object { $_.visualStatus -in @("startup-black-screen", "runtime-ready-but-black") }).Count
        mrtCount = $mrt.Count
        mrtArtifacts = @($mrt)
        renderdocCaptureCount = $renderdocCaptures.Count
        renderdocCaptures = @($renderdocCaptures)
        available = $available
        visualUsable = ($usableScreenshotCount -gt 0 -and $invalidReadiness.Count -eq 0)
        nextAction = if ($invalidReadiness.Count -gt 0) { "存在未就绪或黑屏截图；先读取 screenshotReadiness/readinessStatus，修复启动或渲染路径后重跑" } elseif ($available) { "将截图/MRT/RenderDoc 捕获与目标视觉结果进行人工或视觉模型比对" } else { "当前结果没有截图/MRT/RenderDoc 捕获；在 run_render_test 中设置 screenshotFrame 后再做视觉闭环" }
    }
}

function Get-PerformanceEvidence($WorkflowResults, $RootResult = $null, [string]$RootPath = "") {
    $captures = New-Object 'System.Collections.Generic.List[object]'
    if ([string](Get-PropertyValue $RootResult "tool" "") -eq "nsight_capture") {
        $nsight = Get-PropertyValue $RootResult "nsight" $null
        [void]$captures.Add([ordered]@{
            source = $RootPath
            stepId = ""
            status = [string](Get-PropertyValue $RootResult "status" "")
            success = [bool](Get-PropertyValue $RootResult "success" $false)
            captureType = [string](Get-PropertyValue $RootResult "captureType" "")
            nsightAvailable = [bool](Get-PropertyValue $nsight "available" $false)
            nsightIsAdministrator = [bool](Get-PropertyValue $nsight "isAdministrator" $false)
            resultPath = $RootPath
            performance = Get-PropertyValue $RootResult "performance" $null
            replay = Get-PropertyValue $RootResult "replay" $null
            nextAction = [string](Get-PropertyValue $RootResult "nextAction" "")
        })
    }
    foreach ($entry in @($WorkflowResults)) {
        $workflow = $entry.value
        foreach ($step in @((Get-PropertyValue $workflow "steps" @()))) {
            if ([string](Get-PropertyValue $step "action" "") -ne "capture_performance") { continue }
            $stepResult = Get-PropertyValue $step "result" $null
            [void]$captures.Add([ordered]@{
                source = [string]$entry.path
                stepId = [string](Get-PropertyValue $step "id" "")
                status = [string](Get-PropertyValue $stepResult "status" (Get-PropertyValue $step "status" ""))
                success = [bool](Get-PropertyValue $stepResult "success" $false)
                captureType = [string](Get-PropertyValue $stepResult "captureType" "")
                nsightAvailable = [bool](Get-PropertyValue $stepResult "nsightAvailable" $false)
                nsightIsAdministrator = [bool](Get-PropertyValue $stepResult "nsightIsAdministrator" $false)
                resultPath = [string](Get-PropertyValue $stepResult "resultPath" "")
                performance = Get-PropertyValue $stepResult "performance" $null
                replay = Get-PropertyValue $stepResult "replay" $null
                nextAction = [string](Get-PropertyValue $stepResult "nextAction" (Get-PropertyValue $step "nextAction" ""))
            })
        }
    }
    $gpuTraces = @($script:artifacts | Where-Object { $_.exists -and $_.path -match '(?i)\.ngfx-gputrace$|\.ngfx-gputrace/' })
    $graphicsCaptures = @($script:artifacts | Where-Object { $_.exists -and $_.path -match '(?i)\.ngfx-capture$|\.ngfx-capture/' })
    $metricFiles = @($script:artifacts | Where-Object { $_.exists -and $_.path -match '(?i)(GPUTRACE_FRAME|FRAME)\.xls$|iteration_times\.csv$' })
    $readyCapture = @($captures | Where-Object { $_.status -in @("profiled", "captured") -and $_.success }).Count -gt 0
    $permissionCapture = @($captures | Where-Object { $_.status -eq "permission_denied" }).Count -gt 0
    $status = if ($readyCapture -or $metricFiles.Count -gt 0) { "ready" } elseif ($permissionCapture) { "permission_denied" } elseif ($captures.Count -gt 0) { "failed" } else { "unavailable" }
    return [ordered]@{
        status = $status
        available = ($status -eq "ready")
        captureCount = $captures.Count
        gpuTraceCount = $gpuTraces.Count
        graphicsCaptureCount = $graphicsCaptures.Count
        metricFileCount = $metricFiles.Count
        captures = $captures.ToArray()
        gpuTraceArtifacts = @($gpuTraces)
        graphicsCaptureArtifacts = @($graphicsCaptures)
        metricArtifacts = @($metricFiles)
        nextAction = switch ($status) {
            "ready" { "将 Nsight GPU Trace/Graphics Capture 指标与同机同配置的优化前后结果对比；不要把 profiling/replay 时间直接当作无开销 FPS 基线"; break }
            "permission_denied" { "以管理员身份运行 capture_performance，或在 NVIDIA Control Panel 开启 GPU performance counters 访问后重试"; break }
            "failed" { "读取 capture_performance 的 Nsight stdout/stderr 和 result.json，确认目标启动、驱动和导出目录"; break }
            default { "当前结果没有 Nsight 性能证据；在桌面 NVIDIA 环境执行 capture_performance" }
        }
    }
}

function Get-DiscoveryEvidence($WorkflowResults, $RootResult = $null, [string]$RootPath = "") {
    $queries = New-Object 'System.Collections.Generic.List[object]'
    if ([string](Get-PropertyValue $RootResult "tool" "") -eq "agent_discovery" -and [bool](Get-PropertyValue $RootResult "success" $false)) {
        [void]$queries.Add([ordered]@{
            source = $RootPath
            stepId = ""
            mode = [string](Get-PropertyValue $RootResult "mode" "")
            resultPath = $RootPath
            context = [ordered]@{
                project = Get-PropertyValue $RootResult "project" $null
                projectContext = Get-PropertyValue $RootResult "projectContext" $null
                scene = Get-PropertyValue $RootResult "scene" $null
                assets = Get-PropertyValue $RootResult "assets" $null
                schema = Get-PropertyValue $RootResult "schema" $null
                capabilities = Get-PropertyValue $RootResult "capabilities" $null
            }
        })
    }
    foreach ($entry in @($WorkflowResults)) {
        $workflow = $entry.value
        foreach ($step in @((Get-PropertyValue $workflow "steps" @()))) {
            $action = [string](Get-PropertyValue $step "action" "")
            if ($action -notin @("inspect_project", "get_project_context", "inspect_scene", "query_assets")) { continue }
            $stepResult = Get-PropertyValue $step "result" $null
            if (-not [bool](Get-PropertyValue $stepResult "success" $false)) { continue }
            [void]$queries.Add([ordered]@{
                source = [string]$entry.path
                stepId = [string](Get-PropertyValue $step "id" "")
                mode = [string](Get-PropertyValue $stepResult "mode" $action)
                resultPath = [string](Get-PropertyValue $stepResult "resultPath" "")
                context = Get-PropertyValue $stepResult "context" $null
            })
        }
    }
    $projectQueries = @($queries | Where-Object { $_.mode -in @("project", "inspect_project") })
    $projectContextQueries = @($queries | Where-Object { $_.mode -in @("context", "get_project_context") })
    $sceneQueries = @($queries | Where-Object { $_.mode -in @("scene", "inspect_scene") })
    $assetQueries = @($queries | Where-Object { $_.mode -in @("assets", "query_assets") })
    $latestScene = if ($sceneQueries.Count -gt 0) { $sceneQueries[$sceneQueries.Count - 1] } else { $null }
    $latestAssets = if ($assetQueries.Count -gt 0) { $assetQueries[$assetQueries.Count - 1] } else { $null }
    $latestProjectContext = if ($projectContextQueries.Count -gt 0) { $projectContextQueries[$projectContextQueries.Count - 1] } else { $null }
    $sceneContext = if ($null -ne $latestScene) { Get-PropertyValue $latestScene "context" $null } else { $null }
    $assetContext = if ($null -ne $latestAssets) { Get-PropertyValue $latestAssets "context" $null } else { $null }
    $projectContext = if ($null -ne $latestProjectContext) { Get-PropertyValue $latestProjectContext "context" $null } else { $null }
    return [ordered]@{
        available = ($queries.Count -gt 0)
        queryCount = $queries.Count
        projectQueryCount = $projectQueries.Count
        projectContextQueryCount = $projectContextQueries.Count
        sceneQueryCount = $sceneQueries.Count
        assetQueryCount = $assetQueries.Count
        latestProjectContext = $projectContext
        latestScene = $sceneContext
        latestAssets = $assetContext
        queries = $queries.ToArray()
        nextAction = if ($queries.Count -gt 0) { "将 discovery context 作为下一轮 Planner 的输入；需要改动时再调用 create_script 或 apply_scene_commands" } else { "先调用 inspect_project；确定场景后调用 inspect_scene 和 query_assets，再生成受控 workflow" }
    }
}

function Get-Recommendations($Failures, $Diagnostics, $Android, $Visual, $Performance, $Discovery = $null) {
    $items = New-Object 'System.Collections.Generic.List[object]'
    foreach ($failure in @($Failures)) {
        $category = [string](Get-PropertyValue $failure "failureCategory" "workflow_error")
        $next = switch ($category) {
            "validation_error" { "优先修复场景 Schema、实体引用或资源路径，再重新 preview/execute"; break }
            "build_error" { "按编译器错误文件和行号生成候选脚本/配置修复，不修改断言来绕过构建失败"; break }
            "runtime_error" { "先对比 gameplay dump 与渲染日志，再决定是否进入 Vulkan/设备专项重试"; break }
            "assertion_failed" { "保留 expected 不变，检查脚本/场景行为并重新跑确定性测试"; break }
            "performance_capture_error" { "优先检查 Nsight GPU counters 权限、目标进程是否持续 Present、驱动兼容性和导出目录"; break }
            default { "读取失败步骤的结构化 diagnostics 和日志，人工审查候选修复后重试"; break }
        }
        [void]$items.Add([ordered]@{ priority = "P0"; source = [string](Get-PropertyValue $failure "stepId" ""); recommendation = $next })
    }
    if (@($Diagnostics | Where-Object { $_.class -eq "build" }).Count -gt 0) { [void]$items.Add([ordered]@{ priority = "P0"; source = "diagnostics"; recommendation = "将编译器错误按文件/行/错误码分组后，再生成候选补丁" }) }
    if ([string](Get-PropertyValue $Android "status" "") -ne "ready") { [void]$items.Add([ordered]@{ priority = "P1"; source = "android"; recommendation = [string](Get-PropertyValue $Android "nextAction" "准备 Android 设备") }) }
    if ([int](Get-PropertyValue $Visual "invalidScreenshotCount" 0) -gt 0) { [void]$items.Add([ordered]@{ priority = "P0"; source = "visual"; recommendation = [string](Get-PropertyValue $Visual "nextAction" "修复未就绪或黑屏截图") }) }
    elseif (-not [bool](Get-PropertyValue $Visual "available" $false)) { [void]$items.Add([ordered]@{ priority = "P1"; source = "visual"; recommendation = [string](Get-PropertyValue $Visual "nextAction" "补充视觉证据") }) }
    if (-not [bool](Get-PropertyValue $Performance "available" $false)) { [void]$items.Add([ordered]@{ priority = "P1"; source = "performance"; recommendation = [string](Get-PropertyValue $Performance "nextAction" "补充 Nsight 性能证据") }) }
    if (-not [bool](Get-PropertyValue $Discovery "available" $false)) { [void]$items.Add([ordered]@{ priority = "P1"; source = "discovery"; recommendation = [string](Get-PropertyValue $Discovery "nextAction" "先调用 inspect_project 获取稳定上下文") }) }
    return $items.ToArray()
}

function Write-EvidenceMarkdown($Evidence) {
    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.AppendLine("# Agent Evidence")
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine(("- source: {0}" -f ($Evidence.source.path)))
    [void]$builder.AppendLine(("- target success: {0}" -f ($Evidence.source.targetSuccess)))
    [void]$builder.AppendLine(("- host: {0} / {1}" -f ($Evidence.host.os), ($Evidence.host.processorArchitecture)))
    [void]$builder.AppendLine(("- Android: {0}" -f ($Evidence.platforms.android.status)))
    [void]$builder.AppendLine(("- discovery context: {0} project={1}, scene={2}, assets={3}" -f ($Evidence.discovery.available), ($Evidence.discovery.projectQueryCount), ($Evidence.discovery.sceneQueryCount), ($Evidence.discovery.assetQueryCount)))
    [void]$builder.AppendLine(("- visual evidence: {0} screenshot={1} usable={2} invalid={3}, mrt={4}, renderdoc={5}" -f ($Evidence.visual.available), ($Evidence.visual.screenshotCount), ($Evidence.visual.usableScreenshotCount), ($Evidence.visual.invalidScreenshotCount), ($Evidence.visual.mrtCount), ($Evidence.visual.renderdocCaptureCount)))
    [void]$builder.AppendLine(("- performance evidence: {0} status={1}, gpu_trace={2}, graphics_capture={3}, metric_files={4}" -f ($Evidence.performance.available), ($Evidence.performance.status), ($Evidence.performance.gpuTraceCount), ($Evidence.performance.graphicsCaptureCount), ($Evidence.performance.metricFileCount)))
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("## Failures")
    if (@($Evidence.failures).Count -eq 0) { [void]$builder.AppendLine("- none") }
    else { foreach ($failure in @($Evidence.failures)) { [void]$builder.AppendLine(("- {0} {1} {2}: {3}" -f ($failure.stepId), ($failure.action), ($failure.failureCategory), ($failure.nextAction))) } }
    [void]$builder.AppendLine("")
    [void]$builder.AppendLine("## Recommendations")
    foreach ($item in @($Evidence.recommendations)) { [void]$builder.AppendLine(("- [{0}] {1}: {2}" -f ($item.priority), ($item.source), ($item.recommendation))) }
    return $builder.ToString()
}

function Write-EvidenceManifest {
    $manifestPath = Join-Path $script:runDir "manifest.json"
    $entries = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse | Where-Object { $_.Name -notin @("manifest.json", "evidence.json") })) {
        $entries += [ordered]@{ path = Get-RelativePath $file.FullName; size = [int64]$file.Length; sha256 = Get-Sha256 $file.FullName }
    }
    $manifest = [ordered]@{ schemaVersion = 1; operation = "agent_evidence"; createdAt = (Get-Date).ToString("o"); files = $entries }
    Write-JsonFile $manifestPath $manifest
    foreach ($entry in @($manifest.files)) { $full = Resolve-ProjectPath ([string]$entry.path) $true; if ((Get-Sha256 $full) -ne [string]$entry.sha256) { throw "evidence manifest 校验失败: $($entry.path)" } }
    return $manifestPath
}

$fatalError = ""
$finalEvidence = $null
try {
    $sourceFull = Resolve-ProjectPath $SourceResultPath $true
    $sourceResult = Read-JsonFile $sourceFull
    $outputRootFull = Resolve-ProjectPath $OutputRoot $false
    if (-not $RunId) { $RunId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8)) }
    if ($RunId -notmatch '^[A-Za-z0-9_.-]{1,80}$') { throw "RunId 只能包含字母、数字、下划线、点和短横线" }
    $runDir = Join-Path $outputRootFull $RunId
    if (Test-Path -LiteralPath $runDir) { throw "evidence run 目录已存在，为避免覆盖请换 RunId: $RunId" }
    New-Item -ItemType Directory -Path $runDir -Force | Out-Null
    $script:runDir = $runDir
    $script:resultPath = Join-Path $runDir "evidence.json"
    [System.IO.File]::Copy($sourceFull, (Join-Path $runDir "input.result.json"), $true)
    [void](Add-ArtifactEvidence (Join-Path $runDir "input.result.json") "evidence_input")
    $workflowResults = Get-WorkflowResults $sourceResult $sourceFull
    [void](Add-ArtifactEvidence $sourceFull "source_result")
    Collect-ReferencedArtifacts $sourceResult
    foreach ($workflowEntry in @($workflowResults)) { Collect-ReferencedArtifacts $workflowEntry.value }
    $logDiagnostics = New-Object 'System.Collections.Generic.List[object]'
    foreach ($artifact in @($script:artifacts | Where-Object { $_.exists -and $_.path -match '(?i)\.(log|txt)$' })) {
        $logText = Read-TextFileSafe $artifact.path
        foreach ($diagnostic in @(Get-LogDiagnostics $logText)) { [void]$logDiagnostics.Add([ordered]@{ source = $artifact.path; class = $diagnostic.class; text = $diagnostic.text }) }
    }
    $workflowObservation = Get-WorkflowObservations $workflowResults
    $allDiagnostics = @($workflowObservation.diagnostics) + @($logDiagnostics.ToArray())
    $android = Get-AndroidEvidence
    $hostEvidence = Get-HostEvidence
    $visual = Get-VisualEvidence
    $performance = Get-PerformanceEvidence $workflowResults $sourceResult $sourceFull
    $discovery = Get-DiscoveryEvidence $workflowResults $sourceResult $sourceFull
    $failures = New-Object 'System.Collections.Generic.List[object]'
    foreach ($failure in @($workflowObservation.failures)) { [void]$failures.Add($failure) }
    if ([string](Get-PropertyValue $sourceResult "tool" "") -eq "agent_task") {
        foreach ($failure in @((Get-PropertyValue $sourceResult "failure" @()))) {
            if ($null -ne $failure -and [string](Get-PropertyValue $failure "failureCategory" "")) { [void]$failures.Add([ordered]@{ source = "task"; stepId = [string](Get-PropertyValue $failure "stepId" ""); action = [string](Get-PropertyValue $failure "action" ""); failureCategory = [string](Get-PropertyValue $failure "failureCategory" ""); nextAction = [string](Get-PropertyValue $failure "nextAction" ""); error = [string](Get-PropertyValue $failure "error" ""); diagnostics = @((Get-PropertyValue $failure "diagnostics" @())) }) }
        }
    }
    $targetSuccess = [bool](Get-PropertyValue $sourceResult "success" $false)
    $uniqueFailures = New-Object 'System.Collections.Generic.List[object]'
    $failureKeys = @{}
    foreach ($failure in $failures.ToArray()) {
        $failureKey = "{0}|{1}|{2}|{3}" -f ([string](Get-PropertyValue $failure "source" "")), ([string](Get-PropertyValue $failure "stepId" "")), ([string](Get-PropertyValue $failure "failureCategory" "")), ([string](Get-PropertyValue $failure "error" ""))
        if (-not $failureKeys.ContainsKey($failureKey)) {
            $failureKeys[$failureKey] = $true
            [void]$uniqueFailures.Add($failure)
        }
    }
    $failureArray = $uniqueFailures.ToArray()
    $recommendations = Get-Recommendations $failureArray $allDiagnostics $android $visual $performance $discovery
    $finalEvidence = [ordered]@{
        schemaVersion = 1
        tool = "agent_evidence"
        apiVersion = 1
        success = $true
        generatedAt = (Get-Date).ToString("o")
        source = [ordered]@{ path = Get-RelativePath $sourceFull; sha256 = Get-Sha256 $sourceFull; tool = [string](Get-PropertyValue $sourceResult "tool" ""); targetSuccess = $targetSuccess; targetRunId = [string](Get-PropertyValue $sourceResult "runId" "") }
        host = $hostEvidence
        platforms = [ordered]@{ host = "windows"; android = $android }
        observations = $workflowObservation.observations
        failures = $failureArray
        diagnostics = @($allDiagnostics | Select-Object -First 160)
        visual = $visual
        performance = $performance
        discovery = $discovery
        artifacts = $script:artifacts.ToArray()
        recommendations = $recommendations
        nextAction = if ($targetSuccess) { "目标任务已通过；可将本 evidence 交给 planner，继续补充 Android、视觉或 Nsight 性能证据" } else { "根据 failures、diagnostics 和 recommendations 生成候选 task，先 preview 再 execute" }
    }
    $markdownPath = Join-Path $runDir "evidence.md"
    Write-TextFile $markdownPath (Write-EvidenceMarkdown $finalEvidence)
    [void](Add-ArtifactEvidence $markdownPath "evidence_summary")
    $manifestPath = Write-EvidenceManifest
    [void](Add-ArtifactEvidence $manifestPath "evidence_manifest")
    Write-JsonFile $script:resultPath $finalEvidence
    Write-Output "EVIDENCE_RESULT_PATH=$script:resultPath"
    Write-Output "SUCCESS=True"
    exit 0
} catch {
    $fatalError = $_.Exception.Message
    if ($null -ne $script:runDir -and $null -ne $script:resultPath) {
        $errorResult = [ordered]@{ schemaVersion = 1; tool = "agent_evidence"; apiVersion = 1; success = $false; generatedAt = (Get-Date).ToString("o"); error = $fatalError; nextAction = "修复 evidence 输入或环境后重试"; artifacts = $script:artifacts.ToArray() }
        Write-JsonFile $script:resultPath $errorResult
        Write-Output "EVIDENCE_RESULT_PATH=$script:resultPath"
    }
    Write-Output "ERROR=$fatalError"
    Write-Output "SUCCESS=False"
    exit 3
}
