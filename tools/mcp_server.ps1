# mcp_server.ps1 - MikanEngine AI 开发/测试 MCP server（stdio transport）
# stdout 仅发送 JSON-RPC；诊断信息必须写 stderr。
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\', '/')
$exeDir = Join-Path $root "out\build\x64-Release"
$exePath = Join-Path $exeDir "EngineMain.exe"
$gameplayTestPath = Join-Path $exeDir "MikanTestRunner.exe"
$runsRoot = Join-Path $exeDir "mcp_runs"
$schemaPath = Join-Path $PSScriptRoot "scene_schema.json"
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$script:lastDumpPath = $null
$script:lastRunDir = $null

try { [Console]::InputEncoding = $utf8NoBom } catch {}
try { [Console]::OutputEncoding = $utf8NoBom } catch {}

function Log([string]$Message) { [Console]::Error.WriteLine("[mcp] $Message") }
function Send-Json($Object) {
    $json = $Object | ConvertTo-Json -Depth 30 -Compress
    [Console]::Out.WriteLine($json)
    [Console]::Out.Flush()
}
function Send-Error($Id, [int]$Code, [string]$Message, $Data = $null) {
    $errorObject = @{ code = $Code; message = $Message }
    if ($null -ne $Data) { $errorObject.data = $Data }
    Send-Json @{ jsonrpc = "2.0"; id = $Id; error = $errorObject }
}
function Send-Result($Id, $Result) { Send-Json @{ jsonrpc = "2.0"; id = $Id; result = $Result } }
function Send-ToolResult($Id, [string]$Text, [bool]$IsError = $false) {
    Send-Json @{ jsonrpc = "2.0"; id = $Id; result = @{ content = @(@{ type = "text"; text = $Text }); isError = $IsError } }
}
function Test-HasProperty($Object, [string]$Name) {
    return ($null -ne $Object -and $null -ne $Object.PSObject.Properties[$Name])
}
function Get-ArgumentValue($Object, [string]$Name, $Default = $null) {
    if (Test-HasProperty $Object $Name) { return $Object.PSObject.Properties[$Name].Value }
    return $Default
}

function Resolve-ProjectPath([string]$Path, [bool]$MustExist = $false) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "路径不能为空" }
    $candidate = if ([System.IO.Path]::IsPathRooted($Path)) { $Path } else { Join-Path $root $Path }
    $full = [System.IO.Path]::GetFullPath($candidate)
    $prefix = $root + [System.IO.Path]::DirectorySeparatorChar
    if (-not $full.Equals($root, [System.StringComparison]::OrdinalIgnoreCase) -and
        -not $full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "路径必须位于项目目录内: $Path"
    }
    if ($MustExist -and -not (Test-Path -LiteralPath $full)) { throw "文件不存在: $full" }
    return $full
}

function Get-MikanEngineProcesses {
    $matches = @()
    foreach ($entry in @(@("EngineMain", $exePath), @("MikanTestRunner", $gameplayTestPath))) {
        foreach ($process in @(Get-Process -Name $entry[0] -ErrorAction SilentlyContinue)) {
            try {
                if ($process.Path -and [System.IO.Path]::GetFullPath($process.Path).Equals(
                        [System.IO.Path]::GetFullPath($entry[1]), [System.StringComparison]::OrdinalIgnoreCase)) {
                    $matches += $process
                }
            } catch {}
        }
    }
    return @($matches)
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

function Get-CompactOutput([string]$Text, [int]$TailCount = 120) {
    if ([string]::IsNullOrWhiteSpace($Text)) { return "" }
    $lines = @($Text -split "`r?`n")
    if ($lines.Count -le $TailCount) { return ($lines -join "`n").Trim() }
    return ("... 已省略前 {0} 行 ...`n{1}" -f ($lines.Count - $TailCount), (($lines | Select-Object -Last $TailCount) -join "`n")).Trim()
}

function Invoke-Build($Arguments) {
    $target = [string](Get-ArgumentValue $Arguments "target" "EngineMain")
    if ($target -notin @("EngineMain", "MikanTestRunner", "Editor", "Game", "CompileShaders")) {
        return @{ text = "[ERROR] 不支持的构建目标: $target"; isError = $true }
    }
    $command = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "build.ps1"), "-Target", $target)
    if ([bool](Get-ArgumentValue $Arguments "killEngine" $false)) { $command += "-KillEngine" }
    if ([bool](Get-ArgumentValue $Arguments "cleanFirst" $false)) { $command += "-CleanFirst" }
    if ([bool](Get-ArgumentValue $Arguments "configureIfMissing" $true)) { $command += "-ConfigureIfMissing" }
    $output = (& powershell @command 2>&1 | Out-String)
    $exitCode = $LASTEXITCODE
    return @{ text = "=== build($target) ===`n$(Get-CompactOutput $output)`nexit code: $exitCode"; isError = ($exitCode -ne 0) }
}

