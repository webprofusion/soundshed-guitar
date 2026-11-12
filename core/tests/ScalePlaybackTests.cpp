/**
 * @file ScalePlaybackTests.cpp
 * @brief The Scale Guide's Play: what each view sounds, and the plucked-string voice.
 *
 * The voice is judged by what a player would notice: in tune to a few cents across a guitar's range
 * at the usual rates, dying away, level with a DI track, no click at either end, and no brighter
 * than a real guitar's DI. `--tone` prints the octave bands of the voice beside the demo clips', and
 * `--wav <file>` writes what that check plays, to listen to, and `--wav <file> held` the notes Play
 * leaves ringing. The scaleGuide messages that drive Play are ScalePlaybackMessageTests.cpp.
 */

#include "DemoAudio.h"
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

/// What Play leaves ringing, one after another: an arpeggio run's last note, a strummed chord and a
/// clicked note.
std::vector<float> HeldSample()
{
    auto notes = music::ScaleGuidePlaybackNotes(
        ViewFor({{"mode", "arpeggio"}, {"key", 9}, {"chord", "m"}, {"position", 1}}), 120.0);
    double at = notes.back().startSec + notes.back().lengthSec + 0.3;

    for (auto note :
         music::StrummedChord(ViewFor({{"mode", "chord"}, {"key", 0}, {"chord", "maj"}}), {-1, 3, 2, 0, 1, 0}))
    {
        note.startSec += at;
        notes.push_back(note);
    }

    auto single = music::SingleNote(64);
    single.startSec = notes.back().startSec + notes.back().lengthSec + 0.3;
    notes.push_back(single);
    return music::RenderPluckedNotes(notes, 48000.0);
}

