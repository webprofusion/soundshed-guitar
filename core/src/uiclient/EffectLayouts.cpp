/**
 * EffectLayouts.cpp - Reading layouts (see EffectLayouts.h). Choosing which one an effect is
 * drawn with is in EffectLayoutPreferences.cpp, the drawing arithmetic in EffectLayoutDraw.cpp.
 */

#include "uiclient/EffectLayouts.h"

#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace guitarfx::uiclient
{
namespace
{
std::string StringOf(const nlohmann::json& object, const char* key)
{
    if (!object.is_object())
    {
        return {};
    }

    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

/// A number from a layout, which may hold anything: a number, or a string that reads as one
/// (the web UI's Number(value)).
std::optional<double> NumberOf(const nlohmann::json& object, const char* key)
{
    if (!object.is_object())
    {
        return std::nullopt;
    }

    const auto it = object.find(key);

    if (it == object.end())
    {
        return std::nullopt;
    }

    double value = 0.0;

    if (it->is_number())
    {
        value = it->get<double>();
    }
    else if (it->is_string())
    {
        const auto text = it->get<std::string>();
        char* end = nullptr;
        value = std::strtod(text.c_str(), &end);

        if (text.empty() || end == text.c_str() || *end != '\0')
        {
            return std::nullopt;
        }
    }
    else
    {
        return std::nullopt;
    }

    return guitarfx::IsFinite(value) ? std::optional<double>(value) : std::nullopt;
}

double NumberOr(const nlohmann::json& object, const char* key, double fallback)
{
    return NumberOf(object, key).value_or(fallback);
}

/// A field only a real JSON number sets (the web UI checks `typeof value === "number"`).
std::optional<double> StrictNumberOf(const nlohmann::json& object, const char* key)
{
    if (!object.is_object())
    {
        return std::nullopt;
    }

    const auto it = object.find(key);

    if (it == object.end() || !it->is_number())
    {
        return std::nullopt;
    }

    const double value = it->get<double>();
    return guitarfx::IsFinite(value) ? std::optional<double>(value) : std::nullopt;
}

bool BoolIs(const nlohmann::json& object, const char* key, bool value)
{
    if (!object.is_object())
    {
        return false;
    }

    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>() == value;
}

const nlohmann::json& ObjectAt(const nlohmann::json& object, const char* key)
{
    static const nlohmann::json empty = nlohmann::json::object();

    if (!object.is_object())
    {
        return empty;
    }

    const auto it = object.find(key);
    return it != object.end() && it->is_object() ? *it : empty;
}

LayoutPoint PointOf(const nlohmann::json& object, const char* key, LayoutPoint fallback = {})
{
    const auto& point = ObjectAt(object, key);
    return {NumberOr(point, "x", fallback.x), NumberOr(point, "y", fallback.y)};
}

LayoutKnobImage ParseKnobImage(const nlohmann::json& raw)
{
    LayoutKnobImage image;
    image.mode = StringOf(raw, "mode");
    image.imageId = StringOf(raw, "imageId");
    image.frames = FilmstripFrameCount(StrictNumberOf(raw, "frames").value_or(0.0));
    const double columns = StrictNumberOf(raw, "columns").value_or(1.0);
    image.columns =
        columns >= 1.0 && std::floor(columns) == columns && columns <= 1024.0 ? static_cast<int>(columns) : 1;
    image.minAngle = StrictNumberOf(raw, "minAngle").value_or(-135.0);
    image.maxAngle = StrictNumberOf(raw, "maxAngle").value_or(135.0);
    image.baseImageId = StringOf(raw, "baseImageId");
    image.glareImageId = StringOf(raw, "glareImageId");
    image.clip = !BoolIs(raw, "clip", false);
    return image;
}

/// A strip when `raw` names an image and a usable frame count (usableStrip), else nothing.
std::optional<LayoutImageStrip> ParseStrip(const nlohmann::json& raw)
{
    LayoutImageStrip strip;
    strip.imageId = StringOf(raw, "imageId");
    strip.frames = FilmstripFrameCount(StrictNumberOf(raw, "frames").value_or(0.0));

    if (strip.imageId.empty() || strip.frames == 0)
    {
        return std::nullopt;
    }

    const double columns = StrictNumberOf(raw, "columns").value_or(1.0);
    strip.columns =
        columns >= 1.0 && std::floor(columns) == columns && columns <= 1024.0 ? static_cast<int>(columns) : 1;

    if (const auto aspect = StrictNumberOf(raw, "aspect"); aspect && *aspect > 0.0)
    {
        strip.aspect = aspect;
    }

    return strip;
}

/// The light, each field held in range (usableControlShadow); nothing when `raw` is not an object.
std::optional<LayoutControlShadow> ParseControlShadow(const nlohmann::json& raw)
{
    if (!raw.is_object())
    {
        return std::nullopt;
    }

    LayoutControlShadow light;
    const auto field = [&raw](const char* key, double lo, double hi, double fallback) {
        const auto value = StrictNumberOf(raw, key);
        return value ? std::clamp(*value, lo, hi) : fallback;
    };
    light.angle = std::fmod(std::fmod(field("angle", -3600.0, 3600.0, 315.0), 360.0) + 360.0, 360.0);
    light.elevation = field("elevation", 15.0, 80.0, 45.0);
    light.softness = field("softness", 0.0, 1.0, 0.4);
    light.opacity = field("opacity", 0.0, 1.0, 0.5);
    return light;
}

LayoutControlStyle ParseControlStyle(const nlohmann::json& raw)
{
    LayoutControlStyle style;
    const auto labelPosition = StringOf(raw, "labelPosition");
    style.labelPosition = labelPosition.empty() ? std::string("top") : labelPosition;
    style.hideLabel = BoolIs(raw, "hideLabel", true);
    style.labelColor = StringOf(raw, "labelColor");
    style.showValue = !BoolIs(raw, "showValue", false);
    style.valueColor = StringOf(raw, "valueColor");

    // A class-name suffix in the web UI, so anything but a plain word draws the default knob.
    const auto knobStyle = StringOf(raw, "knobStyle");
    const bool plainWord = !knobStyle.empty() && std::all_of(knobStyle.begin(), knobStyle.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
    });
    style.knobStyle = plainWord ? knobStyle : std::string("default");
    style.knobImageId = StringOf(raw, "knobImageId");
    style.labelFontSize = NumberOf(raw, "labelFontSize");
    style.valueFontSize = NumberOf(raw, "valueFontSize");
    style.knobDiameter = StrictNumberOf(raw, "knobDiameter");
    style.sliderLength = StrictNumberOf(raw, "sliderLength");
    style.toggleWidth = StrictNumberOf(raw, "toggleWidth");
    style.toggleHeight = StrictNumberOf(raw, "toggleHeight");

    if (const auto& knobImage = ObjectAt(raw, "knobImage"); !knobImage.empty())
    {
        style.knobImage = ParseKnobImage(knobImage);
    }

    const auto& toggleImage = ObjectAt(raw, "toggleImage");
    style.toggleOnImageId = StringOf(toggleImage, "onImageId");
    style.toggleOffImageId = StringOf(toggleImage, "offImageId");
    const auto& sliderImage = ObjectAt(raw, "sliderImage");
    style.sliderTrackImageId = StringOf(sliderImage, "trackImageId");
    style.sliderThumbImageId = StringOf(sliderImage, "thumbImageId");
    style.toggleStrip = ParseStrip(ObjectAt(toggleImage, "strip"));
    style.sliderStrip = ParseStrip(ObjectAt(sliderImage, "strip"));
    style.castShadow = !BoolIs(raw, "castShadow", false);

    if (const auto aspect = StrictNumberOf(toggleImage, "aspect"); aspect && *aspect > 0.0)
    {
        style.toggleAspect = aspect;
    }

    return style;
}

std::optional<LayoutControl> ParseControl(const nlohmann::json& raw)
{
    if (!raw.is_object())
    {
        return std::nullopt;
    }

    LayoutControl control;
    control.paramKey = StringOf(raw, "paramKey");

    if (control.paramKey.empty())
    {
        return std::nullopt;
    }

    control.bindingType = StringOf(raw, "bindingType");
    const auto type = StringOf(raw, "type");
    control.type = type.empty() ? std::string("knob") : type;
    control.resourceType = StringOf(raw, "resourceType");
    control.resourceIndex = static_cast<int>(StrictNumberOf(raw, "resourceIndex").value_or(-1.0));
    control.position = PointOf(raw, "position");

    if (const auto& size = ObjectAt(raw, "size"); !size.empty())
    {
        control.size = LayoutSize{NumberOr(size, "width", 0.0), NumberOr(size, "height", 0.0)};
    }

    control.style = ParseControlStyle(ObjectAt(raw, "style"));
    control.labelOverride = StringOf(raw, "labelOverride");
    return control;
}

LayoutOverlay ParseOverlay(const nlohmann::json& raw)
{
    LayoutOverlay overlay;
    overlay.id = StringOf(raw, "id");
    overlay.position = PointOf(raw, "position");
    const auto& size = ObjectAt(raw, "size");
    overlay.size = {NumberOr(size, "width", 0.0), NumberOr(size, "height", 0.0)};

    const auto& style = ObjectAt(raw, "style");
    const auto mode = StringOf(style, "visibilityMode");
    overlay.visibilityMode = mode.empty() ? std::string("always") : mode;
    overlay.toggleBypassOnClick = BoolIs(style, "toggleBypassOnClick", true);

    if (const auto colour = StringOf(style, "backgroundColor"); !colour.empty())
    {
        overlay.backgroundColor = colour;
    }

    if (const auto opacity = StrictNumberOf(style, "backgroundOpacity"))
    {
        overlay.backgroundOpacity = std::clamp(*opacity, 0.0, 1.0);
    }

    if (const auto colour = StringOf(style, "borderColor"); !colour.empty())
    {
        overlay.borderColor = colour;
    }

    overlay.borderWidth = std::max(0.0, NumberOr(style, "borderWidth", 1.0));
    overlay.borderRadius = std::max(0.0, NumberOr(style, "borderRadius", 0.0));
    overlay.imageId = StringOf(style, "imageId");
    const auto fit = StringOf(style, "imageFit");
    overlay.imageFit = fit == "cover" || fit == "stretch" ? fit : std::string("contain");
    return overlay;
}

LayoutTextLabel ParseTextLabel(const nlohmann::json& raw)
{
    LayoutTextLabel label;
    label.id = StringOf(raw, "id");
    label.text = StringOf(raw, "text");
    label.position = PointOf(raw, "position");
    label.fontSize = NumberOr(raw, "fontSize", 12.0);
    const auto weight = StringOf(raw, "fontWeight");
    label.bold = weight == "bold" || weight == "bolder" || std::atoi(weight.c_str()) >= 600;
    label.color = StringOf(raw, "color");
    const auto align = StringOf(raw, "textAlign");
    label.textAlign = align.empty() ? std::string("left") : align;
    return label;
}

std::optional<LayoutScaling> ParseScaling(const nlohmann::json& raw)
{
    if (!raw.is_object())
    {
        return std::nullopt;
    }

    LayoutScaling scaling;
    const auto mode = StringOf(raw, "mode");
    scaling.mode = mode == "fit" || mode == "fixed" ? mode : std::string("fit-width");
    const auto positive = [&raw](const char* key) -> std::optional<double> {
        const auto value = StrictNumberOf(raw, key);
        return value && *value > 0.0 ? value : std::nullopt;
    };
    scaling.minScale = positive("minScale");
    scaling.maxScale = positive("maxScale");
    scaling.align = StringOf(raw, "align") == "start" ? std::string("start") : std::string("center");
    return scaling;
}
} // namespace

// ── Reading ─────────────────────────────────────────────────────────────────────────────

EffectLayout ParseEffectLayout(const nlohmann::json& raw)
{
    EffectLayout layout;

    if (!raw.is_object())
    {
        return layout;
    }

    layout.effectType = StringOf(raw, "effectType");
    layout.layoutId = StringOf(raw, "layoutId");
    layout.blendId = StringOf(raw, "blendId");
    layout.name = StringOf(raw, "name");
    layout.version = static_cast<int>(NumberOr(raw, "version", 1.0));
    layout.thumbnailDataUrl = StringOf(raw, "thumbnailDataUrl");

    const auto& dimensions = ObjectAt(raw, "dimensions");
    const double width = NumberOr(dimensions, "width", 400.0);
    const double height = NumberOr(dimensions, "height", 280.0);
    layout.dimensions = {width > 0.0 ? width : 400.0, height > 0.0 ? height : 280.0};

    if (const auto it = raw.find("backgrounds"); it != raw.end() && it->is_array())
    {
        for (const auto& bg : *it)
        {
            if (!bg.is_object())
            {
                continue;
            }

            LayoutBackground background;
            background.layerIndex = static_cast<int>(NumberOr(bg, "layerIndex", 0.0));
            background.type = StringOf(bg, "type");
            background.value = StringOf(bg, "value");
            background.opacity = NumberOr(bg, "opacity", 1.0);
            const auto size = StringOf(bg, "size");
            background.size = size.empty() ? std::string("cover") : size;
            background.scale = NumberOf(bg, "scale");
            background.offsetX = NumberOr(bg, "offsetX", 0.0);
            background.offsetY = NumberOr(bg, "offsetY", 0.0);
            layout.backgrounds.push_back(std::move(background));
        }

        // Bottom layer first; layers of the same index keep their order, as the web UI's sort.
        std::stable_sort(
            layout.backgrounds.begin(), layout.backgrounds.end(),
            [](const LayoutBackground& a, const LayoutBackground& b) { return a.layerIndex < b.layerIndex; });
    }

    if (const auto it = raw.find("controls"); it != raw.end() && it->is_array())
    {
        for (const auto& entry : *it)
        {
            if (auto control = ParseControl(entry))
            {
                layout.controls.push_back(std::move(*control));
            }
        }
    }

    if (const auto it = raw.find("overlays"); it != raw.end() && it->is_array())
    {
        for (const auto& entry : *it)
        {
            if (entry.is_object())
            {
                layout.overlays.push_back(ParseOverlay(entry));
            }
        }
    }

    if (const auto it = raw.find("textLabels"); it != raw.end() && it->is_array())
    {
        for (const auto& entry : *it)
        {
            if (entry.is_object())
            {
                layout.textLabels.push_back(ParseTextLabel(entry));
            }
        }
    }

    layout.useDefaultControls = BoolIs(raw, "useDefaultControls", true);
    layout.defaultControlsOffset = PointOf(raw, "defaultControlsOffset");
    layout.defaultControlsScale = PointOf(raw, "defaultControlsScale", {1.0, 1.0});

    // usableControlsArea: a box of positive, finite size, rounded, else none.
    if (const auto& area = ObjectAt(raw, "defaultControlsArea"); area.is_object())
    {
        const double width = NumberOr(area, "width", 0.0);
        const double height = NumberOr(area, "height", 0.0);

        if (std::isfinite(width) && std::isfinite(height) && width >= 1.0 && height >= 1.0)
        {
            layout.defaultControlsArea = LayoutSize{std::round(width), std::round(height)};
        }
    }

    layout.containerTheme = StringOf(raw, "containerTheme");
    layout.controlLabelColor = StringOf(raw, "controlLabelColor");
    layout.controlValueColor = StringOf(raw, "controlValueColor");
    layout.controlLabelFontSize = NumberOf(raw, "controlLabelFontSize");
    layout.controlValueFontSize = NumberOf(raw, "controlValueFontSize");
    layout.controlLabelMaxWidth = NumberOr(raw, "controlLabelMaxWidth", 0.0);

    if (const auto& knobImage = ObjectAt(raw, "knobImage"); !knobImage.empty())
    {
        layout.knobImage = ParseKnobImage(knobImage);
    }

    if (const auto& switchImage = ObjectAt(raw, "switchImage"); !switchImage.empty())
    {
        LayoutSwitchImage image;
        image.onImageId = StringOf(switchImage, "onImageId");
        image.offImageId = StringOf(switchImage, "offImageId");
        image.strip = ParseStrip(ObjectAt(switchImage, "strip"));
        image.aspect = StrictNumberOf(switchImage, "aspect");
        image.width = StrictNumberOf(switchImage, "width");
        image.height = StrictNumberOf(switchImage, "height");

        if (image.strip || (!image.onImageId.empty() && !image.offImageId.empty()))
        {
            layout.switchImage = std::move(image);
        }
    }

    if (const auto it = raw.find("controlShadow"); it != raw.end())
    {
        layout.controlShadow = ParseControlShadow(*it);
    }

    ApplyLayoutControlImages(layout);

    if (const auto it = raw.find("scaling"); it != raw.end())
    {
        layout.scaling = ParseScaling(*it);
    }

    return layout;
}

LayoutLibrary ParseLayoutLibrary(const nlohmann::json& raw)
{
    LayoutLibrary library;
    const auto& byType = ObjectAt(raw, "byEffectType");

    for (auto it = byType.begin(); it != byType.end(); ++it)
    {
        if (!it.value().is_array())
        {
            continue;
        }

        std::vector<LayoutLibraryEntry> entries;

        for (const auto& rawEntry : it.value())
        {
            if (!rawEntry.is_object())
            {
                continue;
            }

            LayoutLibraryEntry entry;
            entry.layout = ParseEffectLayout(ObjectAt(rawEntry, "layout"));
            entry.layoutId = StringOf(rawEntry, "layoutId");

            if (entry.layoutId.empty())
            {
                entry.layoutId = entry.layout.layoutId;
            }

            entry.isDefault = BoolIs(rawEntry, "isDefault", true);
            entry.isFactory = BoolIs(rawEntry, "isFactory", true);

            if (!entry.layoutId.empty())
            {
                entries.push_back(std::move(entry));
            }
        }

        library.byEffectType[it.key()] = std::move(entries);
    }

    const auto& defaults = ObjectAt(raw, "defaults");

    for (auto it = defaults.begin(); it != defaults.end(); ++it)
    {
        if (it.value().is_string())
        {
            library.defaults[it.key()] = it.value().get<std::string>();
        }
    }

    return library;
}

LayoutImageRef ParseLayoutImageRef(const nlohmann::json& raw)
{
    LayoutImageRef image;
    image.imageId = StringOf(raw, "imageId");
    image.fileName = StringOf(raw, "fileName");
    image.url = StringOf(raw, "url");
    image.version = StringOf(raw, "version");
    image.width = static_cast<int>(NumberOr(raw, "width", 0.0));
    image.height = static_cast<int>(NumberOr(raw, "height", 0.0));
    return image;
}

std::vector<std::string> LayoutImageIds(const EffectLayout& layout)
{
    std::vector<std::string> ids;
    const auto add = [&ids](const std::string& id) {
        if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end())
        {
            ids.push_back(id);
        }
    };

    for (const auto& bg : layout.backgrounds)
    {
        if (bg.type == "image")
        {
            add(bg.value);
        }
    }

    for (const auto& control : layout.controls)
    {
        const auto& style = control.style;
        add(style.knobImageId);

        if (style.knobImage)
        {
            add(style.knobImage->imageId);
            add(style.knobImage->baseImageId);
            add(style.knobImage->glareImageId);
        }

        add(style.toggleOnImageId);
        add(style.toggleOffImageId);
        add(style.sliderTrackImageId);
        add(style.sliderThumbImageId);
        add(style.toggleStrip ? style.toggleStrip->imageId : std::string());
        add(style.sliderStrip ? style.sliderStrip->imageId : std::string());
    }

    for (const auto& overlay : layout.overlays)
    {
        add(overlay.imageId);
    }

    return ids;
}
} // namespace guitarfx::uiclient
