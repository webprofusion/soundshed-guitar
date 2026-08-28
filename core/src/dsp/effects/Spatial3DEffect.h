#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/TempoSync.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace guitarfx
{
namespace spatial3d
{
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

// Anthropometric averages used by the Woodworth ITD model.
inline constexpr double kHeadRadiusM = 0.0875;
inline constexpr double kSpeedOfSoundMs = 343.0;
// Maximum interaural time difference: lateral angle pi/2 -> (r/c)(pi/2 + 1).
inline constexpr double kMaxItdSec = (kHeadRadiusM / kSpeedOfSoundMs) * (kPi * 0.5 + 1.0);

inline constexpr double kMinDistanceM = 0.2;
inline constexpr double kMaxDistanceM = 10.0;
inline constexpr double kReferenceDistanceM = 1.5; // distance at which gain is unity

// Control-rate sub-block. Filter coefficients and delay targets update at this
// granularity; gains ramp linearly inside it. 64 samples is ~1.5 ms at 44.1 kHz.
inline constexpr int kControlBlock = 64;

// Head-shadow lowpass sweeps between these cutoffs as the source moves off-axis.
inline constexpr double kMaxShadowCutoffHz = 20000.0;
inline constexpr double kMinShadowCutoffHz = 1500.0;

// How the interaural level difference is split between the two ears. A real
// head boosts the near ear far less than it attenuates the far one, so an even
// split would make a source audibly louder at the sides than in front.
inline constexpr double kIpsilateralIldShare = 0.35;
inline constexpr double kContralateralIldShare = 0.65;

// Corner frequency of the shelf carrying the front/back and elevation tilt.
inline constexpr double kShelfFreqHz = 4500.0;

inline constexpr double kRearTapMs = 0.7;
inline constexpr double kMinMotionRateHz = 0.01;
inline constexpr double kMaxMotionRateHz = 4.0;

// Listener mode
inline constexpr int kListenHeadphones = 0;
inline constexpr int kListenSpeakers = 1;

// Delay update strategy
inline constexpr int kDelaySmooth = 0;  // crossfaded delay updates, no pitch shift
inline constexpr int kDelayDoppler = 1; // continuous delay slide, audible pitch shift

// Motion modes
inline constexpr int kMotionOff = 0;
inline constexpr int kMotionOrbit = 1;
inline constexpr int kMotionArc = 2;
inline constexpr int kMotionFigureEight = 3;
inline constexpr int kMotionSpiral = 4;
inline constexpr int kMotionDrift = 5;
inline constexpr int kMotionPendulum = 6;
inline constexpr int kMotionModeCount = 7;

inline std::vector<std::string> MotionModeLabels()
{
    return {"Off", "Orbit", "Arc", "Figure 8", "Spiral", "Drift", "Pendulum"};
}

inline std::vector<std::string> ListenModeLabels()
{
    return {"Headphones", "Speakers"};
}

inline std::vector<std::string> DelayModeLabels()
{
    return {"Smooth", "Doppler"};
}

inline std::vector<std::string> DirectionLabels()
{
    return {"Forward", "Reverse"};
}

// Independent LFO phase ratios. Every trajectory component runs on its own
// wrapped accumulator so no path is discontinuous when a phase wraps.
inline constexpr std::array<double, 7> kPhaseRatios = {1.0, 2.0, 0.25, 0.37, 0.61, 0.29, 0.23};

inline float FlushDenormal(float v)
{
    return (std::fabs(v) < 1.0e-25f) ? 0.0f : v;
}

/** Circular delay line with fractional (linearly interpolated) reads. */
class DelayLine
{
  public:
    void Prepare(std::size_t capacity)
    {
        mBuffer.assign(std::max<std::size_t>(capacity, 4), 0.0f);
        mWrite = 0;
    }

    void Reset()
    {
        std::fill(mBuffer.begin(), mBuffer.end(), 0.0f);
        mWrite = 0;
    }

    [[nodiscard]] std::size_t Size() const
    {
        return mBuffer.size();
    }

    inline void Write(float x)
    {
        mBuffer[mWrite] = x;
    }

    inline void Advance()
    {
        if (++mWrite >= mBuffer.size())
        {
            mWrite = 0;
        }
    }

    /** Reads `delaySamples` behind the most recently written sample. */
    [[nodiscard]] inline float Read(double delaySamples) const
    {
        const double size = static_cast<double>(mBuffer.size());
        const double d = std::clamp(delaySamples, 0.0, size - 2.0);
        double pos = static_cast<double>(mWrite) - d;

        if (pos < 0.0)
        {
            pos += size;
        }

        const auto i0 = static_cast<std::size_t>(pos);
        const double frac = pos - static_cast<double>(i0);
        const std::size_t i1 = (i0 + 1 >= mBuffer.size()) ? 0 : i0 + 1;
        return static_cast<float>(mBuffer[i0] * (1.0 - frac) + mBuffer[i1] * frac);
    }

    /** Integer read, used for fixed taps where interpolation is unnecessary. */
    [[nodiscard]] inline float ReadInt(std::size_t delaySamples) const
    {
        const std::size_t size = mBuffer.size();
        const std::size_t d = std::min(delaySamples, size - 1);
        const std::size_t idx = (mWrite + size - d) % size;
        return mBuffer[idx];
    }

  private:
    std::vector<float> mBuffer;
    std::size_t mWrite = 0;
};

/** One-pole lowpass used for head shadow and air absorption. */
struct OnePoleLowpass
{
    float state = 0.0f;

    void Reset()
    {
        state = 0.0f;
    }

    inline float Process(float x, float coeff)
    {
        state = FlushDenormal(coeff * state + (1.0f - coeff) * x);
        return state;
    }

    /** Converts a cutoff in Hz to the feedback coefficient for the given rate. */
    static float CoeffFor(double cutoffHz, double sampleRate)
    {
        const double fc = std::clamp(cutoffHz, 10.0, sampleRate * 0.49);
        return static_cast<float>(std::exp(-kTwoPi * fc / sampleRate));
    }
};

/** Transposed direct form II biquad (RBJ cookbook coefficients). */
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    float z1 = 0.0f, z2 = 0.0f;

    void Reset()
    {
        z1 = 0.0f;
        z2 = 0.0f;
    }

    void SetIdentity()
    {
        b0 = 1.0;
        b1 = b2 = a1 = a2 = 0.0;
    }

    inline float Process(float x)
    {
        const double y = b0 * static_cast<double>(x) + static_cast<double>(z1);
        z1 = FlushDenormal(static_cast<float>(b1 * x - a1 * y + z2));
        z2 = FlushDenormal(static_cast<float>(b2 * x - a2 * y));
        return static_cast<float>(y);
    }

    void SetPeaking(double sampleRate, double freqHz, double gainDb, double q)
    {
        if (std::fabs(gainDb) < 1.0e-4)
        {
            SetIdentity();
            return;
        }

        const double f0 = std::clamp(freqHz, 20.0, sampleRate * 0.45);
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = kTwoPi * f0 / sampleRate;
        const double cosW = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * std::max(0.05, q));
        const double a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0;
        b1 = (-2.0 * cosW) / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = (-2.0 * cosW) / a0;
        a2 = (1.0 - alpha / A) / a0;
    }

    void SetHighShelf(double sampleRate, double freqHz, double gainDb, double slope)
    {
        if (std::fabs(gainDb) < 1.0e-4)
        {
            SetIdentity();
            return;
        }

        const double f0 = std::clamp(freqHz, 20.0, sampleRate * 0.45);
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = kTwoPi * f0 / sampleRate;
        const double cosW = std::cos(w0);
        const double s = std::clamp(slope, 0.1, 2.0);
        const double alpha = (std::sin(w0) * 0.5) * std::sqrt((A + 1.0 / A) * (1.0 / s - 1.0) + 2.0);
        const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;
        const double a0 = (A + 1.0) - (A - 1.0) * cosW + twoSqrtAAlpha;
        b0 = A * ((A + 1.0) + (A - 1.0) * cosW + twoSqrtAAlpha) / a0;
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW) / a0;
        b2 = A * ((A + 1.0) + (A - 1.0) * cosW - twoSqrtAAlpha) / a0;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosW) / a0;
        a2 = ((A + 1.0) - (A - 1.0) * cosW - twoSqrtAAlpha) / a0;
    }
};

