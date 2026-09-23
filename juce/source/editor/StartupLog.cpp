#include "StartupLog.h"

#include "ProfileFolder.h"

namespace soundshed::editor
{
void writeStartupLog (const juce::String& message)
{
    const auto logDir = soundshed::profileFolder().getChildFile ("logs");
    logDir.createDirectory();
    const auto logFile = logDir.getChildFile ("soundshed-startup.log");
    juce::FileOutputStream stream (logFile);

    if (stream.openedOk())
    {
        stream.setPosition (stream.getFile().getSize()); // append
        const auto line = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S") + "  " + message + "\n";
        stream.writeText (line, false, false, nullptr);
    }

#if ! JUCE_LINUX
    juce::Logger::writeToLog (message);
#endif
}
} // namespace soundshed::editor
