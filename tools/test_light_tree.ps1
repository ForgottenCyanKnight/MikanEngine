[CmdletBinding()]
param([string]$OutputRoot='', [switch]$Gpu)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
if(-not $OutputRoot){$OutputRoot=Join-Path $taskRoot 'out/light-tree-tests'}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
. (Join-Path $PSScriptRoot 'Find-VsDevCmd.ps1')
$taskVs=Find-VsDevCmdPath
if(-not $taskVs){throw 'MSVC environment unavailable'}
$taskSource=Join-Path $taskRoot 'tests/EmissiveLightTreeTests.cpp'
$taskGlm=Join-Path $taskRoot 'dependencies'
$taskExe=Join-Path $OutputRoot 'EmissiveLightTreeTests.exe'
$taskObj=Join-Path $OutputRoot 'EmissiveLightTreeTests.obj'
$taskBatch=Join-Path $OutputRoot 'compile.cmd'
$taskLog=Join-Path $OutputRoot 'compile.log'
@"
@echo off
call "$taskVs" -arch=x64 -host_arch=x64 >nul 2>&1
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /fp:precise /I"$taskGlm" "$taskSource" /Fo"$taskObj" /Fe"$taskExe" >"$taskLog" 2>&1
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $taskBatch -Encoding ascii
& $env:ComSpec /c $taskBatch
if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $taskLog;throw 'Test compilation failed'}
& $taskExe
if($LASTEXITCODE -ne 0){throw 'Light tree tests failed'}
if($Gpu){
    $taskShader=Join-Path $taskRoot 'tests/EmissiveLightTreeGpu.comp'
    $taskSpv=Join-Path $OutputRoot 'EmissiveLightTreeGpu.spv'
    & (Join-Path $taskRoot 'tools/glslang/bin/glslangValidator.exe') -V --target-env vulkan1.2 $taskShader -o $taskSpv
    if($LASTEXITCODE -ne 0){throw 'GPU test shader compilation failed'}
    $taskSource=Join-Path $taskRoot 'tests/EmissiveLightTreeGpuTests.cpp'
    $taskExe=Join-Path $OutputRoot 'GpuTests.exe'
    $taskObj=Join-Path $OutputRoot 'GpuTests.obj'
    $taskLog=Join-Path $OutputRoot 'gpu-compile.log'
    @"
@echo off
call "$taskVs" -arch=x64 -host_arch=x64 >nul 2>&1
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /fp:precise /I"$taskGlm" "$taskSource" /Fo"$taskObj" /Fe"$taskExe" >"$taskLog" 2>&1
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $taskBatch -Encoding ascii
    & $env:ComSpec /c $taskBatch
    if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $taskLog;throw 'GPU test compilation failed'}
    & $taskExe $taskSpv
    if($LASTEXITCODE -ne 0){throw 'GPU light tree tests failed'}
}
