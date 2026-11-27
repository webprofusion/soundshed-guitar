// The effect page's custom layouts: the layout an effect resolves to (the web UI's
// resolveLayoutForNode), drawn in the controls section and bound to the node (LayoutView.cpp),
// and the header's chooser between the standard controls and the layouts it can wear
// (signalPath/layoutSwitch.ts). Which layout is the shared client's decision
// (core/src/uiclient/EffectLayouts.h); the choice is the same app setting the web UI writes.

#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"
#include "ui/dialogs/DialogChrome.h"
#include "uiclient/ExposedResources.h"
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
                             float availableHeight, Ptr defaultControls, el::extent controlsSize, float& height,
                             float controlsFit)
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

        OpenPrompt(param.name, EditableParamText(param, NodeParamValue(*n, param)), "Set", {.placeholder = "Value"},
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
            OpenNodeParamMidiMenu(*n, param.key, at);
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
    input.defaultControlsFit = controlsFit;
    // A model or IR slot the layout places is the editor's own slot row, put where the layout
    // says: the effect's slots by index ("__resource__:primary:<index>"), two for the IR Cabinet,
    // or a composite's or Custom Effect's by the slot's id ("__resource__:<id>:<index>").
    input.resourceSlot = [this, &node](const LayoutResourceSlot& slot, const std::string& label, float) -> Ptr {
        const auto* type = mState.FindEffectType(node.type);

        for (const auto& exposed : NodeExposedResources(mState, node))
        {
            if (exposed.resourceId == slot.resourceType && exposed.resourceType != "plugin")
            {
                return BuildResourceSlot(
                    node, exposed.resourceType, exposed.resourceIndex,
                    label.empty() ? (exposed.displayName.empty() ? exposed.resourceId : exposed.displayName) : label,
                    false, false, exposed.allowBrowseFile);
            }
        }

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
    chip->AccessName("Effect layout");
    chip->onClick = [this, nodeId = node.id] { OpenLayoutPicker({.nodeId = nodeId}); };
    return chip;
}

void Shell::OpenLayoutPicker(LayoutPickerDraft draft)
{
    const auto* node = SelectedNode();

    if (node == nullptr || node->id != draft.nodeId)
    {
        return;
    }

    const auto blendId = BlendIdOf(*node);
    const auto entries = AvailableLayoutEntries(mState.layouts.library, node->type, blendId);
    const auto context = LayoutContextForNode(mState, *node);
    const auto keys = LayoutLookupKeys(node->type, blendId);
    const auto selection =
        ResolveLayoutSelection(&mState.layouts.library, ParseLayoutPreferenceRules(mState.appSettings), context);
    const auto rules = LayoutRulesForKeys(mState.appSettings, keys);
    const auto suggestions = SuggestLayoutKeywords(context.matchText);

    if (draft.layoutId.empty())
    {
        const bool custom = selection.layoutId && *selection.layoutId != kStandardLayoutId &&
                            std::any_of(entries.begin(), entries.end(),
                                        [&](const auto* e) { return e->layoutId == *selection.layoutId; });
        draft.layoutId = custom ? *selection.layoutId : std::string(kStandardLayoutId);
    }

    if (!draft.keywordTyped && draft.keyword.empty() && !suggestions.empty())
    {
        draft.keyword = suggestions.front();
    }

    const auto layoutName = [&](const std::string& id) -> std::string {
        if (id == kStandardLayoutId)
        {
            return "Standard controls";
        }

        for (const auto* e : entries)
        {
            if (e->layoutId == id)
            {
                return e->layout.name.empty() ? std::string("(Unnamed layout)") : e->layout.name;
            }
        }

        return "(missing layout)";
    };
    const auto note = [this](const std::string& text, float width) {
        auto p = std::make_shared<Paragraph>(mTheme, text, width, 12.0f);
        p->Colour(mTheme.TextSecondary());
        return p;
    };
    const auto heading = [this](const std::string& text) {
        auto t = std::make_shared<Text>(mTheme, text, 10.5f, Weight::SemiBold);
        t->Colour(mTheme.TextMuted()).Tracking(0.08f);
        return FixedHeight(20, t);
    };
    // A change reopens the dialog with it; posted, so the control that asked is not freed under it.
    const auto reopen = [this](LayoutPickerDraft next) {
        mSession.View().post([this, next] { OpenLayoutPicker(next); });
    };
    constexpr float kWidth = 440.0f;
    const float inner = kWidth - 40.0f;

    auto tabs = std::make_shared<Segmented>(
        mTheme,
        std::vector<std::string>{"Layout",
                                 rules.empty() ? std::string("Rules") : "Rules (" + std::to_string(rules.size()) + ")"},
        96.0f);
    tabs->AccessName("Effect layout sections");
    tabs->Select(draft.rulesTab ? 1 : 0);
    std::vector<Ptr> rows{FixedHeight(32, Row({tabs, Stretch()})), VGap(14)};
    std::shared_ptr<el::basic_input_box> keywordBox;

    if (!draft.rulesTab)
    {
        rows.push_back(FixedHeight(20, note(LayoutSourceNote(selection), inner)));
        rows.push_back(VGap(10));
        rows.push_back(heading("LAYOUT"));
        auto picker = std::make_shared<PickerField>(mTheme, mIcons);
        picker->Set(layoutName(draft.layoutId));
        picker->AccessName("Layout");
        picker->onClick = [this, draft, reopen, entries, weak = std::weak_ptr<PickerField>(picker)] {
            const auto field = weak.lock();

            if (!field)
            {
                return;
            }

            const auto choose = [draft, reopen](const std::string& id) {
                return [draft, reopen, id] {
                    auto next = draft;
                    next.layoutId = id;
                    reopen(next);
                };
            };
            std::vector<MenuItem> items{
                {"Standard controls", draft.layoutId == kStandardLayoutId, false, choose(kStandardLayoutId)}};

            for (const auto* e : entries)
            {
                const auto& layout = e->layout;
                items.push_back({layout.name.empty() ? std::string("(Unnamed layout)") : layout.name,
                                 e->layoutId == draft.layoutId, false, choose(e->layoutId),
                                 e->isFactory ? "Factory" : ""});
            }

            OpenDropdown(field->Bounds(), std::move(items), 380.0f);
        };
        rows.push_back(FixedHeight(34, picker));
        rows.push_back(VGap(14));
        rows.push_back(heading("REMEMBER THIS FOR"));

        const auto scopeOption = [&](const std::string& scope, const std::string& label, bool enabled) {
            auto option = std::make_shared<CheckBox>(mTheme, label, 12.5f);
            option->Radio(true).Enabled(enabled);
            option->Set(draft.scope == scope);
            option->onClick = [draft, reopen, scope, enabled] {
                if (enabled && draft.scope != scope)
                {
                    auto next = draft;
                    next.scope = scope;
                    reopen(next);
                }
            };
            return FixedHeight(26, option);
        };
        rows.push_back(scopeOption("effectType", "Every use of this effect", true));
        const bool hasPreset = !context.presetId.empty();
        const std::string presetName =
            mState.activePreset && !mState.activePreset->name.empty() ? mState.activePreset->name : context.presetId;
        rows.push_back(scopeOption("preset",
                                   hasPreset ? "Only the preset \"" + presetName + "\""
                                             : std::string("Only this preset (save the preset first)"),
                                   hasPreset));
        rows.push_back(scopeOption("keyword", "Amps/FX matching a keyword", true));
        auto [keywordElement, box] = el::input_box("e.g. twin, ac30, rectifier", Font(Weight::Regular), 1.0f);
        keywordBox = box;
        box->set_text(draft.keyword);
        rows.push_back(VGap(4));
        rows.push_back(
            FixedHeight(34, Rounded(mTheme.Tertiary(), 6.0f, Pad(8, 0, 8, 0, el::share(std::move(keywordElement))))));

        if (!suggestions.empty())
        {
            std::vector<Ptr> chips;

            for (const auto& keyword : suggestions)
            {
                auto chip = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Chip);
                chip->Size(std::max(44.0f, 7.5f * static_cast<float>(keyword.size()) + 18.0f), 24)
                    .Label(keyword, 11.0f);
                chip->onClick = [draft, reopen, keyword] {
                    auto next = draft;
                    next.scope = "keyword";
                    next.keyword = keyword;
                    next.keywordTyped = true;
                    reopen(next);
                };
                chips.push_back(chip);
                chips.push_back(HGap(6));
            }

            chips.push_back(Stretch());
            rows.push_back(VGap(6));
            rows.push_back(FixedHeight(24, Row(chips)));
        }
    }
    else if (rules.empty())
    {
        rows.push_back(FixedHeight(20, note("No saved rules for this effect yet.", inner)));
    }
    else
    {
        rows.push_back(
            FixedHeight(20, note("Most specific rule wins: preset, then keyword, then effect type.", inner)));
        rows.push_back(VGap(8));
        std::vector<Ptr> list;

        for (const auto& rule : rules)
        {
            auto scope = std::make_shared<Text>(mTheme, LayoutRuleScopeLabel(rule), 12.5f, Weight::SemiBold);
            scope->Colour(mTheme.ModalText());
            auto target = std::make_shared<Text>(mTheme, "\xE2\x86\x92 " + layoutName(rule.layoutId), 12.0f);
            target->Colour(mTheme.TextSecondary());
            auto remove = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            remove->Size(28, 28).Label("\xC3\x97", 13.0f);
            remove->AccessName("Delete rule");
            remove->Tooltip("Delete rule");
            remove->onClick = [this, draft, reopen, id = rule.id] {
                mSession.Commands().SetSetting(kLayoutPreferencesSetting,
                                               RulesWithoutLayoutPreference(mState.appSettings, id));
                auto next = draft;
                next.rulesTab = true;
                reopen(next);
            };
            list.push_back(FixedHeight(
                40, Row({Stretch(Col({FixedHeight(20, scope), FixedHeight(18, target)})), HGap(8), Middle(remove)})));
            list.push_back(VGap(4));
        }

        rows.push_back(Stretch(VScroll(Col(list))));
    }

    tabs->onSelect = [draft, reopen, weakBox = std::weak_ptr<el::basic_input_box>(keywordBox)](int index) {
        auto next = draft;
        next.rulesTab = index == 1;

        if (const auto box = weakBox.lock())
        {
            next.keyword = box->get_utf8();
            next.keywordTyped = true;
        }

        reopen(next);
    };

    auto footer = std::make_shared<Text>(mTheme, "New layouts are designed in Soundshed Guitar.", 11.0f);
    footer->Colour(mTheme.TextMuted());
    std::vector<Ptr> actions;

    if (!draft.rulesTab)
    {
        auto apply = DialogButton("Apply", {}, true);
        apply->OnClick([this, draft, layoutName, weakBox = std::weak_ptr<el::basic_input_box>(keywordBox)] {
            const auto* target = SelectedNode();

            if (target == nullptr || target->id != draft.nodeId)
            {
                CloseOverlay();
                return;
            }

            const auto box = weakBox.lock();
            std::string keyword = box ? box->get_utf8() : draft.keyword;
            const auto first = keyword.find_first_not_of(" \t");
            keyword = first == std::string::npos ? std::string() : keyword.substr(first);

            if (draft.scope == "keyword" && keyword.empty())
            {
                ShowToast("Enter a make or model keyword to match", "", true);
                return;
            }

            const auto targetContext = LayoutContextForNode(mState, *target);

            if (draft.scope == "preset" && targetContext.presetId.empty())
            {
                ShowToast("Save the preset before pinning a layout to it", "", true);
                return;
            }

            LayoutPreferenceRule rule;
            rule.lookupKey = LayoutLookupKeys(targetContext.effectType, targetContext.blendId).front();
            rule.scope = draft.scope;
            rule.layoutId = draft.layoutId;
            rule.keyword = keyword;
            rule.presetId = targetContext.presetId;
            rule.presetName = mState.activePreset ? mState.activePreset->name : std::string();
            mSession.Commands().SetSetting(kLayoutPreferencesSetting,
                                           RulesWithLayoutPreference(mState.appSettings, rule, NowIso()));
            ShowToast("Layout set to " + layoutName(draft.layoutId), "", false);
            CloseOverlay();
        });
        actions.push_back(apply);
    }

    ShowDialog({.title = "Effect layout",
                .body = Col(rows),
                .actions = actions,
                .footerLeft = footer,
                .headerExtras = {},
                .width = kWidth,
                .height = draft.rulesTab ? (rules.empty() ? 230.0f : 420.0f) : (suggestions.empty() ? 430.0f : 460.0f),
                .stretch = draft.rulesTab && !rules.empty()});
}
} // namespace nanoq::ui
