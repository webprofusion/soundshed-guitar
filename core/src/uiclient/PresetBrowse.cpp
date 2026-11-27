/**
 * PresetBrowse.cpp - Preset browser filtering and ordering (see PresetBrowse.h).
 */

#include "uiclient/PresetBrowse.h"

#include "uiclient/FeatureFlags.h"

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

namespace
{
void Flatten(const std::vector<PresetFolder>& folders, int depth, std::vector<std::pair<const PresetFolder*, int>>& out)
{
    std::vector<const PresetFolder*> sorted;

    for (const auto& f : folders)
    {
        sorted.push_back(&f);
    }

    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    std::stable_sort(sorted.begin(), sorted.end(), [&lower](const PresetFolder* a, const PresetFolder* b) {
        return lower(a->name) < lower(b->name);
    });

    for (const auto* f : sorted)
    {
        out.emplace_back(f, depth);
        Flatten(f->children, depth + 1, out);
    }
}

std::string FolderPath(const std::vector<PresetFolder>& folders, const std::string& presetId, const std::string& prefix)
{
    for (const auto& f : folders)
    {
        const auto path = prefix.empty() ? f.name : prefix + " > " + f.name;

        if (std::find(f.presetIds.begin(), f.presetIds.end(), presetId) != f.presetIds.end())
        {
            return path;
        }

        if (auto inner = FolderPath(f.children, presetId, path); !inner.empty())
        {
            return inner;
        }
    }

    return {};
}
} // namespace

std::vector<std::pair<const PresetFolder*, int>> FlattenPresetFolders(const std::vector<PresetFolder>& folders)
{
    std::vector<std::pair<const PresetFolder*, int>> out;
    Flatten(folders, 0, out);
    return out;
}

std::string PresetFolderPath(const std::vector<PresetFolder>& folders, const std::string& presetId)
{
    return FolderPath(folders, presetId, {});
}

const Setlist* ActiveSetlist(const ClientState& state)
{
    for (const auto& s : state.setlists)
    {
        if (s.id == state.activeSetlistId)
        {
            return &s;
        }
    }

    return state.setlists.empty() ? nullptr : &state.setlists.front();
}

int NextSetlistBank(const std::vector<Setlist>& setlists)
{
    int highest = 0;

    for (const auto& s : setlists)
    {
        if (s.bank)
        {
            highest = std::max(highest, *s.bank);
        }
    }

    return highest + 1;
}

std::string SetlistBankLabel(const Setlist* setlist)
{
    if (setlist == nullptr)
    {
        return "No bank";
    }

    return setlist->bank ? "Bank " + std::to_string(*setlist->bank) : std::string("Unassigned bank");
}

int PerformancePadCount(const ClientState& state)
{
    const auto it = state.appSettings.find(kPerformancePadCountSetting);

    if (it != state.appSettings.end() && it->is_number())
    {
        const int count = it->get<int>();

        if (count == 4 || count == 6 || count == 8)
        {
            return count;
        }
    }

    return 8;
}

int PresetRating(const ClientState& state, const std::string& presetId)
{
    const auto it = state.presetRatings.find(presetId);
    return it != state.presetRatings.end() && it->is_number() ? std::clamp(it->get<int>(), 0, 5) : 0;
}

bool IsUserPresetId(const std::string& presetId)
{
    if (presetId.empty())
    {
        return false;
    }

    if (std::all_of(presetId.begin(), presetId.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) ||
        presetId.rfind("user-", 0) == 0)
    {
        return true;
    }

    // A UUID, version 1 to 5 and the RFC 4122 variant: 8-4-4-4-12 hex digits.
    if (presetId.size() != 36)
    {
        return false;
    }

    for (std::size_t i = 0; i < presetId.size(); ++i)
    {
        const char c = presetId[i];

        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (c != '-')
            {
                return false;
            }
        }
        else if (std::isxdigit(static_cast<unsigned char>(c)) == 0)
        {
            return false;
        }
    }

    const char version = presetId[14];
    const char variant = static_cast<char>(std::tolower(static_cast<unsigned char>(presetId[19])));
    return version >= '1' && version <= '5' && (variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
}

bool CanModifyActivePreset(const ClientState& state)
{
    const bool held = !state.activePresetId.empty() &&
                      (state.FindPresetSummary(state.activePresetId) != nullptr || state.activePreset.has_value());

    if (held && IsUserPresetId(state.activePresetId))
    {
        return true;
    }

    return IsFeatureEnabled(state, Feature::FactoryPresetArchives);
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

    // Factory presets switched off (Settings > Features) are left out of every list.
    if (!IsFeatureEnabled(state, Feature::FactoryPresetPacks))
    {
        base.erase(
            std::remove_if(base.begin(), base.end(), [](const PresetSummary& p) { return p.source == "factory"; }),
            base.end());
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

    // Every tag chip that is on must be on the preset.
    if (!query.tags.empty())
    {
        std::erase_if(base, [&query](const PresetSummary& p) {
            return !std::all_of(query.tags.begin(), query.tags.end(), [&p](const std::string& tag) {
                return std::find(p.tags.begin(), p.tags.end(), tag) != p.tags.end();
            });
        });
    }

    const auto text = Lower(Trim(query.text));

    if (!text.empty())
    {
        std::erase_if(base, [&text](const PresetSummary& p) {
            return Lower(p.name).find(text) == std::string::npos && Lower(p.category).find(text) == std::string::npos &&
                   Lower(p.description).find(text) == std::string::npos;
        });
    }

    if (!recents)
    {
        std::stable_sort(base.begin(), base.end(),
                         [](const PresetSummary& a, const PresetSummary& b) { return ComparePresetNames(a, b) < 0; });
    }

    return base;
}

std::vector<std::string> CompositeSlotNames(const ClientState& state, const CompositePreset& rig)
{
    std::vector<std::string> names;

    for (const auto& id : rig.presetIds)
    {
        const auto* summary = state.FindPresetSummary(id);
        names.push_back(summary != nullptr ? summary->name : id);
    }

    return names;
}

std::vector<const CompositePreset*> FilterCompositePresets(const ClientState& state, const std::string& text)
{
    const auto needle = Lower(Trim(text));
    std::vector<const CompositePreset*> kept;

    for (const auto& rig : state.mixer.compositePresets)
    {
        if (!needle.empty())
        {
            std::string haystack = rig.name + "\n" + rig.description;

            for (const auto& tag : rig.tags)
            {
                haystack += "\n" + tag;
            }

            for (const auto& name : CompositeSlotNames(state, rig))
            {
                haystack += "\n" + name;
            }

            if (Lower(haystack).find(needle) == std::string::npos)
            {
                continue;
            }
        }

        kept.push_back(&rig);
    }

    return kept;
}

std::vector<std::string> CompositePresetTags(const std::vector<CompositePreset>& rigs)
{
    std::set<std::string> tags;

    for (const auto& rig : rigs)
    {
        for (const auto& tag : rig.tags)
        {
            if (auto t = Lower(Trim(tag)); !t.empty())
            {
                tags.insert(std::move(t));
            }
        }
    }

    return {tags.begin(), tags.end()};
}
} // namespace guitarfx::uiclient
