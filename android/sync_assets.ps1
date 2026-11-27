# 同步引擎资源和指定项目资源到 Android assets 目录（UTF-8 BOM 必需）
# 普通项目使用 Gradle 的打包资源清单，并将结果复制到旧版输出目录。
# 体素原型使用独立资源清单和输出目录。

[CmdletBinding()]
param(
    [string]$ProjectPath = "../projects/third-person-navigation",
    [switch]$VoxelPrototype,
    [switch]$Offline
)

$ErrorActionPreference = "Stop"
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$engineSourceDir = Join-Path $repoRoot "engine"
$targetDir = if ($VoxelPrototype) {
    Join-Path $repoRoot "out\android-voxel-sandbox\assets"
} else {
    Join-Path $PSScriptRoot "app/src/main/assets"
}

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

function Copy-ListedAsset([string]$SourceDir, [string]$RelativePath) {
    $sourceRoot = [System.IO.Path]::GetFullPath($SourceDir).TrimEnd('\') + '\'
    if ([System.IO.Path]::IsPathRooted($RelativePath)) {
        throw "Asset path must be relative: $RelativePath"
    }
    $source = [System.IO.Path]::GetFullPath((Join-Path $sourceRoot $RelativePath))
    if (!$source.StartsWith($sourceRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Asset path escapes its resource root: $RelativePath"
    }
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required Android asset missing: $source"
    }
    $destination = Join-Path $targetDir $source.Substring($sourceRoot.Length)
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

function Sync-ProjectAssets([string]$ProjectDir, [string]$Destination) {
    $gradleArgs = @('-p', $PSScriptRoot, ':app:syncMikanProjectAssets',
        "-PmikanProjectPath=$ProjectDir", '--console=plain')
    if ($Offline) {
        $gradleArgs += '--offline'
    }
    $installedGradle = Get-Command gradle -CommandType Application -ErrorAction SilentlyContinue
    if ($Offline -and $installedGradle) {
        & $installedGradle.Source @gradleArgs
    } elseif ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT) {
        & (Join-Path $PSScriptRoot 'gradlew.bat') @gradleArgs
    } else {
        & sh (Join-Path $PSScriptRoot 'gradlew') @gradleArgs
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle Android 资源同步失败（exit=$LASTEXITCODE）"
    }

    $stagedDir = Join-Path $PSScriptRoot 'app/build/generated/mikan-project-assets'
    if (!(Test-Path -LiteralPath (Join-Path $stagedDir 'mikan_android_scene.txt') -PathType Leaf)) {
        throw "Gradle Android 资源输出不存在: $stagedDir"
    }
    $existing = Get-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
    if ($existing -and ($existing.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        throw "资源输出目录不能是链接或联接点: $Destination"
    }

    $syncId = [System.Guid]::NewGuid().ToString('N')
    $pendingDir = "$Destination.pending-$syncId"
    $backupDir = "$Destination.backup-$syncId"
    try {
        Copy-Item -LiteralPath $stagedDir -Destination $pendingDir -Recurse
        if (Test-Path -LiteralPath $Destination) {
            Move-Item -LiteralPath $Destination -Destination $backupDir
        }
        try {
            Move-Item -LiteralPath $pendingDir -Destination $Destination
        } catch {
            if (Test-Path -LiteralPath $backupDir) {
                Move-Item -LiteralPath $backupDir -Destination $Destination
            }
            throw
        }
        if (Test-Path -LiteralPath $backupDir) {
            Remove-Item -LiteralPath $backupDir -Recurse -Force
        }
    } finally {
        if (Test-Path -LiteralPath $pendingDir) {
            Remove-Item -LiteralPath $pendingDir -Recurse -Force
        }
    }
    Write-Host "Android assets synced to $Destination" -ForegroundColor Green
}

if (-not (Test-Path -LiteralPath $engineSourceDir -PathType Container)) {
    throw "引擎资源目录不存在: $engineSourceDir"
}

$projectSourceDir = Resolve-ProjectPath $ProjectPath
$manifestPath = Join-Path $projectSourceDir "project.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "项目清单不存在: $manifestPath"
}
if (!$VoxelPrototype) {
    Sync-ProjectAssets $projectSourceDir $targetDir
    return
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
    $resolvedTarget = (Resolve-Path -LiteralPath $targetDir).ProviderPath
    if ($resolvedTarget -ne [System.IO.Path]::GetFullPath($targetDir)) {
        throw "资源输出目录解析到了意外的位置: $resolvedTarget"
    }
    Get-ChildItem -LiteralPath $resolvedTarget -Force | ForEach-Object {
        Remove-Item -LiteralPath $_.FullName -Recurse -Force
    }
}
New-Item -ItemType Directory -Path $targetDir -Force | Out-Null

# 1) 引擎系统资产：拍平到 APK assets 根（fonts/shaders/textures/ui 直接放根）
#    Android 运行时通过 SDL_IOFromFile 从 APK assets 根读取。
$engineAssets = Join-Path $PSScriptRoot 'voxel_prototype_engine_assets.txt'
foreach ($line in Get-Content -LiteralPath $engineAssets -Encoding UTF8) {
    $asset = $line.Trim()
    if ($asset -and !$asset.StartsWith('#')) {
        Copy-ListedAsset $engineSourceDir $asset
    }
}
foreach ($asset in (@($manifest.assets) + @($scenePath) | Sort-Object -Unique)) {
    Copy-ListedAsset $resourceSourceDir $asset
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
