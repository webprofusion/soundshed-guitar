#pragma once

/**
 * VerbalEngine.h — one Verbal machine, running: the pre-delay, early reflections, diffusion and
 * the shared tank, with each machine's own stage put in front of or behind it. Verbal keeps two
 * engines so a machine change can let the old one ring out while the new one starts.
 *
 * Wet only: the effect owns the dry path, the mix law, Hold's timing and the Swell's envelope.
 */

#include "dsp/BiquadDesign.h"
#include "dsp/effects/GrainPitchShifter.h"
#include "dsp/effects/TapeTransport.h"
#include "dsp/reverb/FdnTank.h"
#include "dsp/reverb/VerbalParams.h"
#include "dsp/reverb/VerbalSpring.h"
#include "dsp/reverb/VerbalStages.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <numeric>

namespace guitarfx::verbal
{
/// Every control the engine reads, in natural units.
struct Settings
{
    Machine machine = Machine::Hall;
    double decay = 3.0, preDelayMs = 20.0, tone = 0.6, mod = 0.4, lowEnd = 0.5;
    int roomSize = 0, hallSize = 0, plateSize = 1, dwell = 1, springs = 1;
    double midEq = 0.5, bloomLength = 0.3, bloomFeedback = 0.1, diffusion = 0.6;
    int vowel = 0, resonance = 1, shimmerMode = 1, shift1 = 25, shift2 = 0;
    double shimmerAmount = 0.6, echoTimeMs = 400.0, echoFeedback = 0.5;
    int heads = 0, spacing = 0, shape = 1, roomShape = 2;
    double shapeTimeMs = 800.0, shapeFeedback = 0.0, lateLevel = 0.45, modSpeed = 0.5;
    double size = 1.0, listenerX = 0.33, listenerY = 0.0;
};

/// The fixed voicing of a machine's tank and early field.
struct Voicing
{
    double lineMinMs, lineMaxMs; ///< the tank's 16 lengths span this
    double gapMs;                ///< built-in delay before anything arrives, on top of Pre-Delay
    std::size_t erCount;
    double erEndMs, erDecayDb;
    float erLevel, erToTank;
    std::size_t diffStages;
    double diffMinMs, diffMaxMs;
    float diffGain;
    double modMs, modHz;
    double highCrossoverHz;
    float makeupDb;
    double decayScale;           ///< measured RT60 over Decay; the Cloud runs long by design
    double toneMinHz, toneMaxHz; ///< Tone sweeps the output low-pass between these
    double highRatioMin;         ///< the high band's decay ratio at Tone 0 (1 at Tone 1)
};

// Indexed by Machine, with the alternate Size of Room, Hall and Plate at the end. Columns: line
// min/max ms, gap ms, ER count, ER end ms, ER decay dB, ER level, ER to tank, diffuser stages,
// diffuser min/max ms, diffuser gain, mod ms, mod Hz, high crossover Hz, makeup dB, decay scale,
// tone low-pass min/max Hz, high-band ratio at Tone 0.
// clang-format off
inline constexpr std::array<Voicing, kMachineCount + 3> kVoicings = {{
    { 7.0,  29.0, 18.0, 18,  40.0, 10.0, 0.55f, 0.5f, 4, 1.2,  6.0, 0.55f, 0.25,  0.7, 3000.0, -2.9f, 1.09, 1800.0, 10000.0, 0.18}, // Room
    {23.0,  71.0, 30.0, 16,  70.0,  8.0, 0.45f, 0.6f, 4, 2.0,  9.0,  0.6f, 0.35,  0.5, 3500.0, -0.9f, 1.09, 1800.0, 10000.0, 0.18}, // Hall
    { 9.0,  33.0,  2.0,  0,   0.0,  0.0,  0.0f, 0.0f, 6, 0.8,  7.0,  0.7f,  0.3,  0.9, 6000.0, -3.2f, 1.09, 5000.0, 20000.0, 0.45}, // Plate
    {26.0,  54.0, 10.0,  0,   0.0,  0.0,  0.0f, 0.0f, 0, 1.0,  1.0,  0.0f,  0.0,  0.7, 3000.0, -4.0f,  1.0, 3000.0, 16000.0, 0.18}, // Spring
    {23.0,  71.0,  8.0, 12,  60.0,  8.0,  0.3f, 0.6f, 4, 2.0,  9.0,  0.6f, 0.45,  0.6, 3500.0, -5.5f, 1.09, 1600.0, 20000.0, 0.18}, // Swell
    {31.0,  97.0, 25.0,  0,   0.0,  0.0,  0.0f, 0.0f, 0, 1.0,  1.0,  0.0f,  0.3, 0.45, 4000.0,  2.0f, 1.07, 1000.0,  9000.0, 0.18}, // Bloom
    {47.0, 157.0,  0.0,  0,   0.0,  0.0,  0.0f, 0.0f, 8, 6.0, 72.0, 0.72f,  0.6, 0.35, 5000.0,  9.5f,  2.0, 1400.0, 16000.0, 0.25}, // Cloud
    {23.0,  71.0, 30.0, 12,  60.0,  8.0,  0.3f, 0.6f, 4, 2.0,  9.0,  0.6f,  0.5,  0.5, 3500.0,  6.6f, 1.09,  900.0,  4000.0, 0.18}, // Choir
    {29.0,  89.0, 30.0, 10,  50.0,  8.0,  0.3f, 0.6f, 4, 2.0,  9.0,  0.6f,  0.4,  0.5, 4500.0,  0.0f,  1.0, 2200.0, 14000.0, 0.25}, // Shimmer
    {19.0,  61.0,  0.0,  0,   0.0,  0.0,  0.0f, 0.0f, 4, 2.0, 12.0,  0.7f,  0.3,  0.6, 3500.0,  5.9f, 1.09, 2000.0, 14000.0, 0.18}, // Tape Echo
    {23.0,  71.0,  0.0,  0,   0.0,  0.0,  0.0f, 0.0f, 4, 1.5,  9.0,  0.7f,  0.3,  0.6, 4000.0,  3.0f, 1.09, 2500.0, 16000.0, 0.18}, // Nonlinear
    { 5.0,  17.0,  3.0,  0,   0.0,  0.0,  0.0f, 0.0f, 0, 1.0,  1.0,  0.0f,  0.1,  0.8, 5000.0, -6.0f,  1.0, 2000.0,  8000.0,  0.3}, // Reflections
    {11.0,  41.0, 14.0, 22,  55.0,  9.0,  0.6f, 0.5f, 4, 1.5,  8.0, 0.55f,  0.3,  0.7, 3200.0, -2.9f, 1.09, 1800.0, 10000.0, 0.18}, // Room: Club
    {33.0, 107.0, 70.0, 20, 140.0,  7.0,  0.4f, 0.6f, 4, 3.0, 14.0,  0.5f,  0.4, 0.45, 3000.0, -0.9f, 1.09, 1600.0,  9000.0, 0.18}, // Hall: Arena
    { 5.0,  19.0,  1.0,  0,   0.0,  0.0,  0.0f, 0.0f, 6, 0.6,  5.0, 0.72f,  0.3,  1.0, 5000.0, -3.2f, 1.09, 5000.0, 20000.0, 0.45}, // Plate: Small
}};
// clang-format on

[[nodiscard]] inline const Voicing& VoicingFor(const Settings& s) noexcept
{
    switch (s.machine)
    {
    case Machine::Room:
        return kVoicings[s.roomSize == 1 ? kMachineCount : 0];
    case Machine::Hall:
        return kVoicings[s.hallSize == 1 ? kMachineCount + 1 : 1];
    case Machine::Plate:
        return kVoicings[s.plateSize == 0 ? kMachineCount + 2 : 2];
    default:
        return kVoicings[static_cast<std::size_t>(s.machine)];
    }
}

class Engine
{
  public:
    static constexpr double kMaxPreDelayMs = 1500.0;
    static constexpr double kMaxEchoMs = 1500.0;
    static constexpr double kMaxShapeMs = 2000.0;
    /// A pattern change cross-fades over this; tone-shaping filters ramp over the other.
    static constexpr double kEarlyFadeMs = 25.0;
    static constexpr double kFilterRampMs = 30.0;
    /// The most a tank line holds under Hold, before makeup. Makeup then scales it to the same
    /// level at the output on every machine.
    static constexpr float kHoldRail = 1.4f;
    /// Nonlinear's repeat at the top of Feedback: 3 dB down each time round.
    static constexpr float kShapeFeedbackMax = 0.7f;
    /// How much of each input crosses to the other side ahead of Reflections' taps. The room has
    /// one source, so a hard-panned input still has to light both sides of it.
    static constexpr float kReflectionsCross = 0.42f;

