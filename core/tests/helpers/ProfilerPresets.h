/**
 * @file ProfilerPresets.h
 * @brief Preset graphs spanning the range of chain weights the profiler measures.
 *
 * Three tiers, because the answer to "how much of a block is overhead" depends
 * entirely on how much real DSP the chain does:
 *   light    -- input, one trivial node, output: essentially pure framework overhead
 *   baseline -- delay/EQ/reverb, no model files (matches SignalChainThreadingBenchmark)
 *   namconv  -- NAM amp into IR cab into convolution reverb: a realistic amp rig
 */

#pragma once

#include "presets/PresetTypes.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::profiling
{
namespace fs = std::filesystem;

/// The lightest chain that still exercises the whole framework: input, one trivial
/// node, output. Whatever this costs is essentially pure per-block overhead.
inline Preset CreateLightPreset(const std::string& id)
{
    Preset preset;
    preset.id = id;
    preset.name = id;

    SignalGraph graph;
    graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});
    graph.nodes.push_back({"gain1", "gain", "utility", "Gain", true});
    graph.nodes.back().params["gainDb"] = 0.0;
    graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    graph.edges.push_back({"in", "gain1", 0, 0, 1.0});
    graph.edges.push_back({"gain1", "out", 0, 0, 1.0});

    preset.graph = std::move(graph);
    return preset;
}

/// Mirrors SignalChainThreadingBenchmark's baseline: a realistic all-DSP chain with
/// no model files, so numbers stay comparable between the two tools.
inline Preset CreateBaselinePreset(const std::string& id)
{
    Preset preset;
    preset.id = id;
    preset.name = id;

    SignalGraph graph;
    graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});

    graph.nodes.push_back({"gain1", "gain", "utility", "Gain 1", true});
    graph.nodes.back().params["gainDb"] = -1.0;

    graph.nodes.push_back({"delay", "delay_digital", "delay", "Delay", true});
    graph.nodes.back().params["timeMs"] = 85.0;
    graph.nodes.back().params["feedback"] = 0.42;
    graph.nodes.back().params["mix"] = 0.28;

    graph.nodes.push_back({"gain2", "gain", "utility", "Gain 2", true});
    graph.nodes.back().params["gainDb"] = 0.8;

    graph.nodes.push_back({"eq", "eq_parametric", "eq", "Parametric EQ", true});

    graph.nodes.push_back({"reverb", "reverb_room", "reverb", "Room Reverb", true});
    graph.nodes.back().params["mix"] = 0.24;
    graph.nodes.back().params["roomSize"] = 0.62;
    graph.nodes.back().params["damping"] = 0.35;

    graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});

    graph.edges.push_back({"in", "gain1", 0, 0, 1.0});
    graph.edges.push_back({"gain1", "delay", 0, 0, 1.0});
    graph.edges.push_back({"delay", "gain2", 0, 0, 1.0});
    graph.edges.push_back({"gain2", "eq", 0, 0, 1.0});
    graph.edges.push_back({"eq", "reverb", 0, 0, 1.0});
    graph.edges.push_back({"reverb", "out", 0, 0, 1.0});

    preset.graph = std::move(graph);
    return preset;
}

inline bool HasExtension(const fs::path& path, const std::string& extLower)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == extLower;
}

struct NamConvAssets
{
    fs::path model;
    fs::path irCab;
    fs::path reverbIr;
};

inline std::optional<NamConvAssets> DiscoverNamConvAssets(const fs::path& repoRoot)
{
    const fs::path presetsRoot = repoRoot / "resources" / "metal-presets";

    if (!fs::exists(presetsRoot))
    {
        return std::nullopt;
    }

    std::vector<fs::path> models;
    std::vector<fs::path> irs;

    for (const auto& entry : fs::recursive_directory_iterator(presetsRoot))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        if (HasExtension(entry.path(), ".nam"))
        {
            models.push_back(entry.path());
        }
        else if (HasExtension(entry.path(), ".wav"))
        {
            irs.push_back(entry.path());
        }
    }

    if (models.empty() || irs.size() < 2)
    {
        return std::nullopt;
    }

    std::sort(models.begin(), models.end());
    std::sort(irs.begin(), irs.end());

    NamConvAssets assets;
    assets.model = models[0];
    assets.irCab = irs[0];
    assets.reverbIr = irs[1];
    return assets;
}

/// A realistic amp rig: NAM amp into an IR cab into a convolution reverb. This is
/// what most users actually run, so it is the profile that matters most.
inline Preset CreateNamConvPreset(const std::string& id, const NamConvAssets& assets)
{
    Preset preset;
    preset.id = id;
    preset.name = id;

    SignalGraph graph;
    graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});

    GraphNode amp;
    amp.id = "amp";
    amp.type = "amp_nam_optimized";
    amp.category = "amp";
    amp.label = "NAM Amp";
    amp.enabled = true;
    ResourceRef ampRef;
    ampRef.resourceType = "nam";
    ampRef.filePath = assets.model;
    amp.resources.push_back(ampRef);
    graph.nodes.push_back(std::move(amp));

    GraphNode cab;
    cab.id = "cab";
    cab.type = "ir_cab";
    cab.category = "cab";
    cab.label = "IR Cab";
    cab.enabled = true;
    ResourceRef cabRef;
    cabRef.resourceType = "ir";
    cabRef.filePath = assets.irCab;
    cab.resources.push_back(cabRef);
    graph.nodes.push_back(std::move(cab));

    GraphNode reverb;
    reverb.id = "rev";
    reverb.type = "reverb_ir";
    reverb.category = "reverb";
    reverb.label = "Convolution Reverb";
    reverb.enabled = true;
    reverb.params["mix"] = 0.15;
    ResourceRef reverbRef;
    reverbRef.resourceType = "ir";
    reverbRef.filePath = assets.reverbIr;
    reverb.resources.push_back(reverbRef);
    graph.nodes.push_back(std::move(reverb));

    graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});

    graph.edges.push_back({"in", "amp", 0, 0, 1.0});
    graph.edges.push_back({"amp", "cab", 0, 0, 1.0});
    graph.edges.push_back({"cab", "rev", 0, 0, 1.0});
    graph.edges.push_back({"rev", "out", 0, 0, 1.0});

    preset.graph = std::move(graph);
    return preset;
}

// ---------------------------------------------------------------------------
} // namespace guitarfx::profiling
