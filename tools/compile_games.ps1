# CompileGames.ps1 - 编译 games/ 下所有游戏插件 DLL 到 exe 目录
# 供编辑器"重新编译并重载游戏插件"调用(热更新,不重启引擎)。
# 玩法源码必须位于项目清单的 codeRoot（默认 <project>/games）内；不再扫描仓库根目录。
param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectPath = ""
)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$build = "$root\out\build\x64-Release"
. (Join-Path $PSScriptRoot "Find-VsDevCmd.ps1")
$vsdevcmd = Find-VsDevCmdPath
if (-not $vsdevcmd) {
    Write-Host "ERROR: 找不到 VsDevCmd.bat，请安装 Visual Studio C++ 工具链。"
    exit 3
}
# include 分类目录（include/ 根 + 各子目录，兼容无前缀 include "AudioManager.h" 等旧引用）
$includeArgs = "/I `"$root\include`" /I `"$root\include\Core`" /I `"$root\include\Rendering`" /I `"$root\include\ECS`" /I `"$root\include\Editor`" /I `"$root\include\UI`" /I `"$root\include\World`" /I `"$root\include\Game`" /I `"$root\include\Platform`" /I `"$root\dependencies`" /I `"$root\dependencies\glm`" /I `"$root\dependencies\JoltPhysics`" /I `"$root\dependencies\box2d\include`""
# 第三方 include（box2d 等，同 CMakeLists 中 Game target 的路径）
if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
    Write-Host "ERROR: -ProjectPath is required; gameplay code belongs to a selected project."
    exit 1
}

try {
    $projectFullPath = [System.IO.Path]::GetFullPath($ProjectPath)
} catch {
    Write-Host "Invalid ProjectPath: $ProjectPath"
    exit 1
}
$manifestPath = Join-Path $projectFullPath "project.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    Write-Host "Project manifest not found: $manifestPath"
    exit 1
}
try {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
} catch {
    Write-Host "Failed to parse project manifest: $manifestPath"
    exit 1
}
$codeRoot = "games"
if ($manifest.codeRoot) { $codeRoot = [string]$manifest.codeRoot }
$codeRoot = $codeRoot.Replace('/', '\').Trim()
if ([string]::IsNullOrWhiteSpace($codeRoot) -or
    [System.IO.Path]::IsPathRooted($codeRoot) -or
    $codeRoot -eq ".." -or $codeRoot.StartsWith('..\')) {
    Write-Host "Project codeRoot must stay inside the project: $codeRoot"
    exit 1
}
$gamesDir = [System.IO.Path]::GetFullPath((Join-Path $projectFullPath $codeRoot))
$projectPrefix = $projectFullPath.TrimEnd('\') + '\'
if (-not $gamesDir.StartsWith($projectPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    Write-Host "Project codeRoot escapes the project directory: $codeRoot"
    exit 1
}
Write-Host "Project gameplay source: $gamesDir"

if (-not (Test-Path -LiteralPath $gamesDir -PathType Container)) {
    Write-Host "No gameplay source directory: $gamesDir"
    exit 0
}

# 每个直接子目录对应一个插件；项目模式只编译当前项目的 codeRoot。
$pluginDirs = @(Get-ChildItem -LiteralPath $gamesDir -Directory)

foreach ($dir in $pluginDirs) {
    $name = $dir.Name
    $sources = @(Get-ChildItem $dir.FullName -Filter "*.cpp" | ForEach-Object { "`"$($_.FullName)`"" })
    if ($sources.Count -eq 0) { continue }
    $out = "$build\Game$name.dll.tmp"   # 先编译到 .tmp(运行中的 DLL 被锁定,不能直接覆盖)
    # cl 在同时编译多个源文件时，如果没有 /Fo，会把 .obj 写到当前工作目录。
    # 每个插件使用独立的中间目录，避免对象文件散落到仓库根目录或互相覆盖。
    $projectName = Split-Path -Leaf $projectFullPath.TrimEnd('\')
    if ([string]::IsNullOrWhiteSpace($projectName)) { $projectName = "project" }
    $objDir = "$build\obj\projects\$projectName\$name"
    New-Item -ItemType Directory -Path $objDir -Force | Out-Null
    # 引擎未运行时直接输出 Game<name>.dll（首次构建/发布形态开箱即用）；
    # 引擎运行时输出 .tmp，等编辑器热重载重命名（运行中的 DLL 被锁定，不能直接覆盖）。
    $engineRunning = [bool](Get-Process -Name "MikanEngine" -ErrorAction SilentlyContinue)
    if (-not $engineRunning) { $out = "$build\Game$name.dll" }
    $srcList = $sources -join " "
    # Box2D 由 Game.dll 统一持有；项目插件通过 Physics2DManager 使用 2D 物理，不重复链接静态库。
    # cmd.exe 中目录末尾的反斜杠会转义结束引号，因此传递两个反斜杠。
    $objDirArg = "/Fo`"$objDir\\`""
    $cmdLine = "`"$vsdevcmd`" -arch=x64 -host_arch=x64 >nul 2>&1 && cl /nologo /LD /EHsc /std:c++20 /MD /utf-8 /DMIKAN_USE_GAME $includeArgs $objDirArg $srcList /Fe:`"$out`" /link `"$build\Game.lib`" `"$root\lib\x64\SDL3.lib`" `"$root\lib\x64\SDL3_image.lib`""
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
