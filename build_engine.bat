@echo off
call "D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
ninja -C "D:\Engine project\vulkan engine\out\build\x64-Release" EngineMain
set EXIT_CODE=%errorlevel%
if not "%EXIT_CODE%"=="0" (
    echo.
    echo *** BUILD FAILED - exit code %EXIT_CODE% - press any key to close ***
    pause >nul
)
exit /b %EXIT_CODE%
