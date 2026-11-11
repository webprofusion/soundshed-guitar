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
constexpr double kChordRingSec = 4.5;
constexpr double kLastNoteRingSec = 3.5;
constexpr double kOneNoteRingSec = 3.5;
/// A note left ringing fades over this much of its end, or less of a short one.
constexpr double kRingFadeSec = 1.2;
constexpr double kRingFadeShare = 0.4;
/// Vibrato: nothing until the note has been held this long, then it grows to a bend of this many
/// cents and back, the way a finger rocking a string only ever sharpens it. Its rate and depth
/// wander, each towards a new random mark a few times a second.
constexpr double kVibratoAfterSec = 0.55;
constexpr double kVibratoGrowSec = 1.3;
constexpr double kVibratoCents = 16.0;
constexpr double kVibratoHz = 5.1;
constexpr double kVibratoRateWander = 0.12;
constexpr double kVibratoDepthWander = 0.3;
constexpr double kVibratoWanderSec = 0.2;
constexpr double kChordVibrato = 0.35;
/// A slide: how long the finger takes to reach the new fret, a little longer the further it goes,
/// centred on the beat the new note falls on.
constexpr double kSlideSec = 0.06;
constexpr double kSlideSecPerFret = 0.012;
constexpr double kSlideLongestSec = 0.14;
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
    return std::clamp(13.0 - 2.4 * std::log2(hertz / 82.4), 5.0, 13.0);
}

/// The next value of a small generator of our own, from -1 to 1, so a render never varies.
double NextNoise(std::uint32_t& seed)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return static_cast<double>(seed) / 2147483648.0 - 1.0;
}

/// Where in a held note's own samples each one heard is read from: a sample on for every sample,
/// and a little further while the vibrato has the pitch up. `reads` gets one place per frame.
void VibratoReads(std::vector<double>& reads, const PlayedNote& note, double sampleRate)
{
    const double deepest = (std::pow(2.0, kVibratoCents / 1200.0) - 1.0) * std::clamp(note.vibrato, 0.0, 2.0);
    const double settle = 1.0 - std::exp(-1.0 / (kVibratoWanderSec * sampleRate));
    const auto every = std::max<std::size_t>(1, static_cast<std::size_t>(kVibratoWanderSec * sampleRate));
    std::uint32_t seed = 0x85EBCA6Bu * static_cast<std::uint32_t>(note.midi * 31 + note.string * 7 + note.step + 11);
    seed = seed == 0 ? 1 : seed;
    double rateMark = NextNoise(seed);
    double depthMark = NextNoise(seed);
    double rate = rateMark;
    double depth = depthMark;
    double phase = 0.0;
    double place = 0.0;

    for (std::size_t n = 0; n < reads.size(); ++n)
    {
        reads[n] = place;
        const double held = (static_cast<double>(n) / sampleRate - kVibratoAfterSec) / kVibratoGrowSec;
        double bend = 0.0;

        if (held > 0.0)
        {
            if (n % every == 0)
            {
                rateMark = NextNoise(seed);
                depthMark = NextNoise(seed);
            }

            rate += settle * (rateMark - rate);
            depth += settle * (depthMark - depth);
            const double grown = held >= 1.0 ? 1.0 : held * held * (3.0 - 2.0 * held);
            phase += 2.0 * kPi * kVibratoHz * (1.0 + kVibratoRateWander * rate) / sampleRate;
            bend = deepest * grown * (1.0 + kVibratoDepthWander * depth) * (0.5 - 0.5 * std::cos(phase));
        }

        place += 1.0 + bend;
    }
}

/// A slide: the note keeps sounding and its pitch moves by `semitones`, around `atSec` into it.
struct Glide
{
    double atSec = 0.0;
    int semitones = 0;
};

