[CmdletBinding()]
param(
    [string]$DevEcoHome = 'C:/Program Files/Huawei/DevEco Studio',
    [switch]$PrepareOnly,
    [switch]$Emulator
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repo 'projects/voxel-sandbox'
$raw = Join-Path $PSScriptRoot 'entry/src/main/resources/rawfile'
$output = Join-Path $repo 'out/harmony-voxel-sandbox'
$utf8 = [System.Text.UTF8Encoding]::new($false)
New-Item -ItemType Directory -Path $raw, $output -Force | Out-Null

$engine = Join-Path $repo 'engine'
& robocopy.exe $engine $raw /E /XD glsl .git /XF *.tmp /NFL /NDL /NJH /NJS /NP
if ($LASTEXITCODE -ge 8) { throw "Engine resource staging failed ($LASTEXITCODE)." }

$manifest = Get-Content -LiteralPath (Join-Path $source 'project.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$scene = Get-Content -LiteralPath (Join-Path $source $manifest.scene) -Raw -Encoding UTF8 | ConvertFrom-Json
$camera = $scene.entities | Where-Object { $_.camera.isMainCamera } | Select-Object -First 1
$world = $scene.entities | Where-Object { $_.voxelWorld.enabled } | Select-Object -First 1
if (!$camera -or !$world) { throw 'The voxel sandbox needs a main camera and an enabled voxel world.' }

# Match the mobile prototype's noise-generated terrain and ground-checked spawn.
$spawn = @(44.5, 133.5, 95.5)
$yaw = -42.0 * [math]::PI / 360.0
$pitch = -24.0 * [math]::PI / 360.0
$cy = [math]::Cos($yaw)
$sy = [math]::Sin($yaw)
$cp = [math]::Cos($pitch)
$sp = [math]::Sin($pitch)
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

$scenePath = 'scenes/main.json'
New-Item -ItemType Directory -Path (Join-Path $raw 'scenes') -Force | Out-Null
[System.IO.File]::WriteAllText((Join-Path $raw $scenePath), ($scene | ConvertTo-Json -Depth 40), $utf8)
[System.IO.File]::WriteAllText((Join-Path $raw 'mikan_harmony_scene.txt'), "$scenePath`n", $utf8)
Copy-Item -LiteralPath (Join-Path $source 'postprocess_raster.json') -Destination $raw -Force

foreach ($resource in 'data/blocks.csv', 'fonts/simhei.ttf', 'textures/Blocks.png',
    'shaders/spv/world.vert.spv', 'shaders/spv/world.frag.spv', 'shaders/spv/tonemap.frag.spv') {
    if (!(Test-Path -LiteralPath (Join-Path $raw $resource) -PathType Leaf)) {
        throw "Required HarmonyOS resource missing: $resource. Build the engine shaders before packaging."
    }
}
if ($PrepareOnly) { return }

$vendor = Join-Path $repo 'out/vendor'
$assimpSource = Join-Path $vendor 'assimp-6.0.4/CMakeLists.txt'
if (!(Test-Path -LiteralPath $assimpSource -PathType Leaf)) {
    New-Item -ItemType Directory -Path $vendor -Force | Out-Null
    $archive = Join-Path $vendor 'assimp-v6.0.4.tar.gz'
    if (!(Test-Path -LiteralPath $archive -PathType Leaf)) {
        Invoke-WebRequest -Uri 'https://codeload.github.com/assimp/assimp/tar.gz/refs/tags/v6.0.4' -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne 'AFA5487EFDD285661AFA842C85187CD8C541EDAD92E8D4AA85BE4FCA7476ECCC') {
        throw "Unexpected Assimp archive hash: $archive"
    }
    & tar.exe -xf $archive -C $vendor
    if ($LASTEXITCODE -ne 0) { throw "Assimp extraction failed ($LASTEXITCODE)." }
}

$env:DEVECO_SDK_HOME = Join-Path $DevEcoHome 'sdk'
$env:NODE_HOME = Join-Path $DevEcoHome 'tools/node'
$env:JAVA_HOME = Join-Path $DevEcoHome 'jbr'
$hvigor = Join-Path $DevEcoHome 'tools/hvigor/bin/hvigorw.bat'
$product = if ($Emulator) { 'emulator' } else { 'default' }
$architecture = if ($Emulator) { 'x86_64' } else { 'arm64' }
$hapDir = Join-Path $PSScriptRoot "entry/build/$product/outputs/default"
Push-Location -LiteralPath $PSScriptRoot
try {
    & $hvigor --mode module -p "product=$product" -p module=entry@default -p buildMode=release assembleHap --no-daemon
    if ($LASTEXITCODE -ne 0) { throw "HarmonyOS build failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}

$hap = Get-ChildItem -LiteralPath $hapDir -Filter 'entry-default-*.hap' -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$hap) { throw 'Hvigor did not produce a HAP.' }
$artifact = Join-Path $output ($hap.Name.Replace('entry-default', "voxel-sandbox-$architecture"))
Copy-Item -LiteralPath $hap.FullName -Destination $artifact -Force
Write-Host "HAP: $artifact"