    /// Allocates. Call from Prepare.
    void Prepare(double sampleRate)
    {
        mSampleRate = sampleRate;
        const auto ms = [sampleRate](double v) { return static_cast<std::size_t>(v * 0.001 * sampleRate) + 16; };
        mPreL.Resize(ms(kMaxPreDelayMs + 220.0));
        mPreR.Resize(ms(kMaxPreDelayMs + 220.0));
        mTape.Resize(ms(kMaxEchoMs + 40.0));
        mShapeLine.Resize(ms(kMaxShapeMs + 40.0));
        mTank.Prepare(sampleRate, 180.0);
        mDiffuser.Prepare(sampleRate, 80.0);
        mBloom.Prepare(sampleRate, 240.0);
        mSpring.Prepare(sampleRate);
        mFormants.Prepare(sampleRate);
        mShape.Prepare(sampleRate);
        mTransport.Prepare(sampleRate);

        for (auto& voice : mVoices)
        {
            voice.Prepare(sampleRate, 60.0);
        }

        for (std::size_t side = 0; side < 2; ++side)
        {
            mVoiceHp[side].SetCutoff(140.0, sampleRate);
            mVoiceLp[side].SetCutoff(7000.0, sampleRate);
        }

        mPreDelay.Prepare(sampleRate);
        mPreDelay.SetTimeConstantMs(60.0);
        mPreDelay.SetMaxStep(delay_line::kMaxGlideStep);
        mEchoTime.Prepare(sampleRate);
        mEchoTime.SetTimeConstantMs(150.0);
        mEchoTime.SetMaxStep(delay_line::kMaxGlideStep);
        mHighCut = biquad::HighPass(25.0, biquad::kButterworthQ, sampleRate);
        mReflectionLfo.SetRate(0.43, sampleRate);
        mEarlyFadeSamples = static_cast<int>(kEarlyFadeMs * 0.001 * sampleRate);
        mFilterRampSamples = static_cast<int>(kFilterRampMs * 0.001 * sampleRate);
        mEase = static_cast<float>(1.0 - std::exp(-1.0 / (0.03 * sampleRate)));
        Configure(mSettings, true);
        Reset();
    }

