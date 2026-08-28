#pragma once

#include "dsp/MultiPresetMixer.h"
#include "IPluginHost.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx
{
class DemoPreviewService
{
  public:
    DemoPreviewService(IPluginHost& host, MultiPresetMixer& mixer, std::mutex& dspMutex,
                       std::atomic<bool>& signalTestActive,
                       std::function<void(const std::string&, const std::string&)> reportError,
                       std::function<void(const std::string&)> sendMessage);

    void MixIntoInput(float** inputs, int numSamples);
    void StartPreview(const nlohmann::json& payload);
    void StopPreview();
    void OnIdle();
    [[nodiscard]] bool IsPreviewActive() const;

  private:
    struct DemoAudioBuffer
    {
        std::string id;
        std::string title;
        double sampleRate = 0.0;
        int channels = 0;
        std::vector<std::vector<float>> channelSamples;
    };

    IPluginHost& mHost;
    MultiPresetMixer& mPresetMixer;
    std::mutex& mDSPMutex;
    std::atomic<bool>& mSignalTestActive;
    std::function<void(const std::string&, const std::string&)> mReportError;
    std::function<void(const std::string&)> mSendMessage;

    std::shared_ptr<DemoAudioBuffer> mDemoAudioBuffer;
    std::atomic<size_t> mDemoAudioCursor{0};
    std::atomic<bool> mDemoAudioActive{false};
};
} // namespace guitarfx
