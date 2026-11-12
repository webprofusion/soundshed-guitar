/**
 * EffectLayoutDraw.cpp - The arithmetic of drawing a custom layout (see EffectLayouts.h): its
 * scale, its colours and gradients, its controls' sizes, labels and image knobs.
 */

#include "uiclient/EffectLayouts.h"

#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace guitarfx::uiclient
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxScaleLimit = 8.0;

std::string Lower(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return text;
}

std::string Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{}
                                      : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/// What the web UI's safeCssValue lets through: short, of the characters a colour or gradient
/// needs, and no url() or image() (a layout is imported from a file).
bool IsSafeCss(const std::string& value)
{
    if (value.empty() || value.size() > 256)
    {
        return false;
    }

    for (const char c : value)
    {
        const auto u = static_cast<unsigned char>(c);

        if (!(std::isalnum(u) || c == '_' || std::isspace(u) || c == '#' || c == '%' || c == '.' || c == ',' ||
              c == '(' || c == ')' || c == '\'' || c == '"' || c == '+' || c == '-' || c == '/'))
        {
            return false;
        }
    }

    const auto lower = Lower(value);

    for (const char* banned : {"url", "image", "image-set", "expression"})
    {
        for (auto at = lower.find(banned); at != std::string::npos; at = lower.find(banned, at + 1))
        {
            auto after = at + std::char_traits<char>::length(banned);

            while (after < lower.size() && std::isspace(static_cast<unsigned char>(lower[after])))
            {
                ++after;
            }

            const bool wordStart =
                at == 0 || !(std::isalnum(static_cast<unsigned char>(lower[at - 1])) || lower[at - 1] == '_');

            if (wordStart && after < lower.size() && lower[after] == '(')
            {
                return false;
            }
        }
    }

    return true;
}

int HexDigit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }

    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/// Splits the inside of a CSS function at its top-level commas.
std::vector<std::string> SplitArguments(const std::string& inside)
{
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;

    for (const char c : inside)
    {
        if (c == '(')
        {
            ++depth;
        }
        else if (c == ')')
        {
            --depth;
        }

        if (c == ',' && depth == 0)
        {
            parts.push_back(Trim(current));
            current.clear();
        }
        else
        {
            current += c;
        }
    }

    parts.push_back(Trim(current));
    return parts;
}

/// The arguments of `name(...)` when `css` is that call, else nothing.
std::optional<std::vector<std::string>> FunctionArguments(const std::string& css, const std::string& name)
{
    const auto lower = Lower(css);

    if (lower.rfind(name + "(", 0) != 0 || lower.back() != ')')
    {
        return std::nullopt;
    }

    return SplitArguments(css.substr(name.size() + 1, css.size() - name.size() - 2));
}

/// A number with an optional trailing `%` (a share of `percentOf`).
std::optional<double> ParseComponent(const std::string& text, double percentOf)
{
    const auto trimmed = Trim(text);

    if (trimmed.empty())
    {
        return std::nullopt;
    }

    char* end = nullptr;
    const double value = std::strtod(trimmed.c_str(), &end);

    if (end == trimmed.c_str() || !guitarfx::IsFinite(value))
    {
        return std::nullopt;
    }

    const std::string rest = Trim(end);

    if (rest == "%")
    {
        return value / 100.0 * percentOf;
    }

    return rest.empty() ? std::optional<double>(value) : std::nullopt;
}

/// An angle as CSS writes one, in degrees: "180deg", "0.5turn", "3.14rad", "200grad".
std::optional<double> ParseAngle(const std::string& text)
{
    const auto lower = Lower(Trim(text));
    char* end = nullptr;
    const double value = std::strtod(lower.c_str(), &end);

    if (end == lower.c_str() || !guitarfx::IsFinite(value))
    {
        return std::nullopt;
    }

    const std::string unit = end;

    if (unit == "deg")
    {
        return value;
    }

    if (unit == "turn")
    {
        return value * 360.0;
    }

    if (unit == "rad")
    {
        return value * 180.0 / kPi;
    }

    if (unit == "grad")
    {
        return value * 0.9;
    }

    return value == 0.0 && unit.empty() ? std::optional<double>(0.0) : std::nullopt;
}

