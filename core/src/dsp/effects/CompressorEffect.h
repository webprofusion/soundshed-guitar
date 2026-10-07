#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/DriveOutputLimiter.h"
#include "dsp/effects/DynamicsPresets.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace guitarfx
{
namespace compressor_detail
{
/// Time constant Makeup and Mix glide over, so a knob turn, automation or a preset change
/// does not step the level mid-note and click.
inline constexpr double kLevelSmoothingMs = 20.0;

[[nodiscard]] inline float SmoothingCoefficient(double sampleRate)
{
    return static_cast<float>(1.0 - std::exp(-1.0 / (kLevelSmoothingMs * 0.001 * sampleRate)));
}

/// A non-finite input sample would poison the detector for good: measured, one infinity left
/// the VCA putting out NaN from then on, and one NaN left the Opto never compressing again.
/// Scrubbed to silence, so neither the detector nor anything downstream ever sees one.
[[nodiscard]] inline float ScrubbedInput(const float* channel, int index, float fallback)
{
    const float value = channel ? channel[index] : fallback;
    return IsFinite(value) ? value : 0.0f;
}

/// Makeup gain and Mix, gliding to what the controls say. Starts on the targets after a
/// Prepare or Reset, so a render begins where it is set rather than ramping up to it.
struct LevelSmoother
{
    float makeupGain = 1.0f;
    float mix = 1.0f;
    bool primed = false;

    void Advance(float makeupTarget, float mixTarget, float coefficient)
    {
        if (!primed)
        {
            makeupGain = makeupTarget;
            mix = mixTarget;
            primed = true;
            return;
        }

        makeupGain += coefficient * (makeupTarget - makeupGain);
        mix += coefficient * (mixTarget - mix);
    }
};
} // namespace compressor_detail

/**
 * VCA-style compressor: a peak detector, a soft-knee gain computer, attack/release smoothing
 * in the gain domain, makeup gain, dry/wet mix and optional soft clip. With Stereo Link on (the
 * default) both channels follow the louder one, so a stereo image holds still; off, each channel
 * is compressed on its own level.
 */
class CompressorEffect : public EffectProcessor
{
  public:
    /// Keeps its channels apart once dual mono switches off the stereo link (EffectProcessor::KeepsChannelsSeparate).
    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    void SetDualMono(bool dualMono) override
    {
        mDualMono.store(dualMono, std::memory_order_relaxed);
    }

    /// Identical sides in, identical sides out, whatever the settings (EffectProcessor::CanWiden).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    void Prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        if (!ValidatePrepare(sampleRate, 1))
        {
            return;
        }

        mSampleRate = sampleRate;
        UpdateCoefficients();
        // Clear state when sample rate changes to ensure clean initialization
        Reset();
    }

    void Reset() override
    {
        mEnvelope = {0.0f, 0.0f};
        mGainReduction.store(0.0f, std::memory_order_relaxed);
        mLevels.primed = false;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        const float thresholdDb = mThresholdDb.load(std::memory_order_relaxed);
        const float ratio = mRatio.load(std::memory_order_relaxed);
        const float knee = mKnee.load(std::memory_order_relaxed);
        const float makeupGainTarget = std::pow(10.0f, mMakeupDb.load(std::memory_order_relaxed) * 0.05f);
        const float mixTarget = mMix.load(std::memory_order_relaxed);
        const float softClip = mSoftClip.load(std::memory_order_relaxed);
        const float attackCoef = mAttackCoef.load(std::memory_order_relaxed);
        const float releaseCoef = mReleaseCoef.load(std::memory_order_relaxed);
        const float levelCoef = mLevelCoef.load(std::memory_order_relaxed);
        // Dual mono keeps each side to its own detector, whatever Stereo Link says.
        const bool linked =
            mStereoLink.load(std::memory_order_relaxed) >= 0.5f && !mDualMono.load(std::memory_order_relaxed);
        constexpr float kSoftClipTransparentKnee = 0.995f;
        constexpr float kSoftClipMaxKneeReduction = 0.075f;
        // Move the soft-clip knee from nearly transparent at 0.995 to 0.92 at
        // full softClip so makeup gain is rounded off before the hard ceiling.
        const float clipKnee = kSoftClipTransparentKnee - kSoftClipMaxKneeReduction * std::clamp(softClip, 0.0f, 1.0f);
        const auto computeTargetGainDb = [thresholdDb, ratio, knee](float detectorDb) {
            // Gain computer: above-threshold level is reduced by the ratio; the
            // optional knee eases into compression around the threshold.
            float targetGainDb = 0.0f;

            if (detectorDb > thresholdDb)
            {
                const float overDb = detectorDb - thresholdDb;
                const float compressedDb = overDb / ratio;
                targetGainDb = compressedDb - overDb;
            }

            if (knee > 0.0f)
            {
                const float halfKnee = knee * 0.5f;

                if (detectorDb > thresholdDb - halfKnee && detectorDb < thresholdDb + halfKnee)
                {
                    const float x = detectorDb - thresholdDb + halfKnee;
                    const float t = x / knee;
                    const float overDb = t * t * knee * 0.5f;
                    const float compressedDb = overDb / ratio;
                    targetGainDb = compressedDb - overDb;
                }
            }

            return targetGainDb;
        };

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = compressor_detail::ScrubbedInput(inputs[0], i, 0.0f);
            const float inR = compressor_detail::ScrubbedInput(inputs[1], i, inL);

            // Linked, the louder channel drives both detectors, so both get one gain.
            const float peakL = std::abs(inL);
            const float peakR = std::abs(inR);
            const float peaks[2] = {linked ? std::max(peakL, peakR) : peakL, linked ? std::max(peakL, peakR) : peakR};
            float gains[2] = {1.0f, 1.0f};
            mLevels.Advance(makeupGainTarget, mixTarget, levelCoef);

            for (int ch = 0; ch < 2; ++ch)
            {
                const float peakDb = (peaks[ch] > 1e-10f) ? 20.0f * std::log10(peaks[ch]) : -200.0f;
                const float targetEnv = -computeTargetGainDb(peakDb);
                float& envelope = mEnvelope[static_cast<std::size_t>(ch)];

                if (targetEnv > envelope)
                {
                    envelope += attackCoef * (targetEnv - envelope);
                }
                else
                {
                    envelope += releaseCoef * (targetEnv - envelope);
                }

                const float wetGain = std::pow(10.0f, -envelope * 0.05f) * mLevels.makeupGain;
                gains[ch] = (1.0f - mLevels.mix) + wetGain * mLevels.mix;
            }

            float mixedL = inL * gains[0];
            float mixedR = inR * gains[1];

            if (softClip > 0.0f)
            {
                mixedL = drive_output_limiter::SoftClipNearCeiling(mixedL, clipKnee, 1.0f);
                mixedR = drive_output_limiter::SoftClipNearCeiling(mixedR, clipKnee, 1.0f);
            }

            if (outputs[0])
            {
                outputs[0][i] = mixedL;
            }

            if (outputs[1])
            {
                outputs[1][i] = mixedR;
            }
        }

        mGainReduction.store(std::max(mEnvelope[0], mEnvelope[1]), std::memory_order_relaxed);
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "threshold")
        {
            mThresholdDb.store(static_cast<float>(std::clamp(value, -60.0, 0.0)), std::memory_order_relaxed);
        }
        else if (key == "ratio")
        {
            mRatio.store(static_cast<float>(std::clamp(value, 1.0, 20.0)), std::memory_order_relaxed);
        }
        else if (key == "attack")
        {
            mAttackMs.store(static_cast<float>(std::clamp(value, 0.1, 500.0)), std::memory_order_relaxed);
            UpdateCoefficients();
        }
        else if (key == "release")
        {
            mReleaseMs.store(static_cast<float>(std::clamp(value, 10.0, 2000.0)), std::memory_order_relaxed);
            UpdateCoefficients();
        }
        else if (key == "knee")
        {
            mKnee.store(static_cast<float>(std::clamp(value, 0.0, 24.0)), std::memory_order_relaxed);
        }
        else if (key == "makeup")
        {
            mMakeupDb.store(static_cast<float>(std::clamp(value, 0.0, 24.0)), std::memory_order_relaxed);
        }
        else if (key == "mix")
        {
            mMix.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "softClip")
        {
            mSoftClip.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "stereoLink")
        {
            mStereoLink.store(value >= 0.5 ? 1.0f : 0.0f, std::memory_order_relaxed);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "threshold")
        {
            return mThresholdDb.load(std::memory_order_relaxed);
        }

        if (key == "ratio")
        {
            return mRatio.load(std::memory_order_relaxed);
        }

        if (key == "attack")
        {
            return mAttackMs.load(std::memory_order_relaxed);
        }

        if (key == "release")
        {
            return mReleaseMs.load(std::memory_order_relaxed);
        }

        if (key == "knee")
        {
            return mKnee.load(std::memory_order_relaxed);
        }

        if (key == "makeup")
        {
            return mMakeupDb.load(std::memory_order_relaxed);
        }

        if (key == "mix")
        {
            return mMix.load(std::memory_order_relaxed);
        }

        if (key == "softClip")
        {
            return mSoftClip.load(std::memory_order_relaxed);
        }

        if (key == "stereoLink")
        {
            return mStereoLink.load(std::memory_order_relaxed);
        }

        return 0.0;
    }

    /// Current gain reduction in dB, as of the end of the last block. Safe from any thread.
    [[nodiscard]] double GetGainReduction() const
    {
        return mGainReduction.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "compressor_vca";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "dynamics";
    }

  private:
    void UpdateCoefficients()
    {
        if (mSampleRate > 0)
        {
            mAttackCoef.store(static_cast<float>(1.0 - std::exp(-1.0 / (mAttackMs.load(std::memory_order_relaxed) *
                                                                        0.001 * mSampleRate))),
                              std::memory_order_relaxed);
            mReleaseCoef.store(static_cast<float>(1.0 - std::exp(-1.0 / (mReleaseMs.load(std::memory_order_relaxed) *
                                                                         0.001 * mSampleRate))),
                               std::memory_order_relaxed);
            mLevelCoef.store(compressor_detail::SmoothingCoefficient(mSampleRate), std::memory_order_relaxed);
        }
    }

    // Parameters
    std::atomic<float> mThresholdDb{-20.0f};
    std::atomic<float> mRatio{4.0f};
    std::atomic<float> mAttackMs{10.0f};
    std::atomic<float> mReleaseMs{100.0f};
    std::atomic<float> mKnee{6.0f};
    std::atomic<float> mMakeupDb{0.0f};
    std::atomic<float> mMix{1.0f};
    std::atomic<float> mSoftClip{0.0f};

    // Coefficients (derived from params + sample rate, updated atomically)
    std::atomic<float> mAttackCoef{0.0f};
    std::atomic<float> mReleaseCoef{0.0f};
    std::atomic<float> mLevelCoef{1.0f};

    std::atomic<float> mStereoLink{1.0f};
    std::atomic<bool> mDualMono{false};

    // State (audio thread only, no synchronization needed)
    std::array<float, 2> mEnvelope = {0.0f, 0.0f}; // gain reduction in dB, per channel
    compressor_detail::LevelSmoother mLevels;
    std::atomic<float> mGainReduction{0.0f};
};

