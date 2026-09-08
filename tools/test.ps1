# test.ps1 - MikanEngine 统一回归测试入口
# ------------------------------------------------------------------
# 目标：
#   1) 用同一份 test_cases.json 描述测试场景，避免手工拼接命令；
#   2) 分离场景校验、CPU-only 玩法测试和 Vulkan headless 渲染测试；
#   3) 每次运行写入独立目录，保存日志、dump、环境和 result.json；
#   4) 输出机器可读的结果，为后续 MCP/Agent 编排提供稳定边界。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\test.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\test.ps1 -Layer gameplay
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\test.ps1 -Case contact2d,cesiumwalk
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\test.ps1 -SkipBuild
#
# 退出码：
#   0 = 所有选中步骤通过
#   1 = 至少一个步骤失败
#   3 = 测试环境或测试清单错误
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [ValidateSet("all", "validate", "gameplay", "render")]
    [string]$Layer = "all",
    [string[]]$Case = @(),
    [string]$ManifestPath = "",
    [string]$OutputRoot = "",
    [string]$RunId = "",
    [switch]$SkipBuild,
    [switch]$KillEngine,
    [switch]$CleanFirst,
    [switch]$ConfigureIfMissing,
    [switch]$CheckAssets,
    [int]$BuildTimeoutSeconds = 600,
    [int]$TestTimeoutSeconds = 90,
    [switch]$StopOnFailure
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$buildDir = Join-Path $root "out\build\x64-Release"
$buildScript = Join-Path $PSScriptRoot "build.ps1"
$validateScript = Join-Path $PSScriptRoot "validate_scene.ps1"
$compileGamesScript = Join-Path $PSScriptRoot "compile_games.ps1"
$defaultManifest = Join-Path $PSScriptRoot "test_cases.json"
$powershellCommand = Get-Command powershell.exe -ErrorAction SilentlyContinue
if ($null -eq $powershellCommand) {
    Write-Host "ERROR: 找不到 powershell.exe"
    exit 3
}
$powershellExe = $powershellCommand.Source

function Get-RootedPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "路径不能为空" }
    $candidate = if ([System.IO.Path]::IsPathRooted($Path)) { $Path } else { Join-Path $root $Path }
    return [System.IO.Path]::GetFullPath($candidate)
}

function Get-SafeName([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return "unnamed" }
    return [System.Text.RegularExpressions.Regex]::Replace($Value, "[^A-Za-z0-9_.-]+", "_")
}

