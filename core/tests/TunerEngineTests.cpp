#include "dsp/ChromaAnalyzer.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/TunerEngine.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

using namespace guitarfx;

namespace
{
constexpr double kPi = 3.14159265358979323846;

/// `seconds` of a tone at `frequency` (0 for silence), with uniform white noise of RMS `noiseRms`.
std::vector<float> Tone(double sampleRate, double frequency, double seconds, double noiseRms = 0.0)
{
    std::vector<float> signal(static_cast<std::size_t>(seconds * sampleRate));
    std::uint32_t seed = 3u;

    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        seed = seed * 1664525u + 1013904223u;
        const double noise = noiseRms * std::sqrt(3.0) * (static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
        signal[n] =
            static_cast<float>(0.3 * std::sin(2.0 * kPi * frequency * static_cast<double>(n) / sampleRate) + noise);
    }

    return signal;
}

/// The tuner's readings of `signal`, fed one reading's worth (2048 samples at 48 kHz) at a time.
/// Each is waited for, so a newer one cannot replace it before the worker takes it. One can still be
/// lost to the audio thread's try-lock; it is kept as a placeholder (debugRms -1), so the rest stay
/// at their own index, and the checks skip it.
std::vector<TunerEngine::Result> Readings(double sampleRate, const std::vector<float>& signal, double reference = 440.0)
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<TunerEngine::Result> results;
    TunerEngine tuner;
    tuner.Prepare(sampleRate);
    tuner.SetReferenceFrequency(reference);
    tuner.SetCallback([&](const TunerEngine::Result& result) {
        std::lock_guard<std::mutex> lock(mutex);
        results.push_back(result);
        cv.notify_one();
    });
    tuner.SetEnabled(true);
    const auto chunk = static_cast<std::size_t>(std::lround(sampleRate * 2048.0 / 48000.0));

    for (std::size_t start = 0; start + chunk <= signal.size(); start += chunk)
    {
        std::size_t before = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            before = results.size();
        }
        tuner.Process(signal.data() + start, static_cast<int>(chunk));
        std::unique_lock<std::mutex> lock(mutex);

        if (!cv.wait_for(lock, std::chrono::milliseconds(500), [&] { return results.size() > before; }))
        {
            TunerEngine::Result lost;
            lost.debugRms = -1.0;
            results.push_back(lost);
        }
    }

    tuner.SetEnabled(false);
    std::lock_guard<std::mutex> lock(mutex);
    return results;
}

/// The last reading that was not lost.
TunerEngine::Result Last(const std::vector<TunerEngine::Result>& readings)
{
    for (auto it = readings.rbegin(); it != readings.rend(); ++it)
    {
        if (it->debugRms >= 0.0)
        {
            return *it;
        }
    }

    return {};
}

double Cents(double frequency, double reference)
{
    return 1200.0 * std::log2(frequency / reference);
}

/// The detected readings from `from` onwards, as cents from `truth`: mean and spread.
bool Steady(const std::vector<TunerEngine::Result>& readings, std::size_t from, double truth, double& mean,
            double& spread)
{
    std::vector<double> cents;

    for (std::size_t i = from; i < readings.size(); ++i)
    {
        if (readings[i].debugRms < 0.0)
        {
            continue;
        }

        if (!readings[i].detected)
        {
            return false;
        }

        cents.push_back(Cents(readings[i].frequency, truth));
    }

    if (cents.size() < 10)
    {
        return false;
    }

    mean = 0.0;
    spread = 0.0;

    for (const double c : cents)
    {
        mean += c / static_cast<double>(cents.size());
    }

    for (const double c : cents)
    {
        spread += (c - mean) * (c - mean) / static_cast<double>(cents.size());
    }

    spread = std::sqrt(spread);
    return true;
}

