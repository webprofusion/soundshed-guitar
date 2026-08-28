#include "MessageDispatcher.h"
#include "PluginController.h"

namespace guitarfx
{

bool MessageDispatcher::DispatchAutomation(PluginController& c, const nlohmann::json& msg, const std::string& type)
{
    if (type == "getAutomation")
    {
        c.HandleGetAutomationRequest();
        return true;
    }
    if (type == "setAutomationSlot")
    {
        c.HandleSetAutomationSlotRequest(msg);
        return true;
    }
    if (type == "removeAutomationSlot")
    {
        c.HandleRemoveAutomationSlotRequest(msg);
        return true;
    }
    if (type == "setAutomationValue")
    {
        c.HandleSetAutomationValueRequest(msg);
        return true;
    }
    if (type == "armMidiLearn")
    {
        c.HandleArmMidiLearnRequest(msg);
        return true;
    }
    if (type == "cancelMidiLearn")
    {
        c.HandleCancelMidiLearnRequest();
        return true;
    }
    if (type == "setMidiLogEnabled")
    {
        c.SetMidiLogEnabled(msg.value("enabled", false));
        return true;
    }
    return false;
}

} // namespace guitarfx