function Get-RelativeArtifactPath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $prefix = $root.TrimEnd("\") + "\"
    if ($full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($prefix.Length).Replace("\", "/")
    }
    return $full.Replace("\", "/")
}

function Get-ObjectProperty($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-CaseValue($CaseObject, [string]$Name, $Default) {
    $value = Get-ObjectProperty $CaseObject $Name
    if ($null -eq $value) { return $Default }
    return $value
}

function Get-CaseProjectPath($CaseObject) {
    $caseName = [string](Get-CaseValue $CaseObject "name" "unnamed")
    $projectValue = [string](Get-CaseValue $CaseObject "projectPath" "")
    if ([string]::IsNullOrWhiteSpace($projectValue)) {
        throw "测试用例 $caseName 必须声明 projectPath"
    }
    $projectPath = Get-RootedPath $projectValue
    if (-not (Test-Path -LiteralPath (Join-Path $projectPath "project.json") -PathType Leaf)) {
        throw "测试用例 $caseName 的项目清单不存在: $projectPath\project.json"
    }
    return $projectPath
}

function Test-CaseLayer($CaseObject, [string]$WantedLayer) {
    foreach ($value in @(Get-CaseValue $CaseObject "layers" @())) {
        if ([string]$value -eq $WantedLayer) { return $true }
    }
    return $false
}

function Write-JsonFile([string]$Path, $Value) {
    $json = $Value | ConvertTo-Json -Depth 12
    [System.IO.File]::WriteAllText($Path, $json, [System.Text.UTF8Encoding]::new($false))
}

function Write-RunMessage([string]$Message) {
    $line = "$(Get-Date -Format o) $Message"
    Write-Host $line
    [System.IO.File]::AppendAllText($script:runLogPath, $line + [System.Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
}

function ConvertTo-ProcessArgument([string]$Argument) {
    if ($null -eq $Argument) { return '""' }
    if ($Argument -notmatch '[\s"]') { return $Argument }
    $quote = [string][char]34
    $slashQuote = [string][char]92 + [string][char]34
    return $quote + $Argument.Replace($quote, $slashQuote) + $quote
}

function Invoke-ProcessStep {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )

    $safeName = Get-SafeName $Name
    $stdoutPath = Join-Path $runDir ($safeName + ".stdout.log")
    $stderrPath = Join-Path $runDir ($safeName + ".stderr.log")
    $stepLogPath = Join-Path $runDir ($safeName + ".log")
    $argumentString = (($ArgumentList | ForEach-Object { ConvertTo-ProcessArgument ([string]$_) }) -join " ")

    Write-RunMessage "START $Name"
    Write-RunMessage "  file=$FilePath"
    Write-RunMessage "  args=$argumentString"

    $started = Get-Date
    $exitCode = -1
    $timedOut = $false
    $launchError = ""
    $process = $null

    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        $launchError = "可执行文件不存在: $FilePath"
    } elseif (-not (Test-Path -LiteralPath $WorkingDirectory -PathType Container)) {
        $launchError = "工作目录不存在: $WorkingDirectory"
    } else {
        try {
            $startParams = @{
                FilePath = $FilePath
                WorkingDirectory = $WorkingDirectory
                RedirectStandardOutput = $stdoutPath
                RedirectStandardError = $stderrPath
                PassThru = $true
                WindowStyle = "Hidden"
            }
            if ($argumentString) { $startParams.ArgumentList = $argumentString }
            $process = Start-Process @startParams
            $finished = $process.WaitForExit($TimeoutSeconds * 1000)
            if (-not $finished) {
                $timedOut = $true
                try { $process.Kill() } catch {}
                try { $process.WaitForExit() } catch {}
                $exitCode = 124
            } else {
                $process.Refresh()
                $exitCode = [int]$process.ExitCode
            }
        } catch {
            $launchError = $_.Exception.Message
        }
    }

    $stdout = ""
    $stderr = ""
    if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) {
        try { $stdout = [System.IO.File]::ReadAllText($stdoutPath, [System.Text.Encoding]::Default) } catch {}
    }
    if (Test-Path -LiteralPath $stderrPath -PathType Leaf) {
        try { $stderr = [System.IO.File]::ReadAllText($stderrPath, [System.Text.Encoding]::Default) } catch {}
    }

    $logText = @(
        "COMMAND: $FilePath $argumentString"
        "EXIT_CODE: $exitCode"
        "TIMED_OUT: $timedOut"
        "LAUNCH_ERROR: $launchError"
        ""
        "===== STDOUT ====="
        $stdout
        ""
        "===== STDERR ====="
        $stderr
    ) -join [System.Environment]::NewLine
    [System.IO.File]::WriteAllText($stepLogPath, $logText, [System.Text.UTF8Encoding]::new($false))

    $status = if ($launchError -or $timedOut -or $exitCode -ne 0) { "failed" } else { "passed" }
    $result = [ordered]@{
        type = "process"
        name = $Name
        status = $status
        exitCode = $exitCode
        timedOut = $timedOut
        durationMs = [int]((New-TimeSpan -Start $started -End (Get-Date)).TotalMilliseconds)
        log = Get-RelativeArtifactPath $stepLogPath
        stdout = Get-RelativeArtifactPath $stdoutPath
        stderr = Get-RelativeArtifactPath $stderrPath
        error = $launchError
    }
    $script:results.Add([pscustomobject]$result) | Out-Null
    Write-RunMessage "$($status.ToUpperInvariant()) $Name exit=$exitCode durationMs=$($result.durationMs)"
    return [pscustomobject]$result
}

function Add-SkippedStep([string]$Name, [string]$Reason) {
    $result = [ordered]@{
        type = "skipped"
        name = $Name
        status = "skipped"
        reason = $Reason
    }
    $script:results.Add([pscustomobject]$result) | Out-Null
    Write-RunMessage "SKIP $Name reason=$Reason"
}

function Add-DumpCheck {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$DumpPath,
        [Parameter(Mandatory = $true)][string]$ExpectedLayer
    )

    $status = "passed"
    $error = ""
    $actualLayer = ""
    if (-not (Test-Path -LiteralPath $DumpPath -PathType Leaf)) {
        $status = "failed"
        $error = "状态 Dump 不存在: $DumpPath"
    } else {
        try {
            $raw = [System.IO.File]::ReadAllText($DumpPath, [System.Text.UTF8Encoding]::new($false))
            $state = $raw | ConvertFrom-Json
            $actualLayer = [string](Get-ObjectProperty $state "runtime_layer")
            if ($actualLayer -ne $ExpectedLayer) {
                $status = "failed"
                $error = "runtime_layer=$actualLayer，期望=$ExpectedLayer"
            }
        } catch {
            $status = "failed"
            $error = "状态 Dump JSON 解析失败: $($_.Exception.Message)"
        }
    }

    $result = [ordered]@{
        type = "dump"
        name = $Name
        status = $status
        expectedLayer = $ExpectedLayer
        actualLayer = $actualLayer
        dump = Get-RelativeArtifactPath $DumpPath
        error = $error
    }
    $script:results.Add([pscustomobject]$result) | Out-Null
    Write-RunMessage "$($status.ToUpperInvariant()) $Name $error"
    return [pscustomobject]$result
}

function Test-ShouldAbort {
    if (-not $StopOnFailure) { return $false }
    return @($script:results | Where-Object { $_.status -eq "failed" }).Count -gt 0
}

$manifest = if ($ManifestPath) { Get-RootedPath $ManifestPath } else { $defaultManifest }
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
    Write-Host "ERROR: 测试清单不存在: $manifest"
    exit 3
}

