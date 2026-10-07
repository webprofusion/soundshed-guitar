#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "dsp/PitchTracker.h"
#include "dsp/TimeDomainPitchShifter.h"
#include "dsp/effects/PitchPresets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

namespace guitarfx
{
/**
 * Auto-Arpeggiator effect.
 *
 * Rhythmically cycles through a semitone interval pattern by pitch-shifting
 * the full guitar signal, producing an arpeggio effect without the need for
 * a separate synthesizer.
 *
 * Step timing is BPM-synced via the requiresTempo injection mechanism:
 * PluginController::ProcessAudio() calls MultiPresetMixer::SetTempo() each
 * block which ultimately calls SetParam("bpm", bpm) on this effect.
 *
 * Audio architecture, all per sample, so a step starts on its own sample in any block size:
 *  - Steps at 0 semitones play the input itself, undelayed.
 *  - Other steps read TimeDomainPitchShifter, a time-domain shifter that takes a new pitch at once.
 *    Its history is written on every sample, so a step that starts it plays what is coming in now.
 *    From one shifted step to the next its tap only changes speed, so the new pitch is heard on
 *    the step's first sample, with no click. A 0 st step and a shifted one cross-fade over 5 ms.
 *  - The gate envelope shapes each step's length and attack, whichever path plays.
 *  - Wet/dry mix controls blend between gated-wet and always-on-dry.
 *
 * Signalsmith Stretch did the shifting until 2026-10. It was not fed during 0 st steps, so the next
 * shifted step replayed the audio from before them: after a chord change, the old chord at full
 * level. Measured at 48 kHz in 64-sample blocks, with that fixed (Stretch re-seeked onto the input
 * history), against this engine:
 *
 *   a new pitch after a shifted step                     40-51 ms     0.1 ms
 *   CPU per block: mean / p99 / max                 11 / 217 / 396 us   2.7 / 20 / 40 us
 *   pitch of a held step, notes E2-E4, -12 to +12 st    up to 30 cents   1.5 cents
 *   latency of a shifted step                               80 ms     2-25 ms (10 nominal)
 *
 * Pitch trigger (Above/Below a threshold): the left input feeds the shared
 * PitchTracker (dsp/PitchTracker.h), and every analysis frame of 2048 samples at
 * 48 kHz (about 43 ms at any rate) the frame's pitch is smoothed and debounced
 * into the arp's on/off state. The pitch used to come from a brute-force YIN run
 * over each 2048-sample frame on the audio thread: a burst of about 340 us in one
 * 64-sample block of every 32, read up to 190 cents sharp (it took the first whole-
 * sample period under its threshold, not the bottom of the dip, so a note a
 * semitone below the threshold tripped it), and no pitch at all at 96 kHz and
 * above, where the longest period no longer fit the frame.
 */
class AutoArpEffect : public EffectProcessor
{
  public:
    /// Identical sides in, identical sides out, whatever the settings (EffectProcessor::CanWiden).
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

        mShifter.Prepare(sampleRate);
        mPathFadeStep = 1.0f / std::max(1.0f, static_cast<float>(sampleRate * kPathFadeSeconds));
        mConfigured = true;
        mRng.seed(std::random_device{}());

        mTracker.Prepare(sampleRate);
        mPitchFrameLength = std::max(1, static_cast<int>(std::lround(sampleRate * kPitchFrameSeconds)));
        mDetectedHz = 0.0;
        mArpActive = true;

        RebuildStepList();
        UpdatePhaseIncrement();

        Reset();
    }

    void Reset() override
    {
        mPhase = 0.0;
        mCurrentStep = 0;
        mCurrentSemitones = mStepSemitones[0];

        if (mConfigured)
        {
            mShifter.Reset();
        }

        StopShifter();
        mDetectedHz = 0.0;
        mSmoothedHz = 0.0;
        mTriggerVote = 0;
        mArpActive = (mPitchMode == 0);
        ResetPitchGate();
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || !mConfigured)
        {
            return;
        }

