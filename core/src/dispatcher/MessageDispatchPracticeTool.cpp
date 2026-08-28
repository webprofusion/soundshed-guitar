#include "MessageDispatcher.h"
#include "PluginController.h"

namespace guitarfx
{
bool MessageDispatcher::DispatchPracticeTool(PluginController& c, const nlohmann::json& msg, const std::string& type)
{
    if (type == "browsePracticeToolFile")
    {
        c.HandleBrowsePracticeToolFileRequest();
        return true;
    }

    if (type == "loadPracticeToolFile")
    {
        c.HandleLoadPracticeToolFileRequest(msg);
        return true;
    }

    if (type == "loadPracticeToolFileData")
    {
        c.HandleLoadPracticeToolFileDataRequest(msg);
        return true;
    }

    if (type == "setPracticeToolTransport")
    {
        c.HandleSetPracticeToolTransportRequest(msg);
        return true;
    }

    if (type == "seekPracticeToolFile")
    {
        c.HandleSeekPracticeToolFileRequest(msg);
        return true;
    }

    if (type == "setPracticeToolSpeed")
    {
        c.HandleSetPracticeToolSpeedRequest(msg);
        return true;
    }

    if (type == "setPracticeToolPitch")
    {
        c.HandleSetPracticeToolPitchRequest(msg);
        return true;
    }

    if (type == "setPracticeToolGain")
    {
        c.HandleSetPracticeToolGainRequest(msg);
        return true;
    }

    if (type == "setPracticeToolBalance")
    {
        c.HandleSetPracticeToolBalanceRequest(msg);
        return true;
    }

    if (type == "setPracticeToolLoopRegion")
    {
        c.HandleSetPracticeToolLoopRegionRequest(msg);
        return true;
    }

    if (type == "setPracticeToolLooping")
    {
        c.HandleSetPracticeToolLoopingRequest(msg);
        return true;
    }

    return false;
}
} // namespace guitarfx
