/**
 * @file ScalePlaybackTests.cpp
 * @brief The Scale Guide's Play: what each view sounds, and the plucked-string voice.
 *
 * The voice is judged by what a player would notice: in tune to a few cents across a guitar's range
 * at the usual rates, dying away, level with a DI track, no click at either end, and no brighter
 * than a real guitar's DI. `--tone` prints the octave bands of the voice beside the demo clips', and
 * `--wav <file>` writes what that check plays, to listen to.
 */

#include "DemoAudio.h"
#include "MessageHandlerRegistry.h"
#include "controller/ScaleGuideMessages.h"
#include "music/ScaleGuideView.h"
#include "music/ScalePlayback.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace guitarfx;
using nlohmann::json;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << std::endl;
        ++gFailures;
    }
}

json ViewFor(json request)
{
    request["tuning"] = "std";
    return music::BuildScaleGuideView(music::ParseScaleGuideRequest(request));
}

/// The pitch near `hertz`: where the steady part of the note best matches itself a few periods on.
/// Going a few periods out rather than one is what resolves cents at the top of the neck.
double MeasuredHertz(const std::vector<float>& samples, double rate, double hertz)
{
    constexpr double kPeriods = 16.0;
    const double period = kPeriods * rate / hertz;
    const auto from = static_cast<std::size_t>(0.25 * rate);
    const auto span = static_cast<std::size_t>(0.25 * rate);
    const int low = static_cast<int>(period * 0.97);
    const int high = static_cast<int>(period * 1.03) + 1;

    const auto match = [&](int lag) {
        double sum = 0.0;

        for (std::size_t i = from; i < from + span; ++i)
        {
            sum += static_cast<double>(samples[i]) * samples[i + static_cast<std::size_t>(lag)];
        }

        return sum;
    };

    int best = low;
    double bestMatch = match(low);

    for (int lag = low + 1; lag <= high; ++lag)
    {
        if (const double value = match(lag); value > bestMatch)
        {
            bestMatch = value;
            best = lag;
        }
    }

    const double before = match(best - 1);
    const double after = match(best + 1);
    const double curve = before - 2.0 * bestMatch + after;
    const double exact = best + (curve != 0.0 ? 0.5 * (before - after) / curve : 0.0);
    return kPeriods * rate / exact;
}

double Peak(const std::vector<float>& samples, std::size_t from, std::size_t to)
{
    double peak = 0.0;

    for (std::size_t i = from; i < std::min(to, samples.size()); ++i)
    {
        peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
    }

    return peak;
}

/// The share of the signal's power in each octave band, in dB: 63 Hz to 8 kHz, then everything
/// above 11.3 kHz. Averaged over Hann-windowed blocks, leaving out the near-silent ones.
constexpr std::array<double, 8> kBandCentres{63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0};

std::array<double, kBandCentres.size() + 1> OctaveBands(const std::vector<float>& samples, double rate)
{
    constexpr std::size_t kSize = 4096;
    constexpr double kPi = 3.14159265358979323846;
    std::array<double, kBandCentres.size() + 1> power{};
    std::vector<std::complex<double>> block(kSize);
    double loudest = 0.0;

    for (const float sample : samples)
    {
        loudest = std::max(loudest, std::abs(static_cast<double>(sample)));
    }

    for (std::size_t from = 0; from + kSize <= samples.size(); from += kSize / 2)
    {
        double energy = 0.0;

        for (std::size_t i = 0; i < kSize; ++i)
        {
            const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / kSize);
            block[i] = window * samples[from + i];
            energy += static_cast<double>(samples[from + i]) * samples[from + i];
        }

        if (std::sqrt(energy / kSize) < loudest * 0.01)
        {
            continue;
        }

        // An in-place radix-2 FFT.
        for (std::size_t i = 1, j = 0; i < kSize; ++i)
        {
            std::size_t bit = kSize >> 1;

            for (; (j & bit) != 0; bit >>= 1)
            {
                j ^= bit;
            }

            j ^= bit;

            if (i < j)
            {
                std::swap(block[i], block[j]);
            }
        }

        for (std::size_t length = 2; length <= kSize; length <<= 1)
        {
            const auto turn = std::polar(1.0, -2.0 * kPi / static_cast<double>(length));

            for (std::size_t i = 0; i < kSize; i += length)
            {
                std::complex<double> w(1.0);

                for (std::size_t k = 0; k < length / 2; ++k)
                {
                    const auto odd = w * block[i + k + length / 2];
                    block[i + k + length / 2] = block[i + k] - odd;
                    block[i + k] += odd;
                    w *= turn;
                }
            }
        }

        for (std::size_t bin = 1; bin < kSize / 2; ++bin)
        {
            const double hertz = static_cast<double>(bin) * rate / kSize;
            std::size_t band = kBandCentres.size();

            for (std::size_t b = 0; b < kBandCentres.size(); ++b)
            {
                if (hertz < kBandCentres[b] * std::sqrt(2.0))
                {
                    band = b;
                    break;
                }
            }

            power[band] += std::norm(block[bin]);
        }
    }

    double total = 0.0;

    for (const double p : power)
    {
        total += p;
    }

    for (auto& p : power)
    {
        p = total > 0.0 ? 10.0 * std::log10(std::max(p / total, 1e-12)) : -120.0;
    }

    return power;
}

