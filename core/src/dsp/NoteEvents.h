#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>

namespace guitarfx
{
/**
 * Notes passed between nodes of a signal graph: from a node that makes them (Guitar to MIDI) to
 * the nodes downstream of it that play them (a hosted instrument).
 *
 * A source fills a NoteBlock every block it runs: the events it decided on, in time order, and
 * the note (or the chord) it is holding once the block is over. The executor hands a player the blocks of every
 * source upstream of it that ran this block (SignalGraphExecutor, "Note routing"), and the
 * player's NotePlayer turns them into the MIDI its instrument receives.
 *
 * The held note is what keeps a note from hanging. A player that missed an event, because its
 * instrument was busy for a block, a source was bypassed or removed, or the player itself was
 * bypassed and has just come back, works out what should be sounding from the held notes of the
 * sources it can see and sends whatever note-offs, note-ons and bends put its instrument there.
 * A source that did not run this block holds nothing.
 *
 * Everything here is fixed-size, so none of it allocates on the audio thread.
 */
struct NoteEvent
{
    enum class Type : std::uint8_t
    {
        NoteOn,
        NoteOff,
        PitchBend
    };

    Type type = Type::NoteOn;
    std::uint8_t channel = 0;  ///< 0 to 15
    std::uint8_t note = 0;     ///< 0 to 127
    std::uint8_t velocity = 0; ///< 1 to 127 for a note-on
    std::int16_t bend = 0;     ///< -8192 to 8191, for a pitch bend
    int sampleOffset = 0;      ///< within the block
};

/// What a note source made in one block. See the file comment.
struct NoteBlock
{
    static constexpr int kMaxEvents = 64;
    static constexpr std::int16_t kMinBend = -8192;
    static constexpr std::int16_t kMaxBend = 8191;

    std::array<NoteEvent, kMaxEvents> events{};
    int count = 0;

    /// The note held once the block is over, or -1, with its channel and velocity; and the
    /// source's pitch bend on that channel, which stays where it was last sent even between notes.
    int heldNote = -1;
    std::uint8_t heldVelocity = 0;
    std::uint8_t channel = 0;
    std::int16_t bend = 0;

    /// A source that holds several notes at once (a chord) lists them here and leaves heldNote at
    /// -1; a player takes the two together. Each has its own channel, the block's channel unless
    /// the source spreads its notes over channels (MPE).
    static constexpr int kMaxChord = 12;
    std::array<std::uint8_t, kMaxChord> chordNotes{};
    std::array<std::uint8_t, kMaxChord> chordVelocities{};
    std::array<std::uint8_t, kMaxChord> chordChannels{}; ///< see ChordChannel()
    int chordCount = 0;

    /// A source that bends channels other than its own (MPE: a channel per note) holds each one's
    /// bend here, with its bit set in channelBendMask; like `bend`, it stays where it was last sent.
    std::array<std::int16_t, 16> channelBends{};
    std::uint16_t channelBendMask = 0;

    /// MPE: a source playing an MPE lower zone (manager channel 1, as channel 0 here) sets how
    /// many member channels follow it, 1 to 15, and their pitch bend range in semitones; 0 for
    /// none. A player tells its instrument so (MIDI's MPE Configuration Message and each member
    /// channel's pitch bend sensitivity).
    std::uint8_t mpeMemberChannels = 0;
    std::uint8_t mpeBendRange = 0;

    void Clear()
    {
        count = 0;
    }

    void ClearChord()
    {
        chordCount = 0;
        channelBendMask = 0;
    }

    /// Adds a held chord note unless the list is full; on `channel`, or the block's channel.
    bool HoldChordNote(int note, int velocity, int noteChannel = -1)
    {
        if (chordCount >= kMaxChord || note < 0 || note > 127)
        {
            return false;
        }

        chordNotes[static_cast<std::size_t>(chordCount)] = static_cast<std::uint8_t>(note);
        chordVelocities[static_cast<std::size_t>(chordCount)] = static_cast<std::uint8_t>(std::clamp(velocity, 1, 127));
        chordChannels[static_cast<std::size_t>(chordCount)] =
            noteChannel >= 0 ? static_cast<std::uint8_t>(noteChannel & 0x0F) : kBlockChannel;
        ++chordCount;
        return true;
    }

    /// A chord note's channel: its own, or the block's as it stands.
    [[nodiscard]] int ChordChannel(int index) const
    {
        const std::uint8_t c = chordChannels[static_cast<std::size_t>(index)];
        return c == kBlockChannel ? (channel & 0x0F) : (c & 0x0F);
    }

    static constexpr std::uint8_t kBlockChannel = 0xFF;

