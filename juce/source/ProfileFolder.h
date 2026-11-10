#pragma once

#include <juce_core/juce_core.h>

namespace soundshed
{
/// The one profile every Sepia product shares: settings, soundshed.db, the
/// resource library, riffs and logs. Deliberately not derived from the product name, so
/// Sepia and Soundshed Nano Q (through core's FileSystem) read and write the same
/// data.
inline constexpr const char* kProfileFolderName = "Sepia";

inline juce::File profileFolder()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile(kProfileFolderName);
}
} // namespace soundshed
