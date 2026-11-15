/**
 * ScaleGuideListenTests.cpp - What LISTEN hears whatever the guide shows (music/ListenAnalysis.h):
 * the chord a chroma most likely is, the key of a set of notes, and the tracker that steadies both
 * over readings the way TunerService feeds it.
 */

#include "music/ListenAnalysis.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace music = guitarfx::music;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

/// A chroma with these pitch classes at these levels, and a little noise everywhere else.
std::array<float, 12> Chroma(const std::map<int, float>& levels, float noise = 0.005f)
{
    std::array<float, 12> chroma{};
    chroma.fill(noise);

    for (const auto& [pitchClass, level] : levels)
    {
        chroma[pitchClass] = level;
    }

    return chroma;
}

std::string Named(const std::optional<music::HeardChord>& chord)
{
    return chord ? chord->name : std::string("(none)");
}

void TestIdentifyChord()
{
    Expect(Named(music::IdentifyChord(Chroma({{0, 0.35f}, {4, 0.3f}, {7, 0.3f}}))) == "C", "a C major strum is C");
    Expect(Named(music::IdentifyChord(Chroma({{9, 0.3f}, {0, 0.25f}, {4, 0.2f}, {7, 0.2f}}))) == "Am7",
           "a strong 7th makes a 7th chord");
    Expect(Named(music::IdentifyChord(Chroma({{0, 0.35f}, {4, 0.3f}, {7, 0.3f}, {11, 0.03f}}))) == "C",
           "a faint 7th leaves the triad");
    Expect(Named(music::IdentifyChord(Chroma({{7, 0.3f}, {11, 0.25f}, {2, 0.25f}, {5, 0.18f}}))) == "G7", "G7");
    Expect(Named(music::IdentifyChord(Chroma({{4, 0.55f}, {11, 0.4f}}))) == "E5", "a power chord");
    Expect(Named(music::IdentifyChord(Chroma({{0, 0.5f}, {4, 0.45f}}))) == "C", "a triad may lose its plain 5th");
    Expect(Named(music::IdentifyChord(Chroma({{6, 0.35f}, {9, 0.3f}, {1, 0.3f}}))) == "F\xE2\x99\xAFm",
           "F sharp minor is spelled with a sharp");
    // G C D is Gsus4 and Csus2: the name goes to the loudest root.
    Expect(Named(music::IdentifyChord(Chroma({{7, 0.35f}, {0, 0.3f}, {2, 0.3f}}))) == "Gsus4", "G C D, G loudest");
    Expect(Named(music::IdentifyChord(Chroma({{0, 0.35f}, {7, 0.3f}, {2, 0.3f}}))) == "Csus2", "G C D, C loudest");
    Expect(!music::IdentifyChord(Chroma({{9, 0.9f}})), "a single note is no chord");
    Expect(!music::IdentifyChord(Chroma({{9, 0.9f}, {4, 0.06f}})), "nor is a note with a faint 5th");
    Expect(!music::IdentifyChord(Chroma({}, 0.08f)), "nor is noise");
    Expect(!music::IdentifyChord(std::array<float, 12>{}), "nor silence");
}

void TestEstimateKey()
{
    auto weights = [](const std::map<int, double>& levels) {
        std::array<double, 12> out{};

        for (const auto& [pitchClass, level] : levels)
        {
            out[pitchClass] = level;
        }

        return out;
    };

    const auto aMinor = music::EstimateKey(weights({{9, 3.0}, {0, 2.0}, {2, 1.5}, {4, 2.0}, {7, 1.5}}));
    Expect(aMinor && aMinor->name == "A minor" && aMinor->minor, "a minor pentatonic lick leaning on A is A minor");
    Expect(aMinor && aMinor->scale != nullptr && aMinor->scale->id == "minpent" &&
               aMinor->scaleName == "A Minor Pentatonic",
           "and its notes fit the minor pentatonic");

    const auto withBlueNote = music::EstimateKey(weights({{9, 3.0}, {0, 2.0}, {2, 1.5}, {3, 1.0}, {4, 2.0}, {7, 1.5}}));
    Expect(withBlueNote && withBlueNote->scale != nullptr && withBlueNote->scale->id == "blues", "the flat 5 makes it blues");

    const auto cMajor = music::EstimateKey(weights({{0, 3.0}, {2, 1.0}, {4, 2.0}, {5, 1.0}, {7, 2.0}, {9, 1.0}, {11, 1.0}}));
    Expect(cMajor && cMajor->name == "C major" && !cMajor->minor, "a C major scale leaning on C is C major");
    Expect(cMajor && cMajor->scale != nullptr && cMajor->scale->id == "major", "with all seven notes, the major scale");
    Expect(cMajor && cMajor->confidence > 0.7, "and fits its profile well");

    const auto eFlat = music::EstimateKey(weights({{3, 3.0}, {5, 1.0}, {7, 2.0}, {8, 1.0}, {10, 2.0}, {0, 1.0}, {2, 1.0}}));
    Expect(eFlat && eFlat->name == "E\xE2\x99\xAD major", "E flat major is spelled with a flat");

    Expect(!music::EstimateKey(weights({{9, 3.0}, {0, 2.0}, {4, 2.0}})), "three notes name no key");
    Expect(!music::EstimateKey(std::array<double, 12>{}), "nor does nothing");

    // C major's and A minor's notes with no note leaning: either, and the other named beside it.
    const auto even = music::EstimateKey(weights({{0, 1.0}, {2, 1.0}, {4, 1.0}, {5, 1.0}, {7, 1.0}, {9, 1.0}, {11, 1.0}}));
    Expect(even && (even->name == "C major" || even->name == "A minor"), "an even white-note scale is C major or A minor");
}