    void Reset() noexcept
    {
        mPreL.Clear();
        mPreR.Clear();
        mTape.Clear();
        mShapeLine.Clear();
        mTank.Reset();
        mDiffuser.Reset();
        mBloom.Reset();
        mSpring.Reset();
        mFormants.Reset();
        mShape.Reset();
        mTransport.Reset();

        for (auto& voice : mVoices)
        {
            voice.Reset();
        }

        for (std::size_t side = 0; side < 2; ++side)
        {
            mVoiceHp[side].Reset();
            mVoiceLp[side].Reset();
            mFilterState[side] = {};
            mTapeLoopLp[side] = 0.0f;
        }

        mRegenL = mRegenR = 0.0f;
        mBloomFbL = mBloomFbR = 0.0f;
        mReflectionLfo.Reset();
        mLateTapLp = {};
        mPreDelay.Snap(mPreDelay.Target());
        mEchoTime.Snap(mEchoTime.Target());
    }

    /// Applies `s`. Allocation-free, so it may run on the audio thread. `snap` jumps the tank's
    /// lengths instead of gliding them, for a fresh machine.
    void Configure(const Settings& s, bool snap) noexcept
    {
        mSettings = s;
        const Voicing& v = VoicingFor(s);
        mVoicing = &v;
        const double fs = mSampleRate;
        const double rt60 = s.decay * v.decayScale;

        // Tank: lengths spread geometrically and kept coprime, so no two lines share a mode.
        std::array<double, reverb::FdnTank::kLines> lengths{};
        const double ratio = std::pow(v.lineMaxMs / v.lineMinMs, 1.0 / 15.0);
        double ms = v.lineMinMs;

        for (std::size_t i = 0; i < lengths.size(); ++i)
        {
            auto samples = static_cast<long long>(std::lround(ms * 0.001 * fs * (1.0 + 0.03 * std::sin(2.3 * i))));

            while (!Coprime(samples, lengths, i))
            {
                ++samples;
            }

            lengths[i] = static_cast<double>(samples);
            ms *= ratio;
        }

        // Interleave short and long so each input's eight lines cover the whole span.
        std::array<double, reverb::FdnTank::kLines> order{};

        for (std::size_t i = 0; i < order.size(); ++i)
        {
            order[i] = lengths[(i * 7) % order.size()];
        }

        mTank.SetLengths(order, snap);
        // Low End sets the low band's decay as a ratio of the mid's; Tone the high band's.
        const double lowRatio = 0.55 * std::pow(1.25 / 0.55, s.lowEnd);
        const double highRatio = v.highRatioMin * std::pow(1.0 / v.highRatioMin, s.tone);
        mTank.SetCrossovers(220.0, v.highCrossoverHz);
        mTank.SetDecay(s.machine == Machine::Reflections ? 0.3 + 0.35 * s.size : rt60, lowRatio, highRatio);
        mModDepthSamples = (v.modMs + 1.0 * s.mod * s.mod) * 0.001 * fs;
        mModRateHz = v.modHz * (0.6 + 0.8 * s.mod);
        mTank.SetModulation(mModDepthSamples * (1.0 - 0.7 * mLastHold), mModRateHz);
        mMakeup = static_cast<float>(std::pow(10.0, v.makeupDb / 20.0));
        mTank.SetCeiling(kHoldRail / mMakeup);

        // Input diffusion: Diffusion moves it on the machines that have the control.
        float diffGain = v.diffGain;

        if (s.machine == Machine::Room)
        {
            diffGain = static_cast<float>(0.15 + 0.6 * s.diffusion);
        }
        else if (s.machine == Machine::Cloud || s.machine == Machine::TapeEcho || s.machine == Machine::Nonlinear)
        {
            diffGain = static_cast<float>(0.72 * s.diffusion);
        }

        mDiffuser.Configure(v.diffStages, v.diffMinMs, v.diffMaxMs);
        mDiffuser.SetGain(diffGain);
        // The Cloud's diffusers swing in quadrature: deeper up to two thirds of Mod, faster after.
        const double cloudDepth = std::min(s.mod, 0.67) * 1.5;
        const double cloudRate = 0.25 + std::max(0.0, s.mod - 0.67) * 4.0;
        mDiffuser.SetModulation(s.machine == Machine::Cloud ? cloudDepth * 0.001 * fs : 0.0, cloudRate);

        mBloom.Configure(8, 12.0 * (0.4 + 2.0 * s.bloomLength), 70.0 * (0.4 + 2.0 * s.bloomLength));
        mBloom.SetGain(0.7f);
        mBloom.SetModulation(0.3 * s.mod * 0.001 * fs, 0.31);
        mBloomFeedback = static_cast<float>(0.88 * s.bloomFeedback);

        if (s.machine == Machine::Reflections)
        {
            constexpr std::array<double, 3> kAspect = {1.0, 1.0 / 1.618, 1.618};
            const double area = 100.0 * std::pow(10.0, std::clamp(s.size, 0.0, 1.0));
            reverb::BuildRoomImages(mEarlyBuilt, area, kAspect[static_cast<std::size_t>(std::clamp(s.roomShape, 0, 2))],
                                    s.listenerX, s.listenerY, 0.45 - 0.33 * s.tone, fs);
        }
        else
        {
            reverb::BuildEarlyPattern(mEarlyBuilt, v.erCount, 0.5, std::max(1.0, v.erEndMs), v.erDecayDb,
                                      0x7e5u + static_cast<unsigned>(s.machine) * 31u + (s.roomSize + s.hallSize) * 7u,
                                      fs);
        }

        AdoptEarlyPattern(snap);
        mLateTapCoeff = reverb::BandDamping::OnePoleCoeff(1500.0 * std::pow(8.0, s.tone), fs);
        mReflectionModTarget = static_cast<float>(0.6 * s.mod * 0.001 * fs);
        mPreDelay.SetTarget((std::clamp(s.preDelayMs, 0.0, kMaxPreDelayMs) + v.gapMs) * 0.001 * fs);

        mSpring.Configure(s.decay, s.tone, s.lowEnd, s.mod, s.dwell, s.springs + 1, snap);
        mSpring.SetCeiling(kHoldRail / mMakeup);

        mFormants.Configure(s.vowel, s.resonance, static_cast<float>(s.mod));

        mVoices[0].SetSemitones(kShiftSemitones[static_cast<std::size_t>(std::clamp(s.shift1, 0, 27))]);
        mVoices[1].SetSemitones(kShiftSemitones[static_cast<std::size_t>(std::clamp(s.shift1, 0, 27))]);
        mSecondVoice = s.shift2 > 0;
        const auto shift2 = static_cast<std::size_t>(std::clamp(s.shift2 - 1, 0, 27));
        mVoices[2].SetSemitones(kShiftSemitones[shift2]);
        mVoices[3].SetSemitones(kShiftSemitones[shift2]);
        mShimmerAmount = static_cast<float>(s.shimmerAmount);

        mEchoTime.SetTarget(std::clamp(s.echoTimeMs, 200.0, kMaxEchoMs) * 0.001 * fs);

        if (snap)
        {
            mPreDelay.Snap(mPreDelay.Target());
            mEchoTime.Snap(mEchoTime.Target());
            mTank.SnapControls();
            mDiffuser.SnapControls();
            mBloom.SnapControls();
            mReflectionMod = mReflectionModTarget;
        }

        mTransport.SetWow(static_cast<float>(0.8 * s.mod));
        mTransport.SetFlutter(static_cast<float>(0.6 * s.mod));
        // Mapped so the default tail rings as long as a tape echo set the same way: the loop EQ
        // and the tape saturation take about a decibel a pass of their own.
        mEchoFeedback = static_cast<float>(0.995 * std::pow(std::clamp(s.echoFeedback, 0.0, 1.0), 0.4));
        mTapeLpCoeff = reverb::BandDamping::OnePoleCoeff(2000.0 * std::pow(4.5, s.tone), fs);
        mTapeHpCoeff = reverb::BandDamping::OnePoleCoeff(250.0 * std::pow(0.2, s.lowEnd), fs);
        ConfigureHeads(s.heads, s.spacing, snap);

        mShape.Configure(s.shape, std::clamp(s.shapeTimeMs, 50.0, kMaxShapeMs), s.modSpeed, s.mod, snap);
        mShapeFeedback = kShapeFeedbackMax * static_cast<float>(s.shapeFeedback);

        // Output EQ: Low End as a shelf, Tone as a low-pass, the Hall's Mid EQ as a bell. A new
        // design ramps in: stepped, the filters' state no longer fits and the output clicks.
        mLowShelf.target = biquad::LowShelf(180.0, biquad::kButterworthQ, -9.0 + 12.0 * s.lowEnd, fs);
        mToneLp.target = biquad::LowPass(v.toneMinHz * std::pow(v.toneMaxHz / v.toneMinHz, s.tone), 0.6, fs);
        mMidBell.target = biquad::Peaking(650.0, 0.7, s.machine == Machine::Hall ? (s.midEq - 0.5) * 16.0 : 0.0, fs);

        if (snap)
        {
            mLowShelf.Finish();
            mToneLp.Finish();
            mMidBell.Finish();
            mFilterRamp = 0;
        }
        else
        {
            mLowShelf.Begin(mFilterRampSamples);
            mToneLp.Begin(mFilterRampSamples);
            mMidBell.Begin(mFilterRampSamples);
            mFilterRamp = mFilterRampSamples;
        }
    }

