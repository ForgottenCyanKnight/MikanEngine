# nsight_capture.ps1 - MikanEngine 桌面端 Nsight Graphics 性能采集
# ------------------------------------------------------------------
# 受控启动 MikanEngine，并将 Nsight Graphics GPU Trace 或 Graphics Capture
# 结果整理为 Agent 可消费的 result.json、性能指标和 manifest。
#
# 这个脚本不自动提权、不修改 NVIDIA 控制面板设置，也不安装 Nsight。
# GPU Trace 需要目标机允许访问 GPU performance counters；权限不足时会
# 保留完整诊断并返回 permission_denied，便于上层 Agent 生成下一步动作。
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$TargetPath,

    [string[]]$TargetArguments = @(),

    [string]$TargetArgumentsJson = "",

    [string]$TargetArgumentsBase64 = "",

    [string]$WorkingDirectory = "",

    [ValidateSet("gpu_trace", "graphics_capture")]
    [string]$CaptureType = "gpu_trace",

    [int]$CaptureFrame = 60,

    [int]$FrameCount = 1,

    [int]$MaxDurationMilliseconds = 5000,

    [int]$TimeoutSeconds = 360,

    [int]$TraceTimeoutSeconds = 240,

    [int]$ReplayLoops = 3,

    [string]$OutputRoot = "out\nsight_captures",

    [string]$NsightPath = "",

    [string]$RunId = "",

    [ValidateSet("unaltered", "base", "maximum")]
    [string]$SetGpuClocks = "unaltered",

    [switch]$Preview,

    [switch]$SkipReplay
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
$script:runDir = $null
$script:resultPath = $null
$script:manifestPath = $null
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

if (-not [string]::IsNullOrWhiteSpace($TargetArgumentsJson) -and
    -not [string]::IsNullOrWhiteSpace($TargetArgumentsBase64)) {
    throw "TargetArgumentsJson 与 TargetArgumentsBase64 只能传一个"
}
if (-not [string]::IsNullOrWhiteSpace($TargetArgumentsBase64)) {
    try {
        $TargetArgumentsJson = [System.Text.Encoding]::UTF8.GetString(
            [System.Convert]::FromBase64String($TargetArgumentsBase64))
    } catch {
        throw "TargetArgumentsBase64 不是合法 UTF-8 Base64: $($_.Exception.Message)"
    }
}
if (-not [string]::IsNullOrWhiteSpace($TargetArgumentsJson)) {
    try { $decodedArguments = $TargetArgumentsJson | ConvertFrom-Json }
    catch { throw "TargetArgumentsJson 不是合法 JSON: $($_.Exception.Message)" }
    if ($null -eq $decodedArguments) {
        $TargetArguments = @()
    } elseif ($decodedArguments -is [System.Array]) {
        $TargetArguments = @($decodedArguments | ForEach-Object { [string]$_ })
    } elseif ($decodedArguments -is [string]) {
        $TargetArguments = @([string]$decodedArguments)
    } else {
        throw "TargetArgumentsJson 必须是 JSON 字符串数组"
    }
}

function Write-JsonFile([string]$Path, $Value) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [System.IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 60), $utf8NoBom)
}

function Write-TextFile([string]$Path, [string]$Text) {
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [System.IO.File]::WriteAllText($Path, [string]$Text, $utf8NoBom)
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
    if ($full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($prefix.Length).Replace('\', '/')
    }
    return $full.Replace('\', '/')
}

function Get-Sha256([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "" }
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

function Invoke-CapturedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList,
        [Parameter(Mandatory = $true)][string]$ProcessWorkingDirectory,
        [int]$ProcessTimeoutSeconds = 180
    )
    $result = [ordered]@{
        available = $true
        exitCode = $null
        timedOut = $false
        durationMs = 0
        stdout = ""
        stderr = ""
        error = ""
    }
    $started = Get-Date
    try {
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $FilePath
        $startInfo.WorkingDirectory = $ProcessWorkingDirectory
        $startInfo.Arguments = (($ArgumentList | ForEach-Object { ConvertTo-WindowsCommandLineArg ([string]$_) }) -join " ")
        $startInfo.UseShellExecute = $false
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.CreateNoWindow = $true
        $process = [System.Diagnostics.Process]::Start($startInfo)
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($ProcessTimeoutSeconds * 1000)) {
            $result.timedOut = $true
            try {
                & "$env:SystemRoot\System32\taskkill.exe" /PID $process.Id /T /F 2>$null | Out-Null
            } catch {
                try { $process.Kill() } catch {}
            }
            [void]$process.WaitForExit(5000)
        } else {
            [void]$process.WaitForExit()
        }
        if ($stdoutTask.IsCompleted) { try { $result.stdout = $stdoutTask.Result } catch {} }
        if ($stderrTask.IsCompleted) { try { $result.stderr = $stderrTask.Result } catch {} }
        if ($process.HasExited) { $result.exitCode = [int]$process.ExitCode }
    } catch {
        $result.error = $_.Exception.Message
    }
    $result.durationMs = [int]((New-TimeSpan -Start $started -End (Get-Date)).TotalMilliseconds)
    return [pscustomobject]$result
}

