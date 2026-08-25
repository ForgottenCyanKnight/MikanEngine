# CompileGames.ps1 - 编译 games/ 下所有游戏插件 DLL 到 exe 目录
# 供编辑器"重新编译并重载游戏插件"调用(热更新,不重启引擎)。
param()
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$build = "$root\out\build\x64-Release"
$vsdevcmd = "D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
# include 分类目录（include/ 根 + 各子目录，兼容无前缀 include "AudioManager.h" 等旧引用）
$includeArgs = "/I `"$root\include`" /I `"$root\include\Core`" /I `"$root\include\Rendering`" /I `"$root\include\ECS`" /I `"$root\include\Editor`" /I `"$root\include\UI`" /I `"$root\include\World`" /I `"$root\include\Game`" /I `"$root\include\Platform`" /I `"$root\dependencies`" /I `"$root\dependencies\glm`" /I `"$root\dependencies\JoltPhysics`""
# 第三方 include（box2d 等，同 CMakeLists 中 Game target 的路径）
$includeArgs += " /I `"$root\dependencies\box2d\include`" /I `"$root\dependencies\box2d\src`" /I `"$root\dependencies\box2d\msvc_compat`" /I `"$root\dependencies\box2d\extern\simde`""

$gamesDir = "$root\games"
if (-not (Test-Path $gamesDir)) { Write-Host "No games/ directory"; exit 0 }

# 插件源目录集合：
#  1) 引擎内置插件：games/<name>/（每子目录 = 一个插件）
#  2) 项目化插件（2026-08）：assets/models/project list/<proj>/games/（插件名 = 项目名；源码随项目）
$pluginDirs = @()
foreach ($dir in Get-ChildItem $gamesDir -Directory) {
    $pluginDirs += $dir
}
$projectListRoot = "$root\projects"
if (Test-Path $projectListRoot) {
    foreach ($proj in Get-ChildItem $projectListRoot -Directory) {
        $projGames = Join-Path $proj.FullName "games"
        if (Test-Path $projGames) {
            $pluginDirs += [pscustomobject]@{ FullName = $projGames; Name = $proj.Name }
        }
    }
}

foreach ($dir in $pluginDirs) {
    $name = $dir.Name
    $sources = @(Get-ChildItem $dir.FullName -Filter "*.cpp" | ForEach-Object { "`"$($_.FullName)`"" })
    if ($sources.Count -eq 0) { continue }
    $out = "$build\Game$name.dll.tmp"   # 先编译到 .tmp(运行中的 DLL 被锁定,不能直接覆盖)
    # cl 在同时编译多个源文件时，如果没有 /Fo，会把 .obj 写到当前工作目录。
    # 每个插件使用独立的中间目录，避免对象文件散落到仓库根目录或互相覆盖。
    $objDir = "$build\obj\games\$name"
    New-Item -ItemType Directory -Path $objDir -Force | Out-Null
    # 引擎未运行时直接输出 Game<name>.dll（首次构建/发布形态开箱即用）；
    # 引擎运行时输出 .tmp，等编辑器热重载重命名（运行中的 DLL 被锁定，不能直接覆盖）。
    $engineRunning = [bool](Get-Process -Name "EngineMain" -ErrorAction SilentlyContinue)
    if (-not $engineRunning) { $out = "$build\Game$name.dll" }
    $srcList = $sources -join " "
    # cmd.exe 中目录末尾的反斜杠会转义结束引号，因此传递两个反斜杠。
    $objDirArg = "/Fo`"$objDir\\`""
    $cmdLine = "`"$vsdevcmd`" -arch=x64 -host_arch=x64 >nul 2>&1 && cl /nologo /LD /EHsc /std:c++17 /utf-8 /DMIKAN_USE_GAME $includeArgs $objDirArg $srcList /Fe:`"$out`" /link `"$build\Game.lib`" `"$root\lib\x64\SDL3.lib`" `"$root\lib\x64\SDL3_image.lib`""
    Write-Host "Compiling game plugin: $name"
    cmd /c $cmdLine
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED to compile plugin: $name"
        exit 1
    }
    if ($engineRunning) {
        Write-Host "OK (awaiting hot-reload rename): $out"
    } else {
        Write-Host "OK: $out"
    }
}

Write-Host "All game plugins compiled"
exit 0