        numSamples = std::min(numSamples, mMaxBlockSize);

        // Pitch-mode gating — track the pitch, evaluate the trigger condition once per frame.
        if (mPitchMode != 0 && inputs[0])
        {
            // Turned on (again): start from an empty history rather than whatever was playing then.
            if (!mPitchGateRunning)
            {
                ResetPitchGate();
                mPitchGateRunning = true;
            }

            UpdatePitchGate(inputs[0], numSamples);
        }
        else
        {
            mPitchGateRunning = false;
        }

        // When pitch-gated off, pass through dry audio unchanged.
        if (!mArpActive)
        {
            StopShifter();

            for (int i = 0; i < numSamples; ++i)
            {
                const float inL = inputs[0] ? inputs[0][i] : 0.0f;
                const float inR = inputs[1] ? inputs[1][i] : 0.0f;
                // Kept current, so the arp starts on what is being played when it turns on.
                mShifter.Write(inL, inR);

                if (outputs[0])
                {
                    outputs[0][i] = inL;
                }

                if (outputs[1])
                {
                    outputs[1][i] = inR;
                }
            }

            return;
        }

        // Per-sample: apply gate envelope, advance phase, detect step transitions.
        const float dryMix = static_cast<float>(1.0 - mMix);
        const float wetMix = static_cast<float>(mMix);
        // The envelope is judged in double: a phase a hair under 1.0 rounds to 1.0f, which read as
        // past a 100% gate and dropped the last sample of every step to silence, a click.
        const double attackFrac = mAttack;
        const double gateFrac = mGate;
        // Release window starts at gateFrac; clamped so it never overruns phase 1.0
        const double releaseFrac = std::min(static_cast<double>(mRelease), std::max(0.0, 1.0 - gateFrac));
        const double releaseEnd = gateFrac + releaseFrac;

