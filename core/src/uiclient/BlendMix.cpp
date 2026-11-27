// BlendMix - see BlendMix.h.

#include "uiclient/BlendMix.h"

#include "dsp/EffectGuids.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace guitarfx::uiclient
{
namespace
{
/// Two captured values closer than this are one (BLEND_MAPPING_EPS).
constexpr double kMappingEps = 1e-4;
/// The engine leaves out a model asked for less than this share (BLEND_MIN_AUDIBLE_WEIGHT).
constexpr double kMinAudibleWeight = 0.02;

constexpr std::array<BlendParamSpec, 12> kSpecs{{
    {"gain", "Gain", 0, 10},
    {"drive", "Drive", 0, 10},
    {"contour", "Contour", 0, 10},
    {"treble", "Treble", 0, 10},
    {"middle", "Middle", 0, 10},
    {"bass", "Bass", 0, 10},
    {"presence", "Presence", 0, 10},
    {"tone", "Tone", 0, 10},
    {"level", "Level", 0, 10},
    {"custom_a", "Custom A", 0, 10},
    {"custom_b", "Custom B", 0, 10},
    {"custom_c", "Custom C", 0, 10},
}};

bool HasClose(const std::vector<double>& values, double value)
{
    return std::any_of(values.begin(), values.end(), [value](double v) { return std::abs(v - value) <= kMappingEps; });
}

void AddUnique(std::vector<double>& values, double value)
{
    if (!HasClose(values, value))
    {
        values.push_back(value);
    }
}

double Median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    const std::size_t mid = values.size() / 2;
    return values.size() % 2 == 0 ? (values[mid - 1] + values[mid]) / 2.0 : values[mid];
}

std::vector<double> CapturedValues(const std::string& paramId, const BlendSummary& blend)
{
    std::vector<double> values;

    for (const auto& mapping : blend.mappings)
    {
        const auto params = BlendMappingParams(mapping);

        if (const auto it = params.find(paramId); it != params.end())
        {
            values.push_back(it->second);
        }
    }

    return values;
}

BlendMix Prune(BlendMix mix)
{
    if (mix.secondary < 0)
    {
        return mix;
    }

    if (mix.secondaryWeight < kMinAudibleWeight)
    {
        return {mix.primary, 1.0, -1, 0.0};
    }

    if (mix.primaryWeight < kMinAudibleWeight)
    {
        return {mix.secondary, 1.0, -1, 0.0};
    }

    return mix;
}

/// Where a model sits on the Blend sweep, as BlendSupport.cpp places it.
double SweepPosition(const BlendModelMapping& mapping, std::size_t index, std::size_t count)
{
    if (mapping.parameterValue)
    {
        return *mapping.parameterValue;
    }

    if (!mapping.parameterId.empty())
    {
        if (const auto it = mapping.parameters.find(mapping.parameterId); it != mapping.parameters.end())
        {
            return it->second;
        }
    }

    return count > 1 ? static_cast<double>(index) / static_cast<double>(count - 1) : 0.0;
}

std::string ModelName(const ClientState& state, const std::string& modelId)
{
    const auto* resource = state.FindLibraryResource("nam", modelId);

    if (resource == nullptr)
    {
        return modelId;
    }

    const auto first = resource->name.find_first_not_of(" \t");
    return first == std::string::npos
               ? modelId
               : resource->name.substr(first, resource->name.find_last_not_of(" \t") - first + 1);
}
} // namespace

const BlendParamSpec* FindBlendParamSpec(const std::string& paramId)
{
    const auto it = std::find_if(kSpecs.begin(), kSpecs.end(), [&paramId](const auto& s) { return paramId == s.id; });
    return it != kSpecs.end() ? &*it : nullptr;
}

double NormalizeBlendValue(double value, const BlendParamSpec* spec)
{
    if (spec == nullptr)
    {
        return value;
    }

    if (value < 0.0)
    {
        return value / 10.0;
    }

    const double range = spec->max - spec->min;
    return range <= 0.0 ? 0.0 : (std::clamp(value, spec->min, spec->max) - spec->min) / range;
}

double DenormalizeBlendValue(double value, const BlendParamSpec* spec)
{
    if (spec == nullptr)
    {
        return value;
    }

    return value < 0.0 ? value * 10.0 : spec->min + value * (spec->max - spec->min);
}

std::map<std::string, double> BlendMappingParams(const BlendModelMapping& mapping)
{
    if (!mapping.parameters.empty())
    {
        return mapping.parameters;
    }

    if (!mapping.parameterId.empty() && mapping.parameterValue)
    {
        return {{mapping.parameterId, *mapping.parameterValue}};
    }

    return {};
}

const BlendSummary* FindNodeBlend(const ClientState& state, const GraphNode& node)
{
    if (node.type != EffectGuids::kAmpNamBlend)
    {
        return nullptr;
    }

    const auto id = node.config.find("blendId");

    if (id == node.config.end() || id->second.empty())
    {
        return nullptr;
    }

    const auto it = std::find_if(state.blends.begin(), state.blends.end(),
                                 [&id](const BlendSummary& b) { return b.id == id->second; });
    return it != state.blends.end() ? &*it : nullptr;
}

