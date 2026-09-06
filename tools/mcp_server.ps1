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
function Send-ToolObjectResult($Id, $ToolResult) {
    $payload = @{
        content = @(@{ type = "text"; text = [string]$ToolResult.text })
        isError = [bool]$ToolResult.isError
    }
    if ($ToolResult -is [hashtable] -and $ToolResult.ContainsKey("structuredContent") -and
        $null -ne $ToolResult.structuredContent) {
        $payload.structuredContent = $ToolResult.structuredContent
    }
    Send-Result $Id $payload
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

function Invoke-CreateScript($Arguments) {
    $scriptName = [string](Get-ArgumentValue $Arguments "scriptName" "")
    $outputPath = [string](Get-ArgumentValue $Arguments "outputPath" "")
    if ([string]::IsNullOrWhiteSpace($scriptName) -or [string]::IsNullOrWhiteSpace($outputPath)) {
        return @{ text = "[ERROR] create_script 需要 scriptName 和 outputPath"; isError = $true }
    }

    $scaffoldScript = Join-Path $PSScriptRoot "script_scaffold.ps1"
    if (-not (Test-Path -LiteralPath $scaffoldScript -PathType Leaf)) {
        return @{ text = "[ERROR] 脚本生成器不存在: $scaffoldScript"; isError = $true }
    }
    $command = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $scaffoldScript,
        "-ScriptName", $scriptName, "-OutputPath", $outputPath
    )
    $className = [string](Get-ArgumentValue $Arguments "className" "")
    if ($className) { $command += @("-ClassName", $className) }

    $fields = Get-ArgumentValue $Arguments "fields" $null
    $requestDir = $null
    if ($null -ne $fields) {
        $fieldsJson = @($fields) | ConvertTo-Json -Depth 20 -Compress
        $requestId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8))
        $requestDir = Join-Path $runsRoot ("script_request-" + $requestId)
        New-Item -ItemType Directory -Path $requestDir -Force | Out-Null
        $fieldsPath = Join-Path $requestDir "fields.json"
        [System.IO.File]::WriteAllText($fieldsPath, $fieldsJson, $utf8NoBom)
        $command += @("-FieldsPath", $fieldsPath)
    }
    $source = [string](Get-ArgumentValue $Arguments "source" "")
    if ($source) {
        if (-not $requestDir) {
            $requestId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8))
            $requestDir = Join-Path $runsRoot ("script_request-" + $requestId)
            New-Item -ItemType Directory -Path $requestDir -Force | Out-Null
        }
        $sourcePath = Join-Path $requestDir "source.cpp"
        [System.IO.File]::WriteAllText($sourcePath, $source, $utf8NoBom)
        $command += @("-SourcePath", $sourcePath)
    }
    if ([bool](Get-ArgumentValue $Arguments "overwrite" $false)) { $command += "-Force" }
    if ([bool](Get-ArgumentValue $Arguments "compile" $false)) { $command += "-Compile" }

    Log "create_script: script=$scriptName output=$outputPath"
    $output = (& powershell @command 2>&1 | Out-String).Trim()
    $exitCode = [int]$LASTEXITCODE
    $structured = $null
    $outputLines = @($output -split '\r?\n')
    for ($lineIndex = $outputLines.Count - 1; $lineIndex -ge 0; --$lineIndex) {
        $line = $outputLines[$lineIndex]
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try {
            $candidate = $line | ConvertFrom-Json
            if ($candidate -and $candidate.tool -eq "script_scaffold") {
                $structured = $candidate
                break
            }
        } catch {}
    }
    $success = ($exitCode -eq 0 -and $null -ne $structured -and [bool]$structured.success)
    $text = "=== create_script ===`n$(Get-CompactOutput $output)`nexit code: $exitCode (0=通过)"
    return @{ text = $text; isError = (-not $success); structuredContent = $structured }
}

function Invoke-AgentWorkflow($Arguments) {
    $workflowValue = Get-ArgumentValue $Arguments "workflow" $null
    $workflowPathValue = [string](Get-ArgumentValue $Arguments "workflowPath" "")
    if ($null -ne $workflowValue -and $workflowPathValue) {
        return @{ text = "[ERROR] workflow 与 workflowPath 只能二选一"; isError = $true }
    }
    if ($null -eq $workflowValue -and [string]::IsNullOrWhiteSpace($workflowPathValue)) {
        return @{ text = "[ERROR] 必须提供 workflow 对象或 workflowPath"; isError = $true }
    }
    $workflowScript = Join-Path $PSScriptRoot "agent_workflow.ps1"
    if (-not (Test-Path -LiteralPath $workflowScript -PathType Leaf)) {
        return @{ text = "[ERROR] Agent workflow 编排器不存在: $workflowScript"; isError = $true }
    }

    $requestDir = $null
    if ($null -ne $workflowValue) {
        $requestId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8))
        $requestDir = Join-Path $runsRoot ("workflow_request-" + $requestId)
        New-Item -ItemType Directory -Path $requestDir -Force | Out-Null
        $workflowPath = Join-Path $requestDir "workflow.json"
        $workflowJson = $workflowValue | ConvertTo-Json -Depth 60
        [System.IO.File]::WriteAllText($workflowPath, $workflowJson, $utf8NoBom)
    } else {
        try { $workflowPath = Resolve-ProjectPath $workflowPathValue $true } catch {
            return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true }
        }
    }

    $command = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $workflowScript,
        "-WorkflowPath", $workflowPath
    )
    $outputRoot = [string](Get-ArgumentValue $Arguments "outputRoot" "")
    if ($outputRoot) { $command += @("-OutputRoot", $outputRoot) }
    if ([bool](Get-ArgumentValue $Arguments "dryRun" $false)) { $command += "-DryRun" }
    if ([bool](Get-ArgumentValue $Arguments "continueOnFailure" $false)) { $command += "-ContinueOnFailure" }

    Log "run_agent_workflow: workflow=$workflowPath"
    $output = (& powershell @command 2>&1 | Out-String).Trim()
    $exitCode = [int]$LASTEXITCODE
    $resultMarker = @($output -split "\r?\n" | Where-Object { $_ -like "WORKFLOW_RESULT_PATH=*" } | Select-Object -Last 1)
    $resultPath = $null
    if ($resultMarker.Count -gt 0) {
        $resultPath = $resultMarker[0].Substring("WORKFLOW_RESULT_PATH=".Length).Trim()
    }
    $structured = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        try { $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json } catch {
            Log "agent workflow result parse failed: $($_.Exception.Message)"
        }
    }
    $success = ($exitCode -eq 0 -and $null -ne $structured -and [bool]$structured.success)
    $text = "=== run_agent_workflow ===`n$(Get-CompactOutput $output)`nexit code: $exitCode (0=通过)"
    return @{ text = $text; isError = (-not $success); structuredContent = $structured }
}

function Invoke-AgentTask($Arguments) {
    $taskValue = Get-ArgumentValue $Arguments "task" $null
    $taskPathValue = [string](Get-ArgumentValue $Arguments "taskPath" "")
    if ($null -ne $taskValue -and $taskPathValue) {
        return @{ text = "[ERROR] task 与 taskPath 只能二选一"; isError = $true }
    }
    if ($null -eq $taskValue -and [string]::IsNullOrWhiteSpace($taskPathValue)) {
        return @{ text = "[ERROR] 必须提供 task 对象或 taskPath"; isError = $true }
    }
    $taskScript = Join-Path $PSScriptRoot "agent_task.ps1"
    if (-not (Test-Path -LiteralPath $taskScript -PathType Leaf)) {
        return @{ text = "[ERROR] Agent task runner 不存在: $taskScript"; isError = $true }
    }

    $requestDir = $null
    if ($null -ne $taskValue) {
        $requestId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8))
        $requestDir = Join-Path $runsRoot ("task_request-" + $requestId)
        New-Item -ItemType Directory -Path $requestDir -Force | Out-Null
        $taskPath = Join-Path $requestDir "task.json"
        $taskJson = $taskValue | ConvertTo-Json -Depth 60
        [System.IO.File]::WriteAllText($taskPath, $taskJson, $utf8NoBom)
    } else {
        try { $taskPath = Resolve-ProjectPath $taskPathValue $true } catch {
            return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true }
        }
    }

    $command = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $taskScript,
        "-TaskPath", $taskPath
    )
    $outputRoot = [string](Get-ArgumentValue $Arguments "outputRoot" "")
    if ($outputRoot) { $command += @("-OutputRoot", $outputRoot) }
    $mode = [string](Get-ArgumentValue $Arguments "mode" "")
    if ($mode) { $command += @("-Mode", $mode) }
    $maxAttempts = Get-ArgumentValue $Arguments "maxAttempts" $null
    if ($null -ne $maxAttempts) { $command += @("-MaxAttempts", [string]$maxAttempts) }

    Log "run_agent_task: task=$taskPath mode=$(if ($mode) { $mode } else { 'preview' })"
    $output = (& powershell @command 2>&1 | Out-String).Trim()
    $exitCode = [int]$LASTEXITCODE
    $resultMarker = @($output -split "\r?\n" | Where-Object { $_ -like "TASK_RESULT_PATH=*" } | Select-Object -Last 1)
    $resultPath = $null
    if ($resultMarker.Count -gt 0) { $resultPath = $resultMarker[0].Substring("TASK_RESULT_PATH=".Length).Trim() }
    $structured = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        try { $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json } catch {
            Log "agent task result parse failed: $($_.Exception.Message)"
        }
    }
    $success = ($exitCode -eq 0 -and $null -ne $structured -and [bool]$structured.success)
    $text = "=== run_agent_task ===`n$(Get-CompactOutput $output)`nexit code: $exitCode (0=通过)"
    return @{ text = $text; isError = (-not $success); structuredContent = $structured }
}

