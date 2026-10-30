#pragma once

#include "dsp/SignalGraphExecutor.h"

#include <string>
#include <vector>

namespace guitarfx
{
/// A mixer slot's settings: which preset it answers to and how it sits in the mix.
struct PresetInstanceConfig
{
    std::string id;   // Stable preset instance ID (e.g., "p1")
    std::string name; // Display name
    double mix = 1.0; // Linear gain [0.0, 1.0]
    bool mute = false;
    bool solo = false;
    double pan = 0.0; // [-1.0, 1.0] equal-power pan
};

/// Where an instance is in its lifecycle. Retiring instances stay in the voice pool so the
/// existing dispatch/mix paths process them unchanged, but they are hidden from every
/// lookup and query so callers only ever see the live set.
enum class InstancePhase
{
    Active,    ///< Normal: full gain.
    FadingIn,  ///< Just installed, ramping 0 -> 1.
    Tailing,   ///< Superseded, input ramped to zero, output held while the tail rings out.
    FadingOut, ///< On its way out, ramping to 0; retired to the reaper when the ramp ends.
};

/// One preset's chain as the mixer runs it: the executor, its per-block buffers, its slot
/// settings, and where it is in a swap's crossfade or ring-out. PresetVoicePool owns these;
/// MultiPresetMixer::Process() reads and mixes them.
struct PresetInstance
{
    PresetInstanceConfig cfg;
    SignalGraphExecutor executor;
    std::vector<float> outL;
    std::vector<float> outR;
    /// The ramped-to-zero input a tailing instance is fed, in place of the shared
    /// pre-chain output every live instance reads. Per instance rather than one shared
    /// buffer because several may be tailing from different ramp positions at once,
    /// and the parallel dispatch runs them on different threads.
    std::vector<float> tailInL;
    std::vector<float> tailInR;
    int complexityScore = 1;
    /// Whether this graph has anything that could still sound once its input is cut —
    /// a delay, a reverb, or an opaque plugin/WASM node that might be either. Decided
    /// once when the instance is built; a graph with none of them is cut on the declick
    /// ramp as before rather than being run on in the hope that it decays.
    bool canRingOut = false;

    InstancePhase phase = InstancePhase::Active;
    int fadeSamplesRemaining = 0;
    int fadeTotalSamples = 0;

    // Input remains disconnected during the release too, even though phase is FadingOut.
    bool tailInput = false;
    /// Output gain held flat for the whole tail. Frozen at whatever the instance was
    /// at when the tail began, so an instance superseded mid-fade-in does not step up
    /// to unity on its way out.
    float tailGain = 1.0f;
    /// Input ramp 1 -> 0, so nothing new enters the chain but its state decays smoothly
    /// rather than being cut.
    int inputFadeSamplesRemaining = 0;
    int inputFadeTotalSamples = 0;
    /// What is left of the hold budget.
    int tailSamplesRemaining = 0;
    /// Blocks the ramps above wait before they start moving: a swap's incoming chain stays
    /// silent, and the outgoing one stays where it is, until the incoming chain's latency
    /// has filled with signal. See PresetVoicePool::TimeSwap.
    int rampDelaySamplesRemaining = 0;

    /// Mute, and being soloed out, as a ramp rather than a step: 1 is heard, 0 is silent.
    /// Once silent the chain still runs for muteDrainSamplesRemaining, its delays and
    /// reverbs ringing out unheard, and then stops; an unmute has no stale tail to replay.
    /// Audio thread.
    float muteGain = 1.0f;
    int muteDrainSamplesRemaining = 0;

    /// Sizes the per-block buffers. Not on the audio thread.
    void ResizeBuffers(int maxBlockSize);

    /// Gain multiplier at the start of a block of numSamples, and at its end.
    /// Linear (equal-gain) ramp: the outgoing and incoming chains carry the same source
    /// and are strongly correlated, so equal-power would overshoot by up to 3 dB.
    void GetFadeGains(int numSamples, float& startGain, float& endGain) const;

    /// The instance's current fade multiplier.
    [[nodiscard]] float CurrentFadeGain() const;

    /// Start ramping up from silence over `fadeSamples`, as a freshly installed instance does,
    /// once `delaySamples` have passed.
    void BeginFadeIn(int fadeSamples, int delaySamples = 0);

    /// Switch to fading out over `fadeSamples`, after `delaySamples`, starting from whatever
    /// gain the instance is at right now. Switching again while an instance is still fading
    /// in must not snap it back to full gain — that step is exactly the click being designed out.
    void BeginFadeOut(int fadeSamples, int delaySamples = 0);

    /// Switch to ringing out: hold the current output gain for up to `holdSamples`
    /// while the input ramps away over `inputFadeSamples`, after `delaySamples`.
    void BeginTail(int inputFadeSamples, int holdSamples, int delaySamples = 0);

    /// Writes this block's ramped-down input into tailInL/tailInR. Audio thread.
    void FillTailInput(const float* inL, const float* inR, int numSamples);

    /// Moves the ramps on by one block of `numSamples`: the input ramp of a tail, the tail's
    /// hold (starting the `releaseSamples` release when it runs out), and the fade itself,
    /// which settles a finished fade-in to Active. Audio thread, once per block after mixing.
    void AdvanceRamps(int numSamples, int releaseSamples);

    [[nodiscard]] bool IsRetiring() const
    {
        return phase == InstancePhase::FadingOut || phase == InstancePhase::Tailing;
    }

    /// A fade-out that has reached silence and can be handed to the reaper. A tail also
    /// reads as retiring, but it holds a flat gain rather than ramping, so it never is.
    [[nodiscard]] bool IsFinishedFadingOut() const
    {
        return phase == InstancePhase::FadingOut && fadeSamplesRemaining <= 0;
    }

    PresetInstance() = default;
    /// Neither copyable nor movable: the executor's worker threads are bound to its address.
    /// PresetVoicePool holds instances by pointer, so moving one around is a pointer move.
    PresetInstance(const PresetInstance&) = delete;
    PresetInstance& operator=(const PresetInstance&) = delete;
};
} // namespace guitarfx