function Add-PathCandidate($List, [string]$Candidate) {
    if ([string]::IsNullOrWhiteSpace($Candidate)) { return }
    try {
        $full = [System.IO.Path]::GetFullPath($Candidate)
        if ($List -notcontains $full) { [void]$List.Add($full) }
    } catch {}
}

function Resolve-NsightTools {
    $directories = New-Object 'System.Collections.Generic.List[string]'
    $files = New-Object 'System.Collections.Generic.List[string]'
    $inputs = @($NsightPath, $env:NSIGHT_GRAPHICS_PATH, $env:NSIGHT_GRAPHICS_BIN)
    foreach ($input in $inputs) {
        if ([string]::IsNullOrWhiteSpace([string]$input)) { continue }
        try {
            $full = [System.IO.Path]::GetFullPath([string]$input)
            if (Test-Path -LiteralPath $full -PathType Container) { Add-PathCandidate $directories $full; continue }
            if (Test-Path -LiteralPath $full -PathType Leaf) {
                $name = [System.IO.Path]::GetFileName($full).ToLowerInvariant()
                if ($name -in @('ngfx.exe', 'ngfx-capture.exe', 'ngfx-replay.exe', 'ngfx-ui.exe')) {
                    Add-PathCandidate $directories (Split-Path -Parent $full)
                } else {
                    Add-PathCandidate $files $full
                }
            }
        } catch {}
    }
    foreach ($name in @('ngfx.exe', 'ngfx-capture.exe', 'ngfx-replay.exe')) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($null -ne $command) { Add-PathCandidate $files $command.Source }
    }
    foreach ($programRoot in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramW6432, 'D:\Program Files')) {
        if ([string]::IsNullOrWhiteSpace([string]$programRoot) -or -not (Test-Path -LiteralPath $programRoot -PathType Container)) { continue }
        $searchRoots = @($programRoot, (Join-Path $programRoot 'NVIDIA Corporation'))
        foreach ($searchRoot in $searchRoots) {
            if (-not (Test-Path -LiteralPath $searchRoot -PathType Container)) { continue }
            foreach ($product in @(Get-ChildItem -LiteralPath $searchRoot -Directory -Filter 'Nsight Graphics*' -ErrorAction SilentlyContinue)) {
            $hostRoot = Join-Path $product.FullName 'host'
            if (-not (Test-Path -LiteralPath $hostRoot -PathType Container)) { continue }
            foreach ($hostDirectory in @(Get-ChildItem -LiteralPath $hostRoot -Directory -ErrorAction SilentlyContinue)) {
                Add-PathCandidate $directories $hostDirectory.FullName
            }
            }
        }
    }
    foreach ($file in @($files)) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
        $name = [System.IO.Path]::GetFileName($file).ToLowerInvariant()
        if ($name -eq 'ngfx.exe') { Add-PathCandidate $directories (Split-Path -Parent $file) }
    }
    $ngfx = $null
    $capture = $null
    $replay = $null
    foreach ($directory in @($directories)) {
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) { continue }
        if (-not $ngfx) {
            $candidate = Join-Path $directory 'ngfx.exe'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $ngfx = $candidate }
        }
        if (-not $capture) {
            $candidate = Join-Path $directory 'ngfx-capture.exe'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $capture = $candidate }
        }
        if (-not $replay) {
            $candidate = Join-Path $directory 'ngfx-replay.exe'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $replay = $candidate }
        }
    }
    foreach ($file in @($files)) {
        $name = [System.IO.Path]::GetFileName($file).ToLowerInvariant()
        if ($name -eq 'ngfx.exe' -and -not $ngfx) { $ngfx = $file }
        if ($name -eq 'ngfx-capture.exe' -and -not $capture) { $capture = $file }
        if ($name -eq 'ngfx-replay.exe' -and -not $replay) { $replay = $file }
    }
    return [ordered]@{ ngfx = $ngfx; capture = $capture; replay = $replay }
}

function Get-IsAdministrator {
    try {
        $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
        $principal = [System.Security.Principal.WindowsPrincipal]::new($identity)
        return $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch { return $false }
}

function Get-ToolVersion([string]$ToolPath) {
    if ([string]::IsNullOrWhiteSpace($ToolPath) -or -not (Test-Path -LiteralPath $ToolPath -PathType Leaf)) { return "" }
    try {
        $versionResult = Invoke-CapturedProcess -FilePath $ToolPath -ArgumentList @('--version') -ProcessWorkingDirectory $root -ProcessTimeoutSeconds 15
        $text = (($versionResult.stdout + "`n" + $versionResult.stderr) -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -First 5) -join "\n"
        return $text.Trim()
    } catch { return "" }
}

function Add-Artifact([string]$Path, [string]$Kind) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = Get-RelativePath $item.FullName
        kind = $Kind
        exists = $true
        size = [int64]$item.Length
        sha256 = Get-Sha256 $item.FullName
    }
}

