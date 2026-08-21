# mcp_server.ps1 - MikanEngine AI 开发-测试 MCP server（stdio transport）
# ------------------------------------------------------------------
# 暴露工具：build / validate_scene / run_test / read_dump / engine_status / stop_engine
# 协议：JSON-RPC 2.0 over stdio（每行一条 JSON 消息，MCP 规范）。
# ⚠️ 本脚本的 stdout 专用于协议响应，任何日志/调试必须走 stderr（[Console]::Error）。
#
# MCP client 配置（stdio server）：
#   command: powershell
#   args:    ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "D:\\Engine project\\vulkan engine\\tools\\mcp_server.ps1"]
# ------------------------------------------------------------------
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exeDir = Join-Path $root "out\build\x64-Release"
$exePath = Join-Path $exeDir "EngineMain.exe"
$schemaPath = Join-Path $PSScriptRoot "scene_schema.json"

# stdin/stdout 强制 UTF-8 无 BOM（中文系统 PowerShell 5.1 默认 GBK，MCP client 发 UTF-8 会乱码）
try { [Console]::InputEncoding = New-Object System.Text.UTF8Encoding($false) } catch {}
try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch {}

function Log([string]$msg) { [Console]::Error.WriteLine("[mcp] $msg") }
function Send-Json($obj) {
    $json = $obj | ConvertTo-Json -Depth 20 -Compress
    [Console]::Out.WriteLine($json)
    [Console]::Out.Flush()
}
function Send-Error([int]$id, [int]$code, [string]$message) {
    Send-Json @{ jsonrpc = "2.0"; id = $id; error = @{ code = $code; message = $message } }
}
function Send-Result([int]$id, $result) {
    Send-Json @{ jsonrpc = "2.0"; id = $id; result = $result }
}
function Send-ToolResult([int]$id, [string]$text, [bool]$isError = $false) {
    Send-Json @{ jsonrpc = "2.0"; id = $id; result = @{ content = @(@{ type = "text"; text = $text }); isError = $isError } }
}

# ==================== 工具实现 ====================

function Invoke-Build($argsObj) {
    $target = if ($argsObj.target) { [string]$argsObj.target } else { "EngineMain" }
    $killEngine = [bool]$argsObj.killEngine
    $cmd = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "build.ps1"), "-Target", $target)
    if ($killEngine) { $cmd += "-KillEngine" }
    $out = (& powershell @cmd 2>&1 | Out-String)
    $exit = $LASTEXITCODE
    return @{ text = "=== build($target) ===`n$($out.Trim())`nexit code: $exit"; isError = ($exit -ne 0) }
}

function Invoke-ValidateScene($argsObj) {
    $scenePath = [string]$argsObj.scenePath
    $checkAssets = [bool]$argsObj.checkAssets
    if (-not $scenePath) { return @{ text = "[ERROR] 缺少参数 scenePath"; isError = $true } }
    $cmd = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "validate_scene.ps1"), $scenePath)
    if ($checkAssets) { $cmd += "-CheckAssets" }
    $out = (& powershell @cmd 2>&1 | Out-String)
    $exit = $LASTEXITCODE
    return @{ text = "=== validate_scene ===`n$($out.Trim())`nexit code: $exit (0=通过)"; isError = ($exit -ne 0) }
}