try {
    $manifestRaw = [System.IO.File]::ReadAllText($manifest, [System.Text.UTF8Encoding]::new($false))
    $spec = $manifestRaw | ConvertFrom-Json
} catch {
    Write-Host "ERROR: 测试清单解析失败: $($_.Exception.Message)"
    exit 3
}

$allCases = @($spec.cases)
if ($allCases.Count -eq 0) {
    Write-Host "ERROR: 测试清单没有 cases"
    exit 3
}

$selectedCases = if ($Case.Count -eq 0) {
    $allCases
} else {
    $selected = @()
    foreach ($wanted in $Case) {
        $found = @($allCases | Where-Object { [string]$_.name -eq [string]$wanted })
        if ($found.Count -eq 0) {
            Write-Host "ERROR: 测试用例不存在: $wanted"
            exit 3
        }
        $selected += $found
    }
    $selected
}

$caseProjectPaths = @{}
try {
    foreach ($caseObject in $selectedCases) {
        $caseName = [string](Get-CaseValue $caseObject "name" "unnamed")
        $caseProjectPaths[$caseName] = Get-CaseProjectPath $caseObject
    }
} catch {
    Write-Host "ERROR: $_"
    exit 3
}

$defaultFrames = [int](Get-CaseValue $spec.defaults "frames" 60)
$defaultFixedDt = [double](Get-CaseValue $spec.defaults "fixedDeltaSeconds" (1.0 / 60.0))
$defaultTimeout = [int](Get-CaseValue $spec.defaults "timeoutSeconds" $TestTimeoutSeconds)
$defaultCheckAssets = [bool](Get-CaseValue $spec.defaults "checkAssets" $false)
$needsGameplay = ($Layer -in @("all", "gameplay")) -and @($selectedCases | Where-Object { Test-CaseLayer $_ "gameplay" }).Count -gt 0
$needsRender = ($Layer -in @("all", "render")) -and @($selectedCases | Where-Object { Test-CaseLayer $_ "render" }).Count -gt 0

$outputRootPath = if ($OutputRoot) { Get-RootedPath $OutputRoot } else { Join-Path $root "out\test_runs" }
New-Item -ItemType Directory -Path $outputRootPath -Force | Out-Null
if (-not $RunId) {
    $RunId = (Get-Date -Format "yyyyMMdd_HHmmss") + "_" + (New-Guid).ToString("N").Substring(0, 8)
}
$runDir = Join-Path $outputRootPath (Get-SafeName $RunId)
if (Test-Path -LiteralPath $runDir) {
    Write-Host "ERROR: 运行目录已存在，为避免覆盖历史产物: $runDir"
    exit 3
}
New-Item -ItemType Directory -Path $runDir -Force | Out-Null
$script:runLogPath = Join-Path $runDir "run.log"
[System.IO.File]::WriteAllText($script:runLogPath, "", [System.Text.UTF8Encoding]::new($false))
$script:results = New-Object 'System.Collections.Generic.List[object]'
$startedAt = Get-Date

$gitHead = ""
$gitStatus = @()
try {
    $gitHead = [string]((& git rev-parse HEAD 2>$null) | Select-Object -First 1)
    $gitStatus = @(& git status --short 2>$null | ForEach-Object { [string]$_ })
} catch {}

Write-RunMessage "RUN $RunId layer=$Layer"
Write-RunMessage "manifest=$(Get-RelativeArtifactPath $manifest)"
Write-RunMessage "selectedCases=$($selectedCases.name -join ',')"