/** Early-reflection tap set, giving distance a direct/reflected ratio cue. */
inline constexpr std::array<double, 4> kErTapMs = {13.7, 19.3, 27.1, 33.9};
inline constexpr std::array<float, 4> kErTapGain = {0.50f, 0.40f, 0.32f, 0.25f};
inline constexpr std::array<float, 4> kErTapPan = {-0.70f, 0.60f, -0.35f, 0.80f};
inline constexpr double kErBufferSec = 0.045;
} // namespace spatial3d

/**
 * Spatial 3D positioner.
 *
 * Places a (mono-summed) source anywhere on a sphere around the listener using
 * binaural cues over a normal stereo pair:
 *   - left/right   : Woodworth ITD + asymmetric ILD + contralateral head-shadow lowpass
 *   - front/behind : high-shelf tilt plus a short rear pinna reflection
 *   - up/down      : pinna notch that sweeps ~6 kHz (below) to ~11 kHz (above)
 *   - near/far     : inverse-distance gain, air absorption, direct/reflected ratio
 *
 * Elevation and front/back are pinna cues, so they need headphones. The
 * "Speakers" listening mode removes the cues that do not survive loudspeaker
 * playback or mono fold-down instead of pretending they still work.
 *
 * A built-in motion engine drives the position from plain numeric parameters so
 * that animated panning can be captured in a factory effect preset.
 *
 * Real-time safety: all buffers are allocated in Prepare(). Process() performs no
 * allocation, locking, or file access.
 */
class Spatial3DEffect : public EffectProcessor
{
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;

        // Half the maximum ITD is applied to both ears so that the per-ear delay
        // never goes negative, plus headroom for the head-shadow group-delay
        // compensation below. It is a constant, reported as latency.
        const float maxShadowCoeff = spatial3d::OnePoleLowpass::CoeffFor(spatial3d::kMinShadowCutoffHz, sampleRate);
        mMaxShadowGroupDelay =
            static_cast<double>(maxShadowCoeff) / std::max(1.0e-6, 1.0 - static_cast<double>(maxShadowCoeff));
        mBiasSamples = static_cast<int>(std::ceil(0.5 * spatial3d::kMaxItdSec * sampleRate + mMaxShadowGroupDelay)) + 1;

        // Capacity covers the bias, the ITD swing and the Doppler propagation delay
        // for the furthest supported distance.
        const double maxPropagationSec = spatial3d::kMaxDistanceM / spatial3d::kSpeedOfSoundMs;
        const std::size_t mainCapacity =
            static_cast<std::size_t>(std::ceil((maxPropagationSec + spatial3d::kMaxItdSec) * sampleRate)) +
            static_cast<std::size_t>(mBiasSamples) + 8;
        mSourceLine.Prepare(mainCapacity);
        mRearLine.Prepare(static_cast<std::size_t>(std::ceil(0.004 * sampleRate)) + 4);
        mErLine.Prepare(static_cast<std::size_t>(std::ceil(spatial3d::kErBufferSec * sampleRate)) + 4);
        mDryLineL.Prepare(static_cast<std::size_t>(mBiasSamples) + 4);
        mDryLineR.Prepare(static_cast<std::size_t>(mBiasSamples) + 4);

        mRearTapSamples = static_cast<std::size_t>(spatial3d::kRearTapMs * 0.001 * sampleRate);

        for (std::size_t i = 0; i < spatial3d::kErTapMs.size(); ++i)
        {
            mErTapSamples[i] = static_cast<std::size_t>(spatial3d::kErTapMs[i] * 0.001 * sampleRate);
        }

