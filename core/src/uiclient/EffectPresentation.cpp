/**
 * EffectPresentation.cpp - The shared effect presentation table (see EffectPresentation.h).
 */

#include "uiclient/EffectPresentation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace guitarfx::uiclient
{
namespace
{
Argb Pack(int r, int g, int b, double a)
{
    const auto clampByte = [](int v) { return static_cast<Argb>(std::clamp(v, 0, 255)); };
    const auto alpha = static_cast<Argb>(std::clamp(static_cast<int>(std::lround(a * 255.0)), 0, 255));
    return (alpha << 24) | (clampByte(r) << 16) | (clampByte(g) << 8) | clampByte(b);
}

int HexDigit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }

    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
} // namespace

std::optional<Argb> ParseCssColour(const std::string& raw)
{
    std::string text;

    for (const char c : raw)
    {
        if (!std::isspace(static_cast<unsigned char>(c)))
        {
            text.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }

    if (text.size() == 4 || text.size() == 7)
    {
        if (text.front() != '#')
        {
            return std::nullopt;
        }

        int values[3] = {};
        const bool shortForm = text.size() == 4;

        for (int i = 0; i < 3; ++i)
        {
            const int hi = HexDigit(text[static_cast<std::size_t>(shortForm ? 1 + i : 1 + i * 2)]);
            const int lo = HexDigit(text[static_cast<std::size_t>(shortForm ? 1 + i : 2 + i * 2)]);

            if (hi < 0 || lo < 0)
            {
                return std::nullopt;
            }

            values[i] = hi * 16 + lo;
        }

        return Pack(values[0], values[1], values[2], 1.0);
    }

    const auto open = text.find('(');
    const auto close = text.find(')');

    if ((text.rfind("rgba(", 0) != 0 && text.rfind("rgb(", 0) != 0) || open == std::string::npos ||
        close == std::string::npos)
    {
        return std::nullopt;
    }

    std::stringstream parts(text.substr(open + 1, close - open - 1));
    std::string part;
    std::vector<double> numbers;

    while (std::getline(parts, part, ','))
    {
        try
        {
            numbers.push_back(std::stod(part));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    if (numbers.size() < 3)
    {
        return std::nullopt;
    }

    return Pack(static_cast<int>(numbers[0]), static_cast<int>(numbers[1]), static_cast<int>(numbers[2]),
                numbers.size() > 3 ? numbers[3] : 1.0);
}

EffectPresentation::EffectPresentation()
{
    // Enough to draw something sensible when the table cannot be read.
    mCategoryOrder = {"amp", "cab", "drive", "dynamics", "eq", "modulation", "pitch", "delay", "reverb", "synth", "utility"};
}

EffectPresentation EffectPresentation::Load(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);

    if (!stream)
    {
        return {};
    }

    const auto document = nlohmann::json::parse(stream, nullptr, false);
    return document.is_object() ? FromJson(document) : EffectPresentation{};
}

EffectPresentation EffectPresentation::FromJson(const nlohmann::json& document)
{
    EffectPresentation result;
    result.mLoaded = true;
    result.mDefaultIcon = document.value("defaultIcon", result.mDefaultIcon);
    result.mDefaultNodeClass = document.value("defaultNodeClass", result.mDefaultNodeClass);

    if (document.contains("categoryOrder") && document["categoryOrder"].is_array())
    {
        result.mCategoryOrder.clear();

        for (const auto& id : document["categoryOrder"])
        {
            if (id.is_string())
            {
                result.mCategoryOrder.push_back(id.get<std::string>());
            }
        }
    }

    const auto readStringMap = [&document](const char* key, std::map<std::string, std::string>& target) {
        if (document.contains(key) && document[key].is_object())
        {
            for (const auto& [from, to] : document[key].items())
            {
                if (to.is_string())
                {
                    target[from] = to.get<std::string>();
                }
            }
        }
    };

    readStringMap("nodeCategoryAliases", result.mNodeCategoryAliases);
    readStringMap("nodeClasses", result.mNodeClasses);
    readStringMap("legacyEffectEquipmentImages", result.mLegacyImages);

    if (document.contains("categories") && document["categories"].is_object())
    {
        for (const auto& [id, entry] : document["categories"].items())
        {
            if (!entry.is_object())
            {
                continue;
            }

            CategoryPresentation category;
            category.id = id;
            category.name = entry.value("name", id);
            category.colour = ParseCssColour(entry.value("color", "#808080")).value_or(0xff808080);
            category.icon = entry.value("icon", "");
            category.equipmentImage = entry.value("equipmentImage", "");

            if (entry.contains("visualBackground") && entry["visualBackground"].is_array() &&
                entry["visualBackground"].size() == 2)
            {
                const auto from = ParseCssColour(entry["visualBackground"][0].get<std::string>());
                const auto to = ParseCssColour(entry["visualBackground"][1].get<std::string>());

                if (from && to)
                {
                    category.visualBackground = std::make_pair(*from, *to);
                }
            }

            result.mCategories[id] = std::move(category);
        }
    }

    if (document.contains("effects") && document["effects"].is_object())
    {
        for (const auto& [name, entry] : document["effects"].items())
        {
            if (!entry.is_object() || !entry.contains("guid"))
            {
                continue;
            }

            result.mEffectsByGuid[entry.value("guid", "")] =
                EffectEntry{entry.value("icon", ""), entry.value("equipmentImage", "")};
        }
    }

    return result;
}

std::string EffectPresentation::AliasCategory(const std::string& category) const
{
    const auto it = mNodeCategoryAliases.find(category);
    return it == mNodeCategoryAliases.end() ? category : it->second;
}

std::string EffectPresentation::CategoryIcon(const std::string& category) const
{
    if (const auto it = mCategories.find(category); it != mCategories.end() && !it->second.icon.empty())
    {
        return it->second.icon;
    }

    return mDefaultIcon;
}

std::string EffectPresentation::IconFor(const std::string& effectType, const std::string& category) const
{
    if (const auto it = mEffectsByGuid.find(effectType); it != mEffectsByGuid.end() && !it->second.icon.empty())
    {
        return it->second.icon;
    }

    return CategoryIcon(category);
}

const CategoryPresentation* EffectPresentation::Category(const std::string& category) const
{
    const auto it = mCategories.find(AliasCategory(category));
    return it == mCategories.end() ? nullptr : &it->second;
}

Argb EffectPresentation::CategoryColour(const std::string& category) const
{
    if (const auto* entry = Category(category))
    {
        return entry->colour;
    }

    return 0xff808080;
}

std::string EffectPresentation::NodeClass(const std::string& category) const
{
    const auto it = mNodeClasses.find(category);
    return it == mNodeClasses.end() ? mDefaultNodeClass : it->second;
}

std::string EffectPresentation::EquipmentImage(const std::string& effectType, const std::string& category) const
{
    if (const auto it = mEffectsByGuid.find(effectType); it != mEffectsByGuid.end() && !it->second.equipmentImage.empty())
    {
        return it->second.equipmentImage;
    }

    if (const auto it = mLegacyImages.find(effectType); it != mLegacyImages.end())
    {
        return it->second;
    }

    if (const auto* entry = Category(category))
    {
        return entry->equipmentImage;
    }

    return {};
}
} // namespace guitarfx::uiclient