function Invoke-ValidateScene($Arguments) {
    $scenePath = [string](Get-ArgumentValue $Arguments "scenePath" "")
    if ([string]::IsNullOrWhiteSpace($scenePath)) { return @{ text = "[ERROR] 缺少参数 scenePath"; isError = $true } }
    try { $scenePath = Resolve-ProjectPath $scenePath $true } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
    $command = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "validate_scene.ps1"), $scenePath, "-Schema", $schemaPath)
    if ([bool](Get-ArgumentValue $Arguments "checkAssets" $false)) { $command += "-CheckAssets" }
    $output = (& powershell @command 2>&1 | Out-String)
    $exitCode = $LASTEXITCODE
    return @{ text = "=== validate_scene ===`n$(Get-CompactOutput $output)`nexit code: $exitCode (0=通过)"; isError = ($exitCode -ne 0) }
}

function Test-ReservedExtraArgument([string]$Argument) {
    return $Argument -match '^--(headless|headless-no-render|frames|fixed-dt|dump-state|crash-log|scene|game)(=|$)'
}

function Invoke-RunTest($Arguments, [string]$Layer = "compatibility") {
    $scene = [string](Get-ArgumentValue $Arguments "scene" "")
    $game = [string](Get-ArgumentValue $Arguments "game" "")
    $frames = [int](Get-ArgumentValue $Arguments "frames" 120)
    $timeoutMs = [int](Get-ArgumentValue $Arguments "timeoutMs" 60000)
    $fixedDeltaSeconds = [double](Get-ArgumentValue $Arguments "fixedDeltaSeconds" (1.0 / 60.0))
    $requestedRender = [bool](Get-ArgumentValue $Arguments "render" $false)
    if ($Layer -eq "compatibility") { $Layer = if ($requestedRender) { "rendering" } else { "gameplay" } }
    $isGameplay = ($Layer -eq "gameplay")
    $render = -not $isGameplay
    $testExecutable = if ($isGameplay) { $gameplayTestPath } else { $exePath }
    $overwriteDump = [bool](Get-ArgumentValue $Arguments "overwriteDump" $false)
    $extraArgs = @((Get-ArgumentValue $Arguments "extraArgs" @()))

    if ([string]::IsNullOrWhiteSpace($scene)) { return @{ text = "[ERROR] 缺少参数 scene"; isError = $true } }
    if ($frames -lt 1 -or $frames -gt 1000000) { return @{ text = "[ERROR] frames 必须在 1..1000000"; isError = $true } }
    if ($timeoutMs -lt 1000 -or $timeoutMs -gt 3600000) { return @{ text = "[ERROR] timeoutMs 必须在 1000..3600000"; isError = $true } }
    if ($fixedDeltaSeconds -le 0.0 -or $fixedDeltaSeconds -gt 0.1) { return @{ text = "[ERROR] fixedDeltaSeconds 必须在 (0, 0.1]"; isError = $true } }
    if (-not (Test-Path -LiteralPath $testExecutable)) {
        $target = if ($isGameplay) { "MikanTestRunner" } else { "EngineMain" }
        return @{ text = "[ERROR] 测试入口不存在: $testExecutable（先调用 build target=$target）"; isError = $true }
    }
    try { $scene = Resolve-ProjectPath $scene $true } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
    foreach ($argument in $extraArgs) {
        if (Test-ReservedExtraArgument ([string]$argument)) { return @{ text = "[ERROR] extraArgs 不能覆盖 MCP 保留参数: $argument"; isError = $true } }
    }
    $running = @(Get-MikanEngineProcesses)
    if ($running.Count -gt 0) {
        return @{ text = "[ERROR] 当前项目的运行时/测试进程正在运行（PID $($running.Id -join ',')）。先调用 stop_engine 或 build(killEngine=true)。"; isError = $true }
    }

    New-Item -ItemType Directory -Path $runsRoot -Force | Out-Null
    $runId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8))
    $runDir = Join-Path $runsRoot $runId
    New-Item -ItemType Directory -Path $runDir -Force | Out-Null
    $script:lastRunDir = $runDir

    $dumpValue = [string](Get-ArgumentValue $Arguments "dumpStatePath" "")
    if ([string]::IsNullOrWhiteSpace($dumpValue)) {
        $dumpPath = Join-Path $runDir "state.json"
    } else {
        try { $dumpPath = Resolve-ProjectPath $dumpValue $false } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
        if (Test-Path -LiteralPath $dumpPath) {
            if (-not $overwriteDump) { return @{ text = "[ERROR] dumpStatePath 已存在；请换路径或设置 overwriteDump=true: $dumpPath"; isError = $true } }
            Remove-Item -LiteralPath $dumpPath -Force
        }
        $dumpParent = Split-Path -Parent $dumpPath
        if ($dumpParent) { New-Item -ItemType Directory -Path $dumpParent -Force | Out-Null }
    }
    $crashPath = Join-Path $runDir "crash_log.txt"
    $stdoutPath = Join-Path $runDir "stdout.log"
    $stderrPath = Join-Path $runDir "stderr.log"
    $resultPath = Join-Path $runDir "result.json"

    $argumentList = [System.Collections.Generic.List[string]]::new()
    if (-not $isGameplay) { $argumentList.Add("--headless") }
    $argumentList.Add("--frames"); $argumentList.Add([string]$frames)
    $argumentList.Add("--fixed-dt"); $argumentList.Add($fixedDeltaSeconds.ToString("0.########", [System.Globalization.CultureInfo]::InvariantCulture))
    $argumentList.Add("--dump-state"); $argumentList.Add($dumpPath)
    $argumentList.Add("--crash-log"); $argumentList.Add($crashPath)
    foreach ($argument in $extraArgs) { $argumentList.Add([string]$argument) }
    if (-not $isGameplay -and -not ($extraArgs -match '^--no-project-manager$' -or $extraArgs -match '^--project($|=)')) { $argumentList.Add("--no-project-manager") }
    $argumentList.Add("--scene"); $argumentList.Add($scene)
    if (-not [string]::IsNullOrWhiteSpace($game)) { $argumentList.Add("--game"); $argumentList.Add($game) }
    $argumentString = (($argumentList | ForEach-Object { ConvertTo-WindowsCommandLineArg $_ }) -join " ")

    Log "run_test[$runId][$Layer]: $testExecutable $argumentString"
    $startTime = Get-Date
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    $stdout = ""
    $stderr = ""
    $exitCode = $null
    try {
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $testExecutable
        $startInfo.WorkingDirectory = $exeDir
        $startInfo.Arguments = $argumentString
        $startInfo.UseShellExecute = $false
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.CreateNoWindow = $true
        $process = [System.Diagnostics.Process]::Start($startInfo)
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($timeoutMs)) {
            $timedOut = $true
            Log "run_test[$runId] timeout; terminating process tree pid=$($process.Id)"
            try { & "$env:SystemRoot\System32\taskkill.exe" /PID $process.Id /T /F 2>$null | Out-Null } catch { try { $process.Kill() } catch {} }
            [void]$process.WaitForExit(5000)
        } else { $process.WaitForExit() }
        if ($stdoutTask.IsCompleted) { try { $stdout = $stdoutTask.Result } catch {} }
        else { $stderr += "`n[MCP] stdout stream did not close after termination" }
        if ($stderrTask.IsCompleted) { try { $stderr += $stderrTask.Result } catch {} }
        else { $stderr += "`n[MCP] stderr stream did not close after termination" }
        if ($process.HasExited) { $exitCode = $process.ExitCode }
    } catch { $stderr += "`n[MCP] process launch failure: $($_.Exception.Message)" }
    finally { $stopwatch.Stop() }

    [System.IO.File]::WriteAllText($stdoutPath, $stdout, $utf8NoBom)
    [System.IO.File]::WriteAllText($stderrPath, $stderr, $utf8NoBom)
    $dumpExists = Test-Path -LiteralPath $dumpPath
    $crashExists = Test-Path -LiteralPath $crashPath
    $combinedLines = @(($stdout + "`n" + $stderr) -split "`r?`n")
    $diagnostics = @($combinedLines | Where-Object { $_ -match '(?i)\[Headless\]|\[Gameplay(Test|Runtime)\]|\[(ERR|FTL)\]|\[Crash\]|\bERROR\b|\bFatal\b|validation error' })
    $severeLog = @($combinedLines | Where-Object { $_ -match '(?i)\[(ERR|FTL)\]|\[Crash\]|\bERROR\b|\bFatal\b|validation error' }).Count -gt 0
    $failed = $timedOut -or $null -eq $exitCode -or $exitCode -ne 0 -or $crashExists -or -not $dumpExists -or $severeLog

    $dumpSummary = $null
    if ($dumpExists) {
        try {
            $dump = [System.IO.File]::ReadAllText($dumpPath, $utf8NoBom) | ConvertFrom-Json
            $dumpSummary = @{ frames = $dump.frames; entityCount = $dump.entity_count; game = $dump.game; runtimeLayer = $dump.runtime_layer }
            $script:lastDumpPath = $dumpPath
        } catch { $failed = $true; $diagnostics += "[MCP] dump JSON 解析失败: $($_.Exception.Message)" }
    }
    $resultObject = [ordered]@{
        runId = $runId; startedAt = $startTime.ToString("o"); durationMs = [Math]::Round($stopwatch.Elapsed.TotalMilliseconds)
        layer = $Layer; executable = $testExecutable; scene = $scene; game = $game; frames = $frames; fixedDeltaSeconds = $fixedDeltaSeconds; render = $render
        exitCode = $exitCode; timedOut = $timedOut; success = (-not $failed); dumpPath = $dumpPath
        stdoutPath = $stdoutPath; stderrPath = $stderrPath; crashPath = $crashPath; dump = $dumpSummary
    }
    [System.IO.File]::WriteAllText($resultPath, ($resultObject | ConvertTo-Json -Depth 10), $utf8NoBom)

    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.AppendLine("=== run_test ===")
    [void]$builder.AppendLine("run: $runId")
    [void]$builder.AppendLine("layer: $Layer")
    [void]$builder.AppendLine("mode: $(if ($isGameplay) {'CPU gameplay runtime (no SDL Video / no Vulkan initialization)'} else {'Vulkan rendering regression'})")
    [void]$builder.AppendLine("scene: $scene")
    [void]$builder.AppendLine("frames: $frames @ dt=$fixedDeltaSeconds   exit: $(if ($null -eq $exitCode) {'unavailable'} else {$exitCode})   timeout: $timedOut   success: $(-not $failed)")
    [void]$builder.AppendLine("runDir: $runDir")
    [void]$builder.AppendLine("dump: $(if ($dumpExists) {$dumpPath} else {'未生成'})")
    [void]$builder.AppendLine("logs: $stdoutPath | $stderrPath")
    if ($crashExists) { [void]$builder.AppendLine("crash: $crashPath") }
    foreach ($line in @($diagnostics | Select-Object -Last 80)) { [void]$builder.AppendLine("  $line") }
    return @{ text = $builder.ToString().TrimEnd(); isError = $failed }
}

