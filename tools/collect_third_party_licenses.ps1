# Validate and copy retained third-party license originals without network access.
[CmdletBinding(DefaultParameterSetName='Copy')]
param(
    [Parameter(Mandatory=$true,ParameterSetName='Copy')][string]$OutDir,
    [Parameter(Mandatory=$true,ParameterSetName='Validate')][switch]$ValidateOnly
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$bundle=Join-Path $root 'docs\licenses\third-party'
$manifestPath=Join-Path $bundle 'manifest.json'
$utf8=[Text.UTF8Encoding]::new($false)

function Resolve-ContainedPath([string]$Base,[string]$Relative) {
    if([string]::IsNullOrWhiteSpace($Relative) -or [IO.Path]::IsPathRooted($Relative) -or
       @($Relative -split '[\\/]' | Where-Object {$_ -eq '..'}).Count -gt 0){throw "Invalid license path: $Relative"}
    $baseFull=[IO.Path]::GetFullPath($Base).TrimEnd('\','/')
    $full=[IO.Path]::GetFullPath((Join-Path $baseFull $Relative))
    if(-not $full.StartsWith($baseFull+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){
        throw "License path escapes directory: $Relative"
    }
    return $full
}
if(-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)){throw "Missing license manifest: $manifestPath"}
$manifest=ConvertFrom-Json ([IO.File]::ReadAllText($manifestPath,$utf8))
if($manifest.schemaVersion -ne 1 -or @($manifest.files).Count -eq 0){throw 'Invalid or empty license manifest'}
$seen=@{}
foreach($entry in $manifest.files){
    $path=Resolve-ContainedPath $bundle $entry.path
    if($seen.ContainsKey($entry.path)){throw "Duplicate license path: $($entry.path)"}
    $seen[$entry.path]=$true
    if(-not (Test-Path -LiteralPath $path -PathType Leaf)){throw "Missing license original: $($entry.path)"}
    $actual=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if($actual -ne $entry.sha256 -or (Get-Item -LiteralPath $path).Length -ne $entry.size){throw "License hash/size mismatch: $($entry.path)"}
    # A vendor update must not silently leave the collected snapshot stale.
    $source=Resolve-ContainedPath $root $entry.sourcePath
    if(-not (Test-Path -LiteralPath $source -PathType Leaf)){throw "Missing vendor license: $($entry.sourcePath)"}
    if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $entry.sha256){throw "Vendor license changed; refresh manifest: $($entry.sourcePath)"}
}
foreach($name in @('LICENSE','THIRD_PARTY_NOTICES.md')){
    if(-not (Test-Path -LiteralPath (Join-Path $root $name) -PathType Leaf)){throw "Missing root notice: $name"}
}
Write-Host "[LICENSES] verified $(@($manifest.files).Count) originals across $(@($manifest.components).Count) component groups"
if($ValidateOnly){return}

$destination=[IO.Path]::GetFullPath($OutDir).TrimEnd('\','/')
$rootFull=[IO.Path]::GetFullPath($root).TrimEnd('\','/')
if($destination.Equals($rootFull,[StringComparison]::OrdinalIgnoreCase) -or
   $destination.Equals([IO.Path]::GetFullPath($bundle),[StringComparison]::OrdinalIgnoreCase)){
    throw 'License output must be a release directory, not the source or manifest directory.'
}
$targetBundle=Join-Path $destination 'licenses\third-party'
New-Item -ItemType Directory -Path $targetBundle -Force|Out-Null
foreach($entry in $manifest.files){
    $source=Resolve-ContainedPath $bundle $entry.path
    $target=Resolve-ContainedPath $targetBundle $entry.path
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force|Out-Null
    Copy-Item -LiteralPath $source -Destination $target -Force
    if((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $entry.sha256){throw "Published license verification failed: $($entry.path)"}
}
Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $targetBundle 'manifest.json') -Force
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $destination 'LICENSE') -Force
$notice=[IO.File]::ReadAllText((Join-Path $root 'THIRD_PARTY_NOTICES.md'),$utf8)
$notice=$notice.Replace('(docs/licenses/third-party/manifest.json)','(licenses/third-party/manifest.json)')
[IO.File]::WriteAllText((Join-Path $destination 'THIRD_PARTY_NOTICES.md'),$notice,$utf8)
Write-Host "[LICENSES] copied and verified license bundle: $targetBundle"
