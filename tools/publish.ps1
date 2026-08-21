# Publish.ps1 - package the current build into a standalone release folder
#   (exe + engine/editor dlls + runtime dependency dlls + game plugin dlls + assets)
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\publish.ps1            # -> <root>\publish\ (full assets)
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\publish.ps1 -OutDir D:\release
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\publish.ps1 -SkipAssets   # skip 410MB assets
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\publish.ps1 -Project baka3d  # publish only one project
param(
    [string]$OutDir = "",
    [switch]$SkipAssets,
    [switch]$SkipGames,
    [string]$Project = "",
    [switch]$IncludeEditor,
    [switch]$IncludeKtx
)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$build = "$root\out\build\x64-Release"
if (-not $OutDir) { $OutDir = "$root\publish" }

if (-not (Test-Path "$build\EngineMain.exe")) {
    Write-Host "ERROR: build not found. Run tools\build.ps1 -Target EngineMain first." -ForegroundColor Red
    exit 1
}

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Write-Host "Publishing to: $OutDir" -ForegroundColor Green

# 1. Engine exe + runtime core dll (Game.dll). Editor.dll is dev-only: include it
#    only when -IncludeEditor is passed (standalone game builds run without editor).
Copy-Item "$build\EngineMain.exe" -Destination $OutDir -Force
Copy-Item "$build\Game.dll" -Destination $OutDir -Force
if ($IncludeEditor -and (Test-Path "$build\Editor.dll")) { Copy-Item "$build\Editor.dll" -Destination $OutDir -Force }
elseif (-not $IncludeEditor) { Write-Host "Editor.dll excluded (game build; use -IncludeEditor to keep it)." -ForegroundColor Cyan }

# 2. Runtime dependency dlls (SDL3 / SDL3_image / assimp / ktx).
#    ktx.dll is lazy-loaded by TexturePool only when a KTX2 texture is actually
#    decoded - PNG-only projects don't need it. -Project mode excludes it by
#    default (add -IncludeKtx for projects using .ktx2 assets).
foreach ($dep in @("SDL3.dll", "SDL3_image.dll", "assimp-vc143-mtd.dll")) {
    if (Test-Path "$build\$dep") { Copy-Item "$build\$dep" -Destination $OutDir -Force }
    else { Write-Host "WARN: dependency dll missing in build: $dep" -ForegroundColor Yellow }
}
if ($IncludeKtx -or -not $Project) {
    if (Test-Path "$build\ktx.dll") { Copy-Item "$build\ktx.dll" -Destination $OutDir -Force }
}
else { Write-Host "ktx.dll excluded (no KTX2 assets; use -IncludeKtx to keep it)." -ForegroundColor Cyan }

# 3. Game plugin dlls: all of them for full publish; only the project's own plugin for -Project mode
if (-not $SkipGames) {
    if ($Project) {
        $projDll = "$build\Game$Project.dll"
        if (Test-Path $projDll) { Copy-Item $projDll -Destination $OutDir -Force }
        else { Write-Host "WARN: plugin dll not found: Game$Project.dll" -ForegroundColor Yellow }
    } else {
        $plugins = Get-ChildItem "$build" -Filter "Game*.dll" | Where-Object { $_.Name -ne "Game.dll" }
        foreach ($p in $plugins) { Copy-Item $p.FullName -Destination $OutDir -Force }
    }
}

# 4. Engine system assets + project assets next to the exe (ProjectManager auto-detects)
if ($Project) {
    # Projectized publish: copy engine system assets (minus optional unused ones:
    # skybox/ cubemap faces, bluenoise, voxel Blocks atlas - see EngineAssets comments)
    # + only the project folder (no games/ sources - runtime uses plugin dlls), then
    # write a publish-local projects.json so the project list points at the new location.
    robocopy "$root\engine" "$OutDir\engine" /MIR /XD skybox /XF bluenoise.png Blocks.png > $null
    if ($LASTEXITCODE -ge 8) { Write-Host "WARN: robocopy(engine) exited $LASTEXITCODE" -ForegroundColor Yellow }
    $projRel = "projects\$Project"
    $projDir = "$root\$projRel"
    if (-not (Test-Path "$projDir\project.json")) {
        Write-Host "ERROR: project not found (missing project.json): $projDir" -ForegroundColor Red
        exit 1
    }
    $destProj = Join-Path $OutDir $projRel
    New-Item -ItemType Directory -Path $destProj -Force | Out-Null
    robocopy $projDir $destProj /MIR /XD games > $null
    if ($LASTEXITCODE -ge 8) { Write-Host "WARN: robocopy exited $LASTEXITCODE" -ForegroundColor Yellow }

    # Publish-local project registry. Store a RELATIVE path (projects/<name>) so the
    # publish folder stays portable - ResolveProjectPath anchors it to the engine root
    # (= publish folder) at load time. Backslashes must be JSON-escaped (\ -> \\).
    $jsonRel = $projRel.Replace('\', '\\')
    $json = '{"projects":[{"name":"' + $Project + '","path":"' + $jsonRel + '","lastOpened":0}]}'
    [System.IO.File]::WriteAllText((Join-Path $OutDir "projects.json"), $json, [System.Text.UTF8Encoding]::new($false))
    Write-Host "Published project: $Project -> $destProj (games/ sources excluded)" -ForegroundColor Green
} elseif (-not $SkipAssets) {
    Copy-Item "$root\engine" -Destination $OutDir -Recurse -Force
    Copy-Item "$root\assets" -Destination $OutDir -Recurse -Force
} else {
    Write-Host "Skipped assets (engine/ + assets/ not copied)." -ForegroundColor Yellow
}

# Summary
$size = (Get-ChildItem $OutDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host "Done. Published $([math]::Round($size/1MB,1)) MB to $OutDir" -ForegroundColor Green
Write-Host "  run: & `"$OutDir\EngineMain.exe`"" -ForegroundColor Green
exit 0