/// A note held for `readings`, then a gap, as the tuner reads a picked note.
void Pick(music::ListenTracker& tracker, int midi, int readings = 4, bool held = false)
{
    for (int i = 0; i < readings; ++i)
    {
        tracker.PushNote(midi, held);
    }

    for (int i = 0; i < music::ListenTracker::kNoteRelease; ++i)
    {
        tracker.PushNote(std::nullopt, held);
    }
}

void Strum(music::ListenTracker& tracker, const std::array<float, 12>& chroma, int readings = 5)
{
    for (int i = 0; i < readings; ++i)
    {
        tracker.PushChroma(chroma, -20.0, false);
    }
}

void Quiet(music::ListenTracker& tracker, int readings)
{
    for (int i = 0; i < readings; ++i)
    {
        tracker.PushChroma(std::array<float, 12>{}, -80.0, false);
    }
}

void TestTrackerNotes()
{
    music::ListenTracker tracker;

    // A minor pentatonic up from A and back to it.
    for (const int midi : {57, 60, 62, 64})
    {
        Pick(tracker, midi);
    }

    Expect(!tracker.Key(), "four notes are too few for a key");

    for (const int midi : {67, 69, 67, 64, 62, 60, 57})
    {
        Pick(tracker, midi);
    }

    Pick(tracker, 57, 12);
    Expect(tracker.Key() && tracker.Key()->name == "A minor", "a pentatonic lick round A is A minor");
    Expect(tracker.Key() && tracker.Key()->scale != nullptr && tracker.Key()->scale->id == "minpent",
           "in the minor pentatonic");

    const auto json = tracker.Json(false);
    Expect(json["key"]["name"] == "A minor" && json["key"]["scale"] == "minpent" && !json.contains("chord"),
           "heard, in Scales, has the key and no chord");

    // A blip read once is no note.
    music::ListenTracker blip;

    for (int i = 0; i < 12; ++i)
    {
        blip.PushNote(57 + (i % 2) * 5, false);
    }

    Expect(!blip.Key(), "readings that never settle are no notes");

    // The guide's own playing is not the player's.
    music::ListenTracker guide;

    for (const int midi : {57, 60, 62, 64, 67, 69, 67, 64, 62, 60, 57})
    {
        Pick(guide, midi, 4, true);
    }

    Expect(!guide.Key(), "notes heard while the guide sounds count for nothing");
}

void TestTrackerChords()
{
    music::ListenTracker tracker;
    const auto c = Chroma({{0, 0.35f}, {4, 0.3f}, {7, 0.3f}});
    const auto g = Chroma({{7, 0.35f}, {11, 0.3f}, {2, 0.3f}});
    const auto am = Chroma({{9, 0.35f}, {0, 0.3f}, {4, 0.3f}});
    const auto f = Chroma({{5, 0.35f}, {9, 0.3f}, {0, 0.3f}});

    tracker.PushChroma(c, -20.0, false);
    tracker.PushChroma(c, -20.0, false);
    Expect(!tracker.Chord(), "a chord shows only once it has read the same a moment");
    tracker.PushChroma(c, -20.0, false);
    Expect(Named(tracker.Chord()) == "C" && tracker.ChordRinging(), "then it shows, ringing");

    Quiet(tracker, music::ListenTracker::kChordRelease);
    Expect(Named(tracker.Chord()) == "C" && !tracker.ChordRinging(), "it stays shown once it stops ringing");

    Strum(tracker, g);
    Expect(Named(tracker.Chord()) == "G" && tracker.RecentChords() == std::vector<std::string>{"C"},
           "the next chord replaces it, and the last goes into the trail");
    Strum(tracker, am);
    Strum(tracker, f);
    Strum(tracker, c);
    Expect(Named(tracker.Chord()) == "C" && tracker.RecentChords() == std::vector<std::string>{"F", "Am", "G"},
           "the trail is the three before, newest first, without the chord showing");

    Expect(tracker.Key() && (tracker.Key()->name == "C major" || tracker.Key()->alternative == "C major"),
           "C G Am F is in C major");

    const auto json = tracker.Json(true);
    Expect(json["chord"]["name"] == "C" && json["chord"]["id"] == "maj" && json["chord"]["root"] == 0 &&
               json["chord"]["ringing"] == true && json["recent"].size() == 3,
           "heard, in Chords, has the chord and its trail");

    // A chord the guide strums is not one the player did.
    music::ListenTracker guide;

    for (int i = 0; i < 6; ++i)
    {
        guide.PushChroma(c, -20.0, true);
    }

    Expect(!guide.Chord(), "a chord heard while the guide sounds is not shown");

    // A long silence forgets it all.
    Quiet(tracker, music::ListenTracker::kForgetReadings);
    Expect(!tracker.Chord() && !tracker.Key() && tracker.RecentChords().empty(), "a long silence forgets the chord and key");
}
} // namespace