/// "to right", "to bottom left", ... as an angle. A corner is taken as the diagonal of a square.
std::optional<double> ParseSideAngle(const std::string& text)
{
    const auto lower = Lower(Trim(text));

    if (lower.rfind("to ", 0) != 0)
    {
        return std::nullopt;
    }

    const bool top = lower.find("top") != std::string::npos;
    const bool bottom = lower.find("bottom") != std::string::npos;
    const bool left = lower.find("left") != std::string::npos;
    const bool right = lower.find("right") != std::string::npos;
    const double x = right ? 1.0 : left ? -1.0 : 0.0;
    const double y = top ? 1.0 : bottom ? -1.0 : 0.0;

    if (x == 0.0 && y == 0.0)
    {
        return std::nullopt;
    }

    return std::atan2(x, y) * 180.0 / kPi;
}
} // namespace

// ── Sizing ──────────────────────────────────────────────────────────────────────────────

LayoutScaleResult ComputeLayoutScale(double designWidth, double designHeight, double availableWidth,
                                     double availableHeight, const std::optional<LayoutScaling>& scaling)
{
    const LayoutScaling spec = scaling.value_or(LayoutScaling{});
    const std::string mode = spec.mode == "fit" || spec.mode == "fixed" ? spec.mode : std::string("fit-width");
    const double maxScale =
        std::min(kMaxScaleLimit,
                 spec.maxScale && guitarfx::IsFinite(*spec.maxScale) && *spec.maxScale > 0.0 ? *spec.maxScale : 1.0);
    const double minScale = std::min(
        maxScale, spec.minScale && guitarfx::IsFinite(*spec.minScale) && *spec.minScale > 0.0 ? *spec.minScale : 0.0);

    double raw = mode == "fixed" ? 1.0 : (designWidth > 0.0 ? availableWidth / designWidth : 1.0);

    if (mode == "fit" && availableHeight > 0.0 && designHeight > 0.0)
    {
        raw = std::min(raw, availableHeight / designHeight);
    }

    LayoutScaleResult result;
    result.scale = mode == "fixed" ? 1.0 : std::max(minScale, std::min(maxScale, raw));
    const double drawnWidth = designWidth * result.scale;
    result.overflows = drawnWidth > availableWidth + 0.5;
    const bool narrower = drawnWidth < availableWidth - 0.5;
    // A layout at its own size is centred; a shrunk one fills the width exactly, so it is not.
    result.centred = !result.overflows && spec.align != "start" && (result.scale == 1.0 || narrower);
    return result;
}

// ── Colours ─────────────────────────────────────────────────────────────────────────────