    /// Control-rate work, once a block: the Choir's moving formants.
    void BeginBlock(int samples) noexcept
    {
        if (mSettings.machine == Machine::Choir)
        {
            mFormants.UpdateBlock(samples);
        }
    }

    /// One sample. `intake` 0-1 closes the input (Freeze); `hold` 0-1 stops the loss.
    void Process(float inL, float inR, float intake, float hold, float& outL, float& outR) noexcept
    {
        const Machine m = mSettings.machine;
        inL *= intake;
        inR *= intake;
        mTank.SetHold(hold);

        // Held, the modulation eases off: it would otherwise smear the held tail away.
        if (hold != mLastHold)
        {
            mLastHold = hold;
            mTank.SetModulation(mModDepthSamples * (1.0 - 0.7 * hold), mModRateHz);
        }

        if (m == Machine::Reflections)
        {
            mPreL.Write(inL + kReflectionsCross * (inR - inL));
            mPreR.Write(inR + kReflectionsCross * (inL - inR));
        }
        else
        {
            mPreL.Write(inL);
            mPreR.Write(inR);
        }

        const double pre = mPreDelay.Next();

        if (m == Machine::Spring)
        {
            // The spring has its own tank; it takes the pre-delayed input, the rest skipped.
            mSpring.SetHold(hold);
            mSpring.Process(mPreL.ReadHermite(pre), mPreR.ReadHermite(pre), outL, outR);
            Finish(outL, outR);
            return;
        }

        float wetL = 0.0f;
        float wetR = 0.0f;
        float tankL = 0.0f;
        float tankR = 0.0f;

        switch (m)
        {
        case Machine::TapeEcho:
            ProcessTape(inL, inR, hold, wetL, wetR, tankL, tankR);
            break;
        case Machine::Nonlinear:
            ProcessNonlinear(inL, inR, wetL, wetR, tankL, tankR);
            break;
        case Machine::Reflections:
            ProcessReflections(pre, wetL, wetR, tankL, tankR);
            break;
        default: {
            tankL = mPreL.ReadHermite(pre);
            tankR = mPreR.ReadHermite(pre);
            float earlyL = 0.0f;
            float earlyR = 0.0f;
            ReadEarly(pre, earlyL, earlyR, false);
            tankL += earlyL * mVoicing->erToTank;
            tankR += earlyR * mVoicing->erToTank;
            wetL += earlyL * mVoicing->erLevel;
            wetR += earlyR * mVoicing->erLevel;

            if (m == Machine::Bloom)
            {
                float l = tankL + mBloomFbL * mBloomFeedback;
                float r = tankR + mBloomFbR * mBloomFeedback;
                mBloom.Process(l, r);
                mBloomFbL = delay_line::RailClip(r, 0.6f, 1.0f); // crossed, so the bloom widens
                mBloomFbR = delay_line::RailClip(l, 0.6f, 1.0f);
                tankL = l;
                tankR = r;
            }
            else
            {
                mDiffuser.Process(tankL, tankR);
            }

            break;
        }
        }

        if (m == Machine::Shimmer)
        {
            Shimmer(tankL, tankR, hold);
        }

        float lateL = 0.0f;
        float lateR = 0.0f;
        mTank.Process(tankL, tankR, lateL, lateR);

        if (m == Machine::Shimmer)
        {
            mRegenL = lateL;
            mRegenR = lateR;
        }

        const float lateLevel = m == Machine::Nonlinear     ? static_cast<float>(mSettings.lateLevel)
                                : m == Machine::Reflections ? 0.6f
                                : m == Machine::TapeEcho    ? static_cast<float>(0.25 + 0.75 * mSettings.diffusion)
                                                            : 1.0f;
        outL = wetL + lateL * lateLevel;
        outR = wetR + lateR * lateLevel;

        if (m == Machine::Choir)
        {
            mFormants.Process(outL, outR);
        }

        Finish(outL, outR);
    }

