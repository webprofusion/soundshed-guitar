/**
 * PluginControllerTuner.cpp - Tuner control and the signal-path test tone.
 *
 * Both are thin: TunerService hands pitch readings from the audio thread to the
 * UI and SignalTestService owns the tone and its measurement. What lives here
 * is the message plumbing plus the mixer state each one needs set under the DSP
 * lock (tuner enable, reference frequency, live-vs-muted monitoring).
 */

#include "PluginController.h"

#include "controller/SignalTestService.h"
#include "controller/TunerService.h"

namespace guitarfx
{
bool PluginController::StartSignalPathTest(double frequencyHz, double durationSeconds)
{
    return mSignalTest->Start(frequencyHz, durationSeconds, mHost.GetSampleRate());
}

void PluginController::HandleSignalTestRequest(const nlohmann::json& payload)
{
    double freq = payload.value("frequency", 440.0);
    double dur = payload.value("duration", 1.0);
    StartSignalPathTest(freq, dur);
}

void PluginController::HandleTunerRequest(const nlohmann::json& payload)
{
    const std::string action = payload.value("action", "");

    if (action == "start")
    {
        mTuner->SetActive(true);
        double referenceFrequency = 440.0;
        bool liveMode = true;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);

            if (payload.contains("liveMode"))
            {
                mPresetMixer.SetLiveTunerMode(payload.value("liveMode", true));
            }

            if (payload.contains("referenceFrequency"))
            {
                mPresetMixer.SetTunerReferenceFrequency(payload["referenceFrequency"].get<double>());
            }

            mPresetMixer.SetTunerEnabled(true);
            referenceFrequency = mPresetMixer.GetTunerReferenceFrequency();
            liveMode = mPresetMixer.IsLiveTunerMode();
        }

        nlohmann::json message;
        message["type"] = "tunerStarted";
        message["referenceFrequency"] = referenceFrequency;
        message["liveMode"] = liveMode;
        SendMessageToUI(message.dump());
        return;
    }

    if (action == "stop")
    {
        mTuner->SetActive(false);
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.SetTunerEnabled(false);
        }

        nlohmann::json message;
        message["type"] = "tunerStopped";
        SendMessageToUI(message.dump());
        return;
    }

    if (action == "setLiveMode")
    {
        bool liveMode = payload.value("liveMode", true);
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.SetLiveTunerMode(liveMode);
        }

        nlohmann::json message;
        message["type"] = "tunerLiveModeChanged";
        message["liveMode"] = liveMode;
        SendMessageToUI(message.dump());
        return;
    }

    if (action == "setReference")
    {
        double freq = payload.value("referenceFrequency", 440.0);
        double effectiveFrequency = 440.0;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.SetTunerReferenceFrequency(freq);
            effectiveFrequency = mPresetMixer.GetTunerReferenceFrequency();
        }

        nlohmann::json message;
        message["type"] = "tunerReferenceChanged";
        message["referenceFrequency"] = effectiveFrequency;
        SendMessageToUI(message.dump());
        return;
    }

    if (payload.contains("enabled"))
    {
        bool enabled = payload.value("enabled", false);
        mTuner->SetActive(enabled);
        double referenceFrequency = 440.0;
        bool liveMode = true;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.SetTunerEnabled(enabled);
            referenceFrequency = mPresetMixer.GetTunerReferenceFrequency();
            liveMode = mPresetMixer.IsLiveTunerMode();
        }

        nlohmann::json reply;
        reply["type"] = enabled ? "tunerStarted" : "tunerStopped";
        reply["referenceFrequency"] = referenceFrequency;
        reply["liveMode"] = liveMode;
        SendMessageToUI(reply.dump());
    }
}

void PluginController::HandleSetTunerEnabledRequest(const nlohmann::json& payload)
{
    bool enabled = payload.value("enabled", false);
    mTuner->SetActive(enabled);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetTunerEnabled(enabled);
}

void PluginController::HandleSetTunerReferenceRequest(const nlohmann::json& payload)
{
    double freq = payload.value("frequency", 440.0);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetTunerReferenceFrequency(freq);
}
} // namespace guitarfx
