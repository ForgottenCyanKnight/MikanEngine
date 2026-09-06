param(
    [string]$Source = (Join-Path $PSScriptRoot '..\resources\windows\mikan_engine_icon.png'),
    [string]$Output = (Join-Path $PSScriptRoot '..\resources\windows\mikan_engine.ico')
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$sourcePath = [IO.Path]::GetFullPath($Source)
$outputPath = [IO.Path]::GetFullPath($Output)
$outputDirectory = [IO.Path]::GetDirectoryName($outputPath)
if (-not (Test-Path -LiteralPath $sourcePath)) {
    throw "Icon source not found: $sourcePath"
}
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$sourceImage = [Drawing.Image]::FromFile($sourcePath)
$pngImages = @()

try {
    foreach ($size in $sizes) {
        $bitmap = [Drawing.Bitmap]::new(
            $size,
            $size,
            [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
            $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
            $graphics.DrawImage($sourceImage, [Drawing.Rectangle]::new(0, 0, $size, $size))
        }
        finally {
            $graphics.Dispose()
        }

        $stream = [IO.MemoryStream]::new()
        try {
            $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
            $pngImages += ,([PSCustomObject]@{
                Size = $size
                Bytes = $stream.ToArray()
            })
        }
        finally {
            $stream.Dispose()
            $bitmap.Dispose()
        }
    }
}
finally {
    $sourceImage.Dispose()
}

$icoStream = [IO.MemoryStream]::new()
$writer = [IO.BinaryWriter]::new($icoStream)
try {
    $writer.Write([UInt16]0) # reserved
    $writer.Write([UInt16]1) # icon type
    $writer.Write([UInt16]$pngImages.Count)

    $dataOffset = 6 + (16 * $pngImages.Count)
    foreach ($image in $pngImages) {
        $dimension = if ($image.Size -ge 256) { 0 } else { $image.Size }
        $writer.Write([Byte]$dimension)
        $writer.Write([Byte]$dimension)
        $writer.Write([Byte]0) # palette colors
        $writer.Write([Byte]0) # reserved
        $writer.Write([UInt16]1) # color planes
        $writer.Write([UInt16]32) # bits per pixel
        $writer.Write([UInt32]$image.Bytes.Length)
        $writer.Write([UInt32]$dataOffset)
        $dataOffset += $image.Bytes.Length
    }

    foreach ($image in $pngImages) {
        $writer.Write($image.Bytes)
    }
    $writer.Flush()
    [IO.File]::WriteAllBytes($outputPath, $icoStream.ToArray())
}
finally {
    $writer.Dispose()
    $icoStream.Dispose()
}

Write-Output "Generated $outputPath ($($pngImages.Count) sizes from $($sizes[0]) to $($sizes[-1]) px)."
