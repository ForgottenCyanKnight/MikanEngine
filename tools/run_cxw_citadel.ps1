param([string]$Gpu = '4090')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$engine=Join-Path $root 'out/build/x64-release/MikanEngine.exe'
if(!(Test-Path -LiteralPath $engine)){throw 'Build MikanEngine first.'}
$settings=@{
    MIKAN_GPU_PREFERENCE=$Gpu
    MIKAN_VOX_HW_BRICK_DDA='0'; MIKAN_HWRT_VOXEL_DDA='0'
    MIKAN_VOX_HW_QUAD_AABB='0'; MIKAN_HWRT_SOFTWARE_QUAD_BVH='0'
}
$previous=@{}
try {
    foreach($key in $settings.Keys){$previous[$key]=[Environment]::GetEnvironmentVariable($key,'Process');[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
    Push-Location $root
    try {& $engine --project (Join-Path $root 'projects/cxw-citadel');$code=$LASTEXITCODE}
    finally {Pop-Location}
} finally {
    foreach($key in $previous.Keys){[Environment]::SetEnvironmentVariable($key,$previous[$key],'Process')}
}
exit $code
