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

:: --- Decide whether the installer carries the AAX (x64 only) --------------
:: Retail Pro Tools rejects an AAX that is not PACE-signed. PACE's wraptool
:: signs the binary with a code-signing certificate, so a valid Authenticode
:: signature is the check here (it cannot tell a PACE wrap from a plain
:: signtool signature). build_windows.bat documents the GUITARFX_*_AAX settings.
::
:: AAX is disabled unless GUITARFX_ENABLE_AAX=1: a stale AAX left in the
:: artefacts folder by an earlier build is never packaged otherwise.
set "GUITARFX_INCLUDE_AAX=0"
set "AAX_STATUS=disabled (set GUITARFX_ENABLE_AAX=1 to package it)"
if not "%GUITARFX_ENABLE_AAX%"=="1" goto :aax_decided
set "AAX_STATUS=not built for this architecture"
set "TARGET_ARCH=%GUITARFX_WINDOWS_ARCH%"
if not defined TARGET_ARCH set "TARGET_ARCH=x64"
if /I not "%TARGET_ARCH%"=="x64" goto :aax_decided

set "AAX_BINARY=%ARTEFACTS_DIR%\AAX\%PRODUCT_NAME%.aaxplugin\Contents\x64\%PRODUCT_NAME%.aaxplugin"
if not exist "%AAX_BINARY%" (
    set "AAX_STATUS=missing"
    goto :aax_unusable
)
:: pwsh when there is one: Windows PowerShell started from inside pwsh (a CI
:: step, or a pwsh prompt) inherits its module path and cannot load
:: Get-AuthenticodeSignature.
set "AAX_POWERSHELL=powershell"
where pwsh >nul 2>nul && set "AAX_POWERSHELL=pwsh"
"%AAX_POWERSHELL%" -NoProfile -Command "try { $s = Get-AuthenticodeSignature -LiteralPath $env:AAX_BINARY -ErrorAction Stop } catch { Write-Host $_; exit 2 }; if ($s.Status -eq 'Valid') { exit 0 }; Write-Host ('AAX signature status: ' + $s.Status); exit 1"
if errorlevel 2 (
    set "AAX_STATUS=of unknown signature, as the check failed"
    goto :aax_unusable
)
if not errorlevel 1 (
    set "GUITARFX_INCLUDE_AAX=1"
    set "AAX_STATUS=signed"
    goto :aax_decided
)
set "AAX_STATUS=unsigned"
if "%GUITARFX_ALLOW_UNSIGNED_AAX%"=="1" (
    set "GUITARFX_INCLUDE_AAX=1"
    echo WARNING: Packaging an unsigned AAX ^(GUITARFX_ALLOW_UNSIGNED_AAX=1^); retail Pro Tools will reject it.
    goto :aax_decided
)

:aax_unusable
if "%GUITARFX_REQUIRE_SIGNED_AAX%"=="1" (
    echo ERROR: GUITARFX_REQUIRE_SIGNED_AAX=1 but the AAX is %AAX_STATUS%: "%AAX_BINARY%"
    exit /b 1
)
echo NOTE: Leaving the AAX out of the installer; it is %AAX_STATUS%.
echo       Set GUITARFX_AAX_SIGN_SCRIPT for build_windows.bat to sign it.

:aax_decided

:: --- Run Inno Setup -------------------------------------------------------
echo Building installer...
echo   PROJECT_NAME : %PROJECT_NAME%
echo   PRODUCT_NAME : %PRODUCT_NAME%
echo   COMPANY_NAME : %COMPANY_NAME%
echo   BUILD_DIR    : %GUITARFX_WINDOWS_BUILD_DIR%
echo   AAX          : %AAX_STATUS%, included=%GUITARFX_INCLUDE_AAX%
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