/// The power above 2.8 kHz against all of it, in dB: how bright a signal is.
double TopShareDb(const std::vector<float>& samples, double rate)
{
    const auto bands = OctaveBands(samples, rate);
    double top = 0.0;

    for (std::size_t b = 6; b < bands.size(); ++b)
    {
        top += std::pow(10.0, bands[b] / 10.0);
    }

    return 10.0 * std::log10(std::max(top, 1e-12));
}

std::vector<float> Slice(const std::vector<float>& samples, double rate, double fromSec, double toSec)
{
    const auto from = std::min(samples.size(), static_cast<std::size_t>(fromSec * rate));
    const auto to = std::min(samples.size(), static_cast<std::size_t>(toSec * rate));
    return {samples.begin() + static_cast<std::ptrdiff_t>(from), samples.begin() + static_cast<std::ptrdiff_t>(to)};
}

/// What the voice plays in the brightness check, in the demo DI's register so the two compare: a
/// scale low on the neck up and back, then power chords cut short, at 48 kHz.
std::vector<float> VoiceSample()
{
    const auto view = ViewFor({{"mode", "scale"}, {"key", 9}, {"scale", "minpent"}, {"position", 1}});
    auto notes = music::ScaleExampleNotes(view, 120.0);
    double at = notes.back().startSec + notes.back().lengthSec;

    for (const std::vector<int>& frets : std::vector<std::vector<int>>{
             {0, 2, 2, -1, -1, -1}, {-1, 0, 2, 2, -1, -1}, {3, 5, 5, -1, -1, -1}, {0, 2, 2, -1, -1, -1}})
    {
        for (int hit = 0; hit < 2; ++hit, at += 0.35)
        {
            for (auto note : music::StrummedChord(view, frets))
            {
                note.startSec += at;
                note.lengthSec = 0.3;
                notes.push_back(note);
            }
        }
    }

    return music::RenderPluckedNotes(notes, 48000.0);
}

/// Single notes from the low E to the 15th fret of the top string, for looking at the top end.
std::vector<float> HighVoiceSample()
{
    std::vector<music::PlayedNote> notes;
    double at = 0.0;

    for (int midi = 52; midi <= 79; midi += 3, at += 0.6)
    {
        auto note = music::SingleNote(midi);
        note.startSec = at;
        note.lengthSec = 0.6;
        notes.push_back(note);
    }

    return music::RenderPluckedNotes(notes, 48000.0);
}

void PrintTone()
{
    const auto row = [](const std::string& name, const std::vector<float>& samples, double rate) {
        std::cout << std::left << std::setw(20) << name << std::right << std::fixed << std::setprecision(1);

        for (const double band : OctaveBands(samples, rate))
        {
            std::cout << std::setw(7) << band;
        }

        std::cout << "   top " << TopShareDb(samples, rate) << std::endl;
    };

    std::cout << std::left << std::setw(20) << "dB of the whole" << std::right;

    for (const double centre : kBandCentres)
    {
        std::cout << std::setw(7) << static_cast<int>(centre);
    }

    std::cout << std::setw(7) << ">11k" << std::endl;

    for (const char* clip : {guitarfx::test::kDemoDiGuitar, "guitar-riff-01.wav", "guitar-riff-02.wav"})
    {
        double rate = 0.0;
        const auto samples = guitarfx::test::LoadDemoClipMono(clip, rate);
        row(clip, std::strcmp(clip, guitarfx::test::kDemoDiGuitar) == 0 ? Slice(samples, rate, 10.0, 70.0) : samples,
            rate);
    }

    row("voice", VoiceSample(), 48000.0);
    row("voice, higher", HighVoiceSample(), 48000.0);
}