        for (int i = 0; i < numSamples; ++i)
        {
            // Gate envelope: [0, attack) ramp up | [attack, gate) hold | [gate, gate+release) ramp down | silence
            const double phase = mPhase;
            double gateGain = 0.0;

            if (phase < attackFrac)
            {
                gateGain = phase / attackFrac;
            }
            else if (phase < gateFrac)
            {
                gateGain = 1.0;
            }
            else if (releaseFrac > 0.0 && phase < releaseEnd)
            {
                gateGain = 1.0 - (phase - gateFrac) / releaseFrac;
            }

            const float dryL = inputs[0] ? inputs[0][i] : 0.0f;
            const float dryR = inputs[1] ? inputs[1][i] : 0.0f;
            float wetL = dryL;
            float wetR = dryR;
            ShiftSample(dryL, dryR, wetL, wetR);

            const auto wetGain = static_cast<float>(gateGain) * wetMix;

            if (outputs[0])
            {
                outputs[0][i] = dryL * dryMix + wetL * wetGain;
            }

            if (outputs[1])
            {
                outputs[1][i] = dryR * dryMix + wetR * wetGain;
            }

            // Advance phase; on wrap, advance to next step.
            mPhase += mPhaseIncrement;

            if (mPhase >= 1.0)
            {
                mPhase -= 1.0;

                if (mRandomDirection)
                {
                    mCurrentStep = static_cast<int>(mRng() % static_cast<unsigned>(mStepCount));
                }
                else
                {
                    mCurrentStep = (mCurrentStep + 1) % mStepCount;
                }

                if (mRandomPattern)
                {
                    // Fresh random semitone for this step so every note is unpredictable.
                    mCurrentSemitones = RandomSemitones();
                    mStepSemitones[static_cast<size_t>(mCurrentStep)] = mCurrentSemitones;
                }
                else
                {
                    mCurrentSemitones = mStepSemitones[static_cast<size_t>(mCurrentStep)];
                }

                ApplyShift(mCurrentSemitones);
            }
        }
    }

    // Can run on the audio thread (MIDI and DAW automation), so it neither allocates nor throws.
    void SetParam(const std::string& key, double value) override
    {
        // A NaN would reach the int casts below, and a table index read from one.
        if (!IsFinite(value))
        {
            return;
        }

        if (key == "bpm")
        {
            mBpm = std::clamp(value, 30.0, 300.0);
            UpdatePhaseIncrement();
        }
        else if (key == "stepRate")
        {
            mStepRate = static_cast<int>(std::clamp(std::round(value), 0.0, 6.0));
            UpdatePhaseIncrement();
        }
        else if (key == "numSteps")
        {
            mNumSteps = static_cast<int>(std::clamp(std::round(value), 2.0, 8.0));
            RebuildStepList();
        }
        else if (key == "pattern")
        {
            mPattern = static_cast<int>(std::clamp(std::round(value), 0.0, 5.0));
            RebuildStepList();
        }
        else if (key == "direction")
        {
            mDirection = static_cast<int>(std::clamp(std::round(value), 0.0, 2.0));
            RebuildStepList();
        }
        else if (key == "gate")
        {
            mGate = static_cast<float>(std::clamp(value, 0.05, 1.0));
        }
        else if (key == "attack")
        {
            mAttack = static_cast<float>(std::clamp(value, 0.0, 0.5));
        }
        else if (key == "release")
        {
            mRelease = static_cast<float>(std::clamp(value, 0.0, 0.5));
        }
        else if (key == "mix")
        {
            mMix = std::clamp(value, 0.0, 1.0);
        }
        else if (key == "pitchMode")
        {
            mPitchMode = static_cast<int>(std::clamp(std::round(value), 0.0, 2.0));

            if (mPitchMode == 0)
            {
                mArpActive = true;
            }
        }
        else if (key == "pitchThreshold")
        {
            mPitchThreshold = std::clamp(value, 50.0, 2000.0);
        }
        else if (const int idx = CustomStepIndex(key); idx >= 0)
        {
            mCustomSteps[static_cast<size_t>(idx)] = static_cast<int>(std::clamp(std::round(value), -24.0, 24.0));

            if (mPattern == kPatternCustom)
            {
                RebuildStepList();
            }
        }
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "bpm")
        {
            return mBpm;
        }

        if (key == "stepRate")
        {
            return static_cast<double>(mStepRate);
        }

        if (key == "numSteps")
        {
            return static_cast<double>(mNumSteps);
        }

        if (key == "pattern")
        {
            return static_cast<double>(mPattern);
        }

        if (key == "direction")
        {
            return static_cast<double>(mDirection);
        }

        if (key == "gate")
        {
            return mGate;
        }

        if (key == "attack")
        {
            return mAttack;
        }

        if (key == "release")
        {
            return mRelease;
        }

        if (key == "mix")
        {
            return mMix;
        }

        if (key == "pitchMode")
        {
            return static_cast<double>(mPitchMode);
        }

        if (key == "pitchThreshold")
        {
            return mPitchThreshold;
        }

        if (const int idx = CustomStepIndex(key); idx >= 0)
        {
            return static_cast<double>(mCustomSteps[static_cast<size_t>(idx)]);
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "arp_auto";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "modulation";
    }

    [[nodiscard]] int GetLatencySamples() const override
    {
        // A shifted step plays 2-25 ms behind its input as the tap drifts and splices; this is the
        // engine's nominal 10 ms. Steps at 0 st and the dry mix are undelayed (variable by design).
        if (!mConfigured)
        {
            return 0;
        }

        return mShifter.NominalLatencySamples();
    }

  private:
    // ── Pattern table ─────────────────────────────────────────────────────
    static constexpr int kMaxCustomSteps = 8;
    static constexpr int kMaxResolvedSteps = 2 * kMaxCustomSteps - 2; // eight steps, Up-Down
    static constexpr double kPathFadeSeconds = 0.005;                 // input <-> shifter hand-over
    static constexpr int kPatternCustom = 4;
    static constexpr int kPatternRandom = 5;
    /// The trigger is judged once per frame of this length: 2048 samples at 48 kHz, the window the old
    /// detector analysed in one go, so the debounce and smoothing below keep their timing.
    static constexpr double kPitchFrameSeconds = 2048.0 / 48000.0;
    static constexpr double kSilenceRmsSquared = 9.0e-6; // a frame under RMS 0.003 has no pitch
    static constexpr int kActivateFrames = 2;            // consecutive on-windows needed to activate
    static constexpr int kDeactivateFrames = 5;          // consecutive off-windows needed to deactivate
    static constexpr double kPitchEmaAlpha = 0.6;        // EMA weight for new pitch measurement

    // Base patterns: [pattern][step], -1 terminates the list
    static constexpr int kPatternTable[5][9] = {
        {0, 4, 7, 12, -1, -1, -1, -1, -1},    // 0: Major Triad
        {0, 3, 7, 12, -1, -1, -1, -1, -1},    // 1: Minor Triad
        {0, 7, 12, -1, -1, -1, -1, -1, -1},   // 2: Power Chord
        {0, 12, -1, -1, -1, -1, -1, -1, -1},  // 3: Octaves
        {-1, -1, -1, -1, -1, -1, -1, -1, -1}, // 4: Custom (use mCustomSteps / mNumSteps)
    };

    // Beats per step for each stepRate enum value (0-6).
    // Based on a common 4/4 meter reference beat (quarter note = 1 beat).
    static constexpr double kBeatFractions[7] = {
        1.0,        // 0: 1/4  note  = 1 beat
        0.5,        // 1: 1/8  note  = 0.5 beats
        0.25,       // 2: 1/16 note  = 0.25 beats
        0.125,      // 3: 1/32 note  = 0.125 beats
        1.0 / 3.0,  // 4: 1/8  triplet = 1/3 beat
        1.0 / 6.0,  // 5: 1/16 triplet = 1/6 beat
        1.0 / 12.0, // 6: 1/32 triplet = 1/12 beat
    };

    // ── Helpers ───────────────────────────────────────────────────────────

    // Compute phase increment (per sample) for the current BPM and step rate.
    void UpdatePhaseIncrement()
    {
        const double beatsPerStep = kBeatFractions[static_cast<size_t>(mStepRate)];
        const double secondsPerBeat = 60.0 / std::max(1.0, mBpm);
        const double stepSeconds = secondsPerBeat * beatsPerStep;
        const double stepSamples = stepSeconds * std::max(1.0, mSampleRate);
        mPhaseIncrement = 1.0 / std::max(1.0, stepSamples);
    }

    // Rebuild the resolved step semitone list from pattern/direction/numSteps. Runs from SetParam,
    // so on the audio thread too: fixed arrays, no allocation.
    void RebuildStepList()
    {
        std::array<int, kMaxCustomSteps> base{};
        int count = 0;

        mRandomPattern = (mPattern == kPatternRandom);
        mRandomDirection = mRandomPattern; // Random pattern always picks steps randomly

        if (mPattern == kPatternRandom)
        {
            // Populate the pool with mNumSteps random semitones in [-12, +12].
            // Steps are re-randomized individually on each advance in Process().
            for (; count < mNumSteps && count < kMaxCustomSteps; ++count)
            {
                base[static_cast<size_t>(count)] = RandomSemitones();
            }
        }
        else if (mPattern == kPatternCustom)
        {
            for (; count < mNumSteps && count < kMaxCustomSteps; ++count)
            {
                base[static_cast<size_t>(count)] = mCustomSteps[static_cast<size_t>(count)];
            }
        }
        else
        {
            const auto& row = kPatternTable[static_cast<size_t>(mPattern)];

            for (; count < kMaxCustomSteps && row[count] >= 0; ++count)
            {
                base[static_cast<size_t>(count)] = row[count];
            }
        }

        // Direction (ignored for Random pattern — steps are always picked randomly):
        // Up as listed, Down reversed, Up-Down there and back without repeating the ends.
        const bool down = !mRandomPattern && mDirection == 1;
        mStepCount = 0;

        for (int i = 0; i < count; ++i)
        {
            mStepSemitones[static_cast<size_t>(mStepCount++)] = base[static_cast<size_t>(down ? count - 1 - i : i)];
        }

        if (!mRandomPattern && mDirection == 2)
        {
            for (int i = count - 2; i >= 1; --i)
            {
                mStepSemitones[static_cast<size_t>(mStepCount++)] = base[static_cast<size_t>(i)];
            }
        }

        if (mStepCount == 0)
        {
            mStepSemitones[0] = 0;
            mStepCount = 1;
        }

        // Clamp current step index to new list size
        mCurrentStep = mCurrentStep % mStepCount;
        mCurrentSemitones = mStepSemitones[static_cast<size_t>(mCurrentStep)];
        ApplyShift(mCurrentSemitones);
    }

    [[nodiscard]] int RandomSemitones()
    {
        return static_cast<int>(mRng() % 25u) - 12;
    }

    /// The index in "step0".."step7", or -1 for any other key. A plain character check: the old
    /// std::stoi threw on an undeclared key such as "stepMode", on the audio thread.
    [[nodiscard]] static int CustomStepIndex(std::string_view key) noexcept
    {
        if (key.size() != 5 || !key.starts_with("step") || key[4] < '0' || key[4] >= '0' + kMaxCustomSteps)
        {
            return -1;
        }

        return key[4] - '0';
    }

    // One sample of the wet path: the input on a 0 st step, the shifter on any other, and a
    // cross-fade between them as one hands over to the other.
    void ShiftSample(float inL, float inR, float& wetL, float& wetR)
    {
        // Written on every sample, so the shifter starts on what is being played now.
        mShifter.Write(inL, inR);
        const bool shifted = mCurrentSemitones != 0;

        if (shifted && !mShifterRunning)
        {
            mShifter.SetSemitones(static_cast<double>(mCurrentSemitones), false);
            mShifter.Engage();
            mShifterRunning = true;
        }

        if (!mShifterRunning)
        {
            return;
        }

        float shiftedL = 0.0f;
        float shiftedR = 0.0f;
        mShifter.Process(shiftedL, shiftedR);
        wetL += (shiftedL - inL) * mShifterGain;
        wetR += (shiftedR - inR) * mShifterGain;

        mShifterGain =
            shifted ? std::min(1.0f, mShifterGain + mPathFadeStep) : std::max(0.0f, mShifterGain - mPathFadeStep);
        mShifterRunning = shifted || mShifterGain > 0.0f;
    }

    // A new step's interval. The shifter takes it at once, as a change of speed; a 0 st step
    // leaves it at the old interval while it fades out.
    void ApplyShift(int semitones)
    {
        if (semitones != 0)
        {
            mShifter.SetSemitones(static_cast<double>(semitones), false);
        }
    }

    void StopShifter()
    {
        mShifterRunning = false;
        mShifterGain = 0.0f;
    }

    void ResetPitchGate()
    {
        mTracker.Reset();
        mDetectionsSeen = 0;
        mFrameSamples = 0;
        mFrameEnergy = 0.0;
        mFramePitchHz = 0.0;
    }

    // Feeds the pitch tracker and, at the end of each frame, updates the trigger with the frame's
    // pitch: the tracker's accepted pitch from its latest detection in the frame that found one, or
    // none when no detection did or the frame is quieter than RMS 0.003. No allocation, no locks.
    void UpdatePitchGate(const float* input, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float x = input[i];
            mTracker.Push(x);
            mFrameEnergy += static_cast<double>(x) * static_cast<double>(x);

            if (mTracker.DetectionCount() != mDetectionsSeen)
            {
                mDetectionsSeen = mTracker.DetectionCount();

                if (mTracker.HasPitch() && mTracker.FrequencyHz() > 0.0)
                {
                    mFramePitchHz = mTracker.FrequencyHz();
                }
            }

            if (++mFrameSamples >= mPitchFrameLength)
            {
                const bool audible = mFrameEnergy >= kSilenceRmsSquared * static_cast<double>(mFrameSamples);
                UpdateTrigger(audible ? mFramePitchHz : 0.0);
                mFrameSamples = 0;
                mFrameEnergy = 0.0;
                mFramePitchHz = 0.0;
            }
        }
    }

    // One frame's pitch (0 for none) into the smoothed pitch and the debounced on/off state.
    void UpdateTrigger(double frameHz)
    {
        mDetectedHz = frameHz;

        // EMA smoothing — update only when a confident pitch is detected;
        // during silence let the smoothed value decay gently.
        if (mDetectedHz > 0.0)
        {
            mSmoothedHz = kPitchEmaAlpha * mDetectedHz + (1.0 - kPitchEmaAlpha) * mSmoothedHz;
        }
        else
        {
            mSmoothedHz *= 0.80; // decay toward zero on silence
        }

        const bool conditionMet =
            (mPitchMode == 1) ? (mSmoothedHz > mPitchThreshold) : (mSmoothedHz > 20.0 && mSmoothedHz < mPitchThreshold);

        // Asymmetric debounce: fewer windows needed to activate than to release,
        // so the arp latches on quickly but doesn't drop out on brief dips.
        if (conditionMet)
        {
            mTriggerVote = std::max(0, mTriggerVote) + 1;

            if (mTriggerVote >= kActivateFrames && !mArpActive)
            {
                // Reset to beat-start on fresh activation.
                // The shifter stopped while the arp was off, and starts again on the history kept.
                mPhase = 0.0;
                mCurrentStep = 0;
                mCurrentSemitones = mStepSemitones[0];
                ApplyShift(mCurrentSemitones);
                mArpActive = true;
            }
        }
        else
        {
            mTriggerVote = std::min(0, mTriggerVote) - 1;

            if (mTriggerVote <= -kDeactivateFrames)
            {
                mArpActive = false;
            }
        }
    }

    // ── Parameters ────────────────────────────────────────────────────────
    double mBpm = 120.0;
    int mStepRate = 1;  // 0=1/4, 1=1/8, 2=1/16, 3=1/32, 4=1/8T, 5=1/16T, 6=1/32T
    int mNumSteps = 4;  // active steps in Custom and Random
    int mPattern = 0;   // 0=Major, 1=Minor, 2=Power, 3=Octaves, 4=Custom, 5=Random
    int mDirection = 0; // 0=Up, 1=Down, 2=UpDown
    float mGate = 0.8f;
    float mAttack = 0.05f;
    float mRelease = 0.08f; // short fade-out to avoid clicks at gate close
    int mCustomSteps[kMaxCustomSteps] = {0, 4, 7, 12, 0, 0, 0, 0};
    double mMix = 0.8;
    int mPitchMode = 0;             // 0=Always, 1=Above threshold, 2=Below threshold
    double mPitchThreshold = 330.0; // Hz — default E4 (open high E string)

    // ── Internal state ────────────────────────────────────────────────────
    double mSampleRate = 48000.0;
    int mMaxBlockSize = 512;
    bool mConfigured = false;
    double mPhase = 0.0;
    double mPhaseIncrement = 0.0;
    int mCurrentStep = 0;
    int mCurrentSemitones = 0;
    bool mRandomPattern = false;
    bool mRandomDirection = false;
    std::mt19937 mRng;
    std::array<int, kMaxResolvedSteps> mStepSemitones{}; // the pattern as played, mStepCount long
    int mStepCount = 1;
    TimeDomainPitchShifter mShifter;
    bool mShifterRunning = false; // a shifted step is playing, or one is fading out
    float mShifterGain = 0.0f;    // 0 = the input, 1 = the shifter
    float mPathFadeStep = 1.0f / 240.0f;
    // Pitch detection state
    PitchTracker mTracker;
    std::uint64_t mDetectionsSeen = 0;
    bool mPitchGateRunning = false; // the tracker is being fed (a pitch trigger mode is on)
    int mPitchFrameLength = 2048;   // samples per trigger evaluation
    int mFrameSamples = 0;          // samples into the current frame
    double mFrameEnergy = 0.0;      // sum of squares over the current frame
    double mFramePitchHz = 0.0;     // the current frame's pitch so far, 0 for none
    double mDetectedHz = 0.0;       // the last frame's pitch (Hz)
    double mSmoothedHz = 0.0;       // EMA-smoothed fundamental used for threshold comparison
    int mTriggerVote = 0;           // debounce counter (>0 = consecutive on-frames, <0 = off-frames)
    bool mArpActive = true;         // current pitch-gate state
};

