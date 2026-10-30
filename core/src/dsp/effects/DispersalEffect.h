#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/DispersalSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>

namespace guitarfx
{
/**
 * Dispersal: a long chain of second-order all-passes, all centred on one frequency, which holds
 * that band of the spectrum back behind the rest.
 *
 *   in -+- AP - AP - ... - AP -+- tap at Stages (crossfaded on a change) - mix - out
 *       |  <---- Stages ---->  |                                          |
 *       +------------------------------------ dry -------------------------+
 *
 * Nothing is boosted or cut: each stage has unity gain and only turns the phase, so the
 * magnitude response is flat and only timing changes. A stage delays its centre by about
 * 2Q / (pi f0) (dispersal::StagePeakDelaySeconds), and the chain by Stages times that: at the
 * defaults, 64 stages at 800 Hz and Focus 1 hold 800 Hz back 51 ms behind the lows and highs.
 * A pick attack holds every frequency at once, so it comes out as a sweep, a "pew"; a steady
 * note only changes phase and sounds nearly dry. Mix below 1 combs the chain against the dry.
 *
 * - The stages are trapezoidal state-variable filters (dispersal::DesignStage), which stay
 *   bounded while their coefficients move. Every stage on a side shares one design, so a
 *   Frequency or Focus change recomputes four numbers, not 256 sets; they glide in the log
 *   domain once every kControlInterval samples and ramp linearly in between.
 * - Stages is whole. A change crossfades over kStageFadeMs from the old count's output to the
 *   new one's, running the chain to the larger count meanwhile. Stages past the count are not
 *   run, and are cleared when they drop out, so they come back silent rather than replaying
 *   what they held.
 * - Stereo moves the right side's centre by up to an octave either way, so the sweep reaches
 *   the two sides at different moments. At 0 with a mono input the chains converge, and once
 *   their states agree the right side is no longer run: the left's output is copied over it.
 * - No latency: the delay between frequencies is the sound, so nothing is reported.
 */
class DispersalEffect : public EffectProcessor
{
  public:
    DispersalEffect()
    {
        UpdateRateConstants();
        Reset();
    }

    /// Output L hears only input L, and R only R: the sides share settings, nothing else.
    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        UpdateRateConstants();
        Reset();
    }

    void Reset() override
    {
        mChains = {};
        mLinked = true;
        mTickRemaining = 0;
        mFadeRemaining = 0;

        // The first block snaps every glide onto its target, so parameters set before any audio
        // (a preset loading) start there rather than sweeping in from the defaults.
        mStarted = false;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        const float* inL = inputs[0] ? inputs[0] : inputs[1];
        const float* inR = inputs[1] ? inputs[1] : inL;

        if (!inL)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                if (outputs[ch])
                {
                    std::fill_n(outputs[ch], numSamples, 0.0f);
                }
            }

