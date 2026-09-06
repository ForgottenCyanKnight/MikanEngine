# agent_evaluate.ps1 - MikanEngine AI Native 交付门禁
# ------------------------------------------------------------------
# 只读读取 agent_evidence 的结构化结果，按 contract 逐项判定行为、视觉、
# discovery 和桌面性能门槛。它不修改源码/场景，不启动引擎，也不把缺失证据
# 当作通过。
#
# 输出：out\agent_evaluations\<run-id>\request.json / result.json / manifest.json
# 退出码：0=所有门禁通过，1=门禁失败，3=输入或环境错误。
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EvidencePath,

    [string]$ContractPath = "",

    [string]$ContractJsonBase64 = "",

    [string]$OutputRoot = "out\agent_evaluations",

    [string]$RunId = ""
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$script:runDir = $null
$script:resultPath = $null

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
    $json = $Value | ConvertTo-Json -Depth 60
    [System.IO.File]::WriteAllText($Path, $json, $utf8NoBom)
}

function Get-Sha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "" }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        return ([System.BitConverter]::ToString($sha.ComputeHash($stream)).Replace("-", "")).ToLowerInvariant()
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $sha.Dispose()
    }
}

function Get-RelativePath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $prefix = $root.TrimEnd('\', '/') + '\'
    if ($full.Equals($root, [System.StringComparison]::OrdinalIgnoreCase)) { return "" }
    if ($full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) { return $full.Substring($prefix.Length).Replace('\', '/') }
    return $full.Replace('\', '/')
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

function Read-JsonFile([string]$Path) {
    $full = Resolve-ProjectPath $Path $true
    return [System.IO.File]::ReadAllText($full, $utf8NoBom) | ConvertFrom-Json
}

function Get-ArtifactEntry([string]$Path, [string]$Kind) {
    $full = Resolve-ProjectPath $Path $true
    return [ordered]@{
        path = Get-RelativePath $full
        kind = $Kind
        size = [int64](Get-Item -LiteralPath $full).Length
        sha256 = Get-Sha256 $full
    }
}

function Add-Check([System.Collections.Generic.List[object]]$Checks, [string]$Id, [bool]$Passed, $Expected, $Actual, [string]$Message) {
    [void]$Checks.Add([ordered]@{
        id = $Id
        status = if ($Passed) { "passed" } else { "failed" }
        expected = $Expected
        actual = $Actual
        message = $Message
    })
}

function Get-WorkflowSteps($Evidence) {
    $steps = New-Object 'System.Collections.Generic.List[object]'
    foreach ($observation in @((Get-PropertyValue $Evidence "observations" @()))) {
        foreach ($step in @((Get-PropertyValue $observation "steps" @()))) { [void]$steps.Add($step) }
    }
    return $steps.ToArray()
}

function Get-LatestPerformanceCapture($Evidence) {
    $captures = @((Get-PropertyValue (Get-PropertyValue $Evidence "performance" $null) "captures" @()) | Where-Object { [bool](Get-PropertyValue $_ "success" $false) })
    if ($captures.Count -eq 0) { return $null }
    return $captures[$captures.Count - 1]
}

function Evaluate-Evidence($Evidence, $Contract) {
    $checks = New-Object 'System.Collections.Generic.List[object]'
    $targetSuccess = [bool](Get-PropertyValue (Get-PropertyValue $Evidence "source" $null) "targetSuccess" $false)
    if ([bool](Get-PropertyValue $Contract "requireTargetSuccess" $true)) {
        Add-Check $checks "target_success" $targetSuccess $true $targetSuccess "source.targetSuccess 必须为 true"
    }

    $steps = @(Get-WorkflowSteps $Evidence)
    foreach ($requirement in @((Get-PropertyValue $Contract "requiredActions" @()))) {
        $action = [string](Get-PropertyValue $requirement "action" "")
        if (-not $action) { throw "requiredActions.action 不能为空" }
        $minimum = [int](Get-PropertyValue $requirement "minCount" 1)
        $wantedStatus = [string](Get-PropertyValue $requirement "status" "passed")
        if ($minimum -lt 1) { throw "requiredActions.minCount 必须大于 0" }
        $actualCount = @($steps | Where-Object { [string](Get-PropertyValue $_ "action" "") -eq $action -and [string](Get-PropertyValue $_ "status" "") -eq $wantedStatus }).Count
        Add-Check $checks ("action_" + $action) ($actualCount -ge $minimum) ([ordered]@{ action = $action; status = $wantedStatus; minCount = $minimum }) $actualCount ("要求至少 {0} 个 status={1} 的 {2} step" -f $minimum, $wantedStatus, $action)
    }

    $discoveryContract = Get-PropertyValue $Contract "discovery" $null
    $discoveryRequired = [bool](Get-PropertyValue $discoveryContract "required" $false)
    $minProjectQueries = [int](Get-PropertyValue $discoveryContract "minProjectQueries" 0)
    $minSceneQueries = [int](Get-PropertyValue $discoveryContract "minSceneQueries" 0)
    $minAssetQueries = [int](Get-PropertyValue $discoveryContract "minAssetQueries" 0)
    if ($discoveryRequired -or $minProjectQueries -gt 0 -or $minSceneQueries -gt 0 -or $minAssetQueries -gt 0) {
        $discovery = Get-PropertyValue $Evidence "discovery" $null
        $discoveryAvailable = [bool](Get-PropertyValue $discovery "available" $false)
        Add-Check $checks "discovery_available" ($discoveryAvailable) $true $discoveryAvailable "Discovery context 必须存在且来自成功查询"
        Add-Check $checks "discovery_project_queries" ([int](Get-PropertyValue $discovery "projectQueryCount" 0) -ge $minProjectQueries) $minProjectQueries ([int](Get-PropertyValue $discovery "projectQueryCount" 0)) "项目能力查询次数未达到门槛"
        Add-Check $checks "discovery_scene_queries" ([int](Get-PropertyValue $discovery "sceneQueryCount" 0) -ge $minSceneQueries) $minSceneQueries ([int](Get-PropertyValue $discovery "sceneQueryCount" 0)) "场景查询次数未达到门槛"
        Add-Check $checks "discovery_asset_queries" ([int](Get-PropertyValue $discovery "assetQueryCount" 0) -ge $minAssetQueries) $minAssetQueries ([int](Get-PropertyValue $discovery "assetQueryCount" 0)) "资产查询次数未达到门槛"
    }

    $visualContract = Get-PropertyValue $Contract "visual" $null
    $minRenderDoc = [int](Get-PropertyValue $visualContract "minRenderDocCaptures" 0)
    $minScreenshots = [int](Get-PropertyValue $visualContract "minScreenshots" 0)
    $minMrt = [int](Get-PropertyValue $visualContract "minMrt" 0)
    $visualRequired = [bool](Get-PropertyValue $visualContract "required" $false)
    if ($visualRequired -or $minRenderDoc -gt 0 -or $minScreenshots -gt 0 -or $minMrt -gt 0) {
        $visual = Get-PropertyValue $Evidence "visual" $null
        $available = [bool](Get-PropertyValue $visual "available" $false)
        Add-Check $checks "visual_available" $available $true $available "视觉证据必须存在"
        $visualUsable = [bool](Get-PropertyValue $visual "visualUsable" $available)
        Add-Check $checks "visual_usable" $visualUsable $true $visualUsable "截图不能处于未就绪或黑屏状态"
        Add-Check $checks "visual_renderdoc_captures" ([int](Get-PropertyValue $visual "renderdocCaptureCount" 0) -ge $minRenderDoc) $minRenderDoc ([int](Get-PropertyValue $visual "renderdocCaptureCount" 0)) "RenderDoc 捕获数量未达到门槛"
        $usableScreenshotCount = [int](Get-PropertyValue $visual "usableScreenshotCount" (Get-PropertyValue $visual "screenshotCount" 0))
        Add-Check $checks "visual_screenshots" ($usableScreenshotCount -ge $minScreenshots) $minScreenshots $usableScreenshotCount "可用截图数量未达到门槛"
        Add-Check $checks "visual_mrt" ([int](Get-PropertyValue $visual "mrtCount" 0) -ge $minMrt) $minMrt ([int](Get-PropertyValue $visual "mrtCount" 0)) "MRT 数量未达到门槛"
    }

    $performanceContract = Get-PropertyValue $Contract "performance" $null
    $performanceRequired = [bool](Get-PropertyValue $performanceContract "required" $false)
    $performance = Get-PropertyValue $Evidence "performance" $null
    $performanceAvailable = [bool](Get-PropertyValue $performance "available" $false)
    $hasPerformanceThreshold = $false
    foreach ($field in @("maxGpuFrameTimeMs", "maxDrawCount", "maxDispatchCount", "maxGraphicsEngineActivePct", "maxSmThroughputPct", "maxL1texThroughputPct", "maxDramThroughputPct", "maxPcieThroughputPct")) {
        if (Test-Property $performanceContract $field) { $hasPerformanceThreshold = $true; break }
    }
    $requireBaseline = [bool](Get-PropertyValue $performanceContract "requireBaselineReady" $false)
    if ($performanceRequired -or $hasPerformanceThreshold -or $requireBaseline) {
        Add-Check $checks "performance_available" $performanceAvailable $true $performanceAvailable "性能证据必须可用；permission_denied/failed 不能作为通过"
        $capture = Get-LatestPerformanceCapture $Evidence
        $metrics = if ($null -ne $capture) { Get-PropertyValue (Get-PropertyValue $capture "performance" $null) "metrics" $null } else { $null }
        foreach ($mapping in @(
            @("maxGpuFrameTimeMs", "gpuFrameTimeMs"),
            @("maxDrawCount", "drawCount"),
            @("maxDispatchCount", "dispatchCount"),
            @("maxGraphicsEngineActivePct", "graphicsEngineActivePct"),
            @("maxSmThroughputPct", "smThroughputPct"),
            @("maxL1texThroughputPct", "l1texThroughputPct"),
            @("maxDramThroughputPct", "dramThroughputPct"),
            @("maxPcieThroughputPct", "pcieThroughputPct")
        )) {
            $contractField = [string]$mapping[0]
            $metricField = [string]$mapping[1]
            if (-not (Test-Property $performanceContract $contractField)) { continue }
            $limit = [double](Get-PropertyValue $performanceContract $contractField 0)
            $actual = Get-PropertyValue $metrics $metricField $null
            $passed = ($null -ne $actual -and [double]$actual -le $limit)
            Add-Check $checks ("performance_" + $metricField) $passed $limit $actual ("最新成功性能采集的 {0} 必须不大于阈值" -f $metricField)
        }
        if ($requireBaseline) {
            $baselineReady = if ($null -ne $capture) { [bool](Get-PropertyValue (Get-PropertyValue $capture "performance" $null) "baselineReady" $false) } else { $false }
            Add-Check $checks "performance_baseline_ready" $baselineReady $true $baselineReady "contract 要求 baselineReady，但 profiling/replay 证据默认不等于无采集开销基线"
        }
    }

    $failed = @($checks | Where-Object { $_.status -eq "failed" })
    return [ordered]@{
        success = ($failed.Count -eq 0)
        checkCount = $checks.Count
        passedCount = @($checks | Where-Object { $_.status -eq "passed" }).Count
        failedCount = $failed.Count
        checks = $checks.ToArray()
        nextAction = if ($failed.Count -eq 0) { "evaluation_passed；可以把本次 evidence 作为交付候选" } else { "根据 checks 中 failed 项补充 discovery、修复实现或重新测试，不得修改门槛来绕过失败" }
    }
}

try {
    $evidenceFull = Resolve-ProjectPath $EvidencePath $true
    $evidence = [System.IO.File]::ReadAllText($evidenceFull, $utf8NoBom) | ConvertFrom-Json
    if ([string](Get-PropertyValue $evidence "tool" "") -ne "agent_evidence") { throw "EvidencePath 必须指向 tool=agent_evidence 的 evidence.json" }
    if ($ContractPath -and $ContractJsonBase64) { throw "ContractPath 与 ContractJsonBase64 只能二选一" }
    $contract = $null
    $contractFull = $null
    if ($ContractPath) {
        $contractFull = Resolve-ProjectPath $ContractPath $true
        $contract = [System.IO.File]::ReadAllText($contractFull, $utf8NoBom) | ConvertFrom-Json
    } elseif ($ContractJsonBase64) {
        $contractJson = [System.Text.Encoding]::UTF8.GetString([System.Convert]::FromBase64String($ContractJsonBase64))
        $contract = $contractJson | ConvertFrom-Json
    } else {
        $contract = [pscustomobject]@{ schemaVersion = 1; requireTargetSuccess = $true }
    }
    if ([int](Get-PropertyValue $contract "schemaVersion" 0) -ne 1) { throw "evaluation contract schemaVersion 必须为 1" }

    $outputRootFull = Resolve-ProjectPath $OutputRoot $false
    if (-not $RunId) { $RunId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8)) }
    if ($RunId -notmatch '^[A-Za-z0-9_.-]{1,80}$') { throw "RunId 只能包含字母、数字、下划线、点和短横线" }
    $script:runDir = Join-Path $outputRootFull $RunId
    if (Test-Path -LiteralPath $script:runDir) { throw "evaluation run 目录已存在，为避免覆盖请换 RunId: $RunId" }
    New-Item -ItemType Directory -Path $script:runDir -Force | Out-Null
    $script:resultPath = Join-Path $script:runDir "result.json"
    $requestPath = Join-Path $script:runDir "request.json"
    $manifestPath = Join-Path $script:runDir "manifest.json"
    $request = [ordered]@{
        schemaVersion = 1
        tool = "agent_evaluate"
        apiVersion = 1
        evidencePath = Get-RelativePath $evidenceFull
        contractPath = if ($contractFull) { Get-RelativePath $contractFull } else { "" }
        contract = $contract
        outputRoot = Get-RelativePath $outputRootFull
        runId = $RunId
    }
    Write-JsonFile $requestPath $request
    $evaluation = Evaluate-Evidence $evidence $contract
    $result = [ordered]@{
        schemaVersion = 1
        tool = "agent_evaluate"
        apiVersion = 1
        success = [bool]$evaluation.success
        generatedAt = (Get-Date).ToString("o")
        runId = $RunId
        runDir = Get-RelativePath $script:runDir
        evidence = [ordered]@{ path = Get-RelativePath $evidenceFull; sha256 = Get-Sha256 $evidenceFull }
        contract = $contract
        evaluation = $evaluation
        artifacts = @([pscustomobject](Get-ArtifactEntry (Get-RelativePath $evidenceFull) "evidence_input"), [pscustomobject](Get-ArtifactEntry (Get-RelativePath $requestPath) "evaluation_request"))
        manifestPath = ""
        nextAction = [string]$evaluation.nextAction
    }
    if ($contractFull) { $result.artifacts += [pscustomobject](Get-ArtifactEntry (Get-RelativePath $contractFull) "evaluation_contract") }
    Write-JsonFile $script:resultPath $result
    $manifestFiles = @([pscustomobject](Get-ArtifactEntry (Get-RelativePath $evidenceFull) "evidence_input"), [pscustomobject](Get-ArtifactEntry (Get-RelativePath $requestPath) "evaluation_request"))
    if ($contractFull) { $manifestFiles += [pscustomobject](Get-ArtifactEntry (Get-RelativePath $contractFull) "evaluation_contract") }
    $manifest = [ordered]@{ schemaVersion = 1; tool = "agent_evaluate"; apiVersion = 1; generatedAt = (Get-Date).ToString("o"); runDir = Get-RelativePath $script:runDir; files = @($manifestFiles); note = "manifest 不包含 result.json 和自身，避免哈希循环。" }
    Write-JsonFile $manifestPath $manifest
    foreach ($entry in @($manifest.files)) {
        $full = Resolve-ProjectPath ([string]$entry.path) $true
        if ((Get-Sha256 $full) -ne [string]$entry.sha256) { throw "evaluation manifest 校验失败: $($entry.path)" }
    }
    $result.manifestPath = Get-RelativePath $manifestPath
    Write-JsonFile $script:resultPath $result
    Write-Output "EVALUATION_RESULT_PATH=$script:resultPath"
    Write-Output ("SUCCESS=" + [bool]$result.success)
    if ($result.success) { exit 0 } else { exit 1 }
} catch {
    $fatalError = $_.Exception.Message
    if ($null -ne $script:runDir) {
        if ($null -eq $script:resultPath) { $script:resultPath = Join-Path $script:runDir "result.json" }
        Write-JsonFile $script:resultPath ([ordered]@{ schemaVersion = 1; tool = "agent_evaluate"; apiVersion = 1; success = $false; generatedAt = (Get-Date).ToString("o"); error = $fatalError; nextAction = "修复 evaluation 输入或 contract 后重试" })
        Write-Output "EVALUATION_RESULT_PATH=$script:resultPath"
    }
    Write-Output "ERROR=$fatalError"
    Write-Output "SUCCESS=False"
    exit 3
}