function Get-RunArtifacts {
    $artifacts = New-Object 'System.Collections.Generic.List[object]'
    foreach ($item in @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse | Sort-Object FullName)) {
        if ($item.Name -in @('result.json', 'manifest.json')) { continue }
        $kind = 'metadata'
        if ($item.Extension -ieq '.ngfx-gputrace') { $kind = 'nsight_gpu_trace' }
        elseif ($item.Extension -ieq '.ngfx-capture') { $kind = 'nsight_graphics_capture' }
        elseif ($item.Name -ieq 'iteration_times.csv') { $kind = 'nsight_replay_metrics' }
        elseif ($item.Extension -ieq '.xls') { $kind = 'nsight_metrics' }
        elseif ($item.Extension -match '(?i)^\.(png|jpg|jpeg|bmp|exr)$') { $kind = 'nsight_image' }
        elseif ($item.Extension -match '(?i)^\.(log|txt|csv)$') { $kind = 'log' }
        $artifact = Add-Artifact $item.FullName $kind
        if ($null -ne $artifact) { [void]$artifacts.Add($artifact) }
    }
    return $artifacts.ToArray()
}

function Write-RunManifest {
    $entries = New-Object 'System.Collections.Generic.List[object]'
    foreach ($item in @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse | Where-Object { $_.Name -notin @('result.json', 'manifest.json') })) {
        [void]$entries.Add([ordered]@{
            path = Get-RelativePath $item.FullName
            size = [int64]$item.Length
            sha256 = Get-Sha256 $item.FullName
        })
    }
    $manifest = [ordered]@{
        schemaVersion = 1
        operation = 'nsight_capture'
        captureType = $CaptureType
        createdAt = (Get-Date).ToString('o')
        files = $entries.ToArray()
    }
    Write-JsonFile $script:manifestPath $manifest
    $null = [System.IO.File]::ReadAllText($script:manifestPath, $utf8NoBom)
    foreach ($entry in @($manifest.files)) {
        $full = Resolve-ProjectPath ([string]$entry.path) $true
        if ((Get-Sha256 $full) -ne [string]$entry.sha256) { throw "Nsight capture manifest 校验失败: $($entry.path)" }
    }
    return $manifest
}

function Get-FrameLimit([string[]]$Arguments) {
    for ($index = 0; $index -lt $Arguments.Count; $index++) {
        $argument = [string]$Arguments[$index]
        if ($argument -match '^--frames=(\d+)$') { return [int]$Matches[1] }
        if ($argument -eq '--frames' -and $index + 1 -lt $Arguments.Count) {
            $parsed = 0
            if ([int]::TryParse([string]$Arguments[$index + 1], [ref]$parsed)) { return $parsed }
        }
    }
    return 0
}

function Get-NumericValue([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return $null }
    $normalized = $Value.Trim().Replace(',', '')
    $normalized = $normalized.TrimEnd('%')
    $number = 0.0
    if ([double]::TryParse($normalized, [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture, [ref]$number)) { return $number }
    return $null
}

function Read-MetricFile([string]$Path) {
    $metrics = [ordered]@{}
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $metrics }
    foreach ($line in @(Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue)) {
        $parts = @([string]$line -split "`t", 2)
        if ($parts.Count -ne 2) { continue }
        $key = $parts[0].Trim()
        if (-not $key -or $key -eq 'event_text') { continue }
        $valueText = $parts[1].Trim()
        $numeric = Get-NumericValue $valueText
        $metrics[$key] = if ($null -ne $numeric) { $numeric } else { $valueText }
    }
    return $metrics
}

function Get-MetricValue($Map, [string]$Key) {
    if ($null -eq $Map) { return $null }
    if ($Map.Contains($Key)) { return $Map[$Key] }
    return $null
}

