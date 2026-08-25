# Generate the built-in static primitive meshes as self-contained GLB files.
# The script intentionally has no Blender/Assimp dependency so a clean checkout
# can regenerate the engine primitives on Windows.
[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\engine\models\Base Model')
)

$ErrorActionPreference = 'Stop'

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

function Reset-Mesh {
    $script:positions = New-Object 'System.Collections.Generic.List[object]'
    $script:normals = New-Object 'System.Collections.Generic.List[object]'
    $script:texcoords = New-Object 'System.Collections.Generic.List[object]'
    $script:indices = New-Object 'System.Collections.Generic.List[uint32]'
}

function Add-Vertex(
    [float]$x, [float]$y, [float]$z,
    [float]$nx, [float]$ny, [float]$nz,
    [float]$u, [float]$v
) {
    $index = [uint32]$script:positions.Count
    [void]$script:positions.Add([float[]]@($x, $y, $z))
    [void]$script:normals.Add([float[]]@($nx, $ny, $nz))
    [void]$script:texcoords.Add([float[]]@($u, $v))
    return $index
}

function Add-Triangle([uint32]$a, [uint32]$b, [uint32]$c) {
    [void]$script:indices.Add($a)
    [void]$script:indices.Add($b)
    [void]$script:indices.Add($c)
}

function Add-CylinderMesh {
    param([int]$Segments = 32, [float]$Radius = 0.5, [float]$Height = 1.0)

    Reset-Mesh
    $halfHeight = $Height * 0.5
    $sideBottom = @()
    $sideTop = @()
    for ($j = 0; $j -le $Segments; $j++) {
        $theta = 2.0 * [Math]::PI * $j / $Segments
        $c = [float][Math]::Cos($theta)
        $s = [float][Math]::Sin($theta)
        $u = [float]$j / $Segments
        $sideBottom += Add-Vertex ($Radius * $c) (-$halfHeight) ($Radius * $s) $c 0 $s $u 0
        $sideTop += Add-Vertex ($Radius * $c) $halfHeight ($Radius * $s) $c 0 $s $u 1
    }
    for ($j = 0; $j -lt $Segments; $j++) {
        # Side winding is counter-clockwise when viewed from outside.
        Add-Triangle $sideBottom[$j] $sideTop[$j + 1] $sideBottom[$j + 1]
        Add-Triangle $sideBottom[$j] $sideTop[$j] $sideTop[$j + 1]
    }

    $bottomCenter = Add-Vertex 0 (-$halfHeight) 0 0 -1 0 0.5 0.5
    $topCenter = Add-Vertex 0 $halfHeight 0 0 1 0 0.5 0.5
    $bottomRing = @()
    $topRing = @()
    for ($j = 0; $j -le $Segments; $j++) {
        $theta = 2.0 * [Math]::PI * $j / $Segments
        $c = [float][Math]::Cos($theta)
        $s = [float][Math]::Sin($theta)
        $u = 0.5 + 0.5 * $c
        $v = 0.5 + 0.5 * $s
        $bottomRing += Add-Vertex ($Radius * $c) (-$halfHeight) ($Radius * $s) 0 -1 0 $u $v
        $topRing += Add-Vertex ($Radius * $c) $halfHeight ($Radius * $s) 0 1 0 $u $v
    }
    for ($j = 0; $j -lt $Segments; $j++) {
        Add-Triangle $bottomCenter $bottomRing[$j] $bottomRing[$j + 1]
        Add-Triangle $topCenter $topRing[$j + 1] $topRing[$j]
    }
}

