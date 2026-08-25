# convert_obj_to_glb.ps1 - Convert a static OBJ mesh to a self-contained GLB.
# The converter intentionally keeps the dependency surface small so base engine
# meshes can be generated on a clean checkout without Blender or an Assimp CLI.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InputPath,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

$ErrorActionPreference = "Stop"

function Convert-ObjIndex([string]$Value, [int]$Count) {
    $index = [int]$Value
    if ($index -lt 0) { return $Count + $index }
    return $index - 1
}

function New-FloatBytes([System.Collections.Generic.List[float]]$Values) {
    $bytes = New-Object 'System.Collections.Generic.List[byte]'
    foreach ($value in $Values) {
        [void]$bytes.AddRange([BitConverter]::GetBytes([single]$value))
    }
    return $bytes.ToArray()
}

function New-UIntBytes([System.Collections.Generic.List[uint32]]$Values) {
    $bytes = New-Object 'System.Collections.Generic.List[byte]'
    foreach ($value in $Values) {
        [void]$bytes.AddRange([BitConverter]::GetBytes([uint32]$value))
    }
    return $bytes.ToArray()
}

function Add-GlbBlock([byte[]]$Data) {
    while (($script:binary.Count % 4) -ne 0) { [void]$script:binary.Add([byte]0) }
    $offset = $script:binary.Count
    [void]$script:binary.AddRange($Data)
    return [pscustomobject]@{ Offset = $offset; Length = $Data.Length }
}

$inputFullPath = [System.IO.Path]::GetFullPath($InputPath)
$outputFullPath = [System.IO.Path]::GetFullPath($OutputPath)
$inputDirectory = Split-Path -Parent $inputFullPath
$meshName = [System.IO.Path]::GetFileNameWithoutExtension($inputFullPath)

$positions = New-Object 'System.Collections.Generic.List[object]'
$normals = New-Object 'System.Collections.Generic.List[object]'
$texcoords = New-Object 'System.Collections.Generic.List[object]'
$outPositions = New-Object 'System.Collections.Generic.List[object]'
$outNormals = New-Object 'System.Collections.Generic.List[object]'
$outTexcoords = New-Object 'System.Collections.Generic.List[object]'
$indices = New-Object 'System.Collections.Generic.List[uint32]'
$vertexMap = @{}
$mtlReference = ""
$materialName = "DefaultMaterial"

