@echo off
setlocal EnableExtensions EnableDelayedExpansion

:: ---------------------------------------------------------------------------
:: Full release build: JUCE Standalone + VST3, then installer
:: Run from the workspace root.
:: Usage: build_windows.bat [x86|x64|arm64] [--simd=avx2|avx|sse2]
::   --simd=avx2  (default) /arch:AVX2 - Intel Haswell 2013+, AMD Excavator 2015+
::   --simd=avx   /arch:AVX  - Intel Sandy Bridge / AMD Bulldozer 2011+; keeps
::                every 256-bit kernel in core/src/dsp/simd/SimdMath.h
::   --simd=sse2  no /arch: flag - CPUs with no AVX at all
::   --no-avx2 is an alias for --simd=avx, --no-avx an alias for --simd=sse2
:: ---------------------------------------------------------------------------

set "WORKSPACE_ROOT=%~dp0"
set "JUCE_BUILDS=%WORKSPACE_ROOT%juce\Builds"
set "INSTALLER_SCRIPT=%WORKSPACE_ROOT%juce\packaging\build-installer.bat"
set "UI_DIR=%WORKSPACE_ROOT%core\ui"
set "CORE_SIMD_LEVEL=avx2"

:parse_args
if "%~1"=="" goto :args_done
:: --no-avx drops AVX entirely; --no-avx2 keeps AVX and only gives up AVX2.
if /I "%~1"=="--no-avx" (
    set "CORE_SIMD_LEVEL=sse2"
    shift
    goto :parse_args
)
if /I "%~1"=="--no-avx2" (
    set "CORE_SIMD_LEVEL=avx"
    shift
    goto :parse_args
)
:: cmd.exe splits on "=" as well as spaces, so "--simd=avx" arrives here as two
:: separate tokens; consuming the next token handles both spellings.
if /I "%~1"=="--simd" (
    set "CORE_SIMD_LEVEL=%~2"
    shift
    shift
    goto :parse_args
)
if not defined ARCH_INPUT (
    set "ARCH_INPUT=%~1"
    shift
    goto :parse_args
)
echo ERROR: Too many arguments.
echo Usage: %~nx0 [x86^|x64^|arm64] [--simd=avx2^|avx^|sse2]
exit /b 1

:args_done

if /I "%CORE_SIMD_LEVEL%"=="avx2" goto :simd_ok
if /I "%CORE_SIMD_LEVEL%"=="avx" goto :simd_ok
if /I "%CORE_SIMD_LEVEL%"=="sse2" goto :simd_ok
echo ERROR: Unsupported SIMD level "%CORE_SIMD_LEVEL%". Expected one of: avx2, avx, sse2.
exit /b 1
:simd_ok

if not defined ARCH_INPUT (
    if defined GUITARFX_WINDOWS_ARCH (
        set "ARCH_INPUT=%GUITARFX_WINDOWS_ARCH%"
    )
)
if not defined ARCH_INPUT (
    set "ARCH_INPUT=x64"
)

:: Canonical CMake Visual Studio platform names: Win32 (32-bit x86), x64, ARM64.
set "ARCH="
set "ARCH_LABEL="
if /I "%ARCH_INPUT%"=="x86" (
    set "ARCH=Win32"
    set "ARCH_LABEL=x86"
)
if /I "%ARCH_INPUT%"=="Win32" (
    set "ARCH=Win32"
    set "ARCH_LABEL=x86"
)
if /I "%ARCH_INPUT%"=="x64" (
    set "ARCH=x64"
    set "ARCH_LABEL=x64"
)
if /I "%ARCH_INPUT%"=="arm64" (
    set "ARCH=ARM64"
    set "ARCH_LABEL=arm64"
)
if /I "%ARCH_INPUT%"=="ARM64" (
    set "ARCH=ARM64"
    set "ARCH_LABEL=arm64"
)
if not defined ARCH (
    echo ERROR: Unsupported Windows architecture "%ARCH_INPUT%". Expected one of: x86, x64, arm64.
    exit /b 1
)
:: Export the resolved platform for Inno Setup architecture/install path selection.
set "GUITARFX_WINDOWS_ARCH=%ARCH%"
if defined GUITARFX_WINDOWS_CMAKE_GENERATOR (
    set "CMAKE_GENERATOR=%GUITARFX_WINDOWS_CMAKE_GENERATOR%"
) else (
    set "CMAKE_GENERATOR=Visual Studio 18 2026"
)

