@echo off
setlocal
cd /d "%~dp0"

set "MINGW=C:\Program Files (x86)\Dev-Cpp\MinGW64\bin"
if exist "%MINGW%\g++.exe" set "PATH=%MINGW%;%PATH%"

where g++ >nul 2>nul
if errorlevel 1 (
    echo [ERROR] g++ not found. Install MinGW-w64 and add it to PATH.
    exit /b 1
)

if not exist dist mkdir dist

set "CXXFLAGS=-std=c++11 -O2 -D_WIN32_WINNT=0x0600 -DWIN32_LEAN_AND_MEAN -Wall -Wextra -Wno-unused-parameter -Isrc"
set "LDFLAGS=-static -static-libgcc -static-libstdc++"
set "LIBS=-lwinhttp -lws2_32"
set "CORE=src\common.cpp src\net.cpp src\fetcher.cpp src\validator.cpp src\pool.cpp src\server.cpp src\app.cpp"

echo [1/3] compiling console build ...
g++ %CXXFLAGS% %CORE% src\main.cpp -o dist\proxypool.exe %LDFLAGS% %LIBS%
if errorlevel 1 goto :fail

echo [2/3] compiling gui build ...
g++ %CXXFLAGS% %CORE% src\gui.cpp -o dist\proxypool-gui.exe -mwindows %LDFLAGS% ^
    -lgdiplus -ldwmapi -lshell32 -lcomctl32 -lgdi32 -luser32 %LIBS%
if errorlevel 1 goto :fail

echo [3/3] copying config ...
if exist sources.txt copy /y sources.txt dist\sources.txt >nul

echo.
echo Build OK:
echo   dist\proxypool.exe       (console)
echo   dist\proxypool-gui.exe   (window, pick nodes by hand)
exit /b 0

:fail
echo.
echo Build FAILED
exit /b 1