function Invoke-AgentPlan($Arguments) {
    $planValue = Get-ArgumentValue $Arguments "plan" $null
    $planPathValue = [string](Get-ArgumentValue $Arguments "planPath" "")
    if ($null -ne $planValue -and $planPathValue) { return @{ text = "[ERROR] plan 与 planPath 只能二选一"; isError = $true } }
    if ($null -eq $planValue -and [string]::IsNullOrWhiteSpace($planPathValue)) { return @{ text = "[ERROR] 必须提供 plan 对象或 planPath"; isError = $true } }
    $planScript = Join-Path $PSScriptRoot "agent_plan.ps1"
    if (-not (Test-Path -LiteralPath $planScript -PathType Leaf)) { return @{ text = "[ERROR] Agent planner 不存在: $planScript"; isError = $true } }
    if ($null -ne $planValue) {
        $requestId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8))
        $requestDir = Join-Path $runsRoot ("plan_request-" + $requestId)
        New-Item -ItemType Directory -Path $requestDir -Force | Out-Null
        $planPath = Join-Path $requestDir "plan.json"
        [System.IO.File]::WriteAllText($planPath, ($planValue | ConvertTo-Json -Depth 60), $utf8NoBom)
    } else {
        try { $planPath = Resolve-ProjectPath $planPathValue $true } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
    }
    $command = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $planScript, "-PlanPath", $planPath)
    $outputRoot = [string](Get-ArgumentValue $Arguments "outputRoot" "")
    if ($outputRoot) { $command += @("-OutputRoot", $outputRoot) }
    $mode = [string](Get-ArgumentValue $Arguments "mode" "")
    if ($mode) { $command += @("-Mode", $mode) }
    $maxAttempts = Get-ArgumentValue $Arguments "maxAttempts" $null
    if ($null -ne $maxAttempts) { $command += @("-MaxAttempts", [string]$maxAttempts) }
    Log "run_agent_plan: plan=$planPath mode=$(if ($mode) { $mode } else { 'preview' })"
    $output = (& powershell @command 2>&1 | Out-String).Trim()
    $exitCode = [int]$LASTEXITCODE
    $marker = @($output -split "\r?\n" | Where-Object { $_ -like "PLAN_RESULT_PATH=*" } | Select-Object -Last 1)
    $resultPath = $null
    if ($marker.Count -gt 0) { $resultPath = $marker[0].Substring("PLAN_RESULT_PATH=".Length).Trim() }
    $structured = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) { try { $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json } catch { Log "agent plan result parse failed: $($_.Exception.Message)" } }
    $success = ($exitCode -eq 0 -and $null -ne $structured -and [bool]$structured.success)
    return @{ text = "=== run_agent_plan ===`n$(Get-CompactOutput $output)`nexit code: $exitCode (0=通过)"; isError = (-not $success); structuredContent = $structured }
}

function Invoke-AgentGameSpec($Arguments) {
    $specValue = Get-ArgumentValue $Arguments "gameSpec" $null
    $specPathValue = [string](Get-ArgumentValue $Arguments "gameSpecPath" "")
    if ($null -ne $specValue -and $specPathValue) { return @{ text = "[ERROR] gameSpec 与 gameSpecPath 只能二选一"; isError = $true } }
    if ($null -eq $specValue -and [string]::IsNullOrWhiteSpace($specPathValue)) { return @{ text = "[ERROR] 必须提供 gameSpec 对象或 gameSpecPath"; isError = $true } }
    $gameSpecScript = Join-Path $PSScriptRoot "agent_game_spec.ps1"
    if (-not (Test-Path -LiteralPath $gameSpecScript -PathType Leaf)) { return @{ text = "[ERROR] GameSpec 执行器不存在: $gameSpecScript"; isError = $true } }
    if ($null -ne $specValue) {
        $requestId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8))
        $requestDir = Join-Path $runsRoot ("gamespec_request-" + $requestId)
        New-Item -ItemType Directory -Path $requestDir -Force | Out-Null
        $specPath = Join-Path $requestDir "gamespec.json"
        [System.IO.File]::WriteAllText($specPath, ($specValue | ConvertTo-Json -Depth 80), $utf8NoBom)
    } else {
        try { $specPath = Resolve-ProjectPath $specPathValue $true } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
    }
    $command = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $gameSpecScript, "-GameSpecPath", $specPath)
    $outputRoot = [string](Get-ArgumentValue $Arguments "outputRoot" "out\agent_game_specs")
    if ($outputRoot) { $command += @("-OutputRoot", $outputRoot) }
    $mode = [string](Get-ArgumentValue $Arguments "mode" "")
    if ($mode) { $command += @("-Mode", $mode) }
    $runId = [string](Get-ArgumentValue $Arguments "runId" "")
    if ($runId) { $command += @("-RunId", $runId) }
    Log "run_agent_game_spec: spec=$specPath mode=$(if ($mode) { $mode } else { 'preview' })"
    $output = (& powershell @command 2>&1 | Out-String).Trim()
    $exitCode = [int]$LASTEXITCODE
    $marker = @($output -split "\r?\n" | Where-Object { $_ -like "GAMESPEC_RESULT_PATH=*" } | Select-Object -Last 1)
    $resultPath = $null
    if ($marker.Count -gt 0) { $resultPath = $marker[0].Substring("GAMESPEC_RESULT_PATH=".Length).Trim() }
    $structured = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) { try { $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json } catch { Log "game spec result parse failed: $($_.Exception.Message)" } }
    $success = ($exitCode -eq 0 -and $null -ne $structured -and [bool](Get-ArgumentValue $structured "success" $false))
    return @{ text = "=== run_agent_game_spec ===`n$(Get-CompactOutput $output)`nexit code: $exitCode (0=通过)"; isError = (-not $success); structuredContent = $structured }
}

function Invoke-AgentTest($Arguments) {
    $testValue = Get-ArgumentValue $Arguments "testSpec" $null
    $testPathValue = [string](Get-ArgumentValue $Arguments "testSpecPath" "")
    if ($null -ne $testValue -and $testPathValue) { return @{ text = "[ERROR] testSpec 与 testSpecPath 只能二选一"; isError = $true } }
    if ($null -eq $testValue -and [string]::IsNullOrWhiteSpace($testPathValue)) { return @{ text = "[ERROR] 必须提供 testSpec 对象或 testSpecPath"; isError = $true } }
    $testScript = Join-Path $PSScriptRoot "agent_test.ps1"
    if (-not (Test-Path -LiteralPath $testScript -PathType Leaf)) { return @{ text = "[ERROR] Agent test runner 不存在: $testScript"; isError = $true } }
    if ($null -ne $testValue) {
        $requestId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8))
        $requestDir = Join-Path $runsRoot ("test_request-" + $requestId)
        New-Item -ItemType Directory -Path $requestDir -Force | Out-Null
        $testPath = Join-Path $requestDir "test-spec.json"
        [System.IO.File]::WriteAllText($testPath, ($testValue | ConvertTo-Json -Depth 80), $utf8NoBom)
    } else {
        try { $testPath = Resolve-ProjectPath $testPathValue $true } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
    }
    $command = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $testScript, "-TestSpecPath", $testPath)
    $outputRoot = [string](Get-ArgumentValue $Arguments "outputRoot" "out\agent_tests")
    if ($outputRoot) { $command += @("-OutputRoot", $outputRoot) }
    $mode = [string](Get-ArgumentValue $Arguments "mode" "")
    if ($mode) { $command += @("-Mode", $mode) }
    $runId = [string](Get-ArgumentValue $Arguments "runId" "")
    if ($runId) { $command += @("-RunId", $runId) }
    Log "run_agent_test: spec=$testPath mode=$(if ($mode) { $mode } else { 'preview' })"
    $output = (& powershell @command 2>&1 | Out-String).Trim()
    $exitCode = [int]$LASTEXITCODE
    $marker = @($output -split "\r?\n" | Where-Object { $_ -like "TEST_RESULT_PATH=*" } | Select-Object -Last 1)
    $resultPath = $null
    if ($marker.Count -gt 0) { $resultPath = $marker[0].Substring("TEST_RESULT_PATH=".Length).Trim() }
    $structured = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        try { $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json } catch { Log "agent test result parse failed: $($_.Exception.Message)" }
    }
    $success = ($exitCode -eq 0 -and $null -ne $structured -and [bool](Get-ArgumentValue $structured "success" $false))
    return @{ text = "=== run_agent_test ===`n$(Get-CompactOutput $output)`nexit code: $exitCode (0=通过)"; isError = (-not $success); structuredContent = $structured }
}