    [[nodiscard]] bool Finite() const noexcept
    {
        return mTank.StateFinite() && IsFinite(mLastOut);
    }

    [[nodiscard]] const Settings& CurrentSettings() const noexcept
    {
        return mSettings;
    }

  private:
    [[nodiscard]] static bool Coprime(long long n, const std::array<double, reverb::FdnTank::kLines>& set,
                                      std::size_t count) noexcept
    {
        for (std::size_t j = 0; j < count; ++j)
        {
            if (std::gcd(n, static_cast<long long>(set[j])) != 1)
            {
                return false;
            }
        }

        return true;
    }

    /// Takes the pattern just built into mEarlyBuilt. Live, a different pattern cross-fades in;
    /// one that arrives during a fade waits for that fade to end.
    void AdoptEarlyPattern(bool snap) noexcept
    {
        if (snap)
        {
            mEarly[mEarlyNow] = mEarlyBuilt;
            mEarlyFadeLeft = 0;
            mEarlyQueued = false;
        }
        else if (mEarlyFadeLeft > 0)
        {
            mEarlyQueued = !mEarlyBuilt.Same(mEarly[1 - mEarlyNow]);
        }
        else if (!mEarlyBuilt.Same(mEarly[mEarlyNow]))
        {
            StartEarlyFade();
        }
    }

