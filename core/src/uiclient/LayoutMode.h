#pragma once

/**
 * LayoutMode.h - Which shell the native UI uses for the space it has.
 *
 * The decisions of the web UI's compact mode (core/ui/ts/compactMode.ts), which already
 * worked out this layout problem: height is the binding constraint, so both dimensions (in
 * logical pixels, after the UI scale) decide; a landscape screen gets a navigation rail down
 * the left and gives its height to the content, a portrait one keeps stacked bars and gives
 * its width to the content.
 */

namespace guitarfx::uiclient
{
enum class ShellLayout
{
    Rail,  // landscape: navigation rail on the left
    Stack  // portrait: tab bar along the bottom
};

struct LayoutMetrics
{
    ShellLayout shell = ShellLayout::Rail;

    /// Knobs in a grid (landscape) or a list of horizontal sliders (portrait, where knobs
    /// waste width).
    bool sliderList = false;

    /// Short enough that the effect view shows either its visual or its controls, not both.
    bool splitEffectView = false;

    /// The smallest comfortable touch target, in logical pixels.
    int touchTarget = 44;
};

[[nodiscard]] inline LayoutMetrics DecideLayout(double width, double height, bool touch)
{
    LayoutMetrics metrics;
    metrics.shell = width > height ? ShellLayout::Rail : ShellLayout::Stack;
    metrics.sliderList = metrics.shell == ShellLayout::Stack;

    // Below this the rail layout cannot fit a visual beside a useful knob grid; the portrait
    // stack stacks them and scrolls instead.
    metrics.splitEffectView = metrics.shell == ShellLayout::Rail && width < 700.0;
    metrics.touchTarget = touch ? 44 : 32;
    return metrics;
}
} // namespace guitarfx::uiclient
