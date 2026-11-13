<#
.SYNOPSIS
  Builds Skia for Nano Q's Android app in a Debian container (nanoq/android/build-skia.sh).

.DESCRIPTION
  vcpkg's Skia port targets Android from a Linux host, so on Windows the build runs in Docker.
  vcpkg, the NDK and vcpkg's caches stay in the volume soundshed-nanoq-android-skia, so a second
  run is quick; the installed tree lands in nanoq/android/skia/arm64-android-nanoq (ignored by git).

.EXAMPLE
  nanoq\android\build-skia.ps1
#>
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

# The packages vcpkg needs on the host: a compiler for its host tools (gn), CMake, Ninja, Git and
# the archive tools, Python for Skia's gn scripts, and autotools for the ports that use them.
$setup = @'
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake ninja-build git curl ca-certificates \
    zip unzip tar xz-utils pkg-config python3 autoconf automake libtool > /dev/null
# The script's line endings follow the Windows checkout: run a copy with LF.
tr -d '\r' < /out/build-skia.sh > /tmp/build-skia.sh
mkdir -p /tmp/triplets
for f in /out/cmake/vcpkg-triplets/*.cmake; do tr -d '\r' < "$f" > "/tmp/triplets/$(basename "$f")"; done
sed -i 's#--overlay-triplets="${SCRIPT_DIR}/cmake/vcpkg-triplets"#--overlay-triplets=/tmp/triplets#; s#^OUT=.*#OUT=/out/skia/arm64-android-nanoq#' /tmp/build-skia.sh
bash /tmp/build-skia.sh /work
'@

docker run --rm --platform linux/amd64 `
    -v soundshed-nanoq-android-skia:/work `
    -v "${here}:/out" `
    debian:trixie bash -c ($setup -replace "`r", '')
if ($LASTEXITCODE -ne 0) { throw "Skia build failed ($LASTEXITCODE)" }