    void StartEarlyFade() noexcept
    {
        mEarly[1 - mEarlyNow] = mEarlyBuilt;
        mEarlyFadeLeft = mEarlyFadeSamples;
        mEarlyQueued = false;
    }

    void ReadEarly(double offset, float& l, float& r, bool damped) noexcept
    {
        if (mEarlyFadeLeft == 0)
        {
            ReadTaps(mEarly[mEarlyNow], offset, 1.0f, l, r, damped);
            return;
        }

        // Equal power: the two patterns are different arrivals, so they add as noise does.
        const float done = 1.0f - static_cast<float>(mEarlyFadeLeft) / static_cast<float>(mEarlyFadeSamples);
        const float angle = static_cast<float>(0.5 * reverb::kPi) * done;
        ReadTaps(mEarly[mEarlyNow], offset, std::cos(angle), l, r, damped);
        ReadTaps(mEarly[1 - mEarlyNow], offset, std::sin(angle), l, r, damped);

        if (--mEarlyFadeLeft == 0)
        {
            mEarlyNow = 1 - mEarlyNow;

            if (mEarlyQueued)
            {
                StartEarlyFade();
            }
        }
    }

    void ReadTaps(const reverb::TapSet& taps, double offset, float scale, float& l, float& r, bool damped) noexcept
    {
        for (std::size_t i = 0; i < taps.count; ++i)
        {
            const double d = offset + static_cast<double>(taps.delay[i]);
            const double spread = static_cast<double>(taps.spread[i]);
            const float sl = scale * mPreL.ReadLinear(d - spread);
            const float sr = scale * mPreR.ReadLinear(d + spread);

            if (damped)
            {
                // Reflections: the later orders darkened by the room's surfaces.
                const float into = taps.late[i] ? 1.0f : 0.0f;
                mLateTapIn[0] += into * taps.gainL[i] * sl;
                mLateTapIn[1] += into * taps.gainR[i] * sr;
                l += (1.0f - into) * taps.gainL[i] * sl;
                r += (1.0f - into) * taps.gainR[i] * sr;
            }
            else
            {
                l += taps.gainL[i] * sl;
                r += taps.gainR[i] * sr;
            }
        }
    }

    void Shimmer(float& tankL, float& tankR, float hold) noexcept
    {
        const int mode = mSettings.shimmerMode; // 0 Input, 1 Regen, 2 both
        // Held, the tank's loop is at unity, so anything shifted back in would keep climbing.
        const float regenL = (1.0f - hold) * mRegenL;
        const float regenR = (1.0f - hold) * mRegenR;
        const float srcL = mode == 0 ? tankL : mode == 1 ? regenL : 0.5f * (tankL + regenL);
        const float srcR = mode == 0 ? tankR : mode == 1 ? regenR : 0.5f * (tankR + regenR);
        float l = mVoices[0].Process(srcL);
        float r = mVoices[1].Process(srcR);

        if (mSecondVoice)
        {
            l = 0.7f * l + 0.7f * mVoices[2].Process(srcL);
            r = 0.7f * r + 0.7f * mVoices[3].Process(srcR);
        }

        l = delay_line::RailClip(mVoiceHp[0].Process(mVoiceLp[0].Process(l)), 0.25f, 0.5f);
        r = delay_line::RailClip(mVoiceHp[1].Process(mVoiceLp[1].Process(r)), 0.25f, 0.5f);
        // In the loop the shifted voices must lose more than they gain, or the climb runs away.
        const float gain = mode == 0 ? 1.4f * mShimmerAmount : 0.9f * mShimmerAmount;
        tankL += gain * r; // crossed, so each pass also moves across the image
        tankR += gain * l;
    }

