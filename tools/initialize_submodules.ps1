# Initialize pinned engine dependencies without fetching unused DLSS platforms.
[CmdletBinding()]
param([switch]$DLSSOnly)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$manifest=Get-Content -LiteralPath (Join-Path $root 'dependencies/DLSS-sdk-manifest.json') -Encoding UTF8 -Raw|ConvertFrom-Json
if(-not $DLSSOnly){
    $paths=@(git -C $root config -f .gitmodules --get-regexp '^submodule\..*\.path$'|ForEach-Object {($_ -split '\s+',2)[1]}|Where-Object {$_ -ne 'dependencies/DLSS'})
    if($LASTEXITCODE -ne 0){throw 'Cannot read submodule paths'}
    if($paths.Count){
        # Current builds consume SDL headers and prebuilt libraries, not its codec source submodules.
        git -C $root submodule update --init -- $paths
        if($LASTEXITCODE -ne 0){throw 'Engine submodule initialization failed'}
    }
}
git -C $root submodule init -- dependencies/DLSS
if($LASTEXITCODE -ne 0){throw 'DLSS submodule registration failed'}
$sdk=Join-Path $root 'dependencies/DLSS'
$existingSdk=$false
if(-not (Test-Path -LiteralPath (Join-Path $sdk '.git'))){
    if((Test-Path -LiteralPath $sdk) -and @(Get-ChildItem -LiteralPath $sdk -Force).Count){throw 'DLSS directory contains non-submodule files; preserve them before initializing'}
    $url=git -C $root config --get submodule.DLSS.url
    if($LASTEXITCODE -ne 0 -or -not $url){throw 'Missing DLSS submodule URL'}
    git clone --filter=blob:none --depth 1 --branch $manifest.tag --no-checkout -- $url $sdk
    if($LASTEXITCODE -ne 0){throw 'DLSS partial clone failed'}
}else{
    $existingSdk=$true
    $dirty=@(git -C $sdk status --porcelain)
    if($LASTEXITCODE -ne 0 -or $dirty.Count){throw 'DLSS has local changes; preserve them before initializing'}
}
$previousAutoCRLF=git -C $sdk config --get core.autocrlf
git -C $sdk config core.autocrlf false
if($LASTEXITCODE -ne 0){throw 'Cannot preserve upstream SDK bytes'}
if($existingSdk -and $previousAutoCRLF -ne 'false'){
    # The worktree was checked clean above. Re-export its tracked files after changing EOL policy.
    git -C $sdk checkout-index --all --force
    if($LASTEXITCODE -ne 0){throw 'Cannot restore original SDK line endings'}
}
$objectType=$manifest.commit|git -C $sdk cat-file '--batch-check=%(objecttype)'
if($LASTEXITCODE -ne 0 -or $objectType -ne 'commit'){
    git -C $sdk fetch --depth 1 origin $manifest.commit
    if($LASTEXITCODE -ne 0){throw 'Cannot fetch pinned DLSS commit'}
}
$patterns=@('/.gitmodules')
foreach($entry in $manifest.files){
    if([IO.Path]::IsPathRooted($entry.path) -or @($entry.path -split '[\\/]'|Where-Object {$_ -eq '..'}).Count){throw 'Invalid SDK manifest path'}
    $patterns+='/'+$entry.path.Replace('\','/')
}
$patterns|git -C $sdk sparse-checkout set --no-cone --stdin
if($LASTEXITCODE -ne 0){throw 'DLSS sparse checkout setup failed'}
git -C $sdk checkout --detach $manifest.commit
if($LASTEXITCODE -ne 0){throw 'DLSS pinned checkout failed'}
git -C $root submodule absorbgitdirs -- dependencies/DLSS
if($LASTEXITCODE -ne 0){throw 'DLSS git directory registration failed'}
& (Join-Path $PSScriptRoot 'validate_dlss_sdk.ps1')
