#pragma once

/**
 * EffectPresentation.h - How effects and categories look: icon, colour, panel background and
 * stock artwork.
 *
 * One table for both UIs, core/ui/data/effect-presentation.json. Soundshed Guitar's web UI
 * reads it through a generated TypeScript module; Soundshed Guitar Nano reads the shipped
 * copy under resources/ui/data at startup. Keeping it in one file is what stops a new effect
 * getting an icon in one UI and a gear in the other.
 */

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
/// 0xAARRGGBB.
using Argb = std::uint32_t;

/// "#rgb", "#rrggbb" or "rgba(r, g, b, a)"/"rgb(r, g, b)"; nullopt for anything else.
[[nodiscard]] std::optional<Argb> ParseCssColour(const std::string& text);

struct CategoryPresentation
{
    std::string id;
    std::string name;
    Argb colour = 0xff808080;
    std::string icon;
    std::optional<std::pair<Argb, Argb>> visualBackground; // top-left to bottom-right
    std::string equipmentImage;                            // relative to the ui folder
};

class EffectPresentation
{
public:
    EffectPresentation();

    /// From the JSON document; anything missing keeps the built-in fallback.
    static EffectPresentation FromJson(const nlohmann::json& document);

    /// From the file; the built-in fallback if it is missing or unreadable.
    static EffectPresentation Load(const std::filesystem::path& file);

    /// The effect's own icon, else its category's, else the default ("gear"). Icon names are
    /// files in images/icons without the extension.
    [[nodiscard]] std::string IconFor(const std::string& effectType, const std::string& category) const;

    [[nodiscard]] std::string CategoryIcon(const std::string& category) const;

    /// The category (after the node-category aliases), or null for one the table lacks.
    [[nodiscard]] const CategoryPresentation* Category(const std::string& category) const;

    [[nodiscard]] Argb CategoryColour(const std::string& category) const;

    /// The chain node's colour class ("amp", "cab", ...; "utility" for anything else).
    [[nodiscard]] std::string NodeClass(const std::string& category) const;

    /// Stock artwork for an effect: its own, else its category's; empty when there is none.
    [[nodiscard]] std::string EquipmentImage(const std::string& effectType, const std::string& category) const;

    [[nodiscard]] const std::vector<std::string>& CategoryOrder() const
    {
        return mCategoryOrder;
    }

    [[nodiscard]] bool IsLoaded() const
    {
        return mLoaded;
    }

private:
    [[nodiscard]] std::string AliasCategory(const std::string& category) const;

    struct EffectEntry
    {
        std::string icon;
        std::string equipmentImage;
    };

    bool mLoaded = false;
    std::string mDefaultIcon = "gear";
    std::string mDefaultNodeClass = "utility";
    std::vector<std::string> mCategoryOrder;
    std::map<std::string, std::string> mNodeCategoryAliases;
    std::map<std::string, std::string> mNodeClasses;
    std::map<std::string, CategoryPresentation> mCategories;
    std::map<std::string, EffectEntry> mEffectsByGuid;
    std::map<std::string, std::string> mLegacyImages;
};
} // namespace guitarfx::uiclient
