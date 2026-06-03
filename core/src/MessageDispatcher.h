#pragma once

/**
 * MessageDispatcher — Routes incoming JSON messages from the WebView
 * to the appropriate PluginController handler.
 *
 * This replaces duplicated host-side message routing logic.
 *
 * The canonical message set is unified at the core layer to preserve
 * feature parity across host adapters.
 */

#include <string>

#include <nlohmann/json.hpp>

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

private:
    static bool DispatchStateAndLists(PluginController& controller,
                                      const nlohmann::json& msg,
                                      const std::string& type);
    static bool DispatchSettings(PluginController& controller,
                                 const nlohmann::json& msg,
                                 const std::string& type);
    static bool DispatchParameters(PluginController& controller,
                                   const nlohmann::json& msg,
                                   const std::string& type);
    static bool DispatchPresetsAndResources(PluginController& controller,
                                            const nlohmann::json& msg,
                                            const std::string& type);
    static bool DispatchSignalPath(PluginController& controller,
                                   const nlohmann::json& msg,
                                   const std::string& type);
    static bool DispatchMixerAndMonitoring(PluginController& controller,
                                           const nlohmann::json& msg,
                                           const std::string& type);
    static bool DispatchLibraryAndComposite(PluginController& controller,
                                            const nlohmann::json& msg,
                                            const std::string& type);
};

} // namespace guitarfx
