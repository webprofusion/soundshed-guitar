#include "ui/Theme.h"

#include <algorithm>
#include <cmath>

namespace nanoq::ui
{
namespace
{
// WCAG relative luminance of a colour, 0 (black) to 1 (white).
float Luminance(el::color c)
{
    auto channel = [](float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * channel(c.red) + 0.7152f * channel(c.green) + 0.0722f * channel(c.blue);
}

el::color Mix(el::color a, el::color b, float t)
{
    return el::color(a.red + (b.red - a.red) * t, a.green + (b.green - a.green) * t, a.blue + (b.blue - a.blue) * t,
                     a.alpha + (b.alpha - a.alpha) * t);
}
} // namespace

void Theme::Set(const std::string& name)
{
    using namespace soundshed::nano::theme;
    mName = name == "light" || name == "classic" ? name : std::string("dark");
    mTokens = mName == "light" ? &kLight : (mName == "classic" ? &kClassic : &kDark);
}

el::color Theme::AccentFill() const noexcept
{
    return IsLight() ? Accent() : Mix(Accent(), Panel(), 0.2f);
}

el::color Theme::OnAccent() const noexcept
{
    const float whiteContrast = 1.05f / (Luminance(AccentFill()) + 0.05f);
    return whiteContrast >= 4.5f ? el::rgba(255, 255, 255, 255) : el::rgba(0x16, 0x16, 0x1c, 255);
}

el::color Theme::AccentTint() const noexcept
{
    return Accent().opacity(IsLight() ? 0.12f : 0.14f);
}

el::font_descr Font(Weight weight)
{
    switch (weight)
    {
    case Weight::Medium:
        return el::font_descr{"Inter"}.medium();
    case Weight::SemiBold:
        return el::font_descr{"Inter"}.semi_bold();
    default:
        return el::font_descr{"Inter"};
    }
}
} // namespace nanoq::ui