$metadata = [ordered]@{
    schemaVersion = 1
    runId = $RunId
    startedAt = $startedAt.ToString("o")
    root = $root
    buildDir = $buildDir
    manifest = Get-RelativeArtifactPath $manifest
    layer = $Layer
    selectedCases = @($selectedCases | ForEach-Object { [string]$_.name })
    gitHead = $gitHead
    gitStatus = $gitStatus
    powershell = $PSVersionTable.PSVersion.ToString()
    os = [System.Environment]::OSVersion.VersionString
}
Write-JsonFile (Join-Path $runDir "metadata.json") $metadata

$buildFailed = $false
if (-not $SkipBuild -and $Layer -ne "validate") {
    $targets = @()
    if ($needsRender) { $targets += "MikanEngine" }
    if ($needsGameplay) { $targets += "MikanTestRunner" }
    foreach ($target in $targets) {
        $buildArgs = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $buildScript,
            "-Target", $target,
            "-LogPath", (Join-Path $runDir ("build-" + $target + ".log"))
        )
        if ($KillEngine) { $buildArgs += "-KillEngine" }
        if ($CleanFirst) { $buildArgs += "-CleanFirst" }
        if ($ConfigureIfMissing) { $buildArgs += "-ConfigureIfMissing" }
        $buildResult = Invoke-ProcessStep -Name ("build_" + $target) -FilePath $powershellExe -ArgumentList $buildArgs -WorkingDirectory $root -TimeoutSeconds $BuildTimeoutSeconds
        if ($buildResult.status -eq "failed") {
            $buildFailed = $true
            if (Test-ShouldAbort) { break }
        }
    }

    if (-not $buildFailed -and ($needsGameplay -or $needsRender) -and (Test-Path -LiteralPath $compileGamesScript -PathType Leaf)) {
        $projectPaths = @($selectedCases | ForEach-Object {
            $caseName = [string](Get-CaseValue $_ "name" "unnamed")
            $caseProjectPaths[$caseName]
        } | Select-Object -Unique)
        foreach ($projectPath in $projectPaths) {
            $projectName = Split-Path -Leaf ([string]$projectPath).TrimEnd('\')
            $compileResult = Invoke-ProcessStep -Name ("compile_game_plugins_" + (Get-SafeName $projectName)) -FilePath $powershellExe -ArgumentList @(
                "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $compileGamesScript,
                "-ProjectPath", [string]$projectPath
            ) -WorkingDirectory $root -TimeoutSeconds $BuildTimeoutSeconds
            if ($compileResult.status -eq "failed") {
                $buildFailed = $true
                break
            }
        }
    }
}