function Add-ConeMesh {
    param([int]$Segments = 32, [float]$Radius = 0.5, [float]$Height = 1.0)

    Reset-Mesh
    $halfHeight = $Height * 0.5
    $slope = $Radius / $Height
    $normalLength = [float][Math]::Sqrt(1.0 + ($slope * $slope))
    $normalY = [float]($slope / $normalLength)
    $normalR = [float](1.0 / $normalLength)
    $bottomRing = @()
    $apexRing = @()
    for ($j = 0; $j -le $Segments; $j++) {
        $theta = 2.0 * [Math]::PI * $j / $Segments
        $c = [float][Math]::Cos($theta)
        $s = [float][Math]::Sin($theta)
        $u = [float]$j / $Segments
        $bottomRing += Add-Vertex ($Radius * $c) (-$halfHeight) ($Radius * $s) ($normalR * $c) $normalY ($normalR * $s) $u 0
        $apexRing += Add-Vertex 0 $halfHeight 0 ($normalR * $c) $normalY ($normalR * $s) $u 1
    }
    for ($j = 0; $j -lt $Segments; $j++) {
        Add-Triangle $bottomRing[$j] $apexRing[$j] $bottomRing[$j + 1]
    }

    $center = Add-Vertex 0 (-$halfHeight) 0 0 -1 0 0.5 0.5
    $capRing = @()
    for ($j = 0; $j -le $Segments; $j++) {
        $theta = 2.0 * [Math]::PI * $j / $Segments
        $c = [float][Math]::Cos($theta)
        $s = [float][Math]::Sin($theta)
        $capRing += Add-Vertex ($Radius * $c) (-$halfHeight) ($Radius * $s) 0 -1 0 (0.5 + 0.5 * $c) (0.5 + 0.5 * $s)
    }
    for ($j = 0; $j -lt $Segments; $j++) {
        Add-Triangle $center $capRing[$j] $capRing[$j + 1]
    }
}

function Add-CapsuleMesh {
    param([int]$Segments = 32, [int]$HemisphereSegments = 8, [float]$Radius = 0.35, [float]$Height = 1.6)

    Reset-Mesh
    $halfCylinder = [Math]::Max(0.0, ($Height - 2.0 * $Radius) * 0.5)
    $descriptors = @()
    $descriptors += ,([float[]]@(0, (-$halfCylinder - $Radius), 0, -1))
    for ($i = 1; $i -le $HemisphereSegments; $i++) {
        $phi = -[Math]::PI * 0.5 + ([Math]::PI * 0.5 * $i / $HemisphereSegments)
        $descriptors += ,([float[]]@(
            ($Radius * [Math]::Cos($phi)),
            ((-$halfCylinder) + ($Radius * [Math]::Sin($phi))),
            ([Math]::Cos($phi)),
            ([Math]::Sin($phi))
        ))
    }
    $descriptors += ,([float[]]@($Radius, $halfCylinder, 1, 0))
    for ($i = 1; $i -le $HemisphereSegments; $i++) {
        $phi = [Math]::PI * 0.5 * $i / $HemisphereSegments
        $descriptors += ,([float[]]@(
            ($Radius * [Math]::Cos($phi)),
            ($halfCylinder + ($Radius * [Math]::Sin($phi))),
            ([Math]::Cos($phi)),
            ([Math]::Sin($phi))
        ))
    }
    $descriptors += ,([float[]]@(0, ($halfCylinder + $Radius), 0, 1))

    $rings = @()
    $ringCount = $descriptors.Count
    for ($i = 0; $i -lt $ringCount; $i++) {
        $descriptor = $descriptors[$i]
        $ring = @()
        $v = if ($ringCount -gt 1) { [float]$i / ($ringCount - 1) } else { 0.0 }
        for ($j = 0; $j -le $Segments; $j++) {
            $theta = 2.0 * [Math]::PI * $j / $Segments
            $c = [float][Math]::Cos($theta)
            $s = [float][Math]::Sin($theta)
            $ring += Add-Vertex ($descriptor[0] * $c) $descriptor[1] ($descriptor[0] * $s) ($descriptor[2] * $c) $descriptor[3] ($descriptor[2] * $s) ([float]$j / $Segments) $v
        }
        $rings += ,$ring
    }
    for ($i = 0; $i -lt ($rings.Count - 1); $i++) {
        for ($j = 0; $j -lt $Segments; $j++) {
            Add-Triangle $rings[$i][$j] $rings[$i + 1][$j + 1] $rings[$i][$j + 1]
            Add-Triangle $rings[$i][$j] $rings[$i + 1][$j] $rings[$i + 1][$j + 1]
        }
    }
}