        mPrepared = true;
        Reset();
    }

    void Reset() override
    {
        mSourceLine.Reset();
        mRearLine.Reset();
        mErLine.Reset();
        mDryLineL.Reset();
        mDryLineR.Reset();

        mShadowL.Reset();
        mShadowR.Reset();
        mAirFilter.Reset();
        mErFilterL.Reset();
        mErFilterR.Reset();
        mNotchL.Reset();
        mNotchR.Reset();
        mShelfL.Reset();
        mShelfR.Reset();

        // Deterministic start phase, so a preset always begins at the same point.
        const double start = std::fmod(mMotionStartPhaseDeg.load(std::memory_order_relaxed) / 360.0, 1.0);

        for (auto& phase : mPhases)
        {
            phase = start < 0.0 ? start + 1.0 : start;
        }

        mSmoothedAzimuth = mAzimuth.load(std::memory_order_relaxed);
        mSmoothedElevation = mElevation.load(std::memory_order_relaxed);
        mSmoothedDistance = mDistance.load(std::memory_order_relaxed);
        mInitialised = false;

        mDelayL = static_cast<double>(mBiasSamples);
        mDelayR = static_cast<double>(mBiasSamples);
        mGainL = 1.0f;
        mGainR = 1.0f;
        mPublishedItdUs.store(0.0, std::memory_order_relaxed);
        mPublishedIldDb.store(0.0, std::memory_order_relaxed);
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!outputs || numSamples <= 0)
        {
            return;
        }

        if (!mPrepared)
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        const float mix = mMix.load(std::memory_order_relaxed);
        const float trim = static_cast<float>(std::pow(10.0, mOutputTrimDb.load(std::memory_order_relaxed) / 20.0));
        const int listenMode = mListenMode.load(std::memory_order_relaxed);
        const int delayMode = mDelayMode.load(std::memory_order_relaxed);
        const float roomAmount = mRoomAmount.load(std::memory_order_relaxed);
        const bool speakers = (listenMode == spatial3d::kListenSpeakers);

        int offset = 0;

        while (offset < numSamples)
        {
            const int len = std::min(spatial3d::kControlBlock, numSamples - offset);

            AdvanceMotionAndSmoothing(len);
            const Coefficients coeff = ComputeCoefficients(speakers, roomAmount);

            // Delay targets are approached across the sub-block rather than jumped to.
            //
            // Be clear about what this does and does not buy: crossfading between two
            // delay taps interpolates the signal's *phase*, so a continuously moving
            // source is pitch-shifted either way — that is unavoidable, and physically
            // correct. What the crossfade avoids is the audible sweep and aliasing of
            // dragging a read pointer through a large jump (preset load, puck drag).
            // The distinction between the modes is that Doppler additionally tracks the
            // full propagation delay to the source, which bends pitch audibly as the
            // source approaches or recedes; Smooth mode holds propagation constant, so
            // only the head-width ITD travel contributes — well under a cent at any
            // musical motion rate.
            const double prevDelayL = mDelayL;
            const double prevDelayR = mDelayR;
            mDelayL = coeff.delayLeftSamples;
            mDelayR = coeff.delayRightSamples;

            const float prevGainL = mGainL;
            const float prevGainR = mGainR;
            mGainL = coeff.gainLeft;
            mGainR = coeff.gainRight;

            const float invLen = 1.0f / static_cast<float>(len);

            for (int i = 0; i < len; ++i)
            {
                const int n = offset + i;
                const float inL = (inputs && inputs[0]) ? inputs[0][n] : 0.0f;
                const float inR = (inputs && inputs[1]) ? inputs[1][n] : inL;

                // The positioner treats the input as a single point source.
                const float mono = 0.5f * (inL + inR);

                // Air absorption is a property of the path, so it is applied once,
                // before the signal is split into two ears.
                const float airLp = mAirFilter.Process(mono, coeff.airCoeff);
                const float travelled = mono * (1.0f - coeff.airAmount) + airLp * coeff.airAmount;

                // Short rear reflection: a pinna cue that only exists behind the listener.
                mRearLine.Write(travelled);
                const float rear = mRearLine.ReadInt(mRearTapSamples);
                const float shaped = travelled + coeff.rearGain * rear;
                mRearLine.Advance();

                mSourceLine.Write(shaped);

                const float t = static_cast<float>(i + 1) * invLen;
                float earL;
                float earR;

                if (delayMode == spatial3d::kDelayDoppler)
                {
                    const double dl = prevDelayL + (mDelayL - prevDelayL) * static_cast<double>(t);
                    const double dr = prevDelayR + (mDelayR - prevDelayR) * static_cast<double>(t);
                    earL = mSourceLine.Read(dl);
                    earR = mSourceLine.Read(dr);
                }
                else
                {
                    earL = mSourceLine.Read(prevDelayL) * (1.0f - t) + mSourceLine.Read(mDelayL) * t;
                    earR = mSourceLine.Read(prevDelayR) * (1.0f - t) + mSourceLine.Read(mDelayR) * t;
                }

                mSourceLine.Advance();

                // Contralateral head shadow.
                if (coeff.shadowLeft > 0.0f)
                {
                    const float lp = mShadowL.Process(earL, coeff.shadowCoeffLeft);
                    earL = earL * (1.0f - coeff.shadowLeft) + lp * coeff.shadowLeft;
                }
                else
                {
                    mShadowL.Process(earL, coeff.shadowCoeffLeft);
                }

                if (coeff.shadowRight > 0.0f)
                {
                    const float lp = mShadowR.Process(earR, coeff.shadowCoeffRight);
                    earR = earR * (1.0f - coeff.shadowRight) + lp * coeff.shadowRight;
                }
                else
                {
                    mShadowR.Process(earR, coeff.shadowCoeffRight);
                }

                // Pinna spectral cues: elevation notch then front/back + elevation shelf.
                earL = mShelfL.Process(mNotchL.Process(earL));
                earR = mShelfR.Process(mNotchR.Process(earR));

                const float gl = prevGainL + (mGainL - prevGainL) * t;
                const float gr = prevGainR + (mGainR - prevGainR) * t;

                float wetL = earL * gl;
                float wetR = earR * gr;

                // Early reflections are fed from the direct source so that the
                // direct/reflected ratio, not the absolute level, carries distance.
                mErLine.Write(travelled * coeff.erSend);

                if (coeff.erSend > 0.0f)
                {
                    float erL = 0.0f;
                    float erR = 0.0f;

                    for (std::size_t tap = 0; tap < mErTapSamples.size(); ++tap)
                    {
                        const float tapValue = mErLine.ReadInt(mErTapSamples[tap]) * spatial3d::kErTapGain[tap];
                        const float pan = spatial3d::kErTapPan[tap];
                        erL += tapValue * (0.5f - 0.5f * pan);
                        erR += tapValue * (0.5f + 0.5f * pan);
                    }

                    wetL += mErFilterL.Process(erL, coeff.erCoeff);
                    wetR += mErFilterR.Process(erR, coeff.erCoeff);
                }
                else
                {
                    mErFilterL.Process(0.0f, coeff.erCoeff);
                    mErFilterR.Process(0.0f, coeff.erCoeff);
                }

                mErLine.Advance();

                // The dry path is delayed by the reported latency so that blending it
                // with the wet path does not comb.
                mDryLineL.Write(inL);
                mDryLineR.Write(inR);
                const float dryL = mDryLineL.ReadInt(static_cast<std::size_t>(mBiasSamples));
                const float dryR = mDryLineR.ReadInt(static_cast<std::size_t>(mBiasSamples));
                mDryLineL.Advance();
                mDryLineR.Advance();

                if (outputs[0])
                {
                    outputs[0][n] = dryL * (1.0f - mix) + wetL * mix * trim;
                }

                if (outputs[1])
                {
                    outputs[1][n] = dryR * (1.0f - mix) + wetR * mix * trim;
                }
            }

            offset += len;
        }

        PublishPosition();
    }

    void SetParam(const std::string& key, double value) override
    {
        if (key == "azimuth")
        {
            mAzimuth.store(std::clamp(value, -180.0, 180.0), std::memory_order_relaxed);
        }
        else if (key == "elevation")
        {
            mElevation.store(std::clamp(value, -90.0, 90.0), std::memory_order_relaxed);
        }
        else if (key == "distance")
        {
            mDistance.store(std::clamp(value, spatial3d::kMinDistanceM, spatial3d::kMaxDistanceM),
                            std::memory_order_relaxed);
        }
        else if (key == "mix")
        {
            mMix.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "roomAmount")
        {
            mRoomAmount.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "listenMode")
        {
            mListenMode.store(static_cast<int>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "delayMode")
        {
            mDelayMode.store(static_cast<int>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "outputTrim")
        {
            mOutputTrimDb.store(std::clamp(value, -12.0, 12.0), std::memory_order_relaxed);
        }
        else if (key == "motionMode")
        {
            mMotionMode.store(
                static_cast<int>(std::clamp(value, 0.0, static_cast<double>(spatial3d::kMotionModeCount - 1))),
                std::memory_order_relaxed);
        }
        else if (key == "motionRate")
        {
            mMotionRateHz.store(std::clamp(value, spatial3d::kMinMotionRateHz, spatial3d::kMaxMotionRateHz),
                                std::memory_order_relaxed);
        }
        else if (key == "syncMode")
        {
            mSyncMode.store(tempo_sync::ClampSyncMode(value), std::memory_order_relaxed);
        }
        else if (key == "syncDivision")
        {
            mSyncDivision.store(tempo_sync::ClampDivision(value), std::memory_order_relaxed);
        }
        else if (key == "bpm")
        {
            mBpm.store(tempo_sync::ClampBpm(value), std::memory_order_relaxed);
        }
        else if (key == "motionDepth")
        {
            mMotionDepth.store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
        }
        else if (key == "motionElevDepth")
        {
            mMotionElevDepth.store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
        }
        else if (key == "motionDistDepth")
        {
            mMotionDistDepth.store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
        }
        else if (key == "motionPhase")
        {
            mMotionStartPhaseDeg.store(std::clamp(value, 0.0, 360.0), std::memory_order_relaxed);
        }
        else if (key == "motionDirection")
        {
            mMotionDirection.store(static_cast<int>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "motionSmooth")
        {
            mMotionSmooth.store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
        }
        else if (key == "motionSeed")
        {
            mMotionSeed.store(static_cast<int>(std::clamp(value, 0.0, 999.0)), std::memory_order_relaxed);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "azimuth")
        {
            return mAzimuth.load(std::memory_order_relaxed);
        }

        if (key == "elevation")
        {
            return mElevation.load(std::memory_order_relaxed);
        }

        if (key == "distance")
        {
            return mDistance.load(std::memory_order_relaxed);
        }

        if (key == "mix")
        {
            return mMix.load(std::memory_order_relaxed);
        }

        if (key == "roomAmount")
        {
            return mRoomAmount.load(std::memory_order_relaxed);
        }

        if (key == "listenMode")
        {
            return mListenMode.load(std::memory_order_relaxed);
        }

        if (key == "delayMode")
        {
            return mDelayMode.load(std::memory_order_relaxed);
        }

        if (key == "outputTrim")
        {
            return mOutputTrimDb.load(std::memory_order_relaxed);
        }

        if (key == "motionMode")
        {
            return mMotionMode.load(std::memory_order_relaxed);
        }

        if (key == "motionRate")
        {
            return mMotionRateHz.load(std::memory_order_relaxed);
        }

        if (key == "syncMode")
        {
            return mSyncMode.load(std::memory_order_relaxed);
        }

        if (key == "syncDivision")
        {
            return mSyncDivision.load(std::memory_order_relaxed);
        }

        if (key == "bpm")
        {
            return mBpm.load(std::memory_order_relaxed);
        }

        if (key == "effectiveRate")
        {
            return EffectiveMotionRateHz();
        }

        if (key == "motionDepth")
        {
            return mMotionDepth.load(std::memory_order_relaxed);
        }

        if (key == "motionElevDepth")
        {
            return mMotionElevDepth.load(std::memory_order_relaxed);
        }

        if (key == "motionDistDepth")
        {
            return mMotionDistDepth.load(std::memory_order_relaxed);
        }

        if (key == "motionPhase")
        {
            return mMotionStartPhaseDeg.load(std::memory_order_relaxed);
        }

        if (key == "motionDirection")
        {
            return mMotionDirection.load(std::memory_order_relaxed);
        }

        if (key == "motionSmooth")
        {
            return mMotionSmooth.load(std::memory_order_relaxed);
        }

        if (key == "motionSeed")
        {
            return mMotionSeed.load(std::memory_order_relaxed);
        }

        // Read-only feedback used by the UI to keep the on-screen source in sync
        // with what is actually being rendered.
        if (key == "currentAzimuth")
        {
            return mPublishedAzimuth.load(std::memory_order_relaxed);
        }

        if (key == "currentElevation")
        {
            return mPublishedElevation.load(std::memory_order_relaxed);
        }

        if (key == "currentDistance")
        {
            return mPublishedDistance.load(std::memory_order_relaxed);
        }

        if (key == "currentItdUs")
        {
            return mPublishedItdUs.load(std::memory_order_relaxed);
        }

        if (key == "currentIldDb")
        {
            return mPublishedIldDb.load(std::memory_order_relaxed);
        }

        return 0.0;
    }

    [[nodiscard]] bool ProducesStereoOutput() const override
    {
        return true;
    }

    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return false;
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        return mPrepared ? mBiasSamples : 0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "spatial_3d";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

  private:
    struct Coefficients
    {
        double delayLeftSamples = 0.0;
        double delayRightSamples = 0.0;
        float gainLeft = 1.0f;
        float gainRight = 1.0f;
        float shadowLeft = 0.0f;
        float shadowRight = 0.0f;
        float shadowCoeffLeft = 0.0f;
        float shadowCoeffRight = 0.0f;
        float airAmount = 0.0f;
        float airCoeff = 0.0f;
        float rearGain = 0.0f;
        float erSend = 0.0f;
        float erCoeff = 0.0f;
    };

    [[nodiscard]] double EffectiveMotionRateHz() const
    {
        if (mSyncMode.load(std::memory_order_relaxed) != tempo_sync::kSyncModeTempo)
        {
            return mMotionRateHz.load(std::memory_order_relaxed);
        }

        const double bpm = mBpm.load(std::memory_order_relaxed);
        const int division = mSyncDivision.load(std::memory_order_relaxed);
        return std::clamp(tempo_sync::DivisionRateHz(bpm, division), spatial3d::kMinMotionRateHz,
                          spatial3d::kMaxMotionRateHz);
    }

    /** Deterministic hash used to give Drift mode a reproducible trajectory. */
    static double SeedPhase(int seed, int index)
    {
        const double x =
            std::sin(static_cast<double>(seed) * 12.9898 + static_cast<double>(index) * 78.233) * 43758.5453;
        return (x - std::floor(x)) * spatial3d::kTwoPi;
    }

    void AdvanceMotionAndSmoothing(int len)
    {
        const int mode = mMotionMode.load(std::memory_order_relaxed);
        const double dir = (mMotionDirection.load(std::memory_order_relaxed) == 0) ? 1.0 : -1.0;
        const double rate = EffectiveMotionRateHz();
        const double blockSeconds = static_cast<double>(len) / mSampleRate;

        if (mode != spatial3d::kMotionOff)
        {
            for (std::size_t i = 0; i < mPhases.size(); ++i)
            {
                mPhases[i] += rate * spatial3d::kPhaseRatios[i] * blockSeconds;
                mPhases[i] -= std::floor(mPhases[i]);
            }
        }

        const int seed = mMotionSeed.load(std::memory_order_relaxed);

        if (seed != mCachedSeed)
        {
            mCachedSeed = seed;

            for (int i = 0; i < 4; ++i)
            {
                mDriftPhase[static_cast<std::size_t>(i)] = SeedPhase(seed, i);
            }
        }

        const double anchorAz = mAzimuth.load(std::memory_order_relaxed);
        const double anchorEl = mElevation.load(std::memory_order_relaxed);
        const double anchorDist = mDistance.load(std::memory_order_relaxed);

        double azOffset = 0.0;
        double elOffset = 0.0;
        double distMultiplier = 1.0;

        if (mode != spatial3d::kMotionOff)
        {
            const double depth = mMotionDepth.load(std::memory_order_relaxed);
            const double elDepth = mMotionElevDepth.load(std::memory_order_relaxed);
            const double distDepth = mMotionDistDepth.load(std::memory_order_relaxed);
            const auto angle = [this, dir](std::size_t index) { return spatial3d::kTwoPi * mPhases[index] * dir; };

            switch (mode)
            {
            case spatial3d::kMotionOrbit:
                // Orbit always completes a full circle; use Figure 8 or Pendulum for
                // partial sweeps. Depth controls the elevation and distance excursion.
                azOffset = 360.0 * mPhases[0] * dir;
                elOffset = elDepth * 30.0 * std::sin(angle(0));
                distMultiplier = 1.0 + distDepth * 0.40 * std::sin(angle(0));
                break;
            case spatial3d::kMotionArc:
                elOffset = elDepth * 80.0 * std::sin(angle(0));
                distMultiplier = 1.0 + distDepth * 0.30 * std::sin(angle(1));
                break;
            case spatial3d::kMotionFigureEight:
                azOffset = depth * 70.0 * std::sin(angle(0));
                elOffset = elDepth * 40.0 * std::sin(angle(1));
                break;
            case spatial3d::kMotionSpiral:
                azOffset = 360.0 * mPhases[0] * dir;
                elOffset = elDepth * 70.0 * std::sin(angle(2));
                distMultiplier = 1.0 + distDepth * 0.50 * std::sin(angle(2));
                break;
            case spatial3d::kMotionDrift:
                azOffset = depth * 120.0 *
                           (0.6 * std::sin(angle(3) + mDriftPhase[0]) + 0.4 * std::sin(angle(4) + mDriftPhase[1]));
                elOffset = elDepth * 45.0 * std::sin(angle(5) + mDriftPhase[2]);
                distMultiplier = 1.0 + distDepth * 0.40 * std::sin(angle(6) + mDriftPhase[3]);
                break;
            case spatial3d::kMotionPendulum:
                azOffset = depth * 90.0 * (1.0 - std::cos(angle(0)));
                elOffset = elDepth * 20.0 * std::sin(angle(0));
                distMultiplier = 1.0 + distDepth * 0.60 * std::sin(angle(0));
                break;
            default:
                break;
            }
        }

        double targetAz = anchorAz + azOffset;
        const double targetEl = std::clamp(anchorEl + elOffset, -90.0, 90.0);
        const double targetDist =
            std::clamp(anchorDist * distMultiplier, spatial3d::kMinDistanceM, spatial3d::kMaxDistanceM);

        if (!mInitialised)
        {
            mSmoothedAzimuth = targetAz;
            mSmoothedElevation = targetEl;
            mSmoothedDistance = targetDist;
            mInitialised = true;
            return;
        }

        // Azimuth is smoothed on an unwrapped axis: the target is first moved to the
        // equivalent angle nearest the current value so that crossing +/-180 (or an
        // orbit phase wrap) does not sweep the source the long way round.
        targetAz += 360.0 * std::round((mSmoothedAzimuth - targetAz) / 360.0);

        const double timeConstant = 0.010 + mMotionSmooth.load(std::memory_order_relaxed) * 0.190;
        const double alpha = std::exp(-static_cast<double>(len) / (timeConstant * mSampleRate));
        const double follow = 1.0 - alpha;

        mSmoothedAzimuth += (targetAz - mSmoothedAzimuth) * follow;
        mSmoothedElevation += (targetEl - mSmoothedElevation) * follow;
        mSmoothedDistance += (targetDist - mSmoothedDistance) * follow;
    }

    [[nodiscard]] Coefficients ComputeCoefficients(bool speakers, float roomAmount)
    {
        Coefficients c;

        const double azRad = mSmoothedAzimuth * spatial3d::kPi / 180.0;
        const double elRad = std::clamp(mSmoothedElevation, -90.0, 90.0) * spatial3d::kPi / 180.0;
        const double distance = std::clamp(mSmoothedDistance, spatial3d::kMinDistanceM, spatial3d::kMaxDistanceM);

        // Lateral angle on the cone of confusion. Elevation reduces the lateral
        // component, and front/back mirror images share it by construction — they are
        // separated further down by spectral cues, which is how real hearing does it.
        const double sinLateral = std::clamp(std::sin(azRad) * std::cos(elRad), -1.0, 1.0);
        const double lateral = std::asin(sinLateral);

        // Woodworth ITD. Positive means the source is to the right, so the right ear
        // is nearer and its delay is shorter.
        double itdSec = (spatial3d::kHeadRadiusM / spatial3d::kSpeedOfSoundMs) * (lateral + std::sin(lateral));

        if (speakers)
        {
            itdSec = 0.0; // ITD does not survive loudspeaker crosstalk or mono fold-down
        }

        double propagationSamples = 0.0;

        if (mDelayMode.load(std::memory_order_relaxed) == spatial3d::kDelayDoppler)
        {
            propagationSamples = (distance / spatial3d::kSpeedOfSoundMs) * mSampleRate;
        }

        // Head shadow: the ear facing away from the source loses high frequencies.
        const double shadowScale = speakers ? 0.5 : 1.0;
        const double shadowL = std::max(0.0, sinLateral) * shadowScale;
        const double shadowR = std::max(0.0, -sinLateral) * shadowScale;
        c.shadowLeft = static_cast<float>(shadowL);
        c.shadowRight = static_cast<float>(shadowR);
        c.shadowCoeffLeft = spatial3d::OnePoleLowpass::CoeffFor(ShadowCutoff(shadowL), mSampleRate);
        c.shadowCoeffRight = spatial3d::OnePoleLowpass::CoeffFor(ShadowCutoff(shadowR), mSampleRate);

        // The shadow lowpass is not delay-free: it contributes its own group delay to
        // the shadowed ear, which would otherwise inflate the interaural delay beyond
        // the Woodworth value. Subtract it so the ITD stays true to the model no
        // matter how deep the shadow is. mBiasSamples reserves headroom for this.
        const double halfItdSamples = 0.5 * itdSec * mSampleRate;
        c.delayLeftSamples = static_cast<double>(mBiasSamples) + halfItdSamples + propagationSamples -
                             ShadowGroupDelay(c.shadowCoeffLeft, shadowL);
        c.delayRightSamples = static_cast<double>(mBiasSamples) - halfItdSamples + propagationSamples -
                              ShadowGroupDelay(c.shadowCoeffRight, shadowR);

        // The ILD is split unevenly between the ears. A real head boosts the near ear
        // much less than it attenuates the far one, so an even split would make the
        // source audibly swell in loudness every time it passed the sides.
        const double nearFieldBoost = 1.0 + std::max(0.0, 1.0 - distance) * 0.8;
        const double maxIldDb = speakers ? 16.0 : 9.0;
        const double ildDb = maxIldDb * sinLateral * nearFieldBoost;

        const double nearShare = spatial3d::kIpsilateralIldShare;
        const double farShare = spatial3d::kContralateralIldShare;
        const double rightDb = ildDb >= 0.0 ? nearShare * ildDb : farShare * ildDb;
        const double leftDb = ildDb >= 0.0 ? -farShare * ildDb : -nearShare * ildDb;

        const double distanceGain = std::clamp(spatial3d::kReferenceDistanceM / std::max(distance, 0.25), 0.1, 2.0);
        c.gainLeft = static_cast<float>(std::pow(10.0, leftDb / 20.0) * distanceGain);
        c.gainRight = static_cast<float>(std::pow(10.0, rightDb / 20.0) * distanceGain);

        // "Backness" is 0 in front, 1 behind, and exactly 0.5 directly overhead —
        // where front and back are genuinely indistinguishable. Defining it this way
        // keeps trajectories that pass over the head free of discontinuities.
        const double backness = 0.5 - 0.5 * std::cos(azRad) * std::cos(elRad);
        c.rearGain = speakers ? 0.0f : static_cast<float>(0.25 * backness);

        // Pinna notch: sweeps upward as the source rises. This is the dominant
        // elevation cue and it only works over headphones.
        const double elNorm = std::clamp(mSmoothedElevation / 90.0, -1.0, 1.0);
        const double notchFreq =
            std::exp(std::log(6000.0) + (std::log(11000.0) - std::log(6000.0)) * (elNorm + 1.0) * 0.5);
        const double notchDb = speakers ? -3.5 : -9.0;
        mNotchL.SetPeaking(mSampleRate, notchFreq, notchDb, 3.0);
        mNotchR.SetPeaking(mSampleRate, notchFreq, notchDb, 3.0);

        // One shelf carries both the front/back tilt and the "brighter is higher" cue.
        // It is deliberately left uncompensated: head and pinna shadowing is a
        // high-frequency phenomenon, so the low end must pass through untouched. A
        // broadband makeup gain would restore the level but push the front/back
        // difference into the bass, which is both wrong and audibly odd. A source
        // behind therefore is slightly quieter overall — as it is in real life.
        const double shelfScale = speakers ? 0.6 : 1.0;
        const double shelfDb = ((2.0 * (1.0 - backness) - 8.0 * backness) + 3.0 * elNorm) * shelfScale;
        mShelfL.SetHighShelf(mSampleRate, spatial3d::kShelfFreqHz, shelfDb, 0.7);
        mShelfR.SetHighShelf(mSampleRate, spatial3d::kShelfFreqHz, shelfDb, 0.7);

        // Air absorption grows with distance.
        const double airAmount = std::clamp((distance - 1.5) / 8.5, 0.0, 1.0);
        c.airAmount = static_cast<float>(airAmount);
        c.airCoeff = spatial3d::OnePoleLowpass::CoeffFor(20000.0 * std::pow(0.2, airAmount), mSampleRate);

        // Distance is also carried by the direct/reflected ratio, not just by level.
        const double distanceNorm = std::clamp((distance - 0.5) / 5.0, 0.0, 1.0);
        c.erSend = static_cast<float>(roomAmount * (0.25 + 0.75 * distanceNorm));
        c.erCoeff = spatial3d::OnePoleLowpass::CoeffFor(4000.0, mSampleRate);

        mLastItdUs = itdSec * 1.0e6;
        mLastIldDb = ildDb;

        return c;
    }

    static double ShadowCutoff(double shadowAmount)
    {
        // 20 kHz (unshadowed) down to 1.5 kHz (fully shadowed), logarithmically.
        return spatial3d::kMaxShadowCutoffHz * std::pow(spatial3d::kMinShadowCutoffHz / spatial3d::kMaxShadowCutoffHz,
                                                        std::clamp(shadowAmount, 0.0, 1.0));
    }

    /**
     * Group delay contributed by the head-shadow lowpass, in samples. A one-pole
     * y[n] = (1-a)x[n] + a*y[n-1] has a DC group delay of a/(1-a); the filter is
     * crossfaded in by shadowAmount, so its contribution scales with that mix.
     */
    static double ShadowGroupDelay(float coeff, double shadowAmount)
    {
        const double a = std::clamp(static_cast<double>(coeff), 0.0, 0.999999);
        return std::clamp(shadowAmount, 0.0, 1.0) * (a / (1.0 - a));
    }

    void PublishPosition()
    {
        double wrapped = std::fmod(mSmoothedAzimuth + 180.0, 360.0);

        if (wrapped < 0.0)
        {
            wrapped += 360.0;
        }

        mPublishedAzimuth.store(wrapped - 180.0, std::memory_order_relaxed);
        mPublishedElevation.store(std::clamp(mSmoothedElevation, -90.0, 90.0), std::memory_order_relaxed);
        mPublishedDistance.store(std::clamp(mSmoothedDistance, spatial3d::kMinDistanceM, spatial3d::kMaxDistanceM),
                                 std::memory_order_relaxed);
        mPublishedItdUs.store(mLastItdUs, std::memory_order_relaxed);
        mPublishedIldDb.store(mLastIldDb, std::memory_order_relaxed);
    }

    // Position and rendering parameters
    std::atomic<double> mAzimuth{0.0};
    std::atomic<double> mElevation{0.0};
    std::atomic<double> mDistance{spatial3d::kReferenceDistanceM};
    std::atomic<float> mMix{1.0f};
    std::atomic<float> mRoomAmount{0.25f};
    std::atomic<int> mListenMode{spatial3d::kListenHeadphones};
    std::atomic<int> mDelayMode{spatial3d::kDelaySmooth};
    std::atomic<double> mOutputTrimDb{0.0};

    // Motion parameters
    std::atomic<int> mMotionMode{spatial3d::kMotionOff};
    std::atomic<double> mMotionRateHz{0.06};
    std::atomic<int> mSyncMode{tempo_sync::kSyncModeOff};
    std::atomic<int> mSyncDivision{0};
    std::atomic<double> mBpm{tempo_sync::kDefaultBpm};
    std::atomic<double> mMotionDepth{0.6};
    std::atomic<double> mMotionElevDepth{0.3};
    std::atomic<double> mMotionDistDepth{0.2};
    std::atomic<double> mMotionStartPhaseDeg{0.0};
    std::atomic<int> mMotionDirection{0};
    std::atomic<double> mMotionSmooth{0.4};
    std::atomic<int> mMotionSeed{1};

    // Position feedback for the UI
    std::atomic<double> mPublishedAzimuth{0.0};
    std::atomic<double> mPublishedElevation{0.0};
    std::atomic<double> mPublishedDistance{spatial3d::kReferenceDistanceM};
    std::atomic<double> mPublishedItdUs{0.0};
    std::atomic<double> mPublishedIldDb{0.0};

    // Audio-thread state
    spatial3d::DelayLine mSourceLine;
    spatial3d::DelayLine mRearLine;
    spatial3d::DelayLine mErLine;
    spatial3d::DelayLine mDryLineL;
    spatial3d::DelayLine mDryLineR;
    spatial3d::OnePoleLowpass mShadowL;
    spatial3d::OnePoleLowpass mShadowR;
    spatial3d::OnePoleLowpass mAirFilter;
    spatial3d::OnePoleLowpass mErFilterL;
    spatial3d::OnePoleLowpass mErFilterR;
    spatial3d::Biquad mNotchL;
    spatial3d::Biquad mNotchR;
    spatial3d::Biquad mShelfL;
    spatial3d::Biquad mShelfR;

    std::array<double, spatial3d::kPhaseRatios.size()> mPhases{};
    std::array<double, 4> mDriftPhase{};
    int mCachedSeed = -1;

    std::array<std::size_t, spatial3d::kErTapMs.size()> mErTapSamples{};
    std::size_t mRearTapSamples = 0;

    double mSmoothedAzimuth = 0.0;
    double mSmoothedElevation = 0.0;
    double mSmoothedDistance = spatial3d::kReferenceDistanceM;
    bool mInitialised = false;

    double mDelayL = 0.0;
    double mDelayR = 0.0;
    float mGainL = 1.0f;
    float mGainR = 1.0f;
    double mLastItdUs = 0.0;
    double mLastIldDb = 0.0;

    int mBiasSamples = 0;
    double mMaxShadowGroupDelay = 0.0;
    bool mPrepared = false;
};

namespace spatial3d
{
/**
 * Builds a complete factory preset. Effect presets are copied wholesale into a
 * graph node, so every preset must specify every parameter — a partial preset
 * would inherit whatever the node happened to hold before.
 */
inline EffectPresetDefinition MakePreset(const std::string& id, const std::string& displayName, double azimuth,
                                         double elevation, double distance, int motionMode, double motionRate,
                                         double motionDepth, double motionElevDepth, double motionDistDepth,
                                         double motionSmooth, double roomAmount, int listenMode,
                                         double motionPhase = 0.0, int motionDirection = 0,
                                         int syncMode = tempo_sync::kSyncModeOff, int syncDivision = 0,
                                         int motionSeed = 1)
{
    EffectPresetDefinition preset;
    preset.id = id;
    preset.displayName = displayName;
    preset.isFactory = true;
    preset.parameters = {{"azimuth", azimuth},
                         {"elevation", elevation},
                         {"distance", distance},
                         {"mix", 1.0},
                         {"roomAmount", roomAmount},
                         {"listenMode", static_cast<double>(listenMode)},
                         {"delayMode", static_cast<double>(kDelaySmooth)},
                         {"outputTrim", 0.0},
                         {"motionMode", static_cast<double>(motionMode)},
                         {"motionRate", motionRate},
                         {"syncMode", static_cast<double>(syncMode)},
                         {"syncDivision", static_cast<double>(syncDivision)},
                         {"motionDepth", motionDepth},
                         {"motionElevDepth", motionElevDepth},
                         {"motionDistDepth", motionDistDepth},
                         {"motionPhase", motionPhase},
                         {"motionDirection", static_cast<double>(motionDirection)},
                         {"motionSmooth", motionSmooth},
                         {"motionSeed", static_cast<double>(motionSeed)}};
    preset.parameterOrder = {"azimuth",         "elevation",    "distance",     "motionMode",      "motionRate",
                             "syncMode",        "syncDivision", "motionDepth",  "motionElevDepth", "motionDistDepth",
                             "motionDirection", "motionPhase",  "motionSmooth", "motionSeed",      "roomAmount",
                             "listenMode",      "delayMode",    "mix",          "outputTrim"};
    return preset;
}

inline std::vector<EffectPresetDefinition> FactoryPresets()
{
    return {//          id                  display                az     el   dist  mode              rate    depth
            //          elev   dist   smooth room  listen
            MakePreset("static-centre", "Static · Centre", 0.0, 0.0, 1.5, kMotionOff, 0.06, 0.0, 0.0, 0.0, 0.40, 0.20,
                       kListenHeadphones),
            MakePreset("gentle-orbit", "Gentle Orbit", 0.0, 0.0, 1.8, kMotionOrbit, 0.0625, 0.60, 0.25, 0.30, 0.55,
                       0.30, kListenHeadphones),
            MakePreset("slow-carousel", "Slow Carousel", 0.0, 0.0, 2.2, kMotionOrbit, 0.0333, 1.00, 0.10, 0.20, 0.70,
                       0.35, kListenHeadphones),
            MakePreset("wide-sway", "Wide Sway", 0.0, 0.0, 1.6, kMotionFigureEight, 0.14, 0.85, 0.00, 0.00, 0.45, 0.20,
                       kListenHeadphones),
            MakePreset("overhead-arc", "Overhead Arc", 0.0, 0.0, 1.8, kMotionArc, 0.0625, 0.50, 1.00, 0.25, 0.50, 0.30,
                       kListenHeadphones),
            MakePreset("figure-of-eight", "Figure of Eight", 0.0, 0.0, 1.8, kMotionFigureEight, 0.0833, 0.80, 0.70,
                       0.00, 0.45, 0.25, kListenHeadphones),
            MakePreset("fly-by", "Fly-By", 0.0, 0.0, 2.5, kMotionPendulum, 0.04, 1.00, 0.30, 0.70, 0.60, 0.40,
                       kListenHeadphones),
            MakePreset("ambient-drift", "Ambient Drift", 0.0, 0.0, 2.4, kMotionDrift, 0.025, 0.70, 0.50, 0.40, 0.85,
                       0.45, kListenHeadphones),
            MakePreset("rising-spiral", "Rising Spiral", 0.0, -20.0, 2.0, kMotionSpiral, 0.05, 0.60, 0.80, 0.35, 0.60,
                       0.35, kListenHeadphones),
            MakePreset("tempo-orbit-bar", "Tempo Orbit · 1 Bar", 0.0, 0.0, 1.8, kMotionOrbit, 0.5, 0.60, 0.20, 0.20,
                       0.50, 0.30, kListenHeadphones, 0.0, 0, tempo_sync::kSyncModeTempo, 0),
            MakePreset("speaker-safe-sway", "Speaker Safe Sway", 0.0, 0.0, 1.5, kMotionFigureEight, 0.10, 0.75, 0.00,
                       0.10, 0.50, 0.15, kListenSpeakers)};
}
} // namespace spatial3d

inline void RegisterSpatial3DEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kSpatial3D;
    info.aliases = {"spatial_3d"};
    info.displayName = "Spatial 3D";
    info.category = "modulation";
    info.description = "Positions the signal anywhere around the listener — left, right, front, "
                       "behind, above and below — with animated panning presets. Best on headphones.";
    info.requiresResource = false;
    info.requiresTempo = true;
    info.presets = spatial3d::FactoryPresets();
    info.parameters = {
        {"azimuth", "Azimuth", 0.0, -180.0, 180.0, "deg", "Position", false, 0.5, {}},
        {"elevation", "Elevation", 0.0, -90.0, 90.0, "deg", "Position", false, 0.5, {}},
        {"distance",
         "Distance",
         spatial3d::kReferenceDistanceM,
         spatial3d::kMinDistanceM,
         spatial3d::kMaxDistanceM,
         "m",
         "Position",
         false,
         0.05,
         {}},

        {"motionMode", "Motion", 0.0, 0.0, static_cast<double>(spatial3d::kMotionModeCount - 1), "enum", "Motion",
         false, 1.0, spatial3d::MotionModeLabels()},
        {"motionRate",
         "Rate",
         0.06,
         spatial3d::kMinMotionRateHz,
         spatial3d::kMaxMotionRateHz,
         "Hz",
         "Motion",
         false,
         0.01,
         {}},
        {"syncMode", "Sync", 0.0, 0.0, 1.0, "enum", "Motion", false, 1.0, tempo_sync::SyncModeLabels()},
        {"syncDivision", "Division", 0.0, 0.0, 14.0, "enum", "Motion", false, 1.0, tempo_sync::DivisionLabels()},
        {"motionDepth", "Sweep", 0.6, 0.0, 1.0, "amount", "Motion", false, 0.01, {}},
        {"motionElevDepth", "Height Sweep", 0.3, 0.0, 1.0, "amount", "Motion", false, 0.01, {}},
        {"motionDistDepth", "Distance Sweep", 0.2, 0.0, 1.0, "amount", "Motion", false, 0.01, {}},
        {"motionDirection", "Direction", 0.0, 0.0, 1.0, "enum", "Motion", true, 1.0, spatial3d::DirectionLabels()},
        {"motionPhase", "Start Phase", 0.0, 0.0, 360.0, "deg", "Motion", true, 1.0, {}},
        {"motionSmooth", "Glide", 0.4, 0.0, 1.0, "amount", "Motion", true, 0.01, {}},
        {"motionSeed", "Drift Seed", 1.0, 0.0, 999.0, "amount", "Motion", true, 1.0, {}},

        {"roomAmount", "Room", 0.25, 0.0, 1.0, "amount", "Space", false, 0.01, {}},
        {"listenMode", "Listening", 0.0, 0.0, 1.0, "enum", "Space", false, 1.0, spatial3d::ListenModeLabels()},
        {"delayMode", "Movement", 0.0, 0.0, 1.0, "enum", "Space", true, 1.0, spatial3d::DelayModeLabels()},
        {"mix", "Mix", 1.0, 0.0, 1.0, "amount", "Output", false, 0.01, {}},
        {"outputTrim", "Trim", 0.0, -12.0, 12.0, "dB", "Output", true, 0.1, {}}};

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<Spatial3DEffect>(); });
}
} // namespace guitarfx
