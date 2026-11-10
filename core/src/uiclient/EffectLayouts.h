#pragma once

/**
 * EffectLayouts.h - Which custom layout an effect is drawn with, and the arithmetic of drawing
 * one: its scale, its colours, its controls' sizes and its image knobs.
 *
 * Each function follows the web UI's own (named alongside), so a layout looks and resolves the
 * same in Sepia and Soundshed Nano Q: layoutPreferences.ts decides between the
 * standard controls and a layout, layoutScaling.ts sizes it, layoutRenderer.ts and
 * layoutControlImages.ts draw it, layoutKnobImages.ts turns its image knobs.
 */

#include "uiclient/ClientState.h"
#include "uiclient/EffectLayoutTypes.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
// ── Reading ─────────────────────────────────────────────────────────────────────────────

/// A layout as the library or a layout.json holds it; every field optional, and a field of the
/// wrong type reads as its default, since layouts are imported from files.
[[nodiscard]] EffectLayout ParseEffectLayout(const nlohmann::json& layout);
[[nodiscard]] LayoutLibrary ParseLayoutLibrary(const nlohmann::json& library);
[[nodiscard]] LayoutImageRef ParseLayoutImageRef(const nlohmann::json& image);

/// Every image id `layout` draws on, each once, in the order they appear (layoutImageIds).
[[nodiscard]] std::vector<std::string> LayoutImageIds(const EffectLayout& layout);

// ── Choosing a layout (layoutPreferences.ts) ────────────────────────────────────────────

/// The app setting holding the rules, and the layout id that stands for the standard controls.
inline constexpr const char* kLayoutPreferencesSetting = "ui.effectLayoutPreferences";
inline constexpr const char* kStandardLayoutId = "__standard__";

struct LayoutPreferenceRule
{
    std::string id;
    std::string lookupKey;
    std::string scope; // "effectType", "keyword" or "preset"
    std::string layoutId;
    std::string keyword; // lower case
    std::string presetId;
    std::string presetName;
    std::string createdAt;
};

/// The rules in the app settings, malformed entries left out.
[[nodiscard]] std::vector<LayoutPreferenceRule> ParseLayoutPreferenceRules(const nlohmann::json& appSettings);

/// A node's lookup keys, most specific first: "<type>::<blendId>" for a blend, then the type.
[[nodiscard]] std::vector<std::string> LayoutLookupKeys(const std::string& effectType, const std::string& blendId);

struct LayoutResolutionContext
{
    std::string effectType;
    std::string blendId;
    std::string matchText; // the node's names, for keyword rules (LayoutMatchText)
    std::string presetId;
};

struct LayoutSelection
{
    std::optional<std::string> layoutId; // empty when nothing is configured
    std::string source;                  // "preset", "keyword", "effectType", "libraryDefault", "none"
    std::optional<LayoutPreferenceRule> rule;
};

/// Preset rule, then keyword rule (longest keyword wins), then effect-type rule, for each
/// lookup key in turn; with no rule at all, the library's default when it exists.
[[nodiscard]] LayoutSelection ResolveLayoutSelection(const LayoutLibrary* library,
                                                     const std::vector<LayoutPreferenceRule>& rules,
                                                     const LayoutResolutionContext& context);

/// The layouts a node can pick, de-duplicated across its lookup keys.
[[nodiscard]] std::vector<const LayoutLibraryEntry*> AvailableLayoutEntries(const LayoutLibrary& library,
                                                                            const std::string& effectType,
                                                                            const std::string& blendId);

[[nodiscard]] const EffectLayout* FindLayoutById(const LayoutLibrary& library, const std::string& layoutId,
                                                 const std::string& effectType, const std::string& blendId);

/// The lower-cased text keyword rules match: the node's automatic name, its own title, its
/// models' or IRs' names and its effect's name (nodeLabels.ts buildNodeLayoutMatchText).
[[nodiscard]] std::string LayoutMatchText(const ClientState& state, const GraphNode& node);

/// The context a node is resolved in: its type, blend, names and the active preset.
[[nodiscard]] LayoutResolutionContext LayoutContextForNode(const ClientState& state, const GraphNode& node);

/// The layout a node is drawn with, or null for the standard controls.
[[nodiscard]] const EffectLayout* ResolveLayoutForNode(const ClientState& state, const GraphNode& node);

/// The rules after choosing `layoutId` (or kStandardLayoutId) for the node, as the app setting
/// holds them. The standard controls are recorded at the scope that picked the layout (a
/// preset or keyword rule loses to nothing broader); a layout is recorded for the effect
/// type. A rule for the same key, scope and keyword or preset is replaced (setLayoutPreference).
[[nodiscard]] nlohmann::json RulesWithLayoutChoice(const nlohmann::json& appSettings, const ClientState& state,
                                                   const GraphNode& node, const std::string& layoutId,
                                                   const std::string& nowIso);

// ── Sizing (layoutScaling.ts) ───────────────────────────────────────────────────────────

struct LayoutScaleResult
{
    double scale = 1.0;
    bool centred = false;   // narrower than the space, and placed in its middle
    bool overflows = false; // wider than the space at this scale
};

/// The uniform scale for a layout. Without a scaling block it is drawn at its own size when it
/// fits and shrunk to the width when it does not; it never grows. `availableHeight` <= 0 is
/// unbounded.
[[nodiscard]] LayoutScaleResult ComputeLayoutScale(double designWidth, double designHeight, double availableWidth,
                                                   double availableHeight, const std::optional<LayoutScaling>& scaling);

// ── Colours (layoutRenderer.ts safeCssValue, colorWithAlpha) ────────────────────────────

struct LayoutColour
{
    std::uint8_t r = 0, g = 0, b = 0;
    double a = 1.0;
};

/// A CSS colour a layout may hold: #rgb, #rgba, #rrggbb, #rrggbbaa, rgb()/rgba(), a basic named
/// colour or "transparent"; nullopt for anything else.
[[nodiscard]] std::optional<LayoutColour> ParseLayoutColour(const std::string& css);

struct LayoutGradientStop
{
    LayoutColour colour;
    double offset = 0.0; // 0..1 along the gradient line
};

/// What a colour or gradient background paints. A gradient other than a linear one, or a value
/// that does not parse, paints nothing.
struct LayoutPaint
{
    enum class Kind
    {
        None,
        Solid,
        Linear
    };

    Kind kind = Kind::None;
    LayoutColour colour;
    double angleDegrees = 180.0; // CSS: 0 points up, 90 right, 180 down
    std::vector<LayoutGradientStop> stops;
};

[[nodiscard]] LayoutPaint ParseLayoutPaint(const std::string& css);

/// The ends of a CSS linear gradient's line across a w x h box from its centre, as offsets.
void LinearGradientLine(double angleDegrees, double width, double height, double& x0, double& y0, double& x1,
                        double& y1);

// ── Controls (layoutRenderer.ts, layoutControlImages.ts) ────────────────────────────────

enum class LayoutControlKind
{
    Knob,
    Toggle,
    Slider,
    Resource
};

[[nodiscard]] bool IsLayoutResourceControl(const LayoutControl& control);
[[nodiscard]] LayoutControlKind ClassifyLayoutControl(const LayoutControl& control, const EffectParamInfo* param);

/// A slot control's resource type and index: from its fields, else from a
/// "__resource__:<type>:<index>" key; nullopt when it names neither.
struct LayoutResourceSlot
{
    std::string resourceType;
    int resourceIndex = 0;
};

[[nodiscard]] std::optional<LayoutResourceSlot> LayoutControlResourceSlot(const LayoutControl& control);

/// The label: its override, else the parameter's name, else its key.
[[nodiscard]] std::string LayoutControlLabel(const LayoutControl& control, const EffectParamInfo* param);

/// Text sizes: the control's own, else the layout's, else 10 (label) or 9 (value), in 6..96.
[[nodiscard]] double LayoutLabelFontSize(const EffectLayout& layout, const LayoutControl& control);
[[nodiscard]] double LayoutValueFontSize(const EffectLayout& layout, const LayoutControl& control);
/// Colours: the control's own, else the layout's; nullopt for the theme's.
[[nodiscard]] std::optional<LayoutColour> LayoutLabelColour(const EffectLayout& layout, const LayoutControl& control);
[[nodiscard]] std::optional<LayoutColour> LayoutValueColour(const EffectLayout& layout, const LayoutControl& control);
/// How wide a label may be before it ends in an ellipsis: the layout's cap, else 80.
[[nodiscard]] double LayoutLabelMaxWidth(const EffectLayout& layout);

inline constexpr double kLayoutDefaultKnobDiameter = 38.0;

/// A size from a layout clamped to [lo, hi] and rounded, or nullopt when it is not a number.
[[nodiscard]] std::optional<double> LayoutSizeValue(const std::optional<double>& value, double lo, double hi);
[[nodiscard]] double LayoutKnobDiameter(const LayoutControlStyle& style);   // 16..512, default 38
[[nodiscard]] double LayoutSliderLength(const LayoutControlStyle& style);   // 24..2048, default 80
[[nodiscard]] LayoutSize LayoutToggleSize(const LayoutControlStyle& style); // 8..512 each, default 36 x 20

/// The control's image knob when it is usable (a face, and a frame count for a filmstrip).
[[nodiscard]] const LayoutKnobImage* UsableKnobImage(const LayoutControlStyle& style);

/// A filmstrip's frame count when `frames` is a whole number from 2 to 1024, else 0.
[[nodiscard]] int FilmstripFrameCount(double frames);
/// The frame (0-based) for a knob position of 0..1.
[[nodiscard]] int FilmstripFrame(double position, int frames);

struct FilmstripCell
{
    int column = 0;
    int row = 0;
    int columns = 1;
    int rows = 1;
};

/// Where `frame` sits in a strip or sheet of `columns` columns.
[[nodiscard]] FilmstripCell FilmstripCellOf(int frame, int frames, int columns);

/// A turning face's angle in degrees for a knob position of 0..1.
[[nodiscard]] double KnobFaceAngle(double position, double minAngle = -135.0, double maxAngle = 135.0);

/// Whether an overlay is drawn for a node bypassed or not (its bypass hotspot is always there).
[[nodiscard]] bool LayoutOverlayVisible(const LayoutOverlay& overlay, bool bypassed);
} // namespace guitarfx::uiclient
