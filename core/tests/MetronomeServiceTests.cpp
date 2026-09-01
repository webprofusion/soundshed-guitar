/**
 * @file MetronomeServiceTests.cpp
 * @brief Tests for the metronome's meter handling and click placement.
 *
 * The metronome grew from "four beats, one accent" into arbitrary meters with
 * groupings, four accent levels and subdivisions. All four settings are
 * resolved into a single bar plan on the message thread, and Render() walks it
 * on a tick cursor — so these tests come in two halves: the pure resolution
 * (patterns, groupings, plans) and the audio the cursor actually produces.
 *
 * The test host reports no bundled assets, so the click library resolves no
 * kits and Render() falls back to its synthesised beep — whatever directory
 * the binary is run from. That is deliberate: the timing and gain assertions
 * below are about the cursor, not the samples.
 */

#include "IPluginHost.h"
#include "controller/MetronomeService.h"
#include "controller/internal/MetronomeSupport.h"

#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
using guitarfx::MetronomeService;
using namespace guitarfx::controller_detail;

constexpr double kSampleRate = 48000.0;

/// Minimal standalone host. The metronome only asks it for the sample rate,
/// the tempo source and where assets live.
class TestHost final : public guitarfx::IPluginHost
{
  public:
    void SendMessageToUI(const std::string&) override
    {
    }

    void BrowseFileAsync(guitarfx::BrowseFileType, const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)>) override
    {
    }

    void SaveFileAsync(guitarfx::BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const guitarfx::BrowseFileResult&)>) override
    {
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        if (fn)
        {
            fn();
        }
    }

    [[nodiscard]] std::filesystem::path GetUserDataPath() const override
    {
        return {};
    }

    [[nodiscard]] std::filesystem::path GetBundledAssetsPath() const override
    {
        return {};
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return kSampleRate;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return 480;
    }

    [[nodiscard]] bool IsStandalone() const override
    {
        return true;
    }
};

struct Fixture
{
    TestHost host;
    nlohmann::json appSettings = nlohmann::json::object();
    std::filesystem::path resourceRoot;
    std::unique_ptr<MetronomeService> svc;

    Fixture()
    {
        svc = std::make_unique<MetronomeService>(host, appSettings, resourceRoot);
        svc->ApplySettingsFromAppSettings();
        svc->ApplyRequest({{"enabled", true}});
    }
};

/// One click found in the rendered buffer.
struct Onset
{
    std::size_t frame = 0;
    float peak = 0.0f;
};

/// Splits a rendered buffer into clicks. A run of `kGateFrames` frames below
/// the noise floor closes the current click; the beep is ~20 ms and the
/// shortest gap under test is 6000 frames, so the gate cannot merge two.
std::vector<Onset> CollectOnsets(const std::vector<float>& samples)
{
    constexpr float kThreshold = 1.0e-5f;
    constexpr int kGateFrames = 64;

    std::vector<Onset> onsets;
    bool inClick = false;
    int quietRun = 0;

    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const float magnitude = std::fabs(samples[i]);

        if (magnitude > kThreshold)
        {
            if (!inClick)
            {
                inClick = true;
                onsets.push_back({i, magnitude});
            }
            else
            {
                onsets.back().peak = std::max(onsets.back().peak, magnitude);
            }

            quietRun = 0;
            continue;
        }

        if (inClick && ++quietRun >= kGateFrames)
        {
            inClick = false;
            quietRun = 0;
        }
    }

    return onsets;
}

/// Renders `frames` frames in host-sized blocks and returns the left channel.
std::vector<float> Render(MetronomeService& svc, std::size_t frames, int blockSize = 480)
{
    std::vector<float> left(frames, 0.0f);
    std::vector<float> right(frames, 0.0f);

    for (std::size_t offset = 0; offset < frames; offset += static_cast<std::size_t>(blockSize))
    {
        const auto remaining =
            static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(blockSize), frames - offset));
        float* channels[2] = {left.data() + offset, right.data() + offset};
        svc.Render(channels, remaining);
    }

    return left;
}

bool Report(const std::string& label, bool passed, const std::string& detail = {})
{
    std::cout << "  " << std::left << std::setw(52) << (label + ":") << (passed ? "PASS" : "FAIL");

    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }

    std::cout << "\n";
    return passed;
}

// ── Meter resolution ──────────────────────────────────────────────────

