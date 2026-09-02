@echo off
setlocal EnableExtensions
rem ---------------------------------------------------------------------------
rem Installs the APK on the first connected phone or tablet and launches it.
rem Emulators are skipped: they have no low-latency audio path, so nothing
rem about tone or latency can be judged on one (see docs/android-build.md).
rem
rem   deploy.bat                 release APK - the one to use for anything audio
rem   deploy.bat debug           debug APK
rem   deploy.bat release nolaunch
rem
rem Build first:  gradlew assembleRelease -Pssg.abis=arm64-v8a
rem ---------------------------------------------------------------------------

set "HERE=%~dp0"
set "VARIANT=%~1"
if "%VARIANT%"=="" set "VARIANT=release"
if /i not "%VARIANT%"=="release" if /i not "%VARIANT%"=="debug" (
    echo Usage: %~nx0 [release^|debug] [nolaunch]
    exit /b 2
)

set "APK=%HERE%app\build\outputs\apk\%VARIANT%\app-%VARIANT%.apk"
if not exist "%APK%" (
    echo No %VARIANT% APK at:
    echo   %APK%
    echo Build it first:  gradlew assemble%VARIANT% -Pssg.abis=arm64-v8a
    exit /b 1
)

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
    echo Set sdk.dir in android\local.properties, or ANDROID_HOME.
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

echo Installing %VARIANT% APK on %SERIAL% ...
"%ADB%" -s %SERIAL% install -r "%APK%" || exit /b 1

if /i "%~2"=="nolaunch" exit /b 0

"%ADB%" -s %SERIAL% shell am start -n com.soundshed.guitar/.MainActivity >nul || exit /b 1
echo Launched. Follow the log with:
echo   "%ADB%" -s %SERIAL% logcat -s SoundshedGuitar JUCE
exit /b 0

:consider
if defined SERIAL goto :eof
if not "%~2"=="device" goto :eof
set "CANDIDATE=%~1"
if /i "%CANDIDATE:~0,9%"=="emulator-" goto :eof
set "SERIAL=%CANDIDATE%"
goto :eof
