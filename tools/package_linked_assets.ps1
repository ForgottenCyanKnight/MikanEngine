# Materialize linked assets only in a standalone package, never in the authoring project.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$DestinationRoot
)
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath($SourceRoot).TrimEnd('\','/')
$destination = [IO.Path]::GetFullPath($DestinationRoot).TrimEnd('\','/')
function Within([string]$path, [string]$root) {
    return $path.Equals($root, [StringComparison]::OrdinalIgnoreCase) -or
        $path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)
}
if ((Within $destination $source) -or (Within $source $destination)) { throw 'Package and source directories must be disjoint.' }
$manifest = [IO.File]::ReadAllText((Join-Path $source 'project.json')) | ConvertFrom-Json
$resourceRoot = [IO.Path]::GetFullPath((Join-Path $source ([string]$manifest.resourceRoot)))
if ([string]::IsNullOrWhiteSpace([string]$manifest.resourceRoot)) { $resourceRoot = $source }
$outputResources = Join-Path $destination ([string]$manifest.resourceRoot)
if (-not (Within $resourceRoot $source) -or -not (Within ([IO.Path]::GetFullPath($outputResources)) $destination)) {
    throw 'resourceRoot must stay inside the project.'
}
$files = @{}; $roots = @{}; $aliases = @{}; $queue = New-Object 'System.Collections.Generic.Queue[string]'
$utf8 = New-Object Text.UTF8Encoding($false)
$uniformVox = @{}
function AddFile([string]$path) {
    $path = [IO.Path]::GetFullPath($path)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing linked asset/dependency: $path" }
    if (-not $files.ContainsKey($path)) { $files[$path] = $true; $queue.Enqueue($path) }
}
function AddDependency([string]$owner, [string]$dependency) {
    $dependency = [IO.Path]::GetFullPath($dependency)
    AddFile $dependency
    # Binary PMX and OBJ/MTL references must keep their relative directory layout.
    if (-not (Within $owner $source) -and -not (Within $dependency $source)) {
        $common = Split-Path -Parent $owner
        while (-not (Within $dependency $common)) {
            $parent = Split-Path -Parent $common
            if (-not $parent -or $parent -eq $common) { throw "Dependencies span volumes; link a portable asset folder: $owner" }
            $common = $parent
        }
        $roots[$common] = $true
    }
}
function ResolveDependency([string]$base, [string]$reference) {
    if ([IO.Path]::IsPathRooted($reference)) { return [IO.Path]::GetFullPath($reference) }
    return [IO.Path]::GetFullPath((Join-Path $base $reference))
}
function AddLink([string]$path) {
    $path = [IO.Path]::GetFullPath($path)
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing linked resource: $path" }
    if (Test-Path -LiteralPath $path -PathType Container) {
        if (-not (Within $path $resourceRoot)) { $roots[$path] = $true }
        foreach ($file in Get-ChildItem -LiteralPath $path -File -Recurse) {
            if ($file.FullName -match '[\\/](\.git|games)[\\/]' -or $file.Extension -match '^\.blend[12]$') { continue }
            if ($file.FullName -match '[\\/]\.meta[\\/].*[\\/](import\.json|import\.log|model\.dependencies\.json|runtime\.txt)$') { continue }
            AddFile $file.FullName
        }
    } else { AddFile $path }
}
function AbsoluteReferences([string]$text) {
    foreach ($match in [regex]::Matches($text, '"(?:[^"\\]|\\.)*"')) {
        try { $value = $match.Value | ConvertFrom-Json } catch { continue }
        if ($value -is [string] -and [IO.Path]::IsPathRooted($value) -and $value -notmatch '^https?:') {
            # Import diagnostics record original source filenames, not runtime dependencies.
            if (Test-Path -LiteralPath $value -PathType Leaf) { AddFile $value }
            elseif ($value -match '(?i)\.(glb|gltf|pmx|pmd|fbx|obj|png|jpg|jpeg|ktx2|bin|vmd|material)$') {
                throw "Missing absolute resource reference: $value"
            }
        }
    }
}
# Internal assets remain where they already are. Only absolute entries are links.
foreach ($asset in $manifest.assets) { if ([IO.Path]::IsPathRooted([string]$asset)) { AddLink ([string]$asset) } }
foreach ($json in Get-ChildItem -LiteralPath $destination -Recurse -File -Filter '*.json') {
    if ($json.FullName -match '[\\/]\.meta[\\/].*[\\/](import|model.dependencies)\.json$') { continue }
    $text = [IO.File]::ReadAllText($json.FullName)
    AbsoluteReferences $text
    $document = $text | ConvertFrom-Json
    foreach ($entity in $document.entities) {
        if ($entity.voxModel.voxPath -and $entity.material.emissiveIntensity -gt 0) {
            $asset = [string]$entity.voxModel.voxPath
            if ([IO.Path]::IsPathRooted($asset)) { $uniformVox[[IO.Path]::GetFullPath($asset)] = $true }
        }
    }
}
function PmxTextures([string]$path) {
    $stream = [IO.File]::OpenRead($path); $reader = New-Object IO.BinaryReader($stream)
    try {
        if ([Text.Encoding]::ASCII.GetString($reader.ReadBytes(4)) -ne 'PMX ') { throw "Invalid PMX: $path" }
        $null = $reader.ReadSingle(); $header = $reader.ReadBytes($reader.ReadByte())
        $encoding = if ($header[0] -eq 0) { [Text.Encoding]::Unicode } else { [Text.Encoding]::UTF8 }
        for ($i=0; $i -lt 4; $i++) { $null = $reader.ReadBytes($reader.ReadInt32()) }
        $count = $reader.ReadInt32()
        for ($i=0; $i -lt $count; $i++) {
            $stream.Position += 32 + 16 * $header[1]
            switch ($reader.ReadByte()) {
                0 { $stream.Position += $header[5] }
                1 { $stream.Position += 2*$header[5]+4 }
                2 { $stream.Position += 4*$header[5]+16 }
                3 { $stream.Position += 2*$header[5]+40 }
                4 { $stream.Position += 4*$header[5]+16 }
                default { throw "Unsupported PMX weights: $path" }
            }
            $stream.Position += 4
        }
        $indices = $reader.ReadInt32(); $stream.Position += $indices*$header[2]
        $count = $reader.ReadInt32()
        for ($i=0; $i -lt $count; $i++) { $encoding.GetString($reader.ReadBytes($reader.ReadInt32())) }
    } finally { $reader.Dispose(); $stream.Dispose() }
}
while ($queue.Count) {
    $path = $queue.Dequeue(); $parent = Split-Path -Parent $path; $ext = [IO.Path]::GetExtension($path).ToLowerInvariant()
    if ($ext -eq '.blend') {
        $runtime = Join-Path $parent ('.meta\' + [IO.Path]::GetFileName($path) + '\model.glb')
        if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
            throw "Blender source has no runtime conversion. Drag it into a scene before publishing: $path"
        }
        $aliases[$path] = $runtime
        AddFile $runtime
        continue
    }
    if ($ext -eq '.gltf') {
        $gltf = [IO.File]::ReadAllText($path) | ConvertFrom-Json
        foreach ($item in @($gltf.buffers) + @($gltf.images)) {
            if ($item.uri -and $item.uri -notmatch '^data:') {
                AddFile (ResolveDependency $parent ([Uri]::UnescapeDataString([string]$item.uri)))
            }
        }
    }
    if ($ext -eq '.pmx') {
        foreach ($texture in PmxTextures $path) { if ($texture) { AddDependency $path (ResolveDependency $parent $texture) } }
    }
    if ($ext -eq '.obj' -or $ext -eq '.mtl') {
        foreach ($line in [IO.File]::ReadAllLines($path)) {
            if ($line -match '^\s*(?:mtllib|map_\w+|bump|disp)\s+(.+?)\s*$') {
                $dependency = $Matches[1].Trim('"')
                if ($dependency.StartsWith('-')) { throw "Material texture options require linking the containing folder: $path" }
                AddDependency $path (ResolveDependency $parent $dependency)
            }
        }
    }
    if ($ext -in @('.glb','.gltf','.fbx','.pmx','.pmd','.obj','.dae')) {
        foreach ($suffix in @('.import.json','.lights.json')) { if (Test-Path -LiteralPath ($path+$suffix)) { AddFile ($path+$suffix) } }
        $lights = [IO.Path]::ChangeExtension($path,'.lights.json')
        if (Test-Path -LiteralPath $lights) { AddFile $lights }
    }
    if ($ext -in @('.fbx','.pmd','.dae')) {
        # Legacy importers search sibling images and conventional texture folders.
        foreach ($file in Get-ChildItem -LiteralPath $parent -File) {
            if ($file.Extension -match '(?i)^\.(png|jpe?g|tga|bmp|dds|ktx2|hdr)$') { AddFile $file.FullName }
        }
        foreach ($folder in @('textures','texture','tex',([IO.Path]::GetFileNameWithoutExtension($path)+'.fbm'))) {
            $textureFolder = Join-Path $parent $folder
            if (Test-Path -LiteralPath $textureFolder -PathType Container) {
                foreach ($file in Get-ChildItem -LiteralPath $textureFolder -Recurse -File) { AddFile $file.FullName }
            }
        }
    }
    if ($ext -in @('.json','.material')) { AbsoluteReferences ([IO.File]::ReadAllText($path)) }
}
# Choose stable container names; the same source directory is copied just once.
foreach ($path in @($files.Keys)) {
    if (Within $path $resourceRoot) { continue }
    $covered = $false
    foreach ($root in @($roots.Keys)) { if (Within $path $root) { $covered = $true; break } }
    if (-not $covered) { $roots[(Split-Path -Parent $path)] = $true }
}
$containers = @{}; $index = 0
foreach ($root in @($roots.Keys | Sort-Object)) {
    $ancestor = @($roots.Keys | Where-Object { $_ -ne $root -and (Within $root $_) })
    if ($ancestor.Count) { continue }
    $index++
    $containers[$root] = Join-Path $outputResources ('linked\{0:D3}_{1}' -f $index, [IO.Path]::GetFileName($root))
}
function OutputPath([string]$path) {
    $path = [IO.Path]::GetFullPath($path)
    if ($aliases.ContainsKey($path)) { $path = $aliases[$path] }
    if (Within $path $resourceRoot) { return Join-Path $destination $path.Substring($source.Length).TrimStart('\','/') }
    foreach ($root in $containers.Keys) {
        if (Within $path $root) { return Join-Path $containers[$root] $path.Substring($root.Length).TrimStart('\','/') }
    }
    throw "Unmapped linked path: $path"
}
function RelativePath([string]$base, [string]$target) {
    $baseUri = New-Object Uri($base.TrimEnd('\','/') + '\')
    return [Uri]::UnescapeDataString($baseUri.MakeRelativeUri((New-Object Uri($target))).ToString())
}
foreach ($path in @($files.Keys)) {
    if ($aliases.ContainsKey($path) -or [IO.Path]::GetExtension($path) -match '^\.blend[12]?$') { continue }
    $target = OutputPath $path
    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
    if ([IO.Path]::GetExtension($path) -eq '.vox') {
        $cooker = Join-Path (Split-Path $PSScriptRoot) 'out\build\x64-Release\MikanVoxRTValidate.exe'
        $target = [IO.Path]::ChangeExtension($target,'.voxmesh')
        $arguments = @('--cook',$path,$target)
        if ($uniformVox.ContainsKey($path)) { $arguments += '--uniform-materials' }
        & $cooker @arguments
        if ($LASTEXITCODE -ne 0) { throw "Linked VOX cooking failed: $path" }
    } else { Copy-Item -LiteralPath $path -Destination $target -Force }
}
# Rebase JSON values in the output only. Scene paths are resourceRoot-relative;
# glTF URIs are relative to the glTF file. Never modify authoring scenes.
foreach ($json in Get-ChildItem -LiteralPath $destination -Recurse -File | Where-Object { $_.Extension -in @('.json','.material','.gltf') }) {
    $base = $outputResources
    if ($json.Name -eq 'project.json') { $base = $destination }
    $text = [IO.File]::ReadAllText($json.FullName)
    $text = [regex]::Replace($text, '"(?:[^"\\]|\\.)*"', {
        param($match)
        $value = $match.Value | ConvertFrom-Json
        if ($json.Extension -eq '.gltf' -or $value -isnot [string] -or -not [IO.Path]::IsPathRooted($value)) { return $match.Value }
        $key = [IO.Path]::GetFullPath($value)
        if ($files.ContainsKey($key) -or $roots.ContainsKey($key) -or (Within $key $source)) {
            $output = OutputPath $key
            if ([IO.Path]::GetExtension($key) -eq '.vox') { $output = [IO.Path]::ChangeExtension($output,'.voxmesh') }
            return ConvertTo-Json -InputObject (RelativePath $base $output) -Compress
        }
        return $match.Value
    })
    # Relative glTF URI dependencies retain their original directory relationships
    # inside the selected container; dependencies outside it need explicit rebasing.
    if ($json.Extension -eq '.gltf') {
        $original = @($files.Keys | Where-Object { (OutputPath $_) -eq $json.FullName }) | Select-Object -First 1
        if ($original) {
            $document = $text | ConvertFrom-Json
            foreach ($item in @($document.buffers) + @($document.images)) {
                if ($item.uri -and $item.uri -notmatch '^data:') {
                    $dependency = ResolveDependency (Split-Path $original) ([Uri]::UnescapeDataString([string]$item.uri))
                    $item.uri = [Uri]::EscapeUriString((RelativePath (Split-Path $json.FullName) (OutputPath $dependency)))
                }
            }
            $text = $document | ConvertTo-Json -Depth 100
        }
    }
    [IO.File]::WriteAllText($json.FullName, $text, $utf8)
}
Write-Host "Packaged $($files.Count) linked/dependent files in $($containers.Count) shared containers."