$reader = New-Object System.IO.StreamReader($inputFullPath, [System.Text.Encoding]::UTF8, $true)
try {
    while (($line = $reader.ReadLine()) -ne $null) {
        $trim = $line.Trim()
        if ($trim.Length -eq 0 -or $trim.StartsWith('#')) { continue }

        if ($trim.StartsWith('mtllib ')) {
            $mtlReference = $trim.Substring(7).Trim()
            continue
        }
        if ($trim.StartsWith('usemtl ')) {
            $materialName = $trim.Substring(7).Trim()
            if ([string]::IsNullOrWhiteSpace($materialName)) { $materialName = "DefaultMaterial" }
            continue
        }
        if ($trim.StartsWith('v ')) {
            $parts = $trim.Substring(2).Trim() -split '\s+'
            if ($parts.Count -ge 3) {
                $positions.Add([float[]]@([single]$parts[0], [single]$parts[1], [single]$parts[2]))
            }
            continue
        }
        if ($trim.StartsWith('vn ')) {
            $parts = $trim.Substring(3).Trim() -split '\s+'
            if ($parts.Count -ge 3) {
                $normals.Add([float[]]@([single]$parts[0], [single]$parts[1], [single]$parts[2]))
            }
            continue
        }
        if ($trim.StartsWith('vt ')) {
            $parts = $trim.Substring(3).Trim() -split '\s+'
            if ($parts.Count -ge 2) {
                $texcoords.Add([float[]]@([single]$parts[0], [single]$parts[1]))
            }
            continue
        }
        if (-not $trim.StartsWith('f ')) { continue }

        $face = @($trim.Substring(2).Trim() -split '\s+')
        if ($face.Count -lt 3) { continue }
        $faceIndices = New-Object 'System.Collections.Generic.List[uint32]'
        foreach ($token in $face) {
            $parts = $token -split '/', 3
            if ($parts.Count -lt 1 -or [string]::IsNullOrWhiteSpace($parts[0])) { continue }
            $positionIndex = Convert-ObjIndex $parts[0] $positions.Count
            if ($positionIndex -lt 0 -or $positionIndex -ge $positions.Count) { throw "Position index out of range: $token" }

            $texcoordIndex = -1
            if ($parts.Count -ge 2 -and $parts[1] -ne '') {
                $texcoordIndex = Convert-ObjIndex $parts[1] $texcoords.Count
            }
            $normalIndex = -1
            if ($parts.Count -ge 3 -and $parts[2] -ne '') {
                $normalIndex = Convert-ObjIndex $parts[2] $normals.Count
            }
            $key = "$positionIndex/$texcoordIndex/$normalIndex"
            if (-not $vertexMap.ContainsKey($key)) {
                $vertexMap[$key] = [uint32]$outPositions.Count
                [void]$outPositions.Add($positions[$positionIndex])
                if ($normalIndex -ge 0 -and $normalIndex -lt $normals.Count) {
                    [void]$outNormals.Add($normals[$normalIndex])
                } else {
                    [void]$outNormals.Add([float[]]@(0, 1, 0))
                }
                if ($texcoordIndex -ge 0 -and $texcoordIndex -lt $texcoords.Count) {
                    [void]$outTexcoords.Add($texcoords[$texcoordIndex])
                } else {
                    [void]$outTexcoords.Add([float[]]@(0, 0))
                }
            }
            [void]$faceIndices.Add([uint32]$vertexMap[$key])
        }

        for ($i = 1; $i -lt ($faceIndices.Count - 1); $i++) {
            [void]$indices.Add($faceIndices[0])
            [void]$indices.Add($faceIndices[$i])
            [void]$indices.Add($faceIndices[$i + 1])
        }
    }
} finally {
    $reader.Dispose()
}

if ($outPositions.Count -eq 0 -or $indices.Count -eq 0) {
    throw "No mesh geometry found in $inputFullPath"
}

$kd = [float[]]@(0.8, 0.8, 0.8)
$alpha = [single]1.0
$mtlCandidates = New-Object 'System.Collections.Generic.List[string]'
if (-not [string]::IsNullOrWhiteSpace($mtlReference)) {
    [void]$mtlCandidates.Add((Join-Path $inputDirectory $mtlReference))
}
[void]$mtlCandidates.Add((Join-Path $inputDirectory ($meshName + '.mtl')))
$mtlPath = $mtlCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($mtlPath) {
    foreach ($line in [System.IO.File]::ReadLines($mtlPath, [System.Text.Encoding]::UTF8)) {
        $trim = $line.Trim()
        if ($trim.StartsWith('Kd ')) {
            $parts = $trim.Substring(3).Trim() -split '\s+'
            if ($parts.Count -ge 3) { $kd = [float[]]@([single]$parts[0], [single]$parts[1], [single]$parts[2]) }
        } elseif ($trim.StartsWith('d ')) {
            $alpha = [single]($trim.Substring(2).Trim())
        } elseif ($trim.StartsWith('Tr ')) {
            $alpha = [single](1.0 - [single]($trim.Substring(3).Trim()))
        }
    }
}

$positionValues = New-Object 'System.Collections.Generic.List[float]'
$normalValues = New-Object 'System.Collections.Generic.List[float]'
$texcoordValues = New-Object 'System.Collections.Generic.List[float]'
$minPosition = [float[]]@( [float]::PositiveInfinity, [float]::PositiveInfinity, [float]::PositiveInfinity )
$maxPosition = [float[]]@( [float]::NegativeInfinity, [float]::NegativeInfinity, [float]::NegativeInfinity )
foreach ($value in $outPositions) {
    for ($i = 0; $i -lt 3; $i++) {
        $v = [single]$value[$i]
        [void]$positionValues.Add($v)
        if ($v -lt $minPosition[$i]) { $minPosition[$i] = $v }
        if ($v -gt $maxPosition[$i]) { $maxPosition[$i] = $v }
    }
}
foreach ($value in $outNormals) {
    [void]$normalValues.Add([single]$value[0]); [void]$normalValues.Add([single]$value[1]); [void]$normalValues.Add([single]$value[2])
}
foreach ($value in $outTexcoords) {
    [void]$texcoordValues.Add([single]$value[0]); [void]$texcoordValues.Add([single]$value[1])
}

