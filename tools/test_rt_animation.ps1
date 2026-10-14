# CPU/import regression tests for the RT dynamic-model path. Rendering tests
# remain separate because they require a Vulkan ray-query device.
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$build=Join-Path $root 'out\build\x64-Release'
if(-not (Test-Path -LiteralPath (Join-Path $build 'Game.dll'))){throw 'Build MikanEngine/Game before running these tests.'}
. (Join-Path $PSScriptRoot 'Find-VsDevCmd.ps1')
$vs=Find-VsDevCmdPath
if(-not $vs){throw 'VsDevCmd.bat not found'}
$run=Join-Path $root ('out\rt_animation_tests\'+(Get-Date -Format 'yyyyMMdd_HHmmss_fff'))
if(Test-Path -LiteralPath $run){throw 'Test output directory already exists'}
New-Item -ItemType Directory -Path $run -Force|Out-Null
$cmd=@"
@echo off
call "$vs" -arch=x64 -host_arch=x64 >nul 2>&1
if errorlevel 1 exit /b 1
cd /d "$run"
cl /nologo /std:c++20 /EHsc /MD /O2 /utf-8 /I"$root\include" /I"$root\dependencies" "$root\tests\RayTracingDeformationTests.cpp" /Fe:RayTracingDeformationTests.exe /Fo:RayTracingDeformationTests.obj
if errorlevel 1 exit /b 1
RayTracingDeformationTests.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /MD /O2 /utf-8 /DMIKAN_USE_GAME /I"$root\include" /I"$root\dependencies" "$root\tests\RayTracingAnimationImportTests.cpp" /Fe:RayTracingAnimationImportTests.exe /Fo:RayTracingAnimationImportTests.obj /link /LIBPATH:"$build" Game.lib
if errorlevel 1 exit /b 1
set "PATH=$build;%PATH%"
RayTracingAnimationImportTests.exe "$root\tests\fixtures\rt_skin_bend.gltf"
exit /b %errorlevel%
"@
$commandPath=Join-Path $run 'run.cmd'
[IO.File]::WriteAllText($commandPath,$cmd,[Text.UTF8Encoding]::new($false))
& cmd.exe /c "`"$commandPath`"" > (Join-Path $run 'tests.log') 2>&1
$rc=$LASTEXITCODE
Write-Output "RT_ANIMATION_TEST_LOG=$(Join-Path $run 'tests.log')"
if($rc -ne 0){throw "RT animation regression failed (exit=$rc)"}
Write-Output 'PASS: deformation math and animated glTF import regression'
