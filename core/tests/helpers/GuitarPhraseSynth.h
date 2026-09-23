#pragma once

/**
 * @file GuitarPhraseSynth.h
 * @brief A synthetic, monophonic guitar line with known notes, for testing note detection.
 *
 * Each note is a plucked string: harmonics at the pluck position's spectrum, a little stiff-string
 * inharmonicity, upper harmonics dying faster than the fundamental, and a short burst of noise
 * for the pick. A note can instead be sounded legato (a hammer-on, pull-off or slide: the string
 * keeps ringing at a new pitch, with no pick), bent, given vibrato, and muted at its end. Notes are
 * played on one string, so a pick while the last note still rings restarts it at the new pitch,
 * as a guitarist's does.
 *
 * Every note's pitch, start and end are known exactly, which a recording cannot offer.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace guitarfx::test
{
struct PhraseNote
{
    double start = 0.0;  ///< seconds
    double length = 0.3; ///< seconds until it is muted, or the next note takes over
    double hz = 110.0;
    double peak = 0.3;   ///< the pick's strength, roughly the note's peak sample
    bool legato = false; ///< no pick: the string moves to this pitch still ringing
    bool muted = true;   ///< damped at the end; false lets it ring on into whatever follows
    double bendSemitones = 0.0;
    double bendStart = 0.0; ///< seconds after the note's start
    double bendTime = 0.1;  ///< seconds the bend takes to arrive
    double vibratoCents = 0.0;
    double vibratoHz = 5.5;
};

[[nodiscard]] inline double MidiNote(double hz)
{
    return 69.0 + 12.0 * std::log2(hz / 440.0);
}

[[nodiscard]] inline double NoteHz(double midi)
{
    return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
}

/// Renders `notes` (in start order) into `seconds` of mono audio at `sampleRate`.
[[nodiscard]] inline std::vector<float> RenderPhrase(const std::vector<PhraseNote>& notes, double seconds,
                                                     double sampleRate, std::uint32_t seed = 1)
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kMaxHarmonics = 40;
    constexpr double kPluckPosition = 0.18;
    constexpr double kInharmonicity = 1.0e-4;
    constexpr double kMuteSeconds = 0.008;
    constexpr double kPickSeconds = 0.0015;

    const auto total = static_cast<std::size_t>(seconds * sampleRate);
    std::vector<float> out(total, 0.0f);

    std::vector<double> phase(kMaxHarmonics, 0.0);
    std::vector<double> amplitude(kMaxHarmonics, 0.0);
    std::vector<double> decay(kMaxHarmonics, 1.0);
    std::uint32_t noise = seed * 2654435761u + 1u;
    double pick = 0.0;
    double pickDecay = std::exp(-1.0 / (kPickSeconds * sampleRate));
    double muteDecay = std::exp(-1.0 / (kMuteSeconds * sampleRate));

    // What the per-sample loop needs, worked out once: a test renders tens of millions of
    // samples, and in Debug every sqrt, std::min and checked vector[] there is a call.
    double stretch[kMaxHarmonics];

    for (int k = 1; k <= kMaxHarmonics; ++k)
    {
        stretch[k - 1] = std::sqrt(1.0 + kInharmonicity * k * k);
    }

    const double ceiling = std::min(0.45 * sampleRate, 9000.0);
    double* const phases = phase.data();
    double* const amplitudes = amplitude.data();
    const double* const decays = decay.data();

    std::size_t next = 0;
    const PhraseNote* current = nullptr;
    double noteTime = 0.0;
    bool muting = false;

    const auto pluck = [&](const PhraseNote& note) {
        // Pluck-position comb, falling as 1/k, normalised so the harmonics sum to the peak.
        double sum = 0.0;
        std::vector<double> weight(kMaxHarmonics, 0.0);

        for (int k = 1; k <= kMaxHarmonics; ++k)
        {
            weight[static_cast<std::size_t>(k - 1)] = std::abs(std::sin(k * kPi * kPluckPosition)) / k;
            sum += weight[static_cast<std::size_t>(k - 1)];
        }

        // Low notes ring longer; each harmonic dies faster than the one below it.
        const double fundamentalSeconds = std::clamp(3.0 * std::sqrt(110.0 / note.hz), 0.6, 4.0);

        for (int k = 1; k <= kMaxHarmonics; ++k)
        {
            const auto index = static_cast<std::size_t>(k - 1);
            amplitude[index] = note.peak * 1.6 * weight[index] / sum;
            const double tau = fundamentalSeconds / (1.0 + 0.5 * (k - 1));
            decay[index] = std::exp(-1.0 / (tau * sampleRate));
        }

        pick = note.peak * 0.6;
    };

    for (std::size_t n = 0; n < total; ++n)
    {
        const double t = static_cast<double>(n) / sampleRate;

        while (next < notes.size() && t >= notes[next].start)
        {
            const PhraseNote& note = notes[next];

            if (note.legato && current)
            {
                // The string keeps its energy; a hammer-on adds a little.
                for (auto& a : amplitude)
                {
                    a *= 1.15;
                }
            }
            else
            {
                pluck(note);
            }

            current = &note;
            noteTime = 0.0;
            muting = false;
            ++next;
        }

        if (!current)
        {
            continue;
        }

        if (!muting && current->muted && noteTime >= current->length)
        {
            muting = true;
        }

        double bend = 0.0;

        if (current->bendSemitones != 0.0 && noteTime > current->bendStart)
        {
            const double progress =
                std::min(1.0, (noteTime - current->bendStart) / std::max(1.0e-6, current->bendTime));
            bend = current->bendSemitones * progress;
        }

        if (current->vibratoCents != 0.0)
        {
            bend += current->vibratoCents / 100.0 * std::sin(2.0 * kPi * current->vibratoHz * noteTime);
        }

        const double f0 = current->hz * std::pow(2.0, bend / 12.0);
        double sample = 0.0;

        for (int k = 1; k <= kMaxHarmonics; ++k)
        {
            const int index = k - 1;
            const double fk = k * f0 * stretch[index];

            if (fk < ceiling)
            {
                phases[index] += fk / sampleRate;
                phases[index] -= std::floor(phases[index]);
                sample += amplitudes[index] * std::sin(2.0 * kPi * phases[index]);
            }

            amplitudes[index] *= muting ? muteDecay : decays[index];
        }

        noise = noise * 1664525u + 1013904223u;
        const double white = static_cast<double>(noise >> 8) / 8388608.0 - 1.0;
        sample += pick * white;
        pick *= pickDecay;

        out[n] = static_cast<float>(sample);
        noteTime += 1.0 / sampleRate;
    }

    return out;
}
} // namespace guitarfx::test
