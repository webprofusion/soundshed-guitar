#include "music/ScalePlayback.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace guitarfx::music
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kSlowestBpm = 60.0;
constexpr double kFastestBpm = 200.0;
constexpr double kChordStrumSec = 0.016;
constexpr double kChordRingSec = 2.4;
constexpr double kLastNoteRingSec = 1.4;
constexpr double kOneNoteRingSec = 2.0;
/// A fretting hand lets go a touch after the next note sounds, which is what makes a run legato.
constexpr double kNoteOverlap = 1.08;
/// A guitar's loudest peaks on a DI track.
constexpr float kPeakLevel = 0.35f;
constexpr double kReleaseSec = 0.012;
/// The pick leaving the string: fast, but not a step.
constexpr double kAttackSec = 0.0008;
/// Where the pick and the pickup sit along the string, as a fraction of it from the bridge.
constexpr double kPickPlace = 0.14;
constexpr double kPickupPlace = 0.18;
/// How much of the bridge's reflection the pickup hears against the wave itself.
constexpr double kPickupReflection = 0.45;
constexpr double kLoudestLegato = 0.6;
/// How soft the burst that starts a note is: a pick's has little above this, a hammer-on's less.
constexpr double kPickToneHz = 900.0;
constexpr double kLegatoToneHz = 500.0;
/// A magnetic pickup into a cable: a mild peak, then the top falls away at 12 dB an octave, and the
/// cable's capacitance takes a little more from there up. These, the soft burst and the string's own
/// losses are what keep the top to a real guitar's DI (ScalePlaybackTests --tone).
constexpr double kPickupPeakHz = 2200.0;
constexpr double kPickupPeakQ = 0.7;
constexpr double kCableHz = 1800.0;

/// A one-pole low-pass's coefficient for `hertz`.
double OnePole(double hertz, double sampleRate)
{
    return std::exp(-2.0 * kPi * hertz / sampleRate);
}

/// The pickup and cable, a two-pole low-pass and a one-pole after it, run over the whole mix in place.
void PickupTone(std::vector<double>& mix, double sampleRate)
{
    const double cable = OnePole(kCableHz, sampleRate);
    double cabled = 0.0;
    const double w = 2.0 * kPi * std::min(kPickupPeakHz, 0.45 * sampleRate) / sampleRate;
    const double alpha = std::sin(w) / (2.0 * kPickupPeakQ);
    const double norm = 1.0 / (1.0 + alpha);
    const double b0 = 0.5 * (1.0 - std::cos(w)) * norm;
    const double b1 = 2.0 * b0;
    const double a1 = -2.0 * std::cos(w) * norm;
    const double a2 = (1.0 - alpha) * norm;
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;

    for (auto& sample : mix)
    {
        const double y = b0 * sample + b1 * x1 + b0 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = sample;
        y2 = y1;
        y1 = y;
        cabled += (1.0 - cable) * (y - cabled);
        sample = cabled;
    }
}

/// How long a note takes to die away by 60 dB: a low string rings longest.
double RingSeconds(double hertz)
{
    return std::clamp(9.0 - 2.1 * std::log2(hertz / 82.4), 2.5, 9.0);
}