    /// Holds `value` as the bend on `bendChannel` (MPE).
    void HoldChannelBend(int bendChannel, std::int16_t value)
    {
        const int c = bendChannel & 0x0F;
        channelBends[static_cast<std::size_t>(c)] = value;
        channelBendMask = static_cast<std::uint16_t>(channelBendMask | (1u << c));
    }

    /// Appends `event` unless the block is full. The held note is not touched: a caller updates it
    /// once the event has been added, so what is held always matches what was sent.
    bool Push(const NoteEvent& event)
    {
        if (count >= kMaxEvents)
        {
            return false;
        }

        events[static_cast<std::size_t>(count)] = event;
        ++count;
        return true;
    }

    [[nodiscard]] std::span<const NoteEvent> Events() const
    {
        return {events.data(), static_cast<std::size_t>(count)};
    }
};

/// A three-byte MIDI channel message for a player to deliver.
struct NoteMidiMessage
{
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    int sampleOffset = 0;
};

/**
 * The player's side: the MIDI an instrument should receive this block, from the NoteBlocks of
 * the sources feeding it.
 *
 * Build() merges the sources' events in time order and then reconciles, at the end of the block,
 * against their held notes: see the file comment. Commit() once the messages have reached the
 * instrument; a Build() that is never committed (the instrument skipped the block) is simply
 * rebuilt from the same starting point next time, and reconciliation catches up what was missed.
 *
 * It tracks the notes it has sent per channel and note number, so two sources on one channel
 * share them: a note-on for a note already sounding restarts it, and a note-off turns it off even
 * if the other source still holds it, until reconciliation sends it again at the end of the block.
 * Pitch bend is per channel, and the last source to bend a channel sets it.
 */
class NotePlayer
{
  public:
    static constexpr int kMaxMessages = 128;
    static constexpr int kMaxSources = 8;

    /// The messages for a block of `numSamples`. The span stays valid until the next Build().
    [[nodiscard]] std::span<const NoteMidiMessage> Build(std::span<const NoteBlock* const> sources, int numSamples)
    {
        mCount = 0;
        mDropped = 0;
        mPendingSounding = mSounding;
        mPendingBend = mBend;
        mPendingMpe = mMpe;
        const int last = std::max(0, numSamples - 1);

        if (mReleaseAll)
        {
            ReleaseAll(0);
        }

        const auto sourceCount = std::min<std::size_t>(sources.size(), kMaxSources);
        ConfigureMpe(sources.first(sourceCount));
        MergeEvents(sources.first(sourceCount), last);
        Reconcile(sources.first(sourceCount), last);
        return {mMessages.data(), static_cast<std::size_t>(mCount)};
    }

    /// The last Build()'s messages reached the instrument.
    void Commit()
    {
        mSounding = mPendingSounding;
        mBend = mPendingBend;
        mMpe = mPendingMpe;
        mReleaseAll = false;
    }

    /// The next Build() starts by turning off every note sent and centring every bend: for an
    /// instrument that has been reset, which may or may not have stopped its voices. An MPE zone
    /// is set up again too, in case the reset forgot it.
    void ReleaseAllNext()
    {
        mReleaseAll = true;

        if (mMpe != 0)
        {
            mMpe = kMpeUnknown;
        }
    }

    /// The instrument has nothing sounding: a new instance. Nothing is sent to turn anything off,
    /// and reconciliation starts whatever the sources hold.
    void Forget()
    {
        mSounding = {};
        mBend = {};
        mMpe = 0;
        mReleaseAll = false;
    }

    /// The MPE zone the instrument was last told of: member channels in the high byte, bend range
    /// in the low; 0 for none.
    [[nodiscard]] std::uint16_t MpeZone() const
    {
        return mMpe;
    }

    [[nodiscard]] bool IsSounding(int channel, int note) const
    {
        return Test(mSounding, channel, note);
    }

    [[nodiscard]] std::int16_t Bend(int channel) const
    {
        return (channel >= 0 && channel < kChannels) ? mBend[static_cast<std::size_t>(channel)] : 0;
    }

    /// Messages the last Build() had no room for; reconciliation sends them later.
    [[nodiscard]] int Dropped() const
    {
        return mDropped;
    }

  private:
    static constexpr int kChannels = 16;
    static constexpr int kNotes = 128;
    static constexpr std::uint16_t kMpeUnknown = 0xFFFF;
    static constexpr int kWords = kChannels * kNotes / 64;
    using NoteSet = std::array<std::uint64_t, kWords>;

    [[nodiscard]] static bool Test(const NoteSet& set, int channel, int note)
    {
        const int key = channel * kNotes + note;
        return (set[static_cast<std::size_t>(key / 64)] >> (key % 64)) & 1u;
    }

