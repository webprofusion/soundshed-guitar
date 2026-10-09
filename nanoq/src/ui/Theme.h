#pragma once

// The look: the colours the web UI's CSS defines (the same generated tokens Soundshed Guitar
// Nano uses, so a theme looks the same in every product), as Elements colours, plus the
// measurements the views share.

#include "nativeui/theme/ThemeTokens.generated.h"

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

    [[nodiscard]] el::color Accent() const noexcept { return Argb(T().accent); }
    [[nodiscard]] el::color Background() const noexcept { return Argb(T().bgPrimary); }
    [[nodiscard]] el::color Bar() const noexcept { return Argb(T().bgSecondary); }
    [[nodiscard]] el::color Panel() const noexcept { return Argb(T().bgPanel); }
    [[nodiscard]] el::color Card() const noexcept { return Argb(T().bgCard); }
    [[nodiscard]] el::color Elevated() const noexcept { return Argb(T().bgElevated); }
    [[nodiscard]] el::color Input() const noexcept { return Argb(T().bgInput); }
    [[nodiscard]] el::color Text() const noexcept { return Argb(T().textPrimary); }
    [[nodiscard]] el::color TextSecondary() const noexcept { return Argb(T().textSecondary); }
    [[nodiscard]] el::color TextMuted() const noexcept { return Argb(T().textMuted); }
    [[nodiscard]] el::color Border() const noexcept { return Argb(T().borderLight); }
    [[nodiscard]] el::color BorderStrong() const noexcept { return Argb(T().borderMedium); }
    [[nodiscard]] el::color Success() const noexcept { return Argb(T().success); }
    [[nodiscard]] el::color Error() const noexcept { return Argb(T().error); }
    [[nodiscard]] el::color Warning() const noexcept { return Argb(T().warning); }

    /// The accent as a fill. On the dark themes the token is a bright orange that glares as a
    /// block of colour, so fills take it a fifth of the way into the panel colour.
    [[nodiscard]] el::color AccentFill() const noexcept;

    /// Text and icons on AccentFill(): white where it passes WCAG AA (4.5:1), else near-black.
    [[nodiscard]] el::color OnAccent() const noexcept;

    /// The accent tinted into a surface: the playing scene, the loaded preset.
    [[nodiscard]] el::color AccentTint() const noexcept;

private:
    using Tokens = soundshed::nano::theme::ThemeTokens;
    [[nodiscard]] const Tokens& T() const noexcept
    {
        return *mTokens;
    }

    std::string mName = "dark";
    const Tokens* mTokens = &soundshed::nano::theme::kDark;
};

/// Text weights Nano draws in: Inter's static cuts.
enum class Weight
{
    Regular,
    Medium,
    SemiBold
};

[[nodiscard]] el::font_descr Font(Weight weight = Weight::Regular);

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
} // namespace metrics
} // namespace nanoq::ui
