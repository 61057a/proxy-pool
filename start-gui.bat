@echo off
cd /d "%~dp0"

if not exist dist\proxypool-gui.exe (
    echo [ERROR] dist\proxypool-gui.exe not found. Run build.bat first.
    pause
    exit /b 1
)

start "" dist\proxypool-gui.exe %*
