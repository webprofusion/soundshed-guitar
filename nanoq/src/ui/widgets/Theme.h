#pragma once

// The look: the colours the web UI's CSS defines (the same generated tokens Soundshed Guitar
// Nano uses, so a theme looks the same in every product), as Elements colours, plus the
// measurements the views share.

#include "uiclient/ThemeTokens.generated.h"

#include <elements.hpp>

#include <string>

namespace nanoq::ui
{
namespace el = cycfi::elements;

/// The active theme's colours. "light", "dark" or "classic"; anything else is dark, as the
/// web UI normalises it.
class Theme
{
  public:
    void Set(const std::string& name);

    [[nodiscard]] const std::string& Name() const noexcept
    {
        return mName;
    }

    [[nodiscard]] bool IsLight() const noexcept
    {
        return mName == "light";
    }

    [[nodiscard]] static el::color Argb(std::uint32_t value) noexcept
    {
        return el::rgba(static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 8),
                        static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 24));
    }

    [[nodiscard]] el::color Accent() const noexcept
    {
        return Argb(T().accent);
    }

    [[nodiscard]] el::color Background() const noexcept
    {
        return Argb(T().bgPrimary);
    }

    [[nodiscard]] el::color Bar() const noexcept
    {
        return Argb(T().bgSecondary);
    }

    [[nodiscard]] el::color Panel() const noexcept
    {
        return Argb(T().bgPanel);
    }

    [[nodiscard]] el::color Card() const noexcept
    {
        return Argb(T().bgCard);
    }

    [[nodiscard]] el::color Elevated() const noexcept
    {
        return Argb(T().bgElevated);
    }

    [[nodiscard]] el::color Input() const noexcept
    {
        return Argb(T().bgInput);
    }

    [[nodiscard]] el::color Text() const noexcept
    {
        return Argb(T().textPrimary);
    }

    [[nodiscard]] el::color TextSecondary() const noexcept
    {
        return Argb(T().textSecondary);
    }

    [[nodiscard]] el::color TextMuted() const noexcept
    {
        return Argb(T().textMuted);
    }

    [[nodiscard]] el::color Border() const noexcept
    {
        return Argb(T().borderLight);
    }

    [[nodiscard]] el::color BorderStrong() const noexcept
    {
        return Argb(T().borderMedium);
    }

    [[nodiscard]] el::color Success() const noexcept
    {
        return Argb(T().success);
    }

    [[nodiscard]] el::color Error() const noexcept
    {
        return Argb(T().error);
    }

    [[nodiscard]] el::color Warning() const noexcept
    {
        return Argb(T().warning);
    }

    [[nodiscard]] el::color Tertiary() const noexcept
    {
        return Argb(T().bgTertiary);
    }

    [[nodiscard]] el::color Footer() const noexcept
    {
        return Argb(T().bgFooter);
    }

    [[nodiscard]] el::color Header() const noexcept
    {
        return Argb(T().bgHeader);
    }

    [[nodiscard]] el::color TextOnDark() const noexcept
    {
        return Argb(T().textOnDark);
    }

    [[nodiscard]] el::color ControlBorder() const noexcept
    {
        return Argb(T().controlBorder);
    }

    [[nodiscard]] el::color KnobRingStart() const noexcept
    {
        return Argb(T().knobRingStart);
    }

    [[nodiscard]] el::color KnobRingEnd() const noexcept
    {
        return Argb(T().knobRingEnd);
    }

    // The response plots (the web UI's --eq-curve-* properties).
    [[nodiscard]] el::color PlotGrid() const noexcept
    {
        return Argb(T().eqCurveGrid);
    }

    [[nodiscard]] el::color PlotCurve() const noexcept
    {
        return Argb(T().eqCurveResponse);
    }

    [[nodiscard]] el::color PlotHandleStroke() const noexcept
    {
        return Argb(T().eqCurveHandleStroke);
    }

    [[nodiscard]] el::color PlotSpectrumTop() const noexcept
    {
        return Argb(T().eqCurveSpectrumFillTop);
    }

    [[nodiscard]] el::color PlotSpectrumBottom() const noexcept
    {
        return Argb(T().eqCurveSpectrumFillBottom);
    }

    [[nodiscard]] el::color PlotSpectrumLine() const noexcept
    {
        return Argb(T().eqCurveSpectrumLine);
    }

    [[nodiscard]] el::color KnobRingTrack() const noexcept
    {
        return Argb(T().knobRingTrack);
    }

    [[nodiscard]] el::color ToggleBg() const noexcept
    {
        return Argb(T().toggleBg);
    }

    // A dialog's colours (the web UI's --modal-*): its body, the raised header and footer bands,
    // the outline and the rules, and its three weights of text.
    [[nodiscard]] el::color ModalBg() const noexcept
    {
        return Argb(T().modalBg);
    }

    [[nodiscard]] el::color ModalRaised() const noexcept
    {
        return Argb(T().modalBgRaised);
    }

    [[nodiscard]] el::color ModalBorder() const noexcept
    {
        return Argb(T().modalBorder);
    }

    [[nodiscard]] el::color ModalRule() const noexcept
    {
        return Argb(T().modalBorderMedium);
    }

    [[nodiscard]] el::color ModalText() const noexcept
    {
        return Argb(T().modalText);
    }

    [[nodiscard]] el::color ModalTextSecondary() const noexcept
    {
        return Argb(T().modalTextSecondary);
    }

    [[nodiscard]] el::color ModalTextMuted() const noexcept
    {
        return Argb(T().modalTextMuted);
    }

    /// The accent as a fill. On the dark themes the token is a bright orange that glares as a
    /// block of colour, so fills take it a fifth of the way into the panel colour.
    [[nodiscard]] el::color AccentFill() const noexcept;

    /// Text and icons on AccentFill(): white where it passes WCAG AA (4.5:1), else near-black.
    [[nodiscard]] el::color OnAccent() const noexcept;

    /// The accent tinted into a surface: the playing scene, the loaded preset.
    [[nodiscard]] el::color AccentTint() const noexcept;

    /// Text and icons on the chrome (bars, pills, chips): the dark themes' near-white, the light
    /// theme's ink. TextOnDark() is near-white in every theme, so on a light surface it vanishes.
    [[nodiscard]] el::color Ink() const noexcept
    {
        return IsLight() ? Text() : TextOnDark();
    }

    /// `dark` (a colour measured from the dark web UI) on the dark themes, and on the light one
    /// `light`, the token the web UI's light theme draws that part with.
    [[nodiscard]] el::color Pick(el::color dark, el::color light) const noexcept
    {
        return IsLight() ? light : dark;
    }

  private:
    using Tokens = guitarfx::uiclient::theme::ThemeTokens;

    [[nodiscard]] const Tokens& T() const noexcept
    {
        return *mTokens;
    }

    std::string mName = "dark";
    const Tokens* mTokens = &guitarfx::uiclient::theme::kDark;
};