/// The read places of a note that slides: the same pace as before until the slide, then eased to
/// the new pitch's and held there. The string is not struck again, so the read just moves faster
/// or slower through its own samples.
void GlideReads(std::vector<double>& reads, const Glide& glide, double sampleRate)
{
    const double length = std::min(kSlideLongestSec, kSlideSec + kSlideSecPerFret * std::abs(glide.semitones));
    const double from = glide.atSec - 0.5 * length;
    const double arrived = std::pow(2.0, glide.semitones / 12.0);
    double place = 0.0;

    for (std::size_t n = 0; n < reads.size(); ++n)
    {
        const double pace = n + 1 < reads.size() ? reads[n + 1] - reads[n] : 1.0;
        const double moved = (static_cast<double>(n) / sampleRate - from) / length;
        double ratio = moved <= 0.0 ? 1.0 : arrived;

        if (moved > 0.0 && moved < 1.0)
        {
            ratio = std::pow(2.0, moved * moved * (3.0 - 2.0 * moved) * glide.semitones / 12.0);
        }

        reads[n] = place;
        place += pace * ratio;
    }
}

/// One string's note added into `mix`, as a Karplus-Strong delay line: a burst of noise shaped by
/// where it was picked goes round a loop that loses a little, and more of its top, on every pass.
/// With `glide` its pitch slides to another note's part of the way through.
void AddPluck(std::vector<double>& mix, const PlayedNote& note, double sampleRate, const Glide* glide = nullptr)
{
    const double hertz = 440.0 * std::pow(2.0, (note.midi - 69) / 12.0);
    // The loop's own filter delays by `damping` samples and the all-pass makes up the fraction left
    // over, in the range where its phase is close to flat. At a half it loses the most top a pass.
    constexpr double damping = 0.5;
    const double period = sampleRate / hertz;
    const int whole = std::max(2, static_cast<int>(period - damping - 0.3));
    const double fraction = period - damping - whole;
    const double allpass = (1.0 - fraction) / (1.0 + fraction);
    // The loop's filter takes a little of the fundamental too, more the higher the note, so the gain
    // gives that back: what is left is the ring time asked for. Never more than one, or the loop grows.
    const double ringLoss = std::pow(10.0, -3.0 * period / (sampleRate * RingSeconds(hertz)));
    const double loopGain = std::min(1.0, ringLoss / std::cos(kPi * hertz / sampleRate));
    const auto length = static_cast<std::size_t>(whole);

    std::vector<double> loop(length, 0.0);
    std::uint32_t seed = 0x9E3779B9u * static_cast<std::uint32_t>(note.midi + 7);
    double smooth = 0.0;
    double smoother = 0.0;
    const double softness = OnePole(note.legato ? kLegatoToneHz : kPickToneHz, sampleRate);

    for (auto& sample : loop)
    {
        const double noise = NextNoise(seed);
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
    const auto attack = std::max<std::size_t>(1, static_cast<std::size_t>(kAttackSec * sampleRate));
    const double level = note.legato ? kLoudestLegato : 1.0;
    // A stopped note is let go just after its length; one left ringing fades inside it.
    const auto fade = std::max<std::size_t>(
        1, static_cast<std::size_t>(
               (note.ring ? std::min(kRingFadeSec, kRingFadeShare * note.lengthSec) : kReleaseSec) * sampleRate));
    const std::size_t fadeFrom = note.ring ? frames - std::min(frames, fade) : frames;
    const std::size_t total = start < mix.size() ? std::min(fadeFrom + fade, mix.size() - start) : 0;

    // The string's own samples first, then heard through a moving read: the vibrato's, a slide's.
    const bool wavers = note.vibrato > 0.0 && static_cast<double>(total) > kVibratoAfterSec * sampleRate;
    const bool bends = total > 0 && (wavers || glide != nullptr);
    std::vector<double> reads(bends ? total : 0);

    if (wavers)
    {
        VibratoReads(reads, note, sampleRate);
    }
    else
    {
        for (std::size_t n = 0; n < reads.size(); ++n)
        {
            reads[n] = static_cast<double>(n);
        }
    }

    if (bends && glide != nullptr)
    {
        GlideReads(reads, *glide, sampleRate);
    }

    const std::size_t sounded = bends ? static_cast<std::size_t>(reads.back()) + 4 : total;
    std::vector<double> string(sounded);

    std::size_t at = 0;
    std::size_t pickupAt = 0;
    double previous = 0.0;
    double allpassIn = 0.0;
    double allpassOut = 0.0;

    for (std::size_t n = 0; n < sounded; ++n)
    {
        const double delayed = loop[at];
        const double filtered = (1.0 - damping) * delayed + damping * previous;
        previous = delayed;
        const double tuned = allpass * filtered + allpassIn - allpass * allpassOut;
        allpassIn = filtered;
        allpassOut = tuned;
        loop[at] = loopGain * tuned;
        at = at + 1 == length ? 0 : at + 1;

        string[n] = delayed - kPickupReflection * pickup[pickupAt];
        pickup[pickupAt] = delayed;
        pickupAt = pickupAt + 1 == pickupGap ? 0 : pickupAt + 1;
    }

    for (std::size_t n = 0; n < total; ++n)
    {
        double heard = string[n];

        if (bends && reads[n] != static_cast<double>(n))
        {
            // A four-point Hermite read between the samples either side.
            const auto i = static_cast<std::size_t>(reads[n]);
            const double t = reads[n] - static_cast<double>(i);
            const double a = string[i > 0 ? i - 1 : 0];
            const double b = string[i];
            const double c = string[i + 1];
            const double d = string[i + 2];
            heard = b + 0.5 * t * (c - a + t * (2.0 * a - 5.0 * b + 4.0 * c - d + t * (3.0 * (b - c) + d - a)));
        }

        const double rise = n < attack ? static_cast<double>(n) / static_cast<double>(attack) : 1.0;
        const double fall =
            n < fadeFrom ? 1.0 : 0.5 + 0.5 * std::cos(kPi * static_cast<double>(n - fadeFrom) / static_cast<double>(fade));
        mix[start + n] += level * rise * fall * heard;
    }
}

/// How long one note of a run lasts: an eighth note at `bpm`. Past the fastest tempo a run is played
/// at, the notes are twice as long, and below the slowest half as long, as often as it takes: so
/// whatever the metronome is set to, the notes stay on its clicks.
double StepSeconds(double bpm)
{
    double step = 30.0 / std::clamp(bpm, 1.0, 1000.0);

    while (step < 30.0 / kFastestBpm)
    {
        step *= 2.0;
    }

    while (step > 30.0 / kSlowestBpm)
    {
        step *= 0.5;
    }

    return step;
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

/// One step of a run: which of its notes, whether the fretting hand sounds it, and whether it is
/// slid into.
struct RunStep
{
    int note = 0;
    bool legato = false;
    bool slide = false;
};

/// A run's steps as eighth notes at `bpm`, the last one left ringing.
std::vector<PlayedNote> TimedRun(const std::vector<int>& open, const nlohmann::json& notes,
                                 const std::vector<RunStep>& steps, double bpm)
{
    std::vector<PlayedNote> played;
    const double step = StepSeconds(bpm);

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
                          last ? kLastNoteRingSec : step * kNoteOverlap, steps[i].legato, static_cast<int>(i)});
        played.back().ring = last;
        played.back().vibrato = last && fret > 0 ? 1.0 : 0.0;
        played.back().slide = steps[i].slide;
    }

    return played;
}

