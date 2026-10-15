@echo off
setlocal EnableExtensions EnableDelayedExpansion

for %%I in ("%~dp0.") do set "ROOT=%%~fI"
set "OUT=%ROOT%\out"
set "RELEASE_DIR=%OUT%\build-release"
set "TEST_DIR=%OUT%\build-test"
set "PACKAGE_DIR=%OUT%\package"
set "ACTION=%~1"
if not defined ACTION set "ACTION=build"

if /I "%ACTION%"=="clean" goto :clean
if /I not "%ACTION%"=="build" if /I not "%ACTION%"=="test" if /I not "%ACTION%"=="package" goto :usage

call :find_qt || exit /b 1
call :find_mingw || exit /b 1
call :find_zlib || exit /b 1

set "PATH=%MINGW_BIN%;%QT_ROOT%\bin;%PATH%"
set "BUILD_DIR=%RELEASE_DIR%"
set "BUILD_TYPE=Release"
set "BUILD_TESTS=OFF"
set "BUILD_TEST_CLI=OFF"
if /I "%ACTION%"=="test" (
    set "BUILD_DIR=%TEST_DIR%"
    set "BUILD_TYPE=Debug"
    set "BUILD_TESTS=ON"
    set "BUILD_TEST_CLI=ON"
)
if not defined DBACKUP_INCLUDE_SOFTWARE_OPENGL set "DBACKUP_INCLUDE_SOFTWARE_OPENGL=OFF"
if /I "%ACTION%"=="test" (
    call :check_openssl || exit /b 1
)

set "QT_PLUGIN_PATH=%QT_ROOT%\plugins"

call :check_cache || exit /b 1
if /I "%ACTION%"=="package" call :check_openssl || exit /b 1

echo [DBackup] Qt: %QT_ROOT%
echo [DBackup] MinGW: %MINGW_BIN%
cmake -S "%ROOT%" -B "%BUILD_DIR%" -G "MinGW Makefiles" ^
  "-DCMAKE_BUILD_TYPE=%BUILD_TYPE%" ^
  "-DCMAKE_C_COMPILER=%C_COMPILER%" ^
  "-DCMAKE_CXX_COMPILER=%CXX_COMPILER%" ^
  "-DCMAKE_MAKE_PROGRAM=%MAKE_PROGRAM%" ^
  "-DCMAKE_PREFIX_PATH=%QT_ROOT%" ^
  "-DZLIB_INCLUDE_DIR=%ZLIB_INCLUDE%" ^
  "-DZLIB_LIBRARY_RELEASE=%ZLIB_LIBRARY%" ^
  "-DDBACKUP_OPENSSL_ROOT=%OPENSSL_ROOT%" ^
  "-DBUILD_TESTS=%BUILD_TESTS%" ^
  "-DDBACKUP_BUILD_TEST_CLI=%BUILD_TEST_CLI%" ^
  "-DDBACKUP_INCLUDE_SOFTWARE_OPENGL=%DBACKUP_INCLUDE_SOFTWARE_OPENGL%"
if errorlevel 1 goto :error

cmake --build "%BUILD_DIR%" --parallel 4
if errorlevel 1 goto :error
for %%D in (libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll) do (
    if not exist "%MINGW_BIN%\%%D" goto :runtime_error
    copy /y "%MINGW_BIN%\%%D" "%BUILD_DIR%\%%D" >nul
    if errorlevel 1 goto :runtime_error
)
if not exist "%QT_ROOT%\bin\Qt6Core.dll" goto :runtime_error
copy /y "%QT_ROOT%\bin\Qt6Core.dll" "%BUILD_DIR%\Qt6Core.dll" >nul
if errorlevel 1 goto :runtime_error
set "PATH=%BUILD_DIR%;%MINGW_BIN%;%QT_ROOT%\bin;%PATH%"

if /I "%ACTION%"=="test" goto :test
if /I "%ACTION%"=="package" goto :package
echo.
echo Build complete: %BUILD_DIR%\BackupTool.exe
exit /b 0

:test
echo [DBackup] Running automated tests...
ctest --test-dir "%BUILD_DIR%" --output-on-failure
if errorlevel 1 goto :error
echo.
echo Build and tests complete.
exit /b 0

:package
cmake --build "%BUILD_DIR%" --target package
if errorlevel 1 goto :error
if not exist "%PACKAGE_DIR%" mkdir "%PACKAGE_DIR%"
for %%Z in ("%BUILD_DIR%\DBackup-*.zip") do if exist "%%~fZ" copy /y "%%~fZ" "%PACKAGE_DIR%\DBackup-portable.zip" >nul
if not exist "%PACKAGE_DIR%\DBackup-portable.zip" goto :error
echo.
echo Portable package: %PACKAGE_DIR%\DBackup-portable.zip
exit /b 0

:find_qt
set "QT_KIT=%QT_ROOT%"
if not defined QT_KIT for /f "delims=" %%Q in ('where qmake6 2^>nul') do if not defined QT_KIT for /f "delims=" %%P in ('"%%Q" -query QT_INSTALL_PREFIX 2^>nul') do set "QT_KIT=%%P"
if not defined QT_KIT if exist "C:\Qt" for /d %%V in ("C:\Qt\6.*") do for /d %%Q in ("%%V\mingw*_64") do if not defined QT_KIT if exist "%%Q\bin\qmake6.exe" set "QT_KIT=%%~fQ"
if not defined QT_KIT goto :qt_error
for %%I in ("%QT_KIT%") do set "QT_KIT=%%~fI"
if not exist "%QT_KIT%\mkspecs\qconfig.pri" goto :qt_error
set "QT_ROOT=%QT_KIT%"
exit /b 0

