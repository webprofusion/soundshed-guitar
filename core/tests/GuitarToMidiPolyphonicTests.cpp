/**
 * @file GuitarToMidiPolyphonicTests.cpp
 * @brief Tests for Guitar to MIDI's Polyphonic mode (dsp/effects/GuitarToMidiEffect.h). The
 *        detector behind it has its own in PolyNoteDetectorTests.cpp.
 *
 *   - a chord comes out as its notes, sounding together, held as a chord for the player, with no
 *     pitch bend, and every note is turned off once the strings are muted
 *   - transpose and channel reach the chord; a change mid-chord ends it and starts it again there
 *   - leaving the mode, or a Reset, lets the chord go
 *   - Lowest Note rules out what is below the lowest string
 *   - the mono and stereo paths make the same notes
 *   - Polyphonic + Bend sets up an MPE zone and gives each note a member channel of its own, its
 *     bend sent before it starts; a bent or out-of-tune string's channel follows it, and an
 *     in-tune one's stays centred
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "GuitarPhraseSynth.h"
#include "GuitarToMidiTestSupport.h"
#include "dsp/NoteEvents.h"
#include "dsp/effects/GuitarToMidiEffect.h"

namespace
{
using guitarfx::GuitarToMidiEffect;
using guitarfx::NotePlayer;
using guitarfx::test::PhraseNote;
using guitarfx::test::RenderPhrase;
using guitarfx::test::g2m::AnythingSounding;
using guitarfx::test::g2m::kBlock;
using guitarfx::test::g2m::kRate;
using guitarfx::test::g2m::Midi;
using guitarfx::test::g2m::Note;
using guitarfx::test::g2m::NoteOns;
using guitarfx::test::g2m::Notes;
using guitarfx::test::g2m::Play;

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

/// One synthetic string per note, strummed 5 ms apart from `start` and muted `length` later.
std::vector<float> Chord(const std::vector<int>& notes, double start, double length, double seconds)
{
    std::vector<float> audio(static_cast<std::size_t>(seconds * kRate), 0.0f);

    for (std::size_t s = 0; s < notes.size(); ++s)
    {
        const auto string = RenderPhrase({Note(start + 0.005 * static_cast<double>(s), length, notes[s])}, seconds,
                                         kRate, static_cast<std::uint32_t>(21 + s));

        for (std::size_t n = 0; n < audio.size(); ++n)
        {
            audio[n] += string[n];
        }
    }

    return audio;
}

bool Contains(const std::vector<int>& notes, std::initializer_list<int> wanted)
{
    return std::all_of(wanted.begin(), wanted.end(),
                       [&](int note) { return std::find(notes.begin(), notes.end(), note) != notes.end(); });
}

void TestPolyphonicMode()
{
    std::cout << "\nPolyphonic mode" << std::endl;
    const auto audio = Chord({52, 59, 64}, 0.2, 0.7, 1.4);
    const std::size_t midChord = static_cast<std::size_t>(0.6 * kRate) / kBlock * kBlock;

    GuitarToMidiEffect fx;
    fx.SetParam("mode", 2.0);
    fx.Prepare(kRate, kBlock);
    NotePlayer player;
    int heldMid = 0;
    int singleMid = 0;
    double soundingMid = 0.0;
    double lowestMid = 0.0;
    bool allAtOnce = false;
    const auto midi = Play(fx, player, audio, [&](std::size_t start) {
        if (start == midChord)
        {
            heldMid = fx.GetNoteOutput()->chordCount;
            singleMid = fx.GetNoteOutput()->heldNote;
            soundingMid = fx.GetParam("soundingNotes");
            lowestMid = fx.GetParam("currentNote");
            allAtOnce = player.IsSounding(0, 52) && player.IsSounding(0, 59) && player.IsSounding(0, 64);
        }
    });
    const auto ons = NoteOns(midi);
    const bool noBend = std::none_of(midi.begin(), midi.end(), [](const Midi& m) { return m.Type() == 0xE0; });
    Check(Contains(ons, {52, 59, 64}) && ons.size() <= 4, "a chord comes out as its notes", Notes(midi));
    Check(allAtOnce, "sounding together");
    Check(heldMid >= 3 && singleMid == -1 && soundingMid >= 3.0 && lowestMid == 52.0,
          "held for the player as a chord, and reported as one",
          std::to_string(heldMid) + " held, lowest " + std::to_string(static_cast<int>(lowestMid)));
    Check(noBend, "with no pitch bend");
    Check(!AnythingSounding(player), "and every note is turned off once the strings are muted");

    GuitarToMidiEffect moved;
    moved.SetParam("mode", 2.0);
    moved.SetParam("transpose", 12.0);
    moved.SetParam("channel", 5.0);
    moved.Prepare(kRate, kBlock);
    NotePlayer movedPlayer;
    const auto movedMidi = Play(moved, movedPlayer, audio);
    const bool onChannel = !movedMidi.empty() && std::all_of(movedMidi.begin(), movedMidi.end(),
                                                             [](const Midi& m) { return m.Channel() == 4; });
    Check(Contains(NoteOns(movedMidi), {64, 71, 76}) && onChannel, "transpose and channel reach the chord",
          Notes(movedMidi));

    GuitarToMidiEffect changing;
    changing.SetParam("mode", 2.0);
    changing.Prepare(kRate, kBlock);
    NotePlayer changingPlayer;
    const auto changed = Play(changing, changingPlayer, audio, [&](std::size_t start) {
        if (start == midChord)
        {
            changing.SetParam("transpose", -12.0);
        }
    });
    int offOld = 0;
    int onNew = 0;

    for (const auto& m : changed)
    {
        if (m.sample == midChord)
        {
            offOld += (m.Type() == 0x80 && (m.data1 == 52 || m.data1 == 59 || m.data1 == 64)) ? 1 : 0;
            onNew += (m.Type() == 0x90 && (m.data1 == 40 || m.data1 == 47 || m.data1 == 52)) ? 1 : 0;
        }
    }

    Check(offOld == 3 && onNew == 3, "a new transpose mid-chord ends the chord and starts it again there",
          std::to_string(offOld) + " off, " + std::to_string(onNew) + " on");
    Check(!AnythingSounding(changingPlayer), "and those are turned off too");

    GuitarToMidiEffect switching;
    switching.SetParam("mode", 2.0);
    switching.Prepare(kRate, kBlock);
    NotePlayer switchingPlayer;
    bool releasedAtSwitch = false;
    (void)Play(switching, switchingPlayer, audio, [&](std::size_t start) {
        if (start == midChord)
        {
            switching.SetParam("mode", 0.0);
        }
        else if (start == midChord + kBlock)
        {
            releasedAtSwitch = !switchingPlayer.IsSounding(0, 52) && !switchingPlayer.IsSounding(0, 59) &&
                               !switchingPlayer.IsSounding(0, 64) && switching.GetNoteOutput()->chordCount == 0;
        }
    });
    Check(releasedAtSwitch, "leaving Polyphonic mode lets the chord go at once");

    GuitarToMidiEffect reset;
    reset.SetParam("mode", 2.0);
    reset.Prepare(kRate, kBlock);
    NotePlayer resetPlayer;
    bool resetDone = false;
    bool releasedAtReset = false;
    (void)Play(reset, resetPlayer, audio, [&](std::size_t start) {
        if (!resetDone && start == midChord)
        {
            reset.Reset();
            resetDone = true;
            releasedAtReset = reset.GetNoteOutput()->chordCount == 0 && reset.GetParam("soundingNotes") == 0.0;
        }
    });
    Check(releasedAtReset, "Reset() lets the chord go");

    // D2 under an A: with the lowest note at E2 the D never starts.
    const auto low = Chord({38, 57}, 0.2, 0.7, 1.4);
    GuitarToMidiEffect standard;
    standard.SetParam("mode", 2.0);
    standard.SetParam("lowestNote", 0.0);
    standard.Prepare(kRate, kBlock);
    NotePlayer standardPlayer;
    const auto standardOns = NoteOns(Play(standard, standardPlayer, low));
    GuitarToMidiEffect dropped;
    dropped.SetParam("mode", 2.0);
    dropped.SetParam("lowestNote", 1.0);
    dropped.Prepare(kRate, kBlock);
    NotePlayer droppedPlayer;
    const auto droppedOns = NoteOns(Play(dropped, droppedPlayer, low));
    Check(!Contains(standardOns, {38}) && Contains(standardOns, {57}) && Contains(droppedOns, {38, 57}),
          "Lowest Note rules out what is below the lowest string");

    GuitarToMidiEffect stereo;
    stereo.SetParam("mode", 2.0);
    stereo.Prepare(kRate, kBlock);
    NotePlayer stereoPlayer;
    Check(Notes(Play(stereo, stereoPlayer, audio, true)) == Notes(midi), "the stereo path makes the same notes");
}

/// The sent value of RPN `msb`/`lsb` on `channel`, or -1 if it was never set.
int RpnValue(const std::vector<Midi>& midi, int channel, int msb, int lsb)
{
    int selectedMsb = 127;
    int selectedLsb = 127;
    int value = -1;

    for (const auto& m : midi)
    {
        if (m.Type() != 0xB0 || m.Channel() != channel)
        {
            continue;
        }

        selectedMsb = m.data1 == 101 ? m.data2 : selectedMsb;
        selectedLsb = m.data1 == 100 ? m.data2 : selectedLsb;
        value = (m.data1 == 6 && selectedMsb == msb && selectedLsb == lsb) ? m.data2 : value;
    }

    return value;
}

void TestPolyphonicBendMode()
{
    std::cout << "\nPolyphonic + Bend mode" << std::endl;

    // E3 B3 E4, the B string 20 cents sharp and the top string curled up 40 cents mid-chord.
    std::vector<float> audio = Chord({52}, 0.2, 0.9, 1.6);
    PhraseNote sharp = Note(0.205, 0.9, 59);
    sharp.hz *= std::pow(2.0, 0.2 / 12.0);
    PhraseNote curl = Note(0.21, 0.9, 64);
    curl.bendSemitones = 0.4;
    curl.bendStart = 0.3;
    curl.bendTime = 0.1;
    const auto top = RenderPhrase({curl}, 1.6, kRate, 23);
    const auto middle = RenderPhrase({sharp}, 1.6, kRate, 22);

    for (std::size_t n = 0; n < audio.size(); ++n)
    {
        audio[n] += top[n] + middle[n];
    }

    GuitarToMidiEffect fx;
    fx.SetParam("mode", 3.0);
    fx.Prepare(kRate, kBlock);
    NotePlayer player;
    const auto midi = Play(fx, player, audio);
    const auto ons = NoteOns(midi);
    Check(Contains(ons, {52, 59, 64}) && ons.size() <= 4, "a chord comes out as its notes", Notes(midi));

    int channelOf[128];
    std::fill(std::begin(channelOf), std::end(channelOf), -1);
    bool onMembers = true;
    std::size_t firstOn = midi.size();

    for (std::size_t i = 0; i < midi.size(); ++i)
    {
        const Midi& m = midi[i];

        if (m.Type() != 0x90)
        {
            continue;
        }

        firstOn = std::min(firstOn, i);
        channelOf[m.data1] = m.Channel();
        onMembers = onMembers && m.Channel() >= 1;
    }

    const std::vector<Midi> beforeNotes(midi.begin(), midi.begin() + static_cast<std::ptrdiff_t>(firstOn));
    bool zone = RpnValue(beforeNotes, 0, 0, 6) == 15;

    for (int channel = 1; channel <= 15; ++channel)
    {
        zone = zone && RpnValue(beforeNotes, channel, 0, 0) == 2;
    }

    Check(zone, "the MPE zone and each member channel's bend range go first: 15 members, 2 semitones");
    Check(onMembers && channelOf[52] != channelOf[59] && channelOf[59] != channelOf[64] &&
              channelOf[52] != channelOf[64],
          "each note has a member channel of its own",
          std::to_string(channelOf[52]) + " " + std::to_string(channelOf[59]) + " " + std::to_string(channelOf[64]));
    // Each channel's bend over the middle of the chord, in semitones at a range of 2.
    const std::size_t from = static_cast<std::size_t>(0.7 * kRate);
    const std::size_t to = static_cast<std::size_t>(1.0 * kRate);
    auto bendRange = [&](int channel) {
        double low = 99.0;
        double high = -99.0;
        double current = 0.0;

        for (const auto& m : midi)
        {
            if (m.sample > to)
            {
                break;
            }

            current = (m.Type() == 0xE0 && m.Channel() == channel) ? m.Bend() * 2.0 / 8192.0 : current;

            if (m.sample >= from)
            {
                low = std::min(low, current);
                high = std::max(high, current);
            }
        }

        return std::pair{low, high};
    };
    const auto [curledLow, curledHigh] = bendRange(channelOf[64]);
    const auto [lowLow, lowHigh] = bendRange(channelOf[52]);
    const auto [midLow, midHigh] = bendRange(channelOf[59]);
    Check(channelOf[64] >= 0 && curledLow > 0.25 && curledHigh < 0.5,
          "the curled string's channel bends up about 40 cents",
          std::to_string(curledLow) + " to " + std::to_string(curledHigh));
    // The B's 2nd partial is 18 cents from the E's 3rd: the E is pulled a little.
    Check(channelOf[52] >= 0 && std::max(std::abs(lowLow), std::abs(lowHigh)) < 0.15,
          "the E3 stays within 15 cents of in tune", std::to_string(lowLow) + " to " + std::to_string(lowHigh));
    Check(channelOf[59] >= 0 && midLow > 0.12 && midHigh < 0.28, "and the B3 is held about 20 cents sharp",
          std::to_string(midLow) + " to " + std::to_string(midHigh));
    Check(!AnythingSounding(player), "every note is turned off once the strings are muted");

    // Sixteen notes in turn, the first curled: the sixteenth has its channel again, and starts in tune.
    std::vector<PhraseNote> run;

    for (int i = 0; i < 16; ++i)
    {
        run.push_back(Note(0.2 + 0.15 * i, 0.13, 52 + i));
    }

    run[0].bendSemitones = 0.4;
    run[0].bendStart = 0.03;
    run[0].bendTime = 0.03;
    GuitarToMidiEffect cycling;
    cycling.SetParam("mode", 3.0);
    cycling.Prepare(kRate, kBlock);
    NotePlayer cyclingPlayer;
    const auto cycled = Play(cycling, cyclingPlayer, RenderPhrase(run, 2.8, kRate));
    int reusedChannel = -1;
    int highestBend = 0;
    int bendAtReuse = 99999;
    std::string reusedBy;

    for (const auto& m : cycled)
    {
        if (reusedChannel < 0 && m.Type() == 0x90 && m.data1 == 52)
        {
            reusedChannel = m.Channel();
        }
        else if (m.Type() == 0xE0 && m.Channel() == reusedChannel && reusedBy.empty())
        {
            highestBend = std::max(highestBend, m.Bend());
            bendAtReuse = m.Bend();
        }
        else if (m.Type() == 0x90 && m.Channel() == reusedChannel && reusedBy.empty())
        {
            reusedBy = std::to_string(m.data1);
            break;
        }
    }

    Check(highestBend * 2.0 / 8192.0 > 0.25 && !reusedBy.empty() && std::abs(bendAtReuse) * 2.0 / 8192.0 < 0.1,
          "a channel a bent note had is centred again before its next note starts",
          "highest " + std::to_string(highestBend) + ", then " + std::to_string(bendAtReuse) + " for note " + reusedBy);

    GuitarToMidiEffect switching;
    switching.SetParam("mode", 3.0);
    switching.Prepare(kRate, kBlock);
    NotePlayer switchingPlayer;
    const std::size_t midChord = static_cast<std::size_t>(0.6 * kRate) / kBlock * kBlock;
    const auto switched = Play(switching, switchingPlayer, audio, [&](std::size_t start) {
        if (start == midChord)
        {
            switching.SetParam("mode", 2.0);
        }
    });
    std::vector<Midi> afterSwitch;
    std::copy_if(switched.begin(), switched.end(), std::back_inserter(afterSwitch),
                 [&](const Midi& m) { return m.sample >= midChord; });
    const bool onChannelOne = std::all_of(afterSwitch.begin(), afterSwitch.end(),
                                          [](const Midi& m) { return m.Type() != 0x90 || m.Channel() == 0; });
    Check(RpnValue(afterSwitch, 0, 0, 6) == 0 && Contains(NoteOns(afterSwitch), {52, 59, 64}) && onChannelOne,
          "going back to Polyphonic ends the zone, and the chord carries on on channel 1", Notes(afterSwitch));
}
} // namespace

int main()
{
    std::cout << "Guitar to MIDI polyphonic tests" << std::endl;
    TestPolyphonicMode();
    TestPolyphonicBendMode();
    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
