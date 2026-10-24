param(
    [string]$Source='C:/Users/Forgo/Downloads/epic-citadel.8192.packed.cxw',
    [string]$Output='projects/cxw-citadel/assets/epic-citadel.voxmesh',
    [switch]$UniformMaterials,
    [switch]$ClearGlass
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$converter=Join-Path $root 'out/build/x64-release/MikanCxwInspect.exe'
if(!(Test-Path -LiteralPath $converter)){throw 'Build the MikanCxwInspect target first.'}
$sourcePath=if([IO.Path]::IsPathRooted($Source)){$Source}else{Join-Path $root $Source}
$outputPath=if([IO.Path]::IsPathRooted($Output)){$Output}else{Join-Path $root $Output}
New-Item -ItemType Directory -Force (Split-Path $outputPath -Parent) | Out-Null
$arguments=@('--convert',$sourcePath,$outputPath)
if($UniformMaterials){$arguments+='--uniform'}
$previousGlass=$env:MIKAN_CXW_CLEAR_GLASS
try {
    if($ClearGlass){$env:MIKAN_CXW_CLEAR_GLASS='1'; Write-Host 'CXW glass substitution: clear-white dielectric'}
    & $converter @arguments
    if($LASTEXITCODE -ne 0){throw "CXW conversion failed ($LASTEXITCODE)"}
} finally {
    $env:MIKAN_CXW_CLEAR_GLASS=$previousGlass
}
Write-Host "Converted engine asset: $outputPath"