    static void Assign(NoteSet& set, int channel, int note, bool on)
    {
        const int key = channel * kNotes + note;
        const std::uint64_t bit = std::uint64_t{1} << (key % 64);
        auto& word = set[static_cast<std::size_t>(key / 64)];
        word = on ? (word | bit) : (word & ~bit);
    }

    bool Push(std::uint8_t status, std::uint8_t data1, std::uint8_t data2, int offset)
    {
        if (mCount >= kMaxMessages)
        {
            ++mDropped;
            return false;
        }

        mMessages[static_cast<std::size_t>(mCount)] = {status, data1, data2, offset};
        ++mCount;
        return true;
    }

    void NoteOff(int channel, int note, int offset)
    {
        if (Test(mPendingSounding, channel, note) &&
            Push(static_cast<std::uint8_t>(0x80 | channel), static_cast<std::uint8_t>(note), 0, offset))
        {
            Assign(mPendingSounding, channel, note, false);
        }
    }

    void NoteOn(int channel, int note, int velocity, int offset)
    {
        if (Test(mPendingSounding, channel, note))
        {
            NoteOff(channel, note, offset);

            if (Test(mPendingSounding, channel, note))
            {
                return; // no room to restart it; reconciliation will
            }
        }

        const auto clamped = static_cast<std::uint8_t>(std::clamp(velocity, 1, 127));

        if (Push(static_cast<std::uint8_t>(0x90 | channel), static_cast<std::uint8_t>(note), clamped, offset))
        {
            Assign(mPendingSounding, channel, note, true);
        }
    }

    void PitchBend(int channel, std::int16_t bend, int offset)
    {
        auto& current = mPendingBend[static_cast<std::size_t>(channel)];

        if (current == bend)
        {
            return;
        }

        const int value = std::clamp(static_cast<int>(bend), static_cast<int>(NoteBlock::kMinBend),
                                     static_cast<int>(NoteBlock::kMaxBend)) +
                          8192;

        if (Push(static_cast<std::uint8_t>(0xE0 | channel), static_cast<std::uint8_t>(value & 0x7F),
                 static_cast<std::uint8_t>((value >> 7) & 0x7F), offset))
        {
            current = bend;
        }
    }

    void Control(int channel, int controller, int value)
    {
        Push(static_cast<std::uint8_t>(0xB0 | (channel & 0x0F)), static_cast<std::uint8_t>(controller & 0x7F),
             static_cast<std::uint8_t>(value & 0x7F), 0);
    }

    /// Registered parameter `number` (MSB, LSB) on `channel` set to `value` (and `fine`, when not
    /// negative).
    void Rpn(int channel, int msb, int lsb, int value, int fine)
    {
        Control(channel, 101, msb);
        Control(channel, 100, lsb);
        Control(channel, 6, value);

        if (fine >= 0)
        {
            Control(channel, 38, fine);
        }
    }

    void NullRpn(int channel)
    {
        Control(channel, 101, 127);
        Control(channel, 100, 127);
    }

    /// Tells the instrument of the MPE zone the sources ask for, if it differs from what it was
    /// told last: the MPE Configuration Message (RPN 6 on the manager channel) and then, as MPE
    /// asks, each member channel's pitch bend sensitivity (RPN 0). All of it or, short of room,
    /// none of it until the next block.
    void ConfigureMpe(std::span<const NoteBlock* const> sources)
    {
        std::uint16_t wanted = 0;

        for (const NoteBlock* block : sources)
        {
            if (block && block->mpeMemberChannels > 0)
            {
                const int members = std::min<int>(block->mpeMemberChannels, kChannels - 1);
                wanted = static_cast<std::uint16_t>(members << 8 | std::min<int>(block->mpeBendRange, 127));
                break;
            }
        }

        if (wanted == mPendingMpe)
        {
            return;
        }

        const int members = wanted >> 8;
        const int needed = 5 + 6 * members;

        if (mCount + needed > kMaxMessages)
        {
            mDropped += needed;
            return;
        }

        Rpn(0, 0, 6, members, -1);
        NullRpn(0);

        for (int channel = 1; channel <= members; ++channel)
        {
            Rpn(channel, 0, 0, wanted & 0xFF, 0);
            NullRpn(channel);
        }

        mPendingMpe = wanted;
    }

    void ReleaseAll(int offset)
    {
        for (int channel = 0; channel < kChannels; ++channel)
        {
            for (int note = 0; note < kNotes; ++note)
            {
                NoteOff(channel, note, offset);
            }

            PitchBend(channel, 0, offset);
        }
    }