function Get-GpuTracePerformance([string]$TracePath) {
    $base = Join-Path $script:runDir 'BASE_UNLOCKED'
    $frameFile = Join-Path $base 'FRAME.xls'
    $traceFile = Join-Path $base 'GPUTRACE_FRAME.xls'
    if (-not (Test-Path -LiteralPath $frameFile -PathType Leaf)) {
        $frameCandidate = @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse -Filter 'FRAME.xls' -ErrorAction SilentlyContinue | Select-Object -First 1)
        $frameFile = if ($frameCandidate.Count -gt 0) { [string]$frameCandidate[0].FullName } else { '' }
    }
    if (-not (Test-Path -LiteralPath $traceFile -PathType Leaf)) {
        $traceCandidate = @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse -Filter 'GPUTRACE_FRAME.xls' -ErrorAction SilentlyContinue | Select-Object -First 1)
        $traceFile = if ($traceCandidate.Count -gt 0) { [string]$traceCandidate[0].FullName } else { '' }
    }
    $frameMetrics = Read-MetricFile $frameFile
    $traceMetrics = Read-MetricFile $traceFile
    $aliases = [ordered]@{
        gpuFrameTimeMs = @($frameMetrics, 'GPU frame time')
        graphicsEngineActivePct = @($traceMetrics, 'FE_B.TriageAC.gr__cycles_active.avg.pct_of_peak_sustained_elapsed')
        smThroughputPct = @($traceMetrics, 'TriageAC.sm__throughput.avg.pct_of_peak_sustained_elapsed')
        l1texThroughputPct = @($traceMetrics, 'SM_A.TriageAC.l1tex__throughput.avg.pct_of_peak_sustained_elapsed')
        dramThroughputPct = @($traceMetrics, 'FBSP.TriageAC.dramc__throughput.avg.pct_of_peak_sustained_elapsed')
        pcieThroughputPct = @($traceMetrics, 'PCI.TriageAC.pcie__throughput.avg.pct_of_peak_sustained_elapsed')
        drawCount = @($traceMetrics, 'FE_B.TriageAC.fe__draw_count.sum')
        dispatchCount = @($traceMetrics, 'FE_A.TriageAC.gr__dispatch_count.sum')
        pixelShaderActiveWarpsPct = @($traceMetrics, 'TPC.TriageAC.tpc__warps_active_shader_ps_realtime.avg.pct_of_peak_sustained_elapsed')
        computeShaderActiveWarpsPct = @($traceMetrics, 'TPC.TriageAC.tpc__warps_active_shader_cs_realtime.avg.pct_of_peak_sustained_elapsed')
        inactiveSmIdleWarpsPct = @($traceMetrics, 'TriageAC.tpc__warps_inactive_sm_idle_realtime.avg.pct_of_peak_sustained_elapsed')
    }
    $selected = [ordered]@{}
    foreach ($alias in $aliases.Keys) {
        $source = $aliases[$alias][0]
        $key = [string]$aliases[$alias][1]
        $value = Get-MetricValue $source $key
        if ($null -ne $value) { $selected[$alias] = $value }
    }
    return [ordered]@{
        metricSource = 'nsight_gpu_trace_export'
        available = ($traceMetrics.Count -gt 0 -or $frameMetrics.Count -gt 0)
        baselineReady = $false
        baselineNote = 'GPU Trace 含采样和 profiling 开销；适合作为同机同配置的热点证据，不把它直接当作无采集开销的游戏 FPS 基线。'
        tracePath = if ($TracePath) { Get-RelativePath $TracePath } else { '' }
        frameMetricPath = if (-not [string]::IsNullOrWhiteSpace($frameFile) -and (Test-Path -LiteralPath $frameFile -PathType Leaf)) { Get-RelativePath $frameFile } else { '' }
        gpuTraceMetricPath = if (-not [string]::IsNullOrWhiteSpace($traceFile) -and (Test-Path -LiteralPath $traceFile -PathType Leaf)) { Get-RelativePath $traceFile } else { '' }
        metricCount = $traceMetrics.Count + $frameMetrics.Count
        metrics = $selected
    }
}

function Read-ReplayCsv([string]$Path) {
    $rows = New-Object 'System.Collections.Generic.List[object]'
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $rows.ToArray() }
    try {
        foreach ($row in @(Import-Csv -LiteralPath $Path)) {
            $item = [ordered]@{}
            foreach ($name in @('i', 'replayTotalFps', 'replayAdjustedFps', 'msSubmitTime', 'msFinishTime', 'msFrameTime', 'msGpuTime', 'numFrames', 'mae')) {
                if ($null -ne $row.PSObject.Properties[$name]) {
                    $number = Get-NumericValue ([string]$row.$name)
                    $item[$name] = if ($null -ne $number) { $number } else { [string]$row.$name }
                }
            }
            if ($item.Count -gt 0) { [void]$rows.Add($item) }
        }
    } catch {}
    return $rows.ToArray()
}