function Invoke-AgentRepair($Arguments) {
    $sourcePathValue = [string](Get-ArgumentValue $Arguments "sourceResultPath" "")
    if ([string]::IsNullOrWhiteSpace($sourcePathValue)) { return @{ text = "[ERROR] 缺少 sourceResultPath"; isError = $true } }
    try { $sourcePath = Resolve-ProjectPath $sourcePathValue $true } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
    $repairValue = Get-ArgumentValue $Arguments "repair" $null
    $repairPathValue = [string](Get-ArgumentValue $Arguments "repairPath" "")
    if ($null -ne $repairValue -and $repairPathValue) { return @{ text = "[ERROR] repair 与 repairPath 只能二选一"; isError = $true } }
    if ($null -eq $repairValue -and [string]::IsNullOrWhiteSpace($repairPathValue)) { return @{ text = "[ERROR] 必须提供 repair 对象或 repairPath"; isError = $true } }
    $repairScript = Join-Path $PSScriptRoot "agent_repair.ps1"
    if (-not (Test-Path -LiteralPath $repairScript -PathType Leaf)) { return @{ text = "[ERROR] Agent repair runner 不存在: $repairScript"; isError = $true } }
    if ($null -ne $repairValue) {
        $requestId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8))
        $requestDir = Join-Path $runsRoot ("repair_request-" + $requestId)
        New-Item -ItemType Directory -Path $requestDir -Force | Out-Null
        $repairPath = Join-Path $requestDir "repair.json"
        [System.IO.File]::WriteAllText($repairPath, ($repairValue | ConvertTo-Json -Depth 80), $utf8NoBom)
    } else {
        try { $repairPath = Resolve-ProjectPath $repairPathValue $true } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
    }
    $command = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $repairScript, "-SourceResultPath", $sourcePath, "-RepairPath", $repairPath)
    $outputRoot = [string](Get-ArgumentValue $Arguments "outputRoot" "out\agent_repairs")
    if ($outputRoot) { $command += @("-OutputRoot", $outputRoot) }
    $mode = [string](Get-ArgumentValue $Arguments "mode" "")
    if ($mode) { $command += @("-Mode", $mode) }
    $runId = [string](Get-ArgumentValue $Arguments "runId" "")
    if ($runId) { $command += @("-RunId", $runId) }
    $maxAttempts = Get-ArgumentValue $Arguments "maxAttempts" $null
    if ($null -ne $maxAttempts) { $command += @("-MaxAttempts", [string]$maxAttempts) }
    Log "run_agent_repair: source=$sourcePath repair=$repairPath mode=$(if ($mode) { $mode } else { 'preview' })"
    $output = (& powershell @command 2>&1 | Out-String).Trim()
    $exitCode = [int]$LASTEXITCODE
    $marker = @($output -split "\r?\n" | Where-Object { $_ -like "REPAIR_RESULT_PATH=*" } | Select-Object -Last 1)
    $resultPath = $null
    if ($marker.Count -gt 0) { $resultPath = $marker[0].Substring("REPAIR_RESULT_PATH=".Length).Trim() }
    $structured = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        try { $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json } catch { Log "agent repair result parse failed: $($_.Exception.Message)" }
    }
    $success = ($exitCode -eq 0 -and $null -ne $structured -and [bool](Get-ArgumentValue $structured "success" $false))
    return @{ text = "=== run_agent_repair ===`n$(Get-CompactOutput $output)`nexit code: $exitCode (0=通过)"; isError = (-not $success); structuredContent = $structured }
}

function Invoke-AgentEvidence($Arguments) {
    $resultPathValue = [string](Get-ArgumentValue $Arguments "resultPath" "")
    if ([string]::IsNullOrWhiteSpace($resultPathValue)) { return @{ text = "[ERROR] 缺少 resultPath"; isError = $true } }
    try { $sourceResultPath = Resolve-ProjectPath $resultPathValue $true } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
    $evidenceScript = Join-Path $PSScriptRoot "agent_evidence.ps1"
    if (-not (Test-Path -LiteralPath $evidenceScript -PathType Leaf)) { return @{ text = "[ERROR] Agent evidence collector 不存在: $evidenceScript"; isError = $true } }
    $command = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $evidenceScript, "-SourceResultPath", $sourceResultPath)
    $outputRoot = [string](Get-ArgumentValue $Arguments "outputRoot" "")
    if ($outputRoot) { $command += @("-OutputRoot", $outputRoot) }
    Log "collect_agent_evidence: result=$sourceResultPath"
    $output = (& powershell @command 2>&1 | Out-String).Trim()
    $exitCode = [int]$LASTEXITCODE
    $marker = @($output -split "\r?\n" | Where-Object { $_ -like "EVIDENCE_RESULT_PATH=*" } | Select-Object -Last 1)
    $evidencePath = $null
    if ($marker.Count -gt 0) { $evidencePath = $marker[0].Substring("EVIDENCE_RESULT_PATH=".Length).Trim() }
    $structured = $null
    if ($evidencePath -and (Test-Path -LiteralPath $evidencePath -PathType Leaf)) { try { $structured = [System.IO.File]::ReadAllText($evidencePath, $utf8NoBom) | ConvertFrom-Json } catch { Log "agent evidence result parse failed: $($_.Exception.Message)" } }
    $success = ($exitCode -eq 0 -and $null -ne $structured -and [bool]$structured.success)
    return @{ text = "=== collect_agent_evidence ===`n$(Get-CompactOutput $output)`nexit code: $exitCode (0=通过)"; isError = (-not $success); structuredContent = $structured }
}

function Invoke-RenderDocCapture($Arguments) {
    $scene = [string](Get-ArgumentValue $Arguments "scene" "")
    if ([string]::IsNullOrWhiteSpace($scene)) { return @{ text = "[ERROR] 缺少参数 scene"; isError = $true } }
    try { $scene = Resolve-ProjectPath $scene $true } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }

    $captureFrame = [int](Get-ArgumentValue $Arguments "captureFrame" 60)
    $frames = [int](Get-ArgumentValue $Arguments "frames" ([Math]::Max(120, $captureFrame + 30)))
    $fixedDeltaSeconds = [double](Get-ArgumentValue $Arguments "fixedDeltaSeconds" (1.0 / 60.0))
    $timeoutSeconds = [int](Get-ArgumentValue $Arguments "timeoutSeconds" 180)
    $captureWaitSeconds = [int](Get-ArgumentValue $Arguments "captureWaitSeconds" 10)
    if ($captureFrame -lt 1 -or $captureFrame -gt 1000000) { return @{ text = "[ERROR] captureFrame 必须在 1..1000000"; isError = $true } }
    if ($frames -lt $captureFrame -or $frames -gt 1000000) { return @{ text = "[ERROR] frames 必须在 captureFrame..1000000"; isError = $true } }
    if ($fixedDeltaSeconds -le 0.0 -or $fixedDeltaSeconds -gt 0.1) { return @{ text = "[ERROR] fixedDeltaSeconds 必须在 (0, 0.1]"; isError = $true } }
    if ($timeoutSeconds -lt 1 -or $timeoutSeconds -gt 3600) { return @{ text = "[ERROR] timeoutSeconds 必须在 1..3600"; isError = $true } }
    if ($captureWaitSeconds -lt 0 -or $captureWaitSeconds -gt 120) { return @{ text = "[ERROR] captureWaitSeconds 必须在 0..120"; isError = $true } }

    $game = [string](Get-ArgumentValue $Arguments "game" "")
    $extraArgs = @((Get-ArgumentValue $Arguments "extraArgs" @()))
    foreach ($argument in $extraArgs) {
        if ([string]$argument -match '(?i)^--(headless|headless-no-render|frames|fixed-dt|dump-state|crash-log|scene|game|renderdoc-capture-frame|renderdoc-capture-path)(=|$)') {
            return @{ text = "[ERROR] extraArgs 不能覆盖 capture_frame 保留参数: $argument"; isError = $true }
        }
    }

    $captureScript = Join-Path $PSScriptRoot "renderdoc_capture.ps1"
    if (-not (Test-Path -LiteralPath $captureScript -PathType Leaf)) { return @{ text = "[ERROR] RenderDoc 抓帧脚本不存在: $captureScript"; isError = $true } }
    $renderDocCmdPath = [string](Get-ArgumentValue $Arguments "renderDocCmdPath" "")
    $outputRoot = [string](Get-ArgumentValue $Arguments "outputRoot" "")
    $targetArguments = New-Object 'System.Collections.Generic.List[string]'
    [void]$targetArguments.Add("--headless")
    [void]$targetArguments.Add("--frames"); [void]$targetArguments.Add([string]$frames)
    [void]$targetArguments.Add("--fixed-dt"); [void]$targetArguments.Add($fixedDeltaSeconds.ToString("0.########", [System.Globalization.CultureInfo]::InvariantCulture))
    [void]$targetArguments.Add("--no-project-manager")
    [void]$targetArguments.Add("--no-editor")
    foreach ($argument in $extraArgs) { [void]$targetArguments.Add([string]$argument) }
    [void]$targetArguments.Add("--scene"); [void]$targetArguments.Add($scene)
    if (-not [string]::IsNullOrWhiteSpace($game)) { [void]$targetArguments.Add("--game"); [void]$targetArguments.Add($game) }

    $targetArgumentsJson = $targetArguments.ToArray() | ConvertTo-Json -Compress
    $targetArgumentsBase64 = [System.Convert]::ToBase64String(
        [System.Text.Encoding]::UTF8.GetBytes([string]$targetArgumentsJson))
    $command = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $captureScript,
        "-TargetPath", $exePath,
        "-TargetArgumentsBase64", $targetArgumentsBase64
    )
    $command += @(
        "-WorkingDirectory", $exeDir,
        "-CaptureFrame", [string]$captureFrame,
        "-TimeoutSeconds", [string]$timeoutSeconds,
        "-CaptureWaitSeconds", [string]$captureWaitSeconds
    )
    if ($renderDocCmdPath) { $command += @("-RenderDocCmdPath", $renderDocCmdPath) }
    if ($outputRoot) { $command += @("-OutputRoot", $outputRoot) }
    if ([bool](Get-ArgumentValue $Arguments "apiValidation" $false)) { $command += "-ApiValidation" }
    if ([bool](Get-ArgumentValue $Arguments "captureCallstacks" $false)) { $command += "-CaptureCallstacks" }
    if ([bool](Get-ArgumentValue $Arguments "skipThumbnail" $false)) { $command += "-SkipThumbnail" }

    Log "capture_frame: scene=$scene frame=$captureFrame frames=$frames"
    $output = (& powershell @command 2>&1 | Out-String).Trim()
    $exitCode = [int]$LASTEXITCODE
    $marker = @($output -split "\r?\n" | Where-Object { $_ -like "RENDERDOC_RESULT_PATH=*" } | Select-Object -Last 1)
    $resultPath = $null
    if ($marker.Count -gt 0) { $resultPath = $marker[0].Substring("RENDERDOC_RESULT_PATH=".Length).Trim() }
    $structured = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        try { $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json } catch { Log "RenderDoc result parse failed: $($_.Exception.Message)" }
    }
    $success = ($exitCode -eq 0 -and $null -ne $structured -and [bool]$structured.success)
    return @{ text = "=== capture_frame ===`n$(Get-CompactOutput $output)`nexit code: $exitCode (0=已抓帧)"; isError = (-not $success); structuredContent = $structured }
}