std::optional<LayoutColour> ParseLayoutColour(const std::string& rawCss)
{
    const auto css = Trim(rawCss);

    if (!IsSafeCss(css))
    {
        return std::nullopt;
    }

    const auto lower = Lower(css);

    if (lower[0] == '#')
    {
        const auto hex = lower.substr(1);

        if (!std::all_of(hex.begin(), hex.end(), [](char c) { return HexDigit(c) >= 0; }))
        {
            return std::nullopt;
        }

        const auto digit = [&hex](std::size_t i) { return HexDigit(hex[i]); };
        LayoutColour colour;

        if (hex.size() == 3 || hex.size() == 4)
        {
            colour.r = static_cast<std::uint8_t>(digit(0) * 17);
            colour.g = static_cast<std::uint8_t>(digit(1) * 17);
            colour.b = static_cast<std::uint8_t>(digit(2) * 17);
            colour.a = hex.size() == 4 ? digit(3) * 17 / 255.0 : 1.0;
            return colour;
        }

        if (hex.size() == 6 || hex.size() == 8)
        {
            colour.r = static_cast<std::uint8_t>(digit(0) * 16 + digit(1));
            colour.g = static_cast<std::uint8_t>(digit(2) * 16 + digit(3));
            colour.b = static_cast<std::uint8_t>(digit(4) * 16 + digit(5));
            colour.a = hex.size() == 8 ? (digit(6) * 16 + digit(7)) / 255.0 : 1.0;
            return colour;
        }

        return std::nullopt;
    }

    auto args = FunctionArguments(lower, "rgba");

    if (!args)
    {
        args = FunctionArguments(lower, "rgb");
    }

    if (args)
    {
        // The space-separated form too: rgb(255 0 0 / 50%).
        if (args->size() == 1)
        {
            std::vector<std::string> parts;
            std::string spaced = (*args)[0];
            std::replace(spaced.begin(), spaced.end(), '/', ' ');
            std::string token;

            for (std::size_t i = 0; i <= spaced.size(); ++i)
            {
                if (i == spaced.size() || std::isspace(static_cast<unsigned char>(spaced[i])))
                {
                    if (!token.empty())
                    {
                        parts.push_back(token);
                    }

                    token.clear();
                }
                else
                {
                    token += spaced[i];
                }
            }

            args = parts;
        }

        if (args->size() != 3 && args->size() != 4)
        {
            return std::nullopt;
        }

        LayoutColour colour;
        std::array<double, 3> channels{};

        for (std::size_t i = 0; i < 3; ++i)
        {
            const auto value = ParseComponent((*args)[i], 255.0);

            if (!value)
            {
                return std::nullopt;
            }

            channels[i] = std::clamp(*value, 0.0, 255.0);
        }

        colour.r = static_cast<std::uint8_t>(std::lround(channels[0]));
        colour.g = static_cast<std::uint8_t>(std::lround(channels[1]));
        colour.b = static_cast<std::uint8_t>(std::lround(channels[2]));

        if (args->size() == 4)
        {
            const auto alpha = ParseComponent((*args)[3], 1.0);

            if (!alpha)
            {
                return std::nullopt;
            }

            colour.a = std::clamp(*alpha, 0.0, 1.0);
        }

        return colour;
    }

    static const std::pair<const char*, LayoutColour> kNamed[] = {
        {"transparent", {0, 0, 0, 0.0}},     {"black", {0, 0, 0, 1.0}},          {"white", {255, 255, 255, 1.0}},
        {"red", {255, 0, 0, 1.0}},           {"green", {0, 128, 0, 1.0}},        {"blue", {0, 0, 255, 1.0}},
        {"yellow", {255, 255, 0, 1.0}},      {"orange", {255, 165, 0, 1.0}},     {"gray", {128, 128, 128, 1.0}},
        {"grey", {128, 128, 128, 1.0}},      {"silver", {192, 192, 192, 1.0}},   {"gold", {255, 215, 0, 1.0}},
        {"purple", {128, 0, 128, 1.0}},      {"navy", {0, 0, 128, 1.0}},         {"maroon", {128, 0, 0, 1.0}},
        {"teal", {0, 128, 128, 1.0}},        {"cyan", {0, 255, 255, 1.0}},       {"magenta", {255, 0, 255, 1.0}},
        {"lime", {0, 255, 0, 1.0}},          {"olive", {128, 128, 0, 1.0}},      {"brown", {165, 42, 42, 1.0}},
        {"pink", {255, 192, 203, 1.0}},      {"cream", {255, 253, 208, 1.0}},    {"ivory", {255, 255, 240, 1.0}},
        {"beige", {245, 245, 220, 1.0}},     {"darkgray", {169, 169, 169, 1.0}}, {"darkgrey", {169, 169, 169, 1.0}},
        {"lightgray", {211, 211, 211, 1.0}}, {"lightgrey", {211, 211, 211, 1.0}}};

    for (const auto& [name, colour] : kNamed)
    {
        if (lower == name)
        {
            return colour;
        }
    }

    return std::nullopt;
}