/// One string's note added into `mix`, as a Karplus-Strong delay line: a burst of noise shaped by
/// where it was picked goes round a loop that loses a little, and more of its top, on every pass.
void AddPluck(std::vector<double>& mix, const PlayedNote& note, double sampleRate)
{
    const double hertz = 440.0 * std::pow(2.0, (note.midi - 69) / 12.0);
    // The loop's own filter delays by `damping` samples and the all-pass makes up the fraction left
    // over, in the range where its phase is close to flat. At a half it loses the most top a pass.
    constexpr double damping = 0.5;
    const double period = sampleRate / hertz;
    const int whole = std::max(2, static_cast<int>(period - damping - 0.3));
    const double fraction = period - damping - whole;
    const double allpass = (1.0 - fraction) / (1.0 + fraction);
    const double loopGain = std::pow(10.0, -3.0 * period / (sampleRate * RingSeconds(hertz)));
    const auto length = static_cast<std::size_t>(whole);

    std::vector<double> loop(length, 0.0);
    std::uint32_t seed = 0x9E3779B9u * static_cast<std::uint32_t>(note.midi + 7);
    double smooth = 0.0;
    double smoother = 0.0;
    const double softness = OnePole(note.legato ? kLegatoToneHz : kPickToneHz, sampleRate);

    for (auto& sample : loop)
    {
        // A small generator of our own, so a render never varies.
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        const double noise = static_cast<double>(seed) / 2147483648.0 - 1.0;
        smooth += (1.0 - softness) * (noise - smooth);
        smoother += (1.0 - softness) * (smooth - smoother);
        sample = smoother;
    }

    const auto pickGap = std::max<std::size_t>(1, static_cast<std::size_t>(kPickPlace * whole));
    const std::vector<double> burst = loop;

    for (std::size_t i = 0; i < length; ++i)
    {
        loop[i] = burst[i] - burst[(i + length - pickGap) % length];
    }

    // The pickup hears the wave and its reflection from the bridge a moment later.
    const auto pickupGap = std::max<std::size_t>(1, static_cast<std::size_t>(kPickupPlace * whole));
    std::vector<double> pickup(pickupGap, 0.0);

    const auto start = static_cast<std::size_t>(note.startSec * sampleRate);
    const auto frames = static_cast<std::size_t>(note.lengthSec * sampleRate);
    const auto release = std::max<std::size_t>(1, static_cast<std::size_t>(kReleaseSec * sampleRate));
    const auto attack = std::max<std::size_t>(1, static_cast<std::size_t>(kAttackSec * sampleRate));
    const double level = note.legato ? kLoudestLegato : 1.0;

    std::size_t at = 0;
    std::size_t pickupAt = 0;
    double previous = 0.0;
    double allpassIn = 0.0;
    double allpassOut = 0.0;

    for (std::size_t n = 0; n < frames + release && start + n < mix.size(); ++n)
    {
        const double delayed = loop[at];
        const double filtered = (1.0 - damping) * delayed + damping * previous;
        previous = delayed;
        const double tuned = allpass * filtered + allpassIn - allpass * allpassOut;
        allpassIn = filtered;
        allpassOut = tuned;
        loop[at] = loopGain * tuned;
        at = at + 1 == length ? 0 : at + 1;

        const double heard = delayed - kPickupReflection * pickup[pickupAt];
        pickup[pickupAt] = delayed;
        pickupAt = pickupAt + 1 == pickupGap ? 0 : pickupAt + 1;

        const double rise = n < attack ? static_cast<double>(n) / static_cast<double>(attack) : 1.0;
        const double fade = n < frames ? 1.0 : 0.5 + 0.5 * std::cos(kPi * static_cast<double>(n - frames) / release);
        mix[start + n] += level * rise * fade * heard;
    }
}

/// The open strings' pitches, low string first.
std::vector<int> OpenStrings(const nlohmann::json& view)
{
    std::vector<int> open;

    if (const auto strings = view.find("strings"); strings != view.end() && strings->is_array())
    {
        for (const auto& string : *strings)
        {
            open.push_back(string.value("openMidi", 0));
        }
    }

    return open;
}

/// One step of a run: which of its notes, and whether the fretting hand sounds it.
struct RunStep
{
    int note = 0;
    bool legato = false;
};

/// A run's steps as eighth notes at `bpm`, the last one left ringing.
std::vector<PlayedNote> TimedRun(const std::vector<int>& open, const nlohmann::json& notes,
                                 const std::vector<RunStep>& steps, double bpm)
{
    std::vector<PlayedNote> played;
    const double step = 30.0 / std::clamp(bpm, kSlowestBpm, kFastestBpm);

    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        const auto index = static_cast<std::size_t>(steps[i].note);

        if (index >= notes.size())
        {
            continue;
        }

        const int string = notes[index].value("string", 0);
        const int fret = notes[index].value("fret", 0);

        if (string < 0 || string >= static_cast<int>(open.size()))
        {
            continue;
        }

        const bool last = i + 1 == steps.size();
        played.push_back({open[static_cast<std::size_t>(string)] + fret, string, fret, static_cast<double>(i) * step,
                          last ? kLastNoteRingSec : step * kNoteOverlap, steps[i].legato});
    }

    return played;
}
} // namespace