/// The brightness check's sample as a 16-bit WAV, for listening, or the held notes'.
void WriteWav(const std::string& path, bool held)
{
    const auto samples = held ? HeldSample() : VoiceSample();
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

/// The pitch over a tenth of a second from `fromSec`, in cents against `hertz`: the spacing of the
/// rising zero crossings of the fundamental, the harmonics filtered off first.
double CentsAt(const std::vector<float>& samples, double rate, double hertz, double fromSec)
{
    const double keep = std::exp(-2.0 * 3.14159265358979323846 * 0.6 * hertz / rate);
    std::array<double, 5> poles{};
    double lower = 0.0;
    double before = 0.0;
    double first = -1.0;
    double last = -1.0;
    int crossings = 0;
    const auto from = static_cast<std::size_t>(fromSec * rate);
    const auto warm = static_cast<std::size_t>(0.05 * rate);

    for (std::size_t i = from - warm; i < from + static_cast<std::size_t>(0.1 * rate) && i < samples.size(); ++i)
    {
        lower = samples[i];

        for (auto& pole : poles)
        {
            pole += (1.0 - keep) * (lower - pole);
            lower = pole;
        }

        if (i >= from && before < 0.0 && lower >= 0.0)
        {
            const double at = static_cast<double>(i) - lower / (lower - before);
            first = first < 0.0 ? at : first;
            last = at;
            ++crossings;
        }

        before = lower;
    }

    return crossings > 1 ? 1200.0 * std::log2((crossings - 1) * rate / (last - first) / hertz) : 0.0;
}

/// A held note keeps ringing, its pitch starts to waver only once it has been held, never the same
/// way twice running, and it fades out rather than stopping.
void TestHeldNoteRingsAndWavers()
{
    const double rate = 48000.0;
    const double hertz = 440.0 * std::pow(2.0, (57 - 69) / 12.0);
    const auto held = music::RenderPluckedNotes({music::SingleNote(57)}, rate);
    Expect(held.size() > static_cast<std::size_t>(3.0 * rate), "a held note lasts over three seconds");

    double lowest = 1e9;
    double highest = -1e9;
    std::vector<double> swings;
    bool rising = true;
    double turn = 0.0;

    for (double at = 0.15; at < 2.6; at += 0.02)
    {
        const double cents = CentsAt(held, rate, hertz, at);

        if (at < 0.45)
        {
            Expect(std::abs(cents) < 4.0,
                   "steady at first: " + std::to_string(cents) + " cents at " + std::to_string(at));
            continue;
        }

        if (at > 1.9)
        {
            lowest = std::min(lowest, cents);
            highest = std::max(highest, cents);
        }

        // Each top of the vibrato, to see that no two are alike.
        if (rising && cents < turn - 1.0)
        {
            swings.push_back(turn);
            rising = false;
        }
        else if (!rising && cents > turn + 1.0)
        {
            rising = true;
        }

        turn = rising ? std::max(turn, cents) : std::min(turn, cents);
    }

    Expect(highest - lowest > 6.0 && highest - lowest < 30.0,
           "then wavers a little: " + std::to_string(highest - lowest) + " cents");
    Expect(lowest > -4.0, "only ever sharp, as a bent string is: " + std::to_string(lowest));
    Expect(swings.size() >= 4, "several times over: " + std::to_string(swings.size()));

    if (swings.size() >= 4)
    {
        Expect(swings.front() < swings.back(), "growing as it goes");
    }

    const auto second = static_cast<std::size_t>(rate);
    Expect(Peak(held, 2 * second, 2 * second + second / 4) > 0.1 * Peak(held, second / 4, second / 2),
           "still ringing two seconds in");

    for (std::size_t from = 5 * second / 2; from + second / 5 < held.size(); from += second / 5)
    {
        Expect(Peak(held, from + second / 5, from + 2 * second / 5) < Peak(held, from, from + second / 5),
               "fading to the end");
    }

    auto open = music::SingleNote(57);
    open.vibrato = 0.0;
    const auto plain = music::RenderPluckedNotes({open}, rate);
    Expect(std::abs(CentsAt(plain, rate, hertz, 2.2)) < 4.0 && std::abs(CentsAt(plain, rate, hertz, 2.3)) < 4.0,
           "an open string holds its pitch");

    const auto chord =
        music::StrummedChord(ViewFor({{"mode", "chord"}, {"key", 0}, {"chord", "maj"}}), {-1, 3, 2, 0, 1, 0});
    Expect(chord.size() == 5 && chord[0].ring && chord[0].vibrato > 0.0 && chord[0].vibrato < 1.0 &&
               chord[2].vibrato == 0.0,
           "a chord rings with a little vibrato on its fretted strings alone");
}

void TestNotesForEachView()
{
    auto notes = music::ScaleGuidePlaybackNotes(
        ViewFor({{"mode", "scale"}, {"key", 9}, {"scale", "minpent"}, {"position", 1}}), 120.0);
    Expect(notes.size() >= 11, "a scale position goes up and comes back");
    Expect(notes.front().midi == notes.back().midi, "and ends where it began");
    Expect(std::abs(notes[1].startSec - 0.25) < 1e-9, "as eighth notes at the tempo");

    // Whatever the metronome is set to, the notes stay on its clicks: twice as long past the
    // fastest a run is played, half as long below the slowest.
    const auto second = [](double bpm) {
        return music::ScaleGuidePlaybackNotes(
                   ViewFor({{"mode", "scale"}, {"key", 9}, {"scale", "minpent"}, {"position", 1}}), bpm)[1]
            .startSec;
    };
    Expect(std::abs(second(240.0) - 0.25) < 1e-9, "quarter notes at 240");
    Expect(std::abs(second(200.0) - 0.15) < 1e-9 && std::abs(second(60.0) - 0.5) < 1e-9, "eighths from 60 to 200");
    Expect(std::abs(second(40.0) - 0.375) < 1e-9, "sixteenths at 40");

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

/// A progression's arpeggios keep to the bar, and a slide is one stroke whose pitch moves.
void TestProgressionTimingAndSlides()
{
    const json request = {
        {"mode", "arpeggio"}, {"key", 0}, {"progression", "pop"}, {"position", 1}, {"arpeggioSlides", true}};
    const auto view = ViewFor(request);
    const auto bars = view["progressionRun"]["bars"];
    const auto loose = music::ScaleGuidePlaybackNotes(view, 120.0);
    const auto barred = music::ScaleGuidePlaybackNotes(view, 120.0, 2.0);
    Expect(loose.size() == barred.size() && !barred.empty(), "the same notes with a bar length or without");

    for (const auto& bar : bars)
    {
        const auto& first = barred[static_cast<std::size_t>(bar["from"].get<int>())];
        const double inBars = first.startSec / 2.0;
        Expect(std::abs(inBars - std::round(inBars)) < 1e-9, "each chord starts on a bar line");
    }

    const auto second = static_cast<std::size_t>(bars[1]["from"].get<int>());
    Expect(std::abs(loose[second].startSec - 0.25 * static_cast<double>(second)) < 1e-9,
           "without a bar length the chords follow straight on");
    Expect(barred[second].slide &&
               barred[second - 1].startSec + barred[second - 1].lengthSec >= barred[second].startSec,
           "the last note of a chord is held up to the next, which is slid into");
    Expect(barred.back().ring && !barred[second - 1].ring, "only the last note of all is left ringing");

    const auto alone = music::ScaleGuidePlaybackNotes(view, 120.0, 2.0, 1);
    Expect(static_cast<int>(alone.size()) == bars[1]["count"].get<int>() && alone.front().startSec == 0.0 &&
               !alone.front().slide && alone.front().step == bars[1]["from"].get<int>() && alone.back().ring,
           "one bar alone starts at once, picked, and keeps its place in the tab");

    // A slide up two frets on the A string: A to B, one stroke.
    const double rate = 48000.0;
    music::PlayedNote from{57, 1, 12, 0.0, 0.5, false};
    music::PlayedNote to{59, 1, 14, 0.5, 1.0, true};
    to.slide = true;
    const auto slid = music::RenderPluckedNotes({from, to}, rate);
    const double a = 440.0 * std::pow(2.0, (57 - 69) / 12.0);
    const double b = 440.0 * std::pow(2.0, (59 - 69) / 12.0);
    Expect(std::abs(CentsAt(slid, rate, a, 0.2)) < 5.0, "a slide starts on the note before");
    Expect(std::abs(CentsAt(slid, rate, b, 0.75)) < 5.0 && std::abs(CentsAt(slid, rate, b, 1.1)) < 5.0,
           "and comes to rest on the new one: " + std::to_string(CentsAt(slid, rate, b, 0.75)) + " cents");

    to.slide = false;
    const auto struck = music::RenderPluckedNotes({from, to}, rate);
    Expect(struck != slid && std::abs(CentsAt(struck, rate, b, 0.75)) < 5.0, "which is not the same as striking it");

    to.slide = true;
    to.string = 2;
    Expect(music::RenderPluckedNotes({from, to}, rate) != slid, "on another string it is struck after all");
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
        WriteWav(argv[2], argc > 3 && std::string(argv[3]) == "held");
        return 0;
    }

    TestVoice();
    TestNoBrighterThanAGuitar();
    TestScaleExample();
    TestLegatoIsSofter();
    TestHeldNoteRingsAndWavers();
    TestNotesForEachView();
    TestScaleRun();
    TestProgressionTimingAndSlides();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " failure(s)" << std::endl;
        return 1;
    }

    std::cout << "ScalePlaybackTests passed" << std::endl;
    return 0;
}
