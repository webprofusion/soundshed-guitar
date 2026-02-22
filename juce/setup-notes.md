git submodule update --init --recursive

If `JUCE/` is still missing (some clones do not have gitlink entries for the submodule), fetch it manually:

`git clone --depth 1 --branch develop https://github.com/juce-framework/JUCE.git JUCE`

 modify CmakeLists https://melatonin.dev/manuals/pamplejuce/getting-started/setting-your-project-up/
 
 Generate project files and fetch dependencies e.g.:
 
 `cmake -B build -G "Visual Studio 18 2026"`

 Configure to include ASIO and use "builds" folder
 `cmake -G "Visual Studio 18 2026" -A x64 -S juce -B juce/builds -DGUITARFX_ASIO_SDK_DIR="C:/Work/GIT/soundshed-guitar/juce/ASIOSDK";`
 
 `cmake -B build -G "Xcode"`

 
Build release
 
 `cmake --build build --config Release --target SoundshedGuitar_Standalone --parallel`
 `cmake --build build --config Debug --target SoundshedGuitar_Standalone --parallel`
 `cmake --build build --config Release --target SoundshedGuitar_VST3 --parallel`

 Find exe output in 
 `build\SoundshedGuitar_artefacts\Release`

