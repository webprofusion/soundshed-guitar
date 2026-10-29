#pragma once

#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/BuiltinAmpFilters.h"
#include "dsp/effects/BuiltinAmpPresets.h"
#include "dsp/effects/BuiltinAmpRender.h"
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
 * is in BuiltinAmpVoicing.h, its linear filters in BuiltinAmpFilters.h, and
 * how the signal runs through them, a chunk at a time, in BuiltinAmpRender.h.
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
        mControls.shift = mOversamplingFactor == 4 ? 2 : (mOversamplingFactor == 2 ? 1 : 0);
        mPath.Prepare();

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
        mPath.Reset();
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
            mPath.CopyChannel(0, 1);
            mRightChannelStale = false;
        }

        const float* in[2] = {inputs ? inputs[0] : nullptr, inputs ? inputs[1] : nullptr};
        const bool inputFinite = AllFinite(in, 2, numSamples); // before Render: it may write in place
        Render<2>(in, outputs, numSamples);
        ScrubNonFinite(outputs, 2, numSamples, inputFinite);
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
        const bool inputFinite = AllFinite(in, 1, numSamples);
        Render<1>(in, out, numSamples);
        ScrubNonFinite(out, 1, numSamples, inputFinite);
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

    // The processing loop for one channel (ProcessMono) or two (Process), a
    // chunk at a time (BuiltinAmpRender.h). Controls and voicing are shared
    // and advance once per sample either way, so a mono block and a stereo
    // block leave them in the same place.
    template <int Channels> void Render(const float* const* inputs, float* const* outputs, int numSamples)
    {
        const int stageCount = std::clamp(mStageCount, 1, kMaxStages);
        mNominal.Follow();
        const float inputTarget = mStageGainLinear * mNominal.inputGain;
        const float outputTarget = mOutputGainTarget / mNominal.inputGain;

        for (int start = 0; start < numSamples; start += builtin_amp::kChunk)
        {
            StepControls(std::min(builtin_amp::kChunk, numSamples - start), stageCount, inputTarget, outputTarget);
            mPath.Render<Channels>(inputs, outputs, start, mControls);
        }
    }

    [[nodiscard]] static bool AllFinite(const float* const* inputs, int channels, int numSamples)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            for (int i = 0; inputs[ch] && i < numSamples; ++i)
            {
                if (!IsFinite(inputs[ch][i]))
                {
                    return false;
                }
            }
        }

        return true;
    }

    // A non-finite sample from upstream (a hosted plugin on a bad day) latches the stages'
    // recursive state, and the amp put out NaN, or nothing, until its node was rebuilt. So start
    // over after a block that took one in, or whose last sample the filters carried one to. The
    // output alone is not enough: under -ffast-math the clamps and clipper can turn a NaN finite
    // on its way out, and the stages stayed poisoned behind a finite, silent output.
    void ScrubNonFinite(float* const* outputs, int channels, int numSamples, bool inputFinite)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            if (!outputs[ch] || (inputFinite && IsFinite(outputs[ch][numSamples - 1])))
            {
                continue;
            }

            Reset();

            for (int c = 0; c < channels; ++c)
            {
                if (outputs[c])
                {
                    std::fill_n(outputs[c], numSamples, 0.0f);
                }
            }

            return;
        }
    }

    // Controls glide at the host rate: 5-10 ms ramps gain nothing from being
    // stepped four times as often. Each host sample of the chunk is stepped in
    // turn, and what the stages read is kept for the signal path.
    void StepControls(int count, int stageCount, float inputTarget, float outputTarget)
    {
        using namespace builtin_amp;
        const float smoothStep = 1.0f - mControlSmoothCoef;
        ChunkControls& c = mControls;
        c.count = count;
        c.filterStep = 1.0 - mFilterSmoothCoef;

        for (int h = 0; h < count; ++h)
        {
            const std::array<float, 9> before = Smoothed();
            Glide(mVoiceSmoothed, mVoice, smoothStep);
            Glide(mGainSmoothed, mGain, smoothStep);
            Glide(mStageGainSmoothed, inputTarget, smoothStep);
            Glide(mPowerDriveSmoothed, mPowerDrive, smoothStep);
            Glide(mSagSmoothed, mSag, smoothStep);
            Glide(mBiasSmoothed, mBias, smoothStep);
            Glide(mCharacterSmoothed, mCharacter, smoothStep);
            Glide(mOutputGainSmoothed, outputTarget, smoothStep);
            Glide(mPowerMakeupSmoothed, mPowerMakeupTarget, smoothStep);
            const bool stagesMoved = mStageFades.Advance(stageCount, mPath.stageFilters);
            const ClipperChange change = UpdateVoicing(stagesMoved);

            const float gain = mGainSmoothed;
            const float voice = mVoiceSmoothed;
            c.stageGain[h] = mStageGainSmoothed;
            c.voice[h] = voice;
            c.drive[kClipClean][h] = 2.0f + 2.5f * gain;
            c.drive[kClipDrive][h] = StageDrive(gain, 1.0f, 5.0f, 9.0f, 55.0f);
            c.drive[kClipStage2][h] = StageDrive(gain, voice, 1.1f, 1.5f, 7.0f);
            c.drive[kClipStage3][h] = StageDrive(gain, voice, 1.6f, 1.2f, 8.0f);
            c.drive[kClipStage4][h] = StageDrive(gain, voice, 1.35f, 0.8f, 6.0f);

            for (int stage = 0; stage < kMaxStages; ++stage)
            {
                c.mix[stage][h] = mStageFades.mix[stage];
            }

            c.powerDrive[h] = mPowerDriveSmoothed;
            c.sag[h] = mSagSmoothed;
            c.outputGain[h] = mOutputGainSmoothed;
            c.levelMakeup[h] = mLevelMakeup;
            c.powerMakeup[h] = mPowerMakeupSmoothed;
            c.RecordClippers(h, mClippers, change);

            // Each step depends only on the one before, so once one moves nothing, nothing
            // moves for the rest of the chunk: settled controls cost a step, not 64.
            if (!stagesMoved && Smoothed() == before)
            {
                c.HoldFrom(h);
                break;
            }
        }
    }

    // Every smoothed control, which together with the targets decides the next step.
    [[nodiscard]] std::array<float, 9> Smoothed() const
    {
        return {mVoiceSmoothed, mGainSmoothed,      mStageGainSmoothed,  mPowerDriveSmoothed, mSagSmoothed,
                mBiasSmoothed,  mCharacterSmoothed, mOutputGainSmoothed, mPowerMakeupSmoothed};
    }

    /**
     * Everything derived from the smoothed controls that is the same for both
     * channels and all four oversampled steps, worked out once per host sample
     * instead of once per channel per stage. Says what moved in the clippers.
     */
    builtin_amp::ClipperChange UpdateVoicing(bool stagesMoved = false)
    {
        // Each group is only refreshed while its controls are moving; the
        // glides settle exactly on their targets, so a static setting costs a
        // handful of compares here, not tanh and exp2 every sample.
        const bool kneeMoved = mVoicingStale || mCharacterSmoothed != mVoicedCharacter;
        builtin_amp::ClipperChange change;

        if (kneeMoved)
        {
            mVoicedCharacter = mCharacterSmoothed;
            mClippers.SetCharacter(mCharacterSmoothed);
            change.knee = true;
        }

        if (kneeMoved || mPowerDriveSmoothed != mVoicedPowerDrive || mBiasSmoothed != mVoicedBias)
        {
            mVoicedPowerDrive = mPowerDriveSmoothed;
            mVoicedBias = mBiasSmoothed;
            mClippers.SetPowerStage(mPowerDriveSmoothed, mBiasSmoothed);
            change.power = true;
        }

        if (kneeMoved || stagesMoved || mGainSmoothed != mVoicedGain || mVoiceSmoothed != mVoicedVoice)
        {
            mVoicedGain = mGainSmoothed;
            mVoicedVoice = mVoiceSmoothed;
            mLevelMakeup = mStageFades.Makeup(mGainSmoothed, mVoiceSmoothed, mCharacterSmoothed);
        }

        mVoicingStale = false;
        return change;
    }

    static float DbToLinear(double db)
    {
        return static_cast<float>(std::pow(10.0, db * 0.05));
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
            mPath.stageFilters[stage].SetCorners(mDspSampleRate, builtin_amp::kStageHighPass[stage] * highPassScale,
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
        mPath.filters[builtin_amp::kVoicingPeak].SetTarget(
            builtin_amp::DesignPeaking(1800.0, 0.8, gainDb, mDspSampleRate));
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
            mPath.sagAttack = 0.0f;
            mPath.sagRelease = 0.0f;
            return;
        }

        const double attackTau = 0.01;  // 10 ms
        const double releaseTau = 0.18; // 180 ms
        mPath.sagAttack = static_cast<float>(std::exp(-1.0 / (attackTau * mDspSampleRate)));
        mPath.sagRelease = static_cast<float>(std::exp(-1.0 / (releaseTau * mDspSampleRate)));
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

        mPath.filters[kBassShelf].SetTarget(DesignLowShelf(120.0, 0.8, bassGain, mDspSampleRate));
        mPath.filters[kMiddlePeak].SetTarget(DesignPeaking(750.0, 0.9, midGain, mDspSampleRate));
        mPath.filters[kContourPeak].SetTarget(DesignPeaking(600.0, 0.7, contourGain, mDspSampleRate));
        mPath.filters[kTrebleShelf].SetTarget(DesignHighShelf(3500.0, 0.9, trebleGain, mDspSampleRate));
        mPath.filters[kPresencePeak].SetTarget(DesignPeaking(4000.0, 1.2, presenceGain, mDspSampleRate));
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
        mPath.filters[builtin_amp::kPreHighPass].SetTarget(
            builtin_amp::DesignHighPass(60.0 * scale, 0.707, mDspSampleRate));
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

        mPath.filters[builtin_amp::kPreEmphasisShelf].SetTarget(
            builtin_amp::DesignHighShelf(2500.0, 0.8, gainDb, mDspSampleRate));
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

        mPath.filters[kDepthShelf].SetTarget(DesignLowShelf(120.0, 0.9, depthGain, mDspSampleRate));
        mPath.filters[kResonancePeak].SetTarget(DesignPeaking(120.0, 1.0, resonanceGain, mDspSampleRate));
        mPath.filters[kDampingShelf].SetTarget(DesignHighShelf(3500.0, 0.9, dampingGain, mDspSampleRate));
    }

    void UpdatePostFilters()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        mPath.filters[builtin_amp::kPostHighPass].SetTarget(builtin_amp::DesignHighPass(25.0, 0.707, mDspSampleRate));
    }

    double mSampleRate = 44100.0;
    double mDspSampleRate = 44100.0;
    int mMaxBlockSize = 0;
    int mOversamplingFactor = 1;
    builtin_amp::SignalPath mPath;
    builtin_amp::ChunkControls mControls;
    builtin_amp::StageFades mStageFades;
    builtin_amp::NominalLevel mNominal;

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