// ── Registration ──────────────────────────────────────────────────────────

inline void RegisterAutoArpEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kAutoArp;
    info.aliases = {"arp_auto"};
    info.displayName = "Auto Arpeggiator";
    info.category = "modulation";
    info.description =
        "BPM-synced rhythmic arpeggiator. Cycles through interval patterns by pitch-shifting the signal each step.";
    info.requiresTempo = true;
    info.parameters = {
        // Step timing
        {"stepRate",
         "Step Rate",
         1.0,
         0.0,
         6.0,
         "enum",
         "timing",
         false,
         1.0,
         {"1/4 Note", "1/8 Note", "1/16 Note", "1/32 Note", "1/8 Triplet", "1/16 Triplet", "1/32 Triplet"}},
        // Pattern
        {"pattern",
         "Pattern",
         0.0,
         0.0,
         5.0,
         "enum",
         "pattern",
         false,
         1.0,
         {"Major Triad", "Minor Triad", "Power Chord", "Octaves", "Custom", "Random"}},
        {"direction", "Direction", 0.0, 0.0, 2.0, "enum", "pattern", false, 1.0, {"Up", "Down", "Up-Down"}},
        {"numSteps", "Steps", 4.0, 2.0, 8.0, "enum", "pattern", false, 1.0, {"2", "3", "4", "5", "6", "7", "8"}},
        // Envelope
        {"gate", "Gate", 0.8, 0.05, 1.0, "", "envelope", false, 0.0, {}},
        {"attack", "Attack", 0.05, 0.0, 0.5, "", "envelope", false, 0.0, {}},
        {"release", "Release", 0.08, 0.0, 0.5, "", "envelope", false, 0.0, {}},
        // Per-step intervals (Custom mode)
        {"step0", "Step 1", 0.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step1", "Step 2", 4.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step2", "Step 3", 7.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step3", "Step 4", 12.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step4", "Step 5", 0.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step5", "Step 6", 0.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step6", "Step 7", 0.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        {"step7", "Step 8", 0.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
        // Pitch trigger
        {"pitchMode", "Pitch Trigger", 0.0, 0.0, 2.0, "enum", "trigger", false, 1.0, {"Always", "Above", "Below"}},
        {"pitchThreshold", "Pitch", 330.0, 50.0, 2000.0, "Hz", "trigger", false, 0.0, {}},
        // Mix
        {"mix", "Mix", 0.8, 0.0, 1.0, "", "", false, 0.0, {}},
    };
    info.presets = pitch_presets::AutoArp(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<AutoArpEffect>(); });
}
} // namespace guitarfx