bool BlendSnaps(const GraphNode& node, const BlendSummary& blend)
{
    const auto it = node.config.find("blendModeOverride");

    if (it != node.config.end() && (it->second == "snap" || it->second == "interpolate"))
    {
        return it->second == "snap";
    }

    return blend.blendMode == "snap";
}

std::vector<std::string> MappedBlendParamIds(const BlendSummary& blend)
{
    std::vector<std::string> captured;

    for (const auto& mapping : blend.mappings)
    {
        for (const auto& [id, value] : BlendMappingParams(mapping))
        {
            if (std::find(captured.begin(), captured.end(), id) == captured.end())
            {
                captured.push_back(id);
            }
        }
    }

    std::vector<std::string> ordered;

    for (const auto& id : blend.parameters)
    {
        if (std::find(captured.begin(), captured.end(), id) != captured.end() &&
            std::find(ordered.begin(), ordered.end(), id) == ordered.end())
        {
            ordered.push_back(id);
        }
    }

    for (const auto& id : captured)
    {
        if (std::find(ordered.begin(), ordered.end(), id) == ordered.end())
        {
            ordered.push_back(id);
        }
    }

    return ordered;
}

std::map<std::string, double> BlendTarget(const GraphNode& node, const BlendSummary& blend)
{
    std::map<std::string, double> target;

    for (const auto& id : MappedBlendParamIds(blend))
    {
        if (const auto it = node.params.find(id); it != node.params.end())
        {
            target[id] = it->second;
        }
        else if (const auto values = CapturedValues(id, blend); !values.empty())
        {
            target[id] = Median(values);
        }
    }

    return target;
}

BlendParamRange ComputeBlendParamRange(const std::string& paramId, const BlendSummary& blend,
                                       std::optional<double> current)
{
    BlendParamRange range;
    range.spec = FindBlendParamSpec(paramId);
    std::vector<double> values;

    for (const double v : CapturedValues(paramId, blend))
    {
        values.push_back(DenormalizeBlendValue(v, range.spec));
    }

    const double fallback =
        current ? DenormalizeBlendValue(*current, range.spec) : (range.spec != nullptr ? range.spec->min : 0.0);

    if (values.empty())
    {
        range.min = range.spec != nullptr ? range.spec->min : -1.0;
        range.max = range.spec != nullptr ? range.spec->max : 1.0;
        range.defaultValue = fallback;
        return range;
    }

    const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
    range.min = *lo == *hi ? *lo - 0.5 : *lo;
    range.max = *lo == *hi ? *hi + 0.5 : *hi;
    range.defaultValue = current ? fallback : Median(values);
    return range;
}

std::optional<BlendMix> SelectBlendMix(const std::vector<BlendModelMapping>& mappings,
                                       const std::map<std::string, double>& target, double blend, bool snap)
{
    if (mappings.empty())
    {
        return std::nullopt;
    }

    if (mappings.size() == 1)
    {
        return BlendMix{};
    }

    std::vector<std::map<std::string, double>> maps;
    std::vector<std::string> mapped;

    for (const auto& mapping : mappings)
    {
        maps.push_back(BlendMappingParams(mapping));

        for (const auto& [id, value] : maps.back())
        {
            if (std::find(mapped.begin(), mapped.end(), id) == mapped.end())
            {
                mapped.push_back(id);
            }
        }
    }

    std::vector<std::string> active;

    for (const auto& [id, value] : target)
    {
        if (std::find(mapped.begin(), mapped.end(), id) != mapped.end())
        {
            active.push_back(id);
        }
    }

    std::vector<int> order(mappings.size());
    std::iota(order.begin(), order.end(), 0);

    if (!active.empty())
    {
        std::vector<double> distances;

        for (const auto& params : maps)
        {
            double distance = 0.0;
            bool matched = false;

            for (const auto& id : active)
            {
                const auto it = params.find(id);

                if (it == params.end())
                {
                    distance += 4.0;
                    continue;
                }

                const double d = it->second - target.at(id);
                distance += d * d;
                matched = true;
            }

            distances.push_back(matched ? distance : distance + 9.0);
        }

        std::stable_sort(order.begin(), order.end(),
                         [&distances](int a, int b) { return distances[a] < distances[b]; });
        const int best = order[0];
        const int second = order[1];

        if (snap)
        {
            return BlendMix{best, 1.0, -1, 0.0};
        }

        constexpr double eps = 1e-6;
        const double w1 = 1.0 / std::max(distances[best], eps);
        const double w2 = 1.0 / std::max(distances[second], eps);
        const double total = std::max(w1 + w2, eps);
        return Prune({best, w1 / total, second, w2 / total});
    }

    std::vector<double> positions;

    for (std::size_t i = 0; i < mappings.size(); ++i)
    {
        positions.push_back(SweepPosition(mappings[i], i, mappings.size()));
    }

    std::stable_sort(order.begin(), order.end(), [&positions](int a, int b) { return positions[a] < positions[b]; });
    const double min = positions[order.front()];
    const double max = positions[order.back()];
    const double at = min + std::clamp(blend, 0.0, 1.0) * (max - min);

    if (at <= min)
    {
        return BlendMix{order.front(), 1.0, -1, 0.0};
    }

    if (at >= max)
    {
        return BlendMix{order.back(), 1.0, -1, 0.0};
    }

    std::size_t upper = 1;

    while (upper < order.size() && positions[order[upper]] < at)
    {
        ++upper;
    }

    const std::size_t lower = upper - 1;
    const double lowerValue = positions[order[lower]];
    const double upperValue = positions[order[upper]];

    if (snap)
    {
        return BlendMix{at - lowerValue <= upperValue - at ? order[lower] : order[upper], 1.0, -1, 0.0};
    }

    const double t = std::clamp((at - lowerValue) / std::max(upperValue - lowerValue, 1e-9), 0.0, 1.0);
    return Prune({order[lower], 1.0 - t, order[upper], t});
}