/// The voice no brighter than a real guitar's DI: it was harsh when it had a good deal more top.
void TestNoBrighterThanAGuitar()
{
    double rate = 0.0;
    const auto di = guitarfx::test::LoadDemoClipMono(guitarfx::test::kDemoDiGuitar, rate);
    Expect(!di.empty(), "the demo DI clip decodes");

    if (di.empty())
    {
        return;
    }

    const double guitar = TopShareDb(Slice(di, rate, 10.0, 70.0), rate);
    const double voice = TopShareDb(VoiceSample(), 48000.0);
    Expect(voice <= guitar + 1.0, "the voice's top (" + std::to_string(voice) + " dB) is no more than a DI's (" +
                                      std::to_string(guitar) + " dB)");
    // The demo riffs sit 10 dB under the long take: a voice duller than them sounds muffled.
    Expect(voice >= guitar - 11.0, "nor so much less that it sounds muffled (" + std::to_string(voice) + " dB)");
}

/// The brightness check's sample as a 16-bit WAV, for listening.
void WriteWav(const std::string& path)
{
    const auto samples = VoiceSample();
    const auto put = [](std::ofstream& out, std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i)
        {
            out.put(static_cast<char>((value >> (8 * i)) & 0xFF));
        }
    };
    std::ofstream out(path, std::ios::binary);
    const auto data = static_cast<std::uint32_t>(samples.size() * 2);
    out.write("RIFF", 4);
    put(out, 36 + data, 4);
    out.write("WAVEfmt ", 8);
    put(out, 16, 4);
    put(out, 1, 2);
    put(out, 1, 2);
    put(out, 48000, 4);
    put(out, 96000, 4);
    put(out, 2, 2);
    put(out, 16, 2);
    out.write("data", 4);
    put(out, data, 4);

    for (const float sample : samples)
    {
        put(out, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767.0f)), 2);
    }

    std::cout << "wrote " << path << std::endl;
}

void TestVoice()
{
    for (const double rate : {44100.0, 48000.0, 96000.0})
    {
        for (const int midi : {40, 45, 52, 64, 76, 88})
        {
            const auto samples = music::RenderPluckedNotes({music::SingleNote(midi)}, rate);
            const double hertz = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
            const double measured = MeasuredHertz(samples, rate, hertz);
            const double cents = measured > 0.0 ? 1200.0 * std::log2(measured / hertz) : 9999.0;
            const std::string label = "midi " + std::to_string(midi) + " at " + std::to_string(rate);
            Expect(std::abs(cents) < 4.0, "in tune: " + label + " is " + std::to_string(cents) + " cents off");

            const auto second = static_cast<std::size_t>(rate);
            Expect(Peak(samples, 0, second) > 0.3 && Peak(samples, 0, second) < 0.36, "peaks at DI level: " + label);
            Expect(Peak(samples, second, 2 * second) < Peak(samples, 0, second / 10), "dies away: " + label);
            Expect(std::abs(samples.front()) < 0.05f && std::abs(samples.back()) < 0.001f, "no click: " + label);
        }
    }

    const auto once = music::RenderPluckedNotes({music::SingleNote(52)}, 48000.0);
    Expect(once == music::RenderPluckedNotes({music::SingleNote(52)}, 48000.0), "a render never varies");
    Expect(music::RenderPluckedNotes({}, 48000.0).empty(), "nothing to play is no signal");
}

void TestLegatoIsSofter()
{
    auto picked = music::SingleNote(64);
    auto hammered = picked;
    hammered.legato = true;
    picked.startSec = 0.0;
    hammered.startSec = 0.5;
    const auto rate = 48000.0;
    const auto both = music::RenderPluckedNotes({picked, hammered}, rate);
    const auto twenty = static_cast<std::size_t>(0.02 * rate);
    const auto half = static_cast<std::size_t>(0.5 * rate);
    Expect(Peak(both, half, half + twenty) < Peak(both, 0, twenty), "a hammer-on hits softer than a pick");
}

