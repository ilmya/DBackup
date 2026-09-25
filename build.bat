@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "BUILD_DIR=build"
set "BUILD_TYPE=Release"
set "QT_CMAKE_ARG="

if /I "%~1"=="core-test" goto :core_test
if defined QT_ROOT set "QT_CMAKE_ARG=-DCMAKE_PREFIX_PATH=%QT_ROOT%"
if not defined QT_ROOT for /f "delims=" %%Q in ('where qmake6 2^>nul') do (
    for /f "delims=" %%P in ('"%%Q" -query QT_INSTALL_PREFIX') do set "QT_ROOT=%%P"
)
if not defined QT_ROOT (
    for /d %%V in (C:\Qt\6.*) do for /d %%Q in ("%%V\mingw*_64") do if exist "%%Q\bin\qmake6.exe" set "QT_ROOT=%%Q"
)
if defined QT_ROOT set "QT_CMAKE_ARG=-DCMAKE_PREFIX_PATH=%QT_ROOT%"
if not defined QT_ROOT (
    echo [DBackup] Qt 6 was not found.
    echo Install Qt 6.5 or newer with the matching compiler kit, then either:
    echo   1. Run this script from the Qt command prompt, or
    echo   2. Set QT_ROOT to a kit directory such as C:\Qt\6.8.3\mingw_64
    echo.
    echo Core tests can still run without Qt: build.bat core-test
    exit /b 1
)

rem A standard Qt Online Installer layout keeps its matching MinGW under Tools.
rem Prefer that compiler over unrelated MinGW installations already on PATH.
if exist "C:\Qt\Tools" (
    for /d %%T in (C:\Qt\Tools\mingw*) do set "QT_MINGW_BIN=%%T\bin"
    if defined QT_MINGW_BIN set "PATH=%QT_MINGW_BIN%;%QT_ROOT%\bin;%PATH%"
)

:configure
echo [DBackup] Configuring Qt 6 application...
cmake -S . -B "%BUILD_DIR%" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% %QT_CMAKE_ARG%
if errorlevel 1 goto :error
echo [DBackup] Building BackupTool.exe...
cmake --build "%BUILD_DIR%" -j 4
if errorlevel 1 goto :error
if exist "%QT_ROOT%\bin\windeployqt.exe" (
    echo [DBackup] Deploying Qt runtime next to BackupTool.exe...
    "%QT_ROOT%\bin\windeployqt.exe" --release --no-translations "%BUILD_DIR%\BackupTool.exe"
    if errorlevel 1 goto :error
    if exist "%BUILD_DIR%\DBackupServer.exe" (
        set "OPENSSL_BIN="
        for /f "delims=" %%O in ('where openssl 2^>nul') do if not defined OPENSSL_BIN set "OPENSSL_BIN=%%~dpO"
        if defined OPENSSL_BIN for %%R in ("!OPENSSL_BIN!..") do set "OPENSSL_ROOT=%%~fR"
        if defined OPENSSL_ROOT (
            "%QT_ROOT%\bin\windeployqt.exe" --release --no-translations --openssl-root "!OPENSSL_ROOT!" "%BUILD_DIR%\DBackupServer.exe"
        ) else (
            "%QT_ROOT%\bin\windeployqt.exe" --release --no-translations "%BUILD_DIR%\DBackupServer.exe"
            echo [DBackup] Warning: OpenSSL 3 was not found. DBackupServer PEM TLS needs qopensslbackend and OpenSSL runtime DLLs.
        )
        if errorlevel 1 goto :error
    )
)
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

:core_test
set "CORE_DIR=build-core"
echo [DBackup] Building and testing the core without Qt...
cmake -S . -B "%CORE_DIR%" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DDBACKUP_BUILD_GUI=OFF -DBUILD_TESTS=ON
if errorlevel 1 goto :error
cmake --build "%CORE_DIR%" -j 4
if errorlevel 1 goto :error
ctest --test-dir "%CORE_DIR%" --output-on-failure
if errorlevel 1 goto :error
exit /b 0

:package
echo [DBackup] Creating portable ZIP with Qt runtime...
cmake --build "%BUILD_DIR%" --target package
if errorlevel 1 goto :error
echo.
echo Package created under %BUILD_DIR%\DBackup-*.zip
exit /b 0

:usage
echo Usage: build.bat [test^|package^|core-test]
exit /b 2

:error
echo.
echo Build failed. Check the messages above.
exit /b 1