function Get-ReplayPerformance([string]$CapturePath) {
    $csv = @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse -Filter 'iteration_times.csv' -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -First 1)
    $rowValue = if ($csv.Count -gt 0) { Read-ReplayCsv $csv[0].FullName } else { @() }
    $rows = @($rowValue)
    $frameTimes = @($rows | Where-Object { $null -ne $_.msFrameTime -and [double]$_.msFrameTime -ge 0 } | ForEach-Object { [double]$_.msFrameTime })
    $gpuTimes = @($rows | Where-Object { $null -ne $_.msGpuTime -and [double]$_.msGpuTime -ge 0 } | ForEach-Object { [double]$_.msGpuTime })
    $averageFrameTime = if ($frameTimes.Count -gt 0) { ($frameTimes | Measure-Object -Average).Average } else { $null }
    $averageGpuTime = if ($gpuTimes.Count -gt 0) { ($gpuTimes | Measure-Object -Average).Average } else { $null }
    return [ordered]@{
        metricSource = 'nsight_replay'
        available = ($rows.Count -gt 0)
        baselineReady = ($rows.Count -gt 0)
        baselineNote = if ($gpuTimes.Count -eq 0) { '当前 replay CSV 未提供真实 GPU 时间（msGpuTime=-1）；msFrameTime 是 replay 计时，不等于应用原生 GPU frame time。' } else { 'replay 指标用于同机同配置对比；仍需结合真实应用 GPU Trace 判断 GPU 瓶颈。' }
        capturePath = if ($CapturePath) { Get-RelativePath $CapturePath } else { '' }
        csvPath = if ($csv.Count -gt 0) { Get-RelativePath $csv[0].FullName } else { '' }
        iterationCount = $rows.Count
        averageFrameTimeMs = $averageFrameTime
        averageGpuTimeMs = $averageGpuTime
        rows = $rows
    }
}

function Get-PermissionFailure([string]$Text) {
    return $Text -match '(?i)(GPU Performance Counters unavailable|ERR_NVGPUCTRPERM|access to GPU performance counters|permission.{0,40}GPU performance counters)'
}

function Get-IsProcessFailure($ProcessResult) {
    return ($ProcessResult.timedOut -or $null -eq $ProcessResult.exitCode -or $ProcessResult.exitCode -ne 0 -or -not [string]::IsNullOrWhiteSpace([string]$ProcessResult.error))
}

