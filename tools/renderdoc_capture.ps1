# renderdoc_capture.ps1 - MikanEngine 桌面端 RenderDoc 自动抓帧
# ------------------------------------------------------------------
# 由 renderdoccmd 注入 RenderDoc，再由 EngineMain 的
# --renderdoc-capture-frame 在指定 Present 前调用 TriggerCapture。
# 本工具不安装 RenderDoc、不修改系统环境，只负责启动受控引擎进程、
# 等待 .rdc 落盘、生成缩略图并写出可供 Agent Evidence 消费的结果。
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$TargetPath,

    [string[]]$TargetArguments = @(),

    # Use this when the script is launched by another PowerShell process.
    # Passing --frames/--scene as separate native-process arguments makes
    # Windows PowerShell treat them as this script's named parameters.
    [string]$TargetArgumentsJson = "",

    [string]$TargetArgumentsBase64 = "",

    [string]$WorkingDirectory = "",

    [int]$CaptureFrame = 1,

    [string]$OutputRoot = "out\renderdoc_captures",

    [string]$RenderDocCmdPath = "",

    [string]$RunId = "",

    [int]$TimeoutSeconds = 180,

    [int]$CaptureWaitSeconds = 10,

    [switch]$Preview,

    [switch]$ApiValidation,

    [switch]$CaptureCallstacks,

    [switch]$SkipThumbnail
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
    try {
        $decodedArguments = $TargetArgumentsJson | ConvertFrom-Json
    } catch {
        throw "TargetArgumentsJson 不是合法 JSON: $($_.Exception.Message)"
    }
    if ($null -eq $decodedArguments) {
        $TargetArguments = @()
    } elseif ($decodedArguments -is [System.Array]) {
        $TargetArguments = @($decodedArguments | ForEach-Object { [string]$_ })
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
    if ($full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($prefix.Length).Replace('\', '/')
    }
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

function Resolve-RenderDocCmd {
    $candidates = New-Object 'System.Collections.Generic.List[string]'
    if (-not [string]::IsNullOrWhiteSpace($RenderDocCmdPath)) {
        [void]$candidates.Add($RenderDocCmdPath)
    }
    if (-not [string]::IsNullOrWhiteSpace($env:RENDERDOC_CMD_PATH)) {
        [void]$candidates.Add($env:RENDERDOC_CMD_PATH)
    }
    if (-not [string]::IsNullOrWhiteSpace($env:RENDERDOC_PATH)) {
        [void]$candidates.Add($env:RENDERDOC_PATH)
        [void]$candidates.Add((Join-Path $env:RENDERDOC_PATH 'renderdoccmd.exe'))
        [void]$candidates.Add((Join-Path $env:RENDERDOC_PATH 'qrenderdoc.exe'))
    }
    $command = Get-Command renderdoccmd.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) { [void]$candidates.Add($command.Source) }
    foreach ($programRoot in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:LOCALAPPDATA)) {
        if ([string]::IsNullOrWhiteSpace($programRoot)) { continue }
        [void]$candidates.Add((Join-Path $programRoot 'RenderDoc\renderdoccmd.exe'))
        [void]$candidates.Add((Join-Path $programRoot 'RenderDoc\qrenderdoc.exe'))
        [void]$candidates.Add((Join-Path $programRoot 'Programs\RenderDoc\renderdoccmd.exe'))
        [void]$candidates.Add((Join-Path $programRoot 'Programs\RenderDoc\qrenderdoc.exe'))
    }
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace([string]$candidate)) { continue }
        $candidateFull = [System.IO.Path]::GetFullPath([string]$candidate)
        if (Test-Path -LiteralPath $candidateFull -PathType Leaf) {
            if ([System.IO.Path]::GetFileName($candidateFull).Equals('qrenderdoc.exe', [System.StringComparison]::OrdinalIgnoreCase)) {
                $sibling = Join-Path (Split-Path -Parent $candidateFull) 'renderdoccmd.exe'
                if (Test-Path -LiteralPath $sibling -PathType Leaf) { return $sibling }
            }
            if ([System.IO.Path]::GetFileName($candidateFull).Equals('renderdoccmd.exe', [System.StringComparison]::OrdinalIgnoreCase)) {
                return $candidateFull
            }
        }
        if (Test-Path -LiteralPath $candidateFull -PathType Container) {
            $nested = Join-Path $candidateFull 'renderdoccmd.exe'
            if (Test-Path -LiteralPath $nested -PathType Leaf) { return $nested }
        }
    }
    return $null
}

