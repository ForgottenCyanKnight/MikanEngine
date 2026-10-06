# Reproduce the AMD quality baseline on the selected Vulkan GPU.
[CmdletBinding()]
param(
    [string]$ProjectPath = (Join-Path $PSScriptRoot '..\projects\vox\vox model'),
    [switch]$NativeResolution,
    [switch]$GameMode
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$engine=Join-Path $root 'out\build\x64-Release\MikanEngine.exe'
if(-not (Test-Path -LiteralPath $engine)){throw 'Build Game and the engine host first.'}
if(-not (Test-Path -LiteralPath $ProjectPath -PathType Container)){throw 'Project path does not exist.'}
$settings=@{
    MIKAN_HWRT_RTXDI='0';MIKAN_HWRT_RTXDI_SP='0'
    MIKAN_HWRT_RESTIR='1';MIKAN_HWRT_RESTIR_GI='1'
    MIKAN_HWRT_DLSS_RR='0';MIKAN_HWRT_DLSS_SR='0';MIKAN_DLSS_FG='0'
    MIKAN_HWRT_FIREFLY_CLAMP='1';MIKAN_HWRT_FIREFLY_SCALE='1'
    MIKAN_HWRT_ANTIFIREFLY='1';MIKAN_HWRT_RR='1';MIKAN_HWRT_TAA='1'
    MIKAN_HWRT_EMISSIVE_TRIANGLES='1'
    MIKAN_HWRT_PROFILE_MASK=$null;MIKAN_HWRT_PROFILE_MIRRORS=$null
    MIKAN_HWRT_MAX_WIDTH=$(if($NativeResolution){$null}else{'960'})
    MIKAN_HWRT_MAX_HEIGHT=$(if($NativeResolution){$null}else{'540'})
}
$previous=@{}
try{
    foreach($key in $settings.Keys){$previous[$key]=[Environment]::GetEnvironmentVariable($key,'Process');[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
    Write-Host 'Self ReSTIR DI/GI + firefly clamp + NRD diffuse/specular + TAA; Russian roulette ON; RTXDI/DLSS/FG OFF.'
    Write-Host $(if($NativeResolution){'Using the normal selected-GPU resolution cap.'}else{'Matched AMD comparison cap: 960x540.'})
    $argsList=@('--project',[IO.Path]::GetFullPath($ProjectPath));if($GameMode){$argsList+='--no-editor'}
    & $engine @argsList
    $code=$LASTEXITCODE
}finally{
    foreach($key in $previous.Keys){[Environment]::SetEnvironmentVariable($key,$previous[$key],'Process')}
}
exit $code