/// A progression's notes as its chords fall: each chord's bar of the tab starts on a bar line of
/// `barSec` (straight after the last with none), its last note held until the next chord. With
/// `onlyBar` the rest are dropped and that chord's bar starts at once.
void PlaceBars(std::vector<PlayedNote>& played, const nlohmann::json& bars, double bpm, double barSec, int onlyBar)
{
    const double step = StepSeconds(bpm);
    std::vector<PlayedNote> placed;
    double barStart = 0.0;

    for (const auto& bar : bars)
    {
        const int from = bar.value("from", 0);
        const int count = bar.value("count", 0);
        const int chord = bar.value("chord", -1);

        if (count <= 0 || (onlyBar >= 0 && chord != onlyBar))
        {
            continue;
        }

        if (!placed.empty())
        {
            // The chord before is held up to this one, which a slide then leaves from.
            auto& held = placed.back();
            held.lengthSec = (barStart - held.startSec) * kNoteOverlap;
            held.ring = false;
            held.vibrato = 0.0;
        }

        for (const auto& note : played)
        {
            if (note.step >= from && note.step < from + count)
            {
                placed.push_back(note);
                placed.back().chord = chord;
                placed.back().startSec = barStart + (note.step - from) * step;
                placed.back().lengthSec = step * kNoteOverlap;
                placed.back().ring = false;
                placed.back().vibrato = 0.0;
            }
        }

        const double length = count * step;
        barStart += barSec > 0.0 ? std::ceil(length / barSec - 1e-6) * barSec : length;
    }

    if (!placed.empty())
    {
        // Nothing comes before the first note to slide from: it is picked.
        placed.front().slide = false;
        placed.front().legato = false;
        placed.back().lengthSec = kLastNoteRingSec;
        placed.back().ring = true;
        placed.back().vibrato = placed.back().fret > 0 ? 1.0 : 0.0;
    }

    played = std::move(placed);
}

