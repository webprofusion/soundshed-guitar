#pragma once

#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/BuiltinAmpFilters.h"
#include "dsp/effects/BuiltinAmpOversampling.h"
#include "dsp/effects/BuiltinAmpPresets.h"
#include "dsp/effects/BuiltinAmpVoicing.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace guitarfx
{
/**
 * Built-in amp head. The whole nonlinear path runs at up to 4x with an
 * anti-aliasing half-band decimator; cabinet filtering belongs downstream.
 * What it sounds like (parameters, clipper knees, drive law, level table)
 * is in BuiltinAmpVoicing.h, and its linear filters in BuiltinAmpFilters.h.
 */
class BuiltinAmpEffect : public EffectProcessor
{
  public:
    /// Each channel runs on its own state from the same settings (EffectProcessor::KeepsChannelsSeparate).
    [[nodiscard]] bool KeepsChannelsSeparate() const override
    {
        return true;
    }

    /// Identical sides in, identical sides out, whatever the settings (EffectProcessor::CanWiden).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mOversamplingFactor = sampleRate < 88200.0 ? 4 : (sampleRate < 176400.0 ? 2 : 1);
        mDspSampleRate = sampleRate * mOversamplingFactor;

        for (auto* bank : {&mUpFirst, &mDownFirst, &mUpSecond, &mDownSecond})
        {
            for (auto& filter : *bank)
            {
                filter.Prepare();
            }
        }

        UpdateStageFilters();
        UpdateSmoothing();
        UpdateSagCoefficients();
        UpdatePreFilters();
        UpdatePreEmphasis();
        UpdateToneStack();
        UpdateVoicingFilter();
        UpdateSpeakerFilters();
        UpdatePostFilters();
        Reset();
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        return mOversamplingFactor == 4 ? 48 : (mOversamplingFactor == 2 ? 32 : 0);
    }

    void Reset() override
    {
        for (auto* bank : {&mUpFirst, &mDownFirst, &mUpSecond, &mDownSecond})
        {
            for (auto& filter : *bank)
            {
                filter.Reset();
            }
        }

        for (auto& filter : mStageFilters)
        {
            filter.Reset();
        }

        for (auto& filter : mFilters)
        {
            filter.Reset();
        }

        mSagEnv.fill(0.0f);

        mVoiceSmoothed = mVoice;
        mGainSmoothed = mGain;
        mNominal.Follow();
        mStageGainSmoothed = mStageGainLinear * mNominal.inputGain;
        mPowerDriveSmoothed = mPowerDrive;
        mSagSmoothed = mSag;
        mBiasSmoothed = mBias;
        mCharacterSmoothed = mCharacter;
        mOutputGainSmoothed = mOutputGainTarget / mNominal.inputGain;
        mPowerMakeupSmoothed = mPowerMakeupTarget;
        AdvanceFilters(1.0);
        mStageFades.Settle(std::clamp(mStageCount, 1, kMaxStages));
        mVoicingStale = true;
        mRightChannelStale = false;
        UpdateVoicing();
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!outputs || numSamples <= 0)
        {
            return;
        }

        if (mRightChannelStale)
        {
            // The right channel sat still while the input was mono. Its input
            // was the left channel's all along, so the left state is exactly
            // where it would have got to.
            CopyChannelState(0, 1);
            mRightChannelStale = false;
        }

        const float* in[2] = {inputs ? inputs[0] : nullptr, inputs ? inputs[1] : nullptr};
        Render<2>(in, outputs, numSamples);
    }

    // A guitar is mono, and the executor hands a mono signal to effects that
    // can take it. Nearly all of this amp's work is per channel (4x resampling
    // and the whole nonlinear chain), so running one channel halves the cost.
    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        const float* in[1] = {input};
        float* out[1] = {output};
        Render<1>(in, out, numSamples);
        mRightChannelStale = true;
    }

    void SetParam(const std::string& key, double value) override
    {
        using namespace builtin_amp;

        const std::size_t index = FindParam(key);

        // A NaN survives the clamp below, and as a gain it would index the level table with it.
        if (index == kParamCount || !IsFinite(value))
        {
            return;
        }

        value = NormaliseParamValue(kParams[index], value);

        switch (index)
        {
        case kVoice:
            // A channel switch, like Bright; the glide still blends the change.
            mVoice = (value >= 0.5) ? 1.0f : 0.0f;
            break;
        case kGain:
            mGain = static_cast<float>(value);
            break;
        case kCharacter:
            mCharacter = static_cast<float>(value);
            UpdateStageFilters();
            UpdatePreFilters();
            UpdatePreEmphasis();
            UpdateVoicingFilter();
            break;
        case kBright:
            mBright = (value >= 0.5) ? 1.0f : 0.0f;
            UpdatePreEmphasis();
            break;
        case kPreEmphasis:
            mPreEmphasis = static_cast<float>(value);
            UpdatePreEmphasis();
            break;
        case kStageCount:
            // Already rounded into range, unless it was NaN.
            mStageCount = std::clamp(static_cast<int>(value), 1, kMaxStages);
            break;
        case kStageGain:
            mStageGainDb = value;
            mStageGainLinear = DbToLinear(value);
            break;
        case kBass:
            mBass = value;
            UpdateToneStack();
            break;
        case kMiddle:
            mMiddle = value;
            UpdateToneStack();
            break;
        case kTreble:
            mTreble = value;
            UpdateToneStack();
            break;
        case kContour:
            mContour = value;
            UpdateToneStack();
            break;
        case kPresence:
            mPresence = value;
            UpdateToneStack();
            break;
        case kOutput:
            mOutputDb = value;
            mOutputGainTarget = DbToLinear(value);
            break;
        case kPowerDrive:
            mPowerDrive = static_cast<float>(value);
            break;
        case kSag:
            mSag = static_cast<float>(value);
            break;
        case kBias:
            mBias = static_cast<float>(value);
            break;
        case kDepth:
            mDepth = static_cast<float>(value);
            UpdateSpeakerFilters();
            break;
        case kResonance:
            mResonance = static_cast<float>(value);
            UpdateSpeakerFilters();
            break;
        case kDamping:
            mDamping = static_cast<float>(value);
            UpdateSpeakerFilters();
            break;
        default:
            break;
        }

        // What feeds the power stage and how it clips: worked out per change, then glided.
        if (index == kVoice || index == kGain || index == kCharacter || index == kStageCount || index == kStageGain ||
            index == kPowerDrive || index == kBias || index == kSag)
        {
            mPowerMakeupTarget = PowerDriveMakeup(mGain, mVoice, mStageCount, mCharacter, mPowerDrive, mBias, mSag,
                                                  static_cast<float>(mStageGainDb));
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        using namespace builtin_amp;

        switch (FindParam(key))
        {
        case kVoice:
            return mVoice;
        case kGain:
            return mGain;
        case kCharacter:
            return mCharacter;
        case kBright:
            return mBright;
        case kPreEmphasis:
            return mPreEmphasis;
        case kStageCount:
            return mStageCount;
        case kStageGain:
            return mStageGainDb;
        case kBass:
            return mBass;
        case kMiddle:
            return mMiddle;
        case kTreble:
            return mTreble;
        case kContour:
            return mContour;
        case kPresence:
            return mPresence;
        case kOutput:
            return mOutputDb;
        case kPowerDrive:
            return mPowerDrive;
        case kSag:
            return mSag;
        case kBias:
            return mBias;
        case kDepth:
            return mDepth;
        case kResonance:
            return mResonance;
        case kDamping:
            return mDamping;
        default:
            return 0.0;
        }
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "amp_builtin";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "amp";
    }

  private:
    static constexpr int kMaxStages = builtin_amp::kMaxStages;

    // The fixed-shape filters, in the order the signal meets them.
    enum Filter
    {
        kPreHighPass,
        kPreEmphasisShelf,
        kBassShelf,
        kMiddlePeak,
        kContourPeak,
        kTrebleShelf,
        kVoicingPeak,
        kPresencePeak,
        kDepthShelf,
        kResonancePeak,
        kDampingShelf,
        kPostHighPass,
        kFilterCount
    };

    // Every piece of per-channel state; the same list Reset() clears. If a
    // new filter or envelope is added to one, add it here too, or the right
    // channel comes back from a mono stretch out of step with the left
    // (TestMonoPath catches that).
    void CopyChannelState(int from, int to)
    {
        for (auto* bank : {&mUpFirst, &mDownFirst, &mUpSecond, &mDownSecond})
        {
            (*bank)[to] = (*bank)[from];
        }

        for (auto& filter : mStageFilters)
        {
            filter.CopyChannel(from, to);
        }

        for (auto& filter : mFilters)
        {
            filter.CopyChannel(from, to);
        }

        mSagEnv[to] = mSagEnv[from];
    }

    // The processing loop for one channel (ProcessMono) or two (Process).
    // Controls and voicing are shared and advance once per sample either way,
    // so a mono block and a stereo block leave them in the same place.
    template <int Channels> void Render(const float* const* inputs, float* const* outputs, int numSamples)
    {
        using builtin_amp::Glide;
        const int stageCount = std::clamp(mStageCount, 1, kMaxStages);
        const float smoothStep = 1.0f - mControlSmoothCoef;
        mNominal.Follow();
        const float inputTarget = mStageGainLinear * mNominal.inputGain;
        const float outputTarget = mOutputGainTarget / mNominal.inputGain;

        for (int i = 0; i < numSamples; ++i)
        {
            float highInput[Channels][4] = {};
            float highOutput[Channels][4] = {};

            for (int ch = 0; ch < Channels; ++ch)
            {
                const float* in = inputs[ch];
                const float sample = in ? in[i] : 0.0f;

                if (mOversamplingFactor == 1)
                {
                    highInput[ch][0] = sample;
                }
                else
                {
                    float firstEven = 0.0f, firstOdd = 0.0f;
                    mUpFirst[ch].Upsample(sample, firstEven, firstOdd);

                    if (mOversamplingFactor == 2)
                    {
                        highInput[ch][0] = firstEven;
                        highInput[ch][1] = firstOdd;
                    }
                    else
                    {
                        mUpSecond[ch].Upsample(firstEven, highInput[ch][0], highInput[ch][1]);
                        mUpSecond[ch].Upsample(firstOdd, highInput[ch][2], highInput[ch][3]);
                    }
                }
            }

            // Controls glide at the host rate: 5-10 ms ramps gain nothing from
            // being stepped four times as often, and this is a lot of work
            // (every tone filter's coefficients) to repeat per oversampled
            // sample.
            Glide(mVoiceSmoothed, mVoice, smoothStep);
            Glide(mGainSmoothed, mGain, smoothStep);
            mStageGainSmoothed += smoothStep * (inputTarget - mStageGainSmoothed);
            Glide(mPowerDriveSmoothed, mPowerDrive, smoothStep);
            mSagSmoothed += smoothStep * (mSag - mSagSmoothed);
            Glide(mBiasSmoothed, mBias, smoothStep);
            Glide(mCharacterSmoothed, mCharacter, smoothStep);
            mOutputGainSmoothed += smoothStep * (outputTarget - mOutputGainSmoothed);
            Glide(mPowerMakeupSmoothed, mPowerMakeupTarget, smoothStep);
            AdvanceFilters(1.0 - mFilterSmoothCoef);
            UpdateVoicing(mStageFades.Advance(stageCount, mStageFilters));

            for (int sub = 0; sub < mOversamplingFactor; ++sub)
            {
                for (int ch = 0; ch < Channels; ++ch)
                {
                    highOutput[ch][sub] = ProcessAmpSample(highInput[ch][sub], ch);
                }
            }

            for (int ch = 0; ch < Channels; ++ch)
            {
                if (!outputs[ch])
                {
                    continue;
                }

                float output = highOutput[ch][0];

                if (mOversamplingFactor == 2)
                {
                    output = mDownFirst[ch].Downsample(highOutput[ch][0], highOutput[ch][1]);
                }
                else if (mOversamplingFactor == 4)
                {
                    const float first = mDownSecond[ch].Downsample(highOutput[ch][0], highOutput[ch][1]);
                    const float second = mDownSecond[ch].Downsample(highOutput[ch][2], highOutput[ch][3]);
                    output = mDownFirst[ch].Downsample(first, second);
                }

                outputs[ch][i] = output;
            }
        }
    }

    /**
     * Everything derived from the smoothed controls that is the same for both
     * channels and all four oversampled steps, worked out once per host sample
     * instead of once per channel per stage.
     */
    void UpdateVoicing(bool stagesMoved = false)
    {
        // Each group is only refreshed while its controls are moving; the
        // glides settle exactly on their targets, so a static setting costs a
        // handful of compares here, not tanh and exp2 every sample.
        const bool kneeMoved = mVoicingStale || mCharacterSmoothed != mVoicedCharacter;

        if (kneeMoved)
        {
            mVoicedCharacter = mCharacterSmoothed;
            mClippers.SetCharacter(mCharacterSmoothed);
        }

        if (kneeMoved || mPowerDriveSmoothed != mVoicedPowerDrive || mBiasSmoothed != mVoicedBias)
        {
            mVoicedPowerDrive = mPowerDriveSmoothed;
            mVoicedBias = mBiasSmoothed;
            mClippers.SetPowerStage(mPowerDriveSmoothed, mBiasSmoothed);
        }

        if (kneeMoved || stagesMoved || mGainSmoothed != mVoicedGain || mVoiceSmoothed != mVoicedVoice)
        {
            mVoicedGain = mGainSmoothed;
            mVoicedVoice = mVoiceSmoothed;
            mLevelMakeup = mStageFades.Makeup(mGainSmoothed, mVoiceSmoothed, mCharacterSmoothed);
        }

        mVoicingStale = false;
    }

    float ProcessAmpSample(float sample, int ch)
    {
        using namespace builtin_amp;

        double signal = mFilters[kPreHighPass].Process(sample, ch);
        signal = mFilters[kPreEmphasisShelf].Process(signal, ch);

        const float gain = mGainSmoothed;
        const float voice = mVoiceSmoothed;
        const float input = static_cast<float>(signal) * mStageGainSmoothed;
        const float clean = mClippers.Clip(input * (2.0f + 2.5f * gain), kClipClean);
        const float drive = mClippers.Clip(input * StageDrive(gain, 1.0f, 5.0f, 9.0f, 55.0f), kClipDrive);
        float stage = (clean + (drive - clean) * voice) * 0.9f;
        stage = mStageFilters[0].Process(stage, ch);

        if (const float fade = mStageFades.mix[1]; fade > 0.0f)
        {
            float engaged = mClippers.Clip(stage * StageDrive(gain, voice, 1.1f, 1.5f, 7.0f), kClipStage2) * 0.85f;
            engaged = mStageFilters[1].Process(engaged, ch);
            stage = StageFades::SwitchIn(stage, engaged, fade);
        }

        signal = mFilters[kBassShelf].Process(stage, ch);
        signal = mFilters[kMiddlePeak].Process(signal, ch);
        signal = mFilters[kContourPeak].Process(signal, ch);
        signal = mFilters[kTrebleShelf].Process(signal, ch);

        stage = static_cast<float>(signal);

        if (const float fade = mStageFades.mix[2]; fade > 0.0f)
        {
            float engaged = mClippers.Clip(stage * StageDrive(gain, voice, 1.6f, 1.2f, 8.0f), kClipStage3) * 0.8f;
            engaged = mStageFilters[2].Process(engaged, ch);
            stage = StageFades::SwitchIn(stage, engaged, fade);
        }

        if (const float fade = mStageFades.mix[3]; fade > 0.0f)
        {
            float engaged = mClippers.Clip(stage * StageDrive(gain, voice, 1.35f, 0.8f, 6.0f), kClipStage4) * 0.8f;
            engaged = mStageFilters[3].Process(engaged, ch);
            stage = StageFades::SwitchIn(stage, engaged, fade);
        }

        signal = mFilters[kVoicingPeak].Process(stage, ch);

        // Frequency-dependent power-stage drive: these controls shape the
        // distortion as well as the level. The external IR supplies cabinet
        // and microphone tone.
        signal = mFilters[kPresencePeak].Process(signal, ch);
        signal = mFilters[kDepthShelf].Process(signal, ch);
        signal = mFilters[kResonancePeak].Process(signal, ch);
        signal = mFilters[kDampingShelf].Process(signal, ch);

        // Sag is the supply giving way as the power stage works: the envelope
        // pulls the stage's ceiling down to `headroom`, and the knee meets the
        // signal against that lower ceiling, so loud playing clips harder and
        // is squeezed the most. It acts only on the clipped share of the
        // stage, so with no Power Drive it changes nothing. The level it
        // costs at the nominal level is made up (PowerDriveMakeup).
        const float powerInput = static_cast<float>(signal);
        const float detector = std::abs(powerInput);
        const float sagCoefficient = detector > mSagEnv[ch] ? mSagAttackCoef : mSagReleaseCoef;
        mSagEnv[ch] = sagCoefficient * mSagEnv[ch] + (1.0f - sagCoefficient) * detector;
        const float headroom = builtin_amp::SagHeadroom(mSagSmoothed, mSagEnv[ch]);

        const float driveAmount = mPowerDriveSmoothed;
        const float clipped = mClippers.PowerClip(powerInput, headroom);
        const float powered = powerInput + driveAmount * (clipped - powerInput);
        signal = mFilters[kPostHighPass].Process(powered, ch);
        return static_cast<float>(signal) * mOutputGainSmoothed * mLevelMakeup * mPowerMakeupSmoothed;
    }

    static float DbToLinear(double db)
    {
        return static_cast<float>(std::pow(10.0, db * 0.05));
    }

    void AdvanceFilters(double step)
    {
        for (auto& filter : mFilters)
        {
            filter.Advance(step);
        }

        for (auto& filter : mStageFilters)
        {
            filter.Advance(static_cast<float>(step));
        }
    }

    void UpdateStageFilters()
    {
        if (mDspSampleRate <= 0.0)
        {
            return;
        }

        // Loose and dark at the vintage end, so low strings bloom and smear
        // into the next stage and the top is round; tight and open at the
        // modern end. Both scales are exactly 1.0 at the default.
        const double character = mCharacter;
        const double highPassScale = 0.5 + 1.2 * character - 0.4 * character * character;
        const double lowPassScale = 0.55 + 1.15 * character - 0.5 * character * character;

        for (int stage = 0; stage < kMaxStages; ++stage)
        {
            mStageFilters[stage].SetCorners(mDspSampleRate, builtin_amp::kStageHighPass[stage] * highPassScale,
                                            builtin_amp::kStageLowPass[stage] * lowPassScale);
        }
    }

    // Upper-mid voicing after the preamp. Whatever is fed to a clipper comes
    // out as more or less distortion rather than a different colour, so the
    // fuzz-to-modern brightness is set here, on the finished preamp signal:
    // wooly and set back at the vintage end, forward bite at the modern end.
    // Flat at the default Character.
    void UpdateVoicingFilter()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        const double gainDb = (static_cast<double>(mCharacter) - 0.5) * 8.0;
        mFilters[kVoicingPeak].target = builtin_amp::DesignPeaking(1800.0, 0.8, gainDb, mDspSampleRate);
    }

    void UpdateSmoothing()
    {
        mStageFades.SetSampleRate(mSampleRate);

        if (mSampleRate <= 0.0)
        {
            mControlSmoothCoef = 0.0f;
            mFilterSmoothCoef = 0.0;
            return;
        }

        // Stepped once per host sample (see Process), so host-rate constants.
        mControlSmoothCoef = static_cast<float>(std::exp(-1.0 / (0.01 * mSampleRate)));
        mFilterSmoothCoef = std::exp(-1.0 / (0.005 * mSampleRate));
    }

    void UpdateSagCoefficients()
    {
        if (mSampleRate <= 0.0)
        {
            mSagAttackCoef = 0.0f;
            mSagReleaseCoef = 0.0f;
            return;
        }

        const double attackTau = 0.01;  // 10 ms
        const double releaseTau = 0.18; // 180 ms
        mSagAttackCoef = static_cast<float>(std::exp(-1.0 / (attackTau * mDspSampleRate)));
        mSagReleaseCoef = static_cast<float>(std::exp(-1.0 / (releaseTau * mDspSampleRate)));
    }

    void UpdateToneStack()
    {
        using namespace builtin_amp;

        if (mSampleRate <= 0.0)
        {
            return;
        }

        const double bassGain = (mBass - 0.5) * 18.0;
        const double midGain = (mMiddle - 0.5) * 18.0;
        const double trebleGain = (mTreble - 0.5) * 18.0;
        const double contourGain = -12.0 * mContour;
        const double presenceGain = (mPresence - 0.5) * 12.0;

        mFilters[kBassShelf].target = DesignLowShelf(120.0, 0.8, bassGain, mDspSampleRate);
        mFilters[kMiddlePeak].target = DesignPeaking(750.0, 0.9, midGain, mDspSampleRate);
        mFilters[kContourPeak].target = DesignPeaking(600.0, 0.7, contourGain, mDspSampleRate);
        mFilters[kTrebleShelf].target = DesignHighShelf(3500.0, 0.9, trebleGain, mDspSampleRate);
        mFilters[kPresencePeak].target = DesignPeaking(4000.0, 1.2, presenceGain, mDspSampleRate);
    }

    void UpdatePreFilters()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        // Modern high gain tightens the low end before anything clips, the
        // same job a boost pedal in front does; vintage lets it all in. Exactly
        // 60 Hz at the default Character.
        const double character = mCharacter;
        const double scale = 0.75 + 0.25 * character + 0.5 * character * character;
        mFilters[kPreHighPass].target = builtin_amp::DesignHighPass(60.0 * scale, 0.707, mDspSampleRate);
    }

    void UpdatePreEmphasis()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        const double brightBoost = (mBright > 0.5f) ? 3.0 : 0.0;
        const double emphasisBoost = static_cast<double>(mPreEmphasis) * 6.0;
        // What goes into the first clipper decides where the harmonics land. A
        // soft knee distorts at every level and would otherwise out-fizz the
        // hard one, so Character tilts the input too: darker into the vintage
        // stages, brighter into the modern ones. Zero at the default.
        const double characterTilt = (static_cast<double>(mCharacter) - 0.5) * 8.0;
        const double gainDb = brightBoost + emphasisBoost + characterTilt;

        mFilters[kPreEmphasisShelf].target = builtin_amp::DesignHighShelf(2500.0, 0.8, gainDb, mDspSampleRate);
    }

    void UpdateSpeakerFilters()
    {
        using namespace builtin_amp;

        if (mSampleRate <= 0.0)
        {
            return;
        }

        const double depthGain = static_cast<double>(mDepth) * 6.0;
        const double resonanceGain = static_cast<double>(mResonance) * 6.0;
        const double dampingGain = static_cast<double>(mDamping) * -6.0;

        mFilters[kDepthShelf].target = DesignLowShelf(120.0, 0.9, depthGain, mDspSampleRate);
        mFilters[kResonancePeak].target = DesignPeaking(120.0, 1.0, resonanceGain, mDspSampleRate);
        mFilters[kDampingShelf].target = DesignHighShelf(3500.0, 0.9, dampingGain, mDspSampleRate);
    }

    void UpdatePostFilters()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        mFilters[kPostHighPass].target = builtin_amp::DesignHighPass(25.0, 0.707, mDspSampleRate);
    }

    double mSampleRate = 44100.0;
    double mDspSampleRate = 44100.0;
    int mMaxBlockSize = 0;
    int mOversamplingFactor = 1;
    std::array<BuiltinAmpHalfband2x, 2> mUpFirst = {}, mDownFirst = {}, mUpSecond = {}, mDownSecond = {};
    std::array<builtin_amp::StageFilter, kMaxStages> mStageFilters = {};
    builtin_amp::StageFades mStageFades;
    builtin_amp::NominalLevel mNominal;
    std::array<builtin_amp::GlidingBiquad, kFilterCount> mFilters = {};

    float mVoice = 0.0f;
    float mVoiceSmoothed = 0.0f;
    float mControlSmoothCoef = 0.0f;
    double mFilterSmoothCoef = 0.0;
    float mGain = builtin_amp::kDefaultGain;
    float mGainSmoothed = builtin_amp::kDefaultGain;
    double mBass = 0.5;
    double mMiddle = 0.5;
    double mTreble = 0.5;
    double mContour = 0.2;
    double mPresence = 0.5;
    double mOutputDb = 0.0;
    float mOutputGainTarget = 1.0f;
    float mOutputGainSmoothed = 1.0f;
    int mStageCount = builtin_amp::kDefaultStages;
    double mStageGainDb = 0.0;
    float mStageGainLinear = 1.0f;
    float mStageGainSmoothed = 1.0f;
    float mBright = 0.0f;
    float mPreEmphasis = 0.0f;
    float mPowerDrive = 0.0f;
    float mPowerDriveSmoothed = 0.0f;
    float mPowerMakeupTarget = 1.0f;
    float mPowerMakeupSmoothed = 1.0f;
    float mSag = 0.0f;
    float mSagSmoothed = 0.0f;
    float mBias = 0.0f;
    float mBiasSmoothed = 0.0f;
    float mCharacter = 0.5f;
    float mCharacterSmoothed = 0.5f;
    bool mVoicingStale = true;
    bool mRightChannelStale = false;
    float mVoicedCharacter = 0.0f;
    float mVoicedPowerDrive = 0.0f;
    float mVoicedBias = 0.0f;
    float mVoicedGain = 0.0f;
    float mVoicedVoice = 0.0f;
    builtin_amp::Clippers mClippers;
    float mLevelMakeup = 1.0f;
    float mDepth = 0.4f;
    float mResonance = 0.4f;
    float mDamping = 0.5f;
    float mSagAttackCoef = 0.0f;
    float mSagReleaseCoef = 0.0f;
    std::array<float, 2> mSagEnv = {0.0f, 0.0f};
};

inline void RegisterBuiltinAmpEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kAmpBuiltin;
    info.aliases = {"amp_builtin"};
    info.displayName = "Heavy American";
    info.category = "amp";
    info.description = "High-gain amp head for use with a separate cabinet or IR";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(builtin_amp::kParams);
    info.presets = builtin_amp::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<BuiltinAmpEffect>(); });
}
} // namespace guitarfx