for /f %%I in ('powershell -NoProfile -Command "[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()"') do set "BUILD_START_MS=%%I"

echo [0/5] Configuring CMake...
echo       Generator: %CMAKE_GENERATOR%
echo       Architecture: %ARCH_LABEL% ^(CMake platform: %ARCH%^)
echo       SIMD baseline: %CORE_SIMD_LEVEL%
:: GUITARFX_CORE_ENABLE_AVX2 is passed ON so a build tree last configured by the old
:: --no-avx2 (which cached it OFF) cannot force this level down to sse2.
cmake -G "%CMAKE_GENERATOR%" -A "%ARCH%" -S juce -B "%JUCE_BUILDS%" -DGUITARFX_CORE_SIMD_LEVEL=%CORE_SIMD_LEVEL% -DGUITARFX_CORE_ENABLE_AVX2=ON
if !ERRORLEVEL! neq 0 (
    echo ERROR: CMake configure failed.
    goto :fail
)
echo CMake configure succeeded.
echo.

:: --- Build UI ---------------------------------------------------------------
echo [1/5] Building UI ^(npm ci ^&^& npm run build^)...
pushd "%UI_DIR%"
call npm ci
if !ERRORLEVEL! neq 0 (
    echo ERROR: npm ci failed.
    popd
    goto :fail
)
call npm run build
if !ERRORLEVEL! neq 0 (
    echo ERROR: UI build failed.
    popd
    goto :fail
)
popd
echo UI build succeeded.
echo.

:: --- Build Standalone -------------------------------------------------------
echo [2/5] Building Standalone (Release)...
cmake --build "%JUCE_BUILDS%" --config Release --target SoundshedGuitar_Standalone --parallel
if !ERRORLEVEL! neq 0 (
    echo ERROR: Standalone build failed.
    goto :fail
)
echo Standalone build succeeded.
echo.

:: --- Build VST3 -------------------------------------------------------------
echo [3/5] Building VST3 (Release)...
cmake --build "%JUCE_BUILDS%" --config Release --target SoundshedGuitar_VST3 --parallel
if !ERRORLEVEL! neq 0 (
    echo ERROR: VST3 build failed.
    goto :fail
)
echo VST3 build succeeded.
echo.

:: --- Build CLAP -------------------------------------------------------------
echo [4/5] Building CLAP (Release)...
cmake --build "%JUCE_BUILDS%" --config Release --target SoundshedGuitar_CLAP --parallel
if !ERRORLEVEL! neq 0 (
    echo ERROR: CLAP build failed.
    goto :fail
)
echo CLAP build succeeded.
echo.

:: --- Build Installer --------------------------------------------------------
echo [5/5] Building installer...
call "%INSTALLER_SCRIPT%"
if !ERRORLEVEL! neq 0 (
    echo ERROR: Installer build failed.
    goto :fail
)

echo Build and packaging succeeded.
set "BUILD_EXIT_CODE=0"
goto :report_elapsed

:fail
set "BUILD_EXIT_CODE=!ERRORLEVEL!"

:report_elapsed
for /f %%I in ('powershell -NoProfile -Command "$elapsed=[TimeSpan]::FromMilliseconds([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() - %BUILD_START_MS%); '{0:00}:{1:00}:{2:00}.{3:000}' -f [int]$elapsed.TotalHours, $elapsed.Minutes, $elapsed.Seconds, $elapsed.Milliseconds"') do set "BUILD_ELAPSED=%%I"
echo.
if "%BUILD_EXIT_CODE%"=="0" (
    echo Total elapsed time: %BUILD_ELAPSED%
) else (
    echo Total elapsed time before failure: %BUILD_ELAPSED%
)

endlocal & exit /b %BUILD_EXIT_CODE%
