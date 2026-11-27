#include "uiclient/FxLibrary.h"

#include "dsp/EffectGuids.h"
#include "uiclient/FxMarks.h"

namespace guitarfx::uiclient
{
std::string FxLibraryItem::Key() const
{
    return FxItemKey(type, blendId, customEffectId);
}

FxLibraryItem FxItemForEffect(const EffectTypeInfo& info)
{
    FxLibraryItem item;
    item.type = info.type;
    item.name = info.name;
    item.category = info.category;
    item.requiresResource = info.requiresResource;
    item.resourceType = info.resourceType;
    return item;
}

std::vector<FxLibraryItem> BlendFxItems(const ClientState& state, const EffectPresentation& look)
{
    std::vector<FxLibraryItem> items;

    for (const auto& blend : state.blends)
    {
        FxLibraryItem item;
        item.type = EffectGuids::kAmpNamBlend;
        item.name = blend.name.empty() ? std::string("Custom Blend") : blend.name;
        item.category = look.BlendCategory(blend.category);
        item.blendId = blend.id;
        item.requiresResource = true;
        item.resourceType = "nam";
        // As the web's fxItemNodeOptions: the blend, its name, and its gear as the node's category.
        item.options.config["blendId"] = blend.id;
        item.options.label = item.name;
        item.options.category = blend.category;
        items.push_back(std::move(item));
    }

    return items;
}

std::vector<FxLibraryItem> CustomEffectFxItems(const ClientState& state)
{
    std::vector<FxLibraryItem> items;

    for (const auto& entry : state.customEffects)
    {
        FxLibraryItem item;
        item.type = EffectGuids::kWasmHost;
        item.name = entry.name.empty() ? std::string("Custom Effect") : entry.name;
        item.category = entry.category.empty() ? std::string("utility") : entry.category;
        item.description = entry.description;
        item.thumbnailDataUrl = entry.thumbnailDataUrl;
        item.customEffectId = entry.id;
        // As the web's buildCustomEffectNodeOptions: the entry, its name, category, settings and
        // the module it runs.
        item.options.config["customEffectId"] = entry.id;
        item.options.label = entry.name;
        item.options.category = entry.category;
        item.options.params = entry.defaultParams;
        item.options.resources.push_back({entry.moduleResourceType, entry.moduleResourceId});
        items.push_back(std::move(item));
    }

    return items;
}
} // namespace guitarfx::uiclient