function Get-DumpPath($Arguments) {
    $path = [string](Get-ArgumentValue $Arguments "path" "")
    if ([string]::IsNullOrWhiteSpace($path)) { $path = [string]$script:lastDumpPath }
    if ([string]::IsNullOrWhiteSpace($path)) { throw "缺少 path，且当前 MCP 会话还没有成功的 run_test" }
    return Resolve-ProjectPath $path $true
}
function Invoke-ReadDump($Arguments) {
    try { $path = Get-DumpPath $Arguments } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
    try {
        $raw = [System.IO.File]::ReadAllText($path, $utf8NoBom)
        $null = $raw | ConvertFrom-Json
        return @{ text = $raw; isError = $false }
    } catch { return @{ text = "[ERROR] dump JSON 无法读取: $($_.Exception.Message)"; isError = $true } }
}

function Invoke-AssertState($Arguments) {
    try { $path = Get-DumpPath $Arguments } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
    $assertions = @((Get-ArgumentValue $Arguments "assertions" @()))
    if ($assertions.Count -eq 0) { return @{ text = "[ERROR] assertions 至少需要一项"; isError = $true } }
    try { $dump = [System.IO.File]::ReadAllText($path, $utf8NoBom) | ConvertFrom-Json } catch { return @{ text = "[ERROR] dump JSON 无法解析: $($_.Exception.Message)"; isError = $true } }
    $allowedFields = @("pos", "wpos", "rot_deg", "scale", "visible")
    $lines = [System.Collections.Generic.List[string]]::new()
    $failures = 0
    $index = 0
    foreach ($assertion in $assertions) {
        $index++
        $selector = ""
        $entities = @()
        if (Test-HasProperty $assertion "entityId") {
            $entityId = [int64]$assertion.entityId; $selector = "id=$entityId"
            $entities = @($dump.entities | Where-Object { [int64]$_.id -eq $entityId })
        } elseif (Test-HasProperty $assertion "entityName") {
            $entityName = [string]$assertion.entityName; $selector = "name='$entityName'"
            $entities = @($dump.entities | Where-Object { [string]$_.name -ceq $entityName })
        } else { $failures++; $lines.Add("FAIL #${index}: 缺少 entityId 或 entityName"); continue }
        if ($entities.Count -ne 1) { $failures++; $lines.Add("FAIL #${index}: $selector 匹配 $($entities.Count) 个实体（必须唯一）"); continue }
        $field = [string](Get-ArgumentValue $assertion "field" "")
        if ($field -notin $allowedFields) { $failures++; $lines.Add("FAIL #${index}: 不支持字段 '$field'（允许: $($allowedFields -join ', ')）"); continue }
        if (-not (Test-HasProperty $assertion "expected")) { $failures++; $lines.Add("FAIL #${index}: 缺少 expected"); continue }
        $actual = $entities[0].PSObject.Properties[$field].Value
        $expected = $assertion.expected
        $tolerance = [double](Get-ArgumentValue $assertion "tolerance" 0.001)
        if ($tolerance -lt 0.0) { $failures++; $lines.Add("FAIL #${index}: tolerance 不能为负数"); continue }
        $passed = $true
        if ($field -eq "visible") {
            if ($expected -isnot [bool]) { $passed = $false } else { $passed = ([bool]$actual -eq [bool]$expected) }
        } else {
            $actualValues = @($actual); $expectedValues = @($expected)
            if ($actualValues.Count -ne $expectedValues.Count) { $passed = $false }
            else {
                for ($i = 0; $i -lt $actualValues.Count; ++$i) {
                    try { if ([Math]::Abs([double]$actualValues[$i] - [double]$expectedValues[$i]) -gt $tolerance) { $passed = $false; break } }
                    catch { $passed = $false; break }
                }
            }
        }
        $actualText = (@($actual) | ForEach-Object { [string]$_ }) -join ", "
        $expectedText = (@($expected) | ForEach-Object { [string]$_ }) -join ", "
        if ($passed) { $lines.Add("PASS #${index}: $selector $field=[$actualText] expected=[$expectedText] tol=$tolerance") }
        else { $failures++; $lines.Add("FAIL #${index}: $selector $field=[$actualText] expected=[$expectedText] tol=$tolerance") }
    }
    $header = "=== assert_state ===`ndump: $path`nresult: $($assertions.Count - $failures)/$($assertions.Count) passed"
    return @{ text = $header + "`n" + ($lines -join "`n"); isError = ($failures -gt 0) }
}

