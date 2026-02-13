#include "controller/DemoPreviewService.h"

#include "util/Base64.h"
#include "util/Wav.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace guitarfx
{

DemoPreviewService::DemoPreviewService(IPluginHost& host,
                                       MultiPresetMixer& mixer,
                                       std::mutex& dspMutex,
                                       std::atomic<bool>& signalTestActive,
                                       std::function<void(const std::string&, const std::string&)> reportError,
                                       std::function<void(const std::string&)> sendMessage)
    : mHost(host)
    , mPresetMixer(mixer)
    , mDSPMutex(dspMutex)
    , mSignalTestActive(signalTestActive)
    , mReportError(std::move(reportError))
    , mSendMessage(std::move(sendMessage))
{
}

void DemoPreviewService::MixIntoInput(float** inputs, int numSamples)
{
    if (!mDemoAudioActive.load(std::memory_order_acquire))
        return;

    auto buf = std::atomic_load_explicit(&mDemoAudioBuffer, std::memory_order_acquire);
    if (!buf || buf->channels < 1)
        return;

    size_t cursor = mDemoAudioCursor.load(std::memory_order_relaxed);
    const size_t totalSamples = buf->channelSamples[0].size();
    for (int i = 0; i < numSamples && cursor < totalSamples; ++i, ++cursor)
    {
        float sL = buf->channelSamples[0][cursor];
        float sR = (buf->channels > 1) ? buf->channelSamples[1][cursor] : sL;
        inputs[0][i] += sL;
        inputs[1][i] += sR;
    }
    mDemoAudioCursor.store(cursor, std::memory_order_relaxed);
    if (cursor >= totalSamples)
        mDemoAudioActive.store(false, std::memory_order_release);
}

void DemoPreviewService::StartPreview(const nlohmann::json& payload)
{
    if (mSignalTestActive.load(std::memory_order_acquire))
    {
        mReportError("Demo preview unavailable", "Signal path test is currently running");
        return;
    }

    const auto audioIter = payload.find("audio");
    if (audioIter == payload.end() || !audioIter->is_object())
    {
        mReportError("Demo preview unavailable", "Audio payload is missing");
        return;
    }

    const std::string dataEncoded = audioIter->value("data", "");
    if (dataEncoded.empty())
    {
        mReportError("Demo preview unavailable", "Audio payload did not include data");
        return;
    }

    const auto decodedBytes = util::DecodeBase64(dataEncoded);
    if (decodedBytes.empty())
    {
        mReportError("Demo preview unavailable", "Unable to decode audio data");
        return;
    }

    const auto wavData = util::DecodePcmWav(decodedBytes);
    if (!wavData)
    {
        mReportError("Demo preview unavailable", "Unsupported WAV format");
        return;
    }

    const double hostSampleRate = mHost.GetSampleRate();
    const double targetSampleRate = hostSampleRate > 0.0 ? hostSampleRate : wavData->sampleRate;
    if (targetSampleRate <= 0.0)
    {
        mReportError("Demo preview unavailable", "Target sample rate is invalid");
        return;
    }

    auto resampled = util::ConvertToSampleRate(*wavData, targetSampleRate);
    if (resampled.empty() || resampled.front().empty())
    {
        mReportError("Demo preview unavailable", "Audio buffer is empty");
        return;
    }

    std::size_t minFrames = resampled.front().size();
    for (const auto& channel : resampled)
    {
        if (channel.empty())
        {
            mReportError("Demo preview unavailable", "Audio buffer is empty");
            return;
        }
        minFrames = std::min(minFrames, channel.size());
    }
    if (minFrames == 0)
    {
        mReportError("Demo preview unavailable", "Audio buffer is empty");
        return;
    }
    for (auto& channel : resampled)
    {
        if (channel.size() > minFrames) channel.resize(minFrames);
    }

    auto buffer = std::make_shared<DemoAudioBuffer>();
    buffer->id = audioIter->value("id", "");
    buffer->title = audioIter->value("title", buffer->id);
    buffer->sampleRate = targetSampleRate;
    buffer->channels = static_cast<int>(resampled.size());
    buffer->channelSamples = std::move(resampled);

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mPresetMixer.Reset();
        mDemoAudioCursor.store(0, std::memory_order_release);
        std::atomic_store_explicit(&mDemoAudioBuffer, buffer, std::memory_order_release);
        mDemoAudioActive.store(true, std::memory_order_release);
    }

    nlohmann::json startMsg;
    startMsg["type"] = "previewStarted";
    startMsg["id"] = buffer->id;
    startMsg["title"] = buffer->title;
    mSendMessage(startMsg.dump());
}

void DemoPreviewService::StopPreview()
{
    mDemoAudioActive.store(false, std::memory_order_release);
    auto stopped = std::atomic_exchange_explicit(&mDemoAudioBuffer, std::shared_ptr<DemoAudioBuffer>{}, std::memory_order_acq_rel);
    nlohmann::json msg;
    msg["type"] = "previewStopped";
    if (stopped)
    {
        msg["id"] = stopped->id;
        msg["title"] = stopped->title;
    }
    mSendMessage(msg.dump());
}

void DemoPreviewService::OnIdle()
{
    auto demoBuffer = std::atomic_load_explicit(&mDemoAudioBuffer, std::memory_order_acquire);
    if (!demoBuffer || mDemoAudioActive.load(std::memory_order_acquire))
        return;

    nlohmann::json msg;
    msg["type"] = "previewComplete";
    msg["id"] = demoBuffer->id;
    msg["title"] = demoBuffer->title;
    mSendMessage(msg.dump());
    std::atomic_store_explicit(&mDemoAudioBuffer, std::shared_ptr<DemoAudioBuffer>{}, std::memory_order_release);
}

} // namespace guitarfx
