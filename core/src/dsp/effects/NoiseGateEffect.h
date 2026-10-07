#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "dsp/PickAttackDetector.h"
#include "dsp/effects/DynamicsPresets.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace guitarfx
{
/**
 * Noise gate with a linked, high-passed peak detector, hysteresis, hold, and a gain ramp.
 *
 * Three things here are load-bearing, and all three are places a simpler gate goes wrong:
 *
 *  - **Attack and release shape the applied gain, not the detector.** The detector runs
 *    fast and at a fixed rate so the gate sees a pick attack the moment it arrives; the
 *    user's times control how the gain ramps between the floor and unity. A gate that
 *    steps its gain from 0 to 1 instead clicks on every close, and this one runs in front
 *    of the amp, where a step at the threshold level gets the amp's full gain applied to
 *    it along with everything else.
 *  - **One detector drives both channels.** Gating each channel off its own envelope lets
 *    a stereo signal end up with one side open and the other shut, which wanders the image.
 *  - **The sidechain is high-passed.** Mains hum and cabinet rumble are the things a guitar
 *    gate exists to remove, and a full-band detector lets exactly those hold it open.
 *
 * Hysteresis is what keeps a decaying note from chattering: the level that opens the gate
 * is above the level that lets it close again, so a signal hovering at the threshold
 * settles into one state instead of oscillating between them.
 *
 * Mode Swell turns it into a volume swell: every note fades in over the Swell time instead of
 * starting with its pick, the violin-like attack of rolling the volume knob up after each
 * note. A note picked while the last still rings dips the level in a few milliseconds and
 * swells again, so legato lines swell note by note; between detached notes the gate closes as
 * usual. Attack does nothing in Swell; Release, Hold, Range and the thresholds work as in Gate.
 */
class NoiseGateEffect : public EffectProcessor
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

    NoiseGateEffect()
    {
        // mSampleRate holds the base class's default until Prepare runs. Derive the
        // coefficients from it now, so a gate asked to process before Prepare still gates:
        // left at zero, the detector would never move and the gate would never open.
        UpdateCoefficients();

        for (auto& pick : mPick)
        {
            pick.Prepare(mSampleRate);
        }
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        if (!ValidatePrepare(sampleRate, maxBlockSize))
        {
            return;
        }

        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        UpdateCoefficients();

        for (auto& pick : mPick)
        {
            pick.Prepare(sampleRate);
        }

        Reset();
    }

    void Reset() override
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            mDetector[channel] = 0.0f;
            mGain[channel] = 0.0f;
            mOpen[channel] = false;
            mHoldSamplesRemaining[channel] = 0;
            mHighPassState[channel] = 0.0f;
            mSwell[channel] = 0.0f;
            mDucking[channel] = false;
            mPick[static_cast<std::size_t>(channel)].Reset();
        }
    }

    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs || numSamples <= 0)
        {
            return;
        }

        GuardState();

        const Coefficients coefficients = LoadCoefficients();
        const float* inputLeft = inputs[0];
        const float* inputRight = inputs[1];
        float* outputLeft = outputs[0];
        float* outputRight = outputs[1];

        for (int i = 0; i < numSamples; ++i)
        {
            const float left = inputLeft ? inputLeft[i] : 0.0f;
            const float right = inputRight ? inputRight[i] : left;
            const float leftLevel = HighPassedLevel(left, 0, coefficients.highPassCoef);
            const float rightLevel = HighPassedLevel(right, 1, coefficients.highPassCoef);
            float leftGain = 0.0f;
            float rightGain = 0.0f;

            if (coefficients.stereoLink)
            {
                // One detector, one gain: whichever channel is loudest decides for both, so a
                // stereo signal cannot end up with one side open and the other shut.
                const bool pick = coefficients.swell && mPick[0].Process(0.5f * (left + right));
                leftGain = NextGain(std::max(leftLevel, rightLevel), coefficients, 0, pick);
                rightGain = leftGain;
            }
            else
            {
                const bool leftPick = coefficients.swell && mPick[0].Process(left);
                const bool rightPick = coefficients.swell && mPick[1].Process(right);
                leftGain = NextGain(leftLevel, coefficients, 0, leftPick);
                rightGain = NextGain(rightLevel, coefficients, 1, rightPick);
            }

            if (outputLeft)
            {
                outputLeft[i] = left * leftGain;
            }

            if (outputRight)
            {
                outputRight[i] = right * rightGain;
            }
        }
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        if (!input)
        {
            std::fill_n(output, numSamples, 0.0f);
            return;
        }

        GuardState();

        const Coefficients coefficients = LoadCoefficients();

        for (int i = 0; i < numSamples; ++i)
        {
            const float key = HighPassedLevel(input[i], 0, coefficients.highPassCoef);
            const bool pick = coefficients.swell && mPick[0].Process(input[i]);
            output[i] = input[i] * NextGain(key, coefficients, 0, pick);
        }
    }

    // The ranges the registry publishes, kept here so SetParam and the registration below
    // cannot drift apart. SetParam clamps to them: a value from a preset or an automation
    // source has been through JSON and a host, and neither guarantees anything.
    static constexpr double kMinThresholdDb = -80.0;
    static constexpr double kMaxThresholdDb = 0.0;
    static constexpr double kMinAttackMs = 0.1;
    static constexpr double kMaxAttackMs = 50.0;
    static constexpr double kMinHoldMs = 0.0;
    static constexpr double kMaxHoldMs = 500.0;
    static constexpr double kMinReleaseMs = 1.0;
    static constexpr double kMaxReleaseMs = 500.0;
    static constexpr double kMinHysteresisDb = 0.0;
    static constexpr double kMaxHysteresisDb = 24.0;
    static constexpr double kMinRangeDb = -90.0;
    static constexpr double kMaxRangeDb = 0.0;
    static constexpr double kMinSwellMs = 50.0;
    static constexpr double kMaxSwellMs = 4000.0;

    void SetParam(const std::string& key, double value) override
    {
        // A non-finite threshold makes every comparison in NextGain() false, which shuts the
        // gate for good with no way back short of Reset(). Drop the value instead.
        if (!IsFinite(value))
        {
            return;
        }

        if (key == "threshold" || key == "thresholdDb")
        {
            mThresholdDb.store(static_cast<float>(std::clamp(value, kMinThresholdDb, kMaxThresholdDb)),
                               std::memory_order_relaxed);
        }
        else if (key == "attack" || key == "attackMs")
        {
            mAttackMs.store(static_cast<float>(std::clamp(value, kMinAttackMs, kMaxAttackMs)),
                            std::memory_order_relaxed);
        }
        else if (key == "hold" || key == "holdMs")
        {
            mHoldMs.store(static_cast<float>(std::clamp(value, kMinHoldMs, kMaxHoldMs)), std::memory_order_relaxed);
        }
        else if (key == "release" || key == "releaseMs")
        {
            mReleaseMs.store(static_cast<float>(std::clamp(value, kMinReleaseMs, kMaxReleaseMs)),
                             std::memory_order_relaxed);
        }
        else if (key == "hysteresis")
        {
            mHysteresisDb.store(static_cast<float>(std::clamp(value, kMinHysteresisDb, kMaxHysteresisDb)),
                                std::memory_order_relaxed);
        }
        else if (key == "range")
        {
            mRangeDb.store(static_cast<float>(std::clamp(value, kMinRangeDb, kMaxRangeDb)), std::memory_order_relaxed);
        }
        else if (key == "stereoLink")
        {
            mStereoLink.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
        }
        else if (key == "mode")
        {
            mMode.store(static_cast<float>(std::round(std::clamp(value, 0.0, 1.0))), std::memory_order_relaxed);
        }
        else if (key == "swell")
        {
            mSwellMs.store(static_cast<float>(std::clamp(value, kMinSwellMs, kMaxSwellMs)), std::memory_order_relaxed);
        }
        else
        {
            return;
        }

        UpdateCoefficients();
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "threshold" || key == "thresholdDb")
        {
            return mThresholdDb.load(std::memory_order_relaxed);
        }

        if (key == "attack" || key == "attackMs")
        {
            return mAttackMs.load(std::memory_order_relaxed);
        }

        if (key == "hold" || key == "holdMs")
        {
            return mHoldMs.load(std::memory_order_relaxed);
        }

        if (key == "release" || key == "releaseMs")
        {
            return mReleaseMs.load(std::memory_order_relaxed);
        }

        if (key == "hysteresis")
        {
            return mHysteresisDb.load(std::memory_order_relaxed);
        }

        if (key == "range")
        {
            return mRangeDb.load(std::memory_order_relaxed);
        }

        if (key == "stereoLink")
        {
            return mStereoLink.load(std::memory_order_relaxed);
        }

        if (key == "mode")
        {
            return mMode.load(std::memory_order_relaxed);
        }

        if (key == "swell")
        {
            return mSwellMs.load(std::memory_order_relaxed);
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "dynamics_gate";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "dynamics";
    }

    /// True while the gate is passing signal. Exposed for tests and metering, not for the
    /// audio path: it is the state machine's own flag, not the gain actually applied.
    [[nodiscard]] bool IsOpen() const
    {
        return mOpen[0];
    }

  private:
    /// The detector's own timing, deliberately not user-facing. It has to see a pick attack
    /// the instant it arrives, and its release has to be slow enough to bridge the gaps
    /// between the peaks of a low open E (82 Hz, so 12 ms apart) or the detector would sag
    /// once a cycle and chatter the gate within a single note.
    static constexpr double kDetectorAttackMs = 0.2;
    static constexpr double kDetectorReleaseMs = 25.0;

    /// Sidechain corner. Below this is hum, rumble and handling noise -- the things a gate is
    /// asked to remove, which a full-band detector would instead treat as reasons to open.
    static constexpr double kSidechainHighPassHz = 120.0;

    /// Swell: a pick only restarts the swell once the last one has got this far, so the pick
    /// that opened the gate cannot restart its own swell.
    static constexpr float kRetriggerAbove = 0.3f;
    /// How fast a retrigger dips the level, and how far before it swells again.
    static constexpr double kDuckMs = 2.0;
    static constexpr float kDuckFloor = 0.03f;

    struct Coefficients
    {
        float attackCoef = 0.0f;
        float releaseCoef = 0.0f;
        float detectorAttackCoef = 0.0f;
        float detectorReleaseCoef = 0.0f;
        float highPassCoef = 0.0f;
        float openThreshold = 0.0f;
        float closeThreshold = 0.0f;
        float floorGain = 0.0f;
        int holdSamples = 0;
        bool stereoLink = true;
        bool swell = false;
        float swellStep = 0.0f;
        float duckCoef = 0.0f;
    };

    [[nodiscard]] Coefficients LoadCoefficients() const
    {
        Coefficients coefficients;
        coefficients.attackCoef = mAttackCoef.load(std::memory_order_relaxed);
        coefficients.releaseCoef = mReleaseCoef.load(std::memory_order_relaxed);
        coefficients.detectorAttackCoef = mDetectorAttackCoef.load(std::memory_order_relaxed);
        coefficients.detectorReleaseCoef = mDetectorReleaseCoef.load(std::memory_order_relaxed);
        coefficients.highPassCoef = mHighPassCoef.load(std::memory_order_relaxed);
        coefficients.openThreshold = mOpenThreshold.load(std::memory_order_relaxed);
        coefficients.closeThreshold = mCloseThreshold.load(std::memory_order_relaxed);
        coefficients.floorGain = mFloorGain.load(std::memory_order_relaxed);
        coefficients.holdSamples = mHoldSamples.load(std::memory_order_relaxed);
        // Dual mono keeps each side to its own detector, whatever Stereo Link says.
        coefficients.stereoLink =
            mStereoLink.load(std::memory_order_relaxed) >= 0.5f && !mDualMono.load(std::memory_order_relaxed);
        coefficients.swell = mMode.load(std::memory_order_relaxed) >= 0.5f;
        coefficients.swellStep = mSwellStep.load(std::memory_order_relaxed);
        coefficients.duckCoef = mDuckCoef.load(std::memory_order_relaxed);
        return coefficients;
    }

    /// One non-finite input sample would poison the detector and the gain for good -- NaN
    /// compares false against every threshold, so the gate would sit shut with no way back.
    /// Checking the state once per block cannot latch and costs nothing measurable.
    void GuardState()
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            if (!IsFinite(mDetector[channel]) || !IsFinite(mGain[channel]) || !IsFinite(mHighPassState[channel]) ||
                !IsFinite(mSwell[channel]))
            {
                Reset();
                return;
            }
        }
    }

    /// One-pole: the state follows the low end, so what is left over is the high-passed
    /// signal. Rectified here rather than by the caller, because rectifying first would turn
    /// the hum this is meant to reject into a DC term the filter cannot see.
    [[nodiscard]] float HighPassedLevel(float sample, int channel, float coef)
    {
        mHighPassState[channel] += coef * (sample - mHighPassState[channel]);
        return std::abs(sample - mHighPassState[channel]);
    }

    [[nodiscard]] float NextGain(float level, const Coefficients& coefficients, int channel, bool pick)
    {
        float& detector = mDetector[channel];
        detector += (level > detector ? coefficients.detectorAttackCoef : coefficients.detectorReleaseCoef) *
                    (level - detector);

        if (detector > coefficients.openThreshold)
        {
            mOpen[channel] = true;
            mHoldSamplesRemaining[channel] = coefficients.holdSamples;
        }
        else if (mOpen[channel])
        {
            if (detector > coefficients.closeThreshold)
            {
                // Between the two thresholds and already open: there is still signal, so hold
                // the gate where it is rather than starting to close. This is the hysteresis.
                mHoldSamplesRemaining[channel] = coefficients.holdSamples;
            }
            else if (mHoldSamplesRemaining[channel] > 0)
            {
                --mHoldSamplesRemaining[channel];
            }
            else
            {
                mOpen[channel] = false;
            }
        }

        if (coefficients.swell)
        {
            return NextSwellGain(coefficients, channel, pick);
        }

        // Ramp the gain rather than stepping it. The target is the floor, not silence, so the
        // close is an attenuation the ear reads as the noise dropping away instead of the
        // signal being cut -- and the ramp never has to converge on exact zero.
        const float target = mOpen[channel] ? 1.0f : coefficients.floorGain;
        float& gain = mGain[channel];
        gain += (target > gain ? coefficients.attackCoef : coefficients.releaseCoef) * (target - gain);
        return gain;
    }

    /// Swell's gain: a position that climbs from 0 to 1 over the Swell time while the gate is
    /// open and falls at the Release rate once it shuts, squared so the fade sounds even
    /// rather than rushing up at the start, and lifted onto the floor Range sets.
    [[nodiscard]] float NextSwellGain(const Coefficients& coefficients, int channel, bool pick)
    {
        float& position = mSwell[channel];
        bool& ducking = mDucking[channel];

        if (pick && mOpen[channel] && position > kRetriggerAbove)
        {
            ducking = true;
        }

        if (ducking)
        {
            position -= coefficients.duckCoef * position;
            ducking = position > kDuckFloor;
        }
        else if (mOpen[channel])
        {
            position = std::min(1.0f, position + coefficients.swellStep);
        }
        else
        {
            position -= coefficients.releaseCoef * position;
        }

        float& gain = mGain[channel];
        gain = coefficients.floorGain + (1.0f - coefficients.floorGain) * position * position;
        return gain;
    }

    void UpdateCoefficients()
    {
        if (mSampleRate <= 0.0)
        {
            return;
        }

        const auto onePole = [this](double milliseconds) {
            return static_cast<float>(1.0 - std::exp(-1.0 / (std::max(milliseconds, 0.001) * 0.001 * mSampleRate)));
        };

        const double thresholdDb = mThresholdDb.load(std::memory_order_relaxed);
        const double hysteresisDb = mHysteresisDb.load(std::memory_order_relaxed);

        mAttackCoef.store(onePole(mAttackMs.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        mReleaseCoef.store(onePole(mReleaseMs.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        mDetectorAttackCoef.store(onePole(kDetectorAttackMs), std::memory_order_relaxed);
        mDetectorReleaseCoef.store(onePole(kDetectorReleaseMs), std::memory_order_relaxed);
        mHighPassCoef.store(static_cast<float>(1.0 - std::exp(-2.0 * kPi * kSidechainHighPassHz / mSampleRate)),
                            std::memory_order_relaxed);
        mOpenThreshold.store(static_cast<float>(std::pow(10.0, thresholdDb / 20.0)), std::memory_order_relaxed);
        mCloseThreshold.store(static_cast<float>(std::pow(10.0, (thresholdDb - hysteresisDb) / 20.0)),
                              std::memory_order_relaxed);
        mFloorGain.store(static_cast<float>(std::pow(10.0, mRangeDb.load(std::memory_order_relaxed) / 20.0)),
                         std::memory_order_relaxed);
        mHoldSamples.store(static_cast<int>(mHoldMs.load(std::memory_order_relaxed) * 0.001 * mSampleRate),
                           std::memory_order_relaxed);
        mSwellStep.store(static_cast<float>(1000.0 / (mSwellMs.load(std::memory_order_relaxed) * mSampleRate)),
                         std::memory_order_relaxed);
        mDuckCoef.store(onePole(kDuckMs), std::memory_order_relaxed);
    }

    static constexpr double kPi = 3.14159265358979323846;

    // Parameters, written from the message thread and read on the audio thread.
    std::atomic<float> mThresholdDb{-60.0f};
    std::atomic<float> mAttackMs{1.0f};
    std::atomic<float> mHoldMs{50.0f};
    std::atomic<float> mReleaseMs{50.0f};
    std::atomic<float> mHysteresisDb{4.0f};
    std::atomic<float> mRangeDb{-80.0f};
    std::atomic<float> mStereoLink{1.0f};
    std::atomic<bool> mDualMono{false};
    std::atomic<float> mMode{0.0f};
    std::atomic<float> mSwellMs{800.0f};

    // Derived from the parameters and the sample rate by UpdateCoefficients().
    std::atomic<float> mAttackCoef{0.0f};
    std::atomic<float> mReleaseCoef{0.0f};
    std::atomic<float> mDetectorAttackCoef{0.0f};
    std::atomic<float> mDetectorReleaseCoef{0.0f};
    std::atomic<float> mHighPassCoef{0.0f};
    std::atomic<float> mOpenThreshold{0.0f};
    std::atomic<float> mCloseThreshold{0.0f};
    std::atomic<float> mFloorGain{0.0f};
    std::atomic<int> mHoldSamples{0};
    std::atomic<float> mSwellStep{0.0f};
    std::atomic<float> mDuckCoef{0.0f};

    // State, audio thread only. Linked detection uses index 0 for both channels.
    float mDetector[2] = {0.0f, 0.0f};
    float mGain[2] = {0.0f, 0.0f};
    float mHighPassState[2] = {0.0f, 0.0f};
    int mHoldSamplesRemaining[2] = {0, 0};
    bool mOpen[2] = {false, false};
    // Swell's position through its fade, and whether a retrigger is dipping it.
    float mSwell[2] = {0.0f, 0.0f};
    bool mDucking[2] = {false, false};
    std::array<PickAttackDetector, 2> mPick;
};

inline void RegisterNoiseGateEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kDynamicsGate;
    info.aliases = {"dynamics_gate", "gate_noise"};
    info.displayName = "Noise Gate";
    info.category = "dynamics";
    info.description = "Noise gate with hysteresis, hold and a smoothed gain ramp";
    info.requiresResource = false;

    // These ids are the ones GlobalChainEditor, the UI and every other effect in the registry
    // use. The suffixed spellings below are what shipped presets and older stored nodes carry;
    // CanonicalizeNodeParams folds them onto these, so a node cannot end up holding the same
    // parameter twice and letting map order pick the winner.
    ParameterDef threshold{
        "threshold", "Threshold", -60.0, NoiseGateEffect::kMinThresholdDb, NoiseGateEffect::kMaxThresholdDb, "dB"};
    threshold.aliases = {"thresholdDb"};

    ParameterDef attack{"attack", "Attack", 1.0, NoiseGateEffect::kMinAttackMs, NoiseGateEffect::kMaxAttackMs, "ms"};
    attack.aliases = {"attackMs"};

    ParameterDef hold{"hold", "Hold", 50.0, NoiseGateEffect::kMinHoldMs, NoiseGateEffect::kMaxHoldMs, "ms"};
    hold.aliases = {"holdMs"};

    ParameterDef release{"release", "Release", 50.0, NoiseGateEffect::kMinReleaseMs, NoiseGateEffect::kMaxReleaseMs,
                         "ms"};
    release.aliases = {"releaseMs"};

    // How far the level has to fall below Threshold before the gate is allowed to close.
    // Without it a note decaying through the threshold chatters the gate open and shut.
    const ParameterDef hysteresis{
        "hysteresis", "Hysteresis", 4.0, NoiseGateEffect::kMinHysteresisDb, NoiseGateEffect::kMaxHysteresisDb,
        "dB",         "",           true};

    // What the gate attenuates by when closed, rather than muting outright. The default is
    // low enough to read as silence while keeping the close an attenuation, not a cut.
    const ParameterDef range{"range", "Range", -80.0, NoiseGateEffect::kMinRangeDb, NoiseGateEffect::kMaxRangeDb,
                             "dB",    "",      true};

    // Linked is the right default for a gate: one detector for both channels means a stereo
    // signal cannot have one side open and the other shut, which wanders the image. Kept as
    // a switch because gating each channel off its own level is the other valid reading, and
    // StereoProcessingTests holds the gate to it.
    ParameterDef stereoLink{"stereoLink", "Stereo Link", 1.0, 0.0, 1.0, "", "", true, 1.0};
    stereoLink.labels = {"Independent", "Linked"};

    // Swell fades each note in rather than gating it: see the class comment.
    ParameterDef mode{"mode", "Mode", 0.0, 0.0, 1.0, "enum", "", false, 1.0};
    mode.labels = {"Gate", "Swell"};
    const ParameterDef swell = WithLogTaper(
        {"swell", "Swell", 800.0, NoiseGateEffect::kMinSwellMs, NoiseGateEffect::kMaxSwellMs, "ms", "Swell"});

    info.parameters = {threshold, attack, hold, release, hysteresis, range, stereoLink, mode, swell};
    info.presets = dynamics_presets::Gate(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<NoiseGateEffect>(); });
}
} // namespace guitarfx
