@echo off
setlocal

:: ---------------------------------------------------------------------------
:: Local installer build script - mirrors the build-installer GitHub Action
:: Run from any directory; the script resolves paths automatically.
::
:: Prerequisites:
::   - Inno Setup 6 installed (default path below, or override ISCC)
::   - JUCE Release build already done by build_windows.bat
:: ---------------------------------------------------------------------------

:: Resolve workspace root (two levels up from this script's directory)
set "SCRIPT_DIR=%~dp0"
set "JUCE_DIR=%SCRIPT_DIR%.."
set "WORKSPACE_ROOT=%SCRIPT_DIR%..\.."
set "DEFAULT_BUILD_DIR=%JUCE_DIR%\Builds"
set "DEFAULT_ARM64EC_BUILD_DIR=%JUCE_DIR%\builds-arm64ec"
if not defined GUITARFX_WINDOWS_BUILD_DIR set "GUITARFX_WINDOWS_BUILD_DIR=%DEFAULT_BUILD_DIR%"
if not defined GUITARFX_WINDOWS_ARM64EC_BUILD_DIR set "GUITARFX_WINDOWS_ARM64EC_BUILD_DIR=%DEFAULT_ARM64EC_BUILD_DIR%"

:: --- Project identity (must match juce\CMakeLists.txt) -------------------
set "PROJECT_NAME=SoundshedGuitar"
set "PRODUCT_NAME=Soundshed Guitar"
set "COMPANY_NAME=Soundshed"

:: --- Inno Setup compiler path --------------------------------------------
if not defined ISCC set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
    echo ERROR: Inno Setup not found at "%ISCC%"
    echo Install from https://jrsoftware.org/isinfo.php or set ISCC to the correct path.
    exit /b 1
)

:: --- Verify artefacts and the staged runtime UI exist ---------------------
set "ARTEFACTS_DIR=%GUITARFX_WINDOWS_BUILD_DIR%\%PROJECT_NAME%_artefacts\Release"
if not exist "%ARTEFACTS_DIR%" (
    echo WARNING: Expected artefacts at "%ARTEFACTS_DIR%"
    echo          Build with build_windows.bat, or set GUITARFX_WINDOWS_BUILD_DIR.
    exit /b 1
)

:: An arm64 installer also carries the ARM64EC plugins (installer.iss), which
:: build_windows.bat builds into juce\builds-arm64ec\.
if /I "%GUITARFX_WINDOWS_ARCH%"=="ARM64" (
    if not exist "%GUITARFX_WINDOWS_ARM64EC_BUILD_DIR%\%PROJECT_NAME%_artefacts\Release\CLAP\%PRODUCT_NAME%.clap" (
        echo ERROR: ARM64EC plugins not found under "%GUITARFX_WINDOWS_ARM64EC_BUILD_DIR%\%PROJECT_NAME%_artefacts\Release"
        echo        Build them first: build_windows.bat arm64 does, after the native ARM64 targets.
        exit /b 1
    )
)

if not exist "%GUITARFX_WINDOWS_BUILD_DIR%\ui-stage\index.html" (
    echo ERROR: Staged runtime UI not found at "%GUITARFX_WINDOWS_BUILD_DIR%\ui-stage"
    echo        Build one of the release plugin targets before packaging.
    exit /b 1
)

:: --- Run Inno Setup -------------------------------------------------------
echo Building installer...
echo   PROJECT_NAME : %PROJECT_NAME%
echo   PRODUCT_NAME : %PRODUCT_NAME%
echo   COMPANY_NAME : %COMPANY_NAME%
echo   BUILD_DIR    : %GUITARFX_WINDOWS_BUILD_DIR%
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
