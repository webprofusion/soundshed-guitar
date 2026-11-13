@echo off
setlocal EnableExtensions
rem ---------------------------------------------------------------------------
rem Installs Soundshed Nano Q's APK on the first connected phone or tablet and
rem launches it, optionally building it first. Emulators are skipped: they have
rem no low-latency audio path. arm64-v8a only (Skia is built for that alone).
rem
rem   deploy.bat                 install the release APK
rem   deploy.bat debug           install the debug APK
rem   deploy.bat build           build the release APK, then install it
rem   deploy.bat debug build     build the debug APK, then install it
rem   deploy.bat build nolaunch  build and install, but do not start the app
rem
rem Skia must be built once first: build-skia.ps1 (see docs/plans/nano-q-android.md).
rem ---------------------------------------------------------------------------

set "HERE=%~dp0"
set "SELF=%~nx0"
set "VARIANT="
set "REBUILD="
set "NOLAUNCH="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="release" (
    set "VARIANT=release"
    shift
    goto parse
)
if /i "%~1"=="debug" (
    set "VARIANT=debug"
    shift
    goto parse
)
if /i "%~1"=="build" (
    set "REBUILD=1"
    shift
    goto parse
)
if /i "%~1"=="nolaunch" (
    set "NOLAUNCH=1"
    shift
    goto parse
)
echo Unrecognised argument: %~1
echo Usage: %SELF% [release^|debug] [build] [nolaunch]
exit /b 2

:parsed
if not defined VARIANT set "VARIANT=release"
if /i "%VARIANT%"=="release" (set "TASK=assembleRelease") else (set "TASK=assembleDebug")
set "APK=%HERE%app\build\outputs\apk\%VARIANT%\app-%VARIANT%.apk"

rem --- adb: sdk.dir from local.properties, then ANDROID_HOME / ANDROID_SDK_ROOT,
rem --- then the default SDK location.
set "SDK="
if exist "%HERE%local.properties" (
    for /f "usebackq eol=# tokens=1,* delims==" %%a in ("%HERE%local.properties") do (
        if /i "%%a"=="sdk.dir" set "SDK=%%b"
    )
)
if not defined SDK if defined ANDROID_HOME set "SDK=%ANDROID_HOME%"
if not defined SDK if defined ANDROID_SDK_ROOT set "SDK=%ANDROID_SDK_ROOT%"
if not defined SDK set "SDK=%LOCALAPPDATA%\Android\Sdk"
rem Android Studio writes the path Java-properties style: C\:\\Users\\...
set "SDK=%SDK:\:=:%"
set "SDK=%SDK:\\=\%"
set "SDK=%SDK:/=\%"
set "ADB=%SDK%\platform-tools\adb.exe"
if not exist "%ADB%" (
    echo adb not found at %ADB%
    echo Set sdk.dir in nanoq\android\local.properties, or ANDROID_HOME.
    exit /b 1
)

rem --- first device in the "device" state whose serial is not an emulator's
set "SERIAL="
for /f "skip=1 tokens=1,2" %%a in ('call "%ADB%" devices') do call :consider "%%a" "%%b"
if not defined SERIAL (
    echo No connected device in the "device" state ^(emulators are skipped^).
    echo Plug the phone in, unlock it, and accept the USB debugging prompt.
    "%ADB%" devices -l
    exit /b 1
)

if defined REBUILD (
    echo Building %VARIANT% APK ...
    pushd "%HERE%" || exit /b 1
    call "%HERE%gradlew.bat" %TASK%
    if errorlevel 1 (
        popd
        exit /b 1
    )
    popd
)

if not exist "%APK%" (
    echo No %VARIANT% APK at:
    echo   %APK%
    echo Build it here:  %SELF% %VARIANT% build
    exit /b 1
)

echo Installing %VARIANT% APK on %SERIAL% ...
"%ADB%" -s %SERIAL% install -r "%APK%" || exit /b 1

if defined NOLAUNCH exit /b 0

"%ADB%" -s %SERIAL% shell am start -n com.soundshed.guitar.nanoq/com.soundshed.nanoq.NanoQActivity >nul || exit /b 1
echo Launched. Follow the log with:
echo   "%ADB%" -s %SERIAL% logcat -s NanoQ
exit /b 0

:consider
if defined SERIAL goto :eof
if not "%~2"=="device" goto :eof
set "CANDIDATE=%~1"
if /i "%CANDIDATE:~0,9%"=="emulator-" goto :eof
set "SERIAL=%CANDIDATE%"
goto :eof
