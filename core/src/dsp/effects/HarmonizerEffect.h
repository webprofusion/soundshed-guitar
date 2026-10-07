#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/MusicalScale.h"
#include "dsp/ScaleNoteFollower.h"
#include "dsp/SpliceTransposer.h"
#include "dsp/effects/DelayLineSupport.h"
#include "dsp/effects/HarmonizerSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace guitarfx
{
/**
 * Harmonizer: up to four pitch-shifted voices played alongside the guitar.
 *
 *   in -+-------------------------------------------------- x Dry ----------+- out
 *       +- ScaleNoteFollower (Scale mode) -> note -> each voice's shift      |
 *       +- voice 1..4: SpliceTransposer - delay - level, pan -+- High Cut - x Harmony
 *
 * Each voice is its own SpliceTransposer, the engine Transpose and Pitch Shift's Low Latency
 * mode use, so a voice follows a new interval on the next sample, holds bass notes in tune and
 * shifts chords cleanly (docs/transpose-engine.md). Its window is kVoiceWindowSeconds, longer
 * than Transpose's, so a Clean voice can start from a pick that far back; it plays about 21 ms
 * behind the guitar on average, as a second player would. The dry signal is not delayed, and the
 * effect reports no latency, so a harmony never makes the guitar itself late.
 *
 * Modes:
 *   - Scale: the voices are set in scale steps (a 3rd up, a 6th down) in Key and Scale. The note
 *     being played is followed by ScaleNoteFollower (dsp/ScaleNoteFollower.h), snapped to the
 *     nearest note of the scale, and every voice moves by the semitones that interval is from
 *     that note in the key (dsp/MusicalScale.h), so a 3rd up is major or minor as the key has it.
 *     It follows single-note lines. A chord or a palm-muted chug often has no pitch to find, and
 *     keeps the interval the last note had. Until a first note is found the voices are silent.
 *   - Fixed: each voice moves by its own Semitones whatever is played, chords included.
 *
 * Tracking, in Scale mode. A new note's pitch is known 10-30 ms after its pick (the lower the
 * note, the later), and until then the voices would play the new note at the old note's
 * interval: a short wrong harmony at every note that changes it.
 *   - Clean ducks the voices under each pick (2 ms) and, once the note is known, starts them
 *     from just before the pick (SpliceTransposer::EngageAt), so the harmony enters with its own
 *     pick as far behind the guitar's. A pick whose note is not found within kPickWaitSeconds
 *     starts them from the pick at the intervals they had, so a wait is never longer than that.
 *   - Fast never ducks: the voices play from the pick at the old interval and move when the
 *     pitch arrives. Legato notes (no pick) move the same way in both.
 * Glide slides a voice between intervals; a bend moves the harmony once it is nearer the next
 * scale note than its own by ScaleNoteFollower::kMarginSemitones.
 *
 * Per voice: Level, Pan (the mid panned at equal power; any side the input already had narrows
 * as it goes), Detune in cents, and Delay, for a voice that plays behind like a double-tracked
 * part. Humanize gives every voice a slow random drift in pitch and timing of its own. High Cut
 * smooths the harmony only.
 *
 * Every voice's history is written on every sample, on or off, so a voice that starts plays
 * what is coming in now. Nothing allocates or locks after Prepare(); SetParam only stores the
 * value, and Process takes it up at the start of the next block.
 */
class HarmonizerEffect : public EffectProcessor
{
  public:
    HarmonizerEffect()
    {
        mValues = DefaultParamValues(harmonizer::kParams);
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mFollower.Prepare(sampleRate);
        mFollower.SetTimeoutSeconds(harmonizer::kPickWaitSeconds);
        mAppliedLowestNote = -1;

        const auto delayCapacity =
            static_cast<std::size_t>(
                std::ceil((harmonizer::kMaxVoiceDelayMs + harmonizer::kHumanizeDelayMs) * 0.001 * sampleRate)) +
            8;

        for (std::size_t v = 0; v < mVoices.size(); ++v)
        {
            Voice& voice = mVoices[v];
            voice.shifter.Prepare(sampleRate);
            voice.shifter.SetWindowSeconds(harmonizer::kVoiceWindowSeconds);
            voice.delayL.Resize(delayCapacity);
            voice.delayR.Resize(delayCapacity);
            voice.glide.Prepare(sampleRate);
            voice.delay.Prepare(sampleRate);
            voice.delay.SetTimeConstantMs(harmonizer::kControlSmoothingMs);
            voice.delay.SetMaxStep(delay_line::kMaxGlideStep);
            voice.driftDelay.Prepare(sampleRate);
            voice.driftDelay.SetMaxStep(harmonizer::kHumanizeDelayMaxStep);
            voice.driftCents.Prepare(sampleRate);
            voice.driftCents.SetTimeConstantMs(harmonizer::kHumanizeSmoothingSeconds * 1000.0);
            voice.rng.Seed(static_cast<std::uint32_t>(v) * 7919u + 17u);
        }

        mVoiceFadeStep = FadeStep(harmonizer::kVoiceFadeSeconds);
        mDuckFadeStep = FadeStep(harmonizer::kDuckFadeSeconds);
        mReturnFadeStep = FadeStep(harmonizer::kReturnFadeSeconds);
        mSmoothing = static_cast<float>(1.0 - std::exp(-1000.0 / (harmonizer::kControlSmoothingMs * sampleRate)));
        mPreRollSamples = harmonizer::kPickPreRollSeconds * sampleRate;
        mHumanizeIntervalSamples =
            std::max(1, static_cast<int>(std::lround(harmonizer::kHumanizeIntervalSeconds * sampleRate)));
        mPrepared = true;
        Reset();
    }

    void Reset() override
    {
        if (!mPrepared)
        {
            return;
        }

        mFollower.Reset();
        mNote = -1;
        mDucked = false;
        mToneState = {};
        mStarted = false;

        for (Voice& voice : mVoices)
        {
            voice.shifter.Reset();
            voice.delayL.Clear();
            voice.delayR.Clear();
            voice.running = false;
            voice.restart = false;
            voice.gain = 0.0f;
            voice.driftCountdown = 0;
            voice.driftDelay.Snap(0.0);
            voice.driftCents.Snap(0.0);
        }
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        if (!mPrepared)
        {
            CopyStereoInputToOutput(inputs, outputs, numSamples);
            return;
        }

        numSamples = std::min(numSamples, mMaxBlockSize);
        const float* inL = inputs[0] ? inputs[0] : inputs[1];
        const float* inR = inputs[1] ? inputs[1] : inL;
        TakeUpParams(numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const float rawL = inL ? inL[i] : 0.0f;
            const float rawR = inR ? inR[i] : 0.0f;
            const float l = IsFinite(rawL) ? rawL : 0.0f;
            const float r = IsFinite(rawR) ? rawR : 0.0f;

            if (mScaleMode)
            {
                FollowNote(0.5f * (l + r));
            }

            float wetL = 0.0f;
            float wetR = 0.0f;

            for (Voice& voice : mVoices)
            {
                RenderVoice(voice, l, r, wetL, wetR);
            }

            const auto toneL = static_cast<float>(mToneState[0].Process(mTone.current, wetL));
            const auto toneR = static_cast<float>(mToneState[1].Process(mTone.current, wetR));
            mTone.Advance();
            mDry += mSmoothing * (mDryTarget - mDry);
            mHarmonyGain += mSmoothing * (mHarmonyTarget - mHarmonyGain);

            if (outputs[0])
            {
                outputs[0][i] = l * mDry + toneL * mHarmonyGain;
            }

            if (outputs[1])
            {
                outputs[1][i] = r * mDry + toneR * mHarmonyGain;
            }
        }

        mTone.Finish();

        // A NaN that got into a filter would otherwise stay there for good.
        if (!IsFinite(mToneState[0].s1) || !IsFinite(mToneState[1].s1))
        {
            mToneState = {};
        }
    }

    // Can run on the audio thread (MIDI and DAW automation): it stores the value and nothing else.
    void SetParam(const std::string& key, double value) override
    {
        if (!IsFinite(value))
        {
            return;
        }

        const std::size_t index = FindParamSpec(harmonizer::kParams, key);

        if (index != harmonizer::kParamCount)
        {
            mValues[index] = NormaliseParamValue(harmonizer::kParams[index], value);
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (const std::size_t index = FindParamSpec(harmonizer::kParams, key); index != harmonizer::kParamCount)
        {
            return mValues[index];
        }

        // Read-only state, for tests and any future display.
        if (key == "note")
        {
            return static_cast<double>(mNote);
        }

        for (int v = 0; v < harmonizer::kVoiceCount; ++v)
        {
            static constexpr const char* kShiftKeys[] = {"voice1Shift", "voice2Shift", "voice3Shift", "voice4Shift"};

            if (key == kShiftKeys[v])
            {
                return mVoices[static_cast<std::size_t>(v)].shift;
            }
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "harmonizer";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "pitch";
    }

    /// The guitar itself is not delayed, so nothing downstream needs compensating; the voices
    /// play behind it by design.
    [[nodiscard]] int GetLatencySamples() const override
    {
        return 0;
    }

  private:
    struct Voice
    {
        SpliceTransposer shifter;
        delay_line::FractionalDelayLine delayL;
        delay_line::FractionalDelayLine delayR;
        delay_line::GlideRamp glide;      ///< the interval, in semitones, when Glide is set
        delay_line::GlideRamp delay;      ///< the voice delay, in samples
        delay_line::GlideRamp driftDelay; ///< Humanize's timing drift, in samples
        delay_line::GlideRamp driftCents; ///< Humanize's pitch drift
        delay_line::Rng rng;
        int driftCountdown = 0;

        bool on = false;
        bool running = false; ///< the shifter is being played: the voice is heard or fading
        bool restart = false; ///< start the shifter again, from restartDelay, on the next sample
        double restartDelay = 0.0;
        float gain = 0.0f; ///< the on/off and duck fade
        float fadeStep = 0.0f;

        double shift = 0.0; ///< the interval, in semitones, before detune and drift
        double detune = 0.0;
        float level = 1.0f;
        float levelTarget = 1.0f;
        float panL = 1.0f;
        float panR = 1.0f;
        float side = 1.0f;
        float panLTarget = 1.0f;
        float panRTarget = 1.0f;
        float sideTarget = 1.0f;
    };

    [[nodiscard]] double Value(std::size_t index) const
    {
        return mValues[index];
    }

    [[nodiscard]] int Choice(std::size_t index) const
    {
        return static_cast<int>(std::lround(mValues[index]));
    }

    [[nodiscard]] float FadeStep(double seconds) const
    {
        return 1.0f / std::max(1.0f, static_cast<float>(seconds * mSampleRate));
    }

    /// Everything the parameters ask for, once per block. SetParam lands between blocks, so
    /// nothing finer is lost.
    void TakeUpParams(int numSamples)
    {
        using namespace harmonizer;

        const bool scaleMode = static_cast<Mode>(Choice(kMode)) == Mode::Scale;

        // Entering Scale mode starts from a clean tracker, so a note from before cannot be heard.
        if (scaleMode && !mScaleMode)
        {
            mFollower.Reset();
            mNote = -1;
            mDucked = false;
        }

        mScaleMode = scaleMode;
        mClean = static_cast<Tracking>(Choice(kTracking)) == Tracking::Clean;
        mKey = std::clamp(Choice(kKey), 0, music::kPitchClasses - 1);
        mScale = static_cast<music::Scale>(std::clamp(Choice(kScale), 0, static_cast<int>(music::Scale::Count) - 1));

        mFollower.SetScale(mKey, mScale);

        if (const int lowest = Choice(kLowestNote); lowest != mAppliedLowestNote)
        {
            mAppliedLowestNote = lowest;
            mFollower.SetLowestFrequency(pitch_tracker::LowestNoteHz(lowest));
            mNote = mScaleMode ? -1 : mNote;
        }

        const double glideMs = Value(kGlide);
        mHumanize = Value(kHumanize);
        const double samplesPerMs = 0.001 * mSampleRate;

        for (int v = 0; v < kVoiceCount; ++v)
        {
            Voice& voice = mVoices[static_cast<std::size_t>(v)];
            voice.on = Value(VoiceParam(v, kOn)) >= 0.5;
            voice.detune = Value(VoiceParam(v, kDetune)) / 100.0;
            voice.levelTarget = static_cast<float>(std::pow(10.0, Value(VoiceParam(v, kLevel)) / 20.0));
            voice.delay.SetTarget(Value(VoiceParam(v, kDelay)) * samplesPerMs);
            voice.glide.SetTimeConstantMs(glideMs);

            // The mid at equal power, scaled so the centre is unity on both sides.
            const double pan = Value(VoiceParam(v, kPan));
            const double angle = (pan + 1.0) * 0.25 * biquad::kPi;
            voice.panLTarget = static_cast<float>(std::cos(angle) * 1.4142135623730951);
            voice.panRTarget = static_cast<float>(std::sin(angle) * 1.4142135623730951);
            voice.sideTarget = static_cast<float>(1.0 - std::abs(pan));

            UpdateShift(voice, v, false);

            if (!mStarted)
            {
                voice.level = voice.levelTarget;
                voice.panL = voice.panLTarget;
                voice.panR = voice.panRTarget;
                voice.side = voice.sideTarget;
                voice.delay.Snap(voice.delay.Target());
            }
        }

        mDryTarget = static_cast<float>(Value(kDry));
        mHarmonyTarget = static_cast<float>(std::pow(10.0, Value(kHarmonyLevel) / 20.0));

        const double cutoff = Value(kHighCut);
        const double ceiling = kMaxHighCutFraction * mSampleRate;
        mTone.target = cutoff >= kHighCutOffHz || cutoff >= ceiling
                           ? BiquadCoefficients{1.0, 0.0, 0.0, 0.0, 0.0}
                           : biquad::LowPass(cutoff, biquad::kButterworthQ, mSampleRate);

        if (!mStarted)
        {
            mDry = mDryTarget;
            mHarmonyGain = mHarmonyTarget;
            mTone.current = mTone.target;
            mStarted = true;
        }

        mTone.Begin(numSamples);
    }

    /// The voice's interval for the note being played, or its fixed one. With `snap` it lands at
    /// once, for a voice that starts on a new note; otherwise Glide carries it there.
    void UpdateShift(Voice& voice, int v, bool snap)
    {
        using namespace harmonizer;

        double shift = Value(VoiceParam(v, kSemitones));

        if (mScaleMode)
        {
            shift =
                mNote < 0
                    ? voice.glide.Target()
                    : static_cast<double>(music::ScaleStepShift(mNote, mKey, mScale, Choice(VoiceParam(v, kInterval))));
        }

        voice.glide.SetTarget(shift);

        if (snap || !voice.running)
        {
            voice.glide.Snap(shift);
        }
    }

    /// Whether the voice should be heard now.
    [[nodiscard]] bool Audible(const Voice& voice) const
    {
        return voice.on && (!mScaleMode || (mNote >= 0 && !mDucked));
    }

    void UpdateAllShifts(bool snap)
    {
        for (int v = 0; v < harmonizer::kVoiceCount; ++v)
        {
            UpdateShift(mVoices[static_cast<std::size_t>(v)], v, snap);
        }
    }

    /// One sample of the note follower, and what it decides for the voices.
    void FollowNote(float mono)
    {
        mFollower.Push(mono, [this](const ScaleNoteFollower::Event& event) {
            switch (event.type)
            {
            case ScaleNoteFollower::Event::Type::Pick:
                // In Clean, the voices duck until the pick's note is known.
                mDucked = mClean && mNote >= 0;
                break;
            case ScaleNoteFollower::Event::Type::Confirmed: {
                // A voice that waited (ducked in Clean, or for the first note in either) starts
                // from just before the pick, at the new note's interval.
                const bool waited = mNote < 0 || mDucked;
                mNote = event.note;
                mDucked = false;
                UpdateAllShifts(true);

                if (waited)
                {
                    RestartVoices(static_cast<double>(event.samplesSincePick) + mPreRollSamples);
                }

                break;
            }
            case ScaleNoteFollower::Event::Type::TimedOut:
                // No note found in time (a chord, a muted chug): from the pick, at the intervals there are.
                if (mDucked)
                {
                    mDucked = false;
                    RestartVoices(static_cast<double>(event.samplesSincePick) + mPreRollSamples);
                }

                break;
            case ScaleNoteFollower::Event::Type::Moved: {
                // Legato, a bend, or a late confirmation: the voices carry on and glide there.
                const bool first = mNote < 0;
                mNote = event.note;
                UpdateAllShifts(first);
                break;
            }
            }
        });
    }

    /// Starts every voice that is on again at its new interval, `delaySamples` behind the input,
    /// once it has faded out; a voice still fading carries on and simply moves.
    void RestartVoices(double delaySamples)
    {
        for (Voice& voice : mVoices)
        {
            if (!voice.on)
            {
                continue;
            }

            if (voice.gain <= 0.0f || !voice.running)
            {
                voice.restart = true;
                voice.restartDelay = delaySamples;
            }
        }
    }

    void RenderVoice(Voice& voice, float l, float r, float& wetL, float& wetR)
    {
        // Written on every sample, so a voice starts on what is being played now.
        voice.shifter.Write(l, r);

        const bool audible = Audible(voice);
        const float target = audible ? 1.0f : 0.0f;

        if (!voice.running && !audible)
        {
            voice.restart = false;
            voice.delayL.Write(0.0f);
            voice.delayR.Write(0.0f);
            return;
        }

        const double drift = Humanize(voice);
        const double semitones = voice.glide.Next() + voice.detune + drift * harmonizer::kHumanizeCents / 100.0;
        voice.shift = voice.glide.Value();

        if (!voice.running || voice.restart)
        {
            voice.shifter.SetSemitones(semitones, false);

            if (voice.restart && voice.restartDelay > 0.0)
            {
                voice.shifter.EngageAt(voice.restartDelay);
                voice.fadeStep = mReturnFadeStep;
            }
            else
            {
                voice.shifter.Engage();
                voice.fadeStep = mVoiceFadeStep;
            }

            voice.running = true;
            voice.restart = false;
            voice.gain = 0.0f;
        }
        else
        {
            voice.shifter.SetSemitones(semitones, true);
        }

        float shiftedL = 0.0f;
        float shiftedR = 0.0f;
        voice.shifter.Process(shiftedL, shiftedR);

        voice.delayL.Write(shiftedL);
        voice.delayR.Write(shiftedR);
        // Read even at no delay, where it is the line's two-sample minimum: switching between the
        // line and the shifter itself would step when Humanize's drift starts or ends.
        const double delay = voice.delay.Next() + voice.driftDelay.Value();
        shiftedL = voice.delayL.ReadHermite(delay);
        shiftedR = voice.delayR.ReadHermite(delay);

        voice.level += mSmoothing * (voice.levelTarget - voice.level);
        voice.panL += mSmoothing * (voice.panLTarget - voice.panL);
        voice.panR += mSmoothing * (voice.panRTarget - voice.panR);
        voice.side += mSmoothing * (voice.sideTarget - voice.side);

        const float mid = 0.5f * (shiftedL + shiftedR);
        const float side = 0.5f * (shiftedL - shiftedR) * voice.side;
        const float gain = voice.gain * voice.level;
        wetL += gain * (mid * voice.panL + side);
        wetR += gain * (mid * voice.panR - side);

        // Ducking is quicker than any other fade, so little of a pick plays at the old interval.
        const float step = target < voice.gain ? (mDucked ? mDuckFadeStep : mVoiceFadeStep) : voice.fadeStep;
        voice.gain = target > voice.gain ? std::min(target, voice.gain + step) : std::max(target, voice.gain - step);

        if (voice.gain <= 0.0f && !audible)
        {
            voice.running = false;
        }
    }

    /// Advances the voice's random drift and returns its pitch part, -1 to 1. The timing part is
    /// left in driftDelay.
    double Humanize(Voice& voice)
    {
        if (--voice.driftCountdown <= 0)
        {
            // A little irregular, so the voices never wander in step.
            voice.driftCountdown = static_cast<int>(static_cast<float>(mHumanizeIntervalSamples) *
                                                    (1.0f + 0.5f * voice.rng.NextBipolar()));
            voice.driftCents.SetTarget(mHumanize * voice.rng.NextBipolar());
            voice.driftDelay.SetTarget(mHumanize * harmonizer::kHumanizeDelayMs * 0.001 * mSampleRate *
                                       (0.5 + 0.5 * voice.rng.NextBipolar()));
        }

        (void)voice.driftDelay.Next();
        return voice.driftCents.Next();
    }

    std::array<double, harmonizer::kParamCount> mValues{};
    std::array<Voice, harmonizer::kVoiceCount> mVoices;
    ScaleNoteFollower mFollower;

    bool mPrepared = false;
    bool mStarted = false;
    bool mScaleMode = true;
    bool mClean = true;
    int mKey = 0;
    music::Scale mScale = music::Scale::Major;
    int mAppliedLowestNote = -1;
    double mHumanize = 0.0;

    int mNote = -1; ///< the note being harmonised, a MIDI note number; -1 before the first
    bool mDucked = false;

    float mVoiceFadeStep = 1.0f / 240.0f;
    float mDuckFadeStep = 1.0f / 96.0f;
    float mReturnFadeStep = 1.0f / 48.0f;
    float mSmoothing = 0.001f;
    double mPreRollSamples = 144.0;
    int mHumanizeIntervalSamples = 16800;

    float mDry = 1.0f;
    float mDryTarget = 1.0f;
    float mHarmonyGain = 1.0f;
    float mHarmonyTarget = 1.0f;
    biquad::Ramp mTone;
    std::array<biquad::State, 2> mToneState{};
};

inline void RegisterHarmonizerEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kHarmonizer;
    info.aliases = {"harmonizer"};
    info.displayName = "Harmonizer";
    info.category = "pitch";
    info.description = "Up to four harmony voices: in a key and scale, a 3rd or a 6th that follows the notes you "
                       "play, or fixed intervals that work on chords. Per-voice level, pan, detune and delay.";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(harmonizer::kParams);
    info.presets = harmonizer::FactoryPresets(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<HarmonizerEffect>(); });
}
} // namespace guitarfx