std::string BlendMatchLine(const ClientState& state, const GraphNode& node, const BlendSummary& blend)
{
    const auto it = node.params.find("blend");
    const double sweep = it != node.params.end() ? it->second : 0.0;
    const auto mix = SelectBlendMix(blend.mappings, BlendTarget(node, blend), sweep, BlendSnaps(node, blend));

    if (!mix)
    {
        return "Playing: \xE2\x80\x94"; // an em dash: no models
    }

    const auto primary = ModelName(state, blend.mappings[static_cast<std::size_t>(mix->primary)].modelId);

    if (mix->secondary < 0)
    {
        return "Playing: " + primary;
    }

    const auto secondary = ModelName(state, blend.mappings[static_cast<std::size_t>(mix->secondary)].modelId);
    const int share = static_cast<int>(std::lround(mix->primaryWeight * 100.0));
    return "Mix: " + primary + " " + std::to_string(share) + "% / " + secondary + " " + std::to_string(100 - share) +
           "%";
}

std::vector<BlendMappedPoint> BlendMappedPoints(const std::string& paramId, const BlendSummary& blend,
                                                const std::map<std::string, double>& target)
{
    std::vector<double> all, selectable;
    const auto ids = MappedBlendParamIds(blend);

    for (const auto& mapping : blend.mappings)
    {
        const auto params = BlendMappingParams(mapping);
        const auto mine = params.find(paramId);

        if (mine == params.end())
        {
            continue;
        }

        AddUnique(all, mine->second);

        // Reachable as the other knobs stand: every other mapped value of this capture is theirs.
        const bool matches = std::all_of(ids.begin(), ids.end(), [&](const std::string& other) {
            if (other == paramId)
            {
                return true;
            }

            const auto t = target.find(other);
            const auto p = params.find(other);
            return t != target.end() && p != params.end() && std::abs(p->second - t->second) <= kMappingEps;
        });

        if (matches)
        {
            AddUnique(selectable, mine->second);
        }
    }

    std::sort(all.begin(), all.end());
    const auto spec = FindBlendParamSpec(paramId);
    const auto now = target.find(paramId);
    std::vector<BlendMappedPoint> points;

    for (const double v : all)
    {
        points.push_back({DenormalizeBlendValue(v, spec), HasClose(selectable, v),
                          now != target.end() && std::abs(now->second - v) <= kMappingEps});
    }

    return points;
}

const BlendModelMapping* ClosestBlendMapping(const std::string& paramId, double value, const BlendSummary& blend,
                                             const std::map<std::string, double>& target)
{
    const BlendModelMapping* best = nullptr;
    double bestDelta = std::numeric_limits<double>::infinity();
    double bestSecondary = std::numeric_limits<double>::infinity();
    const auto ids = MappedBlendParamIds(blend);

    for (const auto& mapping : blend.mappings)
    {
        const auto params = BlendMappingParams(mapping);
        const auto mine = params.find(paramId);

        if (mine == params.end())
        {
            continue;
        }

        const double delta = std::abs(mine->second - value);
        double secondary = 0.0;

        for (const auto& other : ids)
        {
            if (other == paramId)
            {
                continue;
            }

            const auto t = target.find(other);
            const auto p = params.find(other);

            if (t == target.end() || p == params.end())
            {
                secondary += 4.0;
                continue;
            }

            const double d = p->second - t->second;
            secondary += d * d;
        }

        if (delta < bestDelta - kMappingEps ||
            (std::abs(delta - bestDelta) <= kMappingEps && secondary < bestSecondary))
        {
            best = &mapping;
            bestDelta = delta;
            bestSecondary = secondary;
        }
    }

    return best;
}
} // namespace guitarfx::uiclient