function Invoke-EngineStatus {
    $running = @(Get-MikanEngineProcesses)
    $defaultCrash = Join-Path $exeDir "log\crash_log.txt"
    $latestRun = $null
    if (Test-Path -LiteralPath $runsRoot) { $latestRun = Get-ChildItem -LiteralPath $runsRoot -Directory -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1 }
    $builder = [System.Text.StringBuilder]::new()
    if ($running.Count -gt 0) {
        foreach ($process in $running) { [void]$builder.AppendLine("$($process.ProcessName) 运行中: PID $($process.Id)") }
    } else { [void]$builder.AppendLine("EngineMain/MikanTestRunner 均未运行") }
    [void]$builder.AppendLine("render exe: $exePath 存在=$(Test-Path -LiteralPath $exePath)")
    [void]$builder.AppendLine("gameplay exe: $gameplayTestPath 存在=$(Test-Path -LiteralPath $gameplayTestPath)")
    [void]$builder.AppendLine("默认崩溃日志: $defaultCrash 存在=$(Test-Path -LiteralPath $defaultCrash)")
    if ($latestRun) { [void]$builder.AppendLine("最近 MCP 运行: $($latestRun.FullName)") }
    if ($script:lastDumpPath) { [void]$builder.AppendLine("当前会话最近 dump: $script:lastDumpPath") }
    return @{ text = $builder.ToString().TrimEnd(); isError = $false }
}
function Invoke-StopEngine {
    $running = @(Get-MikanEngineProcesses)
    if ($running.Count -eq 0) { return @{ text = "当前项目的 EngineMain/MikanTestRunner 均未运行"; isError = $false } }
    $ids = @($running.Id)
    foreach ($process in $running) { Stop-Process -Id $process.Id -Force }
    Start-Sleep -Milliseconds 500
    $remaining = @(Get-MikanEngineProcesses)
    return @{ text = "已结束 $($ids.Count) 个当前项目运行时/测试进程（PID $($ids -join ', ')）；remaining=$($remaining.Count)"; isError = ($remaining.Count -gt 0) }
}

