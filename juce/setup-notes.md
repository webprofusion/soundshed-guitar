git submodule update --init --recursive

If `JUCE/` is still missing (some clones do not have gitlink entries for the submodule), fetch it manually:

`git clone --depth 1 --branch master https://github.com/juce-framework/JUCE.git JUCE`

 modify CmakeLists https://melatonin.dev/manuals/pamplejuce/getting-started/setting-your-project-up/
 
 Generate project files and fetch dependencies e.g.:
 
 Configure for Windows and use "builds" folder. ASIO is enabled by default via JUCE's bundled ASIO sources.
 `cmake -G "Visual Studio 18 2026" -A x64 -S juce -B juce/builds-x64`
 
  `cmake -G "Visual Studio 18 2026" -A ARM64 -S juce -B juce/builds-arm64`

 `cmake -G "Visual Studio 18 2026" -A ARM64EC -S juce -B juce/builds-arm64ec`

 Windows on ARM needs both ARM builds of the plugins. A host loads only plugins built for its own
 kind of process: an ARM64EC host (REAPER's Windows ARM build is one) loads ARM64EC and x64 plugins,
 and a native ARM64 host loads ARM64 ones. `build_windows.bat arm64` builds the Standalone and the
 native plugins in `juce/Builds`, then the ARM64EC VST3 and CLAP in `juce/builds-arm64ec`, and the
 installer ships both: `arm64-win` and `arm64ec-win` in the VST3 bundle, and two `.clap` files.
 The ARM64EC build has no Custom Effects (WASM): Wasmtime publishes no ARM64EC library.

 If you need a custom external Steinberg ASIO SDK instead of JUCE's bundled ASIO headers, pass `-DGUITARFX_ASIO_SDK_DIR="C:/path/to/ASIOSDK"` and make sure that directory contains `iasiodrv.h`.

 `cmake -B build -G "Xcode"`

 
Build release
 
`build_windows.bat` is the complete Windows release entry point. It configures CMake,
reuses installed npm dependencies when `package-lock.json` is unchanged, builds the
TypeScript UI from CMake's stamp dependency once, builds all requested plugin formats
together, and then packages the installer. Pass `--refresh-deps` to force `npm ci`.

The standard x64/AVX2 build uses `juce\Builds` so existing development and validation
tools keep working. Other SIMD baselines and architectures use separate directories
such as `juce\Builds-x64-avx`, `juce\Builds-x64-sse2`, and `juce\Builds-arm64-avx2`.
This prevents switching variants from invalidating the previous build tree.

`cmake --build juce/builds-x64 --config Release --target SoundshedGuitar_Standalone --parallel`
`cmake --build juce/builds-arm64 --config Release --target SoundshedGuitar_Standalone --parallel`
`cmake --build juce/builds-arm64ec --config Release --target SoundshedGuitar_Standalone --parallel`

`cmake --build juce/builds-x64 --config Release --target SoundshedGuitar_VST3 --parallel`
`cmake --build juce/builds-arm64 --config Release --target SoundshedGuitar_VST3 --parallel`
`cmake --build juce/builds-arm64ec --config Release --target SoundshedGuitar_VST3 --parallel`

 Find exe output in 
 `builds-x64\SoundshedGuitar_artefacts\Release`

The Windows installer packages one runtime-only UI/resource tree under
`%ProgramData%\Soundshed Guitar\resources`. Standalone, VST3 and CLAP all resolve this
shared tree after checking their executable-local development layout. Build artifacts
still carry a clean local copy for plugin validation.

