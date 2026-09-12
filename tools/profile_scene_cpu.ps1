# profile_scene_cpu.ps1 - fixed-scene CPU/render-stage benchmark
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\profile_scene_cpu.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\profile_scene_cpu.ps1 -SkipBuild -Runs 5
#
# Set MIKAN_CPU_PROFILE=1 to collect SceneRenderer, EngineMain, VulkanManager,
# and editor hierarchy timings. Use -EditorMode -ProfileView scene/game to
# benchmark the actual editor view paths with a fixed frame count.
#
[CmdletBinding()]
param(
    [string]$ProjectPath = "projects/jolt-cube-stack-prototype",
    [string]$Scene = "scenes/main.json",
    [string]$Game = "joltcubestack",
    [ValidateRange(60, 1000000)][int]$Frames = 180,
    [ValidateRange(1, 20)][int]$Runs = 3,
    [ValidateRange(30, 3600)][int]$TimeoutSeconds = 180,
    [string]$OutputRoot = "out/cpu_profile",
    [switch]$EditorMode,
    [ValidateSet('scene', 'game', 'both')][string]$ProfileView = 'game',
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))

function Resolve-RootedPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "Path must not be empty"
    }
    $candidate = if ([System.IO.Path]::IsPathRooted($Path)) {
        $Path
    } else {
        Join-Path $root $Path
    }
    return [System.IO.Path]::GetFullPath($candidate)
}

function Quote-WindowsArgument([string]$Value) {
    if ($null -eq $Value -or $Value.Length -eq 0) {
        return '""'
    }
    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    $escaped = $Value.Replace('\', '\\').Replace('"', '\"')
    return '"' + $escaped + '"'
}

function Parse-CpuProfileLines([string]$Text, [int]$RunIndex) {
    $records = New-Object 'System.Collections.Generic.List[object]'
    $pattern = '(?<key>[A-Za-z0-9_]+)=(?<value>[^\s]+)'
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -notmatch '\[SceneRenderer\]\[CPU\]') {
            continue
        }
        $values = @{}
        foreach ($match in [regex]::Matches($line, $pattern)) {
            $values[$match.Groups['key'].Value] = $match.Groups['value'].Value
        }
        if (-not $values.ContainsKey('view') -or -not $values.ContainsKey('calls')) {
            continue
        }
        $record = [ordered]@{
            run = $RunIndex
            view = [string]$values['view']
            calls = [int64]$values['calls']
        }
        foreach ($key in @(
                'avg_total_ms', 'avg_prepare_ms', 'avg_geometry_ms',
                'avg_roots_ms', 'avg_model_collect_ms', 'avg_vox_collect_ms',
                'avg_camera_collect_ms', 'avg_light_collect_ms',
                'avg_roots', 'avg_model_groups', 'avg_model_entities')) {
            if ($values.ContainsKey($key)) {
                $record[$key] = [double]::Parse(
                    $values[$key],
                    [System.Globalization.CultureInfo]::InvariantCulture)
            }
        }
        $records.Add([pscustomobject]$record)
    }
    return $records.ToArray()
}

function Parse-VulkanProfileLines([string]$Text, [int]$RunIndex) {
    $records = New-Object 'System.Collections.Generic.List[object]'
    $pattern = '(?<key>[A-Za-z0-9_]+)=(?<value>[^\s]+)'
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -notmatch '\[VulkanManager\]\[CPU\]') {
            continue
        }
        $values = @{}
        foreach ($match in [regex]::Matches($line, $pattern)) {
            $values[$match.Groups['key'].Value] = $match.Groups['value'].Value
        }
        if (-not $values.ContainsKey('frames')) {
            continue
        }
        $record = [ordered]@{
            run = $RunIndex
            frames = [int64]$values['frames']
        }
        foreach ($key in @('avg_frame_wall_ms', 'avg_fence_wait_ms', 'avg_command_record_ms')) {
            if ($values.ContainsKey($key)) {
                $record[$key] = [double]::Parse(
                    $values[$key],
                    [System.Globalization.CultureInfo]::InvariantCulture)
            }
        }
        $records.Add([pscustomobject]$record)
    }
    return $records.ToArray()
}