$tools = @(
    @{
        name = "build"; description = "构建 MikanEngine。可在 CMakeCache 缺失时自动配置，也可 clean-first 排除陈旧中间产物。"
        inputSchema = @{ type = "object"; properties = @{
            target = @{ type = "string"; enum = @("EngineMain", "MikanTestRunner", "Editor", "Game", "CompileShaders"); "default" = "EngineMain" }
            killEngine = @{ type = "boolean"; "default" = $false }
            cleanFirst = @{ type = "boolean"; description = "构建前执行 CMake clean-first"; "default" = $false }
            configureIfMissing = @{ type = "boolean"; description = "缺少 CMakeCache 时使用 x64-release preset 自动配置"; "default" = $true }
        } }
    },
    @{
        name = "validate_scene"; description = "离线检查场景 JSON、实体 id/parent、transform 数值、组件字段和可选资源引用。"
        inputSchema = @{ type = "object"; properties = @{ scenePath = @{ type = "string" }; checkAssets = @{ type = "boolean"; "default" = $false } }; required = @("scenePath") }
    },
    @{
        name = "run_gameplay_test"; description = "运行玩法层确定性测试：使用 MikanTestRunner，不创建窗口、不初始化 SDL Video/Vulkan；覆盖场景、脚本、物理、动画和玩法模块。"
        inputSchema = @{ type = "object"; properties = @{
            scene = @{ type = "string" }; game = @{ type = "string" }
            frames = @{ type = "integer"; minimum = 1; maximum = 1000000; "default" = 120 }
            fixedDeltaSeconds = @{ type = "number"; exclusiveMinimum = 0; maximum = 0.1; "default" = 0.016666667 }
            dumpStatePath = @{ type = "string"; description = "可选；默认使用独立 mcp_runs/<run-id>/state.json" }
            overwriteDump = @{ type = "boolean"; "default" = $false }
            extraArgs = @{ type = "array"; items = @{ type = "string" } }
            timeoutMs = @{ type = "integer"; minimum = 1000; maximum = 3600000; "default" = 60000 }
        }; required = @("scene") }
    },
    @{
        name = "run_render_test"; description = "运行渲染层回归测试：使用 EngineMain 完整初始化 SDL Video、Vulkan、FrameRender 与 Present，并输出独立日志和状态 dump。"
        inputSchema = @{ type = "object"; properties = @{
            scene = @{ type = "string" }; game = @{ type = "string" }
            frames = @{ type = "integer"; minimum = 1; maximum = 1000000; "default" = 120 }
            fixedDeltaSeconds = @{ type = "number"; exclusiveMinimum = 0; maximum = 0.1; "default" = 0.016666667 }
            dumpStatePath = @{ type = "string"; description = "可选；默认使用独立 mcp_runs/<run-id>/state.json" }
            overwriteDump = @{ type = "boolean"; "default" = $false }
            extraArgs = @{ type = "array"; items = @{ type = "string" } }
            timeoutMs = @{ type = "integer"; minimum = 1000; maximum = 3600000; "default" = 60000 }
        }; required = @("scene") }
    },
    @{
        name = "run_test"; description = "兼容入口。render=false 使用 CPU-only MikanTestRunner；render=true 使用完整 EngineMain Vulkan 渲染路径。新调用建议改用 run_gameplay_test/run_render_test。"
        inputSchema = @{ type = "object"; properties = @{
            scene = @{ type = "string" }; game = @{ type = "string" }
            frames = @{ type = "integer"; minimum = 1; maximum = 1000000; "default" = 120 }
            fixedDeltaSeconds = @{ type = "number"; exclusiveMinimum = 0; maximum = 0.1; "default" = 0.016666667 }
            render = @{ type = "boolean"; description = "true=渲染层；false=玩法层 CPU-only runner"; "default" = $false }
            dumpStatePath = @{ type = "string"; description = "可选；默认使用独立 mcp_runs/<run-id>/state.json" }
            overwriteDump = @{ type = "boolean"; "default" = $false }
            extraArgs = @{ type = "array"; items = @{ type = "string" } }
            timeoutMs = @{ type = "integer"; minimum = 1000; maximum = 3600000; "default" = 60000 }
        }; required = @("scene") }
    },
    @{
        name = "read_dump"; description = "读取并验证状态 dump JSON。path 可省略，此时使用当前 MCP 会话最近一次成功 run_test 的 dump。"
        inputSchema = @{ type = "object"; properties = @{ path = @{ type = "string" } } }
    },
    @{
        name = "assert_state"; description = "对 dump 中实体状态做机器可判定断言。实体用 entityId 或唯一 entityName 选择；支持 pos/wpos/rot_deg/scale/visible。"
        inputSchema = @{ type = "object"; properties = @{
            path = @{ type = "string"; description = "可省略，使用最近成功 dump" }
            assertions = @{ type = "array"; minItems = 1; items = @{ type = "object"; properties = @{
                entityId = @{ type = "integer" }; entityName = @{ type = "string" }
                field = @{ type = "string"; enum = @("pos", "wpos", "rot_deg", "scale", "visible") }
                expected = @{}; tolerance = @{ type = "number"; minimum = 0; "default" = 0.001 }
            }; required = @("field", "expected") } }
        }; required = @("assertions") }
    },
    @{ name = "engine_status"; description = "查询当前项目 EngineMain/MikanTestRunner、可执行文件、崩溃日志和最近 MCP 运行。"; inputSchema = @{ type = "object"; properties = @{} } },
    @{ name = "stop_engine"; description = "只结束当前项目路径下的 EngineMain.exe/MikanTestRunner.exe，避免误杀其他同名程序。"; inputSchema = @{ type = "object"; properties = @{} } }
)

