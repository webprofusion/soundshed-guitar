#pragma once

/**
 * Instruments.h - The small pieces of logic behind the tuner and the metronome's tap tempo,
 * as the web UI has them (core/ui/ts/tuner.ts, core/ui/ts/metronome.ts).
 */

#include <deque>
#include <optional>
#include <string>

namespace guitarfx::uiclient
{
/// Smooths the tuner's readings over the last six, and says what the display shows.
class TunerModel
{
public:
    static constexpr std::size_t kWindow = 6;

    /// Within this many cents either way the note reads as in tune.
    static constexpr double kInTuneCents = 3.0;

    struct Reading
    {
        bool detected = false;
        std::string note;      // "E", "C#" (the first spelling of "C#/Db")
        std::string noteLeft;  // the semitone below
        std::string noteRight; // the semitone above
        int octave = 0;
        double cents = 0.0;
        double frequency = 0.0;
        bool inTune = false;

        /// Where the needle sits, 0..1 with 0.5 in tune; ±50 cents spans 5%..95%.
        double needle = 0.5;
    };

    /// One engine reading in; the smoothed display reading out. A reading with nothing
    /// detected clears the history, so a new note does not average with the last.
    Reading Push(bool detected, const std::string& noteName, int octave, double cents, double frequency);

    void Reset();

private:
    struct Sample
    {
        double cents;
        double frequency;
    };

    std::deque<Sample> mSamples;
};

/// Tap tempo: at least three taps, averaged over the last eight, reset after 2.5 s of quiet.
class TapTempo
{
public:
    static constexpr double kResetSeconds = 2.5;
    static constexpr std::size_t kHistory = 8;

    /// A tap at `nowSeconds`; the tempo once there are enough taps to tell.
    std::optional<double> Tap(double nowSeconds);

private:
    std::deque<double> mTaps;
};

/// Rounds to 0.1 BPM and clamps to 30..300, as the metronome's controls do.
[[nodiscard]] double ClampBpm(double bpm);
} // namespace guitarfx::uiclient
