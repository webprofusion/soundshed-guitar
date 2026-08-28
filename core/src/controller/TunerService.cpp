#include "controller/TunerService.h"

#include <utility>

#include <nlohmann/json.hpp>

namespace guitarfx
{
TunerService::TunerService(SendMessageFn sendMessage) : mSendMessage(std::move(sendMessage))
{
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

    if (mSendMessage)
    {
        mSendMessage(msg.dump());
    }
}
} // namespace guitarfx
