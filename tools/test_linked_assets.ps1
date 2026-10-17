# CPU-only packaging regression: no Blender, Vulkan or source-project mutations.
[CmdletBinding()]
param([string]$TestRoot = '', [switch]$WithVox)
$ErrorActionPreference = 'Stop'
if (-not $TestRoot) { $TestRoot = Join-Path (Split-Path $PSScriptRoot) ('out\linked-assets-tests\' + (Get-Date -Format 'yyyyMMdd_HHmmss')) }
if (Test-Path -LiteralPath $TestRoot) { throw 'Tests require a new, empty directory.' }
$TestRoot = [IO.Path]::GetFullPath($TestRoot)
$project = Join-Path $TestRoot 'project'
$external = Join-Path $TestRoot ([string][char]0x5171 + [char]0x4eab + ' shared source')
$output = Join-Path $TestRoot 'package'
$utf8 = New-Object Text.UTF8Encoding($false)
function WriteFile([string]$path,[string]$text) {
    New-Item -ItemType Directory -Path (Split-Path $path) -Force | Out-Null
    [IO.File]::WriteAllText($path,$text,$utf8)
}
function Require([bool]$condition,[string]$message) { if (-not $condition) { throw $message } }
$gltf = Join-Path $external 'models\model.gltf'
$bin = Join-Path $external 'models\model.bin'
$texture = Join-Path $external 'images\color.png'
WriteFile $gltf '{"asset":{"version":"2.0"},"buffers":[{"uri":"model.bin","byteLength":4}],"images":[{"uri":"../images/color.png"}]}'
WriteFile $bin '1234'
WriteFile $texture 'texture'
$blend = Join-Path $external 'room.blend'
$converted = Join-Path $external '.meta\room.blend\model.glb'
WriteFile $blend 'source'
WriteFile $converted 'glb'
WriteFile ([IO.Path]::ChangeExtension($converted,'.lights.json')) '{"lights":[]}'
$duplicate = Join-Path $TestRoot 'another source\model.gltf'
WriteFile $duplicate '{"asset":{"version":"2.0"}}'
$folder = Join-Path $external 'folder item'
WriteFile (Join-Path $folder 'sub\item.dat') 'folder asset'
$localSource = Join-Path $project 'authoring\local.png'
WriteFile $localSource 'outside resourceRoot'
$pmx = Join-Path $external 'character\model.pmx'
New-Item -ItemType Directory -Path (Split-Path $pmx) -Force|Out-Null
$writer = New-Object IO.BinaryWriter([IO.File]::Create($pmx))
try {
    $writer.Write([Text.Encoding]::ASCII.GetBytes('PMX ')); $writer.Write([single]2.0)
    $writer.Write([byte]8); $writer.Write([byte[]]@(1,0,1,1,1,1,1,1))
    for ($i=0;$i -lt 4;$i++) { $writer.Write([int]0) }
    $writer.Write([int]0); $writer.Write([int]0); $writer.Write([int]1)
    $name = [Text.Encoding]::UTF8.GetBytes('../images/color.png')
    $writer.Write([int]$name.Length); $writer.Write($name)
} finally { $writer.Dispose() }
$manifest = @{formatVersion=1;name='links';resourceRoot='assets';scene='main.json';assets=@('assets/main.json',$gltf,$blend,$duplicate,$folder,$pmx,$localSource)}
if ($WithVox) {
    $vox = Join-Path $external 'block.vox'
    $writer = New-Object IO.BinaryWriter([IO.File]::Create($vox))
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes('VOX ')); $writer.Write([int]150)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('MAIN')); $writer.Write([int]0); $writer.Write([int]44)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('SIZE')); $writer.Write([int]12); $writer.Write([int]0)
        $writer.Write([int]1); $writer.Write([int]1); $writer.Write([int]1)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('XYZI')); $writer.Write([int]8); $writer.Write([int]0)
        $writer.Write([int]1); $writer.Write([byte[]]@(0,0,0,1))
    } finally { $writer.Dispose() }
    $manifest.assets += $vox
}
WriteFile (Join-Path $project 'project.json') ($manifest|ConvertTo-Json -Depth 10)
$scene = @{entities=@(@{model=@{modelPath=$gltf};material=@{albedoPath=$texture}},@{model=@{modelPath=$converted}})}
if ($WithVox) {
    $scene.entities += @{voxModel=@{voxPath=$vox};material=@{emissiveIntensity=1}}
    WriteFile (Join-Path $project 'assets\internal.vox') 'placeholder'
    Copy-Item -LiteralPath $vox -Destination (Join-Path $project 'assets\internal.vox') -Force
    $scene.entities += @{voxModel=@{voxPath='internal.vox'}}
}
WriteFile (Join-Path $project 'assets\main.json') ($scene|ConvertTo-Json -Depth 10)
$sourceHash = (Get-FileHash -LiteralPath (Join-Path $project 'assets\main.json')).Hash
Copy-Item -LiteralPath $project -Destination $output -Recurse
# publish.ps1 excludes authoring VOX files before both packaging/cooking steps.
if ($WithVox) { Remove-Item -LiteralPath (Join-Path $output 'assets\internal.vox') }
& (Join-Path $PSScriptRoot 'package_linked_assets.ps1') -SourceRoot $project -DestinationRoot $output
Require ((Get-FileHash -LiteralPath (Join-Path $project 'assets\main.json')).Hash -eq $sourceHash) 'Authoring scene changed.'
Require (@(Get-ChildItem -LiteralPath $project -Recurse -Filter '*.gltf').Count -eq 0) 'Model was copied into authoring project.'
$scene = [IO.File]::ReadAllText((Join-Path $output 'assets\main.json')) | ConvertFrom-Json
foreach ($entity in $scene.entities) {
    if (-not $entity.model) { continue }
    $path = [string]$entity.model.modelPath
    Require (-not [IO.Path]::IsPathRooted($path)) 'Scene still references the source drive.'
    Require (Test-Path -LiteralPath (Join-Path $output ('assets\'+$path))) "Missing runtime model: $path"
}
$publishedGltf = Join-Path $output ('assets\'+$scene.entities[0].model.modelPath)
$document = [IO.File]::ReadAllText($publishedGltf)|ConvertFrom-Json
foreach ($item in @($document.buffers)+@($document.images)) {
    Require (Test-Path -LiteralPath (Join-Path (Split-Path $publishedGltf) ([Uri]::UnescapeDataString($item.uri)))) "Missing glTF dependency: $($item.uri)"
}
$publishedManifest = [IO.File]::ReadAllText((Join-Path $output 'project.json'))|ConvertFrom-Json
foreach ($asset in $publishedManifest.assets) {
    Require (-not [IO.Path]::IsPathRooted($asset)) 'Manifest still has an external path.'
    Require (Test-Path -LiteralPath (Join-Path $output $asset)) "Missing manifest asset: $asset"
}
Require (@(Get-ChildItem -LiteralPath $output -Recurse -File -Filter '*.blend').Count -eq 0) 'Blender authoring source entered runtime package.'
$publishedPmx = Get-ChildItem -LiteralPath $output -Recurse -File -Filter '*.pmx'|Select-Object -First 1
Require (Test-Path -LiteralPath (Join-Path $publishedPmx.DirectoryName '../images/color.png')) 'PMX relative texture layout was broken.'
Require (@(Get-ChildItem -LiteralPath $output -Recurse -File -Filter 'item.dat').Count -eq 1) 'Folder link was not packaged.'
& (Join-Path $PSScriptRoot 'cook_vox_assets.ps1') -SourceRoot $project -DestinationRoot $output -ReferencedOnly
if ($WithVox) {
    Require (@(Get-ChildItem -LiteralPath $output -Recurse -File -Filter '*.voxmesh').Count -eq 2) 'Internal or linked VOX was not cooked.'
    Require (@(Get-ChildItem -LiteralPath $output -Recurse -File -Filter '*.vox').Count -eq 0) 'VOX authoring source entered package.'
}
# Relocate only this disposable fixture's source to prove the package is independent.
Move-Item -LiteralPath $external -Destination ($external+'.offline')
Require (Test-Path -LiteralPath $publishedGltf) 'Package depends on original location.'
$failed = $false
try { & (Join-Path $PSScriptRoot 'package_linked_assets.ps1') -SourceRoot $project -DestinationRoot (Join-Path $TestRoot 'missing-package') } catch { $failed = $true }
Require $failed 'Missing link was silently ignored.'
Write-Host "PASS: portable paths, glTF dependencies, Blender metadata, duplicate filenames, source preservation and missing-link failure. $TestRoot"
