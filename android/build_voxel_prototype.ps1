[CmdletBinding()]
param(
    [switch]$PrepareOnly,
    [switch]$Offline
)

$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = Join-Path $repo 'out/android-voxel-sandbox'
$source = Join-Path $repo 'projects/voxel-sandbox'
$stagedProject = Join-Path $output 'project'
$stagedScenes = Join-Path $stagedProject 'scenes'
$encoding = [System.Text.UTF8Encoding]::new($false)
New-Item -ItemType Directory -Force -Path $stagedScenes | Out-Null

$manifest = Get-Content -LiteralPath (Join-Path $source 'project.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$scene = Get-Content -LiteralPath (Join-Path $source $manifest.scene) -Raw -Encoding UTF8 | ConvertFrom-Json
$camera = $scene.entities | Where-Object { $_.camera.isMainCamera } | Select-Object -First 1
$world = $scene.entities | Where-Object { $_.voxelWorld.enabled } | Select-Object -First 1
if (!$camera -or !$world) {
    throw 'The voxel sandbox must contain an active voxel world and a main camera.'
}

# The advanced terrain column at (44,95) has its top block at Y=130.
$spawn = @(44.5, 133.5, 95.5)
$halfYaw = -42.0 * [math]::PI / 360.0
$halfPitch = -24.0 * [math]::PI / 360.0
$cy = [math]::Cos($halfYaw)
$sy = [math]::Sin($halfYaw)
$cp = [math]::Cos($halfPitch)
$sp = [math]::Sin($halfPitch)
$camera.transform.position = $spawn
$camera.transform.rotation = @($cy * $cp; $cy * $sp; $sy * $cp; -$sy * $sp)
$camera.camera.farPlane = 320
$camera.camera.postProcessChain = 'postprocess_raster.json'
$camera.camera.enableHiZCulling = $false
$camera.camera.showFrustumWireframe = $false
$world.voxelWorld.renderRadius = 12
$world.voxelWorld.terrainType = 2
$world.voxelWorld.spawnPosition = $spawn
foreach ($entity in $scene.entities) {
    if ($entity.light) { $entity.light.castShadow = $false }
}

$manifest.scene = 'scenes/main.json'
$manifest.resourceRoot = '.'
$manifest.game = ''
$manifest.editorPostProcessChain = 'postprocess_raster.json'
$manifest.assets = @($manifest.scene, 'postprocess_raster.json')
[System.IO.File]::WriteAllText((Join-Path $stagedScenes 'main.json'), ($scene | ConvertTo-Json -Depth 40), $encoding)
[System.IO.File]::WriteAllText((Join-Path $stagedProject 'project.json'), ($manifest | ConvertTo-Json -Depth 20), $encoding)
Copy-Item -LiteralPath (Join-Path $source 'postprocess_raster.json') -Destination $stagedProject -Force

& (Join-Path $PSScriptRoot 'sync_assets.ps1') -ProjectPath $stagedProject -VoxelPrototype
$assets = Join-Path $output 'assets'
[System.IO.File]::WriteAllText((Join-Path $assets 'mikan_android_voxel_prototype.txt'), "1`n", $encoding)
if ($PrepareOnly) { return }

$gradleApk = Join-Path $PSScriptRoot 'app/build/outputs/apk/debug/app-debug.apk'
# Recreate the ZIP so incremental packaging does not retain gaps from removed assets.
if (Test-Path -LiteralPath $gradleApk -PathType Leaf) {
    Remove-Item -LiteralPath $gradleApk -Force
}

$gradleArgs = @(':app:assembleDebug', '-PmikanVoxelPrototype=true', '--console=plain')
if ($Offline) { $gradleArgs += '--offline' }
Push-Location -LiteralPath $PSScriptRoot
try {
    & ./gradlew.bat @gradleArgs
    if ($LASTEXITCODE -ne 0) { throw "Android build failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}

$apk = Join-Path $output 'voxel-sandbox-arm64.apk'
Copy-Item -LiteralPath $gradleApk -Destination $apk -Force
Write-Host "APK: $apk"
