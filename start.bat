@echo off
setlocal
cd /d "%~dp0"

set /p VER=<VERSION
echo [GameHQ] v%VER%

set "CMAKE=%~dp0tools\cmake\bin\cmake.exe"
set "NINJA=%~dp0tools\ninja.exe"
set "BUILD_OK=0"

if not exist "%CMAKE%" (
    echo [GameHQ] Local toolchain not found - launching the last packaged build.
    goto restart
)

set "QT="
for /f "tokens=1-3 delims=|" %%I in ('powershell.exe -NoProfile -ExecutionPolicy Bypass -File "packaging\select-dev-qt.ps1"') do (
    set "QT=%%I"
    set "MINGW=%%J"
    set "NEED_CONFIG=%%K"
)
if not defined QT (
    echo [GameHQ] ERROR: the developer Qt kit could not be selected. See docs\dev-setup.md.
    exit /b 1
)
set "PATH=%MINGW%;%QT%\bin;%PATH%"

if "%NEED_CONFIG%"=="1" (
    echo [GameHQ] Configuring the developer build tree in out\...
    rem --fresh keeps FetchContent sub-builds, which break when the tree moves to another drive or path.
    if exist "out\_deps" for /d %%D in ("out\_deps\*-subbuild") do rmdir /s /q "%%D"
    "%CMAKE%" -S . -B out --fresh -G Ninja -DCMAKE_BUILD_TYPE=Debug ^
      -DGAMEHQ_ENABLE_PSEUDO_LOCALES=ON ^
      "-DCMAKE_PREFIX_PATH=%QT%" ^
      "-DQt6_DIR=%QT:\=/%/lib/cmake/Qt6" ^
      "-DCMAKE_C_COMPILER=%MINGW%\gcc.exe" ^
      "-DCMAKE_CXX_COMPILER=%MINGW%\g++.exe" ^
      "-DCMAKE_MAKE_PROGRAM=%NINJA%"
    if errorlevel 1 (
        echo [GameHQ] Configure failed - launching the last packaged build.
        goto restart
    )
)

echo [GameHQ] Building required app components in out\...
"%CMAKE%" --build out --target GameHQ GameHQLauncher GameHQUpdater
if errorlevel 1 (
    echo [GameHQ] Build failed - launching the last packaged build.
    goto restart
)
set "BUILD_OK=1"

:restart
rem Stop the running app before refreshing DLLs and executables in build\app.
taskkill /F /IM GameHQ.exe >nul 2>&1
taskkill /F /IM SavePlay.exe >nul 2>&1
taskkill /F /IM PlayHQ.exe >nul 2>&1

if "%BUILD_OK%"=="1" (
    echo [GameHQ] Refreshing clean package in build\...
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "packaging\assemble-package.ps1" ^
      -BuildDirectory out -Destination build -PreserveUserData
    if errorlevel 1 echo [GameHQ] Package refresh failed - launching the last packaged build.
)

if not exist "build\GameHQ.exe" (
    echo [GameHQ] ERROR: build\GameHQ.exe not found. See docs\dev-setup.md for setup.
    exit /b 1
)

start "" "%~dp0build\GameHQ.exe"
echo [GameHQ] Started clean package build\GameHQ.exe
exit /b 0