function Invoke-RunTest($argsObj) {
    $scene = [string]$argsObj.scene
    $game = [string]$argsObj.game
    $frames = if ($null -ne $argsObj.frames) { [int]$argsObj.frames } else { 120 }
    $timeoutMs = if ($null -ne $argsObj.timeoutMs) { [int]$argsObj.timeoutMs } else { 60000 }
    $dumpPath = if ($argsObj.dumpStatePath) { [string]$argsObj.dumpStatePath } else { Join-Path $exeDir "mcp_dump.json" }
    $extraArgs = if ($argsObj.extraArgs) { @($argsObj.extraArgs) } else { @() }
    if (-not $scene) { return @{ text = "[ERROR] 缺少参数 scene（如 assets/contact2d.json）"; isError = $true } }
    if (-not (Test-Path $exePath)) { return @{ text = "[ERROR] 引擎不存在: $exePath（先 build）"; isError = $true } }

    $running = Get-Process -Name "EngineMain" -ErrorAction SilentlyContinue
    if ($running) {
        return @{ text = "[ERROR] EngineMain.exe 正在运行（PID $($running.Id -join ',')），测试会冲突。请先 stop_engine 或 build(killEngine=true)"; isError = $true }
    }

    # 参数拼接（值含空格时加引号，避免拆词）
    $argList = New-Object System.Collections.Generic.List[string]
    $argList.Add("--headless"); $argList.Add("--frames"); $argList.Add([string]$frames)
    $argList.Add("--dump-state"); $argList.Add($dumpPath)
    foreach ($a in $extraArgs) { $argList.Add([string]$a) }
    # --scene 需配合 --project/--no-project-manager 才会加载场景；自动附加，避免 AI 忘传导致场景不加载
    if (-not ($extraArgs -match "^--no-project-manager$" -or $extraArgs -match "^--project$" -or $extraArgs -match "^--project=")) {
        $argList.Add("--no-project-manager")
    }
    $argList.Add("--scene"); $argList.Add($scene)
    if ($game) { $argList.Add("--game"); $argList.Add($game) }
    $argStr = ($argList | ForEach-Object {
        if ($_ -match "\s") { '"{0}"' -f $_.Replace('"', '\"') } else { $_ }
    }) -join " "

    $errFile = Join-Path $exeDir "mcp_test_err.txt"
    Remove-Item $errFile, $dumpPath -ErrorAction SilentlyContinue
    Log "starting headless test: $exePath $argStr"
    # 用 .NET Process 启动（PS 5.1 的 Start-Process -PassThru 在重定向输出时 ExitCode 恒为空，属已知缺陷）
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exePath
    $psi.WorkingDirectory = $exeDir
    $psi.Arguments = $argStr
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    # 异步读输出流，避免管道缓冲填满导致进程阻塞（同步 ReadToEnd + WaitForExit 会死锁）
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    $exited = $proc.WaitForExit($timeoutMs)
    if (-not $exited) {
        try { $proc.Kill() } catch {}
        return @{ text = "[ERROR] 测试超时（${timeoutMs}ms），已强制结束。引擎可能挂起"; isError = $true }
    }
    try { $stdout = $outTask.Result } catch { $stdout = "" }
    try { $stderr = $errTask.Result } catch { $stderr = "" }
    $exit = $proc.ExitCode

    # 收集结果
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("=== run_test(headless) ===")
    [void]$sb.AppendLine("scene: $scene   frames: $frames   exit code: $exit (0=正常跑完, 2=场景加载失败)")
    @($stderr -split "`r?`n") | Where-Object { $_ -match "\[Headless\]|\[Crash\]|ERROR|Fatal" } | ForEach-Object { [void]$sb.AppendLine("  $_") }
    $crashLog = Join-Path $exeDir "crash_log.txt"
    if (Test-Path $crashLog) {
        [void]$sb.AppendLine("  !! crash_log.txt 存在（引擎崩溃）:")
        Get-Content $crashLog -TotalCount 8 | ForEach-Object { [void]$sb.AppendLine("    $_") }
    } elseif ($exit -ne 0) {
        [void]$sb.AppendLine("  !! 非零退出码但无 crash_log")
    }
    if (Test-Path $dumpPath) {
        $dump = [System.IO.File]::ReadAllText($dumpPath, [System.Text.UTF8Encoding]::new($false)) | ConvertFrom-Json
        [void]$sb.AppendLine("dump: $dumpPath ($($dump.entity_count) 实体, $($dump.frames) 帧)")
        [void]$sb.AppendLine("  详细状态用 read_dump(path=`"$dumpPath`") 读取; 例: 断言实体 wpos[1]")
    } else {
        [void]$sb.AppendLine("dump: 未生成（$dumpPath）")
    }
    return @{ text = $sb.ToString(); isError = ($exit -ne 0) }
}

function Invoke-ReadDump($argsObj) {
    $path = [string]$argsObj.path
    if (-not $path) { return @{ text = "[ERROR] 缺少参数 path"; isError = $true } }
    if (-not (Test-Path $path)) { return @{ text = "[ERROR] 文件不存在: $path"; isError = $true } }
    return @{ text = [System.IO.File]::ReadAllText($path, [System.Text.UTF8Encoding]::new($false)); isError = $false }
}

function Invoke-EngineStatus() {
    $running = @(Get-Process -Name "EngineMain" -ErrorAction SilentlyContinue)
    $crash = Test-Path (Join-Path $exeDir "crash_log.txt")
    $sb = New-Object System.Text.StringBuilder
    if ($running.Count -gt 0) {
        [void]$sb.AppendLine("EngineMain 运行中: PID $($running.Id -join ', ')")
    } else {
        [void]$sb.AppendLine("EngineMain 未运行")
    }
    [void]$sb.AppendLine("crash_log.txt: $(if ($crash) {'存在（上次运行崩溃）'} else {'无'})")
    [void]$sb.AppendLine("exe: $exePath 存在: $(Test-Path $exePath)")
    return @{ text = $sb.ToString(); isError = $false }
}

function Invoke-StopEngine() {
    $running = @(Get-Process -Name "EngineMain" -ErrorAction SilentlyContinue)
    if ($running.Count -eq 0) { return @{ text = "EngineMain 未运行"; isError = $false } }
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    return @{ text = "已强制结束 $($running.Count) 个 EngineMain 进程（PID $($running.Id -join ', ')）"; isError = $false }
}

# ==================== 工具元数据（tools/list） ====================
$tools = @(
    @{
        name = "build"
        description = "一键构建引擎（封装 tools/build.ps1）。target: EngineMain(默认,连带 Editor/Game/Shaders)/Editor/Game/CompileShaders。引擎运行时构建会失败(LNK1104),用 killEngine=true 自动关闭。返回退出码(0=成功)与错误摘要。"
        inputSchema = @{ type = "object"; properties = @{
            target = @{ type = "string"; description = "构建目标"; "default" = "EngineMain" }
            killEngine = @{ type = "boolean"; description = "构建前自动关闭运行中的 EngineMain"; "default" = $false }
        } }
    },
    @{
        name = "validate_scene"
        description = "离线校验场景 JSON（无需启动引擎）：组件键/字段白名单(schema)、id 唯一、parent 引用、transform 结构。scenePath 必填；checkAssets=true 时检查资源文件存在。退出码 0=通过。"
        inputSchema = @{ type = "object"; properties = @{
            scenePath = @{ type = "string"; description = "场景 JSON 路径（如 assets/contact2d.json）" }
            checkAssets = @{ type = "boolean"; description = "检查 texture/modelPath 等资源引用存在"; "default" = $false }
        }; required = @("scenePath") }
    },
    @{
        name = "run_test"
        description = "headless 运行引擎测试（隐藏窗口,不弹窗）：--headless --frames N --dump-state。运行后返回退出码(0=正常,2=场景加载失败)+崩溃检查+dump 摘要。用 read_dump 读详细实体状态做断言（如实体 wpos）。引擎已在运行时会拒绝执行。"
        inputSchema = @{ type = "object"; properties = @{
            scene = @{ type = "string"; description = "场景路径（必填,如 assets/contact2d.json）" }
            game = @{ type = "string"; description = "游戏模块名（如 contact2d）" }
            frames = @{ type = "integer"; description = "运行帧数"; "default" = 120 }
            dumpStatePath = @{ type = "string"; description = "状态导出路径(默认 out/build/x64-Release/mcp_dump.json)" }
            extraArgs = @{ type = "array"; items = @{ type = "string" }; description = "额外引擎参数,如 --no-voxel-world --no-project-manager" }
            timeoutMs = @{ type = "integer"; description = "超时毫秒(默认60000,超时强制结束)"; "default" = 60000 }
        }; required = @("scene") }
    },
    @{
        name = "read_dump"
        description = "读取 run_test 导出的场景状态 JSON 全文（实体 id/name/pos/wpos/rot/scale/visible），用于断言。"
        inputSchema = @{ type = "object"; properties = @{
            path = @{ type = "string"; description = "dump 文件路径（run_test 返回中给出）" }
        }; required = @("path") }
    },
    @{
        name = "engine_status"
        description = "查询 EngineMain 是否运行、crash_log.txt 是否存在、exe 是否已构建。"
        inputSchema = @{ type = "object"; properties = @{} }
    },
    @{
        name = "stop_engine"
        description = "强制结束运行中的 EngineMain.exe（释放 Game.dll/Editor.dll 文件锁）。"
        inputSchema = @{ type = "object"; properties = @{} }
    }
)

# ==================== 主循环（JSON-RPC over stdio） ====================
Log "MikanEngine MCP server started (root=$root)"
while ($true) {
    $line = [Console]::In.ReadLine()
    if ($null -eq $line) { Log "stdin EOF, exiting"; exit 0 }
    $line = $line.TrimStart([char]0xFEFF)   # 剥离可能的 UTF-8 BOM
    if ($line.Trim() -eq "") { continue }
    $msg = $null
    try { $msg = $line | ConvertFrom-Json } catch { Log "invalid JSON: $($_.Exception.Message)"; continue }
    $method = [string]$msg.method
    $id = $msg.id
    $isNotification = ($null -eq $id)
    $params = $msg.params

    switch ($method) {
        "initialize" {
            Send-Result $id @{
                protocolVersion = "2025-03-26"
                capabilities = @{ tools = @{ listChanged = $false } }
                serverInfo = @{ name = "mikanengine-mcp"; version = "1.0.0" }
            }
        }
        "notifications/initialized" { Log "client initialized" }
        "ping" { if (-not $isNotification) { Send-Result $id @{} } }
        "tools/list" {
            Send-Result $id @{ tools = $tools }
        }
        "tools/call" {
            $toolName = [string]$params.name
            $argsObj = $params.arguments
            if ($null -eq $argsObj) { $argsObj = @{} }
            Log "tools/call: $toolName"
            $r = $null
            switch ($toolName) {
                "build"            { $r = Invoke-Build $argsObj }
                "validate_scene"   { $r = Invoke-ValidateScene $argsObj }
                "run_test"         { $r = Invoke-RunTest $argsObj }
                "read_dump"        { $r = Invoke-ReadDump $argsObj }
                "engine_status"    { $r = Invoke-EngineStatus }
                "stop_engine"      { $r = Invoke-StopEngine }
                default            { Send-Error $id -32602 "Unknown tool: $toolName" }
            }
            if ($null -ne $r) { Send-ToolResult $id $r.text $r.isError }
        }
        default {
            if (-not $isNotification) { Send-Error $id -32601 "Method not found: $method" }
            else { Log "ignored notification: $method" }
        }
    }
}
