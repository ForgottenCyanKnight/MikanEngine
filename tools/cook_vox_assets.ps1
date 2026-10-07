param([Parameter(Mandatory=$true)][string]$SourceRoot,
      [Parameter(Mandatory=$true)][string]$DestinationRoot,
      [switch]$UniformMaterials,
      [switch]$ReferencedOnly = $true)
$ErrorActionPreference='Stop'
$engineRoot=Split-Path -Parent $PSScriptRoot
$cooker=Join-Path $engineRoot 'out\build\x64-Release\MikanVoxRTValidate.exe'
if (!(Test-Path -LiteralPath $cooker)) { throw 'Build MikanVoxRTValidate before publishing VOX assets.' }
$source=[IO.Path]::GetFullPath($SourceRoot).TrimEnd('\')
$destination=[IO.Path]::GetFullPath($DestinationRoot).TrimEnd('\')
if($source.Equals($destination,[StringComparison]::OrdinalIgnoreCase)){throw 'Cook destination must differ from authoring source.'}
$mapping=@{}
$emissiveAssets=@{}
$referenced=@{}
foreach($scene in Get-ChildItem -LiteralPath $source -Recurse -File -Filter '*.json') {
    $document=[IO.File]::ReadAllText($scene.FullName)|ConvertFrom-Json
    foreach($entity in $document.entities) {
        if($entity.voxModel.voxPath){$referenced[([string]$entity.voxModel.voxPath).Replace('\','/')]=$true}
        if($entity.voxModel.voxPath -and $entity.material.emissiveIntensity -gt 0) {
            $emissiveAssets[([string]$entity.voxModel.voxPath).Replace('\','/') ]=$true
        }
    }
}
foreach($file in Get-ChildItem -LiteralPath $source -Recurse -File -Filter '*.vox') {
    $relative=$file.FullName.Substring($source.Length+1)
    if($ReferencedOnly -and !$referenced.ContainsKey($file.FullName.Replace('\','/')) -and !$referenced.ContainsKey($relative.Replace('\','/'))){continue}
    $output=Join-Path $destination ([IO.Path]::ChangeExtension($relative,'.voxmesh'))
    New-Item -ItemType Directory -Path (Split-Path $output) -Force|Out-Null
    $arguments=@('--cook',$file.FullName,$output)
    if($UniformMaterials -or $emissiveAssets.ContainsKey($file.FullName.Replace('\','/')) -or $emissiveAssets.ContainsKey($relative.Replace('\','/'))){$arguments+='--uniform-materials'}
    & $cooker @arguments
    if($LASTEXITCODE -ne 0){throw "VOX cooking failed: $($file.FullName)"}
    $mapping[$file.FullName.Replace('\','/')]=([IO.Path]::ChangeExtension($output,'.voxmesh')).Replace('\','/')
}
# Rewrite JSON string values, preserving every other value and the original layout.
# Source-local absolute paths become publish-local paths relative to the engine root.
$publishRoot=Split-Path -Parent (Split-Path -Parent $destination)
foreach($json in Get-ChildItem -LiteralPath $destination -Recurse -File -Filter '*.json') {
    $text=[IO.File]::ReadAllText($json.FullName)
    $text=[regex]::Replace($text,'"(?:[^"\\]|\\.)*"',{
        param($match)
        $value=$match.Value|ConvertFrom-Json
        if($value -isnot [string]){return $match.Value}
        $normalized=$value.Replace('\','/')
        if($mapping.ContainsKey($normalized)) {
            $value=$mapping[$normalized].Substring($publishRoot.Length+1)
        } elseif($normalized -match '(?i)\.vox$' -and ![IO.Path]::IsPathRooted($value)) {
            $value=$value.Substring(0,$value.Length-4)+'.voxmesh'
        } else {return $match.Value}
        return ConvertTo-Json -InputObject $value -Compress
    })
    [IO.File]::WriteAllText($json.FullName,$text,[Text.UTF8Encoding]::new($false))
    $document=$text|ConvertFrom-Json
    foreach($entity in $document.entities){
        if($entity.voxModel.voxPath){
            $asset=[string]$entity.voxModel.voxPath
            $resolved=if([IO.Path]::IsPathRooted($asset)){$asset}else{Join-Path $publishRoot $asset}
            if($asset -notmatch '(?i)\.voxmesh$' -or !(Test-Path -LiteralPath $resolved)){
                throw "Unresolved VOX surface dependency in $($json.FullName): $asset"
            }
        }
    }
}
if(@(Get-ChildItem -LiteralPath $destination -Recurse -File -Filter '*.vox').Count){throw 'Package still contains VOX source files.'}
$manifestPath=Join-Path $destination 'project.json'
if(Test-Path -LiteralPath $manifestPath) {
    $manifest=[IO.File]::ReadAllText($manifestPath)|ConvertFrom-Json
    $manifest.assets=@($manifest.assets|Where-Object {
        $_ -notmatch '(?i)\.vox\.(bvh|3d|meta)$' -and
        ($_ -notmatch '(?i)\.voxmesh$' -or (Test-Path -LiteralPath (Join-Path $destination $_)))
    })
    [IO.File]::WriteAllText($manifestPath,($manifest|ConvertTo-Json -Depth 100),[Text.UTF8Encoding]::new($false))
}
