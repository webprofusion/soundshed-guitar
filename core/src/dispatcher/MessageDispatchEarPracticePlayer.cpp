#include "MessageDispatcher.h"
#include "PluginController.h"

namespace guitarfx
{

bool MessageDispatcher::DispatchEarPracticePlayer(PluginController& c,
                                            const nlohmann::json& msg,
                                            const std::string& type)
{
    if (type == "browseEarPracticePlayerFile")
    {
        c.HandleBrowseEarPracticePlayerFileRequest();
        return true;
    }
    if (type == "loadEarPracticePlayerFile")
    {
        c.HandleLoadEarPracticePlayerFileRequest(msg);
        return true;
    }
    if (type == "loadEarPracticePlayerFileData")
    {
        c.HandleLoadEarPracticePlayerFileDataRequest(msg);
        return true;
    }
    if (type == "setEarPracticePlayerTransport")
    {
        c.HandleSetEarPracticePlayerTransportRequest(msg);
        return true;
    }
    if (type == "seekEarPracticePlayerFile")
    {
        c.HandleSeekEarPracticePlayerFileRequest(msg);
        return true;
    }
    if (type == "setEarPracticePlayerSpeed")
    {
        c.HandleSetEarPracticePlayerSpeedRequest(msg);
        return true;
    }
    if (type == "setEarPracticePlayerPitch")
    {
        c.HandleSetEarPracticePlayerPitchRequest(msg);
        return true;
    }
    if (type == "setEarPracticePlayerGain")
    {
        c.HandleSetEarPracticePlayerGainRequest(msg);
        return true;
    }
    if (type == "setEarPracticePlayerBalance")
    {
        c.HandleSetEarPracticePlayerBalanceRequest(msg);
        return true;
    }
    if (type == "setEarPracticePlayerLoopRegion")
    {
        c.HandleSetEarPracticePlayerLoopRegionRequest(msg);
        return true;
    }
    if (type == "setEarPracticePlayerLooping")
    {
        c.HandleSetEarPracticePlayerLoopingRequest(msg);
        return true;
    }
    return false;
}

} // namespace guitarfx