LayoutPaint ParseLayoutPaint(const std::string& rawCss)
{
    LayoutPaint paint;
    const auto css = Trim(rawCss);

    if (!IsSafeCss(css))
    {
        return paint;
    }

    if (const auto colour = ParseLayoutColour(css))
    {
        paint.kind = LayoutPaint::Kind::Solid;
        paint.colour = *colour;
        return paint;
    }

    const auto args = FunctionArguments(css, "linear-gradient");

    if (!args || args->empty())
    {
        return paint;
    }

    std::size_t first = 0;

    if (const auto angle = ParseAngle((*args)[0]))
    {
        paint.angleDegrees = *angle;
        first = 1;
    }
    else if (const auto side = ParseSideAngle((*args)[0]))
    {
        paint.angleDegrees = *side;
        first = 1;
    }

    // Each stop is a colour and an optional position; a missing one is spread evenly between
    // its neighbours, the first at 0 and the last at 1, as CSS places them.
    std::vector<std::pair<LayoutColour, std::optional<double>>> stops;

    for (std::size_t i = first; i < args->size(); ++i)
    {
        const auto& stop = (*args)[i];
        const auto space = stop.find_last_of(" \t");
        std::optional<double> offset;
        std::string colourText = stop;

        if (space != std::string::npos && stop.back() != ')')
        {
            const auto tail = stop.substr(space + 1);

            if (tail.find('%') != std::string::npos)
            {
                offset = ParseComponent(tail, 1.0);
                colourText = Trim(stop.substr(0, space));
            }
        }

        const auto colour = ParseLayoutColour(colourText);

        if (!colour)
        {
            return LayoutPaint{};
        }

        stops.emplace_back(*colour, offset);
    }

    if (stops.empty())
    {
        return LayoutPaint{};
    }

    if (!stops.front().second)
    {
        stops.front().second = 0.0;
    }

    if (!stops.back().second)
    {
        stops.back().second = 1.0;
    }

    for (std::size_t i = 1; i < stops.size(); ++i)
    {
        if (stops[i].second)
        {
            continue;
        }

        std::size_t next = i;

        while (!stops[next].second)
        {
            ++next;
        }

        const double from = *stops[i - 1].second;
        const double to = *stops[next].second;

        for (std::size_t j = i; j < next; ++j)
        {
            stops[j].second = from + (to - from) * static_cast<double>(j - i + 1) / static_cast<double>(next - i + 1);
        }
    }

    paint.kind = LayoutPaint::Kind::Linear;
    double last = 0.0;

    for (const auto& [colour, offset] : stops)
    {
        // A stop placed before an earlier one sits on it, as CSS clamps them.
        last = std::max(last, std::clamp(*offset, 0.0, 1.0));
        paint.stops.push_back({colour, last});
    }

    return paint;
}

void LinearGradientLine(double angleDegrees, double width, double height, double& x0, double& y0, double& x1,
                        double& y1)
{
    // CSS: the line runs through the centre at the angle (0 up, clockwise), long enough that the
    // corners' perpendiculars meet its ends.
    const double radians = angleDegrees * kPi / 180.0;
    const double dx = std::sin(radians);
    const double dy = -std::cos(radians);
    const double half = (std::abs(width * dx) + std::abs(height * dy)) * 0.5;
    x0 = width * 0.5 - dx * half;
    y0 = height * 0.5 - dy * half;
    x1 = width * 0.5 + dx * half;
    y1 = height * 0.5 + dy * half;
}

// ── Controls ────────────────────────────────────────────────────────────────────────────

bool IsLayoutResourceControl(const LayoutControl& control)
{
    return control.bindingType == "resource" || control.paramKey.rfind("__resource__:", 0) == 0;
}

LayoutControlKind ClassifyLayoutControl(const LayoutControl& control, const EffectParamInfo* param)
{
    if (IsLayoutResourceControl(control))
    {
        return LayoutControlKind::Resource;
    }

    if (control.type == "toggle" || (param != nullptr && param->unit == "toggle"))
    {
        return LayoutControlKind::Toggle;
    }

    return control.type == "slider" ? LayoutControlKind::Slider : LayoutControlKind::Knob;
}

std::optional<LayoutResourceSlot> LayoutControlResourceSlot(const LayoutControl& control)
{
    // The web UI places a slot by its key alone: "__resource__:primary:<index>" for the effect's
    // own model or IR slots, "__resource__:<exposed id>:<index>" for a composite's.
    constexpr const char* kPrefix = "__resource__:";

    if (control.paramKey.rfind(kPrefix, 0) != 0)
    {
        return std::nullopt;
    }

    const auto rest = control.paramKey.substr(std::char_traits<char>::length(kPrefix));
    const auto colon = rest.rfind(':');
    LayoutResourceSlot slot;
    slot.resourceType = colon == std::string::npos ? rest : rest.substr(0, colon);

    if (colon != std::string::npos)
    {
        const auto indexText = rest.substr(colon + 1);
        char* end = nullptr;
        const long index = std::strtol(indexText.c_str(), &end, 10);

        if (indexText.empty() || *end != '\0' || index < 0 || index > 64)
        {
            return std::nullopt;
        }

        slot.resourceIndex = static_cast<int>(index);
    }

    return slot.resourceType.empty() ? std::nullopt : std::optional<LayoutResourceSlot>(slot);
}

std::string LayoutControlLabel(const LayoutControl& control, const EffectParamInfo* param)
{
    if (!control.labelOverride.empty())
    {
        return control.labelOverride;
    }

    if (param != nullptr && !param->name.empty())
    {
        return param->name;
    }

    return control.paramKey;
}