/// A run's steps as the JSON view lists them; a hammer-on, pull-off or slide is the fretting hand's.
std::vector<RunStep> RunSteps(const nlohmann::json& run)
{
    std::vector<RunStep> steps;

    for (const auto& step : run.value("steps", nlohmann::json::array()))
    {
        const auto stroke = step.value("stroke", std::string());
        steps.push_back(
            {step.value("note", 0), stroke == "hammer" || stroke == "pull" || stroke == "slide", stroke == "slide"});
    }

    return steps;
}
} // namespace

std::vector<PlayedNote> ScaleGuidePlaybackNotes(const nlohmann::json& view, double bpm, double barSec, int onlyBar)
{
    const int position = view.value("position", 0);

    if (const auto whole = view.find("progressionRun"); whole != view.end() && whole->is_object())
    {
        auto played =
            TimedRun(OpenStrings(view), whole->value("notes", nlohmann::json::array()), RunSteps(*whole), bpm);

        PlaceBars(played, whole->value("bars", nlohmann::json::array()), bpm, barSec, onlyBar);
        return played;
    }

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

    return TimedRun(OpenStrings(view), run->value("notes", nlohmann::json::array()), RunSteps(*run), bpm);
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

    if (count == 0)
    {
        return {};
    }

    auto played = TimedRun(OpenStrings(view), notes, steps, bpm);

    // The example is not what the tab writes out, so nothing follows it there.
    for (auto& note : played)
    {
        note.step = -1;
    }

    return played;
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
            played.back().ring = true;
            played.back().vibrato = frets[s] > 0 ? kChordVibrato : 0.0;
        }
    }

    return played;
}

PlayedNote SingleNote(int midi)
{
    PlayedNote note{midi, 0, 0, 0.0, kOneNoteRingSec, false};
    note.ring = true;
    note.vibrato = 1.0;
    return note;
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

    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        const bool slides = i + 1 < notes.size() && notes[i + 1].slide && !notes[i].slide &&
                            notes[i + 1].string == notes[i].string && notes[i + 1].startSec > notes[i].startSec;

        if (!slides)
        {
            AddPluck(mix, notes[i], sampleRate);
            continue;
        }

        // One string, one stroke: the note before lasts through the one it slides to.
        const auto& to = notes[i + 1];
        PlayedNote both = notes[i];
        both.lengthSec = to.startSec + to.lengthSec - both.startSec;
        both.ring = to.ring;
        both.vibrato = 0.0;
        const Glide glide{to.startSec - both.startSec, to.midi - both.midi};
        AddPluck(mix, both, sampleRate, &glide);
        ++i;
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
