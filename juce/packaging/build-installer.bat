@echo off
setlocal

:: ---------------------------------------------------------------------------
:: Local installer build script - mirrors the build-installer GitHub Action
:: Run from any directory; the script resolves paths automatically.
::
:: Prerequisites:
::   - Inno Setup 6 installed (default path below, or override ISCC)
::   - JUCE Release build already done (juce\build\ or juce\Builds\)
::   - UI bundle already built (core\ui\npm run build)
:: ---------------------------------------------------------------------------

:: Resolve workspace root (two levels up from this script's directory)
set "SCRIPT_DIR=%~dp0"
set "JUCE_DIR=%SCRIPT_DIR%.."
set "WORKSPACE_ROOT=%SCRIPT_DIR%..\.."

:: --- Project identity (must match juce\CMakeLists.txt) -------------------
set "PROJECT_NAME=SoundshedGuitar"
set "PRODUCT_NAME=Soundshed Guitar"
set "COMPANY_NAME=Soundshed"

:: --- Inno Setup compiler path --------------------------------------------
set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
    echo ERROR: Inno Setup not found at "%ISCC%"
    echo Install from https://jrsoftware.org/isinfo.php or set ISCC to the correct path.
    exit /b 1
)

:: --- Verify artefacts exist (installer.iss expects juce\Builds\) ---------
:: NOTE: The local CMake task outputs to juce\build\ (lowercase).
:: If you configured with -B juce/Builds use that; otherwise create a symlink
:: or reconfigure: cmake -S juce -B juce/Builds ...
set "ARTEFACTS_DIR=%JUCE_DIR%\Builds\%PROJECT_NAME%_artefacts\Release"
if not exist "%ARTEFACTS_DIR%" (
    echo WARNING: Expected artefacts at "%ARTEFACTS_DIR%"
    echo          If your local build is in juce\build\ run:
    echo          mklink /D "%JUCE_DIR%\Builds" "%JUCE_DIR%\build"
    echo          or reconfigure CMake with -B juce/Builds
    exit /b 1
)

:: --- Run Inno Setup -------------------------------------------------------
echo Building installer...
echo   PROJECT_NAME : %PROJECT_NAME%
echo   PRODUCT_NAME : %PRODUCT_NAME%
echo   COMPANY_NAME : %COMPANY_NAME%
echo.

pushd "%JUCE_DIR%"
"%ISCC%" "packaging\installer.iss"
set "ISCC_EXIT=%ERRORLEVEL%"
popd

if %ISCC_EXIT% neq 0 (
    echo ERROR: Inno Setup failed with exit code %ISCC_EXIT%
    exit /b %ISCC_EXIT%
)

echo.
echo Installer built successfully.
echo Output: %JUCE_DIR%\packaging\Output\

endlocal
