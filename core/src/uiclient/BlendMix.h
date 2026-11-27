#pragma once

// BlendMix - what a NAM Blend node plays for its knob values, and how its knobs are drawn: the
// web UI's blendUtils.ts and signalPathBlend.ts for the native UIs. A mapped knob shows its
// parameter's own scale (Gain 0..10) over the range the models were captured at; the node and
// the engine hold the value 0..1.

#include "presets/PresetTypes.h"
#include "uiclient/ClientState.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
/// A blend parameter's name and scale (BLEND_PARAM_SPECS).
struct BlendParamSpec
{
    const char* id;
    const char* label;
    double min;
    double max;
};

[[nodiscard]] const BlendParamSpec* FindBlendParamSpec(const std::string& paramId);
/// A knob's value on its parameter's scale to the 0..1 the engine takes, and back. A negative
/// value is a tenth of the scale, as the web UI reads one.
[[nodiscard]] double NormalizeBlendValue(double value, const BlendParamSpec* spec);
[[nodiscard]] double DenormalizeBlendValue(double value, const BlendParamSpec* spec);

/// A mapping's captured values: its parameters, else its one parameterId and value.
[[nodiscard]] std::map<std::string, double> BlendMappingParams(const BlendModelMapping& mapping);

/// The blend a node plays, or null: not a blend node, no blendId, or not in the library.
[[nodiscard]] const BlendSummary* FindNodeBlend(const ClientState& state, const GraphNode& node);
/// The mode in effect: the node's override, else the definition's. True for snap.
[[nodiscard]] bool BlendSnaps(const GraphNode& node, const BlendSummary& blend);

/// The parameters a knob is shown for: those some model was captured at, in the order the
/// definition lists them, then any it does not. None: the Blend sweep picks the model.
[[nodiscard]] std::vector<std::string> MappedBlendParamIds(const BlendSummary& blend);

/// The node's values for the mapped parameters (0..1), an unset one at the median of the
/// captured values, where the engine starts it.
[[nodiscard]] std::map<std::string, double> BlendTarget(const GraphNode& node, const BlendSummary& blend);

/// A mapped knob's range on its parameter's scale: the captured values' span, the default the
/// current value (or the median).
struct BlendParamRange
{
    double min = 0.0;
    double max = 1.0;
    double defaultValue = 0.0;
    const BlendParamSpec* spec = nullptr;
};

[[nodiscard]] BlendParamRange ComputeBlendParamRange(const std::string& paramId, const BlendSummary& blend,
                                                     std::optional<double> current);

/// The models played, as indices into the mappings, with each one's share.
struct BlendMix
{
    int primary = 0;
    double primaryWeight = 1.0;
    int secondary = -1;
    double secondaryWeight = 0.0;
};

/// selectBlendMix: the nearest captures to `target`, or the Blend sweep when none is set.
[[nodiscard]] std::optional<BlendMix> SelectBlendMix(const std::vector<BlendModelMapping>& mappings,
                                                     const std::map<std::string, double>& target, double blend,
                                                     bool snap);

/// The line under the blend's name: "Playing: <model>", or "Mix: A 60% / B 40%".
[[nodiscard]] std::string BlendMatchLine(const ClientState& state, const GraphNode& node, const BlendSummary& blend);

/// Where a model was captured on one knob: on the knob's scale, whether the other knobs' values
/// reach it as they stand (selectable), and whether the knob is on it.
struct BlendMappedPoint
{
    double display = 0.0;
    bool selectable = false;
    bool selected = false;
};

[[nodiscard]] std::vector<BlendMappedPoint> BlendMappedPoints(const std::string& paramId, const BlendSummary& blend,
                                                              const std::map<std::string, double>& target);

/// In Snap mode, the capture a knob turned to `value` (0..1) lands on: the nearest on that knob,
/// the others' distance breaking a tie. Null when no model was captured at that parameter.
[[nodiscard]] const BlendModelMapping* ClosestBlendMapping(const std::string& paramId, double value,
                                                           const BlendSummary& blend,
                                                           const std::map<std::string, double>& target);
} // namespace guitarfx::uiclient
