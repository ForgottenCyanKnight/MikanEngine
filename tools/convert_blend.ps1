# convert_blend.ps1 - Convert a Blender .blend scene into Mikan assets.
# The actual Blender-side work lives in tools/blender/export_mikan.py.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$InputPath,
    [string]$OutputPath = "",
    [string]$BlenderPath = "",
    [string]$AnimationSourcePath = "",
    [switch]$RetargetAnimations,
    [switch]$InspectOnly
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-FullPath([string]$PathValue) {
    return [System.IO.Path]::GetFullPath($PathValue)
}

function Find-Blender([string]$ExplicitPath) {
    $candidates = New-Object 'System.Collections.Generic.List[string]'
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        [void]$candidates.Add($ExplicitPath)
    }
    if (-not [string]::IsNullOrWhiteSpace($env:BLENDER_EXECUTABLE)) {
        [void]$candidates.Add($env:BLENDER_EXECUTABLE)
    }

    $command = Get-Command blender -ErrorAction SilentlyContinue
    if ($command -and $command.Source) {
        [void]$candidates.Add($command.Source)
    }

    $knownRoots = New-Object 'System.Collections.Generic.List[string]'
    foreach ($programFilesRoot in @(${env:ProgramFiles}, ${env:ProgramW6032})) {
        if (-not [string]::IsNullOrWhiteSpace($programFilesRoot)) {
            [void]$knownRoots.Add((Join-Path $programFilesRoot "Blender Foundation"))
        }
    }
    foreach ($fixedRoot in @("C:\Program Files\Blender Foundation", "D:\Program Files\Blender Foundation")) {
        [void]$knownRoots.Add($fixedRoot)
    }
    $knownRoots = @($knownRoots | Where-Object { Test-Path -LiteralPath $_ })
    foreach ($root in $knownRoots) {
        Get-ChildItem -LiteralPath $root -Filter "blender.exe" -File -Recurse -ErrorAction SilentlyContinue |
            ForEach-Object { [void]$candidates.Add($_.FullName) }
    }

    foreach ($candidate in $candidates) {
        try {
            $full = Resolve-FullPath $candidate
            if (Test-Path -LiteralPath $full -PathType Leaf) {
                return $full
            }
        } catch {
            continue
        }
    }
    throw "找不到 Blender。请通过 -BlenderPath 指定 blender.exe，或设置 BLENDER_EXECUTABLE。"
}

function Read-JsonFile([string]$PathValue) {
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    return ([System.IO.File]::ReadAllText($PathValue, $utf8) | ConvertFrom-Json)
}

$inputFullPath = Resolve-FullPath $InputPath
if (-not (Test-Path -LiteralPath $inputFullPath -PathType Leaf)) {
    throw "输入 .blend 不存在: $inputFullPath"
}
if ([System.IO.Path]::GetExtension($inputFullPath).ToLowerInvariant() -ne ".blend") {
    throw "输入文件必须是 .blend: $inputFullPath"
}

$blenderFullPath = Find-Blender $BlenderPath
$scriptFullPath = Resolve-FullPath (Join-Path $PSScriptRoot "blender\export_mikan.py")
if (-not (Test-Path -LiteralPath $scriptFullPath -PathType Leaf)) {
    throw "Blender 导出脚本不存在: $scriptFullPath"
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($inputFullPath)
    $outputFullPath = Resolve-FullPath (Join-Path $repoRoot (Join-Path "assets\blend_import" $stem))
} else {
    $outputFullPath = Resolve-FullPath $OutputPath
}
New-Item -ItemType Directory -Force -Path $outputFullPath | Out-Null

$sceneName = [System.IO.Path]::GetFileNameWithoutExtension($inputFullPath)
$assetRoot = Resolve-FullPath (Join-Path $repoRoot "assets")
$blenderArgs = @(
    "--background",
    $inputFullPath,
    "--python",
    $scriptFullPath,
    "--",
    "--output",
    $outputFullPath,
    "--asset-root",
    $assetRoot,
    "--scene-name",
    $sceneName
)
if ($RetargetAnimations) {
    if ([string]::IsNullOrWhiteSpace($AnimationSourcePath)) {
        $AnimationSourcePath = Join-Path $repoRoot "assets\animations\quaternius\Animation Library[Standard]\Godot\AnimationLibrary_Godot_Standard.glb"
    }
    $animationSourceFullPath = Resolve-FullPath $AnimationSourcePath
    if (-not (Test-Path -LiteralPath $animationSourceFullPath -PathType Leaf)) {
        throw "动作源文件不存在: $animationSourceFullPath"
    }
    $blenderArgs += "--retarget-animations"
    $blenderArgs += @("--animation-source", $animationSourceFullPath)
}
if ($InspectOnly) {
    $blenderArgs += "--inspect-only"
}

Write-Host "[convert_blend] Blender: $blenderFullPath"
Write-Host "[convert_blend] Input:   $inputFullPath"
Write-Host "[convert_blend] Output:  $outputFullPath"
& $blenderFullPath @blenderArgs
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    throw "Blender 转换失败，退出码: $exitCode"
}

$manifestPath = Join-Path $outputFullPath "manifest.json"
$inspectPath = Join-Path $outputFullPath "inspect.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "转换完成但缺少 manifest.json: $manifestPath"
}
if (-not (Test-Path -LiteralPath $inspectPath -PathType Leaf)) {
    throw "转换完成但缺少 inspect.json: $inspectPath"
}

$manifest = Read-JsonFile $manifestPath
$inspection = Read-JsonFile $inspectPath
if ($RetargetAnimations) {
    if ($null -eq $manifest.animationRetarget -or @($manifest.animationRetarget.clips).Count -eq 0) {
        throw "请求了动作重定向，但 manifest 没有生成任何动作 clip；转换未完成。"
    }
}
if (-not $InspectOnly) {
    $scenePath = Join-Path $outputFullPath "scene.mikan.json"
    if (-not (Test-Path -LiteralPath $scenePath -PathType Leaf)) {
        throw "转换完成但缺少 Mikan 场景文件: $scenePath"
    }
    if ([int]$inspection.renderMeshes -gt 0) {
        $glbPath = Join-Path $outputFullPath (([System.IO.Path]::GetFileNameWithoutExtension($inputFullPath)) + ".glb")
        if (-not (Test-Path -LiteralPath $glbPath -PathType Leaf)) {
            throw "检测到渲染网格，但缺少 GLB: $glbPath"
        }
        if ((Get-Item -LiteralPath $glbPath).Length -lt 20) {
            throw "GLB 文件异常过小: $glbPath"
        }
    }
}

Write-Host "[convert_blend] Done. objects=$($inspection.objectsTotal), renderMeshes=$($inspection.renderMeshes), skinned=$(@($inspection.skinnedMeshes).Count), armatures=$(@($inspection.armatures).Count), cameras=$($inspection.cameras), lights=$($inspection.lights), water=$($inspection.waterObjects), clips=$(@($manifest.animationRetarget.clips).Count)"
Write-Host "[convert_blend] Manifest: $manifestPath"
if (-not $InspectOnly) {
    Write-Host "[convert_blend] Scene:    $(Join-Path $outputFullPath 'scene.mikan.json')"
    if ([int]$inspection.renderMeshes -gt 0) {
        Write-Host "[convert_blend] GLB:      $(Join-Path $outputFullPath (([System.IO.Path]::GetFileNameWithoutExtension($inputFullPath)) + '.glb'))"
    }
}
if ($manifest.warnings) {
    Write-Host "[convert_blend] warnings=$(@($manifest.warnings).Count)"
}
