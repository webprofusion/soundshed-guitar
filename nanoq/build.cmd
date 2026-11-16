@echo off
rem Configures (if needed) and builds Soundshed Nano Q with the MSVC environment loaded.
rem   nanoq\build.cmd [Debug|Release|RelWithDebInfo] [target] [--variant <name>]
rem
rem Variants (CPU architecture and SIMD baseline; see core/cmake/GuitarfxSimdLevel.cmake):
rem   x64-avx2  (default) 64-bit, /arch:AVX2 - Intel Haswell 2013+, AMD Excavator 2015+
rem   x64-avx   64-bit, /arch:AVX  - Intel Sandy Bridge / AMD Bulldozer 2011+
rem   x64-sse2  64-bit, no AVX at all - Celeron/Pentium Silver, pre-2011 x86-64
rem   x86-sse3  32-bit, no AVX: /arch:SSE2 code generation plus SimdFFT.h's SSE3 intrinsics
rem             (MMX and SSE3 hardware; Pentium 4 Prescott 2004+, Athlon 64 rev E+)
rem   arm64     ARM64, /arch:armv8.2 where the compiler has it
rem
rem The default variant builds in nanoq\build\<config>, as before; every other variant in
rem nanoq\build\<variant>\<config>, so trees for different variants never share a cache.
rem To force a fresh configure, delete build.ninja in that tree.
setlocal EnableExtensions
rem Before parsing: shift moves %0 as well, and with it %~dp0.
set "HERE=%~dp0"
set "CONFIG="
set "TARGET="
set "VARIANT=x64-avx2"

:parse_args
if "%~1"=="" goto :args_done
rem cmd.exe splits on "=" as well as spaces, so "--variant=x86-sse3" arrives as two tokens.
if /I "%~1"=="--variant" (
  set "VARIANT=%~2"
  shift
  shift
  goto :parse_args
)
if not defined CONFIG (
  set "CONFIG=%~1"
  shift
  goto :parse_args
)
if not defined TARGET (
  set "TARGET=%~1"
  shift
  goto :parse_args
)
echo ERROR: Too many arguments.
echo Usage: nanoq\build.cmd [Debug^|Release^|RelWithDebInfo] [target] [--variant x64-avx2^|x64-avx^|x64-sse2^|x86-sse3^|arm64]
exit /b 1
:args_done
if not defined CONFIG set "CONFIG=Release"

rem Host side of the vcvarsall argument: an ARM64 PC cross-compiles x64 and x86 from arm64.
set "HOST=amd64"
if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "HOST=arm64"

set "SIMD="
set "TGT="
if /I "%VARIANT%"=="x64-avx2" (set "TGT=amd64" & set "SIMD=avx2")
if /I "%VARIANT%"=="x64-avx"  (set "TGT=amd64" & set "SIMD=avx")
if /I "%VARIANT%"=="x64-sse2" (set "TGT=amd64" & set "SIMD=sse2")
if /I "%VARIANT%"=="x86-sse3" (set "TGT=x86"   & set "SIMD=sse2")
if /I "%VARIANT%"=="arm64"    (set "TGT=arm64" & set "SIMD=sse2")
if not defined TGT (
  echo ERROR: Unknown variant "%VARIANT%". Expected one of: x64-avx2, x64-avx, x64-sse2, x86-sse3, arm64.
  exit /b 1
)
if /I "%HOST%"=="%TGT%" (set "VCARG=%HOST%") else (set "VCARG=%HOST%_%TGT%")

set VCVARS=
for %%E in (Community Professional Enterprise Insiders BuildTools) do (
  for %%V in (18 2022) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvarsall.bat" if "%VCVARS%"=="" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvarsall.bat"
  )
)
if "%VCVARS%"=="" (echo Visual Studio with the C++ workload not found & exit /b 1)
rem vcvarsall looks for vswhere.exe on PATH and complains when it is not there.
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VCVARS%" %VCARG% >nul || (echo vcvarsall %VCARG% failed: is the MSVC toolset for %TGT% installed? & exit /b 1)

set "SRC=%HERE%."
if /I "%VARIANT%"=="x64-avx2" (set "BLD=%HERE%build\%CONFIG%") else (set "BLD=%HERE%build\%VARIANT%\%CONFIG%")
echo Nano Q %CONFIG%, variant %VARIANT% (MSVC %VCARG%, SIMD %SIMD%) in %BLD%
rem Configure only a new tree: Ninja re-runs CMake itself when a CMakeLists or the source list
rem changes. Configuring every time cost ~40 s and recompiled miniz and the core's archive code.
if not exist "%BLD%\build.ninja" (
  cmake -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DGUITARFX_CORE_SIMD_LEVEL=%SIMD% -DNANOQ_BUILD_VARIANT=%VARIANT% || exit /b 1
)
if "%TARGET%"=="" (cmake --build "%BLD%") else (cmake --build "%BLD%" --target %TARGET%)