function Add-TorusMesh {
    param([int]$MajorSegments = 32, [int]$TubeSegments = 16, [float]$MajorRadius = 0.65, [float]$MinorRadius = 0.22)

    Reset-Mesh
    $rings = @()
    for ($i = 0; $i -le $MajorSegments; $i++) {
        $theta = 2.0 * [Math]::PI * $i / $MajorSegments
        $ct = [float][Math]::Cos($theta)
        $st = [float][Math]::Sin($theta)
        $ring = @()
        for ($j = 0; $j -le $TubeSegments; $j++) {
            $phi = 2.0 * [Math]::PI * $j / $TubeSegments
            $cp = [float][Math]::Cos($phi)
            $sp = [float][Math]::Sin($phi)
            $radial = $MajorRadius + $MinorRadius * $cp
            $ring += Add-Vertex ($radial * $ct) ($MinorRadius * $sp) ($radial * $st) ($cp * $ct) $sp ($cp * $st) ([float]$i / $MajorSegments) ([float]$j / $TubeSegments)
        }
        $rings += ,$ring
    }
    for ($i = 0; $i -lt $MajorSegments; $i++) {
        for ($j = 0; $j -lt $TubeSegments; $j++) {
            $a = $rings[$i][$j]
            $b = $rings[$i][$j + 1]
            $c = $rings[$i + 1][$j + 1]
            $d = $rings[$i + 1][$j]
            Add-Triangle $a $b $d
            Add-Triangle $b $c $d
        }
    }
}

function Add-PyramidMesh {
    param([float]$HalfSize = 0.5, [float]$Height = 1.0)

    Reset-Mesh
    $bottom = -$Height * 0.5
    $top = $Height * 0.5
    $apex = [float[]]@(0, $top, 0)
    $corners = @(
        [float[]]@(-$HalfSize, $bottom, -$HalfSize),
        [float[]]@($HalfSize, $bottom, -$HalfSize),
        [float[]]@($HalfSize, $bottom, $HalfSize),
        [float[]]@(-$HalfSize, $bottom, $HalfSize)
    )

    $sidePairs = @(@(0, 1), @(1, 2), @(2, 3), @(3, 0))
    foreach ($pair in $sidePairs) {
        $p0 = $corners[$pair[0]]
        $p1 = $corners[$pair[1]]
        $edge = [float[]]@((($p1[0]) - ($p0[0])), (($p1[1]) - ($p0[1])), (($p1[2]) - ($p0[2])))
        $toApex = [float[]]@((($apex[0]) - ($p0[0])), (($apex[1]) - ($p0[1])), (($apex[2]) - ($p0[2])))
        $nx = $edge[1] * $toApex[2] - $edge[2] * $toApex[1]
        $ny = $edge[2] * $toApex[0] - $edge[0] * $toApex[2]
        $nz = $edge[0] * $toApex[1] - $edge[1] * $toApex[0]
        $length = [Math]::Sqrt($nx * $nx + $ny * $ny + $nz * $nz)
        $nx = [float]($nx / $length); $ny = [float]($ny / $length); $nz = [float]($nz / $length)
        $i0 = Add-Vertex $p0[0] $p0[1] $p0[2] $nx $ny $nz 0 0
        $ia = Add-Vertex $apex[0] $apex[1] $apex[2] $nx $ny $nz 0.5 1
        $i1 = Add-Vertex $p1[0] $p1[1] $p1[2] $nx $ny $nz 1 0
        Add-Triangle $i0 $ia $i1
    }

    $baseIndices = @()
    foreach ($corner in $corners) {
        $baseIndices += Add-Vertex $corner[0] $corner[1] $corner[2] 0 -1 0 0 0
    }
    Add-Triangle $baseIndices[0] $baseIndices[1] $baseIndices[2]
    Add-Triangle $baseIndices[0] $baseIndices[2] $baseIndices[3]
}

function Add-GlbBlock([byte[]]$Data) {
    while (($script:binary.Count % 4) -ne 0) { [void]$script:binary.Add([byte]0) }
    $offset = $script:binary.Count
    [void]$script:binary.AddRange($Data)
    return [pscustomobject]@{ Offset = $offset; Length = $Data.Length }
}