function Invoke-NsightCapture($Arguments) {
    $scene = [string](Get-ArgumentValue $Arguments "scene" "")
    if ([string]::IsNullOrWhiteSpace($scene)) { return @{ text = "[ERROR] 缺少参数 scene"; isError = $true } }
    try { $scene = Resolve-ProjectPath $scene $true } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }

    $captureType = [string](Get-ArgumentValue $Arguments "captureType" "gpu_trace")
    $captureFrame = [int](Get-ArgumentValue $Arguments "captureFrame" 60)
    $frames = [int](Get-ArgumentValue $Arguments "frames" ([Math]::Max(180, $captureFrame + 30)))
    $fixedDeltaSeconds = [double](Get-ArgumentValue $Arguments "fixedDeltaSeconds" (1.0 / 60.0))
    $frameCount = [int](Get-ArgumentValue $Arguments "frameCount" 1)
    $maxDurationMilliseconds = [int](Get-ArgumentValue $Arguments "maxDurationMilliseconds" 5000)
    $timeoutSeconds = [int](Get-ArgumentValue $Arguments "timeoutSeconds" 360)
    $traceTimeoutSeconds = [int](Get-ArgumentValue $Arguments "traceTimeoutSeconds" 240)
    $replayLoops = [int](Get-ArgumentValue $Arguments "replayLoops" 3)
    $skipReplay = [bool](Get-ArgumentValue $Arguments "skipReplay" $false)
    $setGpuClocks = [string](Get-ArgumentValue $Arguments "setGpuClocks" "unaltered")
    if ($captureType -notin @("gpu_trace", "graphics_capture")) { return @{ text = "[ERROR] captureType 只支持 gpu_trace 或 graphics_capture"; isError = $true } }
    if ($captureFrame -lt 1 -or $captureFrame -gt 1000000) { return @{ text = "[ERROR] captureFrame 必须在 1..1000000"; isError = $true } }
    if ($frames -le $captureFrame -or $frames -gt 1000000) { return @{ text = "[ERROR] frames 必须大于 captureFrame 且不超过 1000000"; isError = $true } }
    if ($fixedDeltaSeconds -le 0.0 -or $fixedDeltaSeconds -gt 0.1) { return @{ text = "[ERROR] fixedDeltaSeconds 必须在 (0, 0.1]"; isError = $true } }
    if ($frameCount -lt 1 -or $frameCount -gt 60) { return @{ text = "[ERROR] frameCount 必须在 1..60"; isError = $true } }
    if ($maxDurationMilliseconds -lt 1000 -or $maxDurationMilliseconds -gt 600000) { return @{ text = "[ERROR] maxDurationMilliseconds 必须在 1000..600000"; isError = $true } }
    if ($timeoutSeconds -lt 1 -or $timeoutSeconds -gt 3600) { return @{ text = "[ERROR] timeoutSeconds 必须在 1..3600"; isError = $true } }
    if ($traceTimeoutSeconds -lt 1 -or $traceTimeoutSeconds -gt 3600) { return @{ text = "[ERROR] traceTimeoutSeconds 必须在 1..3600"; isError = $true } }
    if ($replayLoops -lt 0 -or $replayLoops -gt 100) { return @{ text = "[ERROR] replayLoops 必须在 0..100"; isError = $true } }
    if ($captureType -eq "graphics_capture" -and -not $skipReplay -and $replayLoops -eq 0) { return @{ text = "[ERROR] graphics_capture 在不使用 skipReplay 时 replayLoops 必须大于 0"; isError = $true } }
    if ($setGpuClocks -notin @("unaltered", "base", "maximum")) { return @{ text = "[ERROR] setGpuClocks 只支持 unaltered、base 或 maximum"; isError = $true } }

    $game = [string](Get-ArgumentValue $Arguments "game" "")
    $extraArgs = @((Get-ArgumentValue $Arguments "extraArgs" @()))
    foreach ($argument in $extraArgs) {
        if ([string]$argument -match '(?i)^--(headless|headless-no-render|frames|fixed-dt|scene|game|crash-log|renderdoc-capture-frame|renderdoc-capture-path)(=|$)') {
            return @{ text = "[ERROR] extraArgs 不能覆盖 capture_performance 保留参数: $argument"; isError = $true }
        }
    }
    $captureScript = Join-Path $PSScriptRoot "nsight_capture.ps1"
    if (-not (Test-Path -LiteralPath $captureScript -PathType Leaf)) { return @{ text = "[ERROR] Nsight 性能采集脚本不存在: $captureScript"; isError = $true } }

    $targetArguments = New-Object 'System.Collections.Generic.List[string]'
    [void]$targetArguments.Add("--headless")
    [void]$targetArguments.Add("--frames"); [void]$targetArguments.Add([string]$frames)
    [void]$targetArguments.Add("--fixed-dt"); [void]$targetArguments.Add($fixedDeltaSeconds.ToString("0.########", [System.Globalization.CultureInfo]::InvariantCulture))
    [void]$targetArguments.Add("--no-project-manager")
    [void]$targetArguments.Add("--no-editor")
    foreach ($argument in $extraArgs) { [void]$targetArguments.Add([string]$argument) }
    [void]$targetArguments.Add("--scene"); [void]$targetArguments.Add((Resolve-ProjectPath $scene $true).Substring($root.Length + 1).Replace('\', '/'))
    if (-not [string]::IsNullOrWhiteSpace($game)) { [void]$targetArguments.Add("--game"); [void]$targetArguments.Add($game) }
    $targetArgumentsJson = $targetArguments.ToArray() | ConvertTo-Json -Compress
    $targetArgumentsBase64 = [System.Convert]::ToBase64String(
        [System.Text.Encoding]::UTF8.GetBytes([string]$targetArgumentsJson))
    $command = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $captureScript,
        "-TargetPath", $exePath,
        "-TargetArgumentsBase64", $targetArgumentsBase64,
        "-WorkingDirectory", $root,
        "-CaptureType", $captureType,
        "-CaptureFrame", [string]$captureFrame,
        "-FrameCount", [string]$frameCount,
        "-MaxDurationMilliseconds", [string]$maxDurationMilliseconds,
        "-TimeoutSeconds", [string]$timeoutSeconds,
        "-TraceTimeoutSeconds", [string]$traceTimeoutSeconds,
        "-ReplayLoops", [string]$replayLoops,
        "-SetGpuClocks", $setGpuClocks
    )
    $nsightPath = [string](Get-ArgumentValue $Arguments "nsightPath" "")
    $outputRoot = [string](Get-ArgumentValue $Arguments "outputRoot" "")
    if ($nsightPath) { $command += @("-NsightPath", $nsightPath) }
    if ($outputRoot) { $command += @("-OutputRoot", $outputRoot) }
    if ($skipReplay) { $command += "-SkipReplay" }

    Log "capture_performance: type=$captureType scene=$scene frame=$captureFrame frames=$frames"
    $output = (& powershell @command 2>&1 | Out-String).Trim()
    $exitCode = [int]$LASTEXITCODE
    $marker = @($output -split "\r?\n" | Where-Object { $_ -like "NSIGHT_RESULT_PATH=*" } | Select-Object -Last 1)
    $resultPath = $null
    if ($marker.Count -gt 0) { $resultPath = $marker[0].Substring("NSIGHT_RESULT_PATH=".Length).Trim() }
    $structured = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        try { $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json } catch { Log "Nsight result parse failed: $($_.Exception.Message)" }
    }
    $success = ($exitCode -eq 0 -and $null -ne $structured -and [bool]$structured.success)
    return @{ text = "=== capture_performance($captureType) ===`n$(Get-CompactOutput $output)`nexit code: $exitCode (0=已生成性能证据)"; isError = (-not $success); structuredContent = $structured }
}

