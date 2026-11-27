#pragma once

// What the user has asked of every app in the system's accessibility settings, as the web UI
// reads them through media queries (prefers-reduced-motion, forced-colors): less motion, and
// high contrast. Each is read at most once a second, not on every draw.

#include <elements.hpp>

#include <optional>

namespace nanoq::ui
{
/// Animations are turned off: Windows' "Show animations", macOS's and iOS's Reduce motion. Off
/// elsewhere (GTK and Android are not read yet).
[[nodiscard]] bool ReducedMotion();

/// Windows' high contrast (a contrast theme) is on: its highlight colour, which the focus ring
/// takes. None when it is off, and everywhere else.
[[nodiscard]] std::optional<cycfi::elements::color> HighContrastHighlight();

/// The focus ring's colour: `accent`, unless it shows less than 3:1 against `background`
/// (WCAG's non-text contrast), when it is white or black, whichever shows more; the system's
/// highlight under high contrast.
[[nodiscard]] cycfi::elements::color FocusRingColour(cycfi::elements::color accent, cycfi::elements::color background);
} // namespace nanoq::ui
