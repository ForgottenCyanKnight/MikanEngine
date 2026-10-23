# Hardware AABB brick traversal; the shared NEE pipeline supports all quality tiers.
[CmdletBinding()]
param(
    [string]$ProjectPath = (Join-Path $PSScriptRoot '..\projects\menger-benchmark'),
    [ValidateSet(2,4,8)][int]$BrickSize = 4,
    [switch]$SuperPerformance
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$engine = Join-Path $root 'out\build\x64-release\MikanEngine.exe'
if (-not (Test-Path -LiteralPath $engine)) { throw 'Build MikanEngine first.' }
$settings = @{
    MIKAN_VOX_HW_BRICK_DDA='1'; MIKAN_VOX_BRICK_SIZE=[string]$BrickSize
    MIKAN_HWRT_VOXEL_DDA='0'; MIKAN_HWRT_DDA_NAADF='0'
    MIKAN_HWRT_DDA_BITSET='0'; MIKAN_HWRT_DDA_SCREEN_BOUNDS='0'
    MIKAN_VOX_HW_QUAD_AABB='0'; MIKAN_HWRT_SOFTWARE_QUAD_BVH='0'
    MIKAN_HWRT_RESTIR_PT='0'; MIKAN_HWRT_DLSS_RR='0'; MIKAN_HWRT_DLSS_SR='0'
    MIKAN_HWRT_PROFILE_MASK=$null; MIKAN_HWRT_PRIMARY_SUN_ONLY=$null
    MIKAN_HWRT_HYBRID=$null; MIKAN_HWRT_HYBRID_SUN=$null
    MIKAN_HWRT_HALF_GI=$(if($SuperPerformance){'2'}else{'0'})
    MIKAN_HWRT_HALF_GI_DENOISE=$(if($SuperPerformance){'1'}else{'0'})
    MIKAN_HWRT_HALF_SPECULAR=$(if($SuperPerformance){'1'}else{'0'})
    MIKAN_HWRT_MAX_WIDTH=$null; MIKAN_HWRT_MAX_HEIGHT=$null
}
$previous=@{}
try {
    foreach($key in $settings.Keys){
        $previous[$key]=[Environment]::GetEnvironmentVariable($key,'Process')
        [Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')
    }
    Push-Location $root
    try { & $engine --project ([IO.Path]::GetFullPath($ProjectPath)); $code=$LASTEXITCODE }
    finally { Pop-Location }
} finally {
    foreach($key in $previous.Keys){[Environment]::SetEnvironmentVariable($key,$previous[$key],'Process')}
}
exit $code