function Parse-EngineProfileLines([string]$Text, [int]$RunIndex) {
    $records = New-Object 'System.Collections.Generic.List[object]'
    $pattern = '(?<key>[A-Za-z0-9_]+)=(?<value>[^\s]+)'
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -notmatch '\[EngineMain\]\[CPU\]') {
            continue
        }
        $values = @{}
        foreach ($match in [regex]::Matches($line, $pattern)) {
            $values[$match.Groups['key'].Value] = $match.Groups['value'].Value
        }
        if (-not $values.ContainsKey('frames')) {
            continue
        }
        $record = [ordered]@{
            run = $RunIndex
            frames = [int64]$values['frames']
        }
        foreach ($key in @('avg_frame_ms', 'avg_update_ms', 'avg_render_ms', 'avg_present_ms', 'avg_editor_ui_ms')) {
            if ($values.ContainsKey($key)) {
                $record[$key] = [double]::Parse(
                    $values[$key],
                    [System.Globalization.CultureInfo]::InvariantCulture)
            }
        }
        $records.Add([pscustomobject]$record)
    }
    return $records.ToArray()
}

function Parse-HierarchyProfileLines([string]$Text, [int]$RunIndex) {
    $records = New-Object 'System.Collections.Generic.List[object]'
    $pattern = '(?<key>[A-Za-z0-9_]+)=(?<value>[^\s]+)'
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -notmatch '\[HierarchyWindow\]\[CPU\]') {
            continue
        }
        $values = @{}
        foreach ($match in [regex]::Matches($line, $pattern)) {
            $values[$match.Groups['key'].Value] = $match.Groups['value'].Value
        }
        if (-not $values.ContainsKey('frames')) {
            continue
        }
        $record = [ordered]@{
            run = $RunIndex
            frames = [int64]$values['frames']
        }
        foreach ($key in @('avg_ms', 'avg_roots')) {
            if ($values.ContainsKey($key)) {
                $record[$key] = [double]::Parse(
                    $values[$key],
                    [System.Globalization.CultureInfo]::InvariantCulture)
            }
        }
        $records.Add([pscustomobject]$record)
    }
    return $records.ToArray()
}

$projectAbsolute = Resolve-RootedPath $ProjectPath
$buildDir = Join-Path $root "out/build/x64-Release"
$engineExe = Join-Path $buildDir "MikanEngine.exe"
$sceneAbsolute = Join-Path $projectAbsolute $Scene
$outputAbsolute = Resolve-RootedPath $OutputRoot

if (-not (Test-Path -LiteralPath (Join-Path $projectAbsolute "project.json") -PathType Leaf)) {
    throw "Project manifest does not exist: $(Join-Path $projectAbsolute 'project.json')"
}
if (-not (Test-Path -LiteralPath $sceneAbsolute -PathType Leaf)) {
    throw "Scene does not exist: $sceneAbsolute"
}

New-Item -ItemType Directory -Path $outputAbsolute -Force | Out-Null
$runId = (Get-Date -Format "yyyyMMdd_HHmmss") + "_" + ([guid]::NewGuid().ToString("N").Substring(0, 8))
$runDirectory = Join-Path $outputAbsolute $runId
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null

if (-not $SkipBuild) {
    $buildScript = Join-Path $root "tools/build.ps1"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript -Target MikanEngine -KillEngine
    if ($LASTEXITCODE -ne 0) {
        throw "MikanEngine build failed with exit code $LASTEXITCODE"
    }
}

if (-not (Test-Path -LiteralPath $engineExe -PathType Leaf)) {
    throw "MikanEngine.exe does not exist; build it first: $engineExe"
}