function Invoke-AgentDiscovery($Arguments, [string]$Mode) {
    $discoveryScript = Join-Path $PSScriptRoot "agent_discovery.ps1"
    if (-not (Test-Path -LiteralPath $discoveryScript -PathType Leaf)) { return @{ text = "[ERROR] Agent discovery 脚本不存在: $discoveryScript"; isError = $true } }
    $command = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $discoveryScript,
        "-Mode", $Mode,
        "-RunId", ("mcp-discovery-" + [Guid]::NewGuid().ToString("N").Substring(0, 12))
    )
    $outputRoot = [string](Get-ArgumentValue $Arguments "outputRoot" "out\agent_discovery")
    $maxResults = [int](Get-ArgumentValue $Arguments "maxResults" 200)
    if ($maxResults -lt 1 -or $maxResults -gt 2000) { return @{ text = "[ERROR] maxResults 必须在 1..2000"; isError = $true } }
    $command += @("-OutputRoot", $outputRoot, "-MaxResults", [string]$maxResults)
    if ($Mode -eq "device") {
        $renderDocPath = [string](Get-ArgumentValue $Arguments "renderDocPath" "")
        $nsightPath = [string](Get-ArgumentValue $Arguments "nsightPath" "")
        $vulkanInfoPath = [string](Get-ArgumentValue $Arguments "vulkanInfoPath" "")
        if (-not [string]::IsNullOrWhiteSpace($renderDocPath)) { $command += @("-RenderDocPath", $renderDocPath) }
        if (-not [string]::IsNullOrWhiteSpace($nsightPath)) { $command += @("-NsightPath", $nsightPath) }
        if (-not [string]::IsNullOrWhiteSpace($vulkanInfoPath)) { $command += @("-VulkanInfoPath", $vulkanInfoPath) }
    }
    $projectPath = [string](Get-ArgumentValue $Arguments "projectPath" "")
    if ($Mode -eq "context" -and [string]::IsNullOrWhiteSpace($projectPath)) { return @{ text = "[ERROR] get_project_context 缺少参数 projectPath"; isError = $true } }
    if ($projectPath) {
        try {
            $projectFull = Resolve-ProjectPath $projectPath $false
            if (-not (Test-Path -LiteralPath $projectFull -PathType Container)) { throw "项目目录不存在: $projectFull" }
        } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
        $command += @("-ProjectPath", $projectPath)
    }
    if ($Mode -eq "scene") {
        $scene = [string](Get-ArgumentValue $Arguments "scene" (Get-ArgumentValue $Arguments "scenePath" ""))
        if ([string]::IsNullOrWhiteSpace($scene)) { return @{ text = "[ERROR] inspect_scene 缺少参数 scene"; isError = $true } }
        if (-not $projectPath) {
            try { [void](Resolve-ProjectPath $scene $true) } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
        }
        $maxEntities = [int](Get-ArgumentValue $Arguments "maxEntities" 500)
        $maxReferences = [int](Get-ArgumentValue $Arguments "maxAssetReferences" 200)
        if ($maxEntities -lt 1 -or $maxEntities -gt 5000) { return @{ text = "[ERROR] maxEntities 必须在 1..5000"; isError = $true } }
        if ($maxReferences -lt 1 -or $maxReferences -gt 5000) { return @{ text = "[ERROR] maxAssetReferences 必须在 1..5000"; isError = $true } }
        $command += @("-ScenePath", $scene, "-MaxEntities", [string]$maxEntities, "-MaxAssetReferences", [string]$maxReferences)
    } elseif ($Mode -eq "assets" -or $Mode -eq "context") {
        $query = [string](Get-ArgumentValue $Arguments "query" "")
        $assetType = [string](Get-ArgumentValue $Arguments "assetType" "all")
        if ($assetType -notin @("all", "scenes", "scripts", "shaders", "models", "textures", "audio")) { return @{ text = "[ERROR] assetType 不支持: $assetType"; isError = $true } }
        if (-not [string]::IsNullOrWhiteSpace($query)) { $command += @("-Query", $query) }
        $command += @("-AssetType", $assetType)
    }
    Log "agent_discovery: mode=$Mode"
    $output = (& powershell @command 2>&1 | Out-String).Trim()
    $exitCode = [int]$LASTEXITCODE
    $marker = @($output -split "\r?\n" | Where-Object { $_ -like "DISCOVERY_RESULT_PATH=*" } | Select-Object -Last 1)
    $resultPath = $null
    if ($marker.Count -gt 0) { $resultPath = $marker[0].Substring("DISCOVERY_RESULT_PATH=".Length).Trim() }
    $structured = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        try { $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json } catch { Log "discovery result parse failed: $($_.Exception.Message)" }
    }
    $success = ($exitCode -eq 0 -and $null -ne $structured -and [bool](Get-ArgumentValue $structured "success" $false))
    return @{ text = "=== $Mode discovery ===`n$(Get-CompactOutput $output)`nexit code: $exitCode (0=已生成上下文)"; isError = (-not $success); structuredContent = $structured }
}

function Invoke-AgentEvaluate($Arguments) {
    $evidencePath = [string](Get-ArgumentValue $Arguments "evidencePath" "")
    if ([string]::IsNullOrWhiteSpace($evidencePath)) { return @{ text = "[ERROR] 缺少参数 evidencePath"; isError = $true } }
    try { [void](Resolve-ProjectPath $evidencePath $true) } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
    $contractPath = [string](Get-ArgumentValue $Arguments "contractPath" "")
    $contract = Get-ArgumentValue $Arguments "contract" $null
    if ($contractPath -and $null -ne $contract) { return @{ text = "[ERROR] contractPath 与 contract 只能二选一"; isError = $true } }
    $evaluateScript = Join-Path $PSScriptRoot "agent_evaluate.ps1"
    if (-not (Test-Path -LiteralPath $evaluateScript -PathType Leaf)) { return @{ text = "[ERROR] Agent evaluator 脚本不存在: $evaluateScript"; isError = $true } }
    $command = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $evaluateScript,
        "-EvidencePath", $evidencePath,
        "-RunId", ("mcp-evaluation-" + [Guid]::NewGuid().ToString("N").Substring(0, 12))
    )
    $outputRoot = [string](Get-ArgumentValue $Arguments "outputRoot" "out\agent_evaluations")
    $command += @("-OutputRoot", $outputRoot)
    if ($contractPath) {
        try { [void](Resolve-ProjectPath $contractPath $true) } catch { return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true } }
        $command += @("-ContractPath", $contractPath)
    } elseif ($null -ne $contract) {
        $contractJson = $contract | ConvertTo-Json -Depth 30 -Compress
        $contractBase64 = [System.Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes([string]$contractJson))
        $command += @("-ContractJsonBase64", $contractBase64)
    }
    Log "agent_evaluate: evidence=$evidencePath"
    $output = (& powershell @command 2>&1 | Out-String).Trim()
    $exitCode = [int]$LASTEXITCODE
    $marker = @($output -split "\r?\n" | Where-Object { $_ -like "EVALUATION_RESULT_PATH=*" } | Select-Object -Last 1)
    $resultPath = $null
    if ($marker.Count -gt 0) { $resultPath = $marker[0].Substring("EVALUATION_RESULT_PATH=".Length).Trim() }
    $structured = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        try { $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json } catch { Log "evaluation result parse failed: $($_.Exception.Message)" }
    }
    $success = ($exitCode -eq 0 -and $null -ne $structured -and [bool](Get-ArgumentValue $structured "success" $false))
    return @{ text = "=== agent_evaluate ===`n$(Get-CompactOutput $output)`nexit code: $exitCode (0=门禁通过)"; isError = (-not $success); structuredContent = $structured }
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

function Invoke-SceneCommands($Arguments) {
    $scene = [string](Get-ArgumentValue $Arguments "scene" "")
    if ([string]::IsNullOrWhiteSpace($scene)) {
        return @{ text = "[ERROR] 缺少参数 scene"; isError = $true }
    }
    try { $scene = Resolve-ProjectPath $scene $true } catch {
        return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true }
    }

    $commandsValue = Get-ArgumentValue $Arguments "commands" $null
    $commandsPathValue = [string](Get-ArgumentValue $Arguments "commandsPath" "")
    if ($null -ne $commandsValue -and $commandsPathValue) {
        return @{ text = "[ERROR] commands 与 commandsPath 只能二选一"; isError = $true }
    }
    if ($null -eq $commandsValue -and [string]::IsNullOrWhiteSpace($commandsPathValue)) {
        return @{ text = "[ERROR] 必须提供 commands 数组或 commandsPath"; isError = $true }
    }

    $requestDir = $null
    if ($null -ne $commandsValue) {
        $requestId = (Get-Date -Format "yyyyMMdd-HHmmssfff") + "-" + ([Guid]::NewGuid().ToString("N").Substring(0, 8))
        $requestDir = Join-Path $runsRoot ("scene_command_request-" + $requestId)
        New-Item -ItemType Directory -Path $requestDir -Force | Out-Null
        $commandsPath = Join-Path $requestDir "commands.json"
        $commandDocument = [ordered]@{
            schemaVersion = 1
            commands = @($commandsValue)
        }
        [System.IO.File]::WriteAllText(
            $commandsPath,
            ($commandDocument | ConvertTo-Json -Depth 40),
            $utf8NoBom)
    } else {
        try { $commandsPath = Resolve-ProjectPath $commandsPathValue $true } catch {
            return @{ text = "[ERROR] $($_.Exception.Message)"; isError = $true }
        }
    }

    $sceneCommandScript = Join-Path $PSScriptRoot "scene_command.ps1"
    if (-not (Test-Path -LiteralPath $sceneCommandScript -PathType Leaf)) {
        return @{ text = "[ERROR] 场景命令执行器不存在: $sceneCommandScript"; isError = $true }
    }
    $testLayer = [string](Get-ArgumentValue $Arguments "testLayer" "none")
    if ($testLayer -notin @("none", "validate", "gameplay", "render", "all")) {
        return @{ text = "[ERROR] 不支持的 testLayer: $testLayer"; isError = $true }
    }
    $testTimeoutSeconds = [int](Get-ArgumentValue $Arguments "testTimeoutSeconds" 120)
    $buildTimeoutSeconds = [int](Get-ArgumentValue $Arguments "buildTimeoutSeconds" 600)
    if ($testTimeoutSeconds -lt 1 -or $testTimeoutSeconds -gt 3600) {
        return @{ text = "[ERROR] testTimeoutSeconds 必须在 1..3600"; isError = $true }
    }
    if ($buildTimeoutSeconds -lt 1 -or $buildTimeoutSeconds -gt 7200) {
        return @{ text = "[ERROR] buildTimeoutSeconds 必须在 1..7200"; isError = $true }
    }

    $command = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $sceneCommandScript,
        "-ScenePath", $scene,
        "-CommandsPath", $commandsPath,
        "-TestLayer", $testLayer,
        "-TestTimeoutSeconds", [string]$testTimeoutSeconds,
        "-BuildTimeoutSeconds", [string]$buildTimeoutSeconds
    )
    $outputPath = [string](Get-ArgumentValue $Arguments "outputPath" "")
    if ($outputPath) { $command += @("-OutputPath", $outputPath) }
    if ([bool](Get-ArgumentValue $Arguments "inPlace" $false)) { $command += "-InPlace" }
    if ([bool](Get-ArgumentValue $Arguments "checkAssets" $true)) { $command += "-CheckAssets" }
    if ([bool](Get-ArgumentValue $Arguments "skipTestBuild" $false)) { $command += "-SkipTestBuild" }

    Log "apply_scene_commands: scene=$scene commands=$commandsPath"
    $output = (& powershell @command 2>&1 | Out-String)
    $exitCode = [int]$LASTEXITCODE
    $resultMarker = @($output -split '\r?\n' | Where-Object { $_ -like "RESULT_PATH=*" } | Select-Object -Last 1)
    $resultPath = $null
    if ($resultMarker.Count -gt 0) {
        $resultPath = $resultMarker[0].Substring("RESULT_PATH=".Length).Trim()
    }
    $structured = $null
    if ($resultPath -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        try {
            $structured = [System.IO.File]::ReadAllText($resultPath, $utf8NoBom) | ConvertFrom-Json
        } catch {
            Log "apply_scene_commands result parse failed: $($_.Exception.Message)"
        }
    }
    $success = ($exitCode -eq 0 -and $null -ne $structured -and [bool]$structured.success)
    $text = "=== apply_scene_commands ===" + [System.Environment]::NewLine +
        (Get-CompactOutput $output) + [System.Environment]::NewLine +
        "exit code: $exitCode (0=通过)"
    return @{
        text = $text
        isError = (-not $success)
        structuredContent = $structured
    }
}