bool TestDefaultPatterns()
{
    std::cout << "Default accent patterns\n";
    bool ok = true;

    ok &= Report("4/4 accents beat one only", DefaultBeatPattern(4, 4, "") == "HLLL", DefaultBeatPattern(4, 4, ""));
    ok &= Report("6/8 marks the second group medium", DefaultBeatPattern(6, 8, "") == "HLLMLL",
                 DefaultBeatPattern(6, 8, ""));
    ok &= Report("9/8 marks both later groups", DefaultBeatPattern(9, 8, "") == "HLLMLLMLL",
                 DefaultBeatPattern(9, 8, ""));
    ok &= Report("7/8 (2+2+3) follows its grouping", DefaultBeatPattern(7, 8, "2+2+3") == "HLMLMLL",
                 DefaultBeatPattern(7, 8, "2+2+3"));
    ok &= Report("5/4 stays flat past beat one", DefaultBeatPattern(5, 4, "") == "HLLLL", DefaultBeatPattern(5, 4, ""));

    return ok;
}

bool TestGroupingValidation()
{
    std::cout << "Grouping validation\n";
    bool ok = true;

    ok &= Report("2+2+3 parses for 7 beats", ParseGrouping("2+2+3", 7).size() == 3);
    ok &= Report("A grouping that misses the bar is dropped", ParseGrouping("2+2", 7).empty());
    ok &= Report("A zero group is dropped", ParseGrouping("3+0+4", 7).empty());
    ok &= Report("Junk is dropped", ParseGrouping("2+x", 7).empty());
    ok &=
        Report("Normalising rewrites separators", NormaliseGrouping("3, 2", 5) == "3+2", NormaliseGrouping("3, 2", 5));
    ok &= Report("Normalising drops a stale grouping", NormaliseGrouping("2+2+3", 4).empty());

    return ok;
}

bool TestPatternNormalisation()
{
    std::cout << "Pattern normalisation\n";
    bool ok = true;

    ok &= Report("A short pattern repeats to fill the bar", NormaliseBeatPattern("HL", 4, 4, "") == "HLHL",
                 NormaliseBeatPattern("HL", 4, 4, ""));
    ok &= Report("A long pattern is cut to the bar", NormaliseBeatPattern("HLLLSSS", 4, 4, "") == "HLLL",
                 NormaliseBeatPattern("HLLLSSS", 4, 4, ""));
    ok &= Report("Legacy silence characters still read as S", NormaliseBeatPattern("H-.L", 4, 4, "") == "HSSL",
                 NormaliseBeatPattern("H-.L", 4, 4, ""));
    ok &= Report("An empty pattern falls back to the default",
                 NormaliseBeatPattern("", 6, 8, "") == DefaultBeatPattern(6, 8, ""));
    ok &= Report("Unknown characters are ignored", NormaliseBeatPattern("H?L?L?L?", 4, 4, "") == "HLLL",
                 NormaliseBeatPattern("H?L?L?L?", 4, 4, ""));

    return ok;
}

bool TestBarPlan()
{
    std::cout << "Bar plan\n";
    bool ok = true;

    const auto plan = BuildBarPlan(7, 8, "2+2+3", "", "1/16");
    ok &= Report("Seven beats in the bar", plan.beats.size() == 7, std::to_string(plan.beats.size()));
    ok &= Report("An eighth-note beat is half a quarter", std::fabs(plan.beatScale - 0.5) < 1e-9);
    ok &= Report("1/16 puts four ticks in a beat", plan.ticksPerBeat == 4, std::to_string(plan.ticksPerBeat));
    ok &= Report("Beat one plays the accent voice", plan.beats[0].voice == ClickVoice::High);
    ok &= Report("A group head is a pulled-back accent",
                 plan.beats[2].level == BeatLevel::Medium && !plan.beats[2].silent &&
                     plan.beats[2].voice == ClickVoice::High &&
                     std::fabs(plan.beats[2].gain - kMetronomeMediumBeatGain) < 1e-6f);
    ok &=
        Report("Off beats play the normal voice", plan.beats[1].voice == ClickVoice::Low &&
                                                      std::fabs(plan.beats[1].gain - kMetronomeNormalBeatGain) < 1e-6f);

    const auto silent = BuildBarPlan(4, 4, "", "HSLS", "1/4");
    ok &= Report("Silent beats are marked silent", silent.beats[1].silent && silent.beats[3].silent);
    ok &= Report("An unknown subdivision falls back to quarters", silent.ticksPerBeat == 1);

    return ok;
}

// ── Click placement ───────────────────────────────────────────────────

