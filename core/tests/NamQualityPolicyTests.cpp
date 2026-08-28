/**
 * @file NamQualityPolicyTests.cpp
 * @brief Verifies the NAM quality policy applied when a host renders offline.
 *
 * A DAW flips AudioProcessor::setNonRealtime() around a bounce/freeze/export, which
 * reaches PluginController::SetOfflineRendering(). Offline there is no realtime deadline,
 * so NAM should render at full quality regardless of the tier chosen for live playing —
 * without modifying the user's stored settings.
 */

#include <iostream>
#include <string>

#include "PluginController.h"
#include "dsp/effects/NAMOversampling.h"
#include "dsp/effects/NAMSlimmableSettings.h"

namespace
{
using NamQuality = guitarfx::PluginController::NamQualityConfig;

int gFailures = 0;

void Check(bool condition, const std::string& label)
{
    std::cout << "  " << label << ": " << (condition ? "PASS" : "FAIL") << "\n";
    if (!condition)
    {
        ++gFailures;
    }
}

/// The headline behaviour: a low realtime tier is lifted for the render.
void TestBoostRaisesLowTier()
{
    NamQuality userTier;
    userTier.slimmableSize = 0.25;    // heavily slimmed for live playing
    userTier.oversamplingIndex = 0;   // oversampling off
    userTier.antiAliasPhaseIndex = 0; // minimum phase

    const NamQuality rendered = guitarfx::PluginController::ApplyOfflineRenderBoost(userTier);

    Check(rendered.slimmableSize == guitarfx::kNamSlimmableSizeMax, "offline render uses maximum NAM quality");
    Check(rendered.oversamplingIndex == guitarfx::PluginController::kOfflineMinimumOversamplingIndex,
          "offline render lifts oversampling to the 2x floor");
    Check(guitarfx::NamOversamplingFactorFromIndex(rendered.oversamplingIndex) == 2, "the offline floor really is 2x");
}

/// The boost is a floor, not an override: a user rendering at 8x keeps 8x.
void TestBoostNeverLowersUserChoice()
{
    NamQuality userTier;
    userTier.slimmableSize = guitarfx::kNamSlimmableSizeMax;
    userTier.oversamplingIndex = 3;   // 8x
    userTier.antiAliasPhaseIndex = 2; // linear long

    const NamQuality rendered = guitarfx::PluginController::ApplyOfflineRenderBoost(userTier);

    Check(rendered.oversamplingIndex == 3, "a higher user oversampling factor is kept");
    Check(rendered.slimmableSize == guitarfx::kNamSlimmableSizeMax, "maximum quality stays maximum");
}

/// The anti-alias filter is deliberately untouched: the host compensates its latency
/// either way, and switching it would change the rendered phase response.
void TestBoostLeavesAntiAliasPhaseAlone()
{
    for (int phase = 0; phase <= guitarfx::kNamAntiAliasPhaseMaxIndex; ++phase)
    {
        NamQuality userTier;
        userTier.antiAliasPhaseIndex = phase;
        const NamQuality rendered = guitarfx::PluginController::ApplyOfflineRenderBoost(userTier);
        if (rendered.antiAliasPhaseIndex != phase)
        {
            Check(false, "anti-alias phase preserved for index " + std::to_string(phase));
            return;
        }
    }
    Check(true, "anti-alias phase is preserved for every setting");
}

/// The boosted tier must stay within the ranges the DSP accepts.
void TestBoostStaysInRange()
{
    NamQuality userTier;
    userTier.oversamplingIndex = guitarfx::kNamOversamplingMaxIndex;
    const NamQuality rendered = guitarfx::PluginController::ApplyOfflineRenderBoost(userTier);

    Check(rendered.oversamplingIndex == guitarfx::kNamOversamplingMaxIndex,
          "the maximum oversampling index survives the boost");
    Check(guitarfx::SanitizeNamSlimmableSize(rendered.slimmableSize) == rendered.slimmableSize,
          "boosted slimmable size is already in range");
    Check(guitarfx::SanitizeNamOversamplingIndex(rendered.oversamplingIndex) == rendered.oversamplingIndex,
          "boosted oversampling index is already in range");
}
} // namespace

int main()
{
    std::cout << "\n--- NAM offline render quality policy ---\n";
    TestBoostRaisesLowTier();
    TestBoostNeverLowersUserChoice();
    TestBoostLeavesAntiAliasPhaseAlone();
    TestBoostStaysInRange();

    if (gFailures > 0)
    {
        std::cerr << "NAM quality policy tests failed: " << gFailures << "\n";
        return 1;
    }

    std::cout << "All NAM quality policy tests passed.\n";
    return 0;
}
