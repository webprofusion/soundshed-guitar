// The effect page's custom layouts: the layout an effect resolves to (the web UI's
// resolveLayoutForNode), drawn in the controls section and bound to the node (LayoutView.cpp),
// and the header's chooser between the standard controls and the layouts it can wear
// (signalPath/layoutSwitch.ts). Which layout is the shared client's decision
// (core/src/uiclient/EffectLayouts.h); the choice is the same app setting the web UI writes.

#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/ParamFormat.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

namespace
{
/// Now, as the web UI's `new Date().toISOString()` writes it.
std::string NowIso()
{
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char text[40];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(millis));
    return text;
}

std::string BlendIdOf(const guitarfx::GraphNode& node)
{
    const auto it = node.config.find("blendId");
    return it != node.config.end() ? it->second : std::string();
}
} // namespace

Ptr Shell::BuildEffectLayout(const guitarfx::GraphNode& node, const EffectLayout& layout, float width,
                             float availableHeight, Ptr defaultControls, el::extent controlsSize, float& height)
{
    // The pictures' list is asked for the first time a layout draws, as the web UI asks; the
    // pictures themselves are read from their files as they are drawn.
    mSession.Client().RequestLayoutImages();

    const auto fit =
        ComputeLayoutScale(layout.dimensions.width, layout.dimensions.height, width, availableHeight, layout.scaling);
    const std::string nodeId = node.id;

    LayoutActions actions;
    actions.setParam = [this, nodeId](const EffectParamInfo& param, double value) {
        mSession.Commands().SetNodeParam(nodeId, param.key, value);
        NoteChainEdit();
        Mark(kValues);
    };
    actions.editValue = [this, nodeId](const EffectParamInfo& param) {
        const auto* n = SelectedNode();

        if (n == nullptr || n->id != nodeId)
        {
            return;
        }

        OpenPrompt(param.name, EditableParamText(param, NodeParamValue(*n, param)), "Set",
                   [this, nodeId, param](const std::string& entered) {
                       if (const auto typed = ParseTypedNumber(entered))
                       {
                           mSession.Commands().SetNodeParam(nodeId, param.key, SnapParamValue(param, *typed));
                           NoteChainEdit();
                           Mark(kValues);
                       }
                   });
    };
    actions.menu = [this](const EffectParamInfo& param, el::point at) {
        if (const auto* n = SelectedNode())
        {
            OpenNodeParamMidiMenu(n->type, param.key, at);
        }
    };
    // Read the node at the time of the click: the layout outlives the value it was built with.
    actions.toggleBypass = [this, nodeId] {
        const auto* n = SelectedNode();

        if (n != nullptr && n->id == nodeId)
        {
            mSession.Commands().SetNodeBypassed(nodeId, n->enabled);
        }
    };

    LayoutViewInput input;
    input.layout = &layout;
    input.node = &node;
    input.type = mState.FindEffectType(node.type);
    input.scale = fit.scale;
    input.defaultControls = std::move(defaultControls);
    input.defaultControlsSize = controlsSize;
    // A model or IR slot the layout places is the editor's own slot row, put where the layout
    // says: the effect's slots by index ("__resource__:primary:<index>"), two for the IR Cabinet.
    input.resourceSlot = [this, &node](const LayoutResourceSlot& slot, const std::string& label, float) -> Ptr {
        const auto* type = mState.FindEffectType(node.type);

        if (type == nullptr || type->resourceType.empty() || type->resourceType == "plugin")
        {
            return nullptr;
        }

        if (slot.resourceType != "primary" && slot.resourceType != type->resourceType)
        {
            return nullptr;
        }

        const bool cab = node.type == guitarfx::EffectGuids::kCabIr;

        if (slot.resourceIndex >= (cab ? 2 : 1))
        {
            return nullptr;
        }

        const auto& rt = type->resourceType;
        const std::string fallback = rt == "nam"  ? "Model"
                                     : rt == "ir" ? (cab ? (slot.resourceIndex == 0 ? "IR A" : "IR B") : "IR")
                                                  : "Resource";
        return BuildResourceSlot(node, rt, cab ? slot.resourceIndex : -1, label.empty() ? fallback : label,
                                 cab && slot.resourceIndex == 1);
    };

    auto parts = BuildLayoutView(mTheme, *mLayoutImages, input, actions);

    for (const auto& control : parts.controls)
    {
        mLive.Feed(control, [this, nodeId](LayoutControlView& c) {
            const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;
            return n != nullptr && c.Update(*n);
        });
    }

    height = parts.size.y;

    if (!parts.element)
    {
        return std::make_shared<el::element>();
    }

    Ptr drawn = FixedWidth(parts.size.x, FixedHeight(parts.size.y, parts.element));

    // Too wide at its smallest scale it scrolls; narrower than the section it sits in the middle,
    // unless it asks to start at the left (layoutOverlay.ts applyCustomLayoutScaling).
    if (fit.overflows)
    {
        return HScroll(drawn);
    }

    if (fit.centred)
    {
        return Row({Stretch(), drawn, Stretch()});
    }

    return Row({drawn, Stretch()});
}

Ptr Shell::BuildLayoutChooser(const guitarfx::GraphNode& node, bool usingLayout, float& width)
{
    width = 0.0f;
    const auto entries = AvailableLayoutEntries(mState.layouts.library, node.type, BlendIdOf(node));

    if (!mState.layouts.loaded || entries.empty())
    {
        return nullptr;
    }

    width = 84.0f;
    auto chip = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Chip);
    chip->Size(width, 26).Label(usingLayout ? "Layout" : "Standard", 11.0f).Caret(true);
    chip->onClick = [this, nodeId = node.id, c = chip.get()] {
        const auto* n = SelectedNode();

        if (n == nullptr || n->id != nodeId)
        {
            return;
        }

        const auto* current = ResolveLayoutForNode(mState, *n);
        auto choose = [this, nodeId](const std::string& layoutId) {
            const auto* target = SelectedNode();

            if (target == nullptr || target->id != nodeId)
            {
                return;
            }

            // Recorded where the web UI records it, so both UIs pick the same face for the effect.
            mSession.Commands().SetSetting(
                kLayoutPreferencesSetting,
                RulesWithLayoutChoice(mState.appSettings, mState, *target, layoutId, NowIso()));
        };
        std::vector<MenuItem> items;
        items.push_back({"Standard controls", current == nullptr, false, [choose] { choose(kStandardLayoutId); }});

        for (const auto* entry : AvailableLayoutEntries(mState.layouts.library, n->type, BlendIdOf(*n)))
        {
            const std::string name = entry->layout.name.empty() ? std::string("Untitled layout") : entry->layout.name;
            const bool chosen = current != nullptr && current == &entry->layout;
            items.push_back({name, chosen, false, [choose, id = entry->layoutId] { choose(id); },
                             entry->isFactory ? "Factory" : ""});
        }

        OpenDropdown(c->Bounds(), std::move(items), 200.0f);
    };
    return chip;
}
} // namespace nanoq::ui