            return;
        }

        if (!mStarted)
        {
            Snap();
        }

        for (int offset = 0; offset < numSamples; offset += kChunk)
        {
            const int count = std::min(kChunk, numSamples - offset);
            RenderChunk(inL + offset, inR + offset, outputs[0] ? outputs[0] + offset : nullptr,
                        outputs[1] ? outputs[1] + offset : nullptr, count);
        }
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        const std::size_t index = FindParamSpec(dispersal::kParams, key);

        if (index == dispersal::kParamCount)
        {
            return;
        }

        mValues[index] = NormaliseParamValue(dispersal::kParams[index], value);
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    /// The parameters, and "peakDelayMs": how far the chain holds its centre back behind the
    /// rest of the spectrum at the current settings (left side), from the digital design.
    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (const std::size_t index = FindParamSpec(dispersal::kParams, key); index != dispersal::kParamCount)
        {
            return mValues[index];
        }

        if (key == "peakDelayMs")
        {
            return mValues[dispersal::kStages] *
                   dispersal::StagePeakDelaySeconds(mValues[dispersal::kFrequency], mValues[dispersal::kFocus],
                                                    mSampleRate) *
                   1000.0;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "dispersal";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

  private:
    /// Audio is processed stage by stage over chunks of this many samples, so a stage's state
    /// stays in registers across a chunk and the scratch buffers live on the stack.
    static constexpr int kChunk = 64;

    using Buffer = std::array<float, kChunk>;

    struct Chain
    {
        std::array<float, dispersal::kMaxStages> ic1{};
        std::array<float, dispersal::kMaxStages> ic2{};
    };

    /// One side's stage coefficients for each sample of a chunk.
    struct ChunkCoefficients
    {
        Buffer a1;
        Buffer a2;
        Buffer a3;
        Buffer twoK;
    };

    struct Ramp
    {
        dispersal::StageCoefficients current;
        dispersal::StageCoefficients step;
        dispersal::StageCoefficients target;
    };

    void UpdateRateConstants()
    {
        const double samplesPerMs = mSampleRate / 1000.0;
        mControlGlide = 1.0 - std::exp(-dispersal::kControlInterval / (dispersal::kGlideMs * samplesPerMs));
        mMixGlide = static_cast<float>(1.0 - std::exp(-1.0 / (dispersal::kMixGlideMs * samplesPerMs)));
        mFadeLength = std::max(1, static_cast<int>(std::lround(dispersal::kStageFadeMs * samplesPerMs)));
    }

    [[nodiscard]] int TargetStages() const
    {
        return static_cast<int>(mValues[dispersal::kStages]);
    }

    [[nodiscard]] dispersal::StageCoefficients Design(std::size_t side) const
    {
        double centre = std::exp2(mLogFrequency);

        if (side == 1)
        {
            centre = dispersal::RightCentreHz(centre, mStereo);
        }

        return dispersal::DesignStage(centre, std::exp2(mLogFocus), mSampleRate);
    }

    void Snap()
    {
        mLogFrequency = std::log2(mValues[dispersal::kFrequency]);
        mLogFocus = std::log2(mValues[dispersal::kFocus]);
        mStereo = mValues[dispersal::kStereo];
        mMix = static_cast<float>(mValues[dispersal::kMix]);
        mStages = TargetStages();
        mFadeRemaining = 0;
        mTickRemaining = 0;

        for (std::size_t side = 0; side < 2; ++side)
        {
            mRamps[side].target = Design(side);
            mRamps[side].current = mRamps[side].target;
            mRamps[side].step = {};
        }

        mStarted = true;
    }

    [[nodiscard]] static double Glide(double value, double target, double coefficient, double snap)
    {
        value += (target - value) * coefficient;
        return std::abs(target - value) < snap ? target : value;
    }

    /// Moves the glides one control interval on and sets each side's ramp toward the new design.
    void StartTick()
    {
        mLogFrequency = Glide(mLogFrequency, std::log2(mValues[dispersal::kFrequency]), mControlGlide, 1.0e-6);
        mLogFocus = Glide(mLogFocus, std::log2(mValues[dispersal::kFocus]), mControlGlide, 1.0e-6);
        mStereo = Glide(mStereo, mValues[dispersal::kStereo], mControlGlide, 1.0e-5);
        constexpr float kInvInterval = 1.0f / static_cast<float>(dispersal::kControlInterval);

        for (std::size_t side = 0; side < 2; ++side)
        {
            Ramp& ramp = mRamps[side];
            ramp.target = Design(side);
            ramp.step.a1 = (ramp.target.a1 - ramp.current.a1) * kInvInterval;
            ramp.step.a2 = (ramp.target.a2 - ramp.current.a2) * kInvInterval;
            ramp.step.a3 = (ramp.target.a3 - ramp.current.a3) * kInvInterval;
            ramp.step.twoK = (ramp.target.twoK - ramp.current.twoK) * kInvInterval;
        }

        mTickRemaining = dispersal::kControlInterval;
    }

    /// Each side's coefficients for the next `count` samples, landing exactly on each tick's
    /// design however the steps rounded. Both sides take the same arithmetic, so with Stereo at
    /// 0 they are bit for bit the same.
    void FillCoefficients(std::array<ChunkCoefficients, 2>& coefficients, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            if (mTickRemaining == 0)
            {
                StartTick();
            }

            --mTickRemaining;

            for (std::size_t side = 0; side < 2; ++side)
            {
                Ramp& ramp = mRamps[side];

                if (mTickRemaining == 0)
                {
                    ramp.current = ramp.target;
                }
                else
                {
                    ramp.current.a1 += ramp.step.a1;
                    ramp.current.a2 += ramp.step.a2;
                    ramp.current.a3 += ramp.step.a3;
                    ramp.current.twoK += ramp.step.twoK;
                }

                coefficients[side].a1[static_cast<std::size_t>(i)] = ramp.current.a1;
                coefficients[side].a2[static_cast<std::size_t>(i)] = ramp.current.a2;
                coefficients[side].a3[static_cast<std::size_t>(i)] = ramp.current.a3;
                coefficients[side].twoK[static_cast<std::size_t>(i)] = ramp.current.twoK;
            }
        }
    }

    /// One sample through one stage: the TPT SVF (Zavalishin, Simper), all-pass output.
    [[nodiscard]] static float StageTick(float x, float& ic1, float& ic2, float a1, float a2, float a3,
                                         float twoK) noexcept
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return x - twoK * v1;
    }

    [[nodiscard]] static float Flush(float value) noexcept
    {
        return std::fabs(value) < dispersal::kStateFlush ? 0.0f : value;
    }

    /// Stores a stage's state back, flushed; false if it has gone non-finite.
    [[nodiscard]] static bool Store(Chain& chain, std::size_t stage, float ic1, float ic2) noexcept
    {
        chain.ic1[stage] = Flush(ic1);
        chain.ic2[stage] = Flush(ic2);
        return IsFinite(ic1) && IsFinite(ic2);
    }

    /// Runs `stages` stages of one chain over `wet` in place, copying the signal into `tap` once
    /// it has been through `tapAt` stages (0: no tap). False if the chain went non-finite.
    [[nodiscard]] static bool RunChain(Chain& chain, const ChunkCoefficients& c, Buffer& wet, Buffer& tap, int stages,
                                       int tapAt, int count) noexcept
    {
        bool finite = true;

        for (int s = 0; s < stages; ++s)
        {
            const auto stage = static_cast<std::size_t>(s);
            float ic1 = chain.ic1[stage];
            float ic2 = chain.ic2[stage];

            for (int i = 0; i < count; ++i)
            {
                const auto n = static_cast<std::size_t>(i);
                wet[n] = StageTick(wet[n], ic1, ic2, c.a1[n], c.a2[n], c.a3[n], c.twoK[n]);
            }

            finite = Store(chain, stage, ic1, ic2) && finite;

            if (s + 1 == tapAt)
            {
                tap = wet;
            }
        }

        return finite;
    }

    /// RunChain for both sides at once: the two chains are independent, so stepping them in the
    /// same loop lets one side's arithmetic overlap the other's instead of waiting on it.
    static void RunChainPair(std::array<Chain, 2>& chains, const std::array<ChunkCoefficients, 2>& c,
                             std::array<Buffer, 2>& wet, std::array<Buffer, 2>& tap, int stages, int tapAt, int count,
                             std::array<bool, 2>& finite) noexcept
    {
        Buffer& wetL = wet[0];
        Buffer& wetR = wet[1];
        const ChunkCoefficients& cl = c[0];
        const ChunkCoefficients& cr = c[1];

        for (int s = 0; s < stages; ++s)
        {
            const auto stage = static_cast<std::size_t>(s);
            float l1 = chains[0].ic1[stage];
            float l2 = chains[0].ic2[stage];
            float r1 = chains[1].ic1[stage];
            float r2 = chains[1].ic2[stage];

            for (int i = 0; i < count; ++i)
            {
                const auto n = static_cast<std::size_t>(i);
                wetL[n] = StageTick(wetL[n], l1, l2, cl.a1[n], cl.a2[n], cl.a3[n], cl.twoK[n]);
                wetR[n] = StageTick(wetR[n], r1, r2, cr.a1[n], cr.a2[n], cr.a3[n], cr.twoK[n]);
            }

            finite[0] = Store(chains[0], stage, l1, l2) && finite[0];
            finite[1] = Store(chains[1], stage, r1, r2) && finite[1];

            if (s + 1 == tapAt)
            {
                tap = wet;
            }
        }
    }

    [[nodiscard]] bool ChainsAgree(int stages) const noexcept
    {
        for (int s = 0; s < stages; ++s)
        {
            const auto stage = static_cast<std::size_t>(s);

            if (std::fabs(mChains[0].ic1[stage] - mChains[1].ic1[stage]) > dispersal::kLinkTolerance ||
                std::fabs(mChains[0].ic2[stage] - mChains[1].ic2[stage]) > dispersal::kLinkTolerance)
            {
                return false;
            }
        }

        return true;
    }

    void ClearStages(int from, int to) noexcept
    {
        for (auto& chain : mChains)
        {
            std::fill(chain.ic1.begin() + from, chain.ic1.begin() + to, 0.0f);
            std::fill(chain.ic2.begin() + from, chain.ic2.begin() + to, 0.0f);
        }
    }

    void RenderChunk(const float* inL, const float* inR, float* outL, float* outR, int count)
    {
        const auto n = static_cast<std::size_t>(count);
        std::array<Buffer, 2> dry;
        std::copy_n(inL, n, dry[0].begin());
        std::copy_n(inR, n, dry[1].begin());
        std::array<Buffer, 2> wet = dry;
        std::array<Buffer, 2> tap;

        // The sides can share one chain when they would compute the same thing: the same design
        // (Stereo at 0 and settled there) and the same input.
        const bool sidesMatch = mStereo == 0.0 && mValues[dispersal::kStereo] == 0.0 &&
                                std::equal(dry[0].begin(), dry[0].begin() + count, dry[1].begin());

        if (mLinked && !sidesMatch)
        {
            mChains[1] = mChains[0];
            mLinked = false;
        }

        std::array<ChunkCoefficients, 2> coefficients;
        FillCoefficients(coefficients, count);

        if (mFadeRemaining == 0 && TargetStages() != mStages)
        {
            mFadeFrom = mStages;
            mFadeTo = TargetStages();
            mFadeRemaining = mFadeLength;
        }

        const bool fading = mFadeRemaining > 0;
        const int stages = fading ? std::max(mFadeFrom, mFadeTo) : mStages;
        const int tapAt = fading ? std::min(mFadeFrom, mFadeTo) : 0;
        std::array<bool, 2> finite = {true, true};

        if (mLinked)
        {
            finite[0] = RunChain(mChains[0], coefficients[0], wet[0], tap[0], stages, tapAt, count);
        }
        else
        {
            RunChainPair(mChains, coefficients, wet, tap, stages, tapAt, count, finite);
        }

        const std::size_t sides = mLinked ? 1 : 2;

        if (fading)
        {
            // The longer chain's output is `wet`, the shorter's `tap`.
            const bool growing = mFadeTo > mFadeFrom;
            const auto length = static_cast<float>(mFadeLength);
            int remaining = mFadeRemaining;

            for (std::size_t i = 0; i < n; ++i)
            {
                float weight = 1.0f;

                if (remaining > 0)
                {
                    weight = static_cast<float>(mFadeLength - remaining + 1) / length;
                    --remaining;
                }

                for (std::size_t side = 0; side < sides; ++side)
                {
                    const float from = growing ? tap[side][i] : wet[side][i];
                    const float to = growing ? wet[side][i] : tap[side][i];
                    wet[side][i] = from + weight * (to - from);
                }
            }

            mFadeRemaining = remaining;

            if (mFadeRemaining == 0)
            {
                if (mFadeTo < mFadeFrom)
                {
                    ClearStages(mFadeTo, mFadeFrom);
                }

                mStages = mFadeTo;
            }
        }

        const auto targetMix = static_cast<float>(mValues[dispersal::kMix]);

        for (std::size_t i = 0; i < n; ++i)
        {
            mMix += (targetMix - mMix) * mMixGlide;

            if (std::fabs(targetMix - mMix) < 1.0e-6f)
            {
                mMix = targetMix;
            }

            const float left = dry[0][i] * (1.0f - mMix) + wet[0][i] * mMix;
            const float right = mLinked ? left : dry[1][i] * (1.0f - mMix) + wet[1][i] * mMix;

            if (outL)
            {
                outL[i] = left;
            }

            if (outR)
            {
                outR[i] = right;
            }
        }

        // A NaN or infinity on the input would otherwise ring round the stages for good.
        for (std::size_t side = 0; side < sides; ++side)
        {
            if (!finite[side])
            {
                mChains[side] = {};
            }
        }

        if (!mLinked && sidesMatch && ChainsAgree(stages))
        {
            mLinked = true;
        }
    }

    dispersal::ParamValues mValues = dispersal::kDefaultValues;

    std::array<Chain, 2> mChains{};
    std::array<Ramp, 2> mRamps{};

    // Glided values: Frequency and Focus as log2, Stereo in semitones.
    double mLogFrequency = 0.0;
    double mLogFocus = 0.0;
    double mStereo = 0.0;
    float mMix = 1.0f;

    int mStages = 1;
    int mFadeFrom = 1;
    int mFadeTo = 1;
    int mFadeRemaining = 0;
    int mFadeLength = 1;
    int mTickRemaining = 0;

    double mControlGlide = 1.0;
    float mMixGlide = 1.0f;

    /// The right chain is not being run: its state is the left's, and so is its output.
    bool mLinked = true;
    bool mStarted = false;
};

inline void RegisterDispersalEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kDispersal;
    info.aliases = {"dispersal"};
    info.displayName = "Dispersal";
    info.category = "modulation";
    info.description = "A long chain of all-pass filters that holds one band of frequencies back behind the rest, "
                       "so every pick attack becomes a falling zap. Nothing is boosted or cut; Mix below 1 combs "
                       "it against the dry signal like a deep phaser.";
    info.requiresResource = false;
    info.requiresTempo = false;
    info.presets = dispersal::FactoryPresets();
    info.parameters = BuildParameterDefs(dispersal::kParams);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<DispersalEffect>(); });
}
} // namespace guitarfx