void TestNotesForEachView()
{
    auto notes = music::ScaleGuidePlaybackNotes(
        ViewFor({{"mode", "scale"}, {"key", 9}, {"scale", "minpent"}, {"position", 1}}), 120.0);
    Expect(notes.size() >= 11, "a scale position goes up and comes back");
    Expect(notes.front().midi == notes.back().midi, "and ends where it began");
    Expect(std::abs(notes[1].startSec - 0.25) < 1e-9, "as eighth notes at the tempo");

    for (std::size_t i = 1; i < notes.size() / 2 + 1; ++i)
    {
        Expect(notes[i].midi > notes[i - 1].midi, "rising to the top");
    }

    Expect(music::ScaleGuidePlaybackNotes(
               ViewFor({{"mode", "scale"}, {"key", 9}, {"scale", "minpent"}, {"position", 0}}), 120.0)
               .empty(),
           "the whole neck has no one way through");

    notes = music::ScaleGuidePlaybackNotes(ViewFor({{"mode", "arpeggio"}, {"key", 9}, {"chord", "m"}, {"position", 3}}),
                                           120.0);
    Expect(!notes.empty(), "an arpeggio plays its run");
    int legato = 0;

    for (const auto& note : notes)
    {
        legato += note.legato ? 1 : 0;
    }

    Expect(legato > 0, "with its hammer-ons and pull-offs");

    const auto view = ViewFor({{"mode", "chord"}, {"key", 0}, {"chord", "maj"}, {"position", 1}});
    notes = music::ScaleGuidePlaybackNotes(view, 120.0);
    Expect(notes.size() >= 3 && notes.size() <= 6, "a chord sounds its strings");
    Expect(notes.back().startSec < 0.1, "strummed, not arpeggiated");
    Expect(notes.front().lengthSec > 1.0, "and left ringing");

    const int capoed =
        music::ScaleGuidePlaybackNotes(
            ViewFor({{"mode", "scale"}, {"key", 9}, {"scale", "minpent"}, {"position", 1}, {"capo", 2}}), 120.0)
            .front()
            .midi;
    Expect(capoed >= 40, "a capo stays at concert pitch");
}

void TestScaleExample()
{
    auto notes = music::ScaleExampleNotes(
        ViewFor({{"mode", "scale"}, {"key", 9}, {"scale", "minpent"}, {"position", 1}}), 120.0);
    Expect(notes.size() == 11, "a pentatonic octave is six notes up and five back");
    Expect(!notes.empty() && notes.front().midi % 12 == 9, "from the root");
    Expect(notes.size() == 11 && notes[5].midi == notes.front().midi + 12 && notes.back().midi == notes.front().midi,
           "to the octave and back");

    for (std::size_t i = 1; i < notes.size(); ++i)
    {
        Expect(i <= 5 ? notes[i].midi > notes[i - 1].midi : notes[i].midi < notes[i - 1].midi,
               "one way, then the other");
    }

    notes =
        music::ScaleExampleNotes(ViewFor({{"mode", "scale"}, {"key", 0}, {"scale", "major"}, {"position", 2}}), 120.0);
    Expect(notes.size() == 15, "a major scale is eight up and seven back");
    Expect(
        music::ScaleExampleNotes(ViewFor({{"mode", "scale"}, {"key", 9}, {"scale", "minpent"}, {"position", 0}}), 120.0)
            .empty(),
        "the whole neck needs a position chosen for it");
    Expect(music::ScaleExampleNotes(ViewFor({{"mode", "arpeggio"}, {"key", 9}, {"chord", "m"}, {"position", 1}}), 120.0)
               .empty(),
           "an arpeggio is not a scale");

    const auto chord =
        music::StrummedChord(ViewFor({{"mode", "chord"}, {"key", 0}, {"chord", "maj"}}), {-1, 3, 2, 0, 1, 0});
    Expect(chord.size() == 5 && chord.front().midi == 48 && chord.front().string == 1,
           "an open C, strummed from the A");
    Expect(chord.back().startSec > chord.front().startSec && chord.back().startSec < 0.1, "in one stroke");
}