    /// Every source's events, in time order; a stable merge, so simultaneous events keep the
    /// order their source gave them, and a lower-numbered source goes first.
    void MergeEvents(std::span<const NoteBlock* const> sources, int last)
    {
        std::array<int, kMaxSources> next{};

        while (true)
        {
            int best = -1;
            int bestOffset = 0;

            for (std::size_t s = 0; s < sources.size(); ++s)
            {
                const NoteBlock* block = sources[s];

                if (!block || next[s] >= std::min(block->count, NoteBlock::kMaxEvents))
                {
                    continue;
                }

                const int offset = block->events[static_cast<std::size_t>(next[s])].sampleOffset;

                if (best < 0 || offset < bestOffset)
                {
                    best = static_cast<int>(s);
                    bestOffset = offset;
                }
            }

            if (best < 0)
            {
                return;
            }

            const NoteEvent& event = sources[static_cast<std::size_t>(best)]
                                         ->events[static_cast<std::size_t>(next[static_cast<std::size_t>(best)]++)];
            const int offset = std::clamp(event.sampleOffset, 0, last);
            const int channel = event.channel & 0x0F;
            const int note = event.note & 0x7F;

            switch (event.type)
            {
            case NoteEvent::Type::NoteOn:
                NoteOn(channel, note, event.velocity, offset);
                break;

            case NoteEvent::Type::NoteOff:
                NoteOff(channel, note, offset);
                break;

            case NoteEvent::Type::PitchBend:
                PitchBend(channel, event.bend, offset);
                break;
            }
        }
    }

    /// Brings what has been sent into line with what the sources hold: bends first, so a note
    /// started here starts at its source's pitch, then note-offs, then note-ons.
    void Reconcile(std::span<const NoteBlock* const> sources, int last)
    {
        NoteSet held{};
        std::array<std::int16_t, kChannels> bend{};
        std::array<std::uint8_t, kChannels * kNotes> velocity{};

        for (const NoteBlock* block : sources)
        {
            if (!block)
            {
                continue;
            }

            const int channel = block->channel & 0x0F;
            bend[static_cast<std::size_t>(channel)] = block->bend;

            for (int c = 0; c < kChannels; ++c)
            {
                if ((block->channelBendMask >> c) & 1u)
                {
                    bend[static_cast<std::size_t>(c)] = block->channelBends[static_cast<std::size_t>(c)];
                }
            }

            if (block->heldNote >= 0 && block->heldNote < kNotes)
            {
                Assign(held, channel, block->heldNote, true);
                velocity[static_cast<std::size_t>(channel * kNotes + block->heldNote)] = block->heldVelocity;
            }

            for (int i = 0; i < std::min(block->chordCount, NoteBlock::kMaxChord); ++i)
            {
                const int note = block->chordNotes[static_cast<std::size_t>(i)] & 0x7F;
                const int noteChannel = block->ChordChannel(i);
                Assign(held, noteChannel, note, true);
                velocity[static_cast<std::size_t>(noteChannel * kNotes + note)] =
                    block->chordVelocities[static_cast<std::size_t>(i)];
            }
        }

        for (int channel = 0; channel < kChannels; ++channel)
        {
            PitchBend(channel, bend[static_cast<std::size_t>(channel)], last);
        }

        for (int word = 0; word < kWords; ++word)
        {
            std::uint64_t extra =
                mPendingSounding[static_cast<std::size_t>(word)] & ~held[static_cast<std::size_t>(word)];

            while (extra != 0)
            {
                const int bit = std::countr_zero(extra);
                extra &= extra - 1;
                const int key = word * 64 + bit;
                NoteOff(key / kNotes, key % kNotes, last);
            }
        }

        for (int word = 0; word < kWords; ++word)
        {
            std::uint64_t missing =
                held[static_cast<std::size_t>(word)] & ~mPendingSounding[static_cast<std::size_t>(word)];

            while (missing != 0)
            {
                const int bit = std::countr_zero(missing);
                missing &= missing - 1;
                const int key = word * 64 + bit;
                NoteOn(key / kNotes, key % kNotes, velocity[static_cast<std::size_t>(key)], last);
            }
        }
    }

    NoteSet mSounding{};
    NoteSet mPendingSounding{};
    std::array<std::int16_t, kChannels> mBend{};
    std::array<std::int16_t, kChannels> mPendingBend{};
    std::uint16_t mMpe = 0;
    std::uint16_t mPendingMpe = 0;
    std::array<NoteMidiMessage, kMaxMessages> mMessages{};
    int mCount = 0;
    int mDropped = 0;
    bool mReleaseAll = false;
};
} // namespace guitarfx
