#pragma once

#include "nativeui/theme/ThemeTokens.generated.h"

#include <juce_graphics/juce_graphics.h>

#include <string>

namespace soundshed::nano
{
/// The active theme's colours, and the measurements every view shares.
///
/// Colours come from the web UI's CSS (ThemeTokens.generated.h), so the theme a user picks
/// in either product - the shared "theme" app setting - looks the same in both.
class NanoTheme
{
public:
    NanoTheme() { setTheme ("dark"); }

    /// "light", "dark" or "classic"; anything else is dark, as the web UI normalises it.
    void setTheme (const std::string& name)
    {
        themeName = name == "light" || name == "classic" ? name : std::string ("dark");
        tokens = themeName == "light" ? &theme::kLight : (themeName == "classic" ? &theme::kClassic : &theme::kDark);
    }

    [[nodiscard]] const std::string& name() const noexcept { return themeName; }
    [[nodiscard]] bool isLight() const noexcept { return themeName == "light"; }
    [[nodiscard]] const theme::ThemeTokens& t() const noexcept { return *tokens; }

    [[nodiscard]] static juce::Colour argb (std::uint32_t value) noexcept { return juce::Colour (value); }

    [[nodiscard]] juce::Colour accent() const noexcept { return argb (t().accent); }
    [[nodiscard]] juce::Colour background() const noexcept { return argb (t().bgPrimary); }
    [[nodiscard]] juce::Colour panel() const noexcept { return argb (t().bgPanel); }
    [[nodiscard]] juce::Colour card() const noexcept { return argb (t().bgCard); }
    [[nodiscard]] juce::Colour elevated() const noexcept { return argb (t().bgElevated); }
    [[nodiscard]] juce::Colour input() const noexcept { return argb (t().bgInput); }
    [[nodiscard]] juce::Colour bar() const noexcept { return argb (t().bgSecondary); }
    [[nodiscard]] juce::Colour text() const noexcept { return argb (t().textPrimary); }
    [[nodiscard]] juce::Colour textSecondary() const noexcept { return argb (t().textSecondary); }
    [[nodiscard]] juce::Colour textMuted() const noexcept { return argb (t().textMuted); }
    [[nodiscard]] juce::Colour border() const noexcept { return argb (t().borderLight); }
    [[nodiscard]] juce::Colour borderStrong() const noexcept { return argb (t().borderMedium); }
    [[nodiscard]] juce::Colour success() const noexcept { return argb (t().success); }
    [[nodiscard]] juce::Colour error() const noexcept { return argb (t().error); }
    [[nodiscard]] juce::Colour warning() const noexcept { return argb (t().warning); }

    // ── Measurements (logical pixels, before the UI scale) ────────────────────
    static constexpr int gap = 8;
    static constexpr int radius = 8;
    static constexpr int barHeight = 48;
    static constexpr int stripHeight = 60;
    static constexpr int railWidth = 64;
    static constexpr int tabBarHeight = 56;
    static constexpr int transportHeight = 44;

private:
    std::string themeName;
    const theme::ThemeTokens* tokens = &theme::kDark;
};
} // namespace soundshed::nano
