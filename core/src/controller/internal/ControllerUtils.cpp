#include "controller/internal/ControllerUtils.h"

#include "IPluginHost.h"
#include "presets/PresetTypes.h"
#include "presets/PresetTypesJson.h"
#include "resources/PluginPathUtils.h"
#include "util/FileIO.h"
#include "util/FileSystem.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace guitarfx::controller_detail
{
bool IsSensitiveDebugKey(std::string_view key)
{
    if (key.empty())
    {
        return false;
    }

    std::string normalizedKey(key);
    std::transform(normalizedKey.begin(), normalizedKey.end(), normalizedKey.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    return normalizedKey.find("token") != std::string::npos || normalizedKey.find("api_key") != std::string::npos ||
           normalizedKey.find("apikey") != std::string::npos || normalizedKey.find("secret") != std::string::npos ||
           normalizedKey.find("password") != std::string::npos ||
           normalizedKey.find("authorization") != std::string::npos ||
           normalizedKey.find("cookie") != std::string::npos || normalizedKey.find("credential") != std::string::npos;
}

void ScrubSensitiveJson(nlohmann::json& value, std::string_view currentKey)
{
    if (IsSensitiveDebugKey(currentKey))
    {
        value = "<redacted>";
        return;
    }

    if (value.is_object())
    {
        for (auto it = value.begin(); it != value.end(); ++it)
        {
            ScrubSensitiveJson(it.value(), it.key());
        }

        return;
    }

    if (value.is_array())
    {
        for (auto& entry : value)
        {
            ScrubSensitiveJson(entry);
        }
    }
}

std::filesystem::path ResolveDebugSnapshotPath(const guitarfx::FileSystem& fileSystem)
{
    return fileSystem.ResolveSettingsDirectory() / kDebugSnapshotFileName;
}

double ToDbFS(double linear)
{
    if (linear <= kMinLinear)
    {
        return -120.0;
    }

    return 20.0 * std::log10(linear);
}

std::string FormatTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &tt);
#else
    localtime_r(&tt, &localTime);
#endif
    std::ostringstream oss;
    oss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

double LinearFromDb(double db)
{
    if (!std::isfinite(db))
    {
        return 0.0;
    }

    return std::pow(10.0, db / 20.0);
}

double ClampValue(double value, double minimum, double maximum)
{
    return std::min(maximum, std::max(minimum, value));
}

int ComputeBarsFromFrames(std::size_t frameCount, double sampleRate, double tempoBpm, int timeSigNum, int timeSigDen)
{
    if (frameCount == 0)
    {
        return 1;
    }

    const double samplesPerBeat =
        sampleRate * (60.0 / std::max(1.0, tempoBpm)) * (4.0 / static_cast<double>(std::max(1, timeSigDen)));
    const double samplesPerBar = samplesPerBeat * static_cast<double>(std::max(1, timeSigNum));
    return std::max(1, static_cast<int>(std::round(static_cast<double>(frameCount) / std::max(1.0, samplesPerBar))));
}

/// Maps a resource type onto a native dialog category.
guitarfx::BrowseFileType ResolveBrowseFileType(const std::string& resourceType)
{
    using guitarfx::BrowseFileType;

    if (resourceType == "nam")
    {
        return BrowseFileType::NAMModel;
    }

    if (resourceType == "ir")
    {
        return BrowseFileType::IRFile;
    }

    if (resourceType == "plugin")
    {
        return BrowseFileType::PluginFile;
    }

    return BrowseFileType::Any;
}

/// Content hashes are only meaningful for regular files.
///
/// A plugin bundle is a *directory*, and hashing one yields either an empty
/// string or the bare FNV offset basis (whether opening a directory succeeds
/// is platform-dependent) — never anything content-derived. So every bundle
/// would share a single hash, and that is not harmless: ResourceLibrary falls
/// back to hash equality when a resource's file is missing, which would
/// silently resolve a moved plugin to an unrelated one. Leave the hash empty
/// for directories instead; plugins de-duplicate on their stable id.
bool ShouldHashResourceFile(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

std::string InferPluginFormatFromPath(const std::filesystem::path& path)
{
    return std::string{guitarfx::pluginpath::PluginFormatId(guitarfx::pluginpath::PluginFormatFromPath(path))};
}

bool HasUnsafeRelativeSegments(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute())
    {
        return false;
    }

    for (const auto& segment : path)
    {
        if (segment == "..")
        {
            return true;
        }
    }

    return false;
}

const guitarfx::GraphNode* FindNodeByIdOrType(const guitarfx::SignalGraph& graph, const std::string& id,
                                              const std::string& type)
{
    for (const auto& node : graph.nodes)
    {
        if (node.id == id)
        {
            return &node;
        }
    }

    for (const auto& node : graph.nodes)
    {
        if (node.type == type)
        {
            return &node;
        }
    }

    return nullptr;
}

int GetGlobalTransposeFromChainConfig(const guitarfx::GlobalSignalChainConfig& config)
{
    const auto* transposeNode =
        FindNodeByIdOrType(config.preChainGraph, "global_transpose", guitarfx::EffectGuids::kTranspose);

    if (!transposeNode || !transposeNode->enabled)
    {
        return 0;
    }

    const auto semitonesIt = transposeNode->params.find("semitones");

    if (semitonesIt == transposeNode->params.end())
    {
        return 0;
    }

    return static_cast<int>(std::round(std::clamp(semitonesIt->second, -12.0, 12.0)));
}

nlohmann::json SerializeGlobalFxSettings(const guitarfx::GlobalSignalChainConfig& config)
{
    // Serialize global FX (gate, transpose, EQ, doubler) to app settings for persistence.
    // This is per-instance state saved to app.json (standalone) or host state (plugin).
    // Global FX are NEVER saved in presets—presets contain only the signal graph.
    return nlohmann::json{{"inputGain", config.inputGain},
                          {"outputGain", config.outputGain},
                          {"preChainGraph", guitarfx::SerializeSignalGraph(config.preChainGraph)},
                          {"postChainGraph", guitarfx::SerializeSignalGraph(config.postChainGraph)}};
}

void SaveJsonFile(const guitarfx::FileSystem& fileSystem, const std::filesystem::path& path,
                  const nlohmann::json& payload)
{
    if (path.empty())
    {
        return;
    }

    try
    {
        [[maybe_unused]] const auto ensuredParent = fileSystem.EnsureDirectory(path.parent_path());
        std::ofstream output(path);

        if (output.is_open())
        {
            output << payload.dump(2);
        }
    }
    catch (const std::exception&)
    {
    }
}

std::string BuildUtcIsoTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm utcTime{};
#ifdef _WIN32
    gmtime_s(&utcTime, &tt);
#else
    gmtime_r(&tt, &utcTime);
#endif
    std::ostringstream oss;
    oss << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string MakeUniqueNodeId(const guitarfx::SignalGraph& graph, const std::string& baseId)
{
    std::string candidate = baseId;
    int suffix = 1;

    while (graph.FindNode(candidate))
    {
        candidate = baseId + std::to_string(suffix++);
    }

    return candidate;
}

bool IsGraphAcyclic(const guitarfx::SignalGraph& graph)
{
    std::unordered_map<std::string, int> indegree;
    std::unordered_map<std::string, std::vector<std::string>> outgoing;

    for (const auto& node : graph.nodes)
    {
        indegree.emplace(node.id, 0);
    }

    for (const auto& edge : graph.edges)
    {
        indegree.try_emplace(edge.from, 0);
        indegree.try_emplace(edge.to, 0);
        outgoing[edge.from].push_back(edge.to);
        indegree[edge.to] += 1;
    }

    std::deque<std::string> queue;

    for (const auto& [id, count] : indegree)
    {
        if (count == 0)
        {
            queue.push_back(id);
        }
    }

    size_t visited = 0;

    while (!queue.empty())
    {
        const std::string nodeId = queue.front();
        queue.pop_front();
        visited += 1;

        const auto outIt = outgoing.find(nodeId);

        if (outIt == outgoing.end())
        {
            continue;
        }

        for (const auto& nextId : outIt->second)
        {
            auto indegreeIt = indegree.find(nextId);

            if (indegreeIt == indegree.end())
            {
                continue;
            }

            indegreeIt->second -= 1;

            if (indegreeIt->second == 0)
            {
                queue.push_back(nextId);
            }
        }
    }

    return visited == indegree.size();
}

std::string GenerateGuidV4String()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::uint32_t> dis(0, 0xFFFFFFFFu);

    std::uint32_t d0 = dis(gen);
    std::uint32_t d1 = dis(gen);
    std::uint32_t d2 = dis(gen);
    std::uint32_t d3 = dis(gen);

    // Set version to 4 (0100)
    d1 = (d1 & 0xFFFF0FFFu) | 0x00004000u;
    // Set variant to 10xx
    d2 = (d2 & 0x3FFFFFFFu) | 0x80000000u;

    auto hex = [](std::uint32_t value, int width) {
        std::ostringstream oss;
        oss << std::hex << std::nouppercase << std::setfill('0') << std::setw(width) << value;
        return oss.str();
    };

    // UUID layout: 8-4-4-4-12
    const std::string part1 = hex(d0, 8);
    const std::string part2 = hex((d1 >> 16) & 0xFFFFu, 4);
    const std::string part3 = hex(d1 & 0xFFFFu, 4);
    const std::string part4 = hex((d2 >> 16) & 0xFFFFu, 4);
    const std::string part5 = hex(d2 & 0xFFFFu, 4) + hex(d3, 8);
    return part1 + "-" + part2 + "-" + part3 + "-" + part4 + "-" + part5;
}

std::string HashStringForLog(std::string_view value)
{
    constexpr std::uint64_t kFNVOffsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t kFNVPrime = 1099511628211ull;

    std::uint64_t hash = kFNVOffsetBasis;

    for (const unsigned char byte : value)
    {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kFNVPrime;
    }

    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

std::string GenerateUserPresetId()
{
    return "user-" + GenerateGuidV4String();
}
} // namespace guitarfx::controller_detail