std::vector<PlayedNote> ScaleGuidePlaybackNotes(const nlohmann::json& view, double bpm)
{
    const int position = view.value("position", 0);

    if (position <= 0)
    {
        return {};
    }

    if (view.value("mode", std::string()) == "chord")
    {
        const auto shapes = view.find("shapes");

        if (shapes == view.end() || !shapes->is_array() || position > static_cast<int>(shapes->size()))
        {
            return {};
        }

        std::vector<int> frets;

        for (const auto& fret :
             (*shapes)[static_cast<std::size_t>(position - 1)].value("frets", nlohmann::json::array()))
        {
            frets.push_back(fret.is_number_integer() ? fret.get<int>() : -1);
        }

        return StrummedChord(view, frets);
    }

    const auto run = view.find("run");

    if (run == view.end() || !run->is_object())
    {
        return {};
    }

    std::vector<RunStep> steps;

    for (const auto& step : run->value("steps", nlohmann::json::array()))
    {
        const auto stroke = step.value("stroke", std::string());
        steps.push_back({step.value("note", 0), stroke == "hammer" || stroke == "pull"});
    }

    return TimedRun(OpenStrings(view), run->value("notes", nlohmann::json::array()), steps, bpm);
}

std::vector<PlayedNote> ScaleExampleNotes(const nlohmann::json& view, double bpm)
{
    const auto run = view.find("run");

    if (view.value("mode", std::string()) != "scale" || run == view.end() || !run->is_object())
    {
        return {};
    }

    // A scale position's run lists its notes lowest first, a pitch once.
    const auto notes = run->value("notes", nlohmann::json::array());
    const int count = static_cast<int>(notes.size());
    const auto isRoot = [&notes](int i) { return notes[static_cast<std::size_t>(i)].value("root", false); };
    int low = 0;

    while (low < count && !isRoot(low))
    {
        ++low;
    }

    low = low < count ? low : 0;
    int high = low + 1;

    while (high < count && !isRoot(high))
    {
        ++high;
    }

    high = std::min(high, count - 1);
    std::vector<RunStep> steps;

    for (int i = low; i <= high; ++i)
    {
        steps.push_back({i, false});
    }

    for (int i = high - 1; i >= low; --i)
    {
        steps.push_back({i, false});
    }

    return count > 0 ? TimedRun(OpenStrings(view), notes, steps, bpm) : std::vector<PlayedNote>{};
}

std::vector<PlayedNote> StrummedChord(const nlohmann::json& view, const std::vector<int>& frets)
{
    std::vector<PlayedNote> played;
    const auto open = OpenStrings(view);

    for (std::size_t s = 0; s < std::min(open.size(), frets.size()); ++s)
    {
        if (frets[s] >= 0)
        {
            const double start = static_cast<double>(played.size()) * kChordStrumSec;
            played.push_back({open[s] + frets[s], static_cast<int>(s), frets[s], start, kChordRingSec, false});
        }
    }

    return played;
}

PlayedNote SingleNote(int midi)
{
    return {midi, 0, 0, 0.0, kOneNoteRingSec, false};
}

std::vector<float> RenderPluckedNotes(const std::vector<PlayedNote>& notes, double sampleRate)
{
    std::vector<float> rendered;
    double end = 0.0;

    for (const auto& note : notes)
    {
        end = std::max(end, note.startSec + note.lengthSec);
    }

    if (notes.empty() || sampleRate <= 0.0)
    {
        return rendered;
    }

    std::vector<double> mix(static_cast<std::size_t>((end + 2.0 * kReleaseSec) * sampleRate), 0.0);

    for (const auto& note : notes)
    {
        AddPluck(mix, note, sampleRate);
    }

    PickupTone(mix, sampleRate);
    double peak = 0.0;

    for (const double sample : mix)
    {
        peak = std::max(peak, std::abs(sample));
    }

    const double gain = peak > 0.0 ? kPeakLevel / peak : 0.0;
    rendered.reserve(mix.size());

    for (const double sample : mix)
    {
        rendered.push_back(static_cast<float>(gain * sample));
    }

    return rendered;
}
} // namespace guitarfx::music