bool TestBeatPlacement()
{
    std::cout << "Click placement\n";
    bool ok = true;

    Fixture fixture;
    fixture.svc->ApplyRequest({{"bpm", 120.0}, {"timeSigNum", 4}, {"timeSigDen", 4}});

    // 120 bpm at 48 kHz is 24000 frames a beat; two bars is 192000 frames.
    const auto onsets = CollectOnsets(Render(*fixture.svc, 192000));
    ok &= Report("Two bars of 4/4 give eight clicks", onsets.size() == 8, std::to_string(onsets.size()));

    bool spacingOk = onsets.size() == 8;

    for (std::size_t i = 1; i < onsets.size() && spacingOk; ++i)
    {
        const auto gap = static_cast<long long>(onsets[i].frame) - static_cast<long long>(onsets[i - 1].frame);
        spacingOk = std::llabs(gap - 24000) <= 1;
    }

    ok &= Report("Clicks land a beat apart", spacingOk);

    return ok;
}

bool TestSubdivisionAndMeter()
{
    std::cout << "Subdivisions and meters\n";
    bool ok = true;

    {
        Fixture fixture;
        fixture.svc->ApplyRequest({{"bpm", 120.0}, {"timeSigNum", 4}, {"timeSigDen", 4}, {"subdivision", "1/8"}});
        const auto onsets = CollectOnsets(Render(*fixture.svc, 96000));
        ok &= Report("1/8 doubles the ticks in a bar", onsets.size() == 8, std::to_string(onsets.size()));

        bool quieter = onsets.size() == 8;

        for (std::size_t i = 1; i < onsets.size() && quieter; i += 2)
        {
            quieter = onsets[i].peak < onsets[i - 1].peak;
        }

        ok &= Report("Subdivision ticks are quieter than beats", quieter);
    }

    {
        Fixture fixture;
        fixture.svc->ApplyRequest({{"bpm", 120.0}, {"timeSigNum", 7}, {"timeSigDen", 8}, {"grouping", "2+2+3"}});
        // An eighth-note beat is 12000 frames, so 96000 frames is eight beats.
        const auto onsets = CollectOnsets(Render(*fixture.svc, 96000));
        ok &= Report("7/8 clicks on eighth notes", onsets.size() == 8, std::to_string(onsets.size()));
    }

    {
        Fixture fixture;
        fixture.svc->ApplyRequest({{"bpm", 120.0}, {"timeSigNum", 4}, {"timeSigDen", 4}, {"beatPattern", "HSLS"}});
        const auto onsets = CollectOnsets(Render(*fixture.svc, 96000));
        ok &= Report("Silent beats produce no click", onsets.size() == 2, std::to_string(onsets.size()));
    }

    return ok;
}

bool TestAccentGains()
{
    std::cout << "Accent levels\n";
    bool ok = true;

    Fixture fixture;
    fixture.svc->ApplyRequest({{"bpm", 120.0}, {"timeSigNum", 4}, {"timeSigDen", 4}, {"beatPattern", "HMLL"}});

    const auto onsets = CollectOnsets(Render(*fixture.svc, 96000));

    if (onsets.size() != 4)
    {
        return Report("Four clicks to compare", false, std::to_string(onsets.size()));
    }

    // A medium beat is the accent voice pulled back, so against the accent on
    // beat one the ratio is exactly the medium gain.
    const float ratio = onsets[1].peak / onsets[0].peak;
    ok &= Report("A medium beat sits below the accent", std::fabs(ratio - kMetronomeMediumBeatGain) < 0.02f,
                 std::to_string(ratio));
    ok &= Report("A normal beat sits below a medium one", onsets[2].peak < onsets[1].peak);
    ok &= Report("Normal beats match each other", std::fabs(onsets[2].peak - onsets[3].peak) < 1.0e-6f);

    return ok;
}

bool TestBeatPulses()
{
    std::cout << "Beat pulses\n";
    bool ok = true;

    Fixture fixture;
    fixture.svc->ApplyRequest({{"bpm", 120.0}, {"timeSigNum", 3}, {"timeSigDen", 4}});

    MetronomeService::BeatPulse pulse;
    ok &= Report("Nothing to report before rendering", !fixture.svc->ConsumeBeatPulse(pulse));

    std::vector<int> seen;
    std::vector<int> levels;

    for (int beat = 0; beat < 6; ++beat)
    {
        Render(*fixture.svc, 24000);

        if (fixture.svc->ConsumeBeatPulse(pulse))
        {
            seen.push_back(pulse.beatIndex);
            levels.push_back(static_cast<int>(pulse.level));
        }
    }

    const std::vector<int> expected = {0, 1, 2, 0, 1, 2};
    ok &= Report("Beat index cycles with the bar", seen == expected, std::to_string(seen.size()) + " pulses");
    ok &= Report("Bar length travels with the pulse", pulse.beatsPerBar == 3, std::to_string(pulse.beatsPerBar));
    ok &= Report("Beat one reports as an accent",
                 !levels.empty() && levels.front() == static_cast<int>(BeatLevel::Accent));
    ok &= Report("A consumed pulse is not reported twice", !fixture.svc->ConsumeBeatPulse(pulse));

    return ok;
}

