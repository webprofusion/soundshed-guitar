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

    /// `payload["audio"]` is the clip; an optional `payload["region"]`
    /// (`{startSec, endSec, looping}`) confines playback to a sub-range and
    /// optionally repeats it.
    void StartPreview(const nlohmann::json& payload);
    void StopPreview();
    void OnIdle();
    [[nodiscard]] bool IsPreviewActive() const;

    /// Retargets the region of the *currently playing* preview, so dragging a
    /// pair of trim markers retunes the loop without restarting the clip.
    /// Message-thread only: it re-renders the wrap crossfade, which must never
    /// happen on the audio thread. An inverted or degenerate pair clears the
    /// region (whole clip, played once). No-op with nothing playing.
    void SetPreviewRegion(double startSec, double endSec, bool looping);

  private:
    // Grants DemoPreviewServiceTestAccess (core/tests/DemoPreviewLoopTests.cpp)
    // the private surface needed to drive the loop wrap synchronously against a
    // synthetic buffer — no base64, no decode, no audio device.
    friend struct DemoPreviewServiceTestAccess;

    struct DemoAudioBuffer
    {
        std::string id;
        std::string title;
        double sampleRate = 0.0;
        int channels = 0;
        std::vector<std::vector<float>> channelSamples;
    };

    /// The slice of the clip to play, whether to repeat it, and the wrap
    /// crossfade — precomputed together so the audio thread only ever copies.
    ///
    /// Repeating a clip by seeking back to the start puts a discontinuity at
    /// the wrap, which reads as a click every cycle — the one artefact you
    /// cannot un-hear when a riff repeats. `carryL/R` hold an equal-power blend
    /// of the frames either side of the seam, computed once on the message
    /// thread; the audio thread plays them out across the wrap and resumes at
    /// `startFrame + carry size`. Empty when not looping. Bundled into one
    /// immutable object so bounds, the repeat flag and the crossfade can never
    /// be observed half-updated: a change builds a new one and swaps the
    /// pointer.
    struct PreviewRegion
    {
        std::size_t startFrame = 0;
        std::size_t endFrame = 0; // exclusive
        bool looping = false;
        std::vector<float> carryL;
        std::vector<float> carryR;
    };

    /// Builds the region bounds and, when looping, their wrap crossfade.
    /// Returns null when the range is degenerate or falls outside the clip —
    /// the caller treats that as "play the whole thing once" rather than an
    /// error, because it is reachable just by dragging a marker past its
    /// partner.
    [[nodiscard]] static std::shared_ptr<const PreviewRegion>
    BuildRegion(const std::shared_ptr<DemoAudioBuffer>& buffer, double startSec, double endSec, bool looping);

    IPluginHost& mHost;
    MultiPresetMixer& mPresetMixer;
    std::mutex& mDSPMutex;
    std::atomic<bool>& mSignalTestActive;
    std::function<void(const std::string&, const std::string&)> mReportError;
    std::function<void(const std::string&)> mSendMessage;

    std::shared_ptr<DemoAudioBuffer> mDemoAudioBuffer;
    std::atomic<size_t> mDemoAudioCursor{0};
    std::atomic<bool> mDemoAudioActive{false};

    // Null unless the current preview is confined to a range. Read on the audio
    // thread via an atomic load of the shared_ptr, so a swap can never tear.
    std::shared_ptr<const PreviewRegion> mPreviewRegion;

    // Audio-thread only: how far through the wrap crossfade we are. Past the
    // carry's size means "not currently crossfading".
    std::size_t mCarryPos = 0;
};
} // namespace guitarfx
