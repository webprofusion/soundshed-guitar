@echo off
setlocal

:: ---------------------------------------------------------------------------
:: Full release build: JUCE Standalone + VST3, then installer
:: Run from the workspace root.
:: ---------------------------------------------------------------------------

set "WORKSPACE_ROOT=%~dp0"
set "JUCE_BUILDS=%WORKSPACE_ROOT%juce\builds"
set "INSTALLER_SCRIPT=%WORKSPACE_ROOT%juce\packaging\build-installer.bat"
set "UI_DIR=%WORKSPACE_ROOT%core\ui"

:: --- Build UI ---------------------------------------------------------------
echo [1/4] Building UI (npm run build)...
pushd "%UI_DIR%"
call npm run build
if %ERRORLEVEL% neq 0 (
    echo ERROR: UI build failed.
    popd
    exit /b %ERRORLEVEL%
)
popd
echo UI build succeeded.
echo.

:: --- Build Standalone -------------------------------------------------------
echo [2/4] Building Standalone (Release)...
cmake --build "%JUCE_BUILDS%" --config Release --target SoundshedGuitar_Standalone --parallel
if %ERRORLEVEL% neq 0 (
    echo ERROR: Standalone build failed.
    exit /b %ERRORLEVEL%
)
echo Standalone build succeeded.
echo.

:: --- Build VST3 -------------------------------------------------------------
echo [3/4] Building VST3 (Release)...
cmake --build "%JUCE_BUILDS%" --config Release --target SoundshedGuitar_VST3 --parallel
if %ERRORLEVEL% neq 0 (
    echo ERROR: VST3 build failed.
    exit /b %ERRORLEVEL%
)
echo VST3 build succeeded.
echo.

:: --- Build Installer --------------------------------------------------------
echo [4/4] Building installer...
call "%INSTALLER_SCRIPT%"
if %ERRORLEVEL% neq 0 (
    echo ERROR: Installer build failed.
    exit /b %ERRORLEVEL%
)

endlocal
