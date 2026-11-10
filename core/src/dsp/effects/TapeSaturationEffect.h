#pragma once

/**
 * Tape: what recording to a worn cassette or a video tape does to a guitar, without an echo.
 *
 * `delay_tape` is a delay that happens to be tape. This is the tape alone, to put anywhere in
 * a chain: the transport's speed error (wow and flutter), the oxide's saturation, the top end
 * the head gap loses, dropouts where the coating has worn, and the hiss underneath.
 *
 * Signal path, per channel:
 *
 *     input -> wow/flutter (a short modulated delay, in the path only while either is up)
 *           -> saturation, 2x oversampled, level-compensated
 *           -> tone low-pass
 *           -> dropout gain
 *           -> + hiss
 *           -> mix with the dry input
 *
 * Every stage that is turned off is out of the path, not merely set to do nothing, so with
 * everything at rest the output is the input, sample for sample, with no latency. A stage
 * coming in or out crossfades over kStageFadeMs.
 *
 * The wow/flutter delay has to sit as far back as the transport swings forward, so it grows
 * with Wow and Flutter and never exceeds 5 ms. It is a pitch wobble's worth of delay, not
 * an echo, and it is not reported as latency: nothing downstream needs aligning to it.
 *
 * Both channels ride the same transport and the same dropouts, as two tracks on one tape do,
 * so the effect never widens a mono signal.
 */

