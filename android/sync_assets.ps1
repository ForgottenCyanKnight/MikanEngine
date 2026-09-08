# 同步引擎资源和指定项目资源到 Android assets 目录（UTF-8 BOM 必需）
# 引擎资源来自 engine/；游戏资源来自 project.json.resourceRoot。
# 根目录 assets/ 仅是本机旧测试资料，不参与 Android 打包。
# 全量重建：每次清空目标后同步，保证结果一致（引擎资产唯一、无残留）。

[CmdletBinding()]
param(
    [string]$ProjectPath = "..\projects\third-person-navigation"
)

$ErrorActionPreference = "Stop"
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$engineSourceDir = Join-Path $repoRoot "engine"
$targetDir = Join-Path $PSScriptRoot "app\src\main\assets"

function Resolve-ProjectPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "ProjectPath 不能为空"
    }
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    # 兼容从 android/ 目录传入 ..\projects\...，以及从仓库根目录传入 projects/...
    $fromScript = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot $Path))
    if (Test-Path -LiteralPath $fromScript -PathType Container) {
        return $fromScript
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Resolve-ResourceRoot([string]$ProjectDir, $Manifest) {
    $resourceRoot = [string]$Manifest.resourceRoot
    if ([string]::IsNullOrWhiteSpace($resourceRoot) -or $resourceRoot -eq ".") {
        return $ProjectDir
    }

    $resourceRoot = $resourceRoot.Replace('/', '\').Trim()
    if ([System.IO.Path]::IsPathRooted($resourceRoot) -or
        $resourceRoot -eq ".." -or $resourceRoot.StartsWith("..\")) {
        throw "project.json.resourceRoot 必须位于项目目录内: $resourceRoot"
    }
    $candidate = [System.IO.Path]::GetFullPath((Join-Path $ProjectDir $resourceRoot))
    $projectPrefix = ([System.IO.Path]::GetFullPath($ProjectDir)).TrimEnd('\') + '\'
    if (-not $candidate.StartsWith($projectPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "project.json.resourceRoot 不能逃逸项目目录: $resourceRoot"
    }
    return $candidate
}

if (-not (Test-Path -LiteralPath $engineSourceDir -PathType Container)) {
    throw "引擎资源目录不存在: $engineSourceDir"
}

$projectSourceDir = Resolve-ProjectPath $ProjectPath
$manifestPath = Join-Path $projectSourceDir "project.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "项目清单不存在: $manifestPath"
}

try {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
} catch {
    throw "项目清单解析失败: $manifestPath ($($_.Exception.Message))"
}

$resourceSourceDir = Resolve-ResourceRoot $projectSourceDir $manifest
if (-not (Test-Path -LiteralPath $resourceSourceDir -PathType Container)) {
    throw "项目资源根目录不存在: $resourceSourceDir"
}

$scenePath = [string]$manifest.scene
if ([string]::IsNullOrWhiteSpace($scenePath)) {
    throw "项目清单没有 scene，无法确定 Android 启动场景: $manifestPath"
}
$scenePath = $scenePath.Replace('\', '/')

# 清空目标（全量重建）
Write-Host "Clearing $targetDir..." -ForegroundColor Yellow
if (Test-Path -LiteralPath $targetDir -PathType Container) {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $targetDir "*")
}
New-Item -ItemType Directory -Path $targetDir -Force | Out-Null

# 1) 引擎系统资产：拍平到 APK assets 根（fonts/shaders/textures/ui 直接放根）
#    Android 运行时通过 SDL_IOFromFile 从 APK assets 根读取。
Write-Host "Syncing engine assets from $engineSourceDir to $targetDir (flattened)..." -ForegroundColor Green
robocopy $engineSourceDir $targetDir /E /XD .git glsl /XF *.tmp
$engineCopyExit = $LASTEXITCODE
if ($engineCopyExit -ge 8) {
    throw "引擎资源同步失败（robocopy=$engineCopyExit）"
}

# 2) 只同步选定项目的 resourceRoot，排除玩法源码。
Write-Host "Syncing project resources from $resourceSourceDir to $targetDir..." -ForegroundColor Green
robocopy $resourceSourceDir $targetDir /E `
    /XD .git games out build `
    /XF project.json *.tmp
$projectCopyExit = $LASTEXITCODE
if ($projectCopyExit -ge 8) {
    throw "项目资源同步失败（robocopy=$projectCopyExit）"
}

# 3) 将项目清单中的启动场景写入 APK 专用配置，避免 native runtime 硬编码旧场景。
$startupScenePath = Join-Path $targetDir "mikan_android_scene.txt"
[System.IO.File]::WriteAllText(
    $startupScenePath,
    $scenePath + [System.Environment]::NewLine,
    [System.Text.UTF8Encoding]::new($false))

Write-Host "Android project: $projectSourceDir" -ForegroundColor Cyan
Write-Host "Android resource root: $resourceSourceDir" -ForegroundColor Cyan
Write-Host "Android startup scene: $scenePath" -ForegroundColor Cyan
Write-Host "Assets synced successfully!" -ForegroundColor Green
