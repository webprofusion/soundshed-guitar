#include "MessageDispatcher.h"
#include "PluginController.h"

namespace guitarfx
{

bool MessageDispatcher::DispatchAudioPlayer(PluginController& c,
                                            const nlohmann::json& msg,
                                            const std::string& type)
{
    if (type == "browseLocalAudioFile")
    {
        c.HandleBrowseLocalAudioFileRequest();
        return true;
    }
    if (type == "loadLocalAudioFile")
    {
        c.HandleLoadLocalAudioFileRequest(msg);
        return true;
    }
    if (type == "setLocalAudioTransport")
    {
        c.HandleSetLocalAudioTransportRequest(msg);
        return true;
    }
    if (type == "seekLocalAudioFile")
    {
        c.HandleSeekLocalAudioFileRequest(msg);
        return true;
    }
    if (type == "setLocalAudioSpeed")
    {
        c.HandleSetLocalAudioSpeedRequest(msg);
        return true;
    }
    if (type == "setLocalAudioPitch")
    {
        c.HandleSetLocalAudioPitchRequest(msg);
        return true;
    }
    if (type == "setLocalAudioGain")
    {
        c.HandleSetLocalAudioGainRequest(msg);
        return true;
    }
    if (type == "setLocalAudioBalance")
    {
        c.HandleSetLocalAudioBalanceRequest(msg);
        return true;
    }
    if (type == "setLocalAudioLoopRegion")
    {
        c.HandleSetLocalAudioLoopRegionRequest(msg);
        return true;
    }
    if (type == "setLocalAudioLooping")
    {
        c.HandleSetLocalAudioLoopingRequest(msg);
        return true;
    }
    return false;
}

} // namespace guitarfx
