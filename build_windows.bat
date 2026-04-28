@echo off
setlocal EnableExtensions EnableDelayedExpansion

:: ---------------------------------------------------------------------------
:: Full release build: JUCE Standalone + VST3, then installer
:: Run from the workspace root.
:: ---------------------------------------------------------------------------

set "WORKSPACE_ROOT=%~dp0"
set "JUCE_BUILDS=%WORKSPACE_ROOT%juce\Builds"
set "INSTALLER_SCRIPT=%WORKSPACE_ROOT%juce\packaging\build-installer.bat"
set "UI_DIR=%WORKSPACE_ROOT%core\ui"
if defined GUITARFX_WINDOWS_ARCH (
    set "ARCH=%GUITARFX_WINDOWS_ARCH%"
) else (
    set "ARCH=x64"
)
if defined GUITARFX_WINDOWS_CMAKE_GENERATOR (
    set "CMAKE_GENERATOR=%GUITARFX_WINDOWS_CMAKE_GENERATOR%"
) else (
    set "CMAKE_GENERATOR=Visual Studio 18 2026"
)
set "CMAKE_EXTRA_ARGS="
if /I "%ARCH%"=="Win32" (
    set "CMAKE_EXTRA_ARGS=-DGUITARFX_CORE_ENABLE_WASM_EFFECTS=OFF"
    echo       Note: disabling WASM effects for Win32 because Wasmtime does not provide an x86 Windows C API bundle.
)

for /f %%I in ('powershell -NoProfile -Command "[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()"') do set "BUILD_START_MS=%%I"

echo [0/4] Configuring CMake...
echo       Generator: %CMAKE_GENERATOR%
echo       Architecture: %ARCH%
cmake -G "%CMAKE_GENERATOR%" -A %ARCH% -S juce -B "%JUCE_BUILDS%" %CMAKE_EXTRA_ARGS%
if !ERRORLEVEL! neq 0 (
    echo ERROR: CMake configure failed.
    goto :fail
)
echo CMake configure succeeded.
echo.

:: --- Build UI ---------------------------------------------------------------
echo [1/4] Building UI (npm run build)...
pushd "%UI_DIR%"
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
echo [2/4] Building Standalone (Release)...
cmake --build "%JUCE_BUILDS%" --config Release --target SoundshedGuitar_Standalone --parallel
if !ERRORLEVEL! neq 0 (
    echo ERROR: Standalone build failed.
    goto :fail
)
echo Standalone build succeeded.
echo.

:: --- Build VST3 -------------------------------------------------------------
echo [3/4] Building VST3 (Release)...
cmake --build "%JUCE_BUILDS%" --config Release --target SoundshedGuitar_VST3 --parallel
if !ERRORLEVEL! neq 0 (
    echo ERROR: VST3 build failed.
    goto :fail
)
echo VST3 build succeeded.
echo.

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