/// What moving the tuner onto the shared PitchTracker changed. Returns false on a failure.
bool TestPitchTracker()
{
    // 192 kHz: the old detector's longest period no longer fit its 4096-sample window, and it
    // found nothing at all.
    double mean = 0.0;
    double spread = 0.0;
    auto readings = Readings(192000.0, Tone(192000.0, 110.0, 1.0));

    if (!Steady(readings, 8, 110.0, mean, spread) || std::abs(mean) > 1.0 || Last(readings).noteName != "A" ||
        Last(readings).octave != 2)
    {
        std::cerr << "Tuner did not read A2 at 192 kHz\n";
        return false;
    }

    // Above about 1.1 kHz the old detector read an octave low (its normalisation began at the
    // shortest period); E6, the 24th fret of the high E, now reads E6.
    readings = Readings(48000.0, Tone(48000.0, 1318.51, 1.0));

    if (!Steady(readings, 8, 1318.51, mean, spread) || std::abs(mean) > 1.0 || Last(readings).noteName != "E" ||
        Last(readings).octave != 6)
    {
        std::cerr << "Tuner did not read E6: " << Last(readings).noteName << Last(readings).octave << "\n";
        return false;
    }

    // Each reading averages the tracker's 5 ms detections over its 43 ms. With white noise 20 dB
    // below a held A2, readings scatter by a fraction of a cent; the old single 85 ms window
    // scattered by 2.6 cents and read 1.7 cents sharp.
    readings = Readings(48000.0, Tone(48000.0, 110.0, 2.0, 0.0211));

    if (!Steady(readings, 8, 110.0, mean, spread) || std::abs(mean) > 0.5 || spread > 1.0)
    {
        std::cerr << "Tuner readings in noise: mean " << mean << ", spread " << spread << " cents\n";
        return false;
    }

    std::cout << "Noisy A2: mean " << mean << " cents, spread " << spread << " cents\n";

    // The reference pitch: A4 = 432 Hz reads in tune against 432 and 31.8 cents flat against 440.
    const auto tone432 = Tone(48000.0, 432.0, 1.0);
    const auto at432 = Readings(48000.0, tone432, 432.0);
    const auto at440 = Readings(48000.0, tone432, 440.0);

    if (!Steady(at432, 8, 432.0, mean, spread) || !Steady(at440, 8, 432.0, mean, spread) ||
        Last(at432).noteName != "A" || std::abs(Last(at432).centOffset) > 0.5 || Last(at440).noteName != "A" ||
        std::abs(Last(at440).centOffset + 31.77) > 0.5)
    {
        std::cerr << "Tuner reference pitch not honoured\n";
        return false;
    }

    // Silence reports no note, and a new note is reported within three readings (130 ms).
    auto change = Tone(48000.0, 0.0, 0.5);
    const auto a2 = Tone(48000.0, 110.0, 0.8);
    const auto d3 = Tone(48000.0, 146.83, 0.5);
    change.insert(change.end(), a2.begin(), a2.end());
    const std::size_t firstD3 = (change.size() + 2047) / 2048;
    change.insert(change.end(), d3.begin(), d3.end());
    readings = Readings(48000.0, change);
    bool silent = readings.size() > firstD3 + 3;

    for (std::size_t i = 0; i < 10 && i < readings.size(); ++i)
    {
        silent = silent && !readings[i].detected;
    }

    bool followed = false;

    for (std::size_t i = firstD3; i < firstD3 + 3 && i < readings.size(); ++i)
    {
        followed = followed || (readings[i].detected && std::abs(Cents(readings[i].frequency, 146.83)) < 5.0);
    }

    if (!silent || !followed)
    {
        std::cerr << "Tuner silence/note change: silent " << silent << ", followed " << followed << "\n";
        return false;
    }

    return true;
}

/// `seconds` of a bass-like tone at `frequency`: its fundamental well under the second and third
/// harmonics, as from a pickup near the bridge.
std::vector<float> BassTone(double sampleRate, double frequency, double seconds)
{
    constexpr double kHarmonics[] = {0.25, 1.0, 0.6, 0.4, 0.25, 0.15};
    std::vector<float> signal(static_cast<std::size_t>(seconds * sampleRate));

    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        double sum = 0.0;

        for (std::size_t h = 0; h < std::size(kHarmonics); ++h)
        {
            sum += kHarmonics[h] *
                   std::sin(2.0 * kPi * frequency * static_cast<double>(h + 1) * static_cast<double>(n) / sampleRate);
        }

        signal[n] = static_cast<float>(0.12 * sum);
    }

    return signal;
}

