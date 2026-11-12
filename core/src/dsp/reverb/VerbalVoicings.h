#pragma once

/**
 * VerbalVoicings.h — what a Verbal engine is told and what each machine is: the controls in
 * natural units (Settings), and every machine's fixed voicing of the tank and early field, with
 * the alternate sizes of Room, Hall and Plate. VerbalEngine.h runs them.
 */

#include "dsp/reverb/VerbalParams.h"

#include <array>
#include <cstddef>

namespace guitarfx::verbal
{
/// Every control the engine reads, in natural units.
struct Settings
{
    Machine machine = Machine::Hall;
    double decay = 3.0, preDelayMs = 20.0, tone = 0.6, mod = 0.4, lowEnd = 0.5;
    int roomSize = 0, hallSize = 0, plateSize = 1, dwell = 1, springs = 1;
    double midEq = 0.5, bloomLength = 0.3, bloomFeedback = 0.1, diffusion = 0.6;
    int vowel = 0, resonance = 1, shimmerMode = 1, shift1 = 25, shift2 = 0;
    double shimmerAmount = 0.6, echoTimeMs = 400.0, echoFeedback = 0.5;
    int heads = 0, spacing = 0, shape = 1, roomShape = 2;
    double shapeTimeMs = 800.0, shapeFeedback = 0.0, lateLevel = 0.45, modSpeed = 0.5;
    double size = 1.0, listenerX = 0.33, listenerY = 0.0;
};

/// The fixed voicing of a machine's tank and early field.
struct Voicing
{
    double lineMinMs, lineMaxMs; ///< the tank's 16 lengths span this
    double gapMs;                ///< built-in delay before anything arrives, on top of Pre-Delay
    std::size_t erCount;
    double erEndMs, erDecayDb;
    float erLevel, erToTank;
    std::size_t diffStages;
    double diffMinMs, diffMaxMs;
    float diffGain;
    double modMs, modHz;
    double highCrossoverHz;
    float makeupDb;
    double decayScale;           ///< measured RT60 over Decay; the Cloud runs long by design
    double toneMinHz, toneMaxHz; ///< Tone sweeps the output low-pass between these
    double highRatioMin;         ///< the high band's decay ratio at Tone 0 (1 at Tone 1)
};

// Indexed by Machine, with the alternate Size of Room, Hall and Plate at the end. Columns: line
// min/max ms, gap ms, ER count, ER end ms, ER decay dB, ER level, ER to tank, diffuser stages,
// diffuser min/max ms, diffuser gain, mod ms, mod Hz, high crossover Hz, makeup dB, decay scale,
// tone low-pass min/max Hz, high-band ratio at Tone 0.
// clang-format off
inline constexpr std::array<Voicing, kMachineCount + 3> kVoicings = {{
    { 7.0,  29.0, 18.0, 18,  40.0, 10.0, 0.55f, 0.5f, 4, 1.2,  6.0, 0.55f, 0.25,  0.7, 3000.0, -2.9f, 1.09, 1800.0, 10000.0, 0.18}, // Room
    {23.0,  71.0, 30.0, 16,  70.0,  8.0, 0.45f, 0.6f, 4, 2.0,  9.0,  0.6f, 0.35,  0.5, 3500.0, -0.9f, 1.09, 1800.0, 10000.0, 0.18}, // Hall
    { 9.0,  33.0,  2.0,  0,   0.0,  0.0,  0.0f, 0.0f, 6, 0.8,  7.0,  0.7f,  0.3,  0.9, 6000.0, -3.2f, 1.09, 5000.0, 20000.0, 0.45}, // Plate
    {26.0,  54.0, 10.0,  0,   0.0,  0.0,  0.0f, 0.0f, 0, 1.0,  1.0,  0.0f,  0.0,  0.7, 3000.0, -4.0f,  1.0, 3000.0, 16000.0, 0.18}, // Spring
    {23.0,  71.0,  8.0, 12,  60.0,  8.0,  0.3f, 0.6f, 4, 2.0,  9.0,  0.6f, 0.45,  0.6, 3500.0, -5.5f, 1.09, 1600.0, 20000.0, 0.18}, // Swell
    {31.0,  97.0, 25.0,  0,   0.0,  0.0,  0.0f, 0.0f, 0, 1.0,  1.0,  0.0f,  0.3, 0.45, 4000.0,  2.0f, 1.07, 1000.0,  9000.0, 0.18}, // Bloom
    {47.0, 157.0,  0.0,  0,   0.0,  0.0,  0.0f, 0.0f, 8, 6.0, 72.0, 0.72f,  0.6, 0.35, 5000.0,  9.5f,  2.0, 1400.0, 16000.0, 0.25}, // Cloud
    {23.0,  71.0, 30.0, 12,  60.0,  8.0,  0.3f, 0.6f, 4, 2.0,  9.0,  0.6f,  0.5,  0.5, 3500.0,  6.6f, 1.09,  900.0,  4000.0, 0.18}, // Choir
    {29.0,  89.0, 30.0, 10,  50.0,  8.0,  0.3f, 0.6f, 4, 2.0,  9.0,  0.6f,  0.4,  0.5, 4500.0,  0.0f,  1.0, 2200.0, 14000.0, 0.25}, // Shimmer
    {19.0,  61.0,  0.0,  0,   0.0,  0.0,  0.0f, 0.0f, 4, 2.0, 12.0,  0.7f,  0.3,  0.6, 3500.0,  5.9f, 1.09, 2000.0, 14000.0, 0.18}, // Tape Echo
    {23.0,  71.0,  0.0,  0,   0.0,  0.0,  0.0f, 0.0f, 4, 1.5,  9.0,  0.7f,  0.3,  0.6, 4000.0,  3.0f, 1.09, 2500.0, 16000.0, 0.18}, // Nonlinear
    { 5.0,  17.0,  3.0,  0,   0.0,  0.0,  0.0f, 0.0f, 0, 1.0,  1.0,  0.0f,  0.1,  0.8, 5000.0, -6.0f,  1.0, 2000.0,  8000.0,  0.3}, // Reflections
    {11.0,  41.0, 14.0, 22,  55.0,  9.0,  0.6f, 0.5f, 4, 1.5,  8.0, 0.55f,  0.3,  0.7, 3200.0, -2.9f, 1.09, 1800.0, 10000.0, 0.18}, // Room: Club
    {33.0, 107.0, 70.0, 20, 140.0,  7.0,  0.4f, 0.6f, 4, 3.0, 14.0,  0.5f,  0.4, 0.45, 3000.0, -0.9f, 1.09, 1600.0,  9000.0, 0.18}, // Hall: Arena
    { 5.0,  19.0,  1.0,  0,   0.0,  0.0,  0.0f, 0.0f, 6, 0.6,  5.0, 0.72f,  0.3,  1.0, 5000.0, -3.2f, 1.09, 5000.0, 20000.0, 0.45}, // Plate: Small
}};
// clang-format on

[[nodiscard]] inline const Voicing& VoicingFor(const Settings& s) noexcept
{
    switch (s.machine)
    {
    case Machine::Room:
        return kVoicings[s.roomSize == 1 ? kMachineCount : 0];
    case Machine::Hall:
        return kVoicings[s.hallSize == 1 ? kMachineCount + 1 : 1];
    case Machine::Plate:
        return kVoicings[s.plateSize == 0 ? kMachineCount + 2 : 2];
    default:
        return kVoicings[static_cast<std::size_t>(s.machine)];
    }
}
} // namespace guitarfx::verbal
