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

    // A message can parse and still hold a field of the wrong JSON type. json::value() then
    // throws type_error, here for a non-string "type" or in whichever handler reads the field.
    // Uncaught, that terminates the app, so log the message and drop it instead.
    // nlohmann::json::exception derives from std::exception, so one handler covers both.
    std::string type;

    try
    {
        type = msg.value("type", "");

        if (!type.empty())
        {
            DispatchByType(c, msg, type);
        }
    }
    catch (const std::exception& e)
    {
        c.AppendSessionLog("Dropped UI message type=" + (type.empty() ? std::string{"(unknown)"} : type) + ": " +
                           e.what());
    }
}

void MessageDispatcher::DispatchByType(PluginController& c, const nlohmann::json& msg, const std::string& type)
{
    // Messages a service registered for itself come first; see MessageHandlerRegistry.
    if (c.mMessageHandlers.Dispatch(type, msg))
    {
        return;
    }

    if (DispatchStateAndLists(c, msg, type))
    {
        return;
    }

    if (DispatchSettings(c, msg, type))
    {
        return;
    }

    if (DispatchParameters(c, msg, type))
    {
        return;
    }

    if (DispatchPresetsAndResources(c, msg, type))
    {
        return;
    }

    if (DispatchSignalPath(c, msg, type))
    {
        return;
    }

    if (DispatchMixerAndMonitoring(c, msg, type))
    {
        return;
    }

    if (DispatchLibraryAndComposite(c, msg, type))
    {
        return;
    }

    if (DispatchAutomation(c, msg, type))
    {
        return;
    }

    if (DispatchPresetEdits(c, msg, type))
    {
        return;
    }

    // Unknown message type — silently ignored
}
} // namespace guitarfx