Log "MikanEngine MCP server started (root=$root)"
while ($true) {
    $line = [Console]::In.ReadLine()
    if ($null -eq $line) { Log "stdin EOF, exiting"; exit 0 }
    $line = $line.TrimStart([char]0xFEFF)
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $id = $null
    try {
        try { $message = $line | ConvertFrom-Json } catch { Log "invalid JSON: $($_.Exception.Message)"; Send-Error $null -32700 "Parse error"; continue }
        if ($null -eq $message -or $message -is [array] -or [string]$message.jsonrpc -ne "2.0" -or -not (Test-HasProperty $message "method")) {
            Send-Error $null -32600 "Invalid Request"; continue
        }
        $hasId = Test-HasProperty $message "id"
        $id = if ($hasId) { $message.id } else { $null }
        $method = [string]$message.method
        $params = Get-ArgumentValue $message "params" $null
        if (-not $hasId) {
            if ($method -eq "notifications/initialized") { Log "client initialized" } else { Log "ignored notification: $method" }
            continue
        }
        switch ($method) {
            "initialize" { Send-Result $id @{ protocolVersion = "2025-03-26"; capabilities = @{ tools = @{ listChanged = $false } }; serverInfo = @{ name = "mikanengine-mcp"; version = "3.0.0" } } }
            "ping" { Send-Result $id @{} }
            "tools/list" { Send-Result $id @{ tools = $tools } }
            "tools/call" {
                if ($null -eq $params -or -not (Test-HasProperty $params "name")) { Send-Error $id -32602 "tools/call requires params.name"; continue }
                $toolName = [string]$params.name
                $arguments = Get-ArgumentValue $params "arguments" @{}
                if ($null -eq $arguments) { $arguments = @{} }
                Log "tools/call: $toolName"
                $result = switch ($toolName) {
                    "build" { Invoke-Build $arguments; break }
                    "validate_scene" { Invoke-ValidateScene $arguments; break }
                    "run_gameplay_test" { Invoke-RunTest $arguments "gameplay"; break }
                    "run_render_test" { Invoke-RunTest $arguments "rendering"; break }
                    "run_test" { Invoke-RunTest $arguments "compatibility"; break }
                    "read_dump" { Invoke-ReadDump $arguments; break }
                    "assert_state" { Invoke-AssertState $arguments; break }
                    "engine_status" { Invoke-EngineStatus; break }
                    "stop_engine" { Invoke-StopEngine; break }
                    default { $null }
                }
                if ($null -eq $result) { Send-Error $id -32602 "Unknown tool: $toolName" } else { Send-ToolResult $id $result.text ([bool]$result.isError) }
            }
            default { Send-Error $id -32601 "Method not found: $method" }
        }
    } catch {
        $messageText = $_.Exception.Message
        Log "request failed: $messageText"
        if ($null -ne $id) { Send-ToolResult $id "[MCP ERROR] $messageText" $true } else { Send-Error $null -32603 "Internal error" $messageText }
    }
}
