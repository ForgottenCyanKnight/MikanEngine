@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0compile_shaders.ps1"
exit /b %errorlevel%