/**
 * Opto-style compressor: an RMS detector (5 ms), a hard-knee gain computer, and a cell whose
 * release slows as it holds more gain reduction. Linked across the channels like the VCA, on
 * the louder channel's power unless Stereo Link is off.
 */
class OptoCompressorEffect : public EffectProcessor
{
  public:
    /// Keeps its channels apart once dual mono switches off the stereo link (EffectProcessor::KeepsChannelsSeparate).
    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    void SetDualMono(bool dualMono) override
    {
        mDualMono.store(dualMono, std::memory_order_relaxed);
    }

    /// Identical sides in, identical sides out, whatever the settings (EffectProcessor::CanWiden).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    void Prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        if (!ValidatePrepare(sampleRate, 1))
        {
            return;
        }

        mSampleRate = sampleRate;
        UpdateCoefficients();
        // Clear state when sample rate changes to ensure clean initialization
        Reset();
    }

    void Reset() override
    {
        mEnvelope = {0.0f, 0.0f};
        mOptoCellState = {0.0f, 0.0f};
        mLevels.primed = false;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        // The input is scrubbed, but the power of a finite sample can still overflow. A
        // non-finite detector would compare false against the threshold and never compress again.
        for (int ch = 0; ch < 2; ++ch)
        {
            if (!IsFinite(mEnvelope[static_cast<std::size_t>(ch)]) ||
                !IsFinite(mOptoCellState[static_cast<std::size_t>(ch)]))
            {
                Reset();
                break;
            }
        }

        const float thresholdDb = mThresholdDb.load(std::memory_order_relaxed);
        const float ratio = mRatio.load(std::memory_order_relaxed);
        const float makeupGainTarget = std::pow(10.0f, mMakeupDb.load(std::memory_order_relaxed) * 0.05f);
        const float mixTarget = mMix.load(std::memory_order_relaxed);
        const float softClip = mSoftClip.load(std::memory_order_relaxed);
        const float attackCoef = mAttackCoef.load(std::memory_order_relaxed);
        const float releaseCoef = mReleaseCoef.load(std::memory_order_relaxed);
        const float detectCoef = mDetectCoef.load(std::memory_order_relaxed);
        const float levelCoef = mLevelCoef.load(std::memory_order_relaxed);
        // Dual mono keeps each side to its own detector, whatever Stereo Link says.
        const bool linked =
            mStereoLink.load(std::memory_order_relaxed) >= 0.5f && !mDualMono.load(std::memory_order_relaxed);
        const float clipKnee = 0.995f - 0.075f * std::clamp(softClip, 0.0f, 1.0f);

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = compressor_detail::ScrubbedInput(inputs[0], i, 0.0f);
            const float inR = compressor_detail::ScrubbedInput(inputs[1], i, inL);

            // Linked, the louder channel's power drives both cells, so both get one gain.
            const float powerL = inL * inL;
            const float powerR = inR * inR;
            const float powers[2] = {linked ? std::max(powerL, powerR) : powerL,
                                     linked ? std::max(powerL, powerR) : powerR};
            float gains[2] = {1.0f, 1.0f};
            mLevels.Advance(makeupGainTarget, mixTarget, levelCoef);

            for (int ch = 0; ch < 2; ++ch)
            {
                float& envelope = mEnvelope[static_cast<std::size_t>(ch)];
                float& cell = mOptoCellState[static_cast<std::size_t>(ch)];
                envelope += detectCoef * (powers[ch] - envelope);
                const float rms = std::sqrt(envelope);
                const float rmsDb = (rms > 1e-10f) ? 20.0f * std::log10(rms) : -200.0f;

                float targetReduction = 0.0f;

                if (rmsDb > thresholdDb)
                {
                    const float overDb = rmsDb - thresholdDb;
                    targetReduction = overDb * (1.0f - 1.0f / ratio);
                }

                float optoSpeed = (targetReduction > cell) ? attackCoef : releaseCoef;

                if (targetReduction < cell)
                {
                    optoSpeed *= std::max(0.2f, 1.0f - cell * 0.02f);
                }

                cell += optoSpeed * (targetReduction - cell);
                const float wetGain = std::pow(10.0f, -cell * 0.05f) * mLevels.makeupGain;
                gains[ch] = (1.0f - mLevels.mix) + wetGain * mLevels.mix;
            }

            float outL = inL * gains[0];
            float outR = inR * gains[1];

            if (softClip > 0.0f)
            {
                outL = drive_output_limiter::SoftClipNearCeiling(outL, clipKnee, 1.0f);
                outR = drive_output_limiter::SoftClipNearCeiling(outR, clipKnee, 1.0f);
            }

            if (outputs[0])
            {
                outputs[0][i] = outL;
            }

            if (outputs[1])
            {
                outputs[1][i] = outR;
            }
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "threshold")
        {
            mThresholdDb.store(static_cast<float>(std::clamp(value, -60.0, 0.0)), std::memory_order_relaxed);
        }
        else if (key == "ratio")
        {
            mRatio.store(static_cast<float>(std::clamp(value, 1.0, 20.0)), std::memory_order_relaxed);
        }
        else if (key == "attack")
        {
            mAttackMs.store(static_cast<float>(std::clamp(value, 5.0, 200.0)), std::memory_order_relaxed);
            UpdateCoefficients();
        }
        else if (key == "release")
        {
            mReleaseMs.store(static_cast<float>(std::clamp(value, 50.0, 3000.0)), std::memory_order_relaxed);
            UpdateCoefficients();
        }
        else if (key == "makeup")
        {
            mMakeupDb.store(static_cast<float>(std::clamp(value, 0.0, 24.0)), std::memory_order_relaxed);
        }
        else if (key == "mix")
        {
            mMix.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "softClip")
        {
            mSoftClip.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "stereoLink")
        {
            mStereoLink.store(value >= 0.5 ? 1.0f : 0.0f, std::memory_order_relaxed);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "threshold")
        {
            return mThresholdDb.load(std::memory_order_relaxed);
        }

        if (key == "ratio")
        {
            return mRatio.load(std::memory_order_relaxed);
        }

        if (key == "attack")
        {
            return mAttackMs.load(std::memory_order_relaxed);
        }

        if (key == "release")
        {
            return mReleaseMs.load(std::memory_order_relaxed);
        }

        if (key == "makeup")
        {
            return mMakeupDb.load(std::memory_order_relaxed);
        }

        if (key == "mix")
        {
            return mMix.load(std::memory_order_relaxed);
        }

        if (key == "softClip")
        {
            return mSoftClip.load(std::memory_order_relaxed);
        }

        if (key == "stereoLink")
        {
            return mStereoLink.load(std::memory_order_relaxed);
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "compressor_opto";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "dynamics";
    }

  private:
    void UpdateCoefficients()
    {
        if (mSampleRate > 0)
        {
            mAttackCoef.store(static_cast<float>(1.0 - std::exp(-1.0 / (mAttackMs.load(std::memory_order_relaxed) *
                                                                        0.001 * mSampleRate))),
                              std::memory_order_relaxed);
            mReleaseCoef.store(static_cast<float>(1.0 - std::exp(-1.0 / (mReleaseMs.load(std::memory_order_relaxed) *
                                                                         0.001 * mSampleRate))),
                               std::memory_order_relaxed);
            mDetectCoef.store(static_cast<float>(1.0 - std::exp(-1.0 / (5.0 * 0.001 * mSampleRate))),
                              std::memory_order_relaxed);
            mLevelCoef.store(compressor_detail::SmoothingCoefficient(mSampleRate), std::memory_order_relaxed);
        }
    }

    std::atomic<float> mThresholdDb{-20.0f};
    std::atomic<float> mRatio{3.0f};
    std::atomic<float> mAttackMs{20.0f};
    std::atomic<float> mReleaseMs{300.0f};
    std::atomic<float> mMakeupDb{0.0f};
    std::atomic<float> mMix{1.0f};
    std::atomic<float> mSoftClip{0.0f};

    std::atomic<float> mAttackCoef{0.0f};
    std::atomic<float> mReleaseCoef{0.0f};
    std::atomic<float> mDetectCoef{0.0f};
    std::atomic<float> mLevelCoef{1.0f};
    std::atomic<float> mStereoLink{1.0f};
    std::atomic<bool> mDualMono{false};

    std::array<float, 2> mEnvelope = {0.0f, 0.0f};      // mean power, per channel
    std::array<float, 2> mOptoCellState = {0.0f, 0.0f}; // gain reduction in dB, per channel
    compressor_detail::LevelSmoother mLevels;
};