function Write-PrimitiveGlb([string]$Name, [float[]]$Color) {
    $positionValues = New-Object 'System.Collections.Generic.List[float]'
    $normalValues = New-Object 'System.Collections.Generic.List[float]'
    $texcoordValues = New-Object 'System.Collections.Generic.List[float]'
    $minPosition = [float[]]@([float]::PositiveInfinity, [float]::PositiveInfinity, [float]::PositiveInfinity)
    $maxPosition = [float[]]@([float]::NegativeInfinity, [float]::NegativeInfinity, [float]::NegativeInfinity)
    foreach ($value in $script:positions) {
        for ($i = 0; $i -lt 3; $i++) {
            $v = [single]$value[$i]
            [void]$positionValues.Add($v)
            if ($v -lt $minPosition[$i]) { $minPosition[$i] = $v }
            if ($v -gt $maxPosition[$i]) { $maxPosition[$i] = $v }
        }
    }
    foreach ($value in $script:normals) {
        [void]$normalValues.Add([single]$value[0])
        [void]$normalValues.Add([single]$value[1])
        [void]$normalValues.Add([single]$value[2])
    }
    foreach ($value in $script:texcoords) {
        [void]$texcoordValues.Add([single]$value[0])
        [void]$texcoordValues.Add([single]$value[1])
    }

    $script:binary = New-Object 'System.Collections.Generic.List[byte]'
    $positionBlock = Add-GlbBlock (New-FloatBytes $positionValues)
    $normalBlock = Add-GlbBlock (New-FloatBytes $normalValues)
    $texcoordBlock = Add-GlbBlock (New-FloatBytes $texcoordValues)
    $indexBlock = Add-GlbBlock (New-UIntBytes $script:indices)
    $bufferViews = @(
        @{ buffer = 0; byteOffset = $positionBlock.Offset; byteLength = $positionBlock.Length; target = 34962 },
        @{ buffer = 0; byteOffset = $normalBlock.Offset; byteLength = $normalBlock.Length; target = 34962 },
        @{ buffer = 0; byteOffset = $texcoordBlock.Offset; byteLength = $texcoordBlock.Length; target = 34962 },
        @{ buffer = 0; byteOffset = $indexBlock.Offset; byteLength = $indexBlock.Length; target = 34963 }
    )
    $accessors = @(
        @{ bufferView = 0; componentType = 5126; count = $script:positions.Count; type = 'VEC3'; min = @($minPosition[0], $minPosition[1], $minPosition[2]); max = @($maxPosition[0], $maxPosition[1], $maxPosition[2]) },
        @{ bufferView = 1; componentType = 5126; count = $script:normals.Count; type = 'VEC3' },
        @{ bufferView = 2; componentType = 5126; count = $script:texcoords.Count; type = 'VEC2' },
        @{ bufferView = 3; componentType = 5125; count = $script:indices.Count; type = 'SCALAR' }
    )
    $material = @{
        name = 'BuiltinPrimitiveMaterial'
        pbrMetallicRoughness = @{
            baseColorFactor = @($Color[0], $Color[1], $Color[2], 1.0)
            metallicFactor = 0.0
            roughnessFactor = 0.55
        }
    }
    $gltf = [ordered]@{
        asset = @{ version = '2.0'; generator = 'MikanEngine built-in primitive generator' }
        scene = 0
        scenes = @(@{ nodes = @(0) })
        nodes = @(@{ name = $Name; mesh = 0 })
        meshes = @(@{ name = $Name; primitives = @(@{ attributes = @{ POSITION = 0; NORMAL = 1; TEXCOORD_0 = 2 }; indices = 3; material = 0 }) })
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
    $outputPath = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ($Name + '.glb')
    $stream = New-Object System.IO.FileStream($outputPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
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
    Write-Output "[Primitive->GLB] $Name vertices=$($script:positions.Count) triangles=$([int]($script:indices.Count / 3)) output=$outputPath bytes=$totalLength"
}

$outputFullPath = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($outputFullPath) | Out-Null

Add-CylinderMesh
Write-PrimitiveGlb 'cylinder' ([float[]]@(0.24, 0.55, 0.95))

Add-ConeMesh
Write-PrimitiveGlb 'cone' ([float[]]@(0.95, 0.42, 0.18))

Add-CapsuleMesh
Write-PrimitiveGlb 'capsule' ([float[]]@(0.28, 0.78, 0.42))

Add-TorusMesh
Write-PrimitiveGlb 'torus' ([float[]]@(0.58, 0.28, 0.85))

Add-PyramidMesh
Write-PrimitiveGlb 'pyramid' ([float[]]@(0.92, 0.68, 0.18))
