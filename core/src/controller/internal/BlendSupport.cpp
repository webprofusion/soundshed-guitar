#include "controller/internal/BlendSupport.h"

#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "presets/PresetTypes.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <unordered_set>

namespace guitarfx::controller_detail
{
namespace
{
/// The editor offers the same range; a hand-edited definition is held to it.
constexpr double kMinBlendLevelTrimDb = -24.0;
constexpr double kMaxBlendLevelTrimDb = 24.0;

/// One model of a definition's `modelMappings`, placed on the blend sweep.
ResourceRef BuildMappedModelRef(const nlohmann::json& mapping, std::size_t index, std::size_t count)
{
    ResourceRef ref;
    ref.resourceType = "nam";
    ref.resourceId = mapping.value("id", "");

    if (mapping.contains("parameters") && mapping["parameters"].is_object())
    {
        for (const auto& [key, value] : mapping["parameters"].items())
        {
            if (value.is_number())
            {
                ref.parameters[key] = value.get<double>();
            }
        }
    }

    // A trim evens out models captured at different levels, from other packs most often.
    if (mapping.contains("levelTrimDb") && mapping["levelTrimDb"].is_number())
    {
        const double trimDb = mapping["levelTrimDb"].get<double>();

        if (guitarfx::IsFinite(trimDb) && trimDb != 0.0)
        {
            ref.levelTrimDb = std::clamp(trimDb, kMinBlendLevelTrimDb, kMaxBlendLevelTrimDb);
        }
    }

    // The captured value of the primary parameter places the model on the blend sweep. The
    // editor writes it twice, as parameterValue and in parameters.
    const std::string parameterId = mapping.value("parameterId", "");
    std::optional<double> capturedValue;

    if (mapping.contains("parameterValue") && mapping["parameterValue"].is_number())
    {
        capturedValue = mapping["parameterValue"].get<double>();
    }
    else if (const auto it = ref.parameters.find(parameterId); it != ref.parameters.end())
    {
        capturedValue = it->second;
    }

    if (capturedValue)
    {
        ref.parameterId = parameterId;
        ref.parameterValue = *capturedValue;

        if (ref.parameters.empty() && !parameterId.empty())
        {
            ref.parameters[parameterId] = *capturedValue;
        }
    }
    else
    {
        // Nothing captured: the model keeps its list position on the sweep. It gets no
        // parameterId, because the effect reads a parameterId with a value as a captured
        // setting and would match knob positions against the list position.
        ref.parameterValue = count > 1 ? static_cast<double>(index) / static_cast<double>(count - 1) : 0.0;
    }

    return ref;
}

std::vector<ResourceRef> BuildBlendResourceRefs(const nlohmann::json& blend)
{
    std::vector<ResourceRef> refs;
    const auto mappings = blend.value("modelMappings", nlohmann::json::array());

    if (mappings.is_array() && !mappings.empty())
    {
        for (std::size_t i = 0; i < mappings.size(); ++i)
        {
            if (mappings[i].is_object() && !mappings[i].value("id", "").empty())
            {
                refs.push_back(BuildMappedModelRef(mappings[i], i, mappings.size()));
            }
        }

        return refs;
    }

    // A definition from before per-model mappings: the models spread evenly over the sweep.
    const auto models = blend.value("models", nlohmann::json::array());

    if (!models.is_array())
    {
        return refs;
    }

    for (std::size_t i = 0; i < models.size(); ++i)
    {
        if (!models[i].is_string())
        {
            continue;
        }

        ResourceRef ref;
        ref.resourceType = "nam";
        ref.resourceId = models[i].get<std::string>();
        ref.parameterValue = models.size() > 1 ? static_cast<double>(i) / static_cast<double>(models.size() - 1) : 0.0;
        refs.push_back(std::move(ref));
    }

    return refs;
}

/// Gives each mapped parameter the node has no value for the median of its captured values,
/// where the UI draws the knob. Without it the effect has no target for that parameter and
/// falls back to the blend sweep, so the knob and the sound disagree.
void SeedUnsetBlendParameters(GraphNode& node)
{
    std::map<std::string, std::vector<double>> captured;

    for (const auto& ref : node.resources)
    {
        for (const auto& [key, value] : ref.parameters)
        {
            captured[key].push_back(value);
        }
    }

    for (auto& [key, values] : captured)
    {
        if (values.empty() || node.params.contains(key))
        {
            continue;
        }

        std::sort(values.begin(), values.end());
        const std::size_t mid = values.size() / 2;
        node.params[key] = values.size() % 2 == 0 ? (values[mid - 1] + values[mid]) / 2.0 : values[mid];
    }
}
} // namespace

const nlohmann::json* FindBlendDefinition(const nlohmann::json& library, const std::string& blendId)
{
    if (!library.is_array() || blendId.empty())
    {
        return nullptr;
    }

    for (const auto& blend : library)
    {
        if (blend.is_object() && blend.value("id", "") == blendId)
        {
            return &blend;
        }
    }

    return nullptr;
}

std::vector<std::string> CollectBlendModelIds(const nlohmann::json& blend)
{
    std::vector<std::string> ids;
    std::unordered_set<std::string> seen;

    if (!blend.is_object())
    {
        return ids;
    }

    const auto add = [&](const std::string& id) {
        if (!id.empty() && seen.insert(id).second)
        {
            ids.push_back(id);
        }
    };

    if (const auto mappings = blend.value("modelMappings", nlohmann::json::array()); mappings.is_array())
    {
        for (const auto& mapping : mappings)
        {
            if (mapping.is_object())
            {
                add(mapping.value("id", ""));
            }
        }
    }

    if (const auto models = blend.value("models", nlohmann::json::array()); models.is_array())
    {
        for (const auto& model : models)
        {
            if (model.is_string())
            {
                add(model.get<std::string>());
            }
        }
    }

    return ids;
}

std::string GetNodeBlendId(const GraphNode& node)
{
    if (node.type != EffectGuids::kAmpNamBlend)
    {
        return {};
    }

    const auto it = node.config.find(kBlendIdConfigKey);
    return it == node.config.end() ? std::string{} : it->second;
}

bool GraphPlaysBlend(const SignalGraph& graph, const std::string& blendId)
{
    return !blendId.empty() && std::any_of(graph.nodes.begin(), graph.nodes.end(),
                                           [&](const GraphNode& node) { return GetNodeBlendId(node) == blendId; });
}

bool PresetUsesBlend(const Preset& preset, const std::string& blendId)
{
    return GraphPlaysBlend(preset.graph, blendId) ||
           std::any_of(preset.scenes.begin(), preset.scenes.end(),
                       [&](const PresetScene& scene) { return GraphPlaysBlend(scene.graph, blendId); });
}

void ApplyBlendDefinitionToNode(GraphNode& node, const nlohmann::json& blend)
{
    node.resources = BuildBlendResourceRefs(blend);
    node.config["blendMode"] = blend.value("blendMode", "interpolate");

    if (node.label.empty())
    {
        node.label = blend.value("name", "");
    }

    SeedUnsetBlendParameters(node);
}

void ApplyBlendDefinitionsToGraph(SignalGraph& graph, const nlohmann::json& library)
{
    for (auto& node : graph.nodes)
    {
        const std::string blendId = GetNodeBlendId(node);

        if (blendId.empty())
        {
            continue;
        }

        const auto* blend = FindBlendDefinition(library, blendId);

        if (!blend)
        {
            node.resources.clear();
            continue;
        }

        ApplyBlendDefinitionToNode(node, *blend);
    }
}
} // namespace guitarfx::controller_detail