$allRecords = New-Object 'System.Collections.Generic.List[object]'
$allFrameRecords = New-Object 'System.Collections.Generic.List[object]'
$allEngineRecords = New-Object 'System.Collections.Generic.List[object]'
$allHierarchyRecords = New-Object 'System.Collections.Generic.List[object]'
for ($runIndex = 1; $runIndex -le $Runs; $runIndex++) {
    $stdoutPath = Join-Path $runDirectory ("run_{0}.stdout.log" -f $runIndex)
    $stderrPath = Join-Path $runDirectory ("run_{0}.stderr.log" -f $runIndex)
    $arguments = @(
        $(if ($EditorMode) { "--headless-editor" } else { "--headless" }),
        "--project", $projectAbsolute,
        "--scene", $Scene,
        "--frames", [string]$Frames,
        "--fixed-dt", "0.0166667",
        "--no-voxel-world"
    )
    if (-not [string]::IsNullOrWhiteSpace($Game)) {
        $arguments += @("--game", $Game)
    }

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $engineExe
    $startInfo.WorkingDirectory = $buildDir
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.Arguments = (($arguments | ForEach-Object { Quote-WindowsArgument ([string]$_) }) -join " ")
    $startInfo.EnvironmentVariables["MIKAN_CPU_PROFILE"] = "1"
    if ($EditorMode) {
        $startInfo.EnvironmentVariables["MIKAN_PROFILE_VIEW"] = $ProfileView
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    Write-Host ("[CPU profile] run {0}/{1}: {2}" -f $runIndex, $Runs, $startInfo.Arguments)
    if (-not $process.Start()) {
        throw "Could not start MikanEngine.exe"
    }

    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $finished = $process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $finished) {
        try { $process.Kill() } catch {}
        try { $process.WaitForExit() } catch {}
        throw "Benchmark run $runIndex timed out"
    }
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    [System.IO.File]::WriteAllText($stdoutPath, $stdout, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($stderrPath, $stderr, [System.Text.UTF8Encoding]::new($false))

    if ($process.ExitCode -ne 0) {
        throw "Benchmark run $runIndex failed with exit code $($process.ExitCode); see $stderrPath"
    }

    $records = Parse-CpuProfileLines $stdout $runIndex
    if ($records.Count -eq 0) {
        throw "No CPU timing output was found in run $runIndex; use a timed MikanEngine build"
    }

    # Each view emits cumulative windows; keep the last record for this run.
    foreach ($viewGroup in ($records | Group-Object view)) {
        $allRecords.Add($viewGroup.Group[-1])
    }

    $frameRecords = Parse-VulkanProfileLines $stdout $runIndex
    if ($frameRecords.Count -gt 0) {
        $allFrameRecords.Add($frameRecords[-1])
    }

    if ($EditorMode) {
        $engineRecords = Parse-EngineProfileLines $stdout $runIndex
        $hierarchyRecords = Parse-HierarchyProfileLines $stdout $runIndex
        if ($engineRecords.Count -eq 0) {
            throw "No EngineMain timing output was found in editor run $runIndex"
        }
        $allEngineRecords.Add($engineRecords[-1])
        if ($hierarchyRecords.Count -gt 0) {
            $allHierarchyRecords.Add($hierarchyRecords[-1])
        }
    }
}

$metricNames = @(
    'avg_total_ms', 'avg_prepare_ms', 'avg_geometry_ms',
    'avg_roots_ms', 'avg_model_collect_ms', 'avg_vox_collect_ms',
    'avg_camera_collect_ms', 'avg_light_collect_ms',
    'avg_roots', 'avg_model_groups', 'avg_model_entities')
$summary = [ordered]@{
    schemaVersion = 2
    runId = $runId
    project = $projectAbsolute
    scene = $sceneAbsolute
    frames = $Frames
    runs = $Runs
    profile = 'MIKAN_CPU_PROFILE'
    records = $allRecords.ToArray()
    frameRecords = $allFrameRecords.ToArray()
    engineRecords = $allEngineRecords.ToArray()
    hierarchyRecords = $allHierarchyRecords.ToArray()
    mean = [ordered]@{}
    frameMean = [ordered]@{}
    engineMean = [ordered]@{}
    hierarchyMean = [ordered]@{}
}

foreach ($viewGroup in ($allRecords | Group-Object view)) {
    $mean = [ordered]@{}
    foreach ($metric in $metricNames) {
        $values = @($viewGroup.Group | ForEach-Object { [double]$_.$metric })
        if ($values.Count -gt 0) {
            $mean[$metric] = ($values | Measure-Object -Average).Average
        }
    }
    $summary.mean[$viewGroup.Name] = [pscustomobject]$mean
}

if ($allFrameRecords.Count -gt 0) {
    foreach ($metric in @('avg_frame_wall_ms', 'avg_fence_wait_ms', 'avg_command_record_ms')) {
        $values = @($allFrameRecords | ForEach-Object { [double]$_.$metric })
        if ($values.Count -gt 0) {
            $summary.frameMean[$metric] = ($values | Measure-Object -Average).Average
        }
    }
}

foreach ($metric in @('avg_frame_ms', 'avg_update_ms', 'avg_render_ms', 'avg_present_ms', 'avg_editor_ui_ms')) {
    $values = @($allEngineRecords | ForEach-Object { [double]$_.$metric })
    if ($values.Count -gt 0) {
        $summary.engineMean[$metric] = ($values | Measure-Object -Average).Average
    }
}

foreach ($metric in @('avg_ms', 'avg_roots')) {
    $values = @($allHierarchyRecords | ForEach-Object { [double]$_.$metric })
    if ($values.Count -gt 0) {
        $summary.hierarchyMean[$metric] = ($values | Measure-Object -Average).Average
    }
}

$resultPath = Join-Path $runDirectory "result.json"
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding UTF8

Write-Host "[CPU profile] result: $resultPath"
foreach ($viewGroup in ($allRecords | Group-Object view)) {
    $meanTotal = ($viewGroup.Group | ForEach-Object { [double]$_.avg_total_ms } | Measure-Object -Average).Average
    $meanPrepare = ($viewGroup.Group | ForEach-Object { [double]$_.avg_prepare_ms } | Measure-Object -Average).Average
    $meanGeometry = ($viewGroup.Group | ForEach-Object { [double]$_.avg_geometry_ms } | Measure-Object -Average).Average
    Write-Host ("[CPU profile] view={0} total={1:N3}ms prepare={2:N3}ms geometry={3:N3}ms" -f $viewGroup.Name, $meanTotal, $meanPrepare, $meanGeometry)
}
if ($allFrameRecords.Count -gt 0) {
    $wall = ($allFrameRecords | ForEach-Object { [double]$_.avg_frame_wall_ms } | Measure-Object -Average).Average
    $wait = ($allFrameRecords | ForEach-Object { [double]$_.avg_fence_wait_ms } | Measure-Object -Average).Average
    $record = ($allFrameRecords | ForEach-Object { [double]$_.avg_command_record_ms } | Measure-Object -Average).Average
    Write-Host ("[CPU profile] FrameRender wall={0:N3}ms fence_wait={1:N3}ms command_record={2:N3}ms" -f $wall, $wait, $record)
} else {
    Write-Host "[CPU profile] FrameRender timing not found (requires at least 60 frames)"
}
if ($allEngineRecords.Count -gt 0) {
    foreach ($recordGroup in ($allEngineRecords | Group-Object run)) {
        $record = $recordGroup.Group[-1]
        Write-Host ("[CPU profile] EngineMain run={0} frame={1:N3}ms update={2:N3}ms render={3:N3}ms present={4:N3}ms editor_ui={5:N3}ms" -f `
            $record.run, $record.avg_frame_ms, $record.avg_update_ms, $record.avg_render_ms, $record.avg_present_ms, $record.avg_editor_ui_ms)
    }
}
if ($allHierarchyRecords.Count -gt 0) {
    $hierarchyMs = ($allHierarchyRecords | ForEach-Object { [double]$_.avg_ms } | Measure-Object -Average).Average
    Write-Host ("[CPU profile] HierarchyWindow avg={0:N3}ms" -f $hierarchyMs)
}
