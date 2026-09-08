# build.ps1 - MikanEngine 一键构建
# ------------------------------------------------------------------
# 封装 VsDevCmd + cmake --build，解决两个已知摩擦点：
#   1) 构建命令冗长脆弱（vsdevcmd 引导 + 引号转义 + 日志重定向）
#   6) 文件锁陷阱（运行中的 MikanEngine.exe 锁定 Game.dll/Editor.dll -> LNK1104）
#
# 用法（项目根执行，或任意目录 -File 调用）：
#   powershell -NoProfile -File tools\build.ps1                          # 默认构建 MikanEngine（连带 Editor/Game/Shaders）
#   powershell -NoProfile -File tools\build.ps1 -Target Editor            # 只构建 Editor.dll
#   powershell -NoProfile -File tools\build.ps1 -Target Game              # 只构建 Game.dll
#   powershell -NoProfile -File tools\build.ps1 -Target CompileShaders    # 只重编 shader
#   powershell -NoProfile -File tools\build.ps1 -KillEngine               # 构建前强制关闭运行中的引擎（避免文件锁）
#   powershell -NoProfile -File tools\build.ps1 -CleanFirst               # 先清理目标，排除陈旧中间产物
#   powershell -NoProfile -File tools\build.ps1 -ConfigureIfMissing       # CMakeCache 缺失时自动使用 preset 配置
#   powershell -NoProfile -File tools\build.ps1 -LogPath <file>           # 自定义构建日志路径
#
# 退出码：
#   0 = 构建成功（无 error C / LNK）
#   1 = 构建失败（错误摘要已打印，完整日志见 -LogPath）
#   2 = 检测到运行中的引擎且未授权关闭（加 -KillEngine 重试）
#   3 = 环境错误（找不到 VsDevCmd / 构建目录无效）
# ------------------------------------------------------------------
[CmdletBinding()]
param(
    [ValidateSet("MikanEngine", "MikanTestRunner", "Editor", "Game", "CompileShaders")][string]$Target = "MikanEngine",
    [switch]$KillEngine,
    [switch]$CleanFirst,
    [switch]$ConfigureIfMissing,
    [string]$LogPath = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "out\build\x64-Release"
$script:logPath = if ($LogPath) { $LogPath } else { Join-Path $root "out\build\build.log" }

function Write-BuildLog([string]$msg) {
    Write-Host "[BUILD] $msg"
}

# ---- 1. 定位 VsDevCmd.bat（vswhere 探测，避免硬编码 VS 安装路径）----
function Get-VsDevCmdPath {
    $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsDir = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($vsDir -and (Test-Path $vsDir)) {
            $candidate = Join-Path $vsDir "Common7\Tools\VsDevCmd.bat"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    # 回退：环境变量或常见路径
    foreach ($c in @("$env:VSINSTALLDIR\Common7\Tools\VsDevCmd.bat",
                     "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
                     "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat")) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    return $null
}

# ---- 2. 文件锁处理（摩擦点 6）：构建前检查运行中的引擎 ----
function Test-EngineRunning {
    foreach ($name in @("MikanEngine", "MikanTestRunner")) {
        foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
            try {
                $expected = Join-Path $buildDir ($name + ".exe")
                if ($process.Path -and [System.IO.Path]::GetFullPath($process.Path).Equals(
                        [System.IO.Path]::GetFullPath($expected), [System.StringComparison]::OrdinalIgnoreCase)) {
                    return $true
                }
            } catch {}
        }
    }
    return $false
}

$vsDevCmd = Get-VsDevCmdPath
if (-not $vsDevCmd) {
    Write-BuildLog "ERROR: 找不到 VsDevCmd.bat（vswhere 与常见路径均未命中），无法进入 MSVC 环境"
    exit 3
}

$logDir = Split-Path -Parent $script:logPath
if ($logDir -and -not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }

if (-not (Test-Path "$buildDir\CMakeCache.txt")) {
    if (-not $ConfigureIfMissing) {
        Write-BuildLog "ERROR: 构建目录未配置：$buildDir （缺少 CMakeCache.txt）"
        Write-BuildLog "       加 -ConfigureIfMissing 自动运行 cmake --preset x64-release"
        exit 3
    }
    Write-BuildLog "CMakeCache 缺失，正在使用 preset x64-release 配置 ..."
    $configureCmd = "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul 2>&1 && cd /d `"$root`" && cmake --preset x64-release > `"$script:logPath`" 2>&1"
    cmd /c $configureCmd
    $configureRc = $LASTEXITCODE
    if ($configureRc -ne 0 -or -not (Test-Path "$buildDir\CMakeCache.txt")) {
        Write-BuildLog "ERROR: CMake 配置失败（exit=$configureRc，日志=$script:logPath）"
        if (Test-Path $script:logPath) {
            [System.IO.File]::ReadAllLines($script:logPath, [System.Text.Encoding]::Default) |
                Select-Object -Last 40 | ForEach-Object { Write-Host "  $_" }
        }
        exit 3
    }
    Write-BuildLog "CMake 配置完成"
}

if (Test-EngineRunning) {
    if (-not $KillEngine) {
        Write-BuildLog "WARN: MikanEngine/MikanTestRunner 正在运行，将锁定 Game.dll/Editor.dll（LNK1104）"
        Write-BuildLog "      加 -KillEngine 自动关闭后构建；或先手动关闭引擎"
        exit 2
    }
    Write-BuildLog "正在关闭运行中的 MikanEngine/MikanTestRunner ..."
    foreach ($name in @("MikanEngine", "MikanTestRunner")) {
        foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
            try {
                $expected = Join-Path $buildDir ($name + ".exe")
                if ($process.Path -and [System.IO.Path]::GetFullPath($process.Path).Equals(
                        [System.IO.Path]::GetFullPath($expected), [System.StringComparison]::OrdinalIgnoreCase)) {
                    Stop-Process -Id $process.Id -Force
                }
            } catch {}
        }
    }
    Start-Sleep -Milliseconds 800
    if (Test-EngineRunning) {
        Write-BuildLog "ERROR: 测试/引擎进程未能关闭，放弃构建"
        exit 2
    }
    Write-BuildLog "引擎已关闭"
}

