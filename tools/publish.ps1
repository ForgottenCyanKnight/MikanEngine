# Publish.ps1 - package one selected project into a standalone release folder
#   (exe + runtime dlls + the project's game plugin + engine/project assets)
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\publish.ps1 -ProjectPath .\projects\third-person-navigation -OutDir D:\release
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\publish.ps1 -Project third-person-navigation -OutDir D:\release
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\publish.ps1 -ProjectPath .\projects\third-person-navigation -SkipAssets
param(
    [string]$OutDir = "",
    [switch]$SkipAssets,
    [switch]$SkipGames,
    [string]$Project = "",
    [string]$ProjectPath = "",
    [switch]$IncludeKtx
)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$build = "$root\out\build\x64-Release"
if (-not $OutDir) { $OutDir = "$root\publish" }

function Resolve-PathFromEngineRoot([string]$path) {
    if ([string]::IsNullOrWhiteSpace($path)) { return "" }
    if ([System.IO.Path]::IsPathRooted($path)) {
        return [System.IO.Path]::GetFullPath($path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $root $path))
}

function Test-PathWithin([string]$candidate, [string]$parent) {
    $candidateFull = [System.IO.Path]::GetFullPath($candidate).TrimEnd('\')
    $parentFull = [System.IO.Path]::GetFullPath($parent).TrimEnd('\')
    return $candidateFull.Equals($parentFull, [System.StringComparison]::OrdinalIgnoreCase) -or
        $candidateFull.StartsWith($parentFull + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

$engineRootFull = [System.IO.Path]::GetFullPath($root).TrimEnd('\')
$outFull = [System.IO.Path]::GetFullPath($OutDir).TrimEnd('\')
$projectDir = ""
$projectManifest = $null
$projectFolderName = ""

if ($ProjectPath) {
    $projectDir = Resolve-PathFromEngineRoot $ProjectPath
} elseif ($Project) {
    $projectDir = Resolve-PathFromEngineRoot (Join-Path "projects" $Project)
}

if ($projectDir) {
    $projectManifestPath = Join-Path $projectDir "project.json"
    if (-not (Test-Path -LiteralPath $projectManifestPath -PathType Leaf)) {
        Write-Host "ERROR: project manifest not found: $projectManifestPath" -ForegroundColor Red
        exit 1
    }
    try {
        $projectManifest = Get-Content -LiteralPath $projectManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        Write-Host "ERROR: failed to parse project manifest: $projectManifestPath" -ForegroundColor Red
        exit 1
    }
    $projectFolderName = Split-Path -Leaf $projectDir
    if ([string]::IsNullOrWhiteSpace($projectFolderName)) { $projectFolderName = "project" }
    if ([string]::IsNullOrWhiteSpace([string]$projectManifest.game)) {
        Write-Host "ERROR: project manifest has no game plugin: $projectManifestPath" -ForegroundColor Red
        exit 1
    }
    if (Test-PathWithin $outFull $projectDir) {
        Write-Host "ERROR: output directory cannot be inside the selected project: $outFull" -ForegroundColor Red
        exit 1
    }
}

if ($outFull.Equals($engineRootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
    Write-Host "ERROR: output directory cannot be the engine root: $outFull" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path "$build\MikanEngine.exe")) {
    Write-Host "ERROR: build not found. Run tools\build.ps1 -Target MikanEngine first." -ForegroundColor Red
    exit 1
}

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Write-Host "Publishing to: $OutDir" -ForegroundColor Green

# 1. Engine exe + runtime core dll (Game.dll). Editor.dll is deliberately excluded
#    from every publish path because this output is a standalone game package.
Copy-Item "$build\MikanEngine.exe" -Destination $OutDir -Force
Copy-Item "$build\Game.dll" -Destination $OutDir -Force
$staleEditor = Join-Path $OutDir "Editor.dll"
if (Test-Path -LiteralPath $staleEditor -PathType Leaf) {
    Remove-Item -LiteralPath $staleEditor -Force
    Write-Host "Removed stale Editor.dll from output." -ForegroundColor Cyan
}
Write-Host "Editor.dll excluded (standalone game package)." -ForegroundColor Cyan

# 2. Runtime dependency dlls (SDL3 / SDL3_image / assimp / ktx).
#    ktx.dll is lazy-loaded by TexturePool only when a KTX2 texture is actually
#    decoded; project packages include it only when the project uses KTX2 assets.
foreach ($dep in @("SDL3.dll", "SDL3_image.dll", "assimp-vc143-mtd.dll")) {
    if (Test-Path "$build\$dep") { Copy-Item "$build\$dep" -Destination $OutDir -Force }
    else { Write-Host "WARN: dependency dll missing in build: $dep" -ForegroundColor Yellow }
}
$projectUsesKtx = $false
if ($projectDir) {
    $projectUsesKtx = @(Get-ChildItem -LiteralPath $projectDir -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -ieq ".ktx2" }).Count -gt 0
}
if ($IncludeKtx -or -not $projectDir -or $projectUsesKtx) {
    if (Test-Path "$build\ktx.dll") { Copy-Item "$build\ktx.dll" -Destination $OutDir -Force }
}
else { Write-Host "ktx.dll excluded (project has no KTX2 assets)." -ForegroundColor Cyan }

# 3. Game plugin dlls: only the selected project's own plugin for project publish.
if (-not $SkipGames) {
    if ($projectDir) {
        $projectGame = [string]$projectManifest.game
        if ($projectGame -notmatch '^[A-Za-z0-9_.-]+$') {
            Write-Host "ERROR: project game plugin name is invalid: $projectGame" -ForegroundColor Red
            exit 1
        }
        $projectDllName = "Game$projectGame.dll"
        $projectDll = Join-Path $build $projectDllName
        if (-not (Test-Path -LiteralPath $projectDll -PathType Leaf)) {
            Write-Host "ERROR: project game plugin not found: $projectDll" -ForegroundColor Red
            Write-Host "       Build it with tools\compile_games.ps1 -ProjectPath `"$projectDir`" first." -ForegroundColor Yellow
            exit 1
        }
        Copy-Item -LiteralPath $projectDll -Destination $OutDir -Force
        Write-Host "Published game plugin: $projectDllName" -ForegroundColor Green
    } else {
        $plugins = Get-ChildItem "$build" -Filter "Game*.dll" | Where-Object { $_.Name -ne "Game.dll" }
        foreach ($p in $plugins) { Copy-Item $p.FullName -Destination $OutDir -Force }
    }
}

# 4. Engine system assets + project assets next to the exe (ProjectManager auto-detects)
if ($projectDir) {
    # Projectized publish: copy the complete engine runtime asset tree. The engine
    # validates shader pairs at startup and scenes may opt into skybox/cloud/voxel
    # resources, so pruning engine assets here would make valid projects fragile.
    # Copy only the project folder (no games/ sources - runtime uses plugin dlls), then
    # write a publish-local projects.json so the project list points at the new location.
    robocopy "$root\engine" "$OutDir\engine" /E > $null
    if ($LASTEXITCODE -ge 8) { Write-Host "WARN: robocopy(engine) exited $LASTEXITCODE" -ForegroundColor Yellow }
    $projRel = "projects\$projectFolderName"
    $destProj = Join-Path $OutDir $projRel
    New-Item -ItemType Directory -Path $destProj -Force | Out-Null
    robocopy $projectDir $destProj /E /XD games .git > $null
    if ($LASTEXITCODE -ge 8) { Write-Host "WARN: robocopy exited $LASTEXITCODE" -ForegroundColor Yellow }

    # Publish-local project registry. Store a RELATIVE path (projects/<name>) so the
    # publish folder stays portable - ResolveProjectPath anchors it to the engine root
    # (= publish folder) at load time. Backslashes must be JSON-escaped (\ -> \\).
    $registryName = [string]$projectManifest.name
    if ([string]::IsNullOrWhiteSpace($registryName)) { $registryName = $projectFolderName }
    $registry = [ordered]@{
        projects = @([ordered]@{
            lastOpened = 0
            name = $registryName
            path = $projRel.Replace('\', '/')
        })
    }
    $registryJson = $registry | ConvertTo-Json -Depth 4
    [System.IO.File]::WriteAllText((Join-Path $OutDir "projects.json"), $registryJson, [System.Text.UTF8Encoding]::new($false))
    Write-Host "Published project: $projectDir -> $destProj (games/ sources excluded)" -ForegroundColor Green
} elseif (-not $SkipAssets) {
    Copy-Item "$root\engine" -Destination $OutDir -Recurse -Force
    Copy-Item "$root\assets" -Destination $OutDir -Recurse -Force
} else {
    Write-Host "Skipped assets (engine/ + assets/ not copied)." -ForegroundColor Yellow
}

# Summary
$size = (Get-ChildItem $OutDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host "Done. Published $([math]::Round($size/1MB,1)) MB to $OutDir" -ForegroundColor Green
if ($projectDir) {
    $publishedProjectPath = Join-Path $OutDir ("projects\" + $projectFolderName)
    Write-Host "  run: & `"$OutDir\MikanEngine.exe`" --project `"$publishedProjectPath`" --no-editor" -ForegroundColor Green
} else {
    Write-Host "  run: & `"$OutDir\MikanEngine.exe`"" -ForegroundColor Green
}
exit 0
