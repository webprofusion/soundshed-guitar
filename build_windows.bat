@echo off
setlocal EnableExtensions EnableDelayedExpansion

:: ---------------------------------------------------------------------------
:: Full release build: JUCE Standalone + VST3 + CLAP, then installer
:: Run from the workspace root.
:: Usage: build_windows.bat [x86|x64|arm64] [--simd=avx2|avx|sse2] [--refresh-deps]
::   --simd=avx2  (default) /arch:AVX2 - Intel Haswell 2013+, AMD Excavator 2015+
::   --simd=avx   /arch:AVX  - Intel Sandy Bridge / AMD Bulldozer 2011+; keeps
::                every 256-bit kernel in core/src/dsp/simd/SimdMath.h
::   --simd=sse2  no /arch: flag - CPUs with no AVX at all
::   --no-avx2 is an alias for --simd=avx, --no-avx an alias for --simd=sse2
::   --refresh-deps forces npm ci; otherwise an unchanged lockfile reuses node_modules
::
:: arm64 builds the plugins twice: native ARM64 in a variant-specific build
:: directory with the Standalone, and ARM64EC in juce\builds-arm64ec. An ARM64EC
:: host (REAPER's Windows ARM build is one) cannot load a native ARM64 plugin,
:: nor a native ARM64 host an ARM64EC one, so the installer ships both.
::
:: AAX (Pro Tools) is disabled: it is neither built nor packaged unless
:: GUITARFX_ENABLE_AAX=1 is set. When enabled, the notes below apply.
::
:: AAX (Pro Tools) is built on x64 only: JUCE creates no AAX target for Win32 or
:: ARM64, and Pro Tools on Windows is x64 only. Retail Pro Tools loads it only
:: once it is PACE-signed (the developer build of Pro Tools loads it unsigned),
:: so the installer packages it only when it is signed:
::   GUITARFX_AAX_SIGN_SCRIPT      Batch script run after the AAX build, called with
::                                 <bundle-path> <binary-path>; does the PACE step.
::   GUITARFX_REQUIRE_SIGNED_AAX=1 Fail instead of leaving an unsigned AAX out.
::   GUITARFX_ALLOW_UNSIGNED_AAX=1 Package an unsigned AAX (developer Pro Tools).
:: The signature check is in juce\packaging\build-installer.bat.
:: ---------------------------------------------------------------------------

set "WORKSPACE_ROOT=%~dp0"
set "INSTALLER_SCRIPT=%WORKSPACE_ROOT%juce\packaging\build-installer.bat"
set "UI_DIR=%WORKSPACE_ROOT%core\ui"
set "CORE_SIMD_LEVEL=avx2"
set "REFRESH_DEPS="
set "AAX_CMAKE_FLAG=-DGUITARFX_ENABLE_AAX=OFF"
if "%GUITARFX_ENABLE_AAX%"=="1" set "AAX_CMAKE_FLAG=-DGUITARFX_ENABLE_AAX=ON"

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
if /I "%~1"=="--refresh-deps" (
    set "REFRESH_DEPS=1"
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
echo Usage: %~nx0 [x86^|x64^|arm64] [--simd=avx2^|avx^|sse2] [--refresh-deps]
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

:: Keep the existing default directory for tools that expect juce\Builds.
:: Other architecture/SIMD combinations get independent trees so switching
:: variants does not invalidate and recompile a previously built configuration.
set "JUCE_BUILDS=%WORKSPACE_ROOT%juce\Builds-%ARCH_LABEL%-%CORE_SIMD_LEVEL%"
if /I "%ARCH_LABEL%"=="x64" if /I "%CORE_SIMD_LEVEL%"=="avx2" set "JUCE_BUILDS=%WORKSPACE_ROOT%juce\Builds"
set "JUCE_BUILDS_ARM64EC=%WORKSPACE_ROOT%juce\builds-arm64ec"
:: Export the resolved platform for Inno Setup architecture/install path selection.
set "GUITARFX_WINDOWS_ARCH=%ARCH%"
set "GUITARFX_WINDOWS_BUILD_DIR=%JUCE_BUILDS%"
set "GUITARFX_WINDOWS_ARM64EC_BUILD_DIR=%JUCE_BUILDS_ARM64EC%"
if defined GUITARFX_WINDOWS_CMAKE_GENERATOR (
    set "CMAKE_GENERATOR=%GUITARFX_WINDOWS_CMAKE_GENERATOR%"
) else (
    set "CMAKE_GENERATOR=Visual Studio 18 2026"
)

for /f %%I in ('powershell -NoProfile -Command "[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()"') do set "BUILD_START_MS=%%I"

echo [0/4] Configuring CMake...
echo       Generator: %CMAKE_GENERATOR%
echo       Architecture: %ARCH_LABEL% ^(CMake platform: %ARCH%^)
echo       SIMD baseline: %CORE_SIMD_LEVEL%
:: GUITARFX_CORE_ENABLE_AVX2 is passed ON so a build tree last configured by the old
:: --no-avx2 (which cached it OFF) cannot force this level down to sse2.
cmake -G "%CMAKE_GENERATOR%" -A "%ARCH%" -S juce -B "%JUCE_BUILDS%" -DGUITARFX_CORE_SIMD_LEVEL=%CORE_SIMD_LEVEL% -DGUITARFX_CORE_ENABLE_AVX2=ON %AAX_CMAKE_FLAG%
if !ERRORLEVEL! neq 0 (
    echo ERROR: CMake configure failed.
    goto :fail
)
echo CMake configure succeeded.
echo.

:: --- Build UI ---------------------------------------------------------------
echo [1/4] Preparing UI dependencies...
pushd "%UI_DIR%"
set "NPM_LOCK_HASH="
for /f "skip=1 tokens=1" %%H in ('certutil -hashfile package-lock.json SHA256 2^>nul') do (
    if not defined NPM_LOCK_HASH set "NPM_LOCK_HASH=%%H"
)

set "NPM_LOCK_MARKER=node_modules\.soundshed-package-lock.sha256"
set "CACHED_NPM_LOCK_HASH="
if not defined REFRESH_DEPS if exist "!NPM_LOCK_MARKER!" (
    set /p "CACHED_NPM_LOCK_HASH="<"!NPM_LOCK_MARKER!"
)

if not defined REFRESH_DEPS if defined NPM_LOCK_HASH if "!NPM_LOCK_HASH!"=="!CACHED_NPM_LOCK_HASH!" (
    echo UI dependencies are unchanged; skipping npm ci.
) else (
    call npm ci
    if !ERRORLEVEL! neq 0 (
        echo ERROR: npm ci failed.
        popd
        goto :fail
    )
    >"!NPM_LOCK_MARKER!" echo(!NPM_LOCK_HASH!
)
popd
echo UI dependencies are ready. CMake will build the bundle once from its stamp dependency.
echo.

:: --- Build desktop formats --------------------------------------------------
set "DESKTOP_TARGETS=SoundshedGuitar_Standalone SoundshedGuitar_VST3 SoundshedGuitar_CLAP"
set "DESKTOP_FORMATS=Standalone, VST3 and CLAP"
if /I "%ARCH%"=="x64" if "%GUITARFX_ENABLE_AAX%"=="1" (
    set "DESKTOP_TARGETS=!DESKTOP_TARGETS! SoundshedGuitar_AAX"
    set "DESKTOP_FORMATS=Standalone, VST3, CLAP and AAX"
)
echo [2/4] Building !DESKTOP_FORMATS! (Release)...
cmake --build "%JUCE_BUILDS%" --config Release --target !DESKTOP_TARGETS! --parallel
if !ERRORLEVEL! neq 0 (
    echo ERROR: Desktop format build failed.
    goto :fail
)
echo Desktop format builds succeeded.
echo.

:: --- Sign the AAX (x64, when a signing script is set) -----------------------
if not "%GUITARFX_ENABLE_AAX%"=="1" goto :aax_sign_done
if /I not "%ARCH%"=="x64" goto :aax_sign_done
if not defined GUITARFX_AAX_SIGN_SCRIPT goto :aax_sign_done

set "AAX_BUNDLE=%JUCE_BUILDS%\SoundshedGuitar_artefacts\Release\AAX\Soundshed Guitar.aaxplugin"
set "AAX_BINARY=%AAX_BUNDLE%\Contents\x64\Soundshed Guitar.aaxplugin"
if not exist "%GUITARFX_AAX_SIGN_SCRIPT%" (
    echo ERROR: GUITARFX_AAX_SIGN_SCRIPT not found: "%GUITARFX_AAX_SIGN_SCRIPT%"
    goto :fail
)
echo Signing the AAX plugin...
call "%GUITARFX_AAX_SIGN_SCRIPT%" "%AAX_BUNDLE%" "%AAX_BINARY%"
if !ERRORLEVEL! neq 0 (
    echo ERROR: AAX signing script failed.
    goto :fail
)
echo AAX signing script finished.
echo.

:aax_sign_done

:: --- Build the ARM64EC plugins (arm64 only) ---------------------------------
if /I not "%ARCH%"=="ARM64" goto :arm64ec_done

echo [3/4] Configuring and building the ARM64EC plugins (Release)...
cmake -G "%CMAKE_GENERATOR%" -A ARM64EC -S juce -B "%JUCE_BUILDS_ARM64EC%" %AAX_CMAKE_FLAG%
if !ERRORLEVEL! neq 0 (
    echo ERROR: ARM64EC CMake configure failed.
    goto :fail
)
cmake --build "%JUCE_BUILDS_ARM64EC%" --config Release --target SoundshedGuitar_VST3 SoundshedGuitar_CLAP --parallel
if !ERRORLEVEL! neq 0 (
    echo ERROR: ARM64EC plugin build failed.
    goto :fail
)
echo ARM64EC plugin builds succeeded.
echo.

:arm64ec_done

:: --- Build Installer --------------------------------------------------------
echo [4/4] Building installer...
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
