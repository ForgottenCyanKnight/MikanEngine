# Verify the selected official SDK files against the pinned manifest.
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$sdk=Join-Path $root 'dependencies/DLSS'
$manifest=Get-Content -LiteralPath (Join-Path $root 'dependencies/DLSS-sdk-manifest.json') -Encoding UTF8 -Raw|ConvertFrom-Json
if($manifest.commit -notmatch '^[0-9a-f]{40}$' -or -not @($manifest.files).Count){throw 'Invalid DLSS SDK manifest'}
foreach($entry in $manifest.files){
    if([IO.Path]::IsPathRooted($entry.path) -or @($entry.path -split '[\\/]'|Where-Object {$_ -eq '..'}).Count){throw 'Invalid SDK manifest path'}
    $file=Join-Path $sdk $entry.path
    if(-not (Test-Path -LiteralPath $file -PathType Leaf)){throw "Missing DLSS SDK file: $($entry.path); run tools/initialize_submodules.ps1 -DLSSOnly"}
    if((Get-Item -LiteralPath $file).Length -ne $entry.size -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.sha256){throw "DLSS SDK bytes differ: $($entry.path); run tools/initialize_submodules.ps1 -DLSSOnly"}
}
Write-Host "[DLSS] verified $(@($manifest.files).Count) Windows x64 files pinned to $($manifest.commit)"