void TestScaleRun()
{
    const auto view = ViewFor({{"mode", "scale"}, {"key", 9}, {"scale", "minpent"}, {"position", 2}});
    const auto& run = view["run"];
    Expect(run.is_object() && run["name"] == "Position 2", "a scale position has a run");
    Expect(run["steps"].size() == 2 * run["notes"].size() - 1, "up and back, the top note once");
    Expect(run["steps"][0]["stroke"] == "down" && run["steps"][1]["stroke"] == "up", "alternate picked");
    Expect(!ViewFor({{"mode", "scale"}, {"key", 9}, {"scale", "minpent"}, {"position", 0}}).contains("run"),
           "the whole neck has none");
    Expect(!ViewFor({{"mode", "chord"}, {"key", 0}, {"chord", "maj"}, {"position", 1}}).contains("run"),
           "nor does a chord");

    const auto arpeggio = ViewFor({{"mode", "arpeggio"}, {"key", 9}, {"chord", "m"}, {"position", 2}});
    Expect(arpeggio["run"] == arpeggio["arpeggio"]["runs"][1], "an arpeggio's chosen run is the same one");
}

void TestMessages()
{
    guitarfx::MessageHandlerRegistry registry;
    std::vector<json> sent;
    int announced = 0;
    int unannounced = 0;
    int stops = 0;
    double renderedAt = 0.0;
    std::size_t lastLength = 0;

    guitarfx::ScaleGuideAudio audio;
    audio.sampleRate = [] { return 44100.0; };
    audio.tempoBpm = [] { return 120.0; };
    audio.play = [&](std::vector<float> samples, double rate, bool announce) {
        ++(announce ? announced : unannounced);
        renderedAt = rate;
        lastLength = samples.size();
    };
    audio.stop = [&stops] { ++stops; };
    guitarfx::RegisterScaleGuideMessages(
        registry, [&sent](const std::string& text) { sent.push_back(json::parse(text)); }, audio);

    json play = {{"type", "scaleGuide"},
                 {"action", "play"},
                 {"request", {{"mode", "arpeggio"}, {"key", 9}, {"chord", "m"}, {"tuning", "std"}, {"position", 1}}}};
    registry.Dispatch("scaleGuide", play);
    Expect(announced == 1 && lastLength > 44100 && renderedAt == 44100.0, "Play renders at the host's rate");
    Expect(sent.size() == 1 && sent[0]["type"] == "scaleGuidePlayback" && sent[0]["notes"].size() > 5,
           "and says when each note falls");

    play["request"]["position"] = 0;
    registry.Dispatch("scaleGuide", play);
    Expect(announced == 1 && sent.size() == 1, "the whole neck has nothing to play");

    registry.Dispatch("scaleGuide", {{"type", "scaleGuide"}, {"action", "playNote"}, {"midi", 64}});
    Expect(unannounced == 1 && sent.size() == 1, "a clicked note plays without a report");

    json example = {
        {"type", "scaleGuide"},
        {"action", "playScale"},
        {"request", {{"mode", "scale"}, {"key", 9}, {"scale", "minpent"}, {"tuning", "std"}, {"position", 0}}}};
    registry.Dispatch("scaleGuide", example);
    Expect(announced == 2 && sent.size() == 2 && sent[1]["notes"].size() == 11,
           "a scale's example plays from the first position when the whole neck shows");

    json strum = {{"type", "scaleGuide"},
                  {"action", "playChord"},
                  {"request", {{"mode", "chord"}, {"key", 0}, {"chord", "maj"}, {"tuning", "std"}}},
                  {"frets", {-1, 3, 2, 0, 1, 0}}};
    registry.Dispatch("scaleGuide", strum);
    Expect(announced == 3 && sent.size() == 3 && sent[2]["notes"].size() == 5, "a chord box strums its shape");

    registry.Dispatch("scaleGuide", {{"type", "scaleGuide"}, {"action", "stop"}});
    Expect(stops == 1, "stop stops");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--tone")
    {
        PrintTone();
        return 0;
    }

    if (argc > 2 && std::string(argv[1]) == "--wav")
    {
        WriteWav(argv[2]);
        return 0;
    }

    TestVoice();
    TestNoBrighterThanAGuitar();
    TestScaleExample();
    TestLegatoIsSofter();
    TestNotesForEachView();
    TestScaleRun();
    TestMessages();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " failure(s)" << std::endl;
        return 1;
    }

    std::cout << "ScalePlaybackTests passed" << std::endl;
    return 0;
}
