/**
 * @file GuitarToMidiEffectTests.cpp
 * @brief Tests for Guitar to MIDI (dsp/effects/GuitarToMidiEffect.h), and the executor's note
 *        routing from it to a player (NotePlayerTests.cpp covers the player itself).
 *
 *   - Notes mode plays a phrase note for note, legato where it was played legato, with every note
 *     turned off and no pitch bend; Notes + Bend holds a bent note and follows it with pitch bend,
 *     at the bend range's scale, moving to the next note only once a bend runs past the range
 *   - transpose and channel reach the notes; changing either mid-note ends the note and starts it
 *     again under the new setting; Reset lets the note go
 *   - Guitar Thru passes or mutes the guitar; the mono and stereo paths make the same notes
 *
 * Polyphonic mode is in GuitarToMidiPolyphonicTests.cpp.
 *   - in a graph, a player hears the note sources upstream of it and no others; a bypassed source
 *     lets its note go, and one brought back starts afresh rather than replaying a stale note
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "GuitarPhraseSynth.h"
#include "GuitarToMidiTestSupport.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/NoteEvents.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/GuitarToMidiEffect.h"

namespace
{
using guitarfx::GuitarToMidiEffect;
using guitarfx::NoteBlock;
using guitarfx::NotePlayer;
using guitarfx::test::NoteHz;
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

/// E3 G3 A3 A3 C4, picked over each other's ringing, a hammer-on to D4, then a muted E4.
std::vector<PhraseNote> Phrase()
{
    std::vector<PhraseNote> notes;
    double t = 0.2;

    for (int midi : {52, 55, 57, 57, 60})
    {
        notes.push_back(Note(t, 0.2, midi, false));
        t += 0.2;
    }

    PhraseNote hammer = Note(t, 0.2, 62, false);
    hammer.legato = true;
    notes.push_back(hammer);
    notes.push_back(Note(t + 0.25, 0.3, 64));
    return notes;
}

void TestRegistration()
{
    std::cout << "\nRegistration" << std::endl;
    auto& registry = guitarfx::EffectRegistry::Instance();
    const auto info = registry.GetTypeInfo(guitarfx::EffectGuids::kGuitarToMidi);
    Check(info.has_value() && info->displayName == "Guitar to MIDI" && info->category == "synth",
          "Guitar to MIDI is registered as a synth effect");
    Check(registry.Resolve("guitar_to_midi") == guitarfx::EffectGuids::kGuitarToMidi, "its readable id resolves to it");
    Check(info && info->parameters.size() == guitarfx::guitar_to_midi::kParamCount &&
              info->parameters[guitarfx::guitar_to_midi::kBendRange].labels.size() == 4,
          "with every parameter, and the enums labelled");

    const auto processor = registry.Create(guitarfx::EffectGuids::kGuitarToMidi);
    Check(processor && processor->GetNoteOutput() != nullptr && !processor->AcceptsNoteInput(),
          "it makes notes and does not play them");
}

void TestNotesMode()
{
    std::cout << "\nNotes mode" << std::endl;
    const auto phrase = Phrase();
    const auto audio = RenderPhrase(phrase, 2.2, kRate);
    GuitarToMidiEffect fx;
    fx.SetParam("mode", 0.0);
    fx.Prepare(kRate, kBlock);
    NotePlayer player;
    const auto midi = Play(fx, player, audio);

    Check(NoteOns(midi) == std::vector<int>{52, 55, 57, 57, 60, 62, 64}, "the phrase is played note for note",
          Notes(midi));

    // The hammer-on: D4 starts before C4 stops, at the same sample.
    bool legato = false;

    for (std::size_t i = 0; i + 1 < midi.size(); ++i)
    {
        legato = legato || (midi[i].Type() == 0x90 && midi[i].data1 == 62 && midi[i + 1].Type() == 0x80 &&
                            midi[i + 1].data1 == 60 && midi[i + 1].sample == midi[i].sample);
    }

    Check(legato, "the hammer-on is legato: the new note starts, then the old one stops");
    Check(!AnythingSounding(player), "every note is turned off by the end");
    Check(std::none_of(midi.begin(), midi.end(), [](const Midi& m) { return m.Type() == 0xE0; }),
          "and no pitch bend is sent");

    const bool velocities = std::all_of(
        midi.begin(), midi.end(), [](const Midi& m) { return m.Type() != 0x90 || (m.data2 >= 1 && m.data2 <= 127); });
    Check(velocities, "velocities are in range");
}

void TestBendMode()
{
    std::cout << "\nNotes + Bend mode" << std::endl;

    PhraseNote bend = Note(0.2, 0.7, 64);
    bend.bendSemitones = 2.0;
    bend.bendStart = 0.15;
    bend.bendTime = 0.15;
    const auto audio = RenderPhrase({bend}, 1.2, kRate);

    for (const int range : {0, 1})
    {
        GuitarToMidiEffect fx;
        fx.SetParam("bendRange", range);
        fx.Prepare(kRate, kBlock);
        NotePlayer player;
        const auto midi = Play(fx, player, audio);
        int highest = 0;

        for (const auto& m : midi)
        {
            highest = m.Type() == 0xE0 ? std::max(highest, m.Bend()) : highest;
        }

        const int expected = range == 0 ? 8191 : 1365; // +2 semitones at a range of 2 and of 12
        Check(NoteOns(midi) == std::vector<int>{64} && std::abs(highest - expected) <= expected / 100 + 2,
              std::string("a whole-step bend is one note, bent to +2 semitones at a range of ") +
                  (range == 0 ? "2" : "12"),
              Notes(midi) + ", highest bend " + std::to_string(highest));
    }

    PhraseNote wide = Note(0.2, 0.8, 64);
    wide.bendSemitones = 3.0;
    wide.bendStart = 0.15;
    wide.bendTime = 0.2;
    GuitarToMidiEffect fx;
    fx.Prepare(kRate, kBlock);
    NotePlayer player;
    const auto midi = Play(fx, player, RenderPhrase({wide}, 1.3, kRate));
    const auto notes = NoteOns(midi);
    int lastBend = 0;

    for (const auto& m : midi)
    {
        lastBend = m.Type() == 0xE0 ? m.Bend() : lastBend;
    }

    // It passes the range's end at +2.3 semitones, where the nearest note is 66, and bends the
    // last semitone from there: +1 at a range of 2 is 4096.
    Check(notes.size() == 2 && notes[0] == 64 && notes[1] == 66 && std::abs(lastBend - 4096) < 100,
          "a bend past a range of 2 moves to the nearest note, legato, and bends on from there",
          Notes(midi) + ", ending at bend " + std::to_string(lastBend));

    PhraseNote vibrato = Note(0.2, 0.8, 57);
    vibrato.vibratoCents = 30.0;
    GuitarToMidiEffect fxVibrato;
    fxVibrato.Prepare(kRate, kBlock);
    NotePlayer vibratoPlayer;
    const auto vibratoMidi = Play(fxVibrato, vibratoPlayer, RenderPhrase({vibrato}, 1.3, kRate));
    int low = 0;
    int high = 0;

    for (const auto& m : vibratoMidi)
    {
        if (m.Type() == 0xE0)
        {
            low = std::min(low, m.Bend());
            high = std::max(high, m.Bend());
        }
    }

    // +/-30 cents at a range of 2 semitones is +/-1229.
    Check(NoteOns(vibratoMidi).size() == 1 && low < -900 && high > 900,
          "vibrato comes out as bends either side of the note", std::to_string(low) + " to " + std::to_string(high));
}

void TestTransposeChannelAndChanges()
{
    std::cout << "\nTranspose, channel, and changes mid-note" << std::endl;
    const auto audio = RenderPhrase({Note(0.2, 0.6, 57)}, 1.2, kRate);

    GuitarToMidiEffect fx;
    fx.SetParam("transpose", 12.0);
    fx.SetParam("channel", 10.0);
    fx.Prepare(kRate, kBlock);
    NotePlayer player;
    const auto midi = Play(fx, player, audio);
    const bool placed =
        !midi.empty() && std::all_of(midi.begin(), midi.end(), [](const Midi& m) { return m.Channel() == 9; });
    Check(NoteOns(midi) == std::vector<int>{69} && placed, "transpose and channel reach the notes", Notes(midi));

    GuitarToMidiEffect changing;
    changing.Prepare(kRate, kBlock);
    NotePlayer changingPlayer;
    const std::size_t changeAt = static_cast<std::size_t>(0.5 * kRate) / kBlock * kBlock;
    const auto changed = Play(changing, changingPlayer, audio, [&](std::size_t start) {
        if (start == changeAt)
        {
            changing.SetParam("channel", 3.0);
        }
    });
    const bool offOld = std::any_of(changed.begin(), changed.end(), [&](const Midi& m) {
        return m.sample == changeAt && m.Type() == 0x80 && m.Channel() == 0 && m.data1 == 57;
    });
    const bool onNew = std::any_of(changed.begin(), changed.end(), [&](const Midi& m) {
        return m.sample == changeAt && m.Type() == 0x90 && m.Channel() == 2 && m.data1 == 57;
    });
    Check(offOld && onNew, "a new channel mid-note ends the note there and starts it on the new channel");
    Check(!AnythingSounding(changingPlayer), "and that one is turned off too");

    GuitarToMidiEffect reset;
    reset.Prepare(kRate, kBlock);
    NotePlayer resetPlayer;
    bool resetDone = false;
    const auto afterReset = Play(reset, resetPlayer, audio, [&](std::size_t start) {
        if (!resetDone && start >= static_cast<std::size_t>(0.4 * kRate))
        {
            reset.Reset();
            resetDone = true;
        }
    });
    const auto resetOff =
        std::find_if(afterReset.begin(), afterReset.end(), [](const Midi& m) { return m.Type() == 0x80; });
    Check(resetOff != afterReset.end() && resetOff->sample < static_cast<std::size_t>(0.4 * kRate) + kBlock,
          "Reset() lets the note go at once");
}

void TestThruAndPaths()
{
    std::cout << "\nGuitar Thru, and mono against stereo" << std::endl;
    const auto audio = RenderPhrase(Phrase(), 2.2, kRate);

    GuitarToMidiEffect thru;
    thru.Prepare(kRate, kBlock);
    std::vector<float> in(audio.begin() + 20000, audio.begin() + 20000 + kBlock);
    std::vector<float> out(kBlock);
    thru.ProcessMono(in.data(), out.data(), kBlock);
    Check(out == in, "Guitar Thru on passes the guitar untouched");

    thru.SetParam("thru", 0.0);

    for (int i = 0; i < 4; ++i)
    {
        thru.ProcessMono(in.data(), out.data(), kBlock);
    }

    Check(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }),
          "off, it is silent once its fade is done");

    GuitarToMidiEffect mono;
    mono.Prepare(kRate, kBlock);
    NotePlayer monoPlayer;
    GuitarToMidiEffect stereo;
    stereo.Prepare(kRate, kBlock);
    NotePlayer stereoPlayer;
    const auto a = Play(mono, monoPlayer, audio, false);
    const auto b = Play(stereo, stereoPlayer, audio, true);
    bool same = a.size() == b.size();

    for (std::size_t i = 0; same && i < a.size(); ++i)
    {
        same = a[i].status == b[i].status && a[i].data1 == b[i].data1 && a[i].data2 == b[i].data2 &&
               a[i].sample == b[i].sample;
    }

    Check(same, "the mono and stereo paths make the same MIDI", std::to_string(a.size()) + " messages");
}

/// A stand-in for a Plugin Host: plays whatever notes it is handed through a NotePlayer and logs
/// the MIDI, and how many sources it heard each block.
class RecordingPlayer : public guitarfx::EffectProcessor
{
  public:
    static inline RecordingPlayer* sLast = nullptr;

    RecordingPlayer()
    {
        sLast = this;
    }

    std::vector<Midi> midi;
    std::vector<int> sourcesPerBlock;
    std::size_t blocks = 0;

    void Prepare(double, int) override
    {
    }

    void Reset() override
    {
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        CopyStereoInputToOutput(inputs, outputs, numSamples);
        sourcesPerBlock.push_back(static_cast<int>(mSources.size()));

        for (const auto& m : mPlayer.Build(mSources, numSamples))
        {
            midi.push_back({m.status, m.data1, m.data2,
                            blocks * static_cast<std::size_t>(numSamples) + static_cast<std::size_t>(m.sampleOffset)});
        }

        mPlayer.Commit();
        mSources = {};
        ++blocks;
    }

    [[nodiscard]] bool AcceptsNoteInput() const override
    {
        return true;
    }

    void SetNoteInput(std::span<const NoteBlock* const> sources) override
    {
        mSources = sources;
    }

    void SetParam(const std::string&, double) override
    {
    }

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string&) const override
    {
        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "test_note_player";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "utility";
    }

  private:
    std::span<const NoteBlock* const> mSources;
    NotePlayer mPlayer;
};

void RegisterRecordingPlayer()
{
    guitarfx::EffectTypeInfo info;
    info.type = "test_note_player";
    info.displayName = "Test Note Player";
    info.category = "utility";
    guitarfx::EffectRegistry::Instance().Register(info.type, info,
                                                  []() { return std::make_unique<RecordingPlayer>(); });
}

using guitarfx::GraphEdge;
using guitarfx::GraphNode;
using guitarfx::SignalGraph;

GraphNode Node(const std::string& id, const std::string& type)
{
    GraphNode node;
    node.id = id;
    node.type = type;
    node.enabled = true;
    return node;
}

/// Runs `audio` through `graph`; `between` runs before each block.
template <typename Between>
void RunGraph(guitarfx::SignalGraphExecutor& executor, const std::vector<float>& audio, Between&& between)
{
    std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);

    for (std::size_t start = 0; start + kBlock <= audio.size(); start += kBlock)
    {
        between(start);
        std::copy_n(audio.begin() + static_cast<std::ptrdiff_t>(start), kBlock, inL.begin());
        std::copy_n(audio.begin() + static_cast<std::ptrdiff_t>(start), kBlock, inR.begin());
        float* in[2] = {inL.data(), inR.data()};
        float* out[2] = {outL.data(), outR.data()};
        executor.Process(in, out, kBlock);
    }
}

void TestRouting()
{
    std::cout << "\nNote routing in a graph" << std::endl;
    const auto audio = RenderPhrase(Phrase(), 2.2, kRate);

    {
        SignalGraph graph;
        graph.nodes = {Node("__input__", guitarfx::kNodeTypeInput), Node("g2m", "guitar_to_midi"), Node("gain", "gain"),
                       Node("player", "test_note_player"), Node("__output__", guitarfx::kNodeTypeOutput)};
        graph.edges = {{"__input__", "g2m"}, {"g2m", "gain"}, {"gain", "player"}, {"player", "__output__"}};
        guitarfx::SignalGraphExecutor executor;
        executor.SetGraph(graph);
        executor.Prepare(kRate, kBlock);
        RunGraph(executor, audio, [](std::size_t) {});
        RecordingPlayer* player = RecordingPlayer::sLast;
        const bool heard = player && std::all_of(player->sourcesPerBlock.begin(), player->sourcesPerBlock.end(),
                                                 [](int n) { return n == 1; });
        Check(heard, "a player downstream hears its source every block, through the nodes between");
        Check(player && NoteOns(player->midi) == std::vector<int>{52, 55, 57, 57, 60, 62, 64},
              "and plays the phrase note for note", player ? Notes(player->midi) : "");
    }

    {
        SignalGraph graph;
        graph.nodes = {Node("__input__", guitarfx::kNodeTypeInput),
                       Node("split", guitarfx::kNodeTypeSplitter),
                       Node("g2m", "guitar_to_midi"),
                       Node("player", "test_note_player"),
                       Node("mix", guitarfx::kNodeTypeMixer),
                       Node("__output__", guitarfx::kNodeTypeOutput)};
        graph.edges = {{"__input__", "split"}, {"split", "g2m", 0, 0},  {"split", "player", 1, 0},
                       {"g2m", "mix", 0, 0},   {"player", "mix", 0, 1}, {"mix", "__output__"}};
        guitarfx::SignalGraphExecutor executor;
        executor.SetGraph(graph);
        executor.Prepare(kRate, kBlock);
        RunGraph(executor, audio, [](std::size_t) {});
        RecordingPlayer* player = RecordingPlayer::sLast;
        const bool deaf =
            player && !player->sourcesPerBlock.empty() &&
            std::all_of(player->sourcesPerBlock.begin(), player->sourcesPerBlock.end(), [](int n) { return n == 0; }) &&
            player->midi.empty();
        Check(deaf, "a player on a parallel branch hears nothing from the other branch's source");
    }

    {
        SignalGraph graph;
        graph.nodes = {Node("__input__", guitarfx::kNodeTypeInput), Node("g2m", "guitar_to_midi"),
                       Node("player", "test_note_player"), Node("__output__", guitarfx::kNodeTypeOutput)};
        graph.edges = {{"__input__", "g2m"}, {"g2m", "player"}, {"player", "__output__"}};
        guitarfx::SignalGraphExecutor executor;
        executor.SetGraph(graph);
        executor.Prepare(kRate, kBlock);

        // One long note; bypass the source while it sounds, then bring it back into silence.
        auto longNote = RenderPhrase({Note(0.1, 1.5, 57, false)}, 1.0, kRate);
        longNote.resize(static_cast<std::size_t>(2.0 * kRate), 0.0f);
        const std::size_t bypassAt = static_cast<std::size_t>(0.5 * kRate) / kBlock * kBlock;
        const std::size_t restoreAt = static_cast<std::size_t>(1.5 * kRate) / kBlock * kBlock;
        RunGraph(executor, longNote, [&](std::size_t start) {
            if (start == bypassAt)
            {
                executor.SetNodeEnabled("g2m", false);
            }

            if (start == restoreAt)
            {
                executor.SetNodeEnabled("g2m", true);
            }
        });

        RecordingPlayer* player = RecordingPlayer::sLast;
        const auto off =
            std::find_if(player->midi.begin(), player->midi.end(), [](const Midi& m) { return m.Type() == 0x80; });
        Check(off != player->midi.end() && off->sample >= bypassAt && off->sample < bypassAt + kBlock,
              "bypassing the source mid-note lets the note go in that block");
        const bool noStale = std::none_of(player->midi.begin(), player->midi.end(),
                                          [&](const Midi& m) { return m.Type() == 0x90 && m.sample >= restoreAt; });
        Check(noStale, "and bringing it back into silence does not replay the note it had");
    }

    {
        SignalGraph graph;
        graph.nodes = {Node("__input__", guitarfx::kNodeTypeInput), Node("a", "guitar_to_midi"),
                       Node("b", "guitar_to_midi"), Node("player", "test_note_player"),
                       Node("__output__", guitarfx::kNodeTypeOutput)};
        graph.nodes[2].params["channel"] = 2.0;
        graph.edges = {{"__input__", "a"}, {"a", "b"}, {"b", "player"}, {"player", "__output__"}};
        guitarfx::SignalGraphExecutor executor;
        executor.SetGraph(graph);
        executor.Prepare(kRate, kBlock);
        RunGraph(executor, audio, [](std::size_t) {});
        RecordingPlayer* player = RecordingPlayer::sLast;
        const bool twoChannels =
            player &&
            std::any_of(player->midi.begin(), player->midi.end(), [](const Midi& m) { return m.Channel() == 0; }) &&
            std::any_of(player->midi.begin(), player->midi.end(), [](const Midi& m) { return m.Channel() == 1; });
        Check(player && !player->sourcesPerBlock.empty() && player->sourcesPerBlock.front() == 2 && twoChannels,
              "two sources in a row both reach the player, each on its own channel");
    }
}
} // namespace

int main()
{
    std::cout << "Guitar to MIDI tests" << std::endl;
    guitarfx::RegisterAllEffects();
    RegisterRecordingPlayer();

    TestRegistration();
    TestNotesMode();
    TestBendMode();
    TestTransposeChannelAndChanges();
    TestThruAndPaths();
    TestRouting();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
