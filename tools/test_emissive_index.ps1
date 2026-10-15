[CmdletBinding()]
param([string]$OutputRoot='')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
if(-not $OutputRoot){$OutputRoot=Join-Path $taskRoot 'out/emissive-index-tests'}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
. (Join-Path $PSScriptRoot 'Find-VsDevCmd.ps1')
$taskVs=Find-VsDevCmdPath
if(-not $taskVs){throw 'MSVC environment unavailable'}
$taskSource=Join-Path $taskRoot 'tests/EmissiveLightIndexTests.cpp'
$taskGlm=Join-Path $taskRoot 'dependencies/glm'
$taskExe=Join-Path $OutputRoot 'EmissiveLightIndexTests.exe'
$taskObj=Join-Path $OutputRoot 'EmissiveLightIndexTests.obj'
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
if($LASTEXITCODE -ne 0){throw 'Emissive index tests failed'}