$fatalError = ""
$runResult = $null
try {
    $targetFull = Resolve-ProjectPath $TargetPath $true
    $targetWorkingDirectory = if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        Split-Path -Parent $targetFull
    } else {
        Resolve-ProjectPath $WorkingDirectory $false
    }
    if (-not (Test-Path -LiteralPath $targetWorkingDirectory -PathType Container)) { throw "工作目录不存在: $targetWorkingDirectory" }
    if ($CaptureFrame -lt 1 -or $CaptureFrame -gt 1000000) { throw "CaptureFrame 必须在 1..1000000" }
    if ($FrameCount -lt 1 -or $FrameCount -gt 60) { throw "FrameCount 必须在 1..60" }
    if ($MaxDurationMilliseconds -lt 1000 -or $MaxDurationMilliseconds -gt 600000) { throw "MaxDurationMilliseconds 必须在 1000..600000" }
    if ($TimeoutSeconds -lt 1 -or $TimeoutSeconds -gt 3600) { throw "TimeoutSeconds 必须在 1..3600" }
    if ($TraceTimeoutSeconds -lt 1 -or $TraceTimeoutSeconds -gt 3600) { throw "TraceTimeoutSeconds 必须在 1..3600" }
    if ($ReplayLoops -lt 0 -or $ReplayLoops -gt 100) { throw "ReplayLoops 必须在 0..100" }
    if ($CaptureType -eq 'graphics_capture' -and -not $SkipReplay -and $ReplayLoops -eq 0) { throw "graphics_capture 在不使用 SkipReplay 时 ReplayLoops 必须大于 0" }

    foreach ($argument in @($TargetArguments)) {
        $text = [string]$argument
        if ($text -match '^--(crash-log|nsight|renderdoc-capture-frame|renderdoc-capture-path)(=|$)') {
            throw "TargetArguments 不能覆盖性能采集脚本保留参数: $text"
        }
        if ($text -match '^--frames=(\d+)$' -or $text -eq '--frames') { continue }
    }
    $frameLimit = Get-FrameLimit @($TargetArguments)
    if ($frameLimit -gt 0 -and $CaptureFrame -ge $frameLimit) {
        throw "CaptureFrame($CaptureFrame) 必须小于目标 --frames($frameLimit)，需要在目标结束前采集"
    }

    $outputRootFull = Resolve-ProjectPath $OutputRoot $false
    if ([string]::IsNullOrWhiteSpace($RunId)) {
        $RunId = (Get-Date -Format 'yyyyMMdd-HHmmssfff') + '-' + ([Guid]::NewGuid().ToString('N').Substring(0, 8))
    }
    if ($RunId -notmatch '^[A-Za-z0-9._-]{1,80}$') { throw "RunId 含有不允许的字符: $RunId" }
    $script:runDir = Join-Path $outputRootFull $RunId
    if (Test-Path -LiteralPath $script:runDir) { throw "运行目录已存在，请更换 RunId: $script:runDir" }
    New-Item -ItemType Directory -Path $script:runDir -Force | Out-Null
    $script:resultPath = Join-Path $script:runDir 'result.json'
    $script:manifestPath = Join-Path $script:runDir 'manifest.json'

    $tools = Resolve-NsightTools
    $requiredTool = if ($CaptureType -eq 'gpu_trace') { $tools.ngfx } else { $tools.capture }
    $replayRequired = ($CaptureType -eq 'graphics_capture' -and -not $SkipReplay -and $ReplayLoops -gt 0)
    $requiredAvailable = ($null -ne $requiredTool -and (Test-Path -LiteralPath $requiredTool -PathType Leaf) -and (-not $replayRequired -or ($null -ne $tools.replay -and (Test-Path -LiteralPath $tools.replay -PathType Leaf))))
    $toolVersions = [ordered]@{}
    if ($tools.ngfx) { $toolVersions.ngfx = Get-ToolVersion $tools.ngfx }
    if ($tools.capture) { $toolVersions.capture = Get-ToolVersion $tools.capture }
    if ($tools.replay) { $toolVersions.replay = Get-ToolVersion $tools.replay }

    $targetArgumentList = New-Object 'System.Collections.Generic.List[string]'
    foreach ($argument in @($TargetArguments)) { [void]$targetArgumentList.Add([string]$argument) }
    [void]$targetArgumentList.Add('--crash-log')
    [void]$targetArgumentList.Add((Join-Path $script:runDir 'crash_log.txt'))
    $targetArgumentLine = (($targetArgumentList.ToArray() | ForEach-Object { ConvertTo-WindowsCommandLineArg ([string]$_) }) -join ' ')
    $capturePath = Join-Path $script:runDir ('mikan_frame{0}.ngfx-capture' -f $CaptureFrame)
    $nsightArguments = $null
    if ($CaptureType -eq 'gpu_trace') {
        $nsightArguments = @(
            '--no-timeout', '--activity', 'GPU Trace Profiler', '--platform', 'Windows',
            '--output-dir', $script:runDir, '--exe', $targetFull, '--dir', $targetWorkingDirectory,
            '--args', $targetArgumentLine, '--start-after-frames', [string]$CaptureFrame,
            '--limit-to-frames', [string]$FrameCount, '--max-duration-ms', [string]$MaxDurationMilliseconds,
            '--set-gpu-clocks', $SetGpuClocks, '--auto-export', '--trace-timeout', [string]$TraceTimeoutSeconds,
            '--verbose'
        )
    } else {
        $nsightArguments = @(
            '-e', $targetFull, '--working-dir', $targetWorkingDirectory, '-o', $capturePath,
            '--capture-frame', [string]$CaptureFrame, '-n', [string]$FrameCount,
            '--terminate-after-capture', '--no-hud', '--diagnostic-mode', '--args', $targetArgumentLine
        )
    }
    $commandDocument = [ordered]@{
        schemaVersion = 1
        tool = 'nsight_capture'
        mode = if ($Preview) { 'preview' } else { 'execute' }
        captureType = $CaptureType
        nsightPath = if ($NsightPath) { $NsightPath } else { '' }
        tools = $tools
        nsightArguments = $nsightArguments
        targetPath = Get-RelativePath $targetFull
        workingDirectory = Get-RelativePath $targetWorkingDirectory
        targetArguments = $targetArgumentList.ToArray()
        captureFrame = $CaptureFrame
        frameCount = $FrameCount
        maxDurationMilliseconds = $MaxDurationMilliseconds
        replayLoops = $ReplayLoops
        skipReplay = [bool]$SkipReplay
    }
    Write-JsonFile (Join-Path $script:runDir 'command.json') $commandDocument

    $baseResult = [ordered]@{
        schemaVersion = 1
        tool = 'nsight_capture'
        apiVersion = 1
        success = $false
        mode = if ($Preview) { 'preview' } else { 'execute' }
        captureType = $CaptureType
        status = ''
        runId = $RunId
        runDir = Get-RelativePath $script:runDir
        target = [ordered]@{ path = Get-RelativePath $targetFull; workingDirectory = Get-RelativePath $targetWorkingDirectory; arguments = $targetArgumentList.ToArray() }
        nsight = [ordered]@{ available = $requiredAvailable; isAdministrator = Get-IsAdministrator; tools = $tools; versions = $toolVersions }
        capture = [ordered]@{ requestedFrame = $CaptureFrame; frameCount = $FrameCount; path = ''; files = @() }
        performance = $null
        replay = $null
        process = $null
        nextAction = ''
    }

    if ($Preview) {
        $baseResult.success = $true
        $baseResult.status = 'preview'
        $baseResult.nextAction = if ($requiredAvailable) { 'preview 通过；将 mode=execute 运行 Nsight 性能采集' } else { '配置 NsightPath/NSIGHT_GRAPHICS_PATH，确认 ngfx 工具存在后再 execute' }
        $runResult = $baseResult
    } elseif (-not $requiredAvailable) {
        $baseResult.status = if ($replayRequired -and $null -eq $tools.replay) { 'replay_unavailable' } else { 'unavailable' }
        $baseResult.nextAction = if ($baseResult.status -eq 'replay_unavailable') { '已找到 ngfx-capture，但缺少 ngfx-replay；补齐同版本 Nsight Graphics CLI 后重试' } else { '安装 Nsight Graphics，或传入 gfx-ui.exe/ngfx.exe 所在目录；GPU Trace 需要 ngfx.exe' }
        $runResult = $baseResult
    } else {
        $primaryTool = if ($CaptureType -eq 'gpu_trace') { $tools.ngfx } else { $tools.capture }
        $primary = Invoke-CapturedProcess -FilePath $primaryTool -ArgumentList $nsightArguments -ProcessWorkingDirectory $root -ProcessTimeoutSeconds $TimeoutSeconds
        $primaryText = [string]$primary.stdout + "`n" + [string]$primary.stderr
        Write-TextFile (Join-Path $script:runDir 'stdout.log') ([string]$primary.stdout)
        Write-TextFile (Join-Path $script:runDir 'stderr.log') ([string]$primary.stderr)
        $processFailed = Get-IsProcessFailure $primary
        $permissionFailure = ($CaptureType -eq 'gpu_trace' -and (Get-PermissionFailure $primaryText))
        $traceFiles = @()
        $replayResult = $null
        $replayPerformance = $null
        $captureExists = $false
        $replayOk = $true
        if ($CaptureType -eq 'gpu_trace') {
            $traceFiles = @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse -Filter '*.ngfx-gputrace' -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc)
            $hasMetrics = @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse -Filter 'GPUTRACE_FRAME.xls' -ErrorAction SilentlyContinue).Count -gt 0
            $status = if ($permissionFailure) { 'permission_denied' } elseif ($primary.timedOut) { 'target_timeout' } elseif ($processFailed -and $traceFiles.Count -eq 0) { 'target_failed' } elseif ($traceFiles.Count -gt 0 -and $hasMetrics) { 'profiled' } elseif ($traceFiles.Count -gt 0) { 'captured_no_export' } else { 'capture_failed' }
            $tracePath = if ($traceFiles.Count -gt 0) { $traceFiles[$traceFiles.Count - 1].FullName } else { '' }
            $baseResult.performance = Get-GpuTracePerformance $tracePath
            $baseResult.capture.files = @($traceFiles | ForEach-Object { Add-Artifact $_.FullName 'nsight_gpu_trace' } | Where-Object { $null -ne $_ })
            $baseResult.capture.path = if ($tracePath) { Get-RelativePath $tracePath } else { '' }
            $baseResult.process = [ordered]@{ exitCode = $primary.exitCode; timedOut = [bool]$primary.timedOut; durationMs = $primary.durationMs; stdoutPath = Get-RelativePath (Join-Path $script:runDir 'stdout.log'); stderrPath = Get-RelativePath (Join-Path $script:runDir 'stderr.log') }
            $baseResult.status = $status
            $baseResult.success = ($status -eq 'profiled')
            $baseResult.nextAction = switch ($status) {
                'profiled' { '已生成 GPU Trace 与导出指标；交给 agent_evidence/planner 分析热点，并用同一配置复测优化前后差异'; break }
                'permission_denied' { 'Nsight 已启动但 GPU performance counters 权限不足；以管理员身份运行 Nsight/Agent，或在 NVIDIA Control Panel 开启开发者 GPU 性能计数器访问后重试'; break }
                'target_timeout' { '检查目标是否在采样帧前卡住；读取 stdout/stderr，并适当增大 TimeoutSeconds/TraceTimeoutSeconds'; break }
                'target_failed' { '读取 Nsight 与 MikanEngine stdout/stderr，先修复目标启动或 Vulkan 初始化错误'; break }
                'captured_no_export' { 'GPU Trace 已落盘但导出指标缺失；检查 ReportGeneratorTags.txt、BASE_UNLOCKED 和 Nsight 版本'; break }
                default { '确认目标在采样帧前持续运行，且 Nsight GPU Trace CLI 参数与驱动版本匹配后重试' }
            }
            $runResult = $baseResult
        } else {
            $deadline = (Get-Date).AddSeconds(15)
            do {
                $captureExists = Test-Path -LiteralPath $capturePath -PathType Leaf
                if ($captureExists -or (Get-Date) -ge $deadline) { break }
                Start-Sleep -Milliseconds 250
            } while ($true)
            if ($captureExists -and -not $SkipReplay -and $ReplayLoops -gt 0) {
                $replayDir = Join-Path $script:runDir 'replay_perf'
                New-Item -ItemType Directory -Path $replayDir -Force | Out-Null
                $replayArguments = @($capturePath, '--loop-count', [string]$ReplayLoops, '--perf-report-dir', $replayDir, '--present-hidden', '--vsync-off', '--inject-full-frame-perf-marker')
                $replayResult = Invoke-CapturedProcess -FilePath $tools.replay -ArgumentList $replayArguments -ProcessWorkingDirectory $root -ProcessTimeoutSeconds $TimeoutSeconds
                Write-TextFile (Join-Path $script:runDir 'replay.stdout.log') ([string]$replayResult.stdout)
                Write-TextFile (Join-Path $script:runDir 'replay.stderr.log') ([string]$replayResult.stderr)
                $replayPerformance = Get-ReplayPerformance $capturePath
                $replayOk = (-not (Get-IsProcessFailure $replayResult) -and [bool]$replayPerformance.available)
            } elseif ($captureExists) {
                $replayPerformance = Get-ReplayPerformance $capturePath
            } elseif ($replayRequired) {
                $replayOk = $false
            }
            $status = if ($primary.timedOut) { 'target_timeout' } elseif ($processFailed -and -not $captureExists) { 'target_failed' } elseif (-not $captureExists) { 'capture_failed' } elseif ($replayRequired -and -not $replayOk) { 'replay_failed' } else { 'captured' }
            $baseResult.capture.path = if ($captureExists) { Get-RelativePath $capturePath } else { '' }
            $baseResult.capture.files = if ($captureExists) { @(Add-Artifact $capturePath 'nsight_graphics_capture') } else { @() }
            $baseResult.replay = $replayPerformance
            $baseResult.performance = $replayPerformance
            $baseResult.process = [ordered]@{ exitCode = $primary.exitCode; timedOut = [bool]$primary.timedOut; durationMs = $primary.durationMs; stdoutPath = Get-RelativePath (Join-Path $script:runDir 'stdout.log'); stderrPath = Get-RelativePath (Join-Path $script:runDir 'stderr.log'); replay = if ($null -ne $replayResult) { [ordered]@{ exitCode = $replayResult.exitCode; timedOut = [bool]$replayResult.timedOut; durationMs = $replayResult.durationMs; stdoutPath = Get-RelativePath (Join-Path $script:runDir 'replay.stdout.log'); stderrPath = Get-RelativePath (Join-Path $script:runDir 'replay.stderr.log') } } else { $null } }
            $baseResult.status = $status
            $baseResult.success = ($status -eq 'captured')
            $baseResult.nextAction = switch ($status) {
                'captured' { if ($replayPerformance -and $replayPerformance.available) { '已生成 Graphics Capture 和 replay 指标；注意 replay 的 msGpuTime 可能不可用，交给 evidence 做同机对比' } else { '已生成 Graphics Capture；可用 Nsight Graphics 打开或补充 replay 性能报告' }; break }
                'target_timeout' { '检查目标是否在抓帧前卡住；读取 stdout/stderr 并调整 TimeoutSeconds'; break }
                'target_failed' { '读取 Nsight 与 MikanEngine stdout/stderr，先修复目标启动或 Vulkan 初始化错误'; break }
                'replay_failed' { 'Graphics Capture 已生成但 replay/性能报告失败；单独打开 .ngfx-capture，检查 replay 日志和驱动兼容性'; break }
                default { '确认目标在 capture frame 前持续 Present，并检查 ngfx-capture 日志中的目标启动错误' }
            }
            $runResult = $baseResult
        }
    }

    Write-JsonFile $script:resultPath $runResult
    $manifest = Write-RunManifest
    $runResult.manifestPath = Get-RelativePath $script:manifestPath
    $runResult.artifacts = @((Get-RunArtifacts))
    $manifestArtifact = Add-Artifact $script:manifestPath 'manifest'
    if ($null -ne $manifestArtifact) { $runResult.artifacts += @($manifestArtifact) }
    Write-JsonFile $script:resultPath $runResult
    Write-Output "NSIGHT_RESULT_PATH=$script:resultPath"
    Write-Output "SUCCESS=$([bool]$runResult.success)"
    if ($runResult.mode -eq 'preview' -or $runResult.success) { exit 0 }
    if ($runResult.status -in @('unavailable', 'replay_unavailable')) { exit 3 }
    if ($runResult.status -eq 'target_timeout') { exit 2 }
    exit 1
} catch {
    $fatalError = $_.Exception.Message
    if ($null -ne $script:runDir) {
        $errorResult = [ordered]@{
            schemaVersion = 1
            tool = 'nsight_capture'
            apiVersion = 1
            success = $false
            mode = if ($Preview) { 'preview' } else { 'execute' }
            captureType = $CaptureType
            status = 'error'
            runId = $RunId
            runDir = Get-RelativePath $script:runDir
            error = $fatalError
            nextAction = '修复 Nsight 性能采集参数或环境后重试'
        }
        Write-JsonFile $script:resultPath $errorResult
        try {
            $null = Write-RunManifest
            $errorResult.manifestPath = Get-RelativePath $script:manifestPath
            $errorResult.artifacts = @((Get-RunArtifacts))
            $manifestArtifact = Add-Artifact $script:manifestPath 'manifest'
            if ($null -ne $manifestArtifact) { $errorResult.artifacts += @($manifestArtifact) }
            Write-JsonFile $script:resultPath $errorResult
        } catch {}
        Write-Output "NSIGHT_RESULT_PATH=$script:resultPath"
    }
    Write-Output "ERROR=$fatalError"
    Write-Output "SUCCESS=False"
    exit 3
}
