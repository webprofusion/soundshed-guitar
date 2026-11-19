#include "ui/widgets/Theme.h"

#include "ui/widgets/Graphics.h"

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

// The families Nano Q ships cuts of (core/nativeui/fonts, from tools/gen-nano-fonts.mjs), each
// with Inter after it for any character its subset lacks. font_descr keeps only a view of its
// family list, so these are the lists it views.
constexpr std::string_view kInter = "Inter";
constexpr std::string_view kThemeFaces[][2] = {
    {"Oswald", "Oswald, Inter"},
    {"Archivo", "Archivo, Inter"},
};

std::string_view FamilyList(std::string_view family)
{
    for (const auto& face : kThemeFaces)
    {
        if (face[0] == family)
        {
            return face[1];
        }
    }

    return kInter;
}

// The text face of the theme last Set (one theme for every editor: it is an account setting).
// The families are views into the generated table, which lives as long as the program.
struct TextFace
{
    std::string_view family = kInter;
    int weight = 400;
    std::string_view titleFamily = kInter;
    int titleWeight = 600;
    float titleScale = 1.0f;
};

TextFace& CurrentFace()
{
    static TextFace face;
    return face;
}
} // namespace

void Theme::Set(const std::string& name)
{
    mInfo = &guitarfx::uiclient::theme::FindTheme(name);
    const auto& controls = Controls();
    auto& face = CurrentFace();
    face.family = controls.body.family;
    face.weight = controls.body.weight;
    face.titleFamily = controls.title.family;
    face.titleWeight = controls.title.weight;
    face.titleScale = controls.titleScale > 0.0f ? controls.titleScale : 1.0f;
}

std::shared_ptr<cycfi::artist::image> Theme::Picture(std::string_view relativePath) const
{
    return mImages != nullptr && !relativePath.empty() ? mImages->Find(std::string(relativePath)) : nullptr;
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

el::font_descr FaceFor(std::string_view family, int weight)
{
    const auto list = FamilyList(family);

    if (list == kInter)
    {
        // Inter's cuts: 400, 500 and 600, the heavier weights drawn in the heaviest.
        const auto inter = el::font_descr{kInter};
        return weight >= 600 ? inter.semi_bold() : weight >= 500 ? inter.medium() : inter;
    }

    const auto step = static_cast<cycfi::artist::font_constants::weight_enum>(std::clamp(weight, 100, 900) / 10);
    return el::font_descr{list}.weight(step);
}

el::font_descr Font(Weight weight)
{
    const auto& face = CurrentFace();

    switch (weight)
    {
    case Weight::Medium:
        return FaceFor(face.family, face.weight + 100);
    case Weight::SemiBold:
        return FaceFor(face.family, face.weight + 200);
    case Weight::Title:
        return FaceFor(face.titleFamily, face.titleWeight);
    default:
        return FaceFor(face.family, face.weight);
    }
}

float TitleScale()
{
    return CurrentFace().titleScale;
}

el::font_descr TitleFont(float size)
{
    return Font(Weight::Title).size(size * TitleScale());
}

el::font_descr MonoFont()
{
#if defined(_WIN32)
    return el::font_descr{"Consolas"};
#elif defined(__APPLE__)
    return el::font_descr{"Menlo"};
#else
    return el::font_descr{"DejaVu Sans Mono"};
#endif
}
} // namespace nanoq::ui