function Add-Artifact([string]$Path, [string]$Kind) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = Get-RelativePath $item.FullName
        kind = $Kind
        exists = $true
        size = [int64]$item.Length
        sha256 = Get-Sha256 $item.FullName
    }
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
        operation = 'renderdoc_capture'
        createdAt = (Get-Date).ToString('o')
        files = $entries.ToArray()
    }
    Write-JsonFile $script:manifestPath $manifest
    $null = [System.IO.File]::ReadAllText($script:manifestPath, $utf8NoBom)
    foreach ($entry in @($manifest.files)) {
        $full = Resolve-ProjectPath ([string]$entry.path) $true
        if ((Get-Sha256 $full) -ne [string]$entry.sha256) {
            throw "RenderDoc capture manifest 校验失败: $($entry.path)"
        }
    }
    return $manifest
}

function Get-CaptureArtifacts {
    $artifacts = New-Object 'System.Collections.Generic.List[object]'
    foreach ($item in @(Get-ChildItem -LiteralPath $script:runDir -File -Recurse)) {
        if ($item.Name -eq 'result.json' -or $item.Name -eq 'manifest.json') { continue }
        $kind = 'metadata'
        if ($item.Extension -ieq '.rdc') { $kind = 'renderdoc_capture' }
        elseif ($item.Extension -match '(?i)^\.(png|jpg|jpeg|bmp)$') { $kind = 'renderdoc_thumbnail' }
        elseif ($item.Extension -match '(?i)^\.(log|txt)$') { $kind = 'log' }
        [void]$artifacts.Add((Add-Artifact $item.FullName $kind))
    }
    return $artifacts.ToArray()
}

function Get-FrameFiles {
    return @(Get-ChildItem -LiteralPath $script:runDir -File -Filter '*.rdc' -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc)
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

$fatalError = ""
$runResult = $null
try {
    $targetFull = Resolve-ProjectPath $TargetPath $true
    $targetWorkingDirectory = if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        Split-Path -Parent $targetFull
    } else {
        Resolve-ProjectPath $WorkingDirectory $false
    }
    if (-not (Test-Path -LiteralPath $targetWorkingDirectory -PathType Container)) {
        throw "工作目录不存在: $targetWorkingDirectory"
    }
    if ($CaptureFrame -lt 1 -or $CaptureFrame -gt 1000000) { throw "CaptureFrame 必须在 1..1000000" }
    if ($TimeoutSeconds -lt 1 -or $TimeoutSeconds -gt 3600) { throw "TimeoutSeconds 必须在 1..3600" }
    if ($CaptureWaitSeconds -lt 0 -or $CaptureWaitSeconds -gt 120) { throw "CaptureWaitSeconds 必须在 0..120" }

    foreach ($argument in @($TargetArguments)) {
        $text = [string]$argument
        if ($text -match '^--renderdoc-capture-frame(?:=|$)' -or
            $text -match '^--renderdoc-capture-path(?:=|$)' -or
            $text -match '^--crash-log(?:=|$)') {
            throw "TargetArguments 不能覆盖 RenderDoc 脚本保留参数: $text"
        }
        if ($text -eq '--headless-no-render') { throw "RenderDoc 抓帧不能使用 --headless-no-render" }
    }
    $frameLimit = Get-FrameLimit @($TargetArguments)
    if ($frameLimit -gt 0 -and $CaptureFrame -gt $frameLimit) {
        throw "CaptureFrame($CaptureFrame) 大于目标 --frames($frameLimit)，无法触发 Present"
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

    $captureTemplate = Join-Path $script:runDir 'mikan_frame'
    $targetArgumentList = New-Object 'System.Collections.Generic.List[string]'
    foreach ($argument in @($TargetArguments)) { [void]$targetArgumentList.Add([string]$argument) }
    [void]$targetArgumentList.Add('--renderdoc-capture-frame')
    [void]$targetArgumentList.Add([string]$CaptureFrame)
    [void]$targetArgumentList.Add('--renderdoc-capture-path')
    [void]$targetArgumentList.Add($captureTemplate)
    [void]$targetArgumentList.Add('--crash-log')
    [void]$targetArgumentList.Add((Join-Path $script:runDir 'crash_log.txt'))

    $renderDocCmd = Resolve-RenderDocCmd
    $renderDocVersion = $null
    if ($renderDocCmd) {
        $renderDocVersion = Invoke-CapturedProcess -FilePath $renderDocCmd -ArgumentList @('version') -ProcessWorkingDirectory $root -ProcessTimeoutSeconds 15
        Write-TextFile (Join-Path $script:runDir 'renderdoc_version.stdout.log') ([string]$renderDocVersion.stdout)
        Write-TextFile (Join-Path $script:runDir 'renderdoc_version.stderr.log') ([string]$renderDocVersion.stderr)
    }

    $renderDocArguments = New-Object 'System.Collections.Generic.List[string]'
    [void]$renderDocArguments.Add('capture')
    [void]$renderDocArguments.Add('--wait-for-exit')
    [void]$renderDocArguments.Add('--capture-file')
    [void]$renderDocArguments.Add($captureTemplate)
    [void]$renderDocArguments.Add('--working-dir')
    [void]$renderDocArguments.Add($targetWorkingDirectory)
    [void]$renderDocArguments.Add('--opt-disallow-vsync')
    [void]$renderDocArguments.Add('--opt-disallow-fullscreen')
    if ($ApiValidation) { [void]$renderDocArguments.Add('--opt-api-validation') }
    if ($CaptureCallstacks) { [void]$renderDocArguments.Add('--opt-capture-callstacks') }
    [void]$renderDocArguments.Add($targetFull)
    foreach ($argument in $targetArgumentList.ToArray()) { [void]$renderDocArguments.Add($argument) }

    $commandDocument = [ordered]@{
        schemaVersion = 1
        tool = 'renderdoc_capture'
        mode = if ($Preview) { 'preview' } else { 'execute' }
        renderdocCmdPath = $renderDocCmd
        renderdocArguments = $renderDocArguments.ToArray()
        targetPath = Get-RelativePath $targetFull
        workingDirectory = Get-RelativePath $targetWorkingDirectory
        targetArguments = $targetArgumentList.ToArray()
        captureFrame = $CaptureFrame
        captureTemplate = Get-RelativePath $captureTemplate
    }
    Write-JsonFile (Join-Path $script:runDir 'command.json') $commandDocument

    if ($Preview) {
        $runResult = [ordered]@{
            schemaVersion = 1
            tool = 'renderdoc_capture'
            apiVersion = 1
            success = $true
            mode = 'preview'
            status = 'preview'
            runId = $RunId
            runDir = Get-RelativePath $script:runDir
            target = [ordered]@{ path = Get-RelativePath $targetFull; workingDirectory = Get-RelativePath $targetWorkingDirectory; arguments = $targetArgumentList.ToArray() }
            renderdoc = [ordered]@{ available = ($null -ne $renderDocCmd); cmdPath = if ($renderDocCmd) { Get-RelativePath $renderDocCmd } else { $null }; version = if ($renderDocVersion) { ([string]$renderDocVersion.stdout).Trim() } else { '' } }
            capture = [ordered]@{ requestedFrame = $CaptureFrame; template = Get-RelativePath $captureTemplate; files = @(); thumbnail = $null }
            process = $null
            nextAction = if ($renderDocCmd) { 'preview 通过；将 mode=execute 运行实际抓帧' } else { '安装 RenderDoc 或设置 RenderDocCmdPath/RENDERDOC_CMD_PATH 后再 execute' }
        }
    } elseif ($null -eq $renderDocCmd) {
        $runResult = [ordered]@{
            schemaVersion = 1
            tool = 'renderdoc_capture'
            apiVersion = 1
            success = $false
            mode = 'execute'
            status = 'unavailable'
            runId = $RunId
            runDir = Get-RelativePath $script:runDir
            target = [ordered]@{ path = Get-RelativePath $targetFull; workingDirectory = Get-RelativePath $targetWorkingDirectory; arguments = $targetArgumentList.ToArray() }
            renderdoc = [ordered]@{ available = $false; cmdPath = $null; version = '' }
            capture = [ordered]@{ requestedFrame = $CaptureFrame; template = Get-RelativePath $captureTemplate; files = @(); thumbnail = $null }
            process = $null
            nextAction = '安装 64 位 RenderDoc，并将 renderdoccmd.exe 加入 PATH，或传入 RenderDocCmdPath'
        }
    } else {
        $launchResult = Invoke-CapturedProcess -FilePath $renderDocCmd -ArgumentList $renderDocArguments.ToArray() -ProcessWorkingDirectory $root -ProcessTimeoutSeconds $TimeoutSeconds
        Write-TextFile (Join-Path $script:runDir 'stdout.log') ([string]$launchResult.stdout)
        Write-TextFile (Join-Path $script:runDir 'stderr.log') ([string]$launchResult.stderr)

        $captureFiles = @()
        $deadline = (Get-Date).AddSeconds($CaptureWaitSeconds)
        do {
            $captureFiles = Get-FrameFiles
            if ($captureFiles.Count -gt 0 -or (Get-Date) -ge $deadline) { break }
            Start-Sleep -Milliseconds 200
        } while ($true)

        $thumbnailPath = $null
        if (-not $SkipThumbnail -and $captureFiles.Count -gt 0) {
            $capture = $captureFiles[0]
            $thumbnailPath = Join-Path $script:runDir (([System.IO.Path]::GetFileNameWithoutExtension($capture.Name)) + '.png')
            $thumbnailResult = Invoke-CapturedProcess -FilePath $renderDocCmd -ArgumentList @('thumb', '--out', $thumbnailPath, $capture.FullName) -ProcessWorkingDirectory $root -ProcessTimeoutSeconds 60
            Write-TextFile (Join-Path $script:runDir 'thumbnail.stdout.log') ([string]$thumbnailResult.stdout)
            Write-TextFile (Join-Path $script:runDir 'thumbnail.stderr.log') ([string]$thumbnailResult.stderr)
            if (-not (Test-Path -LiteralPath $thumbnailPath -PathType Leaf)) { $thumbnailPath = $null }
        }

        $processFailed = $launchResult.timedOut -or $null -eq $launchResult.exitCode -or $launchResult.exitCode -ne 0 -or -not [string]::IsNullOrWhiteSpace([string]$launchResult.error)
        $captured = $captureFiles.Count -gt 0
        $status = if ($launchResult.timedOut) { 'target_timeout' } elseif ($processFailed) { 'target_failed' } elseif ($captured) { 'captured' } else { 'capture_failed' }
        $nextAction = switch ($status) {
            'captured' { '已生成 .rdc；可用 qrenderdoc 打开，或交给 agent_evidence 继续分析'; break }
            'target_timeout' { '检查目标是否需要窗口交互、是否在 capture frame 前卡住；读取 stdout/stderr'; break }
            'target_failed' { '读取 RenderDoc 与目标 stdout/stderr，先修复目标启动/渲染错误'; break }
            default { '确认引擎已重新构建且 RenderDoc 注入成功；检查日志中的 [RenderDoc] ready/triggered' }
        }
        $captureEntries = @($captureFiles | ForEach-Object { Add-Artifact $_.FullName 'renderdoc_capture' })
        $runResult = [ordered]@{
            schemaVersion = 1
            tool = 'renderdoc_capture'
            apiVersion = 1
            success = (-not $processFailed -and $captured)
            mode = 'execute'
            status = $status
            runId = $RunId
            runDir = Get-RelativePath $script:runDir
            target = [ordered]@{ path = Get-RelativePath $targetFull; workingDirectory = Get-RelativePath $targetWorkingDirectory; arguments = $targetArgumentList.ToArray() }
            renderdoc = [ordered]@{ available = $true; cmdPath = Get-RelativePath $renderDocCmd; version = if ($renderDocVersion) { ([string]$renderDocVersion.stdout).Trim() } else { '' } }
            capture = [ordered]@{ requestedFrame = $CaptureFrame; template = Get-RelativePath $captureTemplate; files = $captureEntries; thumbnail = if ($thumbnailPath) { Add-Artifact $thumbnailPath 'renderdoc_thumbnail' } else { $null } }
            process = [ordered]@{ exitCode = $launchResult.exitCode; timedOut = [bool]$launchResult.timedOut; durationMs = $launchResult.durationMs; stdoutPath = Get-RelativePath (Join-Path $script:runDir 'stdout.log'); stderrPath = Get-RelativePath (Join-Path $script:runDir 'stderr.log') }
            nextAction = $nextAction
        }
    }

    Write-JsonFile $script:resultPath $runResult
    $manifest = Write-RunManifest
    $runResult.manifestPath = Get-RelativePath $script:manifestPath
    $runResult.artifacts = @((Get-CaptureArtifacts))
    $runResult.artifacts += @(Add-Artifact $script:manifestPath 'manifest')
    Write-JsonFile $script:resultPath $runResult
    Write-Output "RENDERDOC_RESULT_PATH=$script:resultPath"
    Write-Output "SUCCESS=$([bool]$runResult.success)"
    if ($runResult.mode -eq 'preview' -or $runResult.success) { exit 0 }
    if ($runResult.status -eq 'unavailable') { exit 3 }
    if ($runResult.status -eq 'target_timeout') { exit 2 }
    exit 1
} catch {
    $fatalError = $_.Exception.Message
    if ($null -ne $script:runDir) {
        $errorResult = [ordered]@{
            schemaVersion = 1
            tool = 'renderdoc_capture'
            apiVersion = 1
            success = $false
            mode = if ($Preview) { 'preview' } else { 'execute' }
            status = 'error'
            runId = $RunId
            runDir = Get-RelativePath $script:runDir
            error = $fatalError
            nextAction = '修复 RenderDoc 抓帧参数或环境后重试'
        }
        Write-JsonFile $script:resultPath $errorResult
        try {
            $null = Write-RunManifest
            $errorResult.manifestPath = Get-RelativePath $script:manifestPath
            $errorResult.artifacts = @((Get-CaptureArtifacts))
            $errorResult.artifacts += @(Add-Artifact $script:manifestPath 'manifest')
            Write-JsonFile $script:resultPath $errorResult
        } catch {}
        Write-Output "RENDERDOC_RESULT_PATH=$script:resultPath"
    }
    Write-Output "ERROR=$fatalError"
    Write-Output "SUCCESS=False"
    exit 3
}