$script:binary = New-Object 'System.Collections.Generic.List[byte]'
$positionBlock = Add-GlbBlock (New-FloatBytes $positionValues)
$normalBlock = Add-GlbBlock (New-FloatBytes $normalValues)
$texcoordBlock = Add-GlbBlock (New-FloatBytes $texcoordValues)
$indexBlock = Add-GlbBlock (New-UIntBytes $indices)

$bufferViews = @(
    @{ buffer = 0; byteOffset = $positionBlock.Offset; byteLength = $positionBlock.Length; target = 34962 },
    @{ buffer = 0; byteOffset = $normalBlock.Offset; byteLength = $normalBlock.Length; target = 34962 },
    @{ buffer = 0; byteOffset = $texcoordBlock.Offset; byteLength = $texcoordBlock.Length; target = 34962 },
    @{ buffer = 0; byteOffset = $indexBlock.Offset; byteLength = $indexBlock.Length; target = 34963 }
)
$accessors = @(
    @{ bufferView = 0; componentType = 5126; count = $outPositions.Count; type = 'VEC3'; min = @($minPosition[0], $minPosition[1], $minPosition[2]); max = @($maxPosition[0], $maxPosition[1], $maxPosition[2]) },
    @{ bufferView = 1; componentType = 5126; count = $outNormals.Count; type = 'VEC3' },
    @{ bufferView = 2; componentType = 5126; count = $outTexcoords.Count; type = 'VEC2' },
    @{ bufferView = 3; componentType = 5125; count = $indices.Count; type = 'SCALAR' }
)
$material = @{
    name = $materialName
    pbrMetallicRoughness = @{
        baseColorFactor = @($kd[0], $kd[1], $kd[2], $alpha)
        metallicFactor = 0.0
        roughnessFactor = 0.5
    }
}
$gltf = [ordered]@{
    asset = @{ version = '2.0'; generator = 'MikanEngine OBJ to GLB converter' }
    scene = 0
    scenes = @(@{ nodes = @(0) })
    nodes = @(@{ name = $meshName; mesh = 0 })
    meshes = @(@{ name = $meshName; primitives = @(@{ attributes = @{ POSITION = 0; NORMAL = 1; TEXCOORD_0 = 2 }; indices = 3; material = 0 }) })
    materials = @($material)
    accessors = $accessors
    bufferViews = $bufferViews
    buffers = @(@{ byteLength = $script:binary.Count })
}

$jsonText = $gltf | ConvertTo-Json -Depth 20 -Compress
$jsonBytes = [System.Text.Encoding]::UTF8.GetBytes($jsonText)
while (($jsonBytes.Length % 4) -ne 0) {
    $jsonText += ' '
    $jsonBytes = [System.Text.Encoding]::UTF8.GetBytes($jsonText)
}
$binBytes = $script:binary.ToArray()
$totalLength = 12 + 8 + $jsonBytes.Length + 8 + $binBytes.Length

$outputParent = Split-Path -Parent $outputFullPath
if ($outputParent) { [System.IO.Directory]::CreateDirectory($outputParent) | Out-Null }
$stream = New-Object System.IO.FileStream($outputFullPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
$writer = New-Object System.IO.BinaryWriter($stream)
try {
    $writer.Write([uint32]0x46546C67)
    $writer.Write([uint32]2)
    $writer.Write([uint32]$totalLength)
    $writer.Write([uint32]$jsonBytes.Length)
    $writer.Write([uint32]0x4E4F534A)
    $writer.Write($jsonBytes)
    $writer.Write([uint32]$binBytes.Length)
    $writer.Write([uint32]0x004E4942)
    $writer.Write($binBytes)
} finally {
    $writer.Dispose()
    $stream.Dispose()
}

Write-Output ("[OBJ->GLB] $meshName vertices=$($outPositions.Count) triangles=$([int]($indices.Count / 3)) output=$outputFullPath bytes=$totalLength")