/// A bass's low strings, below the 45 Hz the tracker stops at by default, read as their own
/// notes. Returns false on a failure.
bool TestBass()
{
    struct Note
    {
        double frequency;
        const char* name;
        int octave;
    };

    // An extended-range bass's low C#, the same a quarter-tone flat, a seven-string's low F#, a
    // five-string's low B and a four-string's low E.
    const Note notes[] = {{17.324, "C#/Db", 0},
                          {17.324 * std::pow(2.0, -25.0 / 1200.0), "C#/Db", 0},
                          {23.125, "F#/Gb", 0},
                          {30.868, "B", 0},
                          {41.203, "E", 1}};

    for (const double sampleRate : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        for (const Note& note : notes)
        {
            double mean = 0.0;
            double spread = 0.0;
            const auto readings = Readings(sampleRate, BassTone(sampleRate, note.frequency, 1.0));

            if (!Steady(readings, 4, note.frequency, mean, spread) || std::abs(mean) > 1.0 ||
                Last(readings).noteName != note.name || Last(readings).octave != note.octave)
            {
                std::cerr << "Tuner did not read " << note.name << note.octave << " (" << note.frequency << " Hz) at "
                          << sampleRate << " Hz: " << Last(readings).noteName << Last(readings).octave << ", "
                          << Last(readings).frequency << " Hz\n";
                return false;
            }
        }
    }

    std::cout << "Bass low C#, low F#, low B and low E read at 44.1-192 kHz\n";
    return true;
}

/// A strummed chord: each note with its first five harmonics, falling off as a string's do.
std::vector<float> Chord(double sampleRate, const std::vector<double>& frequencies, double seconds)
{
    std::vector<float> signal(static_cast<std::size_t>(seconds * sampleRate));

    for (const double frequency : frequencies)
    {
        for (int harmonic = 1; harmonic <= 5; ++harmonic)
        {
            const double amplitude = 0.08 / harmonic;

            for (std::size_t n = 0; n < signal.size(); ++n)
            {
                signal[n] += static_cast<float>(
                    amplitude * std::sin(2.0 * kPi * frequency * harmonic * static_cast<double>(n) / sampleRate));
            }
        }
    }

    return signal;
}

/// The pitch classes holding the most of a chroma vector, strongest first.
std::vector<int> Strongest(const ChromaAnalyzer::Chroma& chroma, std::size_t count)
{
    std::vector<int> order(12);

    for (int i = 0; i < 12; ++i)
    {
        order[static_cast<std::size_t>(i)] = i;
    }

    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return chroma[static_cast<std::size_t>(a)] > chroma[static_cast<std::size_t>(b)]; });
    order.resize(count);
    std::sort(order.begin(), order.end());
    return order;
}

