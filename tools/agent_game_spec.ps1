# agent_game_spec.ps1 - MikanEngine AI Native GameSpec 编译、执行与交付
# ------------------------------------------------------------------
# GameSpec 是模型和引擎之间的稳定边界：模型只生成结构化规格，执行器负责
# 生成受控 workflow，调用既有场景命令/脚本 SDK/测试/RenderDoc/Nsight，最后
# 按验收 contract 判断是否允许交付。
#
# 默认 preview，不修改项目、不启动引擎；显式 -Mode execute 才执行。
# 每次运行写入 out\agent_game_specs\<run-id>\，并在成功时生成 delivery.zip。
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameSpecPath,
    [string]$OutputRoot = "out\agent_game_specs",
    [string]$RunId = "",
    [string]$Mode = ""
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$script:runDir = $null
$script:resultPath = $null
$script:spec = $null
$script:specName = ""
$script:steps = New-Object 'System.Collections.Generic.List[object]'
$script:artifacts = New-Object 'System.Collections.Generic.List[object]'
$script:startedAt = Get-Date

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

function Assert-KnownProperties($Object, [string[]]$Allowed, [string]$Context) {
    if ($null -eq $Object) { return }
    $unknown = @($Object.PSObject.Properties.Name | Where-Object { $_ -notin $Allowed })
    if ($unknown.Count -gt 0) { throw "$Context 含不支持字段: $($unknown -join ', ')" }
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

function Get-GameSpecResourceRoot([string]$ProjectFull) {
    $manifestPath = Join-Path $ProjectFull "project.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { return $ProjectFull }
    try {
        $manifest = [System.IO.File]::ReadAllText($manifestPath, $utf8NoBom) | ConvertFrom-Json
        $resourceValue = [string](Get-PropertyValue $manifest "resourceRoot" ".")
        if ([string]::IsNullOrWhiteSpace($resourceValue)) { $resourceValue = "." }
        if ([System.IO.Path]::IsPathRooted($resourceValue)) { throw "resourceRoot 必须是项目内相对路径" }
        $resourceFull = [System.IO.Path]::GetFullPath((Join-Path $ProjectFull $resourceValue))
        $prefix = $ProjectFull.TrimEnd('\', '/') + '\'
        if (-not $resourceFull.Equals($ProjectFull, [System.StringComparison]::OrdinalIgnoreCase) -and
            -not $resourceFull.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "resourceRoot 不能逃逸项目目录"
        }
        return $resourceFull
    } catch {
        throw "无法解析项目资源根: $manifestPath；$($_.Exception.Message)"
    }
}

function Resolve-GameSpecScenePath([string]$SceneValue, [string]$ProjectFull = "") {
    if ($ProjectFull -and -not [System.IO.Path]::IsPathRooted($SceneValue)) {
        $clean = $SceneValue.Replace('\', '/').TrimStart('/')
        $base = if ($clean.StartsWith('assets/', [System.StringComparison]::OrdinalIgnoreCase)) { $ProjectFull } else { Get-GameSpecResourceRoot $ProjectFull }
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
    try { return ([System.BitConverter]::ToString($sha.ComputeHash([System.IO.File]::ReadAllBytes($Path))).Replace('-', '').ToLowerInvariant()) }
    finally { $sha.Dispose() }
}

function Add-Artifact([string]$Path, [string]$Kind = "artifact") {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    try {
        $full = [System.IO.Path]::GetFullPath($Path)
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { return $null }
        $relative = Get-RelativePath $full
        foreach ($existing in $script:artifacts.ToArray()) { if ([string]$existing.path -eq $relative) { return $existing } }
        $item = Get-Item -LiteralPath $full
        $entry = [ordered]@{ path = $relative; kind = $Kind; exists = $true; size = [int64]$item.Length; sha256 = Get-Sha256 $full }
        [void]$script:artifacts.Add([pscustomobject]$entry)
        return [pscustomobject]$entry
    } catch { return $null }
}

function Copy-Json($Value) {
    if ($null -eq $Value) { return $null }
    return ($Value | ConvertTo-Json -Depth 80 | ConvertFrom-Json)
}

function Assert-Name([string]$Value, [string]$Context, [int]$MaxLength = 160) {
    if ([string]::IsNullOrWhiteSpace($Value) -or $Value.Length -gt $MaxLength -or $Value -notmatch '^[A-Za-z][A-Za-z0-9_.-]*$') {
        throw "$Context 不合法: $Value"
    }
}

function Assert-IntegerRange($Value, [string]$Context, [int]$Min, [int]$Max) {
    $number = [int]0
    if (-not [int]::TryParse([string]$Value, [ref]$number) -or $number -lt $Min -or $number -gt $Max) {
        throw "$Context 必须在 $Min..$Max"
    }
    return $number
}

function Assert-FiniteNumber($Value, [string]$Context, [double]$Min, [double]$Max) {
    $number = [double]0
    if (-not [double]::TryParse([string]$Value, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$number) -or
        [double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -le $Min -or $number -gt $Max) {
        throw "$Context 必须位于 ($Min, $Max]"
    }
    return $number
}

function Test-PathUnder([string]$Path, [string]$RelativeDirectory) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $directory = [System.IO.Path]::GetFullPath((Join-Path $root $RelativeDirectory)).TrimEnd('\', '/') + '\'
    return $full.StartsWith($directory, [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-AssetType([string]$AssetType, [string]$Context) {
    if ($AssetType -notin @("all", "scenes", "scripts", "shaders", "models", "textures", "audio")) {
        throw "$Context.assetType 不支持: $AssetType"
    }
}

function Get-MapFromObject($Object) {
    $map = [ordered]@{}
    if ($null -eq $Object) { return $map }
    foreach ($property in $Object.PSObject.Properties) { $map[$property.Name] = $property.Value }
    return $map
}

function New-Step([string]$Id, [string]$Action, [string[]]$DependsOn = @(), $StepArgs = $null, [int]$TimeoutSeconds = 600, [bool]$ContinueOnError = $false) {
    $step = [ordered]@{ id = $Id; action = $Action; timeoutSeconds = $TimeoutSeconds }
    if ($DependsOn.Count -gt 0) { $step.dependsOn = @($DependsOn) }
    if ($null -ne $StepArgs) { $step.args = $StepArgs }
    if ($ContinueOnError) { $step.continueOnError = $true }
    return [pscustomobject]$step
}

function Add-Step([string]$Id, [string]$Action, [string[]]$DependsOn = @(), $StepArgs = $null, [int]$TimeoutSeconds = 600, [bool]$ContinueOnError = $false) {
    foreach ($existing in $script:steps.ToArray()) { if ([string]$existing.id -eq $Id) { throw "GameSpec 生成了重复 workflow step: $Id" } }
    $step = New-Step $Id $Action $DependsOn $StepArgs $TimeoutSeconds $ContinueOnError
    [void]$script:steps.Add($step)
    return $step
}

function Get-LastStepId {
    if ($script:steps.Count -eq 0) { return "" }
    return [string]$script:steps[$script:steps.Count - 1].id
}

function Get-Boolean($Object, [string]$Name, [bool]$Default) {
    if (-not (Test-Property $Object $Name)) { return $Default }
    return [bool](Get-PropertyValue $Object $Name $Default)
}

function Get-OptionalInt($Object, [string]$Name, [int]$Default, [int]$Min, [int]$Max, [string]$Context) {
    if (-not (Test-Property $Object $Name)) { return $Default }
    return Assert-IntegerRange (Get-PropertyValue $Object $Name $Default) "$Context.$Name" $Min $Max
}

function Get-OptionalDouble($Object, [string]$Name, [double]$Default, [double]$Max, [string]$Context) {
    if (-not (Test-Property $Object $Name)) { return $Default }
    return Assert-FiniteNumber (Get-PropertyValue $Object $Name $Default) "$Context.$Name" 0.0 $Max
}

function Convert-CommonTestArgs($Test, [string]$Context, [int]$DefaultFrames = 60, [int]$DefaultTimeout = 60000) {
    $frames = Get-OptionalInt $Test "frames" $DefaultFrames 1 1000000 $Context
    $fixedDt = Get-OptionalDouble $Test "fixedDeltaSeconds" (1.0 / 60.0) 0.1 $Context
    $timeoutMs = Get-OptionalInt $Test "timeoutMs" $DefaultTimeout 1000 7200000 $Context
    $args = [ordered]@{ frames = $frames; fixedDeltaSeconds = $fixedDt; timeoutMs = $timeoutMs }
    if ($Context -eq "tests.render") {
        $args.screenshotFrame = Get-OptionalInt $Test "screenshotFrame" 0 0 1000000 $Context
        if ($args.screenshotFrame -gt $frames) { throw "$Context.screenshotFrame 必须不大于 frames" }
    }
    if (Test-Property $Test "extraArgs") { $args.extraArgs = @((Get-PropertyValue $Test "extraArgs" @()) | ForEach-Object { [string]$_ }) }
    if ($Context -eq "tests.gameplay" -and (Test-Property $Test "replayPath")) {
        $replayPathValue = [string](Get-PropertyValue $Test "replayPath" "")
        if ([string]::IsNullOrWhiteSpace($replayPathValue)) { throw "$Context.replayPath 不能为空" }
        $replayFull = Resolve-ProjectPath $replayPathValue $true
        foreach ($extra in @($args.extraArgs)) {
            if ([string]$extra -match '(?i)^--input-replay(=|$)') {
                throw "$Context.extraArgs 不能重复提供 --input-replay；请使用 replayPath"
            }
        }
        $args.extraArgs = @("--input-replay", $replayFull) + @($args.extraArgs)
    }
    return $args
}

function Convert-CaptureArgs($Capture) {
    $captureFrame = Get-OptionalInt $Capture "captureFrame" 60 1 1000000 "tests.capture"
    $frames = Get-OptionalInt $Capture "frames" ([Math]::Max(120, $captureFrame + 30)) $captureFrame 1000000 "tests.capture"
    $fixedDt = Get-OptionalDouble $Capture "fixedDeltaSeconds" (1.0 / 60.0) 0.1 "tests.capture"
    $args = [ordered]@{
        captureFrame = $captureFrame
        frames = $frames
        fixedDeltaSeconds = $fixedDt
        captureWaitSeconds = Get-OptionalInt $Capture "captureWaitSeconds" 10 0 120 "tests.capture"
        captureTimeoutSeconds = Get-OptionalInt $Capture "captureTimeoutSeconds" 180 1 3600 "tests.capture"
    }
    foreach ($field in @("renderDocCmdPath", "extraArgs")) {
        if (Test-Property $Capture $field) { $args[$field] = Get-PropertyValue $Capture $field $null }
    }
    foreach ($field in @("apiValidation", "captureCallstacks", "skipThumbnail")) {
        if (Test-Property $Capture $field) { $args[$field] = [bool](Get-PropertyValue $Capture $field $false) }
    }
    return $args
}

function Convert-PerformanceArgs($Performance) {
    $captureFrame = Get-OptionalInt $Performance "captureFrame" 60 1 1000000 "tests.performance"
    $frames = Get-OptionalInt $Performance "frames" ([Math]::Max(180, $captureFrame + 30)) $captureFrame 1000000 "tests.performance"
    $args = [ordered]@{
        captureType = [string](Get-PropertyValue $Performance "captureType" "gpu_trace")
        captureFrame = $captureFrame
        frames = $frames
        fixedDeltaSeconds = Get-OptionalDouble $Performance "fixedDeltaSeconds" (1.0 / 60.0) 0.1 "tests.performance"
        frameCount = Get-OptionalInt $Performance "frameCount" 60 1 1000000 "tests.performance"
        maxDurationMilliseconds = Get-OptionalInt $Performance "maxDurationMilliseconds" 0 0 7200000 "tests.performance"
        captureTimeoutSeconds = Get-OptionalInt $Performance "captureTimeoutSeconds" 180 1 3600 "tests.performance"
        traceTimeoutSeconds = Get-OptionalInt $Performance "traceTimeoutSeconds" 600 1 7200 "tests.performance"
        replayLoops = Get-OptionalInt $Performance "replayLoops" 1 1 100 "tests.performance"
        skipReplay = Get-Boolean $Performance "skipReplay" $false
        setGpuClocks = Get-Boolean $Performance "setGpuClocks" $false
    }
    if ($args.maxDurationMilliseconds -eq 0) { $args.Remove("maxDurationMilliseconds") }
    foreach ($field in @("nsightPath", "extraArgs", "outputRoot")) {
        if (Test-Property $Performance $field) { $args[$field] = Get-PropertyValue $Performance $field $null }
    }
    if ($args.captureType -notin @("gpu_trace", "graphics_capture")) { throw "tests.performance.captureType 不支持: $($args.captureType)" }
    return $args
}

function New-EvaluationContract($Data) {
    $requiredActions = New-Object 'System.Collections.Generic.List[object]'
    foreach ($item in @($Data.requiredActions)) { [void]$requiredActions.Add($item) }
    $contract = [ordered]@{
        schemaVersion = 1
        requireTargetSuccess = $true
        requiredActions = $requiredActions.ToArray()
        discovery = [ordered]@{
            required = $true
            minProjectQueries = 1
            minSceneQueries = 1
            minAssetQueries = [int]$Data.requiredAssetQueries
        }
    }
    if ($Data.captureEnabled -or $Data.screenshotEnabled) {
        $contract.visual = [ordered]@{ required = $true; minScreenshots = 1 }
    }
    if ($Data.performanceEnabled) {
        $performance = [ordered]@{ required = $true }
        $thresholds = Get-PropertyValue $Data.performance "thresholds" $null
        if ($null -ne $thresholds) {
            foreach ($property in $thresholds.PSObject.Properties) { $performance[$property.Name] = $property.Value }
        }
        $contract.performance = $performance
    }
    return $contract
}

function Invoke-ChildProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$LogDir,
        [int]$TimeoutSeconds = 7200
    )
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
    $stdoutPath = Join-Path $LogDir "stdout.log"
    $stderrPath = Join-Path $LogDir "stderr.log"
    $stdout = ""
    $stderr = ""
    $exitCode = $null
    $timedOut = $false
    $errorText = ""
    $started = Get-Date
    $process = $null
    try {
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $FilePath
        $startInfo.WorkingDirectory = $WorkingDirectory
        $startInfo.Arguments = (($ArgumentList | ForEach-Object { '"' + ([string]$_).Replace('"', '\"') + '"' }) -join ' ')
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
    } catch { $errorText = $_.Exception.Message }
    [System.IO.File]::WriteAllText($stdoutPath, $stdout, $utf8NoBom)
    [System.IO.File]::WriteAllText($stderrPath, $stderr, $utf8NoBom)
    return [pscustomobject][ordered]@{
        success = ($null -ne $exitCode -and $exitCode -eq 0 -and -not $timedOut -and -not $errorText)
        exitCode = $exitCode
        timedOut = $timedOut
        durationMs = [int]((New-TimeSpan -Start $started -End (Get-Date)).TotalMilliseconds)
        stdoutPath = $stdoutPath
        stderrPath = $stderrPath
        stdout = $stdout
        stderr = $stderr
        error = $errorText
    }
}

function Get-MarkerPath([string]$Output, [string]$Marker) {
    $lines = @($Output -split "\r?\n" | Where-Object { $_.Trim().StartsWith($Marker) } | Select-Object -Last 1)
    if ($lines.Count -eq 0) { return "" }
    return $lines[0].Trim().Substring($Marker.Length).Trim()
}

function Invoke-Plan([string]$PlanPath, [string]$PlanOutputRoot, [string]$PlanMode) {
    $powershellPath = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    if (-not (Test-Path -LiteralPath $powershellPath -PathType Leaf)) { $powershellPath = "powershell.exe" }
    $planScript = Join-Path $PSScriptRoot "agent_plan.ps1"
    $process = Invoke-ChildProcess $powershellPath @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $planScript,
        "-PlanPath", $PlanPath, "-OutputRoot", $PlanOutputRoot, "-Mode", $PlanMode
    ) $root (Join-Path $script:runDir "plan_process") 7200
    $marker = Get-MarkerPath ($process.stdout + "`n" + $process.stderr) "PLAN_RESULT_PATH="
    $structured = $null
    if ($marker -and (Test-Path -LiteralPath $marker -PathType Leaf)) {
        try { $structured = Read-JsonFile $marker } catch { $process.error = $_.Exception.Message }
    }
    $process | Add-Member -MemberType NoteProperty -Name resultPath -Value $marker
    $process | Add-Member -MemberType NoteProperty -Name result -Value $structured
    $process.success = ($process.success -and $null -ne $structured -and [bool](Get-PropertyValue $structured "success" $false))
    return $process
}

function Copy-DeliveryFile([string]$SourcePath, [string]$DestinationRelative, [string]$Kind = "delivery") {
    if ([string]::IsNullOrWhiteSpace($SourcePath)) { return $null }
    try { $source = Resolve-ProjectPath $SourcePath $true } catch { return $null }
    $destinationRelative = $DestinationRelative.Replace('\', '/').TrimStart('/')
    if ($destinationRelative -match '(^|/)\.\.(/|$)' -or [System.IO.Path]::IsPathRooted($destinationRelative)) { throw "交付目标路径不安全: $DestinationRelative" }
    $destination = Join-Path (Join-Path $script:runDir "delivery") $destinationRelative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
    $item = Add-Artifact $destination $Kind
    return [pscustomobject][ordered]@{ source = Get-RelativePath $source; path = Get-RelativePath $destination; size = [int64](Get-Item -LiteralPath $destination).Length; sha256 = Get-Sha256 $destination; kind = $Kind }
}

function Write-DeliveryManifest([string]$DeliveryDir) {
    $manifestPath = Join-Path $DeliveryDir "delivery.manifest.json"
    $files = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $DeliveryDir -File -Recurse | Where-Object { $_.FullName -ne $manifestPath })) {
        $files += [ordered]@{ path = $file.FullName.Substring($DeliveryDir.Length).TrimStart('\', '/').Replace('\', '/'); size = [int64]$file.Length; sha256 = Get-Sha256 $file.FullName }
    }
    $manifest = [ordered]@{ schemaVersion = 1; tool = "agent_game_spec"; createdAt = (Get-Date).ToString("o"); files = $files }
    Write-JsonFile $manifestPath $manifest
    foreach ($entry in @($manifest.files)) {
        $full = Join-Path $DeliveryDir ([string]$entry.path)
        if ((Get-Sha256 $full) -ne [string]$entry.sha256) { throw "交付 manifest 校验失败: $($entry.path)" }
    }
    return $manifestPath
}

function New-Delivery($Spec, $PlanProcess, [string]$PlanPath, [string]$TaskPath, [string]$WorkflowPath, [string]$ContractPath, [string]$GeneratedScenePath, [object[]]$ScriptSpecs) {
    $deliverySpec = Get-PropertyValue $Spec "delivery" $null
    $enabled = Get-Boolean $deliverySpec "enabled" $true
    if (-not $enabled) { return [ordered]@{ status = "disabled"; enabled = $false; files = @() } }
    $deliveryDir = Join-Path $script:runDir "delivery"
    New-Item -ItemType Directory -Path $deliveryDir -Force | Out-Null
    $files = New-Object 'System.Collections.Generic.List[object]'
    foreach ($item in @(
        (Copy-DeliveryFile (Join-Path $script:runDir "gamespec.input.json") "spec/gamespec.input.json" "spec"),
        (Copy-DeliveryFile $PlanPath "spec/plan.generated.json" "spec"),
        (Copy-DeliveryFile $TaskPath "spec/task.generated.json" "spec"),
        (Copy-DeliveryFile $WorkflowPath "spec/workflow.generated.json" "spec"),
        (Copy-DeliveryFile $ContractPath "spec/evaluation.generated.json" "spec"),
        (Copy-DeliveryFile (Get-PropertyValue $PlanProcess "resultPath" "") "reports/plan.result.json" "report")
    )) { if ($null -ne $item) { [void]$files.Add($item) } }

    $planResult = Get-PropertyValue $PlanProcess "result" $null
    if ($null -ne $planResult) {
        foreach ($property in @("taskResultPath", "evidenceResultPath", "evaluationResultPath")) {
            $path = [string](Get-PropertyValue $planResult $property "")
            if ($path) {
                $destination = switch ($property) {
                    "taskResultPath" { "reports/task.result.json" }
                    "evidenceResultPath" { "reports/evidence.json" }
                    "evaluationResultPath" { "reports/evaluation.result.json" }
                }
                $item = Copy-DeliveryFile $path $destination "report"
                if ($null -ne $item) { [void]$files.Add($item) }
            }
        }
        $taskResultPath = [string](Get-PropertyValue $planResult "taskResultPath" "")
        if ($taskResultPath) {
            try {
                $taskResult = Read-JsonFile $taskResultPath
                $attempts = @((Get-PropertyValue $taskResult "attempts" @()))
                $workflowResultPath = ""
                foreach ($attempt in $attempts) {
                    if ([string](Get-PropertyValue $attempt "status" "") -in @("passed", "planned")) {
                        $candidate = [string](Get-PropertyValue $attempt "workflowResultPath" "")
                        if ($candidate) { $workflowResultPath = $candidate }
                    }
                }
                if ($workflowResultPath) {
                    $item = Copy-DeliveryFile $workflowResultPath "reports/workflow.result.json" "report"
                    if ($null -ne $item) { [void]$files.Add($item) }
                }
            } catch {}
        }
    }

    if ($GeneratedScenePath) {
        $item = Copy-DeliveryFile $GeneratedScenePath "scene/scene.generated.json" "scene"
        if ($null -ne $item) { [void]$files.Add($item) }
    }
    $scriptIndex = 0
    foreach ($scriptSpec in @($ScriptSpecs)) {
        $scriptPath = [string](Get-PropertyValue $scriptSpec "outputPath" "")
        if ($scriptPath) {
            $destination = "scripts/{0:D2}-{1}" -f $scriptIndex, ([System.IO.Path]::GetFileName($scriptPath))
            $item = Copy-DeliveryFile $scriptPath $destination "script"
            if ($null -ne $item) { [void]$files.Add($item) }
        }
        $scriptIndex++
    }

    $testsSpec = Get-PropertyValue $Spec "tests" $null
    $gameplaySpec = Get-PropertyValue $testsSpec "gameplay" $null
    $replayPath = [string](Get-PropertyValue $gameplaySpec "replayPath" "")
    if ($replayPath) {
        $item = Copy-DeliveryFile $replayPath "tests/input-replay.json" "test_input_replay"
        if ($null -ne $item) { [void]$files.Add($item) }
    }

    if (Get-Boolean $deliverySpec "includeBuildArtifacts" $false) {
        $buildPaths = @(
            "out/build/x64-Release/EngineMain.exe",
            "out/build/x64-Release/MikanTestRunner.exe",
            "out/build/x64-Release/Game.dll",
            "out/build/x64-Release/SDL3.dll",
            "out/build/x64-Release/SDL3_image.dll",
            "out/build/x64-Release/assimp-vc143-mtd.dll",
            "out/build/x64-Release/ktx.dll"
        )
        $deliveryGame = [string](Get-PropertyValue (Get-PropertyValue $Spec "project" $null) "game" "")
        if ($deliveryGame -match '^[A-Za-z][A-Za-z0-9_]{0,63}$') {
            $buildPaths += "out/build/x64-Release/Game$deliveryGame.dll"
        }
        foreach ($buildPath in $buildPaths) {
            $item = Copy-DeliveryFile $buildPath ("build/" + [System.IO.Path]::GetFileName($buildPath)) "build"
            if ($null -ne $item) { [void]$files.Add($item) }
        }
    }

    $deliveryManifest = Write-DeliveryManifest $deliveryDir
    [void](Add-Artifact $deliveryManifest "delivery_manifest")
    $zipPath = $null
    if (Get-Boolean $deliverySpec "packageZip" $true) {
        $zipPath = Join-Path $script:runDir "delivery.zip"
        if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
        Compress-Archive -Path (Join-Path $deliveryDir "*") -DestinationPath $zipPath -CompressionLevel Optimal
        [void](Add-Artifact $zipPath "delivery_package")
    }
    $zipRelativePath = $null
    if ($zipPath) { $zipRelativePath = Get-RelativePath $zipPath }
    return [ordered]@{
        status = "ready"
        enabled = $true
        directory = Get-RelativePath $deliveryDir
        manifestPath = Get-RelativePath $deliveryManifest
        zipPath = $zipRelativePath
        fileCount = $files.Count
        files = $files.ToArray()
    }
}

function Write-RunManifest {
    $manifestPath = Join-Path $script:runDir "manifest.json"
    $entries = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse | Where-Object { $_.Name -notin @("manifest.json", "result.json") })) {
        $entries += [ordered]@{ path = Get-RelativePath $file.FullName; size = [int64]$file.Length; sha256 = Get-Sha256 $file.FullName }
    }
    $manifest = [ordered]@{ schemaVersion = 1; operation = "agent_game_spec"; runId = [string]$script:runId; createdAt = (Get-Date).ToString("o"); files = $entries }
    Write-JsonFile $manifestPath $manifest
    foreach ($entry in @($manifest.files)) {
        $full = Resolve-ProjectPath ([string]$entry.path) $true
        if ((Get-Sha256 $full) -ne [string]$entry.sha256) { throw "GameSpec manifest 校验失败: $($entry.path)" }
    }
    return $manifestPath
}

function Save-Result([bool]$Success, [string]$ErrorText = "", [string]$NextAction = "", $PlanProcess = $null, $Delivery = $null, $Generated = $null, $AssetPreflight = $null) {
    if ($null -eq $script:resultPath) { return }
    $planResult = if ($null -ne $PlanProcess) { Get-PropertyValue $PlanProcess "result" $null } else { $null }
    $planSummary = $null
    if ($null -ne $PlanProcess) {
        $planSummary = [ordered]@{
            success = [bool](Get-PropertyValue $PlanProcess "success" $false)
            exitCode = Get-PropertyValue $PlanProcess "exitCode" $null
            resultPath = [string](Get-PropertyValue $PlanProcess "resultPath" "")
            result = $planResult
        }
    }
    $result = [ordered]@{
        schemaVersion = 1
        tool = "agent_game_spec"
        apiVersion = 1
        success = $Success
        mode = $script:mode
        name = $script:specName
        goal = [string](Get-PropertyValue $script:spec "goal" "")
        runId = $script:runId
        runDir = Get-RelativePath $script:runDir
        specPath = Get-RelativePath $script:specPath
        generated = $Generated
        assetPreflight = $AssetPreflight
        plan = $planSummary
        delivery = $Delivery
        artifacts = $script:artifacts.ToArray()
        error = $ErrorText
        nextAction = $NextAction
    }
    Write-JsonFile $script:resultPath $result
}

$fatalError = ""
$planProcess = $null
$delivery = $null
$generated = $null
$assetPreflight = [ordered]@{ requiredPaths = @(); missingPaths = @(); success = $true }
try {
    $script:specPath = Resolve-ProjectPath $GameSpecPath $true
    $script:spec = Read-JsonFile $script:specPath
    if ($null -eq $script:spec -or [int](Get-PropertyValue $script:spec "schemaVersion" 0) -ne 1) { throw "GameSpec schemaVersion 必须为 1" }
    $script:specName = [string](Get-PropertyValue $script:spec "name" "")
    Assert-Name $script:specName "name"
    $goal = [string](Get-PropertyValue $script:spec "goal" "")
    if ([string]::IsNullOrWhiteSpace($goal) -or $goal.Length -gt 4000) { throw "goal 必须存在且不超过 4000 字符" }
    $allowDestructive = Get-Boolean $script:spec "allowDestructive" $false
    $script:mode = if ($Mode) { $Mode.ToLowerInvariant() } else { [string](Get-PropertyValue $script:spec "mode" "preview").ToLowerInvariant() }
    if ($script:mode -notin @("preview", "execute")) { throw "mode 只支持 preview 或 execute" }
    if (-not $RunId) { $RunId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8)) }
    if ($RunId -notmatch '^[A-Za-z0-9_.-]{1,80}$') { throw "RunId 只能包含字母、数字、下划线、点和短横线" }
    $script:runId = $RunId
    $outputRootFull = Resolve-ProjectPath $OutputRoot $false
    $script:runDir = Join-Path $outputRootFull $RunId
    if (Test-Path -LiteralPath $script:runDir) { throw "GameSpec run 目录已存在，为避免覆盖请换 RunId: $RunId" }
    New-Item -ItemType Directory -Path $script:runDir -Force | Out-Null
    $script:resultPath = Join-Path $script:runDir "result.json"
    [void](Add-Artifact $script:specPath "gamespec_input")
    [System.IO.File]::Copy($script:specPath, (Join-Path $script:runDir "gamespec.input.json"), $true)
    [void](Add-Artifact (Join-Path $script:runDir "gamespec.input.json") "gamespec_input_copy")

    $project = Get-PropertyValue $script:spec "project" $null
    if ($null -eq $project) { throw "project 必须存在" }
    $sceneValue = [string](Get-PropertyValue $project "scenePath" "")
    $projectPathValue = [string](Get-PropertyValue $project "projectPath" "")
    $projectFull = if ($projectPathValue) { Resolve-ProjectDirectory $projectPathValue $true } else { "" }
    $sourceSceneFull = Resolve-GameSpecScenePath $sceneValue $projectFull
    $sourceScene = Get-RelativePath $sourceSceneFull
    $projectRelative = if ($projectFull) { Get-RelativePath $projectFull } else { "" }
    $game = [string](Get-PropertyValue $project "game" "")
    $assets = Get-PropertyValue $script:spec "assets" $null
    $requiredPaths = @((Get-PropertyValue $assets "requiredPaths" @()))
    $missingPaths = New-Object 'System.Collections.Generic.List[string]'
    $requiredPathEntries = New-Object 'System.Collections.Generic.List[object]'
    foreach ($pathValue in $requiredPaths) {
        $relative = [string]$pathValue
        $full = Resolve-ProjectPath $relative $false
        $exists = Test-Path -LiteralPath $full -PathType Leaf
        $size = 0
        $hash = ""
        if ($exists) {
            $size = [int64](Get-Item -LiteralPath $full).Length
            $hash = Get-Sha256 $full
        }
        $entry = [ordered]@{ path = $relative.Replace('\', '/'); exists = $exists; size = $size; sha256 = $hash }
        [void]$requiredPathEntries.Add([pscustomobject]$entry)
        if (-not $exists) { [void]$missingPaths.Add($relative) }
    }
    $assetPreflight = [ordered]@{ requiredPaths = $requiredPathEntries.ToArray(); missingPaths = $missingPaths.ToArray(); success = ($missingPaths.Count -eq 0) }
    if ($missingPaths.Count -gt 0) { throw "GameSpec requiredPaths 缺失: $($missingPaths -join ', ')" }

    $sceneSpec = Get-PropertyValue $script:spec "scene" $null
    $sceneCommandsPath = ""
    $generatedScenePath = $sourceScene
    if ($null -ne $sceneSpec) {
        $hasCommands = Test-Property $sceneSpec "commands"
        $commandsPathValue = [string](Get-PropertyValue $sceneSpec "commandsPath" "")
        if ($hasCommands -and $commandsPathValue) { throw "scene.commands 与 scene.commandsPath 只能二选一" }
        if (-not $hasCommands -and -not $commandsPathValue) { throw "scene 必须提供 commands 或 commandsPath" }
        if ($hasCommands) {
            $commandsPath = Join-Path $script:runDir "scene.commands.json"
            Write-JsonFile $commandsPath ([ordered]@{ schemaVersion = 1; commands = @((Get-PropertyValue $sceneSpec "commands" @())) })
            $sceneCommandsPath = Get-RelativePath $commandsPath
        } else {
            $commandsFull = Resolve-ProjectPath $commandsPathValue $true
            $sceneCommandsPath = Get-RelativePath $commandsFull
        }
        $requestedOutput = [string](Get-PropertyValue $sceneSpec "outputPath" "")
        if ($requestedOutput) {
            $requestedFull = Resolve-ProjectPath $requestedOutput $false
            if (-not (Test-PathUnder $requestedFull "out\agent_game_specs") -or $requestedFull.Equals($sourceSceneFull, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "scene.outputPath 必须位于 out/agent_game_specs 且不能覆盖源场景"
            }
            $generatedScenePath = Get-RelativePath $requestedFull
        } else {
            $generatedScenePath = Get-RelativePath (Join-Path $script:runDir "generated\scene.generated.json")
        }
    }

    $scriptSpecs = @((Get-PropertyValue $script:spec "scripts" @()))
    foreach ($scriptSpec in $scriptSpecs) {
        $scriptName = [string](Get-PropertyValue $scriptSpec "scriptName" "")
        Assert-Name $scriptName "scripts.scriptName" 64
        $outputPath = [string](Get-PropertyValue $scriptSpec "outputPath" "")
        if ($outputPath -notmatch '^(games|projects/[^/]+/games)/.+\.cpp$') { throw "脚本 outputPath 必须位于 games 或 projects/<project>/games 下，且为 .cpp: $outputPath" }
        if (Test-Property $scriptSpec "source" -and Test-Property $scriptSpec "sourcePath") { throw "脚本 $scriptName 的 source 与 sourcePath 只能二选一" }
        if (Test-Property $scriptSpec "sourcePath") { [void](Resolve-ProjectPath ([string](Get-PropertyValue $scriptSpec "sourcePath" "")) $true) }
        if (Test-Property $scriptSpec "source") {
            $source = [string](Get-PropertyValue $scriptSpec "source" "")
            if ($source.Length -gt 200000) { throw "脚本 $scriptName source 超过 200000 字符" }
        }
        if (Get-Boolean $scriptSpec "overwrite" $false -and -not $allowDestructive) { throw "脚本 $scriptName overwrite=true 需要 GameSpec.allowDestructive=true" }
    }

    $tests = Get-PropertyValue $script:spec "tests" $null
    if ($null -eq $tests) { throw "tests 必须存在；GameSpec 必须显式声明验收范围" }
    Assert-KnownProperties $tests @("gameplay", "assertions", "render", "capture", "performance") "tests"
    $gameplay = Get-PropertyValue $tests "gameplay" $null
    Assert-KnownProperties $gameplay @("enabled", "frames", "fixedDeltaSeconds", "timeoutMs", "replayPath", "extraArgs") "tests.gameplay"
    $gameplayEnabled = Get-Boolean $gameplay "enabled" $true
    $assertions = @((Get-PropertyValue $tests "assertions" @()))
    if ($assertions.Count -gt 0 -and -not $gameplayEnabled) { throw "tests.assertions 需要启用 tests.gameplay" }
    $render = Get-PropertyValue $tests "render" $null
    Assert-KnownProperties $render @("enabled", "frames", "fixedDeltaSeconds", "timeoutMs", "screenshotFrame", "extraArgs") "tests.render"
    $renderEnabled = Get-Boolean $render "enabled" $false
    $renderScreenshotFrame = if ($renderEnabled) { Get-OptionalInt $render "screenshotFrame" 0 0 1000000 "tests.render" } else { 0 }
    if ($renderScreenshotFrame -gt 0 -and $renderScreenshotFrame -gt (Get-OptionalInt $render "frames" 60 1 1000000 "tests.render")) {
        throw "tests.render.screenshotFrame 必须不大于 frames"
    }
    $capture = Get-PropertyValue $tests "capture" $null
    $captureEnabled = Get-Boolean $capture "enabled" $false
    $performance = Get-PropertyValue $tests "performance" $null
    $performanceEnabled = Get-Boolean $performance "enabled" $false
    $deliverySpec = Get-PropertyValue $script:spec "delivery" $null

    $contextDependencies = New-Object 'System.Collections.Generic.List[string]'
    if ($projectRelative) {
        Add-Step "get_project_context" "get_project_context" @() ([ordered]@{ projectPath = $projectRelative; maxResults = 200 }) 600 | Out-Null
        [void]$contextDependencies.Add("get_project_context")
    }
    $projectInspectArgs = [ordered]@{ maxResults = 200 }
    if ($projectRelative) { $projectInspectArgs.projectPath = $projectRelative }
    $projectStep = Add-Step "inspect_project" "inspect_project" @($contextDependencies.ToArray()) $projectInspectArgs
    $sceneInspectArgs = [ordered]@{ scene = $sourceScene; maxEntities = 500; maxAssetReferences = 200 }
    if ($projectRelative) { $sceneInspectArgs.projectPath = $projectRelative }
    $sceneStep = Add-Step "inspect_scene" "inspect_scene" @("inspect_project") $sceneInspectArgs
    $requiredAssetQueries = 0
    $queryIndex = 0
    foreach ($querySpec in @((Get-PropertyValue $assets "queries" @()))) {
        $query = [string](Get-PropertyValue $querySpec "query" "")
        if ($query.Length -gt 256) { throw "assets.queries[$queryIndex].query 过长" }
        $assetType = [string](Get-PropertyValue $querySpec "assetType" "all")
        Assert-AssetType $assetType "assets.queries[$queryIndex]"
        $required = Get-Boolean $querySpec "required" $false
        if ($required) { $requiredAssetQueries++ }
        $queryArgs = [ordered]@{ query = $query; assetType = $assetType; maxResults = Get-OptionalInt $querySpec "maxResults" 200 1 2000 "assets.queries[$queryIndex]" }
        if ($projectRelative) { $queryArgs.projectPath = $projectRelative }
        Add-Step ("query_assets_{0:D2}" -f $queryIndex) "query_assets" @("inspect_project") $queryArgs 600 (-not $required) | Out-Null
        $queryIndex++
    }

    $activeScene = $sourceScene
    $sceneReadyStep = "inspect_scene"
    if ($null -ne $sceneSpec) {
        $sceneArgs = [ordered]@{
            scene = $sourceScene
            commandsPath = $sceneCommandsPath
            outputPath = $generatedScenePath
            checkAssets = Get-Boolean $sceneSpec "checkAssets" $true
        }
        if ($projectRelative) { $sceneArgs.projectPath = $projectRelative }
        Add-Step "apply_scene" "apply_scene_commands" @("inspect_scene") $sceneArgs 1200 | Out-Null
        $sceneReadyStep = "apply_scene"
        $activeScene = $generatedScenePath
    }
    $validateArgs = [ordered]@{ scenePath = $activeScene; checkAssets = $true }
    if ($projectRelative) { $validateArgs.projectPath = $projectRelative }
    Add-Step "validate_scene" "validate_scene" @($sceneReadyStep) $validateArgs 600 | Out-Null
    $lastBuildDependency = "validate_scene"

    $scriptIndex = 0
    foreach ($scriptSpec in $scriptSpecs) {
        $scriptArgs = [ordered]@{
            scriptName = [string](Get-PropertyValue $scriptSpec "scriptName" "")
            outputPath = [string](Get-PropertyValue $scriptSpec "outputPath" "")
            compile = $false
        }
        foreach ($field in @("className", "fields", "source", "sourcePath")) {
            if (Test-Property $scriptSpec $field) { $scriptArgs[$field] = Get-PropertyValue $scriptSpec $field $null }
        }
        if (Get-Boolean $scriptSpec "overwrite" $false) { $scriptArgs.overwrite = $true }
        $scriptStepId = "create_script_{0:D2}" -f $scriptIndex
        Add-Step $scriptStepId "create_script" @($lastBuildDependency) $scriptArgs 1200 | Out-Null
        $lastBuildDependency = $scriptStepId
        $scriptIndex++
    }
    if ($scriptSpecs.Count -gt 0) {
        Add-Step "compile_games" "compile_games" @($lastBuildDependency) $null 1800 | Out-Null
        $lastBuildDependency = "compile_games"
    }

    $requiredActions = New-Object 'System.Collections.Generic.List[object]'
    if ($projectRelative) { [void]$requiredActions.Add([ordered]@{ action = "get_project_context"; status = "passed"; minCount = 1 }) }
    [void]$requiredActions.Add([ordered]@{ action = "inspect_project"; status = "passed"; minCount = 1 })
    [void]$requiredActions.Add([ordered]@{ action = "inspect_scene"; status = "passed"; minCount = 1 })
    [void]$requiredActions.Add([ordered]@{ action = "validate_scene"; status = "passed"; minCount = 1 })
    if ($null -ne $sceneSpec) { [void]$requiredActions.Add([ordered]@{ action = "apply_scene_commands"; status = "passed"; minCount = 1 }) }
    if ($scriptSpecs.Count -gt 0) {
        [void]$requiredActions.Add([ordered]@{ action = "create_script"; status = "passed"; minCount = $scriptSpecs.Count })
        [void]$requiredActions.Add([ordered]@{ action = "compile_games"; status = "passed"; minCount = 1 })
    }
    if ($gameplayEnabled) {
        $gameplayArgs = Convert-CommonTestArgs $gameplay "tests.gameplay"
        $gameplayArgs.scene = $activeScene
        if ($projectRelative) { $gameplayArgs.projectPath = $projectRelative }
        $gameplayArgs.game = $game
        Add-Step "build_runner" "build" @($lastBuildDependency) ([ordered]@{ target = "MikanTestRunner"; configureIfMissing = $true }) 1800 | Out-Null
        Add-Step "gameplay" "run_gameplay_test" @("build_runner") $gameplayArgs 1800 | Out-Null
        Add-Step "read_gameplay_state" "read_dump" @("gameplay") ([ordered]@{ path = '${steps.gameplay.result.dumpPath}' }) 600 | Out-Null
        [void]$requiredActions.Add([ordered]@{ action = "run_gameplay_test"; status = "passed"; minCount = 1 })
        if ($assertions.Count -gt 0) {
            Add-Step "assert_gameplay_state" "assert_state" @("read_gameplay_state") ([ordered]@{ path = '${steps.gameplay.result.dumpPath}'; assertions = $assertions }) 600 | Out-Null
            [void]$requiredActions.Add([ordered]@{ action = "assert_state"; status = "passed"; minCount = 1 })
        }
    }

    $engineBuildStep = ""
    if ($renderEnabled -or $captureEnabled -or $performanceEnabled -or [bool](Get-PropertyValue $project "engineBuild" $false)) {
        Add-Step "build_engine" "build" @($lastBuildDependency) ([ordered]@{ target = "EngineMain"; configureIfMissing = $true; killEngine = $true }) 2400 | Out-Null
        $engineBuildStep = "build_engine"
    }
    if ($renderEnabled) {
        $renderArgs = Convert-CommonTestArgs $render "tests.render" 60 120000
        $renderArgs.scene = $activeScene
        if ($projectRelative) { $renderArgs.projectPath = $projectRelative }
        $renderArgs.game = $game
        Add-Step "render" "run_render_test" @($engineBuildStep) $renderArgs 1800 | Out-Null
        [void]$requiredActions.Add([ordered]@{ action = "run_render_test"; status = "passed"; minCount = 1 })
    }
    if ($captureEnabled) {
        $captureArgs = Convert-CaptureArgs $capture
        $captureArgs.scene = $activeScene
        if ($projectRelative) { $captureArgs.projectPath = $projectRelative }
        $captureArgs.game = $game
        Add-Step "capture_frame" "capture_frame" @($engineBuildStep) $captureArgs 2400 | Out-Null
        [void]$requiredActions.Add([ordered]@{ action = "capture_frame"; status = "passed"; minCount = 1 })
    }
    if ($performanceEnabled) {
        $performanceArgs = Convert-PerformanceArgs $performance
        $performanceArgs.scene = $activeScene
        if ($projectRelative) { $performanceArgs.projectPath = $projectRelative }
        $performanceArgs.game = $game
        Add-Step "capture_performance" "capture_performance" @($engineBuildStep) $performanceArgs 3600 | Out-Null
        [void]$requiredActions.Add([ordered]@{ action = "capture_performance"; status = "passed"; minCount = 1 })
    }

    $workflow = [ordered]@{
        schemaVersion = 1
        name = "gamespec-$($script:specName)"
        description = [string](Get-PropertyValue $script:spec "description" $goal)
        allowDestructive = $allowDestructive
        defaults = [ordered]@{ timeoutSeconds = 600 }
        steps = $script:steps.ToArray()
    }
    $workflowPath = Join-Path $script:runDir "workflow.generated.json"
    Write-JsonFile $workflowPath $workflow
    $task = [ordered]@{
        schemaVersion = 1
        name = "gamespec-task-$($script:specName)"
        description = "由 GameSpec 编译的受控任务；候选修复必须通过 agent_task 白名单。"
        workflowPath = Get-RelativePath $workflowPath
        mode = $script:mode
        maxAttempts = 3
        timeoutSeconds = 7200
        candidates = @()
    }
    $taskPath = Join-Path $script:runDir "task.generated.json"
    Write-JsonFile $taskPath $task
    $contractData = [pscustomobject][ordered]@{
        requiredActions = $requiredActions.ToArray()
        requiredAssetQueries = $requiredAssetQueries
        captureEnabled = $captureEnabled
        screenshotEnabled = ($renderEnabled -and $renderScreenshotFrame -gt 0)
        performanceEnabled = $performanceEnabled
        performance = $performance
    }
    $contract = New-EvaluationContract $contractData
    $contractPath = Join-Path $script:runDir "evaluation.generated.json"
    Write-JsonFile $contractPath $contract
    $plan = [ordered]@{
        schemaVersion = 1
        name = "gamespec-plan-$($script:specName)"
        goal = $goal
        taskPath = Get-RelativePath $taskPath
        evaluationContractPath = Get-RelativePath $contractPath
        mode = $script:mode
    }
    $planPath = Join-Path $script:runDir "plan.generated.json"
    Write-JsonFile $planPath $plan
    foreach ($path in @($workflowPath, $taskPath, $contractPath, $planPath)) { [void](Add-Artifact $path "generated_spec") }

    $generated = [ordered]@{
        workflowPath = Get-RelativePath $workflowPath
        taskPath = Get-RelativePath $taskPath
        evaluationContractPath = Get-RelativePath $contractPath
        planPath = Get-RelativePath $planPath
        scenePath = $activeScene
        projectPath = $projectRelative
        stepCount = $script:steps.Count
        scriptCount = $scriptSpecs.Count
    }

    # agent_plan 会继续嵌套 task/evidence/workflow 运行目录。使用短的项目内根目录，
    # 避免 Windows PowerShell 5.1 在长路径下无法创建 step input 文件；结果仍会
    # 通过 plan result 和 delivery manifest 关联回本次 GameSpec run。
    $planOutputRoot = "out/g"
    $planProcess = Invoke-Plan (Get-RelativePath $planPath) $planOutputRoot $script:mode
    foreach ($path in @($planProcess.stdoutPath, $planProcess.stderrPath, $planProcess.resultPath)) { [void](Add-Artifact $path "plan_process") }
    $planSuccess = [bool](Get-PropertyValue $planProcess "success" $false)
    if ($planSuccess -or $script:mode -eq "preview") {
        $deliveryScenePath = ""
        if ($null -ne $sceneSpec -and $script:mode -eq "execute") { $deliveryScenePath = $activeScene }
        $delivery = New-Delivery $script:spec $planProcess (Get-RelativePath $planPath) (Get-RelativePath $taskPath) (Get-RelativePath $workflowPath) (Get-RelativePath $contractPath) $deliveryScenePath $scriptSpecs
    }
    $finalSuccess = $planSuccess
    $nextAction = if ($finalSuccess -and $script:mode -eq "preview") { "检查生成的 workflow、GameSpec contract 和场景/脚本输入后，以 mode=execute 重跑" } elseif ($finalSuccess) { "GameSpec 已完成构建、测试、证据和交付打包" } else { "读取 plan.result.json/evidence.json 的失败诊断，修正 GameSpec 后重试" }
    $manifestPath = Write-RunManifest
    [void](Add-Artifact $manifestPath "gamespec_manifest")
    Save-Result $finalSuccess "" $nextAction $planProcess $delivery $generated $assetPreflight
    Write-Output "GAMESPEC_RESULT_PATH=$script:resultPath"
    Write-Output "SUCCESS=$finalSuccess"
    if ($finalSuccess) { exit 0 } else { exit 1 }
} catch {
    $fatalError = $_.Exception.Message
    if ($null -ne $script:runDir) {
        if ($null -eq $script:spec) { $script:spec = [pscustomobject]@{ goal = "" } }
        if ($null -eq $script:specName) { $script:specName = "invalid" }
        try {
            $manifestPath = Write-RunManifest
            [void](Add-Artifact $manifestPath "gamespec_manifest")
        } catch {}
        Save-Result $false $fatalError "修复 GameSpec 输入或执行环境后重试" $planProcess $delivery $generated $assetPreflight
        Write-Output "GAMESPEC_RESULT_PATH=$script:resultPath"
    }
    Write-Output "ERROR=$fatalError"
    Write-Output "SUCCESS=False"
    exit 3
}