    void ConfigureHeads(int heads, int spacing, bool snap) noexcept
    {
        // Every head that isn't in use parks at the end of the tape, where the last head always
        // sits, so a change only ever slides heads along it and fades them in or out.
        static constexpr std::array<std::size_t, 3> kHeadCount = {3, 4, 6};
        static constexpr std::array<std::array<float, 6>, 3> kUneven = {{
            {0.31f, 0.72f, 1.0f, 0.0f, 0.0f, 0.0f},
            {0.23f, 0.48f, 0.81f, 1.0f, 0.0f, 0.0f},
            {0.13f, 0.29f, 0.44f, 0.62f, 0.83f, 1.0f},
        }};
        const auto h = static_cast<std::size_t>(std::clamp(heads, 0, 2));
        const std::size_t count = kHeadCount[h];
        const bool uneven = spacing == 1;
        mUneven = uneven ? 1.0f : 0.0f;
        const float gain = 0.95f / std::sqrt(static_cast<float>(count));

        for (std::size_t i = 0; i < mHeadTarget.size(); ++i)
        {
            const bool used = i < count;
            mHeadTarget[i] = !used    ? 1.0f
                             : uneven ? kUneven[h][i]
                                      : static_cast<float>(i + 1) / static_cast<float>(count);
            mHeadLevelTarget[i] = used ? gain : 0.0f;
        }

        mHeadCount = count;

        if (snap)
        {
            mHeadFraction = mHeadTarget;
            mHeadLevel = mHeadLevelTarget;
            mUnevenNow = mUneven;
        }
    }

    void ProcessTape(float inL, float inR, float hold, float& wetL, float& wetR, float& tankL, float& tankR) noexcept
    {
        const double time = mEchoTime.Next();
        const double wobble = mTransport.Next();
        float l = 0.0f;
        float r = 0.0f;
        float last = 0.0f;
        float previous = 0.0f;
        mUnevenNow += (mUneven - mUnevenNow) * mEase;
        // No head outruns half tape speed, or a long slide would read the tape backwards.
        const float slide = static_cast<float>(delay_line::kMaxGlideStep / std::max(1.0, time));

        for (std::size_t i = 0; i < mHeadFraction.size(); ++i)
        {
            mHeadFraction[i] += std::clamp((mHeadTarget[i] - mHeadFraction[i]) * mEase, -slide, slide);
            mHeadLevel[i] += (mHeadLevelTarget[i] - mHeadLevel[i]) * mEase;

            if (i >= mHeadCount && mHeadLevel[i] < 1.0e-4f)
            {
                continue;
            }

            const float head = mTape.ReadHermite(time * mHeadFraction[i] + wobble);
            ((i & 1u) ? r : l) += mHeadLevel[i] * head;

            if (i < mHeadCount)
            {
                previous = last;
                last = head;
            }
        }

        // Feedback off the last head, or the last two when the heads are uneven; through the
        // loop's EQ and the tape's own saturation, so each repeat is darker and softer.
        float fb = last + mUnevenNow * 0.5f * (previous - last);
        mTapeLoopLp[0] = delay_line::FlushDenormal(mTapeLoopLp[0] + mTapeLpCoeff * (fb - mTapeLoopLp[0]));
        mTapeLoopLp[1] = delay_line::FlushDenormal(mTapeLoopLp[1] + mTapeHpCoeff * (mTapeLoopLp[0] - mTapeLoopLp[1]));
        fb = delay_line::AsymSaturate(mTapeLoopLp[0] - mTapeLoopLp[1], 0.6f, 0.1f);
        const float gain = mEchoFeedback + (1.0f - mEchoFeedback) * hold;
        mTape.Write(delay_line::FlushDenormal(0.5f * (inL + inR) + gain * fb));

        const auto diffusion = static_cast<float>(mSettings.diffusion);
        wetL = l * (1.0f - 0.55f * diffusion);
        wetR = r * (1.0f - 0.55f * diffusion);
        tankL = l;
        tankR = r;
        mDiffuser.Process(tankL, tankR);
    }

    void ProcessNonlinear(float inL, float inR, float& wetL, float& wetR, float& tankL, float& tankR) noexcept
    {
        float genL = 0.0f;
        float genR = 0.0f;
        mShape.Process(mShapeLine, genL, genR);
        // Feedback is one read at Time, so the shape repeats every Time. It must not come off the
        // taps: their sum peaks well above unity at some frequencies, and the loop then sings.
        const float repeat = mShapeLine.ReadLinear(mShape.TimeSamples());
        mShapeLine.Write(
            delay_line::FlushDenormal(0.5f * (inL + inR) + mShapeFeedback * delay_line::RailClip(repeat, 0.5f, 1.0f)));
        mDiffuser.Process(genL, genR);
        wetL = genL;
        wetR = genR;
        tankL = genL;
        tankR = genR;
    }

