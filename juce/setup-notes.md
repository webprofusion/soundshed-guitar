
 git submodule update --init --recursive

 modify CmakeLists https://melatonin.dev/manuals/pamplejuce/getting-started/setting-your-project-up/
 
 Generate project files and fetch dependencies e.g.:
 
 `cmake -B build -G "Visual Studio 18 2026"`
 
 `cmake -B build -G "Xcode"`

 
Build release
 
 `cmake --build build --config Release --target SoundshedGuitar_Standalone --parallel`

 Find exe output in 
 `build\SoundshedGuitar_artefacts\Release`