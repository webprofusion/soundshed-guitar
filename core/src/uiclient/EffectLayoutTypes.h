#pragma once

/**
 * EffectLayoutTypes.h - Custom effect layouts, as the native UI reads them.
 *
 * A layout is a picture of an effect designed in Sepia's layout designer: a canvas of
 * a fixed size with backgrounds, rectangles, text and the effect's controls placed on it in
 * design pixels from its top-left corner. The engine sends the whole library in
 * `layoutLibraryLoaded` and the list of images in `layoutImagesLoaded`; these are the fields the
 * web UI's renderer reads (core/ui/ts/layoutTypes.ts), parsed leniently by EffectLayouts.cpp.
 * Designing layouts stays in Sepia; nothing here writes one.
 */

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
struct LayoutPoint
{
    double x = 0.0;
    double y = 0.0;
};

struct LayoutSize
{
    double width = 0.0;
    double height = 0.0;
};

/// One image a layout draws on: the engine resolves the id to a file in a layout's folder.
struct LayoutImageRef
{
    std::string imageId;
    std::string fileName;
    std::string url;
    std::string version; // the file's write time, which changes when the image does
    int width = 0;       // 0 when the engine could not read the size
    int height = 0;
};

struct LayoutBackground
{
    int layerIndex = 0;
    std::string type;  // "color", "gradient" or "image"
    std::string value; // a CSS colour or gradient, or an image id
    double opacity = 1.0;
    std::string size = "cover";  // "cover", "contain", "stretch", "tile" or "custom"
    std::optional<double> scale; // "custom": the image's width as a share of the canvas's
    double offsetX = 0.0;
    double offsetY = 0.0;
};

/// A knob drawn from pictures: a face that turns, or a filmstrip of pre-rendered frames.
struct LayoutKnobImage
{
    std::string mode; // "rotate" or "filmstrip"
    std::string imageId;
    int frames = 0;  // a filmstrip's frame count; 0 when the layout's value is not a usable one
    int columns = 1; // frames run left to right across this many columns, then down
    double minAngle = -135.0;
    double maxAngle = 135.0;
    std::string baseImageId;  // under the face, not turned
    std::string glareImageId; // over the face, not turned
    bool clip = true;         // the face clipped to a circle inset 4 px
};

struct LayoutControlStyle
{
    std::string labelPosition = "top"; // "top", "bottom", or anything else for no label
    bool hideLabel = false;
    std::string labelColor;
    bool showValue = true;
    std::string valueColor;
    std::string knobStyle = "default"; // "default", "pedal", "amp", "minimal" or "custom"
    std::string knobImageId;           // the face of a "custom" knob
    std::optional<double> labelFontSize;
    std::optional<double> valueFontSize;
    std::optional<double> knobDiameter;
    std::optional<double> sliderLength;
    std::optional<double> toggleWidth;
    std::optional<double> toggleHeight;
    std::optional<LayoutKnobImage> knobImage;
    std::string toggleOnImageId;
    std::string toggleOffImageId;
    std::string sliderTrackImageId;
    std::string sliderThumbImageId;
};

/// A control placed on the canvas: a parameter's knob, switch or slider, or a model/IR slot.
struct LayoutControl
{
    std::string paramKey;
    std::string bindingType;   // "resource" for a slot; empty or "parameter" otherwise
    std::string type = "knob"; // "knob", "toggle", "slider" or "dropdown" (drawn as a knob)
    std::string resourceType;
    int resourceIndex = -1;
    LayoutPoint position;
    std::optional<LayoutSize> size;
    LayoutControlStyle style;
    std::string labelOverride;
};

struct LayoutTextLabel
{
    std::string id;
    std::string text;
    LayoutPoint position;
    double fontSize = 12.0;
    bool bold = false;
    std::string color;
    std::string textAlign = "left";
};

/// A rectangle over the backgrounds: a panel, an LED that follows the effect's bypass, or a
/// footswitch area that toggles it.
struct LayoutOverlay
{
    std::string id;
    LayoutPoint position;
    LayoutSize size;
    std::string visibilityMode = "always"; // "always", "enabled" or "bypassed"
    bool toggleBypassOnClick = false;
    std::string backgroundColor = "#000000";
    double backgroundOpacity = 0.25;
    std::string borderColor = "#ffffff";
    double borderWidth = 1.0;
    double borderRadius = 0.0;
    std::string imageId;
    std::string imageFit = "contain"; // "contain", "cover" or "stretch"
};

/// How a layout fits the space it is given (core/ui/ts/layoutScaling.ts).
struct LayoutScaling
{
    std::string mode = "fit-width"; // "fit-width", "fit" or "fixed"
    std::optional<double> minScale;
    std::optional<double> maxScale;
    std::string align = "center"; // "center" or "start"
};

struct EffectLayout
{
    std::string effectType;
    std::string layoutId;
    std::string blendId;
    std::string name;
    int version = 1;
    LayoutSize dimensions{400.0, 280.0};
    std::vector<LayoutBackground> backgrounds; // bottom layer first
    std::vector<LayoutControl> controls;
    std::vector<LayoutOverlay> overlays;
    std::vector<LayoutTextLabel> textLabels;
    /// The layout is only a backdrop: the effect's standard controls are drawn on it, moved by
    /// defaultControlsOffset and scaled by defaultControlsScale.
    bool useDefaultControls = false;
    LayoutPoint defaultControlsOffset;
    LayoutPoint defaultControlsScale{1.0, 1.0};
    std::string containerTheme; // "light", "dark", "classic", or empty for the app's theme
    std::string controlLabelColor;
    std::string controlValueColor;
    std::optional<double> controlLabelFontSize;
    std::optional<double> controlValueFontSize;
    double controlLabelMaxWidth = 0.0; // 0: the 80 px every label is capped at
    std::optional<LayoutScaling> scaling;
};

struct LayoutLibraryEntry
{
    EffectLayout layout;
    std::string layoutId;
    bool isDefault = false;
    bool isFactory = false;
};

struct LayoutLibrary
{
    /// By lookup key: an effect type, or "<effectType>::<blendId>" for one blend's layouts.
    std::map<std::string, std::vector<LayoutLibraryEntry>> byEffectType;
    std::map<std::string, std::string> defaults; // lookup key -> layout id
};

/// Topic::EffectLayouts.
struct LayoutState
{
    bool loaded = false; // a layoutLibraryLoaded has arrived
    LayoutLibrary library;
    std::map<std::string, LayoutImageRef> images; // by image id, once layoutImagesLoaded has come
    bool imagesRequested = false;
    bool imagesLoaded = false;
    /// Counts the library and image lists received, so a view can tell its pictures are stale.
    int revision = 0;
};
} // namespace guitarfx::uiclient
