/**
 * PresetBrowse.cpp - Preset browser filtering and ordering (see PresetBrowse.h).
 */

#include "uiclient/PresetBrowse.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace guitarfx::uiclient
{
namespace
{
std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
    {
        return {};
    }

    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/// Case-insensitive, with runs of digits compared by value.
int NaturalCompare(const std::string& a, const std::string& b)
{
    std::size_t i = 0;
    std::size_t j = 0;

    while (i < a.size() && j < b.size())
    {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[j]);

        if (std::isdigit(ca) && std::isdigit(cb))
        {
            std::size_t ei = i;
            std::size_t ej = j;

            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei])))
            {
                ++ei;
            }

            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej])))
            {
                ++ej;
            }

            auto na = a.substr(i, ei - i);
            auto nb = b.substr(j, ej - j);
            na.erase(0, std::min(na.find_first_not_of('0'), na.size() - 1));
            nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size() - 1));

            if (na.size() != nb.size())
            {
                return na.size() < nb.size() ? -1 : 1;
            }

            if (const int c = na.compare(nb); c != 0)
            {
                return c < 0 ? -1 : 1;
            }

            i = ei;
            j = ej;
            continue;
        }

        const auto la = std::tolower(ca);
        const auto lb = std::tolower(cb);

        if (la != lb)
        {
            return la < lb ? -1 : 1;
        }

        ++i;
        ++j;
    }

    if (i < a.size())
    {
        return 1;
    }

    return j < b.size() ? -1 : 0;
}

void CollectPresetIds(const PresetFolder& folder, std::set<std::string>& ids)
{
    ids.insert(folder.presetIds.begin(), folder.presetIds.end());

    for (const auto& child : folder.children)
    {
        CollectPresetIds(child, ids);
    }
}
} // namespace

int ComparePresetNames(const PresetSummary& left, const PresetSummary& right)
{
    const auto leftName = Trim(left.name).empty() ? left.id : Trim(left.name);
    const auto rightName = Trim(right.name).empty() ? right.id : Trim(right.name);

    if (const int byName = NaturalCompare(leftName, rightName); byName != 0)
    {
        return byName;
    }

    return NaturalCompare(left.id, right.id);
}

bool IsPresetFavorite(const ClientState& state, const std::string& presetId)
{
    return std::find(state.presetFavorites.begin(), state.presetFavorites.end(), presetId) !=
           state.presetFavorites.end();
}

const PresetFolder* FindPresetFolder(const std::vector<PresetFolder>& folders, const std::string& id)
{
    for (const auto& folder : folders)
    {
        if (folder.id == id)
        {
            return &folder;
        }

        if (const auto* found = FindPresetFolder(folder.children, id))
        {
            return found;
        }
    }

    return nullptr;
}

std::vector<PresetSummary> FilterPresets(const ClientState& state, const PresetQuery& query)
{
    const bool recents = query.folderId == kPresetFolderRecents;
    std::vector<PresetSummary> base;

    if (recents)
    {
        for (const auto& id : state.presetRecents)
        {
            if (const auto* summary = state.FindPresetSummary(id))
            {
                base.push_back(*summary);
            }
        }
    }
    else
    {
        base = state.presetList;
    }

    if (query.folderId == kPresetFolderFavorites)
    {
        std::erase_if(base, [&state](const PresetSummary& p) { return !IsPresetFavorite(state, p.id); });
    }
    else if (const auto* folder = FindPresetFolder(state.presetFolders, query.folderId))
    {
        std::set<std::string> allowed;
        CollectPresetIds(*folder, allowed);
        std::erase_if(base, [&allowed](const PresetSummary& p) { return !allowed.contains(p.id); });
    }

    const auto text = Lower(Trim(query.text));

    if (!text.empty())
    {
        std::erase_if(base, [&text](const PresetSummary& p) {
            return Lower(p.name).find(text) == std::string::npos && Lower(p.category).find(text) == std::string::npos;
        });
    }

    if (!recents)
    {
        std::stable_sort(base.begin(), base.end(),
                         [](const PresetSummary& a, const PresetSummary& b) { return ComparePresetNames(a, b) < 0; });
    }

    return base;
}
} // namespace guitarfx::uiclient
