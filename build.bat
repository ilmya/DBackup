@echo off
setlocal

set "BUILD_DIR=build"
set "BUILD_TYPE=Release"

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [DBackup] Configuring %BUILD_TYPE% build...
    cmake -S . -B "%BUILD_DIR%" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
    if errorlevel 1 goto :error
)

echo [DBackup] Building BackupTool.exe...
cmake --build "%BUILD_DIR%" -j 4
if errorlevel 1 goto :error

if /I "%~1"=="test" goto :test
if /I "%~1"=="package" goto :package
if not "%~1"=="" goto :usage

echo.
echo Build complete: %BUILD_DIR%\BackupTool.exe
exit /b 0

:test
echo [DBackup] Running tests...
ctest --test-dir "%BUILD_DIR%" --output-on-failure
if errorlevel 1 goto :error
echo.
echo Build and tests complete.
exit /b 0

:package
echo [DBackup] Creating portable ZIP package...
cmake --build "%BUILD_DIR%" --target package
if errorlevel 1 goto :error
echo.
echo Package created under %BUILD_DIR%\DBackup-*-win64.zip
exit /b 0

:usage
echo Usage: build.bat [test^|package]
exit /b 2

:error
echo.
echo Build failed. Check the messages above.
exit /b 1
