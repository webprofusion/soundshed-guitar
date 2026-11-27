#include "ui/input/SystemPreferences.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #include <objc/message.h>
    #include <objc/runtime.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
extern "C" bool UIAccessibilityIsReduceMotionEnabled(void);
#endif

namespace nanoq::ui
{
namespace el = cycfi::elements;

namespace
{
double Now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/// `read()` again once a second at most.
template <typename T, typename Read> T Cached(double& checked, T& value, Read read)
{
    const double now = Now();

    if (now - checked > 1.0)
    {
        checked = now;
        value = read();
    }

    return value;
}

bool ReadReducedMotion()
{
#if defined(_WIN32)
    BOOL animate = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0) && !animate;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return UIAccessibilityIsReduceMotionEnabled();
#elif defined(__APPLE__)
    // [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceMotion], through the
    // runtime: this file is C++.
    const auto cls = reinterpret_cast<id>(objc_getClass("NSWorkspace"));

    if (cls == nullptr)
    {
        return false;
    }

    const auto workspace = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(cls, sel_registerName("sharedWorkspace"));
    const SEL reduce = sel_registerName("accessibilityDisplayShouldReduceMotion");

    if (workspace == nullptr || !class_respondsToSelector(object_getClass(workspace), reduce))
    {
        return false;
    }

    return reinterpret_cast<BOOL (*)(id, SEL)>(objc_msgSend)(workspace, reduce);
#else
    return false;
#endif
}

std::optional<el::color> ReadHighContrast()
{
#if defined(_WIN32)
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);

    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) || (hc.dwFlags & HCF_HIGHCONTRASTON) == 0)
    {
        return std::nullopt;
    }

    const COLORREF c = GetSysColor(COLOR_HIGHLIGHT);
    return el::rgba(GetRValue(c), GetGValue(c), GetBValue(c), 255);
#else
    return std::nullopt;
#endif
}

/// WCAG's relative luminance of an sRGB colour.
double Luminance(el::color c)
{
    const auto channel = [](float v) {
        const double s = std::clamp(static_cast<double>(v), 0.0, 1.0);
        return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c.red) + 0.7152 * channel(c.green) + 0.0722 * channel(c.blue);
}

double Contrast(el::color a, el::color b)
{
    const double la = Luminance(a), lb = Luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}
} // namespace

bool ReducedMotion()
{
    static double checked = -10.0;
    static bool reduced = false;
    return Cached(checked, reduced, ReadReducedMotion);
}

std::optional<el::color> HighContrastHighlight()
{
    static double checked = -10.0;
    static std::optional<el::color> highlight;
    return Cached(checked, highlight, ReadHighContrast);
}

el::color FocusRingColour(el::color accent, el::color background)
{
    if (const auto highlight = HighContrastHighlight())
    {
        return *highlight;
    }

    const auto opaque = accent.opacity(1.0f);

    if (Contrast(opaque, background) >= 3.0)
    {
        return opaque;
    }

    const auto white = el::rgba(255, 255, 255, 255), black = el::rgba(0, 0, 0, 255);
    return Contrast(white, background) >= Contrast(black, background) ? white : black;
}
} // namespace nanoq::ui
