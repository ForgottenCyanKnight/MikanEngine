[CmdletBinding()]
param([string]$OutputRoot='')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
if(-not $OutputRoot){$OutputRoot=Join-Path $taskRoot 'out/system-font-tests'}
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
$null=New-Item -ItemType Directory -Force $OutputRoot
. (Join-Path $PSScriptRoot 'Find-VsDevCmd.ps1')
$taskVs=Find-VsDevCmdPath
if(-not $taskVs){throw 'MSVC environment unavailable'}
$taskSource=Join-Path $taskRoot 'tests/WindowsSystemFontTests.cpp'
$taskProvider=Join-Path $taskRoot 'src/Platform/Windows/SystemFont.cpp'
$taskExe=Join-Path $OutputRoot 'WindowsSystemFontTests.exe'
$taskBatch=Join-Path $OutputRoot 'compile.cmd'
$taskLog=Join-Path $OutputRoot 'compile.log'
@"
@echo off
call "$taskVs" -arch=x64 -host_arch=x64 >nul 2>&1
if errorlevel 1 exit /b 1
cd /d "$OutputRoot"
cl /nologo /std:c++20 /utf-8 /EHsc /W4 /O2 /DNOMINMAX /I"$taskRoot\include" /I"$taskRoot\dependencies" "$taskSource" "$taskProvider" /Fe"$taskExe" /link dwrite.lib >"$taskLog" 2>&1
exit /b %errorlevel%
"@|Set-Content -LiteralPath $taskBatch -Encoding ascii
& $env:ComSpec /c $taskBatch
if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $taskLog;throw 'System font test compilation failed'}
Push-Location $OutputRoot
try { & $taskExe; if($LASTEXITCODE -ne 0){throw 'System font test failed'} }
finally { Pop-Location }
