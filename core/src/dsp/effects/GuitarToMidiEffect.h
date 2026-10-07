#pragma once

#include "dsp/EffectGuids.h"
#include "dsp/EffectParamSpec.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/NoteEvents.h"
#include "dsp/NoteTracker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace guitarfx
{
namespace guitar_to_midi
{
inline constexpr const char* kModeLabels[] = {"Notes", "Notes + Bend"};
inline constexpr const char* kBendRangeLabels[] = {"2 semitones", "12 semitones", "24 semitones", "48 semitones"};
inline constexpr double kBendRanges[] = {2.0, 12.0, 24.0, 48.0};
/// Shared with the other note-following effects (dsp/PitchTracker.h).
using pitch_tracker::kLowestNoteLabels;

enum Param : std::size_t
{
    kMode,
    kBendRange,
    kChannel,
    kTranspose,
    kThreshold,
    kDynamics,
    kLowestNote,
    kThru,
    kParamCount
};

/// The one place parameter ranges live: registration and SetParam's clamping both read it.
inline constexpr std::array<EffectParamSpec, kParamCount> kParams = {{
    {"mode", "Mode", 1.0, 0.0, 1.0, "enum", "Notes", false, 1.0, kModeLabels},
    {"bendRange", "Bend Range", 0.0, 0.0, 3.0, "enum", "Notes", false, 1.0, kBendRangeLabels},
    {"channel", "MIDI Channel", 1.0, 1.0, 16.0, "", "Notes", false, 1.0},
    {"transpose", "Transpose", 0.0, -24.0, 24.0, "st", "Notes", false, 1.0},
    {"threshold", "Threshold", -50.0, -60.0, -20.0, "dB", "Input", false, 0.0},
    {"dynamics", "Dynamics", 0.7, 0.0, 1.0, "amount", "Input", false, 0.0},
    {"lowestNote", "Lowest Note", 1.0, 0.0, 3.0, "enum", "Input", false, 1.0, kLowestNoteLabels},
    {"thru", "Guitar Thru", 1.0, 0.0, 1.0, "toggle", "Output", false, 1.0},
}};

static_assert(kParams[kMode].maxValue == static_cast<double>(std::size(kModeLabels) - 1));
static_assert(kParams[kBendRange].maxValue == static_cast<double>(std::size(kBendRangeLabels) - 1));
static_assert(std::size(kBendRanges) == std::size(kBendRangeLabels));
static_assert(kParams[kLowestNote].maxValue == static_cast<double>(std::size(kLowestNoteLabels) - 1));

/// In Notes mode the pitch has to move this far past halfway to the next note before it switches,
/// so vibrato on a note that is a little out of tune does not flicker between two.
constexpr double kNoteHysteresisSemitones = 0.2;
/// In Notes + Bend mode a bend that goes this far past the end of the bend range moves to the
/// nearest note; short of that, the bend holds at the end of the range. So a whole-step bend that
/// goes a little sharp, at the usual range of 2 semitones, still bends rather than restarting.
constexpr double kBendOvershootSemitones = 0.3;
/// Bends closer than this to the last one sent are not sent: finer than anyone hears.
constexpr double kBendDeadbandCents = 1.0;
/// Velocity when Dynamics is at zero.
constexpr int kFixedVelocity = 100;
constexpr double kThruRampSeconds = 0.005;

[[nodiscard]] inline std::size_t FindParam(const std::string& key)
{
    return FindParamSpec(kParams, key);
}
} // namespace guitar_to_midi

/**
 * Guitar to MIDI: single notes played into it come out as MIDI notes, for a virtual instrument
 * in a Plugin Host downstream of it in the same chain.
 *
 * The notes come from the shared note tracker (dsp/NoteTracker.h): a pick starts a note at its
 * pitch, with a velocity from how hard it was picked; a hammer-on, pull-off or slide moves it
 * legato (the new note starts before the old one stops, which mono synths glide or tie); the
 * note stops when the string is muted or dies away under the threshold. It is monophonic: a chord
 * gives one of its notes at best, often late, and a power chord often nothing at all.
 *
 * - Notes: pitches are rounded to the nearest note, and a bend that passes the next note by a
 *   little moves to it, legato.
 * - Notes + Bend: the note is held and pitch bend follows the guitar, bends and vibrato and the
 *   guitar's own tuning included. Bend Range must match the instrument's, or the pitch comes out
 *   wrong; most default to 2 semitones. A bend that goes past the end of the range moves to the
 *   nearest note and bends on from there.
 *
 * Lowest Note narrows the pitch tracker to the guitar's lowest string: the higher it is, the
 * sooner a note is recognised. At the drop-D default a note from C#3 up starts about 30 ms after
 * the pick on the demo riffs. A low note picked hard takes about 55 ms: for its first 50 ms the
 * string is not periodic enough for the tracker to read, and the note starts one detection after
 * the tracker first reads it. Guitar Thru passes the guitar on, for an instrument layered with
 * it or a chain that carries on; off, only the instrument (through its Plugin Host's Mix) is
 * heard.
 *
 * The notes reach every Plugin Host downstream of this node (SignalGraphExecutor, "Note
 * routing"). Changing the channel, transpose, mode or bend range mid-note ends the note and
 * starts it again under the new settings.
 */
class GuitarToMidiEffect : public EffectProcessor
{
  public:
    /// Identical sides in, identical sides out, whatever the settings (EffectProcessor::CanWiden).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    GuitarToMidiEffect()
    {
        for (std::size_t index = 0; index < guitar_to_midi::kParamCount; ++index)
        {
            mValues[index] = guitar_to_midi::kParams[index].defaultValue;
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
        mMono.assign(static_cast<std::size_t>(maxBlockSize), 0.0f);
        mThruStep = static_cast<float>(1.0 / std::max(1.0, guitar_to_midi::kThruRampSeconds * sampleRate));
        mAppliedThresholdDb = mValues[guitar_to_midi::kThreshold];
        mTracker.SetThresholdDb(mAppliedThresholdDb);
        mTracker.Prepare(sampleRate);
        mTracker.SetLowestFrequency(LowestNoteHz());
        mPrepared = true;
        Reset();
    }

    void Reset() override
    {
        mTracker.Reset();
        mNotes.Clear();
        mActiveNote = -1;
        mVelocity = 0;
        mBendSent = 0;
        ApplyNoteSettings();
        mNotes.heldNote = -1;
        mNotes.heldVelocity = 0;
        mNotes.channel = static_cast<std::uint8_t>(mChannel);
        mNotes.bend = 0;
        mThruGain = ThruTarget();
    }

    [[nodiscard]] bool SupportsMonoProcessing() const override
    {
        return true;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!outputs || numSamples <= 0)
        {
            return;
        }

        const float* left = inputs ? inputs[0] : nullptr;
        const float* right = inputs ? inputs[1] : nullptr;
        BeginBlock();

        for (int start = 0; start < numSamples;)
        {
            const int count = std::min(numSamples - start, static_cast<int>(mMono.size()));

            if (count <= 0)
            {
                break;
            }

            for (int i = 0; i < count; ++i)
            {
                const float l = left ? left[start + i] : 0.0f;
                mMono[static_cast<std::size_t>(i)] = right ? 0.5f * (l + right[start + i]) : l;
            }

            Track(mMono.data(), count, start);
            start += count;
        }

        // Both channels ramp alike. A mono input is copied across once written, since the output
        // may be the input's own buffer.
        const float thruFrom = mThruGain;
        WriteThru(left, outputs[0], numSamples, thruFrom);

        if (outputs[1])
        {
            if (right || !outputs[0])
            {
                WriteThru(right ? right : left, outputs[1], numSamples, thruFrom);
            }
            else
            {
                std::copy(outputs[0], outputs[0] + numSamples, outputs[1]);
            }
        }

        EndBlock();
    }

    void ProcessMono(float* input, float* output, int numSamples) override
    {
        if (!output || numSamples <= 0)
        {
            return;
        }

        BeginBlock();
        Track(input, numSamples, 0);
        WriteThru(input, output, numSamples, mThruGain);
        EndBlock();
    }

    [[nodiscard]] const NoteBlock* GetNoteOutput() const override
    {
        return &mNotes;
    }

    void SetParam(const std::string& key, double value) override
    {
        const std::size_t index = guitar_to_midi::FindParam(key);

        if (index == guitar_to_midi::kParamCount || !IsFinite(value))
        {
            return;
        }

        mValues[index] = NormaliseParamValue(guitar_to_midi::kParams[index], value);
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (const std::size_t index = guitar_to_midi::FindParam(key); index != guitar_to_midi::kParamCount)
        {
            return mValues[index];
        }

        // Read-only state, for tests and any future note display.
        if (key == "currentNote")
        {
            return mActiveNote;
        }

        if (key == "currentBend")
        {
            return mBendSent;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "guitar_to_midi";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "synth";
    }

  private:
    [[nodiscard]] int Choice(guitar_to_midi::Param param) const
    {
        return static_cast<int>(std::lround(mValues[param]));
    }

    [[nodiscard]] double LowestNoteHz() const
    {
        return pitch_tracker::LowestNoteHz(Choice(guitar_to_midi::kLowestNote));
    }

    [[nodiscard]] float ThruTarget() const
    {
        return mValues[guitar_to_midi::kThru] >= 0.5 ? 1.0f : 0.0f;
    }

    /// Takes up the settings that decide which MIDI a pitch becomes.
    void ApplyNoteSettings()
    {
        mBendMode = Choice(guitar_to_midi::kMode) == 1;
        mBendRange = guitar_to_midi::kBendRanges[static_cast<std::size_t>(std::clamp(
            Choice(guitar_to_midi::kBendRange), 0, static_cast<int>(std::size(guitar_to_midi::kBendRanges)) - 1))];
        mChannel = std::clamp(Choice(guitar_to_midi::kChannel), 1, 16) - 1;
        mTranspose = Choice(guitar_to_midi::kTranspose);
    }

    [[nodiscard]] bool NoteSettingsChanged() const
    {
        const bool bendMode = Choice(guitar_to_midi::kMode) == 1;
        const double bendRange = guitar_to_midi::kBendRanges[static_cast<std::size_t>(std::clamp(
            Choice(guitar_to_midi::kBendRange), 0, static_cast<int>(std::size(guitar_to_midi::kBendRanges)) - 1))];
        return bendMode != mBendMode || bendRange != mBendRange ||
               std::clamp(Choice(guitar_to_midi::kChannel), 1, 16) - 1 != mChannel ||
               Choice(guitar_to_midi::kTranspose) != mTranspose;
    }

    /// Parameters land between blocks; this is where they are taken up.
    void BeginBlock()
    {
        mNotes.Clear();

        if (!mPrepared)
        {
            return;
        }

        if (mValues[guitar_to_midi::kThreshold] != mAppliedThresholdDb)
        {
            mAppliedThresholdDb = mValues[guitar_to_midi::kThreshold];
            mTracker.SetThresholdDb(mAppliedThresholdDb);
        }

        if (mTracker.LowestFrequencyHz() != LowestNoteHz())
        {
            // A new range starts the tracker again, so whatever it was holding is gone.
            StopNote(0);
            mTracker.SetLowestFrequency(LowestNoteHz());
        }

        if (NoteSettingsChanged())
        {
            StopNote(0);

            if (mBendSent != 0)
            {
                SendBend(0, 0);
            }

            ApplyNoteSettings();

            if (mTracker.IsSounding())
            {
                StartNote(mTracker.Pitch(), mTracker.Velocity(), 0);
            }
        }
    }

    void EndBlock()
    {
        mNotes.heldNote = mActiveNote;
        mNotes.heldVelocity = static_cast<std::uint8_t>(mActiveNote >= 0 ? mVelocity : 0);
        mNotes.channel = static_cast<std::uint8_t>(mChannel);
        mNotes.bend = static_cast<std::int16_t>(mBendSent);
    }

    void Track(const float* mono, int count, int blockOffset)
    {
        if (!mPrepared)
        {
            return;
        }

        mTracker.Process(mono, count, [this, blockOffset](const NoteTracker::Event& event) {
            const int offset = blockOffset + event.sampleOffset;

            switch (event.type)
            {
            case NoteTracker::Event::Type::Start:
                StartNote(event.pitch, event.velocity, offset);
                break;

            case NoteTracker::Event::Type::Legato:
                MoveTo(event.pitch, offset, true);
                break;

            case NoteTracker::Event::Type::Glide:
                MoveTo(event.pitch, offset, false);
                break;

            case NoteTracker::Event::Type::Stop:
                StopNote(offset);
                break;
            }
        });
    }

    void WriteThru(const float* input, float* output, int numSamples, float from)
    {
        if (!output)
        {
            return;
        }

        const float target = ThruTarget();
        float gain = from;

        for (int i = 0; i < numSamples; ++i)
        {
            gain = gain < target ? std::min(target, gain + mThruStep) : std::max(target, gain - mThruStep);
            output[i] = input ? input[i] * gain : 0.0f;
        }

        mThruGain = gain;
    }

    [[nodiscard]] int VelocityFrom(float strength) const
    {
        const double dynamics = mValues[guitar_to_midi::kDynamics];
        const double fixed = (guitar_to_midi::kFixedVelocity - 1) / 126.0;
        const double amount = fixed + dynamics * (std::clamp(static_cast<double>(strength), 0.0, 1.0) - fixed);
        return std::clamp(1 + static_cast<int>(std::lround(126.0 * amount)), 1, 127);
    }

    [[nodiscard]] int BendFor(double semitones) const
    {
        const double value = std::round(8192.0 * semitones / mBendRange);
        return static_cast<int>(
            std::clamp(value, static_cast<double>(NoteBlock::kMinBend), static_cast<double>(NoteBlock::kMaxBend)));
    }

    void Push(NoteEvent::Type type, int note, int velocity, int bend, int offset)
    {
        NoteEvent event;
        event.type = type;
        event.channel = static_cast<std::uint8_t>(mChannel);
        event.note = static_cast<std::uint8_t>(std::clamp(note, 0, 127));
        event.velocity = static_cast<std::uint8_t>(std::clamp(velocity, 0, 127));
        event.bend = static_cast<std::int16_t>(bend);
        event.sampleOffset = offset;
        // A full block drops the event but not the change: the held note and bend at the end of
        // the block still say where things stand, and the player catches up from those.
        mNotes.Push(event);
    }

    void SendOn(int note, int offset)
    {
        Push(NoteEvent::Type::NoteOn, note, mVelocity, 0, offset);
        mActiveNote = note;
    }

    void SendOff(int note, int offset)
    {
        Push(NoteEvent::Type::NoteOff, note, 0, 0, offset);
    }

    void SendBend(int bend, int offset)
    {
        Push(NoteEvent::Type::PitchBend, 0, 0, bend, offset);
        mBendSent = bend;
    }

    /// A bend unless it is within the deadband of the last one sent.
    void SendBendIfMoved(int bend, int offset)
    {
        const double unitsPerCent = 8192.0 / (100.0 * mBendRange);
        const int deadband =
            std::max(1, static_cast<int>(std::lround(guitar_to_midi::kBendDeadbandCents * unitsPerCent)));

        if (std::abs(bend - mBendSent) >= deadband)
        {
            SendBend(bend, offset);
        }
    }

    void StartNote(double pitch, float strength, int offset)
    {
        StopNote(offset);
        const double target = pitch + mTranspose;
        const auto note = static_cast<int>(std::lround(target));

        if (note < 0 || note > 127)
        {
            return;
        }

        mVelocity = VelocityFrom(strength);

        if (mBendMode)
        {
            const int bend = BendFor(target - note);

            if (bend != mBendSent)
            {
                SendBend(bend, offset);
            }
        }
        else if (mBendSent != 0)
        {
            SendBend(0, offset);
        }

        SendOn(note, offset);
    }

    void StopNote(int offset)
    {
        if (mActiveNote >= 0)
        {
            SendOff(mActiveNote, offset);
            mActiveNote = -1;
        }
    }

    /// The new note starts before the old one stops, which a mono synth plays legato.
    void LegatoTo(int note, double target, int offset)
    {
        const int previous = mActiveNote;

        if (note < 0 || note > 127)
        {
            StopNote(offset);
            return;
        }

        if (mBendMode)
        {
            SendBend(BendFor(target - note), offset);
        }

        SendOn(note, offset);
        SendOff(previous, offset);
    }

    void MoveTo(double pitch, int offset, bool legato)
    {
        if (mActiveNote < 0)
        {
            return;
        }

        const double target = pitch + mTranspose;
        const auto nearest = static_cast<int>(std::lround(target));
        const double away = target - mActiveNote;

        if (mBendMode)
        {
            if ((legato || std::abs(away) > mBendRange + guitar_to_midi::kBendOvershootSemitones) &&
                nearest != mActiveNote)
            {
                LegatoTo(nearest, target, offset);
            }
            else
            {
                SendBendIfMoved(BendFor(away), offset);
            }

            return;
        }

        if (nearest != mActiveNote && (legato || std::abs(away) > 0.5 + guitar_to_midi::kNoteHysteresisSemitones))
        {
            LegatoTo(nearest, target, offset);
        }
    }

    std::array<double, guitar_to_midi::kParamCount> mValues{};
    NoteTracker mTracker;
    NoteBlock mNotes;
    std::vector<float> mMono;
    bool mPrepared = false;
    double mAppliedThresholdDb = NoteTracker::kDefaultThresholdDb;

    bool mBendMode = true;
    double mBendRange = 2.0;
    int mChannel = 0;
    int mTranspose = 0;

    int mActiveNote = -1;
    int mVelocity = 0;
    int mBendSent = 0;

    float mThruGain = 1.0f;
    float mThruStep = 1.0f;
};

inline void RegisterGuitarToMidiEffect()
{
    EffectTypeInfo info;
    info.type = EffectGuids::kGuitarToMidi;
    info.aliases = {"guitar_to_midi"};
    info.displayName = "Guitar to MIDI";
    info.category = "synth";
    info.description = "Plays single notes into a virtual instrument. Put a Plugin Host with an instrument after "
                       "it in the chain";
    info.requiresResource = false;
    info.parameters = BuildParameterDefs(guitar_to_midi::kParams);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<GuitarToMidiEffect>(); });
}
} // namespace guitarfx
