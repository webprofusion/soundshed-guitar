#pragma once

#include "nativeui/theme/ThemeTokens.generated.h"

#include <juce_graphics/juce_graphics.h>

#include <cmath>
#include <string>

namespace soundshed::nano
{
/// The weights Nano draws text in: static cuts of the web UI's Inter (theme/fonts, made by
/// tools/gen-nano-fonts.mjs). Body text is Regular, labels and values Medium, headings SemiBold.
enum class FontWeight
{
    regular,
    medium,
    semibold
};

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

    // ── The accent, used sparingly ────────────────────────────────────────────
    // A filled accent surface is for the one thing that is on or next: a switch that is on,
    // a slider's travel, the button that confirms. Selection elsewhere (tabs, chips, the page
    // in the rail) is neutral, so the accent still means something where it does appear.

    /// The accent as a fill. On the dark themes the token is a bright orange that glares as a
    /// block of colour, so fills take it a fifth of the way into the panel colour.
    [[nodiscard]] juce::Colour accentFill() const noexcept
    {
        return isLight() ? accent() : accent().interpolatedWith (panel(), 0.2f);
    }

    /// Text and icons on accentFill(): white only where it passes WCAG AA (4.5:1). The dark
    /// and classic accents are too light for white text, so they get near-black instead.
    [[nodiscard]] juce::Colour onAccent() const noexcept
    {
        const auto whiteContrast = 1.05f / (luminance (accentFill()) + 0.05f);
        return whiteContrast >= 4.5f ? juce::Colours::white : juce::Colour (0xff16161c);
    }

    /// The accent tinted into a surface: the playing scene, the loaded preset.
    [[nodiscard]] juce::Colour accentTint() const noexcept { return accent().withAlpha (isLight() ? 0.12f : 0.14f); }

    /// A selected surface that is not a call to action: the text colour, faintly.
    [[nodiscard]] juce::Colour selectedFill() const noexcept { return text().withAlpha (isLight() ? 0.08f : 0.10f); }

    /// Under the pointer, or pressed.
    [[nodiscard]] juce::Colour hoverFill() const noexcept { return text().withAlpha (0.05f); }
    [[nodiscard]] juce::Colour pressedFill() const noexcept { return text().withAlpha (0.12f); }

    /// WCAG relative luminance.
    [[nodiscard]] static float luminance (juce::Colour colour) noexcept
    {
        const auto linear = [] (float channel) {
            return channel <= 0.03928f ? channel / 12.92f : std::pow ((channel + 0.055f) / 1.055f, 2.4f);
        };

        return 0.2126f * linear (colour.getFloatRed()) + 0.7152f * linear (colour.getFloatGreen())
               + 0.0722f * linear (colour.getFloatBlue());
    }

    // ── Type (logical pixels) ────────────────────────────────────────────────
    static constexpr float textTitle = 17.0f;    // the preset's name
    static constexpr float textHeading = 15.0f;  // an effect's name, sheet titles
    static constexpr float textBody = 14.0f;     // list rows, settings
    static constexpr float textLabel = 13.0f;    // buttons, control names and values
    static constexpr float textCaption = 12.0f;  // second lines
    static constexpr float textOverline = 11.0f; // section headings, in capitals

    // ── Icons (logical pixels) ───────────────────────────────────────────────
    // The icons are 24-unit strokes 2 units wide, so 18 px draws them 1.5 px thick.
    static constexpr float iconSize = 18.0f;  // a toolbar button
    static constexpr float iconSmall = 15.0f; // beside a label
    static constexpr float iconNav = 20.0f;   // the navigation rail and tab bar

    // ── Measurements (logical pixels, before the UI scale) ────────────────────
    static constexpr int gap = 8;
    static constexpr int radius = 8;        // cards, panels
    static constexpr int controlRadius = 6; // buttons, fields, chips
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
