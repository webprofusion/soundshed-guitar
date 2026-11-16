#pragma once

#include "dsp/FiniteCheck.h"
#include "dsp/NoteEvents.h"
#include "dsp/PolyNoteDetector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace guitarfx::guitar_to_midi
{
/// Polyphonic + Bend plays an MPE lower zone: channel 1 manages it, and each note has one of
/// these member channels (2 to 16) to itself, so each bends on its own.
inline constexpr int kMpeFirstMember = 1;
inline constexpr int kMpeMembers = 15;
/// Bends closer than this to the last one sent are not sent: finer than anyone hears.
inline constexpr double kBendDeadbandCents = 1.0;

/// Pitch bend for `semitones` at a bend range of `range`, clamped to 14 bits.
[[nodiscard]] inline int BendValue(double semitones, double range)
{
    const double value = std::round(8192.0 * semitones / range);
    return static_cast<int>(
        std::clamp(value, static_cast<double>(NoteBlock::kMinBend), static_cast<double>(NoteBlock::kMaxBend)));
}

/// kBendDeadbandCents in bend units at a bend range of `range`.
[[nodiscard]] inline int BendDeadband(double range)
{
    return std::max(1, static_cast<int>(std::lround(kBendDeadbandCents * 8192.0 / (100.0 * range))));
}

/**
 * Guitar to MIDI's Polyphonic modes as MIDI: which of the detector's notes are sounding and at
 * what velocity, and in Polyphonic + Bend each one's MPE member channel and the bend that
 * channel holds. Events go into the NoteBlock passed in, under the settings passed in.
 */
class PolyVoices
{
  public:
    struct Settings
    {
        int channel = 0; ///< MIDI Channel, 0-based; not used with mpe
        int transpose = 0;
        bool mpe = false;
        double bendRange = 2.0;
    };

    void Reset()
    {
        mVelocity = {};
        mChannel = {};
        mChannelUsers = {};
        mChannelFreedAt = {};
        mChannelBend = {};
        mClock = 0;
    }

    /// Starts note `index` at `pitch` (its channel bent there first, with mpe).
    void Start(NoteBlock& out, const Settings& settings, int index, int velocity, double pitch, int offset)
    {
        if (index < 0 || index >= PolyNoteDetector::kNotes)
        {
            return;
        }

        Stop(out, settings, index, offset);
        mVelocity[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(velocity);

        if (settings.mpe)
        {
            mChannel[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(AllocateChannel());
            // The bend goes first, so the note starts where the string is.
            SendBend(out, settings, index, pitch, offset, true);
        }

        if (const int note = OutputNote(settings, index); note >= 0)
        {
            Push(out, NoteEvent::Type::NoteOn, note, velocity, 0, offset, Channel(settings, index));
        }
    }

    void Bend(NoteBlock& out, const Settings& settings, int index, double pitch, int offset)
    {
        if (settings.mpe && index >= 0 && index < PolyNoteDetector::kNotes &&
            mVelocity[static_cast<std::size_t>(index)] != 0)
        {
            SendBend(out, settings, index, pitch, offset, false);
        }
    }

    /// `releasePitch`, with mpe: where the note's release should sound, its channel bent there
    /// before the note-off. NaN leaves the bend as it is.
    void Stop(NoteBlock& out, const Settings& settings, int index, int offset,
              double releasePitch = std::numeric_limits<double>::quiet_NaN())
    {
        if (index < 0 || index >= PolyNoteDetector::kNotes || mVelocity[static_cast<std::size_t>(index)] == 0)
        {
            return;
        }

        if (settings.mpe && IsFinite(releasePitch))
        {
            SendBend(out, settings, index, releasePitch, offset, false);
        }

        mVelocity[static_cast<std::size_t>(index)] = 0;

        if (const int note = OutputNote(settings, index); note >= 0)
        {
            Push(out, NoteEvent::Type::NoteOff, note, 0, 0, offset, Channel(settings, index));
        }

        if (settings.mpe)
        {
            // The channel's bend stays: the note's release is still sounding there.
            ReleaseChannel(mChannel[static_cast<std::size_t>(index)]);
            mChannel[static_cast<std::size_t>(index)] = 0;
        }
    }

    /// Note-offs for the whole chord, which stays on record as sounding...
    void SendOffs(NoteBlock& out, const Settings& settings, int offset) const
    {
        for (int index = 0; index < PolyNoteDetector::kNotes; ++index)
        {
            if (const int note = OutputNote(settings, index);
                mVelocity[static_cast<std::size_t>(index)] != 0 && note >= 0)
            {
                Push(out, NoteEvent::Type::NoteOff, note, 0, 0, offset, Channel(settings, index));
            }
        }
    }

    /// ...and note-ons for it again, under `settings` now, each on the channel it had and at the
    /// pitch the detector has it at.
    void SendOns(NoteBlock& out, const Settings& settings, const PolyNoteDetector& detector, int offset)
    {
        for (int index = 0; index < PolyNoteDetector::kNotes; ++index)
        {
            if (mVelocity[static_cast<std::size_t>(index)] == 0)
            {
                continue;
            }

            if (settings.mpe)
            {
                SendBend(out, settings, index, detector.Pitch(PolyNoteDetector::kLowestNote + index), offset, true);
            }

            if (const int note = OutputNote(settings, index); note >= 0)
            {
                Push(out, NoteEvent::Type::NoteOn, note, mVelocity[static_cast<std::size_t>(index)], 0, offset,
                     Channel(settings, index));
            }
        }
    }

    /// What the block holds at its end: the chord, and with mpe each member channel's bend and
    /// the zone.
    void Hold(NoteBlock& out, const Settings& settings) const
    {
        out.ClearChord();

        for (int index = 0; index < PolyNoteDetector::kNotes; ++index)
        {
            if (const int note = OutputNote(settings, index);
                mVelocity[static_cast<std::size_t>(index)] != 0 && note >= 0)
            {
                out.HoldChordNote(note, mVelocity[static_cast<std::size_t>(index)],
                                  settings.mpe ? mChannel[static_cast<std::size_t>(index)] : -1);
            }
        }

        if (settings.mpe)
        {
            for (int c = 0; c < kMpeMembers; ++c)
            {
                out.HoldChannelBend(kMpeFirstMember + c, mChannelBend[static_cast<std::size_t>(c)]);
            }
        }

        out.mpeMemberChannels = static_cast<std::uint8_t>(settings.mpe ? kMpeMembers : 0);
        out.mpeBendRange = static_cast<std::uint8_t>(settings.mpe ? std::lround(settings.bendRange) : 0);
    }

    [[nodiscard]] int Count() const
    {
        return static_cast<int>(
            std::count_if(mVelocity.begin(), mVelocity.end(), [](std::uint8_t velocity) { return velocity != 0; }));
    }

    /// The lowest sounding note as it goes out, or -1.
    [[nodiscard]] int Lowest(const Settings& settings) const
    {
        for (int index = 0; index < PolyNoteDetector::kNotes; ++index)
        {
            if (mVelocity[static_cast<std::size_t>(index)] != 0)
            {
                return OutputNote(settings, index);
            }
        }

        return -1;
    }

  private:
    /// The MIDI note the detector's note `index` plays as, or -1 when Transpose takes it out of range.
    [[nodiscard]] static int OutputNote(const Settings& settings, int index)
    {
        const int note = PolyNoteDetector::kLowestNote + index + settings.transpose;
        return (note >= 0 && note <= 127) ? note : -1;
    }

    /// The channel note `index` plays on: its own member channel with mpe, or MIDI Channel.
    [[nodiscard]] int Channel(const Settings& settings, int index) const
    {
        return settings.mpe ? mChannel[static_cast<std::size_t>(index)] : settings.channel;
    }

    static void Push(NoteBlock& out, NoteEvent::Type type, int note, int velocity, int bend, int offset, int channel)
    {
        NoteEvent event;
        event.type = type;
        event.channel = static_cast<std::uint8_t>(channel & 0x0F);
        event.note = static_cast<std::uint8_t>(std::clamp(note, 0, 127));
        event.velocity = static_cast<std::uint8_t>(std::clamp(velocity, 0, 127));
        event.bend = static_cast<std::int16_t>(bend);
        event.sampleOffset = offset;
        // A full block drops the event but not the change: what the block holds at its end says
        // where things stand, and the player catches up from that.
        out.Push(event);
    }

    /// A member channel for a new note: a free one, the one longest free so that it is done with
    /// its last note's release; with all fifteen in use, the one with fewest notes.
    [[nodiscard]] int AllocateChannel()
    {
        int best = 0;

        for (int c = 1; c < kMpeMembers; ++c)
        {
            const auto i = static_cast<std::size_t>(c);
            const auto b = static_cast<std::size_t>(best);

            if (mChannelUsers[i] < mChannelUsers[b] ||
                (mChannelUsers[i] == mChannelUsers[b] && mChannelFreedAt[i] < mChannelFreedAt[b]))
            {
                best = c;
            }
        }

        ++mChannelUsers[static_cast<std::size_t>(best)];
        return kMpeFirstMember + best;
    }

    void ReleaseChannel(int channel)
    {
        const auto c = static_cast<std::size_t>(channel - kMpeFirstMember);

        if (c < mChannelUsers.size() && mChannelUsers[c] > 0)
        {
            --mChannelUsers[c];
            mChannelFreedAt[c] = ++mClock;
        }
    }

    /// Sends the bend that puts note `index` at `pitch`, on its channel, unless within the deadband.
    void SendBend(NoteBlock& out, const Settings& settings, int index, double pitch, int offset, bool always)
    {
        const int channel = mChannel[static_cast<std::size_t>(index)];
        const auto c = static_cast<std::size_t>(channel - kMpeFirstMember);

        if (c >= mChannelBend.size())
        {
            return;
        }

        const int bend = BendValue(pitch - (PolyNoteDetector::kLowestNote + index), settings.bendRange);

        if (bend == mChannelBend[c] || (!always && std::abs(bend - mChannelBend[c]) < BendDeadband(settings.bendRange)))
        {
            return;
        }

        Push(out, NoteEvent::Type::PitchBend, 0, 0, bend, offset, channel);
        mChannelBend[c] = static_cast<std::int16_t>(bend);
    }

    /// By the detector's note index: the velocity each sounding note went out with (0 for one
    /// that is off), and with mpe its member channel (0 for none).
    std::array<std::uint8_t, PolyNoteDetector::kNotes> mVelocity{};
    std::array<std::uint8_t, PolyNoteDetector::kNotes> mChannel{};
    /// By member channel: how many notes it has, when it was last let go, and the bend it holds.
    std::array<std::uint8_t, kMpeMembers> mChannelUsers{};
    std::array<std::uint64_t, kMpeMembers> mChannelFreedAt{};
    std::array<std::int16_t, kMpeMembers> mChannelBend{};
    std::uint64_t mClock = 0;
};
} // namespace guitarfx::guitar_to_midi
