/**
 * MessageDispatcher.cpp — Canonical message routing for all WebView messages.
 *
 * This is the single source of truth for all UI↔engine message types.
 * It keeps host message routing unified in one dispatch table.
 */

#include "MessageDispatcher.h"
#include "PluginController.h"

namespace guitarfx
{

void MessageDispatcher::Dispatch(PluginController& c, const std::string& jsonMessage)
{
    nlohmann::json msg;
    try
    {
        msg = nlohmann::json::parse(jsonMessage);
    }
    catch (const std::exception&)
    {
        return; // Ignore malformed JSON
    }

    const std::string type = msg.value("type", "");
    if (type.empty()) return;
    if (DispatchStateAndLists(c, msg, type)) return;
    if (DispatchSettings(c, msg, type)) return;
    if (DispatchParameters(c, msg, type)) return;
    if (DispatchPresetsAndResources(c, msg, type)) return;
    if (DispatchSignalPath(c, msg, type)) return;
    if (DispatchMixerAndMonitoring(c, msg, type)) return;
    if (DispatchLibraryAndComposite(c, msg, type)) return;

    // Unknown message type — silently ignored
}

} // namespace guitarfx
