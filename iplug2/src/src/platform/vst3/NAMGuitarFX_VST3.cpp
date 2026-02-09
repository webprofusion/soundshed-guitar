// Include VST3 SDK headers before plugin header to ensure all VST3 types are available
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"

#include "GuitarFXPlugin.h"

// This translation unit intentionally remains lightweight. By including the
// plugin header we ensure the VST3 module inherits the same preprocessor
// context as the core target without introducing duplicate entry points.