/// The Scale Guide's LISTEN: chroma vectors of chords, and the tuner reading for it while off.
bool TestListen()
{
    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        ChromaAnalyzer analyzer(ChromaAnalyzer::WindowFor(sampleRate));
        const auto window = analyzer.Size();

        // C major in open position (C3 E3 G3 C4 E4), A minor (A2 E3 A3 C4 E4) and a lone A3.
        const auto cMajor = Chord(sampleRate, {130.81, 164.81, 196.0, 261.63, 329.63}, 0.5);
        const auto aMinor = Chord(sampleRate, {110.0, 164.81, 220.0, 261.63, 329.63}, 0.5);
        const auto a3 = Chord(sampleRate, {220.0}, 0.5);
        const auto c = analyzer.Analyze(cMajor.data() + 1000, sampleRate);
        const auto am = analyzer.Analyze(aMinor.data() + 1000, sampleRate);
        const auto a = analyzer.Analyze(a3.data() + 1000, sampleRate);
        const std::vector<float> silence(window, 0.0f);
        const auto quiet = analyzer.Analyze(silence.data(), sampleRate);
        float total = 0.0f;

        for (const float value : c)
        {
            total += value;
        }

        if (Strongest(c, 3) != std::vector<int>{0, 4, 7} || Strongest(am, 3) != std::vector<int>{0, 4, 9} ||
            Strongest(a, 1) != std::vector<int>{9} || std::abs(total - 1.0f) > 1e-3f ||
            std::any_of(quiet.begin(), quiet.end(), [](float v) { return v != 0.0f; }))
        {
            std::cerr << "Chroma misread a chord at " << sampleRate << " Hz\n";
            return false;
        }

        // Without overtone removal E's 3rd harmonic (B) and G's 3rd (D) hold this near 0.87.
        if (c[0] + c[4] + c[7] < 0.95f)
        {
            std::cerr << "C major's tones hold only " << (c[0] + c[4] + c[7]) << " of its chroma\n";
            return false;
        }
    }

    // Listening reads the input with the tuner off, and sends the chroma with each reading.
    constexpr double sampleRate = 48000.0;
    TunerEngine tuner;
    tuner.Prepare(sampleRate);
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<TunerEngine::Result> withChroma;
    tuner.SetCallback([&](const TunerEngine::Result& result) {
        std::lock_guard<std::mutex> lock(mutex);

        if (result.hasChroma && result.levelDb > -60.0)
        {
            withChroma = result;
            cv.notify_one();
        }
    });
    tuner.SetListening(true, true);

    if (tuner.IsEnabled() || !tuner.IsActive())
    {
        std::cerr << "Listening turned the tuner on, or did not start reading\n";
        return false;
    }

    // E major in open position: E2 B2 E3 G#3 B3 E4.
    const auto eMajor = Chord(sampleRate, {82.41, 123.47, 164.81, 207.65, 246.94, 329.63}, 1.0);

    for (std::size_t start = 0; start + 64 <= eMajor.size(); start += 64)
    {
        tuner.Process(eMajor.data() + start, 64);
    }

    {
        std::unique_lock<std::mutex> lock(mutex);

        if (!cv.wait_for(lock, std::chrono::seconds(3), [&] { return withChroma.has_value(); }) ||
            Strongest(withChroma->chroma, 3) != std::vector<int>{4, 8, 11})
        {
            std::cerr << "Listening gave no chroma, or misread E major\n";
            return false;
        }
    }

    tuner.SetListening(false, false);

    if (tuner.IsActive())
    {
        std::cerr << "The tuner kept reading after listening stopped\n";
        return false;
    }

    std::cout << "Chroma reads C, Am, A3 and E major; listening reads with the tuner off\n";
    return true;
}
} // namespace

int main()
{
    constexpr double sampleRate = 48000.0;
    constexpr double frequency = 110.0;
    constexpr int blockSize = 64;
    constexpr double pi = 3.14159265358979323846;

    TunerEngine tuner;
    tuner.Prepare(sampleRate);
    tuner.SetReferenceFrequency(440.0);
    tuner.SetLiveMode(false);

    if (tuner.IsLiveMode() || tuner.GetReferenceFrequency() != 440.0)
    {
        std::cerr << "Tuner settings were not retained\n";
        return 1;
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::optional<TunerEngine::Result> detected;
    tuner.SetCallback([&](const TunerEngine::Result& result) {
        if (result.detected)
        {
            std::lock_guard<std::mutex> lock(mutex);
            detected = result;
            cv.notify_one();
        }
    });
    tuner.SetEnabled(true);

    std::vector<float> block(blockSize);

    for (int start = 0; start < 16384; start += blockSize)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            block[static_cast<std::size_t>(i)] =
                static_cast<float>(0.5 * std::sin(2.0 * pi * frequency * (start + i) / sampleRate));
        }

        tuner.Process(block.data(), blockSize);
    }

    TunerEngine::Result result;
    {
        std::unique_lock<std::mutex> lock(mutex);

        if (!cv.wait_for(lock, std::chrono::seconds(3), [&] { return detected.has_value(); }))
        {
            std::cerr << "Tuner did not detect a sustained A2 input\n";
            return 1;
        }

        result = *detected;
    }

    if (result.noteName != "A" || result.octave != 2 || std::abs(result.frequency - frequency) > 3.0 ||
        result.debugRms < 0.1)
    {
        std::cerr << "Unexpected tuner result: " << result.noteName << result.octave << " at " << result.frequency
                  << " Hz\n";
        return 1;
    }

    tuner.SetEnabled(false);

    if (tuner.IsEnabled())
    {
        std::cerr << "Tuner remained enabled\n";
        return 1;
    }

    return TestPitchTracker() && TestBass() && TestListen() ? 0 : 1;
}
