param(
    [string]$ProjectPath = (Join-Path $PSScriptRoot '..\projects\vox\vox model'),
    [switch]$DisableFG,
    [switch]$UseNRD,
    [ValidateSet('quality','balanced','performance','ultraperformance','dlaa','0')][string]$SuperResolution = 'quality'
)
$ErrorActionPreference = 'Stop'
$engine = Join-Path $PSScriptRoot '..\out\build\x64-release\MikanEngine.exe'
if (-not (Test-Path -LiteralPath $engine)) { throw 'Build the x64-release engine first.' }
if (-not (Test-Path -LiteralPath $ProjectPath -PathType Container)) { throw "Project not found: $ProjectPath" }
$previousFG = $env:MIKAN_DLSS_FG
$previousRR = $env:MIKAN_HWRT_DLSS_RR
$previousSR = $env:MIKAN_HWRT_DLSS_SR
try {
    $env:MIKAN_DLSS_FG = if ($DisableFG) { '0' } else { '1' }
    $env:MIKAN_HWRT_DLSS_RR = if ($UseNRD) { '0' } else { '1' }
    $env:MIKAN_HWRT_DLSS_SR = $SuperResolution
    Write-Host ('DLSS FG={0}, DLSS RR={1}; standalone game view, disable VSync in settings.' -f $env:MIKAN_DLSS_FG,$env:MIKAN_HWRT_DLSS_RR)
    & $engine --project ([System.IO.Path]::GetFullPath($ProjectPath)) --no-editor
} finally {
    $env:MIKAN_DLSS_FG = $previousFG
    $env:MIKAN_HWRT_DLSS_RR = $previousRR
    $env:MIKAN_HWRT_DLSS_SR = $previousSR
}