/// Text weights Nano draws in: Inter's static cuts.
enum class Weight
{
    Regular,
    Medium,
    SemiBold
};

[[nodiscard]] el::font_descr Font(Weight weight = Weight::Regular);

/// A fixed-pitch face, for code (a preset's JSON).
[[nodiscard]] el::font_descr MonoFont();

/// Logical sizes. Everything is laid out in these pixels; the view scale (the zoom) turns them
/// into screen pixels.
namespace metrics
{
inline constexpr float kRadius = 8.0f;
inline constexpr float kGap = 8.0f;
inline constexpr float kPad = 12.0f;
inline constexpr float kBarHeight = 48.0f;
inline constexpr float kRowHeight = 40.0f;
inline constexpr float kTextSize = 14.0f;
inline constexpr float kSmallText = 12.0f;

// The web UI's compact rail layout: a 60 px rail, a 40 px preset bar, a 32 px footer.
inline constexpr float kRailWidth = 60.0f;
inline constexpr float kTopBarHeight = 40.0f;
inline constexpr float kFooterHeight = 32.0f;

// The full layout, above the compact breakpoint (core/ui/ts/compactMode.ts): the PLAY / TONES tab
// row, the control bar with the preset selector in it, and a 44 px footer.
inline constexpr float kTabBarHeight = 36.0f;
inline constexpr float kHeaderHeight = 134.0f;
inline constexpr float kWideFooterHeight = 44.0f;
/// The web UI goes compact at or below either (COMPACT_MAX_WIDTH, COMPACT_MAX_HEIGHT).
inline constexpr float kCompactMaxWidth = 980.0f;
inline constexpr float kCompactMaxHeight = 620.0f;
/// The smallest window the compact layout still fits.
inline constexpr float kMinWidth = 640.0f;
inline constexpr float kMinHeight = 400.0f;
} // namespace metrics
} // namespace nanoq::ui
