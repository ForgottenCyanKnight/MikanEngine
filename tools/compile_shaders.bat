@echo off
setlocal

set GLSLANG_VALIDATOR="%~dp0glslang\bin\glslangValidator.exe"
set GLSL_DIR=%~dp0..\engine\shaders\glsl
set SPV_DIR=%~dp0..\engine\shaders\spv

if not exist "%SPV_DIR%" mkdir "%SPV_DIR%"

echo Compiling GLSL shaders to SPIR-V...
echo.

set ERR=0

for %%f in ("%GLSL_DIR%\*.vert") do (
    echo Compiling: %%~nxf
    %GLSLANG_VALIDATOR% -V "%%f" -o "%SPV_DIR%\%%~nf.vert.spv"
    if errorlevel 1 set ERR=1
)

for %%f in ("%GLSL_DIR%\*.frag") do (
    echo Compiling: %%~nxf
    %GLSLANG_VALIDATOR% -V "%%f" -o "%SPV_DIR%\%%~nf.frag.spv"
    if errorlevel 1 set ERR=1
)

for %%f in ("%GLSL_DIR%\*.comp") do (
    echo Compiling: %%~nxf
    %GLSLANG_VALIDATOR% -V "%%f" -o "%SPV_DIR%\%%~nf.comp.spv"
    if errorlevel 1 set ERR=1
)

echo.
if "%ERR%"=="1" (
    echo *** COMPILE FAILED — 按任意键关闭 ***
    pause >nul
    endlocal
    exit /b 1
)
echo Done! SPIR-V files are in: %SPV_DIR%
endlocal
