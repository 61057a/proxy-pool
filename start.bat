@echo off
cd /d "%~dp0"

if not exist dist\proxypool.exe (
    echo [ERROR] dist\proxypool.exe not found. Run build.bat first.
    pause
    exit /b 1
)

echo Starting proxy pool on http://127.0.0.1:8080 ...
echo Press Ctrl+C to stop.
echo.

dist\proxypool.exe --port 8080
pause
