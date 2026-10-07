#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/PitchTracker.h"
#include "dsp/effects/RingModSupport.h"
#include "dsp/effects/TempoSync.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace guitarfx
{
/**
 * Ring modulator: the signal multiplied by a carrier oscillator.
 *
 * Multiplying by a sine at Fc replaces every frequency F in the input with the pair F - Fc and
 * F + Fc. The input itself is gone, and because the new partials sit a fixed distance from the
 * old ones rather than at a fixed ratio, a note or chord comes out as a clangorous, bell-like
 * or robotic cluster. Mix below 1 puts the dry signal back, which is amplitude modulation.
 *
 *   in -+- DC block - x carrier - 20 Hz high-pass - tone low-pass - x Level -+
 *       |                  ^                                               |
 *       +- pitch tracker --+ (Tracking mode)                               |
 *       +------------------------------------------------------------------+- mix - out
 *
 * - Frequency sets the carrier. An LFO can sweep it by up to three octaves either way, as the
 *   classic analogue ring modulators do; the sweep is in octaves, so it sounds even around any
 *   setting, and its rate can follow the host tempo.
 * - Tracking mode runs the carrier at the pitch being played (PitchTracker), moved by Interval
 *   and Fine, so the sidebands keep the same relation to every note: at Interval 0 they are the
 *   note's own harmonics. The carrier glides to each new pitch over Glide, holds the last note
 *   through silence and chords, and sits at Frequency until a first note is found. The LFO
 *   still sweeps around it.
 * - The triangle and square carriers are band-limited (polynomial BLAMP and BLEP).
 * - Each carrier is scaled to unity RMS, so the effect is level-matched with its bypass and
 *   between waveforms.
 * - Stereo Spread runs the right channel's LFO up to half a cycle ahead and its carrier up to a
 *   quarter of a cycle ahead. When it returns to zero, the right carrier is phase-locked back
 *   onto the left and the effect can run mono again.
 */
class RingModEffect : public EffectProcessor
{
  public:
    RingModEffect()
    {
        mToneLog = std::log2(ring_mod::ToneCutoffHz(mValues[ring_mod::kTone]));
        mTracker.Prepare(mSampleRate);
        UpdateRateCoefficients();
        Reset();
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mTracker.Prepare(sampleRate);
        UpdateRateCoefficients();
        Reset();
    }

    void Reset() override
    {
        mTracker.Reset();
        mChannels = {};
        mCarrierPhase = {};
        mCarrierDt = {};
        mDtTarget = {};
        mDtStep = {};
        mLfo = {};
        mNudge = 0.0;
        mLfoPhase = 0.0;
        mLfoCycle = 0;
        mFadeRemaining = 0;

        // The first block snaps every smoothed value onto its target, so parameters set before
        // any audio (a preset loading) start there rather than gliding in from the defaults.
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

        Render(inL, inR, outputs[0], outputs[1], numSamples);
    }

    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        if (key == "bpm")
        {
            mBpm = tempo_sync::ClampBpm(value);
            return;
        }

        const std::size_t index = FindParamSpec(ring_mod::kParams, key);

        if (index == ring_mod::kParamCount)
        {
            return;
        }

        mValues[index] = NormaliseParamValue(ring_mod::kParams[index], value);
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (const std::size_t index = FindParamSpec(ring_mod::kParams, key); index != ring_mod::kParamCount)
        {
            return mValues[index];
        }

        if (key == "bpm")
        {
            return mBpm;
        }

        // Read-only state, for tests and any future display.
        if (key == "effectiveRate")
        {
            return EffectiveLfoRateHz();
        }

        if (key == "carrierFrequency")
        {
            return mCarrierHz;
        }

        if (key == "trackedFrequency")
        {
            return mTracker.FrequencyHz();
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "ring_mod";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

  private:
    struct ChannelState
    {
        float dc = 0.0f;  ///< DC blocker's one-pole low-pass state
        float hp1 = 0.0f; ///< wet high-pass integrator states
        float hp2 = 0.0f;
        float ic1 = 0.0f; ///< tone filter's integrator states
        float ic2 = 0.0f;

        [[nodiscard]] bool IsFinite() const
        {
            return guitarfx::IsFinite(dc) && guitarfx::IsFinite(hp1) && guitarfx::IsFinite(hp2) &&
                   guitarfx::IsFinite(ic1) && guitarfx::IsFinite(ic2);
        }
    };

    [[nodiscard]] static float Approach(float current, float target, float coefficient)
    {
        const float next = current + coefficient * (target - current);
        return (std::abs(target - next) < 1.0e-6f) ? target : next;
    }

    [[nodiscard]] static double Glide(double current, double target, double coefficient, double snap)
    {
        const double next = current + coefficient * (target - current);
        return (std::abs(target - next) < snap) ? target : next;
    }

    /// The one-pole coefficient that covers `samples` samples of a `milliseconds` time constant.
    [[nodiscard]] double SmoothingCoefficient(double milliseconds, int samples) const
    {
        return 1.0 - std::exp(-static_cast<double>(samples) / (milliseconds * 0.001 * mSampleRate));
    }

    /// `cached` is the coefficient for a full control interval; a short last one is worked out.
    [[nodiscard]] double TickCoefficient(double cached, double milliseconds, int samples) const
    {
        return samples == ring_mod::kControlInterval ? cached : SmoothingCoefficient(milliseconds, samples);
    }

    void UpdateRateCoefficients()
    {
        using namespace ring_mod;

        mPitchGlide = SmoothingCoefficient(kFrequencyGlideMs, kControlInterval);
        mControlGlide = SmoothingCoefficient(kControlSmoothingMs, kControlInterval);
        mLfoSlew = SmoothingCoefficient(kLfoSlewMs, kControlInterval);
        mLockGlide = SmoothingCoefficient(kPhaseLockMs, kControlInterval);
        mSampleSmoothing = static_cast<float>(SmoothingCoefficient(kControlSmoothingMs, 1));
        mFadeSamples = std::max(1, static_cast<int>(kWaveformFadeMs * 0.001 * mSampleRate));
        mMaxCarrierHz = std::min(kMaxCarrierHz, kMaxCarrierFraction * mSampleRate);

        const double g = std::tan(kPi * kDcBlockHz / mSampleRate);
        mDcG = static_cast<float>(g / (1.0 + g));

        const auto highG = static_cast<float>(std::tan(kPi * kWetHighPassHz / mSampleRate));
        mHighA1 = 1.0f / (1.0f + highG * (highG + kButterworthDamping));
        mHighA2 = highG * mHighA1;
        mHighA3 = highG * mHighA2;
        UpdateToneCoefficients();
    }

    void UpdateToneCoefficients()
    {
        const double cutoff = std::min(std::exp2(mToneLog), ring_mod::kMaxToneFraction * mSampleRate);
        const auto g = static_cast<float>(std::tan(ring_mod::kPi * cutoff / mSampleRate));
        mToneA1 = 1.0f / (1.0f + g * (g + ring_mod::kButterworthDamping));
        mToneA2 = g * mToneA1;
        mToneA3 = g * mToneA2;
    }

    [[nodiscard]] double EffectiveLfoRateHz() const
    {
        using namespace ring_mod;

        if (static_cast<int>(mValues[kSyncMode]) != tempo_sync::kSyncModeTempo)
        {
            return mValues[kLfoRate];
        }

        const double rate = tempo_sync::DivisionRateHz(mBpm, static_cast<int>(mValues[kSyncDivision]));
        return std::clamp(rate, kParams[kLfoRate].minValue, kParams[kLfoRate].maxValue);
    }

    /// Both channels run one carrier: Spread is, and is heading, nowhere else than zero, and the
    /// right carrier's phase has locked onto the left's.
    [[nodiscard]] bool IsLocked() const
    {
        return mValues[ring_mod::kSpread] == 0.0 && mSpread == 0.0 && mCarrierPhase[0] == mCarrierPhase[1];
    }

    /// Everything the parameters ask for, read once per block. SetParam lands between blocks,
    /// so nothing finer is lost.
    void ComputeTargets()
    {
        using namespace ring_mod;

        mTargetPitch = std::log2(mValues[kFrequency]);
        mTargetDepth = mValues[kLfoDepth];
        mTargetSpread = mValues[kSpread];
        mTargetToneLog = std::log2(ToneCutoffHz(mValues[kTone]));
        mTargetLevel = static_cast<float>(std::pow(10.0, mValues[kLevel] / 20.0));
        mTargetMix = static_cast<float>(mValues[kMix]);
        mLfoShape = static_cast<LfoShape>(static_cast<int>(mValues[kLfoShape]));
        mLfoIncrement = EffectiveLfoRateHz() / mSampleRate;

        // The tracker only listens in Tracking mode. Entering it starts from a clean history, so
        // a note from before the switch cannot be picked up.
        const bool tracking = static_cast<CarrierMode>(static_cast<int>(mValues[kMode])) == CarrierMode::Tracking;

        if (tracking && !mTracking)
        {
            mTracker.Reset();
        }

        mTracking = tracking;
        mTrackOffset = (mValues[kInterval] + mValues[kFine] / 100.0) / 12.0;
        mTrackGlideMs = mValues[kGlide];
        mTrackGlide = mTrackGlideMs > 0.0 ? SmoothingCoefficient(mTrackGlideMs, kControlInterval) : 1.0;

        if (const auto waveform = static_cast<Waveform>(static_cast<int>(mValues[kWaveform])); waveform != mWaveform)
        {
            mFadeFrom = mWaveform;
            mWaveform = waveform;
            mFadeRemaining = mFadeSamples;
        }
    }

    void SnapToTargets()
    {
        mPitch = mTargetPitch;
        mDepth = mTargetDepth;
        mSpread = mTargetSpread;
        mToneLog = mTargetToneLog;
        UpdateToneCoefficients();
        mLevel = mTargetLevel;
        mMix = mTargetMix;
        mFadeRemaining = 0;

        mCarrierPhase[1] = ring_mod::WrapPhase(mCarrierPhase[0] + ring_mod::kSpreadCarrierOffset * mSpread);

        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            mLfo[ch] = LfoAt(ch);
            mCarrierDt[ch] = CarrierHz(ch) / mSampleRate;
            mDtTarget[ch] = mCarrierDt[ch];
            mDtStep[ch] = 0.0;
        }

        mCarrierHz = CarrierHz(0);
    }

    /// The LFO shape's value for channel `ch`, before the slew. The right channel's phase leads
    /// by the Spread offset, and a lead past the cycle's end is the next cycle.
    [[nodiscard]] double LfoAt(std::size_t ch) const
    {
        double phase = mLfoPhase;
        std::uint32_t cycle = mLfoCycle;

        if (ch == 1)
        {
            phase += ring_mod::kSpreadLfoOffset * mSpread;

            if (phase >= 1.0)
            {
                phase -= 1.0;
                ++cycle;
            }
        }

        return ring_mod::LfoValue(mLfoShape, phase, cycle);
    }

    [[nodiscard]] double CarrierHz(std::size_t ch) const
    {
        return std::clamp(std::exp2(mPitch + mDepth * mLfo[ch]), ring_mod::kMinCarrierHz, mMaxCarrierHz);
    }

    void StartCarrierRamp(std::size_t ch, int samples)
    {
        const double hz = CarrierHz(ch);
        mDtTarget[ch] = hz / mSampleRate;
        mDtStep[ch] = (mDtTarget[ch] - mCarrierDt[ch]) / samples;

        if (ch == 0)
        {
            mCarrierHz = hz;
        }
    }

    /// The control-rate work for the next `samples` samples: parameter glides, the LFO, the
    /// carriers' phase-increment ramps, and on the right channel Spread's offset and the lock.
    void UpdateControl(int samples, bool independentRight)
    {
        using namespace ring_mod;

        const double controlGlide = TickCoefficient(mControlGlide, kControlSmoothingMs, samples);
        const double previousSpread = mSpread;

        // Tracking follows the latest accepted pitch, read every control tick so the carrier keeps
        // up however large the host's blocks are; until a first note, it stays on Frequency.
        double targetPitch = mTargetPitch;
        double pitchGlide = TickCoefficient(mPitchGlide, kFrequencyGlideMs, samples);

        if (mTracking)
        {
            pitchGlide = mTrackGlideMs > 0.0 ? TickCoefficient(mTrackGlide, mTrackGlideMs, samples) : 1.0;

            if (const double tracked = mTracker.FrequencyHz(); tracked > 0.0)
            {
                targetPitch = std::log2(tracked) + mTrackOffset;
            }
        }

        mPitch = Glide(mPitch, targetPitch, pitchGlide, 1.0e-6);
        mDepth = Glide(mDepth, mTargetDepth, controlGlide, 1.0e-6);
        mSpread = Glide(mSpread, mTargetSpread, controlGlide, 1.0e-6);

        if (mToneLog != mTargetToneLog)
        {
            mToneLog = Glide(mToneLog, mTargetToneLog, controlGlide, 1.0e-4);
            UpdateToneCoefficients();
        }

        mLfoPhase += mLfoIncrement * samples;

        while (mLfoPhase >= 1.0)
        {
            mLfoPhase -= 1.0;
            ++mLfoCycle;
        }

        const double lfoSlew = TickCoefficient(mLfoSlew, kLfoSlewMs, samples);
        mLfo[0] += lfoSlew * (LfoAt(0) - mLfo[0]);
        StartCarrierRamp(0, samples);
        mNudge = 0.0;

        if (!independentRight)
        {
            mLfo[1] = mLfo[0];
            return;
        }

        mLfo[1] += lfoSlew * (LfoAt(1) - mLfo[1]);
        StartCarrierRamp(1, samples);

        // Spread's fixed carrier offset: a change moves the right phase along by the difference,
        // a little each sample, rather than jumping it.
        double shift = kSpreadCarrierOffset * (mSpread - previousSpread);

        if (mTargetSpread == 0.0 && mSpread == 0.0)
        {
            double error = mCarrierPhase[0] - mCarrierPhase[1];
            error -= std::floor(error + 0.5); // the short way round, in [-0.5, 0.5)

            if (std::abs(error) < kPhaseLockSnap && std::abs(mLfo[1] - mLfo[0]) < kPhaseLockSnap)
            {
                // Locked: from here on the right channel shadows the left exactly.
                mCarrierPhase[1] = mCarrierPhase[0];
                mCarrierDt[1] = mCarrierDt[0];
                mDtTarget[1] = mDtTarget[0];
                mDtStep[1] = mDtStep[0];
                mLfo[1] = mLfo[0];
                return;
            }

            shift += error * TickCoefficient(mLockGlide, kPhaseLockMs, samples);
        }

        mNudge = shift / samples;
    }

    [[nodiscard]] float Carrier(double phase, double dt) const
    {
        const double width = std::max(std::abs(dt), 1.0e-12);
        const float value = ring_mod::CarrierSample(mWaveform, phase, width);

        if (mFadeRemaining <= 0)
        {
            return value;
        }

        const float from = ring_mod::CarrierSample(mFadeFrom, phase, width);
        const float fraction = static_cast<float>(mFadeRemaining) / static_cast<float>(mFadeSamples);
        return value + fraction * (from - value);
    }

    [[nodiscard]] float ProcessSample(ChannelState& state, float input, float carrier) const
    {
        // DC blocker: the input less a 10 Hz one-pole low-pass (trapezoidal).
        const float v = (input - state.dc) * mDcG;
        const float low = v + state.dc;
        state.dc = ring_mod::FlushDenormal(low + v);
        const float ring = (input - low) * carrier;

        // The wet high-pass: a Butterworth trapezoidal state-variable filter's high output.
        const float h3 = ring - state.hp2;
        const float h1 = mHighA1 * state.hp1 + mHighA2 * h3;
        const float h2 = state.hp2 + mHighA2 * state.hp1 + mHighA3 * h3;
        state.hp1 = ring_mod::FlushDenormal(2.0f * h1 - state.hp1);
        state.hp2 = ring_mod::FlushDenormal(2.0f * h2 - state.hp2);
        const float high = ring - ring_mod::kButterworthDamping * h1 - h2;

        // Tone: a Butterworth low-pass, the same filter's low output.
        const float v3 = high - state.ic2;
        const float v1 = mToneA1 * state.ic1 + mToneA2 * v3;
        const float v2 = state.ic2 + mToneA2 * state.ic1 + mToneA3 * v3;
        state.ic1 = ring_mod::FlushDenormal(2.0f * v1 - state.ic1);
        state.ic2 = ring_mod::FlushDenormal(2.0f * v2 - state.ic2);

        return input + mMix * (v2 * mLevel - input);
    }

    /// `inR` null runs the left channel alone.
    void Render(const float* inL, const float* inR, float* outL, float* outR, int numSamples)
    {
        ComputeTargets();

        if (!mStarted)
        {
            SnapToTargets();
            mStarted = true;
        }

        const bool stereo = inR != nullptr;
        const bool independentRight = stereo && !IsLocked();

        for (int start = 0; start < numSamples; start += ring_mod::kControlInterval)
        {
            const int end = std::min(numSamples, start + ring_mod::kControlInterval);
            UpdateControl(end - start, independentRight);

            for (int i = start; i < end; ++i)
            {
                mMix = Approach(mMix, mTargetMix, mSampleSmoothing);
                mLevel = Approach(mLevel, mTargetLevel, mSampleSmoothing);

                mCarrierDt[0] += mDtStep[0];
                const float carrierL = Carrier(mCarrierPhase[0], mCarrierDt[0]);
                mCarrierPhase[0] = ring_mod::WrapPhase(mCarrierPhase[0] + mCarrierDt[0]);
                float carrierR = carrierL;

                if (independentRight)
                {
                    mCarrierDt[1] += mDtStep[1];
                    const double dtR = mCarrierDt[1] + mNudge;
                    carrierR = Carrier(mCarrierPhase[1], dtR);
                    mCarrierPhase[1] = ring_mod::WrapPhase(mCarrierPhase[1] + dtR);
                }

                if (mFadeRemaining > 0)
                {
                    --mFadeRemaining;
                }

                const float left = ProcessSample(mChannels[0], inL[i], carrierL);

                if (outL)
                {
                    outL[i] = left;
                }

                if (stereo)
                {
                    const float right = ProcessSample(mChannels[1], inR[i], carrierR);

                    if (outR)
                    {
                        outR[i] = right;
                    }
                }

                if (mTracking)
                {
                    mTracker.Push(stereo ? 0.5f * (inL[i] + inR[i]) : inL[i]);
                }
            }

            // Land exactly on the ramps' targets, however the steps rounded.
            mCarrierDt[0] = mDtTarget[0];
            mCarrierDt[1] = independentRight ? mDtTarget[1] : mCarrierDt[0];
        }

        if (!independentRight)
        {
            mCarrierPhase[1] = mCarrierPhase[0];
        }

        // A non-finite input poisons the filters for good. Clear them so the effect recovers
        // on the next block instead of emitting NaN until the preset is reloaded.
        for (auto& channel : mChannels)
        {
            if (!channel.IsFinite())
            {
                channel = {};
            }
        }
    }

    ring_mod::ParamValues mValues = ring_mod::kDefaultValues;
    double mBpm = tempo_sync::kDefaultBpm;

    std::array<ChannelState, 2> mChannels{};
    std::array<double, 2> mCarrierPhase{}; ///< in cycles, [0, 1)
    std::array<double, 2> mCarrierDt{};    ///< cycles per sample
    std::array<double, 2> mDtTarget{};
    std::array<double, 2> mDtStep{};
    std::array<double, 2> mLfo{}; ///< slewed, in [-1, 1]
    /// Extra phase per sample on the right carrier: Spread's offset changing, and the lock.
    double mNudge = 0.0;
    double mLfoPhase = 0.0;
    std::uint32_t mLfoCycle = 0;
    double mLfoIncrement = 0.0;
    ring_mod::LfoShape mLfoShape = ring_mod::LfoShape::Sine;
    double mCarrierHz = 440.0;

    ring_mod::Waveform mWaveform = ring_mod::Waveform::Sine;
    ring_mod::Waveform mFadeFrom = ring_mod::Waveform::Sine;
    int mFadeRemaining = 0;
    int mFadeSamples = 1;

    double mTargetPitch = 0.0; ///< log2 of Frequency; Tracking replaces it once a note is found
    double mPitch = 0.0;       ///< log2 of the carrier frequency, before the LFO
    double mTargetDepth = 0.0;
    double mDepth = 0.0;
    double mTargetSpread = 0.0;
    double mSpread = 0.0;
    double mTargetToneLog = 0.0; ///< log2 of the tone cutoff
    double mToneLog = 0.0;
    float mTargetLevel = 1.0f;
    float mLevel = 1.0f;
    float mTargetMix = 1.0f;
    float mMix = 1.0f;
    bool mStarted = false;

    double mPitchGlide = 1.0;
    double mControlGlide = 1.0;
    double mLfoSlew = 1.0;
    double mLockGlide = 1.0;
    float mSampleSmoothing = 1.0f;
    double mMaxCarrierHz = ring_mod::kMaxCarrierHz;
    float mDcG = 0.0f;
    float mHighA1 = 1.0f;
    float mHighA2 = 0.0f;
    float mHighA3 = 0.0f;
    float mToneA1 = 1.0f;
    float mToneA2 = 0.0f;
    float mToneA3 = 0.0f;

    PitchTracker mTracker;
    bool mTracking = false;
    double mTrackOffset = 0.0; ///< Interval and Fine, in octaves
    double mTrackGlideMs = 0.0;
    double mTrackGlide = 1.0;
};

inline void RegisterRingModEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kRingMod;
    info.aliases = {"ring_mod"};
    info.displayName = "Ring Modulator";
    info.category = "modulation";
    info.description = "Multiplies the signal by a carrier oscillator for metallic, bell-like and robotic tones. "
                       "Tracking mode follows the notes you play; an LFO can sweep the carrier; Mix below 1 gives "
                       "amplitude modulation.";
    info.requiresResource = false;
    info.requiresTempo = true;
    info.presets = ring_mod::FactoryPresets();
    info.parameters = BuildParameterDefs(ring_mod::kParams);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<RingModEffect>(); });
}
} // namespace guitarfx