// ── Settings ──────────────────────────────────────────────────────────

bool TestMeterChangeReseedsPattern()
{
    std::cout << "Meter changes\n";
    bool ok = true;

    Fixture fixture;
    fixture.svc->ApplyRequest({{"beatPattern", "HSSL"}});
    ok &= Report("An explicit pattern is kept", fixture.svc->BeatPattern() == "HSSL", fixture.svc->BeatPattern());

    fixture.svc->ApplyRequest({{"timeSigNum", 6}, {"timeSigDen", 8}});
    ok &= Report("A meter change re-seeds the pattern", fixture.svc->BeatPattern() == "HLLMLL",
                 fixture.svc->BeatPattern());

    fixture.svc->ApplyRequest({{"timeSigNum", 5}, {"timeSigDen", 4}, {"beatPattern", "HLLML"}});
    ok &=
        Report("A pattern sent with the meter wins", fixture.svc->BeatPattern() == "HLLML", fixture.svc->BeatPattern());

    fixture.svc->ApplyRequest({{"grouping", "2+2+3"}});
    ok &= Report("A grouping that does not fit the meter is refused",
                 fixture.appSettings.value(kMetronomeGroupingSettingKey, std::string{"?"}).empty());

    nlohmann::json state;
    fixture.svc->AppendStateTo(state);
    ok &= Report("State carries the meter", state.value("timeSigNum", 0) == 5 && state.value("timeSigDen", 0) == 4);
    ok &= Report("State carries the pattern", state.value("beatPattern", std::string{}) == "HLLML");
    ok &= Report("State lists the subdivisions", state.contains("subdivisions") && state["subdivisions"].size() == 6);

    return ok;
}

bool TestSettingsRoundTrip()
{
    std::cout << "Settings round trip\n";
    bool ok = true;

    Fixture fixture;
    fixture.svc->ApplyRequest({{"timeSigNum", 7}, {"timeSigDen", 8}, {"grouping", "2+2+3"}, {"subdivision", "1/16"}});

    // A second service over the same settings must come back identical.
    MetronomeService reloaded(fixture.host, fixture.appSettings, fixture.resourceRoot);
    reloaded.ApplySettingsFromAppSettings();

    nlohmann::json state;
    reloaded.AppendStateTo(state);

    ok &= Report("Meter survives a reload", state.value("timeSigNum", 0) == 7 && state.value("timeSigDen", 0) == 8);
    ok &= Report("Grouping survives a reload", state.value("grouping", std::string{}) == "2+2+3");
    ok &= Report("Subdivision survives a reload", state.value("subdivision", std::string{}) == "1/16");
    ok &= Report("Pattern survives a reload", state.value("beatPattern", std::string{}) == "HLMLMLL",
                 state.value("beatPattern", std::string{}));

    return ok;
}

bool TestLegacySettingsMigration()
{
    std::cout << "Legacy settings migration\n";
    bool ok = true;

    TestHost host;
    std::filesystem::path resourceRoot;
    nlohmann::json settings = nlohmann::json::object();
    settings[kMetronomeClickTypeSettingKey] = "drum";
    settings[kMetronomeClickConfigSettingKey] = nlohmann::json::array({{{"id", "drum"},
                                                                        {"label", "Drum"},
                                                                        {"lowPath", "metronome/kit1/low.wav"},
                                                                        {"highPath", "metronome/kit1/high.wav"}}});
    settings[kMetronomeBeatPatternSettingKey] = "HLL";

    MetronomeService svc(host, settings, resourceRoot);
    svc.ApplySettingsFromAppSettings();

    ok &= Report("The old built-in click config is dropped", !settings.contains(kMetronomeClickConfigSettingKey));
    ok &= Report("The old click id maps to the bundled kit",
                 settings.value(kMetronomeClickTypeSettingKey, std::string{}) == kMetronomeDefaultClickType,
                 settings.value(kMetronomeClickTypeSettingKey, std::string{}));
    ok &=
        Report("A three-character pattern is stretched to the 4/4 bar", svc.BeatPattern() == "HLLH", svc.BeatPattern());

    return ok;
}
} // namespace

int main()
{
    bool allPassed = true;

    for (const auto& test : {TestDefaultPatterns, TestGroupingValidation, TestPatternNormalisation, TestBarPlan,
                             TestBeatPlacement, TestSubdivisionAndMeter, TestAccentGains, TestBeatPulses,
                             TestMeterChangeReseedsPattern, TestSettingsRoundTrip, TestLegacySettingsMigration})
    {
        if (!test())
        {
            allPassed = false;
        }
    }

    std::cout << "\n" << (allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << "\n";
    return allPassed ? 0 : 1;
}