function Test-ReservedExtraArgument([string]$Argument) {
    return $Argument -match '^--(headless|headless-no-render|frames|fixed-dt|dump-state|crash-log|scene|game|screenshot-frame|screenshot-path)(=|$)'
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
    $screenshotFrame = [int](Get-ArgumentValue $Arguments "screenshotFrame" 0)

    if ([string]::IsNullOrWhiteSpace($scene)) { return @{ text = "[ERROR] 缺少参数 scene"; isError = $true } }
    if ($frames -lt 1 -or $frames -gt 1000000) { return @{ text = "[ERROR] frames 必须在 1..1000000"; isError = $true } }
    if ($timeoutMs -lt 1000 -or $timeoutMs -gt 3600000) { return @{ text = "[ERROR] timeoutMs 必须在 1000..3600000"; isError = $true } }
    if ($fixedDeltaSeconds -le 0.0 -or $fixedDeltaSeconds -gt 0.1) { return @{ text = "[ERROR] fixedDeltaSeconds 必须在 (0, 0.1]"; isError = $true } }
    if ($screenshotFrame -lt 0 -or $screenshotFrame -gt 1000000) { return @{ text = "[ERROR] screenshotFrame 必须在 0..1000000"; isError = $true } }
    if (-not (Test-Path -LiteralPath $testExecutable)) {
        $target = if ($isGameplay) { "MikanTestRunner" } else { "EngineMain" }
        return @{ text = "[ERROR] 测试入口不存在: $testExecutable（先调用 build target=$target）"; isError = $true }
    }
    if ($isGameplay -and $screenshotFrame -gt 0) { return @{ text = "[ERROR] screenshotFrame 只支持渲染层测试"; isError = $true } }
    if ($screenshotFrame -gt $frames) { return @{ text = "[ERROR] screenshotFrame 必须不大于 frames"; isError = $true } }
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
    $screenshotPath = ""
    $screenshotMetadataPath = ""
    if (-not $isGameplay -and $screenshotFrame -gt 0) {
        $screenshotPath = Join-Path $runDir ("frame-{0:D4}.png" -f $screenshotFrame)
        $screenshotMetadataPath = [System.IO.Path]::ChangeExtension($screenshotPath, ".json")
    }

    $argumentList = [System.Collections.Generic.List[string]]::new()
    if (-not $isGameplay) { $argumentList.Add("--headless") }
    $argumentList.Add("--frames"); $argumentList.Add([string]$frames)
    $argumentList.Add("--fixed-dt"); $argumentList.Add($fixedDeltaSeconds.ToString("0.########", [System.Globalization.CultureInfo]::InvariantCulture))
    $argumentList.Add("--dump-state"); $argumentList.Add($dumpPath)
    $argumentList.Add("--crash-log"); $argumentList.Add($crashPath)
    if (-not $isGameplay -and $screenshotFrame -gt 0) {
        $argumentList.Add("--screenshot-frame"); $argumentList.Add([string]$screenshotFrame)
        $argumentList.Add("--screenshot-path"); $argumentList.Add($screenshotPath)
    }
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
                if (-not [bool](Get-ArgumentValue $screenshotSummary "runtimeReady" $false)) {
                    $failed = $true
                    $diagnostics += "截图对应运行时尚未 ready: $([string](Get-ArgumentValue $screenshotSummary 'readinessStatus' 'unknown'))"
                }
                $visualStatus = [string](Get-ArgumentValue $screenshotSummary "visualStatus" "")
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
            $dumpSummary = @{ frames = $dump.frames; entityCount = $dump.entity_count; game = $dump.game; runtimeLayer = $dump.runtime_layer }
            $script:lastDumpPath = $dumpPath
        } catch { $failed = $true; $diagnostics += "[MCP] dump JSON 解析失败: $($_.Exception.Message)" }
    }
    $artifactRecords = @()
    $artifactItems = @(
        @($stdoutPath, "stdout"), @($stderrPath, "stderr"), @($dumpPath, "state_dump"), @($crashPath, "crash_log"),
        @($screenshotPath, "engine_screenshot"), @($screenshotMetadataPath, "engine_screenshot_metadata")
    )
    foreach ($item in $artifactItems) {
        $artifactPath = [string]$item[0]
        if ([string]::IsNullOrWhiteSpace($artifactPath) -or -not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) { continue }
        $artifactFile = Get-Item -LiteralPath $artifactPath
        $artifactRecords += [ordered]@{
            path = $artifactPath
            kind = [string]$item[1]
            size = [int64]$artifactFile.Length
            sha256 = Get-Sha256 $artifactPath
        }
    }
    $resultObject = [ordered]@{
        runId = $runId; startedAt = $startTime.ToString("o"); durationMs = [Math]::Round($stopwatch.Elapsed.TotalMilliseconds)
        layer = $Layer; executable = $testExecutable; scene = $scene; game = $game; frames = $frames; fixedDeltaSeconds = $fixedDeltaSeconds; render = $render
        exitCode = $exitCode; timedOut = $timedOut; success = (-not $failed); dumpPath = $dumpPath
        stdoutPath = $stdoutPath; stderrPath = $stderrPath; crashPath = $crashPath
        screenshotFrame = $screenshotFrame
        screenshotPath = if ($screenshotExists) { $screenshotPath } else { "" }
        screenshotMetadataPath = if ($screenshotMetadataExists) { $screenshotMetadataPath } else { "" }
        screenshot = $screenshotSummary
        dump = $dumpSummary
        diagnostics = @($diagnostics)
        artifacts = @($artifactRecords)
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
    if ($screenshotFrame -gt 0) { [void]$builder.AppendLine("screenshot: $(if ($screenshotExists) {$screenshotPath} else {'未生成'})") }
    if ($crashExists) { [void]$builder.AppendLine("crash: $crashPath") }
    foreach ($line in @($diagnostics | Select-Object -Last 80)) { [void]$builder.AppendLine("  $line") }
    return @{ text = $builder.ToString().TrimEnd(); isError = $failed; structuredContent = $resultObject }
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
        name = "inspect_project"; description = "只读发现项目能力、CMake 构建入口、场景 Schema、组件键以及受限项目资产索引，为 Agent Planner 提供稳定上下文。"
        inputSchema = @{ type = "object"; properties = @{ projectPath = @{ type = "string"; description = "可选；项目目录，例如 projects/third-person-combat" }; query = @{ type = "string"; description = "可选；对选定项目的语义资产查询" }; assetType = @{ type = "string"; enum = @("all", "scenes", "scripts", "shaders", "models", "textures", "audio"); default = "all" }; maxResults = @{ type = "integer"; minimum = 1; maximum = 2000; default = 200 }; outputRoot = @{ type = "string" } } }
    },
    @{
        name = "get_project_context"; description = "只读读取项目 manifest、资源根、默认场景、场景资源引用和语义资产索引；返回项目相对路径，供 Agent 规划场景、玩法和测试。"
        inputSchema = @{ type = "object"; properties = @{ projectPath = @{ type = "string"; description = "项目目录，例如 projects/third-person-combat" }; query = @{ type = "string"; description = "可选；按路径、名称、semanticRole 或标签检索素材" }; assetType = @{ type = "string"; enum = @("all", "scenes", "scripts", "shaders", "models", "textures", "audio"); default = "all" }; maxResults = @{ type = "integer"; minimum = 1; maximum = 2000; default = 200 }; outputRoot = @{ type = "string" } }; required = @("projectPath") }
    },
    @{
        name = "get_device_capabilities"; description = "只读探测 Windows 主机、Vulkan 设备、RenderDoc/Nsight 工具和引擎构建产物；不提权、不修改驱动，未执行性能采集时 permission 保持 not_tested。"
        inputSchema = @{ type = "object"; properties = @{ renderDocPath = @{ type = "string"; description = "可选；qrenderdoc.exe 或 RenderDoc 目录" }; nsightPath = @{ type = "string"; description = "可选；ngfx-ui.exe 或 Nsight host 目录" }; vulkanInfoPath = @{ type = "string"; description = "可选；vulkaninfo.exe 路径" }; outputRoot = @{ type = "string" } } }
    },
    @{
        name = "inspect_scene"; description = "只读解析场景 JSON，返回实体层级、组件统计、脚本名、资源引用和 Schema 结构摘要；不会修改场景。"
        inputSchema = @{ type = "object"; properties = @{ projectPath = @{ type = "string"; description = "可选；项目目录，允许 scene 使用项目相对路径" }; scene = @{ type = "string" }; scenePath = @{ type = "string" }; maxResults = @{ type = "integer"; minimum = 1; maximum = 2000; default = 200 }; maxEntities = @{ type = "integer"; minimum = 1; maximum = 5000; default = 500 }; maxAssetReferences = @{ type = "integer"; minimum = 1; maximum = 5000; default = 200 }; outputRoot = @{ type = "string" } }; required = @("scene") }
    },
    @{
        name = "query_assets"; description = "只读按名称/路径和类型查询项目资产，返回受限结果、大小、修改时间和 SHA-256；不扫描 out、backup、第三方依赖或移动端目录。"
        inputSchema = @{ type = "object"; properties = @{ projectPath = @{ type = "string"; description = "可选；指定项目后使用 project.json 白名单和语义索引" }; query = @{ type = "string"; description = "可选的路径/文件名/语义角色关键词" }; assetType = @{ type = "string"; enum = @("all", "scenes", "scripts", "shaders", "models", "textures", "audio"); default = "all" }; maxResults = @{ type = "integer"; minimum = 1; maximum = 2000; default = 200 }; outputRoot = @{ type = "string" } } }
    },
    @{
        name = "evaluate_agent_result"; description = "只读读取 agent_evidence 并按 contract 判定行为、Discovery、视觉和 Nsight 性能门槛；缺失证据或阈值不满足时返回失败，不修改门槛。"
        inputSchema = @{ type = "object"; properties = @{
            evidencePath = @{ type = "string"; description = "项目目录内的 evidence.json" }
            contractPath = @{ type = "string"; description = "可选；项目目录内的 evaluation contract JSON，与 contract 二选一" }
            contract = @{ type = "object"; description = "可选内联 contract，与 contractPath 二选一；省略时默认要求 source.targetSuccess=true" }
            outputRoot = @{ type = "string"; description = "可选；项目目录内的 evaluator 输出根目录，默认 out/agent_evaluations" }
        }; required = @("evidencePath") }
    },
    @{
        name = "run_agent_game_spec"; description = "执行结构化 AI Native GameSpec：将模型输出的项目、素材、场景命令、玩法脚本、测试和交付规格编译为受控 plan/workflow，默认 preview，execute 后生成证据、验收结果和 delivery.zip。"
        inputSchema = @{ type = "object"; properties = @{
            gameSpec = @{ type = "object"; description = "内联 GameSpec JSON；与 gameSpecPath 二选一" }
            gameSpecPath = @{ type = "string"; description = "项目目录内的 GameSpec JSON；与 gameSpec 二选一" }
            mode = @{ type = "string"; enum = @("preview", "execute"); description = "preview 只编译和校验，不启动引擎；execute 才执行构建、测试和交付"; default = "preview" }
            runId = @{ type = "string"; description = "可选运行 id；必须唯一且只含字母、数字、点、下划线和短横线" }
            outputRoot = @{ type = "string"; description = "可选；项目目录内 GameSpec 输出根目录，默认 out/agent_game_specs" }
        }; required = @() }
    },
    @{
        name = "run_agent_test"; description = "执行只测试型 AI Native TestSpec：只读发现、场景校验、构建、玩法/渲染测试、状态断言、RenderDoc/Nsight 证据和 Evaluation；不接受场景写入、脚本源码或任意命令。默认 preview。"
        inputSchema = @{ type = "object"; properties = @{
            testSpec = @{ type = "object"; description = "内联只测试 TestSpec；与 testSpecPath 二选一" }
            testSpecPath = @{ type = "string"; description = "项目目录内的 agent_test.schema.json 规格；与 testSpec 二选一" }
            mode = @{ type = "string"; enum = @("preview", "execute"); description = "preview 只生成测试计划；execute 才启动构建/引擎/采集"; default = "preview" }
            runId = @{ type = "string"; description = "可选运行 id；必须唯一且只含字母、数字、点、下划线和短横线" }
            outputRoot = @{ type = "string"; description = "可选；项目目录内测试输出根目录，默认 out/agent_tests" }
        }; required = @() }
    },
    @{
        name = "run_agent_repair"; description = "对失败的 agent_test 结果执行受控参数修复：只允许重跑测试 workflow 的白名单参数，不修改场景、脚本、源码或断言期望值。默认 preview。"
        inputSchema = @{ type = "object"; properties = @{
            sourceResultPath = @{ type = "string"; description = "失败 agent_test 的 result.json" }
            repair = @{ type = "object"; description = "内联 agent_repair.schema.json；与 repairPath 二选一" }
            repairPath = @{ type = "string"; description = "项目目录内的 agent_repair.schema.json 规格；与 repair 二选一" }
            mode = @{ type = "string"; enum = @("preview", "execute"); description = "preview 只生成候选 workflow；execute 才重跑测试"; default = "preview" }
            maxAttempts = @{ type = "integer"; minimum = 1; maximum = 5 }
            runId = @{ type = "string"; description = "可选运行 id；必须唯一且只含字母、数字、点、下划线和短横线" }
            outputRoot = @{ type = "string"; description = "可选；项目目录内修复输出根目录，默认 out/agent_repairs" }
        }; required = @("sourceResultPath") }
    },
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
        name = "create_script"; description = "生成或写入受控玩法脚本：使用 ECS::ScriptContext，限制脚本目录/include/危险调用，并返回源码哈希；可选编译插件。"
        inputSchema = @{ type = "object"; properties = @{
            scriptName = @{ type = "string"; pattern = "^[A-Za-z][A-Za-z0-9_]{0,63}$" }
            className = @{ type = "string"; pattern = "^[A-Za-z][A-Za-z0-9_]{0,63}$" }
            outputPath = @{ type = "string"; description = "必须位于 games/<plugin>/ 或 projects/<project>/games/ 下，且为 .cpp" }
            fields = @{ type = "array"; maxItems = 32; items = @{ type = "object"; properties = @{
                name = @{ type = "string"; pattern = "^[A-Za-z][A-Za-z0-9_]{0,63}$" }
                type = @{ type = "string"; enum = @("bool", "int", "float", "vec2", "vec3", "string") }
                label = @{ type = "string" }
                default = @{}
            }; required = @("name", "type") } }
            source = @{ type = "string"; maxLength = 200000; description = "可选；完整源码，必须包含 IScriptBehaviour 和 REGISTER_SCRIPT(...)" }
            overwrite = @{ type = "boolean"; description = "覆盖前生成 out/script_backups 备份"; "default" = $false }
            compile = @{ type = "boolean"; description = "写入后编译 games/ 插件"; "default" = $false }
        }; required = @("scriptName", "outputPath") }
    },
    @{
        name = "run_agent_workflow"; description = "执行声明式 Agent 工作流：按依赖串联脚本生成、构建、场景命令、玩法/渲染测试、dump 读取和状态断言；每次运行保留步骤日志、结果和 manifest。"
        inputSchema = @{ type = "object"; properties = @{
            workflow = @{ type = "object"; description = "内联 workflow JSON；与 workflowPath 二选一" }
            workflowPath = @{ type = "string"; description = "项目目录内的 workflow JSON；与 workflow 二选一" }
            outputRoot = @{ type = "string"; description = "可选；项目目录内的运行输出根目录，默认 out/agent_runs" }
            dryRun = @{ type = "boolean"; description = "只校验并输出执行计划，不调用外部工具"; "default" = $false }
            continueOnFailure = @{ type = "boolean"; description = "允许无依赖步骤在失败后继续；默认遇到失败停止"; "default" = $false }
        } }
    },
    @{
        name = "run_agent_task"; description = "执行带失败诊断和候选修复重跑的 Agent 任务；默认 preview，候选只能修改受控 workflow action 参数，execute 后保留每次尝试和 manifest。"
        inputSchema = @{ type = "object"; properties = @{
            task = @{ type = "object"; description = "内联 task JSON；与 taskPath 二选一" }
            taskPath = @{ type = "string"; description = "项目目录内的 agent task JSON；与 task 二选一" }
            mode = @{ type = "string"; enum = @("preview", "execute"); description = "preview 只校验计划；execute 才执行基础 workflow 和候选重跑"; "default" = "preview" }
            maxAttempts = @{ type = "integer"; minimum = 1; maximum = 5; description = "可选，覆盖 task.maxAttempts" }
            outputRoot = @{ type = "string"; description = "可选；项目目录内的任务输出根目录，默认 out/agent_tasks" }
        } }
    },
    @{
        name = "run_agent_plan"; description = "执行模型/人工提交的高层 Agent plan：调用受控 task，再生成结构化 evidence，返回目标、任务、平台和视觉证据索引。默认 preview。"
        inputSchema = @{ type = "object"; properties = @{
            plan = @{ type = "object"; description = "内联 plan JSON；与 planPath 二选一" }
            planPath = @{ type = "string"; description = "项目目录内的 Agent plan JSON；与 plan 二选一" }
            mode = @{ type = "string"; enum = @("preview", "execute"); description = "preview 不执行引擎；execute 显式运行 task"; "default" = "preview" }
            maxAttempts = @{ type = "integer"; minimum = 1; maximum = 5 }
            outputRoot = @{ type = "string"; description = "可选；项目目录内的 Planner 输出根目录，默认 out/agent_plans" }
        } }
    },
    @{
        name = "collect_agent_evidence"; description = "只读采集 agent_task/agent_workflow 结果的失败、日志、构建产物、Android adb 和视觉证据；不执行引擎、不安装 APK、不改源码。"
        inputSchema = @{ type = "object"; properties = @{
            resultPath = @{ type = "string"; description = "项目目录内的 task/workflow result.json" }
            outputRoot = @{ type = "string"; description = "可选；项目目录内的 evidence 输出根目录，默认 out/agent_evidence" }
        }; required = @("resultPath") }
    },
    @{
        name = "capture_frame"; description = "桌面端 RenderDoc 自动抓帧：用 renderdoccmd 注入 EngineMain，在指定 Present 前触发 RenderDoc，保存 .rdc、缩略图、日志和 manifest。默认要求 execute 环境已有 64 位 RenderDoc。"
        inputSchema = @{ type = "object"; properties = @{
            scene = @{ type = "string"; description = "项目目录内的场景 JSON" }
            game = @{ type = "string"; description = "可选游戏插件名" }
            captureFrame = @{ type = "integer"; minimum = 1; maximum = 1000000; "default" = 60; description = "一基、与目标 Present 对齐的帧号" }
            frames = @{ type = "integer"; minimum = 1; maximum = 1000000; description = "目标总帧数，必须不小于 captureFrame；省略时至少保留 30 帧余量" }
            fixedDeltaSeconds = @{ type = "number"; exclusiveMinimum = 0; maximum = 0.1; "default" = 0.016666667 }
            extraArgs = @{ type = "array"; items = @{ type = "string" }; description = "可选引擎参数；不能覆盖场景、帧数、RenderDoc 保留参数" }
            timeoutSeconds = @{ type = "integer"; minimum = 1; maximum = 3600; "default" = 180 }
            captureWaitSeconds = @{ type = "integer"; minimum = 0; maximum = 120; "default" = 10 }
            renderDocCmdPath = @{ type = "string"; description = "可选 renderdoccmd.exe 绝对路径；也可使用 PATH/RENDERDOC_CMD_PATH" }
            outputRoot = @{ type = "string"; description = "可选，项目目录内输出根目录，默认 out/renderdoc_captures" }
            apiValidation = @{ type = "boolean"; "default" = $false }
            captureCallstacks = @{ type = "boolean"; "default" = $false }
            skipThumbnail = @{ type = "boolean"; "default" = $false }
        }; required = @("scene") }
    },
    @{
        name = "capture_performance"; description = "桌面端 Nsight Graphics 性能采集：支持 GPU Trace 或 Graphics Capture，自动保存报告、导出指标、replay CSV、日志和 manifest。GPU Trace 需要管理员权限或 NVIDIA GPU performance counters 访问权限。"
        inputSchema = @{ type = "object"; properties = @{
            scene = @{ type = "string"; description = "项目目录内的场景 JSON" }
            game = @{ type = "string"; description = "可选游戏插件名" }
            captureType = @{ type = "string"; enum = @("gpu_trace", "graphics_capture"); "default" = "gpu_trace"; description = "gpu_trace=导出 GPU counters/硬件指标；graphics_capture=保存 .ngfx-capture 并可 replay" }
            captureFrame = @{ type = "integer"; minimum = 1; maximum = 1000000; "default" = 60; description = "一基、必须早于目标 --frames 结束帧" }
            frames = @{ type = "integer"; minimum = 1; maximum = 1000000; description = "目标总帧数，必须大于 captureFrame" }
            fixedDeltaSeconds = @{ type = "number"; exclusiveMinimum = 0; maximum = 0.1; "default" = 0.016666667 }
            frameCount = @{ type = "integer"; minimum = 1; maximum = 60; "default" = 1; description = "GPU Trace/Graphics Capture 连续采集帧数" }
            maxDurationMilliseconds = @{ type = "integer"; minimum = 1000; maximum = 600000; "default" = 5000; description = "GPU Trace 每次采样窗口上限" }
            timeoutSeconds = @{ type = "integer"; minimum = 1; maximum = 3600; "default" = 360 }
            traceTimeoutSeconds = @{ type = "integer"; minimum = 1; maximum = 3600; "default" = 240 }
            replayLoops = @{ type = "integer"; minimum = 0; maximum = 100; "default" = 3; description = "graphics_capture 的 replay 次数" }
            skipReplay = @{ type = "boolean"; "default" = $false }
            nsightPath = @{ type = "string"; description = "可选 gfx-ui.exe/ngfx.exe 所在路径；也可使用 NSIGHT_GRAPHICS_PATH" }
            setGpuClocks = @{ type = "string"; enum = @("unaltered", "base", "maximum"); "default" = "unaltered" }
            outputRoot = @{ type = "string"; description = "可选，项目目录内输出根目录，默认 out/nsight_captures" }
            extraArgs = @{ type = "array"; items = @{ type = "string" }; description = "可选引擎参数；不能覆盖场景、帧数和 crash-log 保留参数" }
        }; required = @("scene") }
    },
    @{
        name = "validate_scene"; description = "离线检查场景 JSON、实体 id/parent、transform 数值、组件字段和可选资源引用。"
        inputSchema = @{ type = "object"; properties = @{ scenePath = @{ type = "string" }; checkAssets = @{ type = "boolean"; "default" = $false } }; required = @("scenePath") }
    },
    @{
        name = "apply_scene_commands"; description = "应用结构化场景命令：创建/删除/重命名实体、修改变换和组件、维护父子层级；默认生成副本并先校验，可选自动跑玩法或 Vulkan 回归。"
        inputSchema = @{ type = "object"; properties = @{
            scene = @{ type = "string" }
            commands = @{ type = "array"; minItems = 1; maxItems = 500; items = @{ type = "object" } }
            commandsPath = @{ type = "string"; description = "可选；项目目录内的 scene commands JSON 文件。与 commands 二选一。" }
            outputPath = @{ type = "string"; description = "可选；项目目录内的输出场景路径。省略则写入独立 scene_commands 运行目录。" }
            inPlace = @{ type = "boolean"; description = "是否覆盖源场景；覆盖前自动备份 source.before.json。"; "default" = $false }
            checkAssets = @{ type = "boolean"; "default" = $true }
            testLayer = @{ type = "string"; enum = @("none", "validate", "gameplay", "render", "all"); "default" = "none" }
            skipTestBuild = @{ type = "boolean"; "default" = $false }
            testTimeoutSeconds = @{ type = "integer"; minimum = 1; maximum = 3600; "default" = 120 }
            buildTimeoutSeconds = @{ type = "integer"; minimum = 1; maximum = 7200; "default" = 600 }
        }; required = @("scene", "commands") }
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
        name = "run_render_test"; description = "运行渲染层回归测试：使用 EngineMain 完整初始化 SDL Video、Vulkan、FrameRender 与 Present，并输出独立日志、状态 dump 和可供 Agent 检查的 PNG/JSON 截图证据。"
        inputSchema = @{ type = "object"; properties = @{
            scene = @{ type = "string" }; game = @{ type = "string" }
            frames = @{ type = "integer"; minimum = 1; maximum = 1000000; "default" = 120 }
            fixedDeltaSeconds = @{ type = "number"; exclusiveMinimum = 0; maximum = 0.1; "default" = 0.016666667 }
            screenshotFrame = @{ type = "integer"; minimum = 0; maximum = 1000000; "default" = 0; description = "0=不截图；大于 0 时在目标渲染帧导出 PNG 与同名 JSON 元数据，并检查 runtimeReady/黑屏状态" }
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
            screenshotFrame = @{ type = "integer"; minimum = 0; maximum = 1000000; "default" = 0; description = "仅 render=true 生效；0=不截图，否则导出 PNG+JSON 并检查画面就绪状态" }
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
            "initialize" { Send-Result $id @{ protocolVersion = "2025-03-26"; capabilities = @{ tools = @{ listChanged = $false } }; serverInfo = @{ name = "mikanengine-mcp"; version = "3.12.0" } } }
            "ping" { Send-Result $id @{} }
            "tools/list" { Send-Result $id @{ tools = $tools } }
            "tools/call" {
                if ($null -eq $params -or -not (Test-HasProperty $params "name")) { Send-Error $id -32602 "tools/call requires params.name"; continue }
                $toolName = [string]$params.name
                $arguments = Get-ArgumentValue $params "arguments" @{}
                if ($null -eq $arguments) { $arguments = @{} }
                Log "tools/call: $toolName"
                $result = switch ($toolName) {
                    "inspect_project" { Invoke-AgentDiscovery $arguments "project"; break }
                    "get_project_context" { Invoke-AgentDiscovery $arguments "context"; break }
                    "get_device_capabilities" { Invoke-AgentDiscovery $arguments "device"; break }
                    "inspect_scene" { Invoke-AgentDiscovery $arguments "scene"; break }
                    "query_assets" { Invoke-AgentDiscovery $arguments "assets"; break }
                    "evaluate_agent_result" { Invoke-AgentEvaluate $arguments; break }
                    "run_agent_game_spec" { Invoke-AgentGameSpec $arguments; break }
                    "run_agent_test" { Invoke-AgentTest $arguments; break }
                    "run_agent_repair" { Invoke-AgentRepair $arguments; break }
                    "build" { Invoke-Build $arguments; break }
                    "create_script" { Invoke-CreateScript $arguments; break }
                    "run_agent_workflow" { Invoke-AgentWorkflow $arguments; break }
                    "run_agent_task" { Invoke-AgentTask $arguments; break }
                    "run_agent_plan" { Invoke-AgentPlan $arguments; break }
                    "collect_agent_evidence" { Invoke-AgentEvidence $arguments; break }
                    "capture_frame" { Invoke-RenderDocCapture $arguments; break }
                    "capture_performance" { Invoke-NsightCapture $arguments; break }
                    "validate_scene" { Invoke-ValidateScene $arguments; break }
                    "apply_scene_commands" { Invoke-SceneCommands $arguments; break }
                    "run_gameplay_test" { Invoke-RunTest $arguments "gameplay"; break }
                    "run_render_test" { Invoke-RunTest $arguments "rendering"; break }
                    "run_test" { Invoke-RunTest $arguments "compatibility"; break }
                    "read_dump" { Invoke-ReadDump $arguments; break }
                    "assert_state" { Invoke-AssertState $arguments; break }
                    "engine_status" { Invoke-EngineStatus; break }
                    "stop_engine" { Invoke-StopEngine; break }
                    default { $null }
                }
                if ($null -eq $result) { Send-Error $id -32602 "Unknown tool: $toolName" } else { Send-ToolObjectResult $id $result }
            }
            default { Send-Error $id -32601 "Method not found: $method" }
        }
    } catch {
        $messageText = $_.Exception.Message
        Log "request failed: $messageText"
        if ($null -ne $id) { Send-ToolResult $id "[MCP ERROR] $messageText" $true } else { Send-Error $null -32603 "Internal error" $messageText }
    }
}