#include "dsp/BiquadDesign.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/HalfBandIir.h"
#include "dsp/effects/FactoryPresetSupport.h"
#include "dsp/effects/TapeTransport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace guitarfx
{
namespace tape_saturation
{
/// How far the transport swings the read point at full Wow and at full Flutter, in seconds.
/// The sums of TapeTransport's component amplitudes (pitch deviation / (2 pi rate)), with the
/// scrape noise's share rounded up into Flutter's.
inline constexpr double kWowSwingSeconds = 0.00385 * 0.55;
/// Wow at 1, as a share of TapeTransport's own full depth. The read point sits one swing
/// behind and moves one swing either way, so the delay reaches twice the swing: this keeps
/// that under 5 ms, short enough to play through, and is still a pitch error of about 1%.
inline constexpr float kWowDepth = 0.55f;
inline constexpr double kFlutterSwingSeconds = 0.00025;
/// Kept behind the swing so the read never reaches the write point.
inline constexpr double kDelayGuardSamples = 4.0;
inline constexpr double kMaxDelaySeconds = 2.0 * (kWowSwingSeconds + kFlutterSwingSeconds);

inline constexpr double kStageFadeMs = 20.0;

/// Drive 1 pushes the tape this much harder. Past it a 2x oversampled tanh starts to alias
/// audibly on a hot high note.
inline constexpr double kMaxDriveGain = 6.0;
/// Share of the drive gain taken back out afterwards, as an exponent: enough that a guitar
/// played at an ordinary level comes out about as loud as it went in.
inline constexpr double kDriveMakeupExponent = 0.7;

inline constexpr double kToneMinHz = 2500.0;
inline constexpr double kToneMaxHz = 16000.0;
/// Tone at or above this is out of the path: a low-pass at 16 kHz would still shift phase.
inline constexpr double kToneBypassAbove = 0.995;

/// Hiss at 1, as the RMS of the noise added: about -56 dBFS. Under any guitar signal, and low
/// enough that a gate set for single-coil hum stays shut.
inline constexpr double kMaxHissRms = 0.0016;
inline constexpr double kHissLowCutHz = 1200.0;

[[nodiscard]] inline double ToneHz(double tone) noexcept
{
    return kToneMinHz * std::pow(kToneMaxHz / kToneMinHz, std::clamp(tone, 0.0, 1.0));
}

[[nodiscard]] inline std::vector<EffectPresetDefinition> FactoryPresets(const std::vector<ParameterDef>& params)
{
    const factory_presets::Builder b(params, {});

    return {
        // A well-kept reel-to-reel: a little compression and a barely moving pitch.
        b.Defaults("clean-reel", "Clean Reel"),
        b.Make("cassette", "Cassette",
               {{"drive", 0.45}, {"wow", 0.35}, {"flutter", 0.4}, {"hiss", 0.35}, {"tone", 0.45}}),
        b.Make("worn-cassette", "Worn Cassette",
               {{"drive", 0.6}, {"wow", 0.7}, {"flutter", 0.6}, {"hiss", 0.6}, {"dropout", 0.5}, {"tone", 0.3}}),
        // Video tape's linear audio track: slow, narrow and noisy, with a long lazy wow.
        b.Make("vhs", "VHS",
               {{"drive", 0.5}, {"wow", 0.85}, {"flutter", 0.3}, {"hiss", 0.5}, {"dropout", 0.3}, {"tone", 0.2}}),
        b.Make("saturation-only", "Saturation Only",
               {{"drive", 0.6}, {"wow", 0.0}, {"flutter", 0.0}, {"hiss", 0.0}, {"dropout", 0.0}, {"tone", 1.0}}),
    };
}

/// A stage's share of the path, moved linearly toward 0 or 1.
class StageFade
{
  public:
    void Prepare(double sampleRate) noexcept
    {
        mStep = static_cast<float>(1000.0 / (kStageFadeMs * std::max(1.0, sampleRate)));
    }

    void Snap(bool on) noexcept
    {
        mValue = on ? 1.0f : 0.0f;
    }

    [[nodiscard]] float Next(bool on) noexcept
    {
        const float target = on ? 1.0f : 0.0f;
        mValue = (mValue < target) ? std::min(target, mValue + mStep) : std::max(target, mValue - mStep);
        return mValue;
    }

    [[nodiscard]] float Value() const noexcept
    {
        return mValue;
    }

  private:
    float mValue = 0.0f;
    float mStep = 1.0f;
};
} // namespace tape_saturation

class TapeSaturationEffect : public EffectProcessor
{
  public:
    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        const auto lineSamples = static_cast<std::size_t>(
            std::ceil(tape_saturation::kMaxDelaySeconds * sampleRate + 2.0 * tape_saturation::kDelayGuardSamples));

        for (auto& channel : mChannels)
        {
            channel.line.Resize(lineSamples);
            channel.oversampler.Prepare(2);
        }

        mTransport.Prepare(sampleRate);
        mDropout.Prepare(sampleRate);
        mDelayGlide.Prepare(sampleRate);
        mDelayGlide.SetTimeConstantMs(60.0);
        mDelayFade.Prepare(sampleRate);
        mDriveFade.Prepare(sampleRate);
        mToneFade.Prepare(sampleRate);
        mHissHighPass = biquad::HighPass(tape_saturation::kHissLowCutHz, biquad::kButterworthQ, sampleRate);
        mDesignedTone = -1.0f;
        Reset();
    }

    void Reset() override
    {
        for (auto& channel : mChannels)
        {
            channel.line.Clear();
            channel.oversampler.Reset();
            channel.tone.Reset();
        }

        mTransport.Reset();
        mDropout.Reset();
        mHissRng.Seed(0x7a9e5eedu);
        mHissState.Reset();
        mSnapPending = true;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        const float* in[2] = {inputs[0], inputs[1]};
        float* out[2] = {outputs[0], outputs[1]};
        Run(in, out, 2, numSamples);
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        const float* in[2] = {input, nullptr};
        float* out[2] = {output, nullptr};
        Run(in, out, 1, numSamples);
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        if (std::atomic<float>* target = Find(key))
        {
            target->store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        const std::atomic<float>* source = const_cast<TapeSaturationEffect*>(this)->Find(key);
        return source ? static_cast<double>(source->load(std::memory_order_relaxed)) : 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "tape_saturation";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "drive";
    }

  private:
    struct Channel
    {
        delay_line::FractionalDelayLine line;
        halfband::Oversampler oversampler;
        biquad::State tone;
    };

    [[nodiscard]] std::atomic<float>* Find(const std::string& key) noexcept
    {
        if (key == "drive")
        {
            return &mDrive;
        }

        if (key == "wow")
        {
            return &mWow;
        }

        if (key == "flutter")
        {
            return &mFlutter;
        }

        if (key == "hiss")
        {
            return &mHiss;
        }

        if (key == "dropout")
        {
            return &mDropoutAmount;
        }

        if (key == "tone")
        {
            return &mTone;
        }

        if (key == "mix")
        {
            return &mMix;
        }

        return nullptr;
    }

    void Run(const float* const* in, float* const* out, int channels, int numSamples) noexcept
    {
        const float drive = mDrive.load(std::memory_order_relaxed);
        const float wow = mWow.load(std::memory_order_relaxed);
        const float flutter = mFlutter.load(std::memory_order_relaxed);
        const float hiss = mHiss.load(std::memory_order_relaxed);
        const float tone = mTone.load(std::memory_order_relaxed);
        const float mix = mMix.load(std::memory_order_relaxed);

        mTransport.SetWow(wow * tape_saturation::kWowDepth);
        mTransport.SetFlutter(flutter);
        mDropout.SetAmount(mDropoutAmount.load(std::memory_order_relaxed));

        const bool delayOn = !mTransport.Idle();
        const bool driveOn = drive > 0.0f;
        const bool toneOn = tone < static_cast<float>(tape_saturation::kToneBypassAbove);
        const bool dropoutOn = !mDropout.Idle();

        if (toneOn && tone != mDesignedTone)
        {
            mToneFilter = biquad::LowPass(tape_saturation::ToneHz(tone), biquad::kButterworthQ, mSampleRate);
            mDesignedTone = tone;
        }

        const double baseDelay = tape_saturation::kDelayGuardSamples +
                                 mSampleRate * (wow * tape_saturation::kWowSwingSeconds +
                                                flutter * tape_saturation::kFlutterSwingSeconds);
        mDelayGlide.SetTarget(baseDelay);

        if (mSnapPending)
        {
            mDelayGlide.Snap(baseDelay);
            mDelayFade.Snap(delayOn);
            mDriveFade.Snap(driveOn);
            mToneFade.Snap(toneOn);
            mSnapPending = false;
        }

        const double driveGain = 1.0 + drive * (tape_saturation::kMaxDriveGain - 1.0);
        const double makeup = std::pow(driveGain, -tape_saturation::kDriveMakeupExponent);
        // Squared, so the lower half of the knob is the useful half.
        const float hissGain = hiss * hiss * static_cast<float>(tape_saturation::kMaxHissRms / kHissUnitRms);
        bool finite = true;

        for (int i = 0; i < numSamples; ++i)
        {
            float dry[2] = {0.0f, 0.0f};

            for (int c = 0; c < channels; ++c)
            {
                const float sample = in[c] ? in[c][i] : 0.0f;
                dry[c] = IsFinite(sample) ? sample : 0.0f;
            }

            const float delayShare = mDelayFade.Next(delayOn);
            const float driveShare = mDriveFade.Next(driveOn);
            const float toneShare = mToneFade.Next(toneOn);
            const bool delayRunning = delayOn || delayShare > 0.0f;
            const double delay = mDelayGlide.Next() + (delayRunning ? static_cast<double>(mTransport.Next()) : 0.0);
            const float dropoutGain = dropoutOn ? mDropout.Next() : 1.0f;
            float noise = 0.0f;

            if (hissGain > 0.0f)
            {
                noise = hissGain *
                        static_cast<float>(mHissState.Process(mHissHighPass, static_cast<double>(mHissRng.NextBipolar())));
            }

            for (int c = 0; c < channels; ++c)
            {
                Channel& channel = mChannels[static_cast<std::size_t>(c)];
                float wet = dry[c];

                if (delayRunning)
                {
                    const float delayed = channel.line.ReadHermite(delay);
                    wet += delayShare * (delayed - wet);
                }

                channel.line.Write(dry[c]);

                if (driveOn || driveShare > 0.0f)
                {
                    double lanes[halfband::Oversampler::kMaxFactor];
                    channel.oversampler.Up(static_cast<double>(wet), lanes);

                    for (int lane = 0; lane < 2; ++lane)
                    {
                        lanes[lane] = std::tanh(lanes[lane] * driveGain) * makeup;
                    }

                    const auto driven = static_cast<float>(channel.oversampler.Down(lanes));
                    wet += driveShare * (driven - wet);
                }

                if (toneOn || toneShare > 0.0f)
                {
                    const auto filtered = static_cast<float>(channel.tone.Process(mToneFilter, static_cast<double>(wet)));
                    wet += toneShare * (filtered - wet);
                }

                wet = wet * dropoutGain + noise;
                const float result = dry[c] + mix * (wet - dry[c]);
                finite = finite && IsFinite(result);

                if (out[c])
                {
                    out[c][i] = IsFinite(result) ? result : 0.0f;
                }
            }
        }

        // A value too large to be audio can lodge in a filter for good; start those again.
        if (!finite || !StateIsFinite())
        {
            for (auto& channel : mChannels)
            {
                channel.line.Clear();
                channel.oversampler.Reset();
                channel.tone.Reset();
            }

            mHissState.Reset();
        }
    }

    [[nodiscard]] bool StateIsFinite() const noexcept
    {
        for (const auto& channel : mChannels)
        {
            if (!IsFinite(channel.tone.s1) || !IsFinite(channel.tone.s2))
            {
                return false;
            }
        }

        return IsFinite(mHissState.s1) && IsFinite(mHissState.s2);
    }

    /// RMS of the unit hiss (uniform noise through the low cut), measured at 48 kHz.
    static constexpr double kHissUnitRms = 0.56;

    std::atomic<float> mDrive{0.3f};
    std::atomic<float> mWow{0.25f};
    std::atomic<float> mFlutter{0.25f};
    std::atomic<float> mHiss{0.0f};
    std::atomic<float> mDropoutAmount{0.0f};
    std::atomic<float> mTone{0.6f};
    std::atomic<float> mMix{1.0f};

    std::array<Channel, 2> mChannels;
    delay_line::TapeTransport mTransport;
    delay_line::TapeDropout mDropout;
    delay_line::GlideRamp mDelayGlide;
    delay_line::Rng mHissRng;
    tape_saturation::StageFade mDelayFade;
    tape_saturation::StageFade mDriveFade;
    tape_saturation::StageFade mToneFade;
    BiquadCoefficients mToneFilter;
    BiquadCoefficients mHissHighPass;
    biquad::State mHissState;
    float mDesignedTone = -1.0f;
    bool mSnapPending = true;
};

inline void RegisterTapeSaturationEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kTapeSaturation;
    info.aliases = {"tape_saturation"};
    info.displayName = "Tape";
    info.category = "drive";
    info.description = "Cassette and video tape character without an echo: saturation, wow and flutter, "
                       "hiss, dropouts and a worn top end";
    info.requiresResource = false;
    info.parameters = {
        {"drive", "Drive", 0.3, 0.0, 1.0, "amount"},
        {"wow", "Wow", 0.25, 0.0, 1.0, "amount"},
        {"flutter", "Flutter", 0.25, 0.0, 1.0, "amount"},
        {"hiss", "Hiss", 0.0, 0.0, 1.0, "amount"},
        {"dropout", "Dropout", 0.0, 0.0, 1.0, "amount"},
        {"tone", "Tone", 0.6, 0.0, 1.0, "amount"},
        {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
    };
    info.presets = tape_saturation::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<TapeSaturationEffect>(); });
}
} // namespace guitarfx