    void ProcessReflections(double pre, float& wetL, float& wetR, float& tankL, float& tankR) noexcept
    {
        mLateTapIn = {};
        // Pre-Delay Mod: the whole pattern drifts against the dry, a slow, random-sounding chorus.
        mReflectionLfo.Advance();
        mReflectionMod += (mReflectionModTarget - mReflectionMod) * mEase;
        ReadEarly(pre + static_cast<double>(mReflectionMod * (1.0f + mReflectionLfo.Sin())), wetL, wetR, true);

        for (std::size_t side = 0; side < 2; ++side)
        {
            mLateTapLp[side] =
                delay_line::FlushDenormal(mLateTapLp[side] + mLateTapCoeff * (mLateTapIn[side] - mLateTapLp[side]));
        }

        wetL += mLateTapLp[0];
        wetR += mLateTapLp[1];
        tankL = mLateTapLp[0];
        tankR = mLateTapLp[1];
    }

    void Finish(float& l, float& r) noexcept
    {
        if (mFilterRamp > 0)
        {
            StepFilterRamp();
        }

        l = Filter(0, l) * mMakeup;
        r = Filter(1, r) * mMakeup;
        mLastOut = std::fabs(l) + std::fabs(r);
    }

    void StepFilterRamp() noexcept
    {
        if (--mFilterRamp == 0)
        {
            mLowShelf.Finish();
            mToneLp.Finish();
            mMidBell.Finish();
            return;
        }

        mLowShelf.Advance();
        mToneLp.Advance();
        mMidBell.Advance();
    }

    [[nodiscard]] float Filter(std::size_t side, float x) noexcept
    {
        auto& st = mFilterState[side];
        double y = st[0].Process(mHighCut, static_cast<double>(x));
        y = st[1].Process(mLowShelf.current, y);
        y = st[2].Process(mMidBell.current, y);
        y = st[3].Process(mToneLp.current, y);
        return static_cast<float>(y);
    }

    double mSampleRate = 48000.0;
    Settings mSettings;
    const Voicing* mVoicing = &kVoicings[1];

    delay_line::FractionalDelayLine mPreL;
    delay_line::FractionalDelayLine mPreR;
    delay_line::GlideRamp mPreDelay;
    std::array<reverb::TapSet, 2> mEarly; ///< the pattern in use and, during a fade, the next
    reverb::TapSet mEarlyBuilt;           ///< the latest asked for
    std::size_t mEarlyNow = 0;
    int mEarlyFadeLeft = 0;
    int mEarlyFadeSamples = 1;
    bool mEarlyQueued = false;
    reverb::DiffuserChain mDiffuser;
    reverb::DiffuserChain mBloom;
    float mBloomFbL = 0.0f, mBloomFbR = 0.0f, mBloomFeedback = 0.0f;
    reverb::FdnTank mTank;
    reverb::VerbalSpring mSpring;
    reverb::FormantBank mFormants;

    std::array<GrainPitchShifter, 4> mVoices;
    std::array<delay_line::OnePoleHp, 2> mVoiceHp{};
    std::array<delay_line::OnePoleLp, 2> mVoiceLp{};
    float mRegenL = 0.0f, mRegenR = 0.0f, mShimmerAmount = 0.6f;
    bool mSecondVoice = false;

    delay_line::FractionalDelayLine mTape;
    delay_line::GlideRamp mEchoTime;
    delay_line::TapeTransport mTransport;
    std::array<float, 6> mHeadFraction{};
    std::array<float, 6> mHeadTarget{};
    std::array<float, 6> mHeadLevel{};
    std::array<float, 6> mHeadLevelTarget{};
    std::size_t mHeadCount = 3;
    float mUneven = 0.0f, mUnevenNow = 0.0f;
    float mEchoFeedback = 0.5f, mTapeLpCoeff = 0.3f, mTapeHpCoeff = 0.03f;
    std::array<float, 2> mTapeLoopLp{};

    delay_line::FractionalDelayLine mShapeLine;
    reverb::ShapeGenerator mShape;
    float mShapeFeedback = 0.0f;

    std::array<float, 2> mLateTapIn{};
    std::array<float, 2> mLateTapLp{};
    float mLateTapCoeff = 0.3f;
    float mReflectionMod = 0.0f;
    float mReflectionModTarget = 0.0f;
    reverb::RotatingPhasor mReflectionLfo;

    BiquadCoefficients mHighCut;
    biquad::Ramp mLowShelf, mToneLp, mMidBell;
    int mFilterRamp = 0;
    int mFilterRampSamples = 1;
    std::array<std::array<biquad::State, 4>, 2> mFilterState{};
    float mMakeup = 1.0f;
    float mEase = 0.001f;
    double mModDepthSamples = 0.0;
    double mModRateHz = 0.5;
    float mLastHold = 0.0f;
    float mLastOut = 0.0f;
};
} // namespace guitarfx::verbal