# ---- 3. 执行构建（VsDevCmd 环境内，日志重定向）----
$cleanArg = if ($CleanFirst) { " --clean-first" } else { "" }
$cmd = "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul 2>&1 && cmake --build `"$buildDir`" --target $Target$cleanArg > `"$script:logPath`" 2>&1"
Write-BuildLog "target=$Target  日志=$script:logPath"
Write-BuildLog "cmake --build --target $Target$(if ($CleanFirst) {' --clean-first'} else {''}) ..."
cmd /c $cmd
$rc = $LASTEXITCODE

# ---- 4. 错误摘要（过滤噪音，只给 AI/人看真实错误）----
$errors = @()
$warnings = 0
if (Test-Path $script:logPath) {
    $lines = [System.IO.File]::ReadAllLines($script:logPath, [System.Text.Encoding]::Default)
    $errors = @($lines | Where-Object { $_ -match "(^|: )(fatal )?error C\d+|LNK\d+|FAILED:" })
    $warnings = @($lines | Where-Object { $_ -match "warning C\d+" }).Count
}

if ($rc -eq 0 -and $errors.Count -eq 0) {
    Write-BuildLog "OK: $Target 构建成功（warnings=$warnings）"
    $artifacts = switch ($Target) {
        "MikanEngine" { @("MikanEngine.exe", "Game.dll", "Editor.dll") }
        "MikanTestRunner" { @("MikanTestRunner.exe", "Game.dll") }
        "Editor" { @("Editor.dll") }
        "Game" { @("Game.dll") }
        default { @() }
    }
    foreach ($artifact in $artifacts) {
        $artifactPath = Join-Path $buildDir $artifact
        if (-not (Test-Path $artifactPath)) {
            Write-BuildLog "ERROR: 构建返回成功，但缺少预期产物：$artifactPath"
            exit 1
        }
        $info = Get-Item -LiteralPath $artifactPath
        Write-BuildLog "artifact: $($info.Name) size=$($info.Length) modified=$($info.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))"
    }
    exit 0
}

Write-BuildLog "FAILED: $Target 构建失败（exit=$rc, errors=$($errors.Count), warnings=$warnings）"
Write-BuildLog "--- 错误摘要（前 20 条，完整日志见 $script:logPath）---"
$errors | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }
exit 1