:qt_error
echo [DBackup] Qt 6 MinGW kit not found. Set QT_ROOT to its kit directory, for example C:\Qt\6.8.3\mingw_64.
exit /b 1

:find_mingw
set "QT_GCC_MAJOR="
set "QT_GCC_MINOR="
set "QT_GCC_PATCH="
for /f "tokens=3" %%V in ('findstr /B "QT_GCC_MAJOR_VERSION =" "%QT_ROOT%\mkspecs\qconfig.pri"') do set "QT_GCC_MAJOR=%%V"
for /f "tokens=3" %%V in ('findstr /B "QT_GCC_MINOR_VERSION =" "%QT_ROOT%\mkspecs\qconfig.pri"') do set "QT_GCC_MINOR=%%V"
for /f "tokens=3" %%V in ('findstr /B "QT_GCC_PATCH_VERSION =" "%QT_ROOT%\mkspecs\qconfig.pri"') do set "QT_GCC_PATCH=%%V"
if not defined QT_GCC_MAJOR goto :mingw_error
if not defined QT_GCC_MINOR goto :mingw_error
if not defined QT_GCC_PATCH goto :mingw_error
set "MINGW_DIR=%QT_ROOT%\..\..\Tools\mingw%QT_GCC_MAJOR%%QT_GCC_MINOR%%QT_GCC_PATCH%_64"
for %%I in ("%MINGW_DIR%") do set "MINGW_DIR=%%~fI"
set "MINGW_BIN=%MINGW_DIR%\bin"
if not exist "%MINGW_BIN%\g++.exe" goto :mingw_error
for /f "delims=" %%V in ('"%MINGW_BIN%\g++.exe" -dumpfullversion') do set "GCC_VERSION=%%V"
set "EXPECTED_GCC=%QT_GCC_MAJOR%.%QT_GCC_MINOR%.%QT_GCC_PATCH%"
if not "%GCC_VERSION%"=="%EXPECTED_GCC%" goto :mingw_error
set "C_COMPILER=%MINGW_BIN%\gcc.exe"
set "CXX_COMPILER=%MINGW_BIN%\g++.exe"
set "MAKE_PROGRAM=%MINGW_BIN%\mingw32-make.exe"
for /f "delims=" %%T in ('"%CXX_COMPILER%" -dumpmachine') do set "MINGW_TRIPLE=%%T"
if not exist "%MAKE_PROGRAM%" goto :mingw_error
exit /b 0

:mingw_error
echo [DBackup] The Qt kit requires matching MinGW %QT_GCC_MAJOR%.%QT_GCC_MINOR%.%QT_GCC_PATCH% under %MINGW_DIR%.
exit /b 1

:find_zlib
set "ZLIB_ROOT=%MINGW_DIR%\%MINGW_TRIPLE%"
set "ZLIB_INCLUDE=%ZLIB_ROOT%\include"
set "ZLIB_LIBRARY=%ZLIB_ROOT%\lib\libz.a"
if not exist "%ZLIB_INCLUDE%\zlib.h" goto :zlib_error
if not exist "%ZLIB_LIBRARY%" goto :zlib_error
exit /b 0

:zlib_error
echo [DBackup] The matching MinGW kit must provide zlib.h and libz.a.
echo [DBackup] Expected: %ZLIB_INCLUDE%\zlib.h and %ZLIB_LIBRARY%
exit /b 1

:check_cache
if not exist "%BUILD_DIR%\CMakeCache.txt" exit /b 0
set "CACHE_CXX="
for /f "tokens=1,* delims==" %%A in ('findstr /B /C:"CMAKE_CXX_COMPILER:" "%BUILD_DIR%\CMakeCache.txt"') do if not defined CACHE_CXX set "CACHE_CXX=%%B"
set "CACHE_CXX=!CACHE_CXX:\=/!"
set "EXPECTED_CXX=!CXX_COMPILER:\=/!"
if /I "!CACHE_CXX!"=="!EXPECTED_CXX!" exit /b 0
echo [DBackup] Removing stale cache from a different compiler in %BUILD_DIR%.
rmdir /s /q "%BUILD_DIR%"
if exist "%BUILD_DIR%" exit /b 1
exit /b 0

:check_openssl
set "OPENSSL_ROOT=%OPENSSL_ROOT_DIR%"
if not defined OPENSSL_ROOT for /f "delims=" %%O in ('where openssl 2^>nul') do if not defined OPENSSL_EXE set "OPENSSL_EXE=%%~fO"
if not defined OPENSSL_ROOT if defined OPENSSL_EXE for %%R in ("%OPENSSL_EXE%\..\..") do set "OPENSSL_ROOT=%%~fR"
if not defined OPENSSL_ROOT goto :openssl_error
for %%I in ("%OPENSSL_ROOT%") do set "OPENSSL_ROOT=%%~fI"
if not exist "%OPENSSL_ROOT%\bin\libssl-3-x64.dll" goto :openssl_error
if not exist "%OPENSSL_ROOT%\bin\libcrypto-3-x64.dll" goto :openssl_error
set "PATH=%OPENSSL_ROOT%\bin;%PATH%"
exit /b 0

:openssl_error
echo [DBackup] OpenSSL 3 runtime not found. Set OPENSSL_ROOT_DIR to its installation root.
exit /b 1

:runtime_error
echo [DBackup] The selected MinGW runtime DLLs could not be copied beside the executables.
exit /b 1

:clean
for %%D in ("%RELEASE_DIR%" "%TEST_DIR%" "%PACKAGE_DIR%") do if exist "%%~fD" rmdir /s /q "%%~fD"
echo Build outputs removed from out\build-release, out\build-test, and out\package.
exit /b 0

:usage
echo Usage: build.bat [test^|package^|clean]
exit /b 2

:error
echo.
echo Build failed. Check the compiler and dependency messages above.
exit /b 1
