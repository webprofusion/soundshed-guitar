/**
 * @file SimpleCabLoFiTests.cpp
 * @brief The Simple Cabinet's lo-fi voicings: AM Radio, Telephone, Boombox, Practice Amp, VHS.
 *
 *   - the five guitar cabinets measure exactly as they did before these were appended
 *   - each lo-fi voicing keeps to the band it stands for
 *   - the cabinet control reaches them, and clamps at the last
 */

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#include "dsp/effects/SimpleCabEffect.h"

namespace
{
using guitarfx::SimpleCabEffect;
namespace cab = guitarfx::simple_cab;

int gFailures = 0;

void Check(bool condition, const std::string& what)
{
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what << std::endl;
}

void CheckRange(const std::string& label, double value, double minimum, double maximum)
{
    Check(value >= minimum && value <= maximum, label + ": expected " + std::to_string(minimum) + ".." +
                                                    std::to_string(maximum) + ", got " + std::to_string(value));
}

/// The five guitar cabinets, as they measured before the lo-fi voicings were appended after
/// them: twelve log-spaced points from 40 Hz to 12 kHz at default settings, 48 kHz. A preset
/// that stores cabinet 0 to 4 must sound exactly as it did.
void TestGuitarCabinetsUnchangedByLoFiOnes()
{
    constexpr double kBefore[5][12] = {
        {-6.699020891, 2.439313785, 8.794111507, 8.575553633, 4.582932953, 2.058892671, 0.976963277, 1.030899574,
         3.387810924, 0.938576484, -17.759806643, -53.503818709},
        {-10.552751823, -0.736941756, 7.417844832, 5.690033015, 4.260191999, 4.444891794, 4.354845507, 3.819653158,
         4.997797317, 4.509282374, -7.532486344, -43.021047529},
        {-8.029782731, 2.179867810, 8.733756455, 5.568647298, 3.816108961, 3.613648432, 3.191267262, 3.103260751,
         5.153177099, 3.584728607, -11.949351854, -47.642978765},
        {-7.124095559, 2.471488616, 9.225400147, 7.446208014, 3.918923293, 2.214292742, 1.629899511, 1.855405101,
         4.175528423, 2.161358908, -15.656823715, -51.396585569},
        {-8.012516657, 0.374107323, 6.495833547, 8.052010028, 4.150809153, 2.206963697, 2.423695556, 2.872308571,
         4.262721378, 5.770842872, -4.251181195, -39.480486869},
    };
    const cab::Voicer voicer(48000.0);
    double worst = 0.0;

    for (int c = 0; c < 5; ++c)
    {
        cab::Settings settings;
        settings.cabinet = static_cast<cab::Cabinet>(c);
        const auto design = voicer.Build(settings);

        for (int i = 0; i < 12; ++i)
        {
            const double hz = 40.0 * std::pow(300.0, i / 11.0);
            worst = std::max(worst, std::fabs(voicer.MagnitudeDb(design, hz) - kBefore[c][i]));
        }
    }

    Check(worst < 1.0e-6, "the five guitar cabinets measure as before (worst " + std::to_string(worst) + " dB)");
    Check(static_cast<int>(cab::Cabinet::AmRadio) == 5 && static_cast<int>(cab::Cabinet::Vhs) == 9 &&
              static_cast<int>(cab::Cabinet::Count) == 10,
          "the lo-fi voicings are appended, so stored indices keep their meaning");
}

/// The lo-fi voicings are small band-limited speakers. Each is held to the band it stands for,
/// relative to its own level at 1 kHz, with every control at noon.
void TestLoFiCabinets()
{
    struct Band
    {
        cab::Cabinet cabinet;
        const char* name;
        double lowMin, lowMax;   ///< where the low end is 3 dB down
        double highMin, highMax; ///< where the top end is 3 dB down
    };

    const Band bands[] = {
        {cab::Cabinet::AmRadio, "AM Radio", 250.0, 450.0, 2200.0, 3200.0},
        {cab::Cabinet::Telephone, "Telephone", 300.0, 450.0, 2800.0, 3600.0},
        {cab::Cabinet::Boombox, "Boombox", 90.0, 140.0, 5000.0, 7000.0},
        {cab::Cabinet::PracticeAmp, "Practice Amp", 150.0, 220.0, 3500.0, 5000.0},
        {cab::Cabinet::Vhs, "VHS", 80.0, 130.0, 4000.0, 6000.0},
    };
    const cab::Voicer voicer(48000.0);
    const auto design = [&](cab::Cabinet cabinet) {
        cab::Settings settings;
        settings.cabinet = cabinet;
        return voicer.Build(settings);
    };
    const auto relative = [&](const cab::Design& d, double hz) {
        return voicer.MagnitudeDb(d, hz) - voicer.MagnitudeDb(d, 1000.0);
    };
    const auto peakBetween = [&](const cab::Design& d, double from, double to) {
        double peak = -200.0;

        for (double hz = from; hz <= to; hz *= 1.01)
        {
            peak = std::max(peak, relative(d, hz));
        }

        return peak;
    };

    for (const Band& band : bands)
    {
        const auto d = design(band.cabinet);
        double low = 0.0;
        double high = 0.0;

        for (double hz = 20.0; hz < 1000.0; hz *= 1.005)
        {
            low = relative(d, hz) < -3.0 ? hz : low;
        }

        for (double hz = 20000.0; hz > 1000.0; hz /= 1.005)
        {
            high = relative(d, hz) < -3.0 ? hz : high;
        }

        CheckRange(std::string(band.name) + " low corner (Hz)", low, band.lowMin, band.lowMax);
        CheckRange(std::string(band.name) + " high corner (Hz)", high, band.highMin, band.highMax);
    }

    CheckRange("AM Radio honk between 1 and 2 kHz", peakBetween(design(cab::Cabinet::AmRadio), 1000.0, 2000.0), 3.0,
               9.0);
    const auto telephone = design(cab::Cabinet::Telephone);
    CheckRange("Telephone passband ripple", peakBetween(telephone, 500.0, 2800.0), 0.0, 3.0);
    CheckRange("Telephone at 100 Hz", relative(telephone, 100.0), -60.0, -20.0);
    CheckRange("Telephone at 8 kHz", relative(telephone, 8000.0), -90.0, -24.0);
    CheckRange("Boombox boom between 120 and 220 Hz", peakBetween(design(cab::Cabinet::Boombox), 120.0, 220.0), 4.0,
               9.0);
    CheckRange("Practice Amp box between 300 and 600 Hz", peakBetween(design(cab::Cabinet::PracticeAmp), 300.0, 600.0),
               2.0, 6.0);
    CheckRange("VHS has no resonance", peakBetween(design(cab::Cabinet::Vhs), 60.0, 6000.0), -1.0, 2.0);

    // As loud as the guitar cabinets, so switching to one is a change of tone and not of level.
    for (const Band& band : bands)
    {
        CheckRange(std::string(band.name) + " trim (dB)", voicer.CabinetTrimDb(band.cabinet), 0.0, 12.0);
    }

    SimpleCabEffect effect;
    effect.Prepare(48000.0, 512);
    effect.SetParam("cabinet", 9.0);
    Check(effect.GetParam("cabinet") == 9.0, "the cabinet control reaches the last voicing");
    effect.SetParam("cabinet", 10.0);
    Check(effect.GetParam("cabinet") == 9.0, "and clamps there");
}
} // namespace

int main()
{
    std::cout << "=== SimpleCabLoFiTests ===" << std::endl;
    TestGuitarCabinetsUnchangedByLoFiOnes();
    TestLoFiCabinets();
    std::cout << (gFailures == 0 ? "\nSimpleCabLoFiTests passed" : "\nSimpleCabLoFiTests FAILED") << std::endl;
    return gFailures == 0 ? 0 : 1;
}
