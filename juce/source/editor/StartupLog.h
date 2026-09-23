#pragma once

#include <juce_core/juce_core.h>

namespace soundshed::editor
{
/// Appends a timestamped line to <profile>/logs/soundshed-startup.log (and the JUCE logger
/// where one is attached). The file outlives the process and works in release builds, which
/// is the point: it is how an editor that came up blank or at the wrong size gets diagnosed.
void writeStartupLog (const juce::String& message);
} // namespace soundshed::editor
