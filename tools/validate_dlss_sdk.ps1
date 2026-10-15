# Verify the selected official SDK files and the pinned submodule commit.
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$sdk=Join-Path $root 'dependencies/DLSS'
$manifest=Get-Content -LiteralPath (Join-Path $root 'dependencies/DLSS-sdk-manifest.json') -Encoding UTF8 -Raw|ConvertFrom-Json
$commit=git -C $sdk rev-parse HEAD
if($LASTEXITCODE -ne 0 -or $commit -ne $manifest.commit){throw 'DLSS is not at the pinned SDK commit; run tools/initialize_submodules.ps1'}
if(-not @($manifest.files).Count){throw 'Empty SDK manifest'}
foreach($entry in $manifest.files){
    if([IO.Path]::IsPathRooted($entry.path) -or @($entry.path -split '[\\/]'|Where-Object {$_ -eq '..'}).Count){throw 'Invalid SDK manifest path'}
    $file=Join-Path $sdk $entry.path
    if(-not (Test-Path -LiteralPath $file -PathType Leaf)){throw "Missing DLSS SDK file: $($entry.path); run tools/initialize_submodules.ps1"}
    if((Get-Item -LiteralPath $file).Length -ne $entry.size -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256){throw "DLSS SDK bytes differ: $($entry.path); initialize with tools/initialize_submodules.ps1 to preserve upstream line endings"}
}
Write-Host "[DLSS] verified $(@($manifest.files).Count) files at $commit"
