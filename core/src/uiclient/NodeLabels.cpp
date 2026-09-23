/**
 * NodeLabels.cpp - Node display names (see NodeLabels.h).
 */

#include "uiclient/NodeLabels.h"

#include "uiclient/ChainLayout.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace guitarfx::uiclient
{
namespace
{
std::string Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string MetadataString(const nlohmann::json& metadata, const char* key)
{
    if (!metadata.is_object() || !metadata.contains(key))
    {
        return {};
    }

    const auto& value = metadata[key];

    if (value.is_string())
    {
        return value.get<std::string>();
    }

    if (value.is_number_integer())
    {
        return std::to_string(value.get<long long>());
    }

    return {};
}
} // namespace

std::string NodeResourceName(const ClientState& state, const GraphNode& node, std::size_t index)
{
    if (index >= node.resources.size())
    {
        return {};
    }

    const auto& ref = node.resources[index];

    if (ref.IsLibraryRef())
    {
        if (const auto* resource = state.FindLibraryResource(ref.resourceType, ref.resourceId))
        {
            return resource->name;
        }
    }

    if (ref.IsFilePath())
    {
        return util::PathToUtf8(ref.filePath.stem());
    }

    return ref.IsLibraryRef() ? ref.resourceId : std::string{};
}

std::string NodeDisplayName(const ClientState& state, const GraphNode& node)
{
    if (node.id == kInputNodeId || node.type == kNodeTypeInput)
    {
        return "Input";
    }

    if (node.id == kOutputNodeId || node.type == kNodeTypeOutput)
    {
        return "Output";
    }

    const auto* typeInfo = state.FindEffectType(node.type);

    if (typeInfo != nullptr && typeInfo->requiresResource)
    {
        std::string names;

        for (std::size_t i = 0; i < node.resources.size(); ++i)
        {
            const auto name = NodeResourceName(state, node, i);

            if (!name.empty())
            {
                names += names.empty() ? name : " + " + name;
            }
        }

        if (!names.empty())
        {
            return names;
        }
    }

    if (const auto label = Trim(node.label); !label.empty())
    {
        return label;
    }

    if (IsSplitterType(node.type))
    {
        return "Split";
    }

    if (IsMixerType(node.type))
    {
        return "Mix";
    }

    if (typeInfo != nullptr && !typeInfo->name.empty())
    {
        return typeInfo->name;
    }

    return node.type.empty() ? std::string("(Unknown)") : node.type;
}

std::string NodeArchitectureBadge(const ClientState& state, const GraphNode& node)
{
    for (const auto& ref : node.resources)
    {
        if (!ref.IsLibraryRef() || ref.resourceType != "nam")
        {
            continue;
        }

        const auto* resource = state.FindLibraryResource(ref.resourceType, ref.resourceId);

        if (resource == nullptr)
        {
            continue;
        }

        auto raw = MetadataString(resource->metadata, "architecture");

        if (raw.empty())
        {
            raw = MetadataString(resource->metadata, "architectureVersion");
        }

        std::transform(raw.begin(), raw.end(), raw.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        raw = Trim(raw);

        if (raw == "2" || raw == "a2")
        {
            return "A2";
        }

        if (raw == "1" || raw == "a1")
        {
            return "A1";
        }

        if (raw == "custom")
        {
            return "Custom";
        }
    }

    return {};
}

std::string NodeCategory(const ClientState& state, const GraphNode& node)
{
    if (const auto* typeInfo = state.FindEffectType(node.type); typeInfo != nullptr && !typeInfo->category.empty())
    {
        return typeInfo->category;
    }

    return node.category;
}
} // namespace guitarfx::uiclient