namespace
{
double TextSize(const std::optional<double>& own, const std::optional<double>& layoutDefault, double fallback)
{
    return std::clamp(own.value_or(layoutDefault.value_or(fallback)), 6.0, 96.0);
}

std::optional<LayoutColour> PickColour(const std::string& own, const std::string& layoutDefault)
{
    if (auto colour = ParseLayoutColour(own))
    {
        return colour;
    }

    return ParseLayoutColour(layoutDefault);
}
} // namespace

double LayoutLabelFontSize(const EffectLayout& layout, const LayoutControl& control)
{
    return TextSize(control.style.labelFontSize, layout.controlLabelFontSize, 10.0);
}

double LayoutValueFontSize(const EffectLayout& layout, const LayoutControl& control)
{
    return TextSize(control.style.valueFontSize, layout.controlValueFontSize, 9.0);
}

std::optional<LayoutColour> LayoutLabelColour(const EffectLayout& layout, const LayoutControl& control)
{
    return PickColour(control.style.labelColor, layout.controlLabelColor);
}

std::optional<LayoutColour> LayoutValueColour(const EffectLayout& layout, const LayoutControl& control)
{
    return PickColour(control.style.valueColor, layout.controlValueColor);
}

double LayoutLabelMaxWidth(const EffectLayout& layout)
{
    return layout.controlLabelMaxWidth > 0.0 ? std::round(layout.controlLabelMaxWidth) : 80.0;
}

std::optional<double> LayoutSizeValue(const std::optional<double>& value, double lo, double hi)
{
    if (!value || !guitarfx::IsFinite(*value))
    {
        return std::nullopt;
    }

    return std::clamp(std::round(*value), lo, hi);
}

double LayoutKnobDiameter(const LayoutControlStyle& style)
{
    return LayoutSizeValue(style.knobDiameter, 16.0, 512.0).value_or(kLayoutDefaultKnobDiameter);
}

double LayoutSliderLength(const LayoutControlStyle& style)
{
    return LayoutSizeValue(style.sliderLength, 24.0, 2048.0).value_or(80.0);
}

LayoutSize LayoutToggleSize(const LayoutControlStyle& style)
{
    return {LayoutSizeValue(style.toggleWidth, 8.0, 512.0).value_or(36.0),
            LayoutSizeValue(style.toggleHeight, 8.0, 512.0).value_or(20.0)};
}

const LayoutKnobImage* UsableKnobImage(const LayoutControlStyle& style)
{
    if (!style.knobImage || style.knobImage->imageId.empty())
    {
        return nullptr;
    }

    const auto& image = *style.knobImage;

    if (image.mode == "filmstrip")
    {
        return image.frames > 0 ? &image : nullptr;
    }

    return image.mode == "rotate" ? &image : nullptr;
}

int FilmstripFrameCount(double frames)
{
    return guitarfx::IsFinite(frames) && std::floor(frames) == frames && frames >= 2.0 && frames <= 1024.0
               ? static_cast<int>(frames)
               : 0;
}

int FilmstripFrame(double position, int frames)
{
    const double clamped = guitarfx::IsFinite(position) ? std::clamp(position, 0.0, 1.0) : 0.0;
    return static_cast<int>(std::lround(clamped * (frames - 1)));
}

FilmstripCell FilmstripCellOf(int frame, int frames, int columns)
{
    FilmstripCell cell;
    cell.columns = std::max(1, std::min(frames, columns > 0 ? columns : 1));
    cell.rows = (frames + cell.columns - 1) / cell.columns;
    cell.column = frame % cell.columns;
    cell.row = frame / cell.columns;
    return cell;
}

double KnobFaceAngle(double position, double minAngle, double maxAngle)
{
    const double clamped = guitarfx::IsFinite(position) ? std::clamp(position, 0.0, 1.0) : 0.0;
    const double from = guitarfx::IsFinite(minAngle) ? minAngle : -135.0;
    const double to = guitarfx::IsFinite(maxAngle) ? maxAngle : 135.0;
    return from + clamped * (to - from);
}

bool LayoutOverlayVisible(const LayoutOverlay& overlay, bool bypassed)
{
    return overlay.visibilityMode == "always" || (overlay.visibilityMode == "enabled" && !bypassed) ||
           (overlay.visibilityMode == "bypassed" && bypassed);
}
} // namespace guitarfx::uiclient
