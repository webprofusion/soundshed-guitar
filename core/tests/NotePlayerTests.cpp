/**
 * @file NotePlayerTests.cpp
 * @brief Tests for the note player a hosted instrument uses (NotePlayer in dsp/NoteEvents.h).
 *
 *   - two sources' events are merged in time order
 *   - what was sent is reconciled with what the sources hold: a source that stops running has its
 *     note turned off, a dropped block's note-on is sent in the next, an instrument that was reset
 *     has everything turned off and the held note started again, and a new one is sent the held
 *     note with nothing to turn off
 *   - pitch bend goes out at 14 bits and is centred when its source goes
 *   - a block with more than it has room for drops the rest, and the next brings it back
 *   - a source holding a chord has every note of it reconciled: started when an event was missed,
 *     turned off one by one as the chord thins, and all let go when the source stops running
 *   - an MPE source has its zone and member bend ranges sent before any note, again after a reset,
 *     and ended when it stops; each chord note plays on its own channel, after that channel's bend
 */

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "dsp/NoteEvents.h"

namespace
{
using guitarfx::NoteBlock;
using guitarfx::NoteEvent;
using guitarfx::NotePlayer;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

NoteBlock Holding(int note, int channel = 0, int velocity = 90)
{
    NoteBlock block;
    block.heldNote = note;
    block.heldVelocity = static_cast<std::uint8_t>(velocity);
    block.channel = static_cast<std::uint8_t>(channel);
    return block;
}

void PushOn(NoteBlock& block, int note, int offset, int channel = 0)
{
    NoteEvent on;
    on.type = NoteEvent::Type::NoteOn;
    on.channel = static_cast<std::uint8_t>(channel);
    on.note = static_cast<std::uint8_t>(note);
    on.velocity = 90;
    on.sampleOffset = offset;
    block.Push(on);
}

void TestNotePlayer()
{
    std::cout << "\nThe note player" << std::endl;

    // Two sources merged in time order.
    NotePlayer player;
    NoteBlock first = Holding(60);
    PushOn(first, 60, 40);
    NoteBlock second = Holding(64, 1);
    PushOn(second, 64, 10, 1);
    const NoteBlock* both[2] = {&first, &second};
    const auto merged = player.Build(both, 64);
    Check(merged.size() == 2 && merged[0].data1 == 64 && merged[0].sampleOffset == 10 && merged[1].data1 == 60 &&
              merged[1].sampleOffset == 40,
          "two sources' events come out in time order");
    player.Commit();

    // One source goes away: its note is turned off at the end of the block.
    const NoteBlock* one[1] = {&second};
    NoteBlock quiet = Holding(64, 1);
    one[0] = &quiet;
    const auto gone = player.Build(one, 64);
    Check(gone.size() == 1 && gone[0].status == 0x80 && gone[0].data1 == 60 && gone[0].sampleOffset == 63,
          "a source that stops running has its note turned off");
    player.Commit();

    // A block the instrument never got: the next block catches up from what the source holds.
    NotePlayer dropped;
    NoteBlock start = Holding(57);
    PushOn(start, 57, 5);
    const NoteBlock* startSource[1] = {&start};
    (void)dropped.Build(startSource, 64); // not committed
    NoteBlock holding = Holding(57);
    const NoteBlock* holdingSource[1] = {&holding};
    const auto caughtUp = dropped.Build(holdingSource, 64);
    Check(caughtUp.size() == 1 && caughtUp[0].status == 0x90 && caughtUp[0].data1 == 57,
          "a dropped block's note-on is sent in the next");
    dropped.Commit();

    dropped.ReleaseAllNext();
    const auto released = dropped.Build(holdingSource, 64);
    Check(released.size() == 2 && released[0].status == 0x80 && released[0].sampleOffset == 0 &&
              released[1].status == 0x90,
          "after a reset every note is turned off, and what is still held starts again");
    dropped.Commit();

    dropped.Forget();
    const auto fresh = dropped.Build(holdingSource, 64);
    Check(fresh.size() == 1 && fresh[0].status == 0x90,
          "a new instrument is sent the held note and nothing to turn off");
    dropped.Commit();

    // Pitch bend: scaled to 14 bits, and centred again when its source goes.
    NotePlayer bender;
    NoteBlock bent = Holding(60);
    NoteEvent bend;
    bend.type = NoteEvent::Type::PitchBend;
    bend.bend = 4096;
    bent.Push(bend);
    bent.bend = 4096;
    const NoteBlock* bentSource[1] = {&bent};
    const auto bendOut = bender.Build(bentSource, 64);
    const bool scaled = std::any_of(bendOut.begin(), bendOut.end(), [](const guitarfx::NoteMidiMessage& m) {
        return m.status == 0xE0 && (m.data1 | (m.data2 << 7)) == 8192 + 4096;
    });
    bender.Commit();
    const auto centred = bender.Build({}, 64);
    const bool back = std::any_of(centred.begin(), centred.end(), [](const guitarfx::NoteMidiMessage& m) {
        return m.status == 0xE0 && (m.data1 | (m.data2 << 7)) == 8192;
    });
    Check(scaled && back, "a bend goes out at 14 bits, and is centred when its source goes");
    bender.Commit();

    // More than a block's room: what did not fit is caught up later.
    NotePlayer crowded;
    std::vector<NoteBlock> many(NotePlayer::kMaxSources);
    std::vector<const NoteBlock*> pointers;

    for (int s = 0; s < NotePlayer::kMaxSources; ++s)
    {
        many[static_cast<std::size_t>(s)] = Holding(s, s);

        for (int e = 0; e < NoteBlock::kMaxEvents; ++e)
        {
            PushOn(many[static_cast<std::size_t>(s)], s, e, s); // each after the first restarts it: off + on
        }

        pointers.push_back(&many[static_cast<std::size_t>(s)]);
    }

    const auto full = crowded.Build(pointers, 64);
    Check(full.size() == NotePlayer::kMaxMessages && crowded.Dropped() > 0, "a crowded block fills and drops the rest",
          std::to_string(crowded.Dropped()) + " dropped");
    crowded.Commit();
    std::vector<NoteBlock> steady;

    for (int s = 0; s < NotePlayer::kMaxSources; ++s)
    {
        steady.push_back(Holding(s, s));
    }

    std::vector<const NoteBlock*> steadyPointers;

    for (const auto& block : steady)
    {
        steadyPointers.push_back(&block);
    }

    (void)crowded.Build(steadyPointers, 64);
    crowded.Commit();
    bool allOn = true;

    for (int s = 0; s < NotePlayer::kMaxSources; ++s)
    {
        allOn = allOn && crowded.IsSounding(s, s);
    }

    Check(allOn, "and the next block brings every held note back");
}

void TestChords()
{
    std::cout << "\nChords" << std::endl;
    NotePlayer player;
    NoteBlock chord;
    chord.channel = 2;
    chord.HoldChordNote(52, 80);
    chord.HoldChordNote(59, 70);
    chord.HoldChordNote(64, 60);
    const NoteBlock* sources[1] = {&chord};

    // No events at all, as after a dropped block: the held chord alone starts every note.
    const auto first = player.Build(sources, 64);
    int ons = 0;
    bool velocities = true;

    for (const auto& m : first)
    {
        if ((m.status & 0xF0) == 0x90 && (m.status & 0x0F) == 2)
        {
            ++ons;
            velocities = velocities && m.data2 == (m.data1 == 52 ? 80 : m.data1 == 59 ? 70 : 60);
        }
    }

    player.Commit();
    Check(ons == 3 && first.size() == 3 && velocities, "a held chord starts each of its notes, at its own velocity",
          std::to_string(ons) + " note-ons of " + std::to_string(first.size()) + " messages");
    Check(player.IsSounding(2, 52) && player.IsSounding(2, 59) && player.IsSounding(2, 64),
          "and all three are sounding");

    const auto steady = player.Build(sources, 64);
    player.Commit();
    Check(steady.empty(), "a chord that is still held sends nothing more");

    // One string muted: that note alone goes off.
    chord.ClearChord();
    chord.HoldChordNote(52, 80);
    chord.HoldChordNote(64, 60);
    const auto thinned = player.Build(sources, 64);
    player.Commit();
    Check(thinned.size() == 1 && thinned[0].status == (0x80 | 2) && thinned[0].data1 == 59,
          "a note dropped from the chord is turned off, and only that one");

    // A single held note beside the chord is one more note of the set.
    chord.heldNote = 40;
    chord.heldVelocity = 100;
    const auto withBass = player.Build(sources, 64);
    player.Commit();
    Check(withBass.size() == 1 && withBass[0].status == (0x90 | 2) && withBass[0].data1 == 40 &&
              player.IsSounding(2, 40) && player.IsSounding(2, 52),
          "the single held note and the chord are taken together");

    Check(!chord.HoldChordNote(128, 90) && !chord.HoldChordNote(-1, 90) && chord.chordCount == 2,
          "a note outside 0 to 127 is not held");

    for (int extra = 0; extra < NoteBlock::kMaxChord + 4; ++extra)
    {
        chord.HoldChordNote(70 + extra, 90);
    }

    Check(chord.chordCount == NoteBlock::kMaxChord, "and the chord holds no more than it has room for");

    // The source stops running: everything it held is let go.
    const auto gone = player.Build({}, 64);
    player.Commit();
    int offs = 0;

    for (const auto& m : gone)
    {
        offs += (m.status & 0xF0) == 0x80 ? 1 : 0;
    }

    Check(offs == 3 && !player.IsSounding(2, 40) && !player.IsSounding(2, 52) && !player.IsSounding(2, 64),
          "a chord's source that stops running has every note turned off", std::to_string(offs) + " note-offs");
}

/// The value sent for RPN `msb`/`lsb` on `channel` in `messages`, or -1.
template <typename Messages> int RpnValue(const Messages& messages, int channel, int msb, int lsb)
{
    int selectedMsb = 127;
    int selectedLsb = 127;
    int value = -1;

    for (const auto& m : messages)
    {
        if (m.status != (0xB0 | channel))
        {
            continue;
        }

        selectedMsb = m.data1 == 101 ? m.data2 : selectedMsb;
        selectedLsb = m.data1 == 100 ? m.data2 : selectedLsb;
        value = (m.data1 == 6 && selectedMsb == msb && selectedLsb == lsb) ? m.data2 : value;
    }

    return value;
}

void TestMpe()
{
    std::cout << "\nMPE" << std::endl;
    NotePlayer player;
    NoteBlock block;
    block.mpeMemberChannels = 15;
    block.mpeBendRange = 48;
    block.HoldChordNote(52, 80, 1);
    block.HoldChordNote(59, 70, 2);
    block.HoldChannelBend(1, 1000);
    const NoteBlock* sources[1] = {&block};

    // No events, as after a dropped block: the zone, then each channel's bend, then its note.
    const auto first = player.Build(sources, 64);
    player.Commit();
    std::size_t firstNote = first.size();
    std::size_t bendAt = first.size();
    int ons = 0;

    for (std::size_t i = 0; i < first.size(); ++i)
    {
        const auto& m = first[i];
        bendAt = m.status == 0xE1 && bendAt == first.size() ? i : bendAt;

        if ((m.status & 0xF0) == 0x90)
        {
            firstNote = std::min(firstNote, i);
            ++ons;
        }
    }

    const std::vector<guitarfx::NoteMidiMessage> zone(first.begin(),
                                                      first.begin() + static_cast<std::ptrdiff_t>(firstNote));
    bool ranges = RpnValue(zone, 0, 0, 6) == 15;

    for (int channel = 1; channel <= 15; ++channel)
    {
        ranges = ranges && RpnValue(zone, channel, 0, 0) == 48;
    }

    Check(ranges && player.MpeZone() == (15 << 8 | 48),
          "the MPE zone and every member channel's bend range are sent before any note");
    Check(ons == 2 && player.IsSounding(1, 52) && player.IsSounding(2, 59),
          "each chord note starts on the channel it was held on");
    Check(bendAt < firstNote && first[bendAt].data1 + (first[bendAt].data2 << 7) - 8192 == 1000 &&
              player.Bend(1) == 1000,
          "and a channel's bend is sent before its note");

    const auto steady = player.Build(sources, 64);
    player.Commit();
    Check(steady.empty(), "nothing more while nothing changes, the zone included");

    player.ReleaseAllNext();
    const auto again = player.Build(sources, 64);
    player.Commit();
    Check(RpnValue(again, 0, 0, 6) == 15 && RpnValue(again, 15, 0, 0) == 48 && player.IsSounding(1, 52),
          "an instrument that was reset is told the zone again");

    block.ClearChord();
    block.mpeMemberChannels = 0;
    block.HoldChordNote(52, 80);
    const auto plain = player.Build(sources, 64);
    player.Commit();
    Check(RpnValue(plain, 0, 0, 6) == 0 && player.MpeZone() == 0 && player.IsSounding(0, 52) &&
              !player.IsSounding(1, 52) && player.Bend(1) == 0,
          "a source that stops using MPE ends the zone, and a chord note held without a channel is on the block's",
          std::to_string(plain.size()) + " messages");
}
} // namespace

int main()
{
    std::cout << "Note player tests" << std::endl;
    TestNotePlayer();
    TestChords();
    TestMpe();
    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
