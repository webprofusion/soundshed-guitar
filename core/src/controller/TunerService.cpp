#include "controller/TunerService.h"

#include "MessageHandlerRegistry.h"
#include "dsp/MultiPresetMixer.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include <nlohmann/json.hpp>

namespace guitarfx
{
namespace
{
/// The tuner's note ("C#/Db", octave 3) as a MIDI note (uiclient/ScaleGuide.h ReadingMidi).
std::optional<int> ReadingMidi(const std::string& noteName, int octave)
{
    static constexpr int kLetters[] = {9, 11, 0, 2, 4, 5, 7}; // A … G

    if (noteName.empty() || noteName[0] < 'A' || noteName[0] > 'G')
    {
        return std::nullopt;
    }

    const int sharp = noteName.size() > 1 && noteName[1] == '#' ? 1 : 0;
    return (octave + 1) * 12 + (kLetters[noteName[0] - 'A'] + sharp) % 12;
}
} // namespace

TunerService::TunerService(SendMessageFn sendMessage, MultiPresetMixer& mixer, std::mutex& dspMutex)
    : mSendMessage(std::move(sendMessage)), mMixer(mixer), mDSPMutex(dspMutex)
{
}

void TunerService::RegisterMessageHandlers(MessageHandlerRegistry& registry)
{
    registry.Register("tuner", [this](const nlohmann::json& payload) { HandleTunerRequest(payload); });
}

void TunerService::HandleTunerRequest(const nlohmann::json& payload)
{
    const std::string action = payload.value("action", "");

    if (action == "start")
    {
        SetActive(true);
        double referenceFrequency = 440.0;
        bool liveMode = true;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);

            if (payload.contains("liveMode"))
            {
                mMixer.SetLiveTunerMode(payload.value("liveMode", true));
            }

            if (payload.contains("referenceFrequency"))
            {
                mMixer.SetTunerReferenceFrequency(payload["referenceFrequency"].get<double>());
            }

            mMixer.SetTunerEnabled(true);
            referenceFrequency = mMixer.GetTunerReferenceFrequency();
            liveMode = mMixer.IsLiveTunerMode();
        }

        nlohmann::json message;
        message["type"] = "tunerStarted";
        message["referenceFrequency"] = referenceFrequency;
        message["liveMode"] = liveMode;
        mSendMessage(message.dump());
        return;
    }

    if (action == "stop")
    {
        SetActive(false);
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mMixer.SetTunerEnabled(false);
        }

        nlohmann::json message;
        message["type"] = "tunerStopped";
        mSendMessage(message.dump());
        return;
    }

    if (action == "setLiveMode")
    {
        bool liveMode = payload.value("liveMode", true);
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mMixer.SetLiveTunerMode(liveMode);
        }

        nlohmann::json message;
        message["type"] = "tunerLiveModeChanged";
        message["liveMode"] = liveMode;
        mSendMessage(message.dump());
        return;
    }

    // The Scale Guide's LISTEN: readings without turning the tuner on, so nothing is muted, and
    // a chroma vector with each when `chroma` is set. Not acknowledged: readings are the answer.
    if (action == "listen")
    {
        const bool enabled = payload.value("enabled", false);
        const bool chroma = payload.value("chroma", false);

        if (enabled != mListening || chroma != mListenChroma)
        {
            mListen.Reset();
        }

        mListening = enabled;
        mListenChroma = enabled && chroma;
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mMixer.SetTunerListening(enabled, chroma);
        return;
    }

    if (action == "setReference")
    {
        double freq = payload.value("referenceFrequency", 440.0);
        double effectiveFrequency = 440.0;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mMixer.SetTunerReferenceFrequency(freq);
            effectiveFrequency = mMixer.GetTunerReferenceFrequency();
        }

        nlohmann::json message;
        message["type"] = "tunerReferenceChanged";
        message["referenceFrequency"] = effectiveFrequency;
        mSendMessage(message.dump());
        return;
    }

    if (payload.contains("enabled"))
    {
        bool enabled = payload.value("enabled", false);
        SetActive(enabled);
        double referenceFrequency = 440.0;
        bool liveMode = true;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mMixer.SetTunerEnabled(enabled);
            referenceFrequency = mMixer.GetTunerReferenceFrequency();
            liveMode = mMixer.IsLiveTunerMode();
        }

        nlohmann::json reply;
        reply["type"] = enabled ? "tunerStarted" : "tunerStopped";
        reply["referenceFrequency"] = referenceFrequency;
        reply["liveMode"] = liveMode;
        mSendMessage(reply.dump());
    }
}

void TunerService::HoldListen(double seconds)
{
    const auto length = std::chrono::duration<double>(std::max(0.0, seconds));
    mListenHeldUntil =
        std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(length);
}

void TunerService::PostReading(const Reading& reading)
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mReading = reading;
    }
    mPending.store(true, std::memory_order_release);
}

void TunerService::OnIdle()
{
    if (!mPending.load(std::memory_order_acquire))
    {
        return;
    }

    mPending.store(false, std::memory_order_release);

    Reading reading;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        reading = mReading;
    }

    nlohmann::json msg;
    msg["type"] = "tunerUpdate";
    msg["noteName"] = reading.noteName;
    msg["octave"] = reading.octave;
    msg["frequency"] = reading.frequency;
    msg["centOffset"] = reading.centOffset;
    msg["confidence"] = reading.confidence;
    msg["detected"] = reading.detected;
    msg["levelDb"] = std::round(reading.levelDb * 10.0) / 10.0;

    if (reading.hasChroma)
    {
        nlohmann::json chroma = nlohmann::json::array();

        for (const float value : reading.chroma)
        {
            chroma.push_back(std::round(value * 1000.0f) / 1000.0f);
        }

        msg["chroma"] = chroma;
    }

    if (mListening)
    {
        const bool held = std::chrono::steady_clock::now() < mListenHeldUntil;

        if (mListenChroma && reading.hasChroma)
        {
            mListen.PushChroma(reading.chroma, reading.levelDb, held);
        }
        else if (!mListenChroma)
        {
            mListen.PushNote(reading.detected ? ReadingMidi(reading.noteName, reading.octave) : std::nullopt, held);
        }

        msg["heard"] = mListen.Json(mListenChroma);
    }

    if (mSendMessage)
    {
        mSendMessage(msg.dump());
    }
}
} // namespace guitarfx