/// `--identify <chroma.f32> <out.txt>`: names each 12-float chroma of a raw file, one line each
/// ("<root> <chord id> <share>", or "-"), so labelled recordings can score IdentifyChord.
int Identify(const char* inPath, const char* outPath)
{
    std::FILE* in = std::fopen(inPath, "rb");
    std::ofstream out(outPath);

    if (in == nullptr || !out)
    {
        return 3;
    }

    std::array<float, 12> chroma{};

    while (std::fread(chroma.data(), sizeof(float), chroma.size(), in) == chroma.size())
    {
        const auto chord = music::IdentifyChord(chroma);

        if (chord)
        {
            out << chord->rootPitchClass << ' ' << chord->def->id << ' ' << chord->share << '\n';
        }
        else
        {
            out << "-\n";
        }
    }

    std::fclose(in);
    return 0;
}

/// `--track <chroma.f32> <levels.f32> <offsets.txt> <out.txt>`: each strum's readings (offsets: one
/// start per line, then the end) through a fresh ListenTracker, and the chord it shows at the end.
int Track(const char* chromaPath, const char* levelsPath, const char* offsetsPath, const char* outPath)
{
    std::FILE* chromaFile = std::fopen(chromaPath, "rb");
    std::FILE* levelsFile = std::fopen(levelsPath, "rb");
    std::ifstream offsetsFile(offsetsPath);
    std::ofstream out(outPath);

    if (chromaFile == nullptr || levelsFile == nullptr || !offsetsFile || !out)
    {
        return 3;
    }

    std::vector<std::array<float, 12>> readings;
    std::vector<float> levels;
    std::array<float, 12> chroma{};
    float level = 0.0f;

    while (std::fread(chroma.data(), sizeof(float), chroma.size(), chromaFile) == chroma.size() &&
           std::fread(&level, sizeof(float), 1, levelsFile) == 1)
    {
        readings.push_back(chroma);
        levels.push_back(level);
    }

    std::fclose(chromaFile);
    std::fclose(levelsFile);
    std::vector<std::size_t> offsets;

    for (std::size_t offset = 0; offsetsFile >> offset;)
    {
        offsets.push_back(offset);
    }

    for (std::size_t i = 0; i + 1 < offsets.size(); ++i)
    {
        music::ListenTracker tracker;

        for (std::size_t k = offsets[i]; k < offsets[i + 1] && k < readings.size(); ++k)
        {
            tracker.PushChroma(readings[k], levels[k], false);
        }

        if (const auto& chord = tracker.Chord())
        {
            out << chord->rootPitchClass << ' ' << chord->def->id << '\n';
        }
        else
        {
            out << "-\n";
        }
    }

    return 0;
}

/// `--keys <readings.txt> <out.txt>`: tuner readings, one a line (a MIDI note, or "-" for none),
/// takes separated by a "#" line. For each take, the key shown at each reading, as runs
/// ("A minor|C major*120" etc: key|alternative*count, "-" for none), one take a line.
int Keys(const char* inPath, const char* outPath)
{
    std::ifstream in(inPath);
    std::ofstream out(outPath);

    if (!in || !out)
    {
        return 3;
    }

    music::ListenTracker tracker;
    std::string run;
    int count = 0;
    std::string line;
    std::string takeLine;

    const auto flushRun = [&] {
        if (count > 0)
        {
            takeLine += (takeLine.empty() ? "" : ";") + run + "*" + std::to_string(count);
        }

        count = 0;
    };

    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        if (line == "#")
        {
            flushRun();
            out << takeLine << '\n';
            takeLine.clear();
            run.clear();
            tracker.Reset();
            continue;
        }

        tracker.PushNote(line == "-" ? std::nullopt : std::optional<int>(std::stoi(line)), false);
        const auto& key = tracker.Key();
        const std::string now = key ? key->name + "|" + key->alternative : "-";

        if (now != run)
        {
            flushRun();
            run = now;
        }

        ++count;
    }

    flushRun();

    if (!takeLine.empty())
    {
        out << takeLine << '\n';
    }

    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 4 && std::string(argv[1]) == "--keys")
    {
        return Keys(argv[2], argv[3]);
    }

    if (argc == 4 && std::string(argv[1]) == "--identify")
    {
        return Identify(argv[2], argv[3]);
    }

    if (argc == 6 && std::string(argv[1]) == "--track")
    {
        return Track(argv[2], argv[3], argv[4], argv[5]);
    }

    TestIdentifyChord();
    TestEstimateKey();
    TestTrackerNotes();
    TestTrackerChords();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " failure(s)" << std::endl;
        return 1;
    }

    std::cout << "ScaleGuideListenTests passed" << std::endl;
    return 0;
}
