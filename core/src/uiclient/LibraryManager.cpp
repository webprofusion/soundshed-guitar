#include "uiclient/LibraryManager.h"

#include "uiclient/ResourceIndex.h"
#include "uiclient/UiCommands.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace guitarfx::uiclient::library_manager
{
namespace
{
std::string Lower(std::string text)
{
    for (auto& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

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

std::string Meta(const LibraryResource& resource, const char* key)
{
    const auto it = resource.metadata.find(key);
    return it != resource.metadata.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string Category(const LibraryResource& resource)
{
    return resource.category.empty() ? std::string("Uncategorized") : resource.category;
}

/// The one value of a set, or `many`.
std::string OneOr(const std::set<std::string>& values, const char* many)
{
    return values.size() == 1 ? *values.begin() : std::string(many);
}

void AddSorted(std::vector<std::string>& out, std::set<std::string> values)
{
    out.assign(values.begin(), values.end());
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
}
} // namespace

std::string Origin(const LibraryResource& resource)
{
    std::string provider = Meta(resource, "provider");

    if (provider.empty())
    {
        provider = Meta(resource, "archiveProvider");
    }

    if (provider.empty())
    {
        provider = Meta(resource, "source");
    }

    provider = Lower(Trim(provider));

    if (provider == "tone3000")
    {
        return "Tone3000";
    }

    static const std::set<std::string> imported{"presetarchive", "blendarchive", "factory-archives", "remote",
                                                "generatedpack", "zipimport",    "tonesharingapi",   "imported"};

    if (!provider.empty() && imported.contains(provider))
    {
        return "Imported";
    }

    if (resource.filePath.empty())
    {
        return "Unknown";
    }

    std::string path = Lower(resource.filePath);
    std::replace(path.begin(), path.end(), '\\', '/');
    const auto has = [&path](const char* part) { return path.find(part) != std::string::npos; };

    if (has("/content/tone3000/"))
    {
        return "Tone3000";
    }

    if (has("/content/presetarchive/") || has("/content/blendarchive/") || has("/content/factory-archives/") ||
        has("/content/remote/") || has("/imports/"))
    {
        return "Imported";
    }

    if (has("/settings/") || has("/appdata/") || has("/documents/"))
    {
        return "Local";
    }

    return "Built-in";
}

std::string TypeLabel(const std::string& type)
{
    if (type == "nam")
    {
        return "Amp Model";
    }

    if (type == "ir")
    {
        return "Cab IR";
    }

    std::string upper = type;

    for (auto& c : upper)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    return upper;
}

std::vector<Item> AllItems(const ClientState& state)
{
    std::vector<Item> items;
    const auto add = [&items](const std::string& type, const std::vector<LibraryResource>& resources) {
        for (const auto& r : resources)
        {
            items.push_back({&r, type});
        }
    };

    // Models, then IRs, then anything else the library holds.
    for (const char* first : {"nam", "ir"})
    {
        if (const auto it = state.resources.find(first); it != state.resources.end())
        {
            add(it->first, it->second);
        }
    }

    for (const auto& [type, resources] : state.resources)
    {
        if (type != "nam" && type != "ir")
        {
            add(type, resources);
        }
    }

    return items;
}

std::vector<Item> OfType(const std::vector<Item>& items, const std::string& type)
{
    if (type == "all")
    {
        return items;
    }

    std::vector<Item> kept;
    std::copy_if(items.begin(), items.end(), std::back_inserter(kept),
                 [&type](const Item& item) { return item.type == type; });
    return kept;
}

std::vector<std::string> Categories(const std::vector<Item>& items)
{
    std::set<std::string> values;

    for (const auto& item : items)
    {
        if (!Trim(item.resource->category).empty())
        {
            values.insert(item.resource->category);
        }
    }

    std::vector<std::string> out;
    AddSorted(out, std::move(values));
    return out;
}

std::vector<std::string> Creators(const std::vector<Item>& items)
{
    std::set<std::string> values;

    for (const auto& item : items)
    {
        if (const auto creator = ResourceCreator(*item.resource); !creator.empty())
        {
            values.insert(creator);
        }
    }

    std::vector<std::string> out;
    AddSorted(out, std::move(values));
    return out;
}

std::vector<Item> Filtered(const ClientState& state, const Filter& filter)
{
    const auto query = Lower(Trim(filter.query));
    const auto creator = Lower(Trim(filter.creator));
    std::vector<Item> kept;

    for (const auto& item : OfType(AllItems(state), filter.type))
    {
        const auto& r = *item.resource;

        if (filter.category != "all" && r.category != filter.category)
        {
            continue;
        }

        if (filter.source != "all" && Lower(Origin(r)) != filter.source)
        {
            continue;
        }

        if (filter.creator != "all" && Lower(ResourceCreator(r)) != creator)
        {
            continue;
        }

        if (!query.empty())
        {
            std::string haystack = r.name + " " + r.id + " " + r.category + " " + r.description + " " + r.filePath +
                                   " " + ResourceCreator(r);

            for (const auto& tag : ResourceTags(r))
            {
                haystack += " " + tag;
            }

            if (Lower(haystack).find(query) == std::string::npos)
            {
                continue;
            }
        }

        kept.push_back(item);
    }

    return kept;
}

std::vector<ToneGroup> GroupByTone(const std::vector<Item>& items)
{
    struct Building
    {
        ToneGroup group;
        std::set<std::string> types, categories, origins;
    };

    std::map<std::string, Building> groups;
    std::vector<std::string> order;

    for (const auto& item : items)
    {
        const auto& r = *item.resource;
        std::string id = Meta(r, "toneId");
        std::string title = Meta(r, "toneTitle");

        if (id.empty())
        {
            id = Meta(r, "groupId");
        }

        if (title.empty())
        {
            title = Meta(r, "groupName");
        }

        if (id.empty() || title.empty())
        {
            continue;
        }

        const std::string key = id + ":" + title;
        auto [it, added] = groups.try_emplace(key);

        if (added)
        {
            it->second.group.groupId = id;
            it->second.group.title = title;
            it->second.group.author = Meta(r, "authorUsername");
            order.push_back(key);
        }

        it->second.types.insert(TypeLabel(item.type));
        it->second.categories.insert(Category(r));
        it->second.origins.insert(Origin(r));
        it->second.group.items.push_back(item);
    }

    std::vector<ToneGroup> out;

    for (const auto& key : order)
    {
        auto& b = groups[key];
        b.group.typeLabel = OneOr(b.types, "Mixed");
        b.group.categoryLabel = OneOr(b.categories, "Multiple");
        b.group.originLabel = OneOr(b.origins, "Mixed");
        out.push_back(std::move(b.group));
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const ToneGroup& a, const ToneGroup& b) { return Lower(a.title) < Lower(b.title); });
    return out;
}

std::string RowMeta(const Item& item)
{
    const auto& r = *item.resource;
    std::string meta = TypeLabel(item.type) + " \xC2\xB7 " + Category(r) + " \xC2\xB7 " + Origin(r);

    if (const auto creator = ResourceCreator(r); !creator.empty())
    {
        meta += " \xC2\xB7 by " + creator;
    }

    for (const auto& tag : ResourceTags(r))
    {
        meta += " #" + tag;
    }

    return meta;
}

std::string GroupMeta(const ToneGroup& group)
{
    std::string meta = group.typeLabel + " \xC2\xB7 " + group.categoryLabel + " \xC2\xB7 " + group.originLabel +
                       " \xC2\xB7 " + std::to_string(group.items.size()) + " models";

    if (!group.author.empty())
    {
        meta += " \xC2\xB7 by " + group.author;
    }

    return meta;
}

std::string Summary(std::size_t shown, std::size_t total, bool grouped)
{
    return "Showing " + std::to_string(shown) + " of " + std::to_string(total) + " resources" +
           (grouped ? " (grouped)" : "");
}

std::string PathActionLabel(const Item& item)
{
    if (Origin(*item.resource) != "Local")
    {
        return {};
    }

    return item.resource->fileMissing ? "Set Path" : "Change Path";
}

bool CanEdit(const Item& item)
{
    return Origin(*item.resource) != "Built-in";
}

std::vector<Item> CleanupCandidates(const ClientState& state, const std::string& scope)
{
    std::vector<Item> out;

    for (const auto& item : OfType(AllItems(state), scope))
    {
        if (Origin(*item.resource) == "Imported")
        {
            out.push_back(item);
        }
    }

    return out;
}

void RemoveUnused(UiCommands& commands, const std::string& scope, const std::vector<Item>& items)
{
    std::vector<std::pair<std::string, std::string>> resources;

    for (const auto& item : items)
    {
        resources.emplace_back(item.type, item.resource->id);
    }

    commands.CleanupResourceLibrary(scope, resources, true);
}
} // namespace guitarfx::uiclient::library_manager
