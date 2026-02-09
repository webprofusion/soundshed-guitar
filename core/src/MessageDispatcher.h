#pragma once

/**
 * MessageDispatcher — Routes incoming JSON messages from the WebView
 * to the appropriate PluginController handler.
 *
 * This replaces the duplicated if-else chains in:
 *   - GuitarFXPlugin::HandleUIMessage (iPlug2, ~380 lines)
 *   - PluginProcessor::handleWebMessage (JUCE, ~430 lines)
 *
 * The canonical message set is the union of all message types from both
 * frameworks, ensuring full feature parity.
 */

#include <string>

namespace guitarfx
{

class PluginController;

class MessageDispatcher
{
public:
    /**
     * Parse a JSON message string and dispatch to the appropriate
     * PluginController handler based on the "type" field.
     *
     * Thread safety: this is called on the UI/main thread. The handlers
     * may acquire mDSPMutex when modifying DSP state.
     */
    static void Dispatch(PluginController& controller, const std::string& jsonMessage);
};

} // namespace guitarfx
