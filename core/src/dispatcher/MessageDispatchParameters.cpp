#include "MessageDispatcher.h"
#include "PluginController.h"

namespace guitarfx
{
bool MessageDispatcher::DispatchParameters(PluginController& c, const nlohmann::json& msg, const std::string& type)
{
    if (type == "setParameter")
    {
        c.HandleSetParameterRequest(msg);
        return true;
    }

    if (type == "setGlobalChainParam")
    {
        c.HandleSetGlobalChainParamRequest(msg);
        return true;
    }

    return false;
}
} // namespace guitarfx
