/**
 * Instruments.cpp - Tuner smoothing and tap tempo (see Instruments.h).
 */

#include "uiclient/Instruments.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace guitarfx::uiclient
{
namespace
{
constexpr std::array<const char*, 12> kNoteNames = {"C",  "C#/Db", "D",  "D#/Eb", "E",  "F",
                                                    "F#/Gb", "G", "G#/Ab", "A", "A#/Bb", "B"};

std::string FirstSpelling(const std::string& name)
{
    const auto slash = name.find('/');
    return slash == std::string::npos ? name : name.substr(0, slash);
}
} // namespace

TunerModel::Reading TunerModel::Push(bool detected, const std::string& noteName, int octave, double cents,
                                     double frequency)
{
    Reading reading;

    if (!detected)
    {
        mSamples.clear();
        return reading;
    }

    mSamples.push_back({cents, frequency});

    while (mSamples.size() > kWindow)
    {
        mSamples.pop_front();
    }

    double centSum = 0.0;
    double frequencySum = 0.0;

    for (const auto& sample : mSamples)
    {
        centSum += sample.cents;
        frequencySum += sample.frequency;
    }

    const auto count = static_cast<double>(mSamples.size());
    reading.detected = true;
    reading.cents = centSum / count;
    reading.frequency = frequencySum / count;
    reading.octave = octave;
    reading.inTune = std::abs(reading.cents) < kInTuneCents;
    reading.needle = std::clamp(0.5 + (reading.cents / 50.0) * 0.45, 0.05, 0.95);

    const auto base = FirstSpelling(noteName);
    reading.note = base.empty() ? std::string("?") : base;
    reading.noteLeft = "-";
    reading.noteRight = "-";

    for (std::size_t i = 0; i < kNoteNames.size(); ++i)
    {
        if (!base.empty() && std::string(kNoteNames[i]).rfind(base, 0) == 0)
        {
            reading.noteLeft = FirstSpelling(kNoteNames[(i + 11) % 12]);
            reading.noteRight = FirstSpelling(kNoteNames[(i + 1) % 12]);
            break;
        }
    }

    return reading;
}

void TunerModel::Reset()
{
    mSamples.clear();
}

std::optional<double> TapTempo::Tap(double nowSeconds)
{
    if (!mTaps.empty() && nowSeconds - mTaps.back() > kResetSeconds)
    {
        mTaps.clear();
    }

    mTaps.push_back(nowSeconds);

    while (mTaps.size() > kHistory)
    {
        mTaps.pop_front();
    }

    if (mTaps.size() < 3)
    {
        return std::nullopt;
    }

    const double averageInterval = (mTaps.back() - mTaps.front()) / static_cast<double>(mTaps.size() - 1);

    if (averageInterval <= 0.0)
    {
        return std::nullopt;
    }

    return ClampBpm(60.0 / averageInterval);
}

double ClampBpm(double bpm)
{
    if (!std::isfinite(bpm))
    {
        return 120.0;
    }

    return std::clamp(std::round(bpm * 10.0) / 10.0, 30.0, 300.0);
}
} // namespace guitarfx::uiclient
