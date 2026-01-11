#include <iostream>
#include <filesystem>

#include "dsp/effects/IRCabEffect.h"

int main() {
    namespace fs = std::filesystem;
    
    const fs::path resourcesDir = fs::path("C:/Work/GIT/misc/neuron-guitar/src/resources");
    
    const char* testFiles[] = {
        "ir/Guitar/Scharrface/G12T-75, G12-65 blend 1.wav",
        "ir/Guitar/Scharrface/V30 G12-65 blend 1.wav",
        "ir/Bass/Misc/Bass/Ampeg 810 One.wav",
        "ir/Bass/Misc/Bass/SWR 115 Blend.wav",
        "ir/Guitar/Devil's Lab/Hell.wav" // This one should work
    };
    
    for (const char* relPath : testFiles) {
        fs::path fullPath = resourcesDir / relPath;
        std::cout << "\nTesting: " << relPath << std::endl;
        std::cout << "Full path: " << fullPath << std::endl;
        std::cout << "Exists: " << (fs::exists(fullPath) ? "YES" : "NO") << std::endl;
        
        if (fs::exists(fullPath)) {
            guitarfx::IRCabEffect effect;
            effect.Prepare(48000.0, 512);

            bool loaded = effect.LoadResource(fullPath);
            std::cout << "Load result: " << (loaded ? "SUCCESS" : "FAILED") << std::endl;
        }
    }
    
    return 0;
}