if ($buildFailed) {
    foreach ($caseObject in $selectedCases) {
        Add-SkippedStep ("case_" + [string]$caseObject.name) "构建失败"
    }
} else {
    foreach ($caseObject in $selectedCases) {
        $caseName = [string](Get-CaseValue $caseObject "name" "unnamed")
        $sceneRelative = [string](Get-CaseValue $caseObject "scene" "")
        $projectPath = [string]$caseProjectPaths[$caseName]
        $scenePath = if ($sceneRelative) {
            if ([System.IO.Path]::IsPathRooted($sceneRelative)) {
                Get-RootedPath $sceneRelative
            } else {
                Get-RootedPath (Join-Path $projectPath $sceneRelative)
            }
        } else { "" }
        $game = [string](Get-CaseValue $caseObject "game" "")
        $frames = [int](Get-CaseValue $caseObject "frames" $defaultFrames)
        $fixedDt = [double](Get-CaseValue $caseObject "fixedDeltaSeconds" $defaultFixedDt)
        $timeout = [int](Get-CaseValue $caseObject "timeoutSeconds" $defaultTimeout)
        $checkAssetsForCase = [bool](Get-CaseValue $caseObject "checkAssets" $defaultCheckAssets)
        if ($CheckAssets) { $checkAssetsForCase = $true }
        $extras = @()
        $extraValue = Get-ObjectProperty $caseObject "extraArgs"
        if ($null -ne $extraValue) {
            $extras = @($extraValue | ForEach-Object { [string]$_ })
        }

        if (-not $scenePath -or -not (Test-Path -LiteralPath $scenePath -PathType Leaf)) {
            $missing = [ordered]@{
                type = "validation"
                name = "validate_" + $caseName
                status = "failed"
                error = "场景不存在: $scenePath"
            }
            $script:results.Add([pscustomobject]$missing) | Out-Null
            Write-RunMessage "FAILED validate_$caseName 场景不存在"
            if (Test-ShouldAbort) { break }
            continue
        }

        $validateArgs = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $validateScript,
            $scenePath,
            "-Schema", (Join-Path $PSScriptRoot "scene_schema.json")
        )
        $validateArgs += @("-ProjectPath", $projectPath)
        if ($checkAssetsForCase) { $validateArgs += "-CheckAssets" }
        $validateResult = Invoke-ProcessStep -Name ("validate_" + $caseName) -FilePath $powershellExe -ArgumentList $validateArgs -WorkingDirectory $root -TimeoutSeconds $timeout
        if ($validateResult.status -eq "failed") {
            if ($Layer -ne "validate") {
                if (Test-CaseLayer $caseObject "gameplay") { Add-SkippedStep ($caseName + "_gameplay") "场景校验失败" }
                if (Test-CaseLayer $caseObject "render") { Add-SkippedStep ($caseName + "_render") "场景校验失败" }
            }
            if (Test-ShouldAbort) { break }
            continue
        }

        if ($Layer -in @("all", "gameplay") -and (Test-CaseLayer $caseObject "gameplay")) {
            $dumpPath = Join-Path $runDir ((Get-SafeName $caseName) + ".gameplay.state.json")
            $crashPath = Join-Path $runDir ((Get-SafeName $caseName) + ".gameplay.crash.log")
            $testArgs = @(
                "--project", $projectPath,
                "--frames", [string]$frames,
                "--fixed-dt", $fixedDt.ToString("0.#########", [System.Globalization.CultureInfo]::InvariantCulture),
                "--dump-state", $dumpPath,
                "--crash-log", $crashPath,
                "--scene", $scenePath
            )
            if ($game) { $testArgs += @("--game", $game) }
            $testArgs += $extras
            $runResult = Invoke-ProcessStep -Name ($caseName + "_gameplay") -FilePath (Join-Path $buildDir "MikanTestRunner.exe") -ArgumentList $testArgs -WorkingDirectory $buildDir -TimeoutSeconds $timeout
            if ($runResult.status -eq "passed") {
                [void](Add-DumpCheck -Name ($caseName + "_gameplay_dump") -DumpPath $dumpPath -ExpectedLayer "gameplay-cpu")
            }
            if (Test-ShouldAbort) { break }
        }

        if ($Layer -in @("all", "render") -and (Test-CaseLayer $caseObject "render")) {
            $dumpPath = Join-Path $runDir ((Get-SafeName $caseName) + ".render.state.json")
            $crashPath = Join-Path $runDir ((Get-SafeName $caseName) + ".render.crash.log")
            $renderArgs = @(
                "--headless",
                "--project", $projectPath,
                "--frames", [string]$frames,
                "--fixed-dt", $fixedDt.ToString("0.#########", [System.Globalization.CultureInfo]::InvariantCulture),
                "--dump-state", $dumpPath,
                "--crash-log", $crashPath,
                "--scene", $scenePath
            )
            if ($game) { $renderArgs += @("--game", $game) }
            $renderArgs += $extras
            $runResult = Invoke-ProcessStep -Name ($caseName + "_render") -FilePath (Join-Path $buildDir "MikanEngine.exe") -ArgumentList $renderArgs -WorkingDirectory $buildDir -TimeoutSeconds $timeout
            if ($runResult.status -eq "passed") {
                [void](Add-DumpCheck -Name ($caseName + "_render_dump") -DumpPath $dumpPath -ExpectedLayer "render-vulkan")
            }
            if (Test-ShouldAbort) { break }
        }
    }
}

$endedAt = Get-Date
$failedCount = @($script:results | Where-Object { $_.status -eq "failed" }).Count
$passedCount = @($script:results | Where-Object { $_.status -eq "passed" }).Count
$skippedCount = @($script:results | Where-Object { $_.status -eq "skipped" }).Count
$summary = [ordered]@{
    schemaVersion = 1
    runId = $RunId
    startedAt = $startedAt.ToString("o")
    endedAt = $endedAt.ToString("o")
    durationMs = [int](($endedAt - $startedAt).TotalMilliseconds)
    layer = $Layer
    manifest = Get-RelativeArtifactPath $manifest
    success = ($failedCount -eq 0)
    counts = [ordered]@{
        passed = $passedCount
        failed = $failedCount
        skipped = $skippedCount
    }
    steps = $script:results.ToArray()
}
Write-JsonFile (Join-Path $runDir "result.json") $summary
Write-RunMessage "SUMMARY success=$($summary.success) passed=$passedCount failed=$failedCount skipped=$skippedCount"
Write-RunMessage "RESULT $(Get-RelativeArtifactPath (Join-Path $runDir 'result.json'))"

if ($failedCount -gt 0) { exit 1 }
exit 0