namespace compressor_detail
{
/// Linked is the default for the same reason as the gate's: one detector on the louder channel
/// gives both the same gain, so a stereo image cannot lean. Independent, each channel is
/// compressed on its own level; StereoProcessingTests holds that mode to it.
[[nodiscard]] inline ParameterDef StereoLinkParameter()
{
    ParameterDef link{"stereoLink", "Stereo Link", 1.0, 0.0, 1.0, "", "", true, 1.0};
    link.labels = {"Independent", "Linked"};
    return link;
}
} // namespace compressor_detail

inline void RegisterCompressorEffects()
{
    using compressor_detail::StereoLinkParameter;

    // VCA compressor
    {
        EffectTypeInfo info;
        info.type = EffectGuids::kCompressorVca;
        info.aliases = {"compressor_vca"};
        info.displayName = "VCA Compressor";
        info.category = "dynamics";
        info.description = "Clean, precise VCA-style compressor";
        info.requiresResource = false;
        info.parameters = {{"threshold", "Threshold", -20.0, -60.0, 0.0, "dB"},
                           {"ratio", "Ratio", 4.0, 1.0, 20.0, ":1"},
                           {"attack", "Attack", 10.0, 0.1, 500.0, "ms"},
                           {"release", "Release", 100.0, 10.0, 2000.0, "ms"},
                           {"knee", "Knee", 6.0, 0.0, 24.0, "dB"},
                           {"makeup", "Makeup", 0.0, 0.0, 24.0, "dB"},
                           {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
                           {"softClip", "Soft Clip", 0.0, 0.0, 1.0, "amount", "Output", true},
                           StereoLinkParameter()};
        info.presets = dynamics_presets::Vca(info.parameters);

        EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<CompressorEffect>(); });
    }

    // Opto compressor
    {
        EffectTypeInfo info;
        info.type = EffectGuids::kCompressorOpto;
        info.aliases = {"compressor_opto"};
        info.displayName = "Opto Compressor";
        info.category = "dynamics";
        info.description = "Smooth optical-style compressor";
        info.requiresResource = false;
        info.parameters = {{"threshold", "Threshold", -20.0, -60.0, 0.0, "dB"},
                           {"ratio", "Ratio", 3.0, 1.0, 20.0, ":1"},
                           {"attack", "Attack", 20.0, 5.0, 200.0, "ms"},
                           {"release", "Release", 300.0, 50.0, 3000.0, "ms"},
                           {"makeup", "Makeup", 0.0, 0.0, 24.0, "dB"},
                           {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
                           {"softClip", "Soft Clip", 0.0, 0.0, 1.0, "amount", "Output", true},
                           StereoLinkParameter()};
        info.presets = dynamics_presets::Opto(info.parameters);

        EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<OptoCompressorEffect>(); });
    }
}
} // namespace guitarfx
