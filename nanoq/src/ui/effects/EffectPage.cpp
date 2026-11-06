// The effect page: the selected node drawn as the web UI's effect view draws it at its compact
// density (core/ui/ts/signalPath/paramsPanel/panel.ts, css/signal-path/params-panel.css): a
// panel hanging from the preset bar, centred on the stage, with a header (the node's light, its
// name with a pencil to rename it, its category and effect, Presets and the on/off switch) over
// inset sections: the model or IR slots (NodeControls.cpp) with the Main / Advanced tabs beside
// them, a split's branch count, and the parameters in groups, each a knob, a switch or a list.
// A parameter the engine declares with `visibleWhen` shows only while that holds.

#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/EffectLayouts.h"
#include "uiclient/NodeLabels.h"
#include "uiclient/ParamFormat.h"
#include "uiclient/SplitRouting.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

namespace
{
namespace artist = cycfi::artist;

/// The one control the input and output nodes carry: their gain in dB, which the effect catalog
/// does not list since they are not effects.
EffectParamInfo BoundaryTrim()
{
    EffectParamInfo trim;
    trim.key = guitarfx::kBoundaryGainParam;
    trim.name = "Gain";
    trim.unit = "dB";
    trim.minValue = guitarfx::kBoundaryGainMinDb;
    trim.maxValue = guitarfx::kBoundaryGainMaxDb;
    trim.defaultValue = 0.0;
    return trim;
}

// The web UI's measurements: 60 px cells 13 apart in groups padded 13 at the sides, inside a
// section padded 8, inside the panel padded 10; the panel at least 448 wide.
constexpr float kCellWidth = 74.0f;
constexpr float kCellHeight = 72.0f;
constexpr float kPickerWidth = 120.0f;
constexpr float kCellGap = 4.0f;
constexpr float kGroupPadX = 11.0f;
constexpr float kGroupGap = 13.0f;
constexpr float kSectionPad = 14.0f;
constexpr float kPanelPad = 10.0f;
constexpr float kPanelMinWidth = 448.0f;
constexpr float kHeaderHeight = 36.0f;
constexpr float kStackedSlot = 16.0f + 6.0f + 26.0f; // a slot's label over its row

/// An enum shown as a list rather than turned through on a knob: the engine asks for it.
bool WantsPicker(const EffectParamInfo& info)
{
    return info.control == "picker";
}

/// The panel hanging from the preset bar (.default-effect-shell): a dark surface shading down
/// from its top, its category's colour faint along the top, a deep shadow under it.
class EffectPanel final : public el::element
{
  public:
    EffectPanel(const Theme& theme, el::color tint) : mTheme(theme), mTint(tint)
    {
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const bool light = mTheme.IsLight();
        {
            auto state = cnv.new_state();
            cnv.shadow_style(el::point{0.0f, 18.0f}, 28.0f, el::rgba(0, 0, 0, light ? 40 : 128));
            cnv.fill_style(light ? mTheme.Card() : el::rgba(10, 11, 14, 255));
            cnv.begin_path();
            cnv.add_round_rect(b, 0.0f);
            cnv.fill();
        }

        if (!light)
        {
            auto state = cnv.new_state();
            artist::canvas::linear_gradient face{b.top_left(), el::point{b.left + b.height() * 0.36f, b.bottom}};
            face.add_color_stop(0.0f, el::rgba(26, 28, 33, 245));
            face.add_color_stop(1.0f, el::rgba(10, 11, 14, 245));
            cnv.fill_style(face);
            cnv.fill_rect(b);

            artist::canvas::linear_gradient wash{b.top_left(), b.bottom_left()};
            wash.add_color_stop(0.0f, mTint.opacity(0.12f));
            wash.add_color_stop(std::min(1.0f, 60.0f / std::max(1.0f, b.height())), mTint.opacity(0.02f));
            wash.add_color_stop(1.0f, mTint.opacity(0.0f));
            cnv.fill_style(wash);
            cnv.fill_rect(b);
        }

        cnv.stroke_style(el::rgba(255, 255, 255, light ? 120 : 20));
        cnv.line_width(1.0f);
        cnv.begin_path();
        cnv.move_to(b.left, b.top + 0.5f);
        cnv.line_to(b.right, b.top + 0.5f);
        cnv.stroke();
        cnv.stroke_style(el::rgba(0, 0, 0, light ? 40 : 148));
        cnv.begin_path();
        cnv.move_to(b.left, b.bottom - 0.5f);
        cnv.line_to(b.right, b.bottom - 0.5f);
        cnv.stroke();
    }

  private:
    const Theme& mTheme;
    el::color mTint;
};

/// The stage behind the panel: the page colour, shaded darker under the panel.
class EffectBackdrop final : public el::element
{
  public:
    EffectBackdrop(const Theme& theme, float shadeTo) : mTheme(theme), mShadeTo(shadeTo)
    {
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.Background());
        cnv.fill_rect(b);

        if (mTheme.IsLight())
        {
            return;
        }

        auto state = cnv.new_state();
        artist::canvas::linear_gradient shade{b.top_left(), el::point{b.left, b.top + mShadeTo}};
        shade.add_color_stop(0.0f, el::rgba(0, 0, 0, 90));
        shade.add_color_stop(1.0f, el::rgba(0, 0, 0, 0));
        cnv.fill_style(shade);
        cnv.fill_rect(el::rect{b.left, b.top, b.right, std::min(b.bottom, b.top + mShadeTo)});
    }

  private:
    const Theme& mTheme;
    float mShadeTo;
};

/// The effect's artwork beside its controls (.default-effect-shell-equipment-panel): the loaded
/// model's own picture, read as it draws since it may still be downloading, else the stock
/// picture of its kind, the whole of it within a rounded panel.
class ArtworkPanel final : public el::element
{
  public:
    ArtworkPanel(const Theme& theme, std::function<std::shared_ptr<artist::image>()> picture,
                 std::shared_ptr<artist::image> stock)
        : mTheme(theme), mPicture(std::move(picture)), mStock(std::move(stock))
    {
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        auto image = mPicture ? mPicture() : nullptr;

        if (!image)
        {
            image = mStock;
        }

        auto state = cnv.new_state();
        cnv.begin_path();
        cnv.add_round_rect(b, 10.0f);
        cnv.clip();
        cnv.fill_style(mTheme.Pick(el::rgba(10, 12, 18, 255), mTheme.Card()));
        cnv.fill_rect(b);

        if (image)
        {
            DrawImageFitted(cnv, *image, b.inset(8.0f, 8.0f));
        }
    }

  private:
    const Theme& mTheme;
    std::function<std::shared_ptr<artist::image>()> mPicture;
    std::shared_ptr<artist::image> mStock;
};

/// A fixed-size box around `content`: a row of fixed children reports no maximum (Row()), so
/// the size is given rather than measured.
Ptr Sized(float w, float h, Ptr content)
{
    return FixedWidth(w, FixedHeight(h, std::move(content)));
}
} // namespace

std::string Shell::EffectPageSignature() const
{
    const auto* node = SelectedNode();

    if (node == nullptr)
    {
        return "none";
    }

    const float width = ViewSize().x;
    std::string s =
        node->id + "|" + node->type + "|" + NodeDisplayName(mState, *node) + "|" + (node->enabled ? "1" : "0") + "|" +
        (mShowAdvanced ? "adv" : "main") + "|" + std::to_string(static_cast<int>(width / 24.0f)) + "x" +
        std::to_string(static_cast<int>(ViewSize().y / 24.0f)) + "|" + NodeArchitectureBadge(mState, *node) + "|cm" +
        node->channelMode + (mState.appSettings.value("audio.signalPath.channelModeControl", false) ? "+" : "-");

    const auto* type = mState.FindEffectType(node->type);

    if (type != nullptr)
    {
        s += "|" + std::to_string(type->parameters.size()) + "|" + std::to_string(type->presets.size());

        // A control that comes and goes with another's value redraws the page when it does.
        for (const auto& param : type->parameters)
        {
            if (!param.visibleWhenKey.empty())
            {
                s += IsParamVisible(param, *node, type) ? "v" : "h";
            }
        }
    }

    // A blend node shows its blend and mode.
    for (const char* key : {"blendId", "blendModeOverride"})
    {
        if (const auto c = node->config.find(key); c != node->config.end())
        {
            s += std::string("|") + key + "=" + c->second;
        }
    }

    s += "|blends" + std::to_string(mState.blends.size());

    // The model and IR slots show what is loaded and what the library holds.
    for (const auto& ref : node->resources)
    {
        s += "|r" + ref.resourceType + ":" + ref.resourceId + ":" + guitarfx::util::PathToUtf8(ref.filePath);
    }

    if (type != nullptr && !type->resourceType.empty())
    {
        const auto it = mState.resources.find(type->resourceType);
        s += "|lib" + std::to_string(it != mState.resources.end() ? it->second.size() : 0);

        // A plugin host also shows whether it is loading and which plugins are starred.
        if (type->resourceType == "plugin")
        {
            s += std::string(mState.pluginLoadsPending.count(node->id) > 0 ? "|loading" : "") + "|fav" +
                 mState.appSettings.value("plugins.hostFavorites", nlohmann::json::array()).dump();
        }
    }

    if (std::find(mState.missingNodeResources.begin(), mState.missingNodeResources.end(), node->id) !=
        mState.missingNodeResources.end())
    {
        s += "|missing";
    }

    // The custom layout it resolves to (the rules are a setting), redrawn when the library or
    // its pictures arrive; and how many it could choose from, for the header's chooser.
    if (!IsBoundaryNode(*node))
    {
        if (const auto* layout = ResolveLayoutForNode(mState, *node))
        {
            s += "|lay" + layout->layoutId + ":" + layout->name + "#" + std::to_string(mState.layouts.revision);
        }

        const auto blend = node->config.find("blendId");
        s +=
            "|lays" + std::to_string(AvailableLayoutEntries(mState.layouts.library, node->type,
                                                            blend != node->config.end() ? blend->second : std::string())
                                         .size());
    }

    // A splitter's Branches control follows its edges.
    if (IsSplitterType(node->type) && mState.activePreset)
    {
        for (const auto& edge : mState.activePreset->graph.edges)
        {
            if (edge.from == node->id)
            {
                s += "|e" + edge.to;
            }
        }
    }

    return s;
}

Ptr Shell::BuildEffectPage()
{
    mEffectPlot.reset();
    mEffectDspStatus.reset();

    const auto* node = SelectedNode();

    if (node == nullptr)
    {
        auto hint = std::make_shared<Text>(mTheme, "Choose an effect in the chain to see its controls.", 13.0f);
        hint->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
        return Over(PadXY(24, 48, Col({FixedHeight(24, hint), std::make_shared<el::element>()})),
                    std::make_shared<EffectBackdrop>(mTheme, 0.0f));
    }

    const std::string nodeId = node->id;
    const bool boundary = IsBoundaryNode(*node);
    const auto* type = mState.FindEffectType(node->type);
    const auto category = NodeCategory(mState, *node);
    const auto tint = Theme::Argb(mPresentation.CategoryColour(category));
    const bool hasPlugin = !boundary && type != nullptr && type->resourceType == "plugin";
    const bool hasSlots = !boundary && type != nullptr && !type->resourceType.empty() &&
                          type->resourceType != "plugin" && node->type != guitarfx::EffectGuids::kAmpNamBlend;

    // A custom layout draws the effect in place of its groups of controls; a backdrop layout has
    // them drawn on it (panel.ts). A layout that places the model or IR slots takes them too.
    const auto* layout = boundary ? nullptr : ResolveLayoutForNode(mState, *node);
    const bool backdrop = layout != nullptr && layout->useDefaultControls;
    const bool customControls = layout != nullptr && !backdrop;
    const bool layoutPlacesSlots =
        customControls && std::any_of(layout->controls.begin(), layout->controls.end(),
                                      [](const LayoutControl& c) { return IsLayoutResourceControl(c); });

    // The full layout centres the editor under the chain, no wider than the web UI's panel.
    const float stageWidth = mWide ? std::min(StageWidth(), 900.0f) : StageWidth();
    const float stageHeight = mWide ? ViewSize().y - metrics::kTabBarHeight - metrics::kHeaderHeight -
                                          mChainAreaHeight - metrics::kWideFooterHeight
                                    : ViewSize().y - metrics::kTopBarHeight - metrics::kFooterHeight;

    // ── Which controls: this tab's, and only those whose condition holds ──
    std::vector<EffectParamInfo> params;
    bool anyAdvanced = false;

    if (boundary)
    {
        params.push_back(BoundaryTrim());
    }
    else if (type != nullptr && !customControls)
    {
        // A splitter shows only the split notes its branches use.
        const int branches = IsSplitterType(node->type) && mState.activePreset
                                 ? guitarfx::uiclient::CountSplitBranches(mState.activePreset->graph, node->id)
                                 : 0;

        for (const auto& param : type->parameters)
        {
            anyAdvanced = anyAdvanced || param.advanced;

            if (branches > 0 && !guitarfx::uiclient::IsSplitterParamInUse(param.key, branches))
            {
                continue;
            }

            if (param.advanced == mShowAdvanced && IsParamVisible(param, *node, type))
            {
                params.push_back(param);
            }
        }
    }

    if (!anyAdvanced)
    {
        mShowAdvanced = false;
    }

    // ── The groups: each a box of cells ──
    struct Group
    {
        std::string name;
        std::vector<std::pair<float, Ptr>> cells; // width, cell
    };

    std::vector<Group> groups;

    // The node's value of a parameter now, which the controls follow (automation, a preset recalled,
    // another editor). By id, not the selection, which may move on before this page is rebuilt.
    const auto valueOf = [this, nodeId](const EffectParamInfo& info) {
        const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;
        return n != nullptr ? NodeParamValue(*n, info) : info.defaultValue;
    };

    for (const auto& info : params)
    {
        // A group gathers every control of its name, where they are apart in the list too
        // (a NAM amp's Input and Output both in Level), as the web UI groups them.
        auto group =
            std::find_if(groups.begin(), groups.end(), [&info](const Group& g) { return g.name == info.group; });

        if (group == groups.end())
        {
            groups.push_back({info.group, {}});
            group = std::prev(groups.end());
        }

        auto& target = *group;
        // A right click on any of them: MIDI Learn, for whichever control it is (Automation.cpp).
        auto menu = [this, key = info.key](el::point at) {
            if (const auto* n = SelectedNode())
            {
                OpenNodeParamMidiMenu(n->type, key, at);
            }
        };

        if (IsEnumParam(info) && WantsPicker(info))
        {
            // A long list: its current choice on a chip; a tap opens the list.
            auto picker = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Chip);
            picker->Size(kPickerWidth, 26).Caret(true);
            picker->AccessName(info.name);
            picker->onClick = [this, nodeId, info] { OpenEnumPicker(nodeId, info); };
            picker->onMenu = menu;
            mLive.Bind(
                picker, [valueOf, info] { return EnumLabelIndex(info, valueOf(info)); },
                [labels = info.labels](IconButton& p, int index) {
                    p.Label(index >= 0 && static_cast<std::size_t>(index) < labels.size()
                                ? labels[static_cast<std::size_t>(index)]
                                : std::string(),
                            11.0f);
                });
            auto label = std::make_shared<Text>(mTheme, Upper(info.name), 9.0f, Weight::SemiBold);
            label->Colour(mTheme.Pick(el::rgba(201, 209, 225, 184), mTheme.TextSecondary()))
                .Alignment(Text::Align::Centre);
            target.cells.emplace_back(kPickerWidth,
                                      Sized(kPickerWidth, kCellHeight,
                                            Col({FixedHeight(12, label), VGap(12), FixedHeight(26, picker),
                                                 std::make_shared<el::element>()})));
        }
        else if (IsToggleParam(info))
        {
            auto toggle = std::make_shared<ParamToggle>(mTheme, info.name);
            toggle->onMenu = menu;
            mLive.Bind(
                toggle, [valueOf, info] { return valueOf(info) > 0.5 * (info.minValue + info.maxValue); },
                &ParamToggle::Set);
            toggle->onClick = [this, nodeId, info] {
                const auto* n = SelectedNode();

                if (n == nullptr)
                {
                    return;
                }

                const double now = NodeParamValue(*n, info);
                const double next = now > 0.5 * (info.minValue + info.maxValue) ? info.minValue : info.maxValue;
                mSession.Commands().SetNodeParam(nodeId, info.key, next);
                NoteChainEdit();
                Mark(kValues | kPage);
            };
            target.cells.emplace_back(kCellWidth, toggle);
        }
        else
        {
            // A knob: continuous values, and an enum short enough to turn through, as the web
            // UI's Mode knobs are.
            auto knob = std::make_shared<ParamKnob>(mTheme, info.name);
            knob->Large(true).DefaultPosition(ParamValueToPosition(info, info.defaultValue));
            knob->onMenu = menu;
            mLive.Bind(
                knob, [valueOf, info] { return valueOf(info); },
                [info](ParamKnob& k, double v) { k.Update(ParamValueToPosition(info, v), FormatParamValue(info, v)); });
            const bool stepped = IsEnumParam(info);

            if (stepped)
            {
                knob->KeySteps(static_cast<int>(info.labels.size()));
            }

            knob->onChange = [this, nodeId, info, stepped, k = knob.get()](double position) {
                const double value = SnapParamValue(info, ParamPositionToValue(info, position));
                k->SetValueText(FormatParamValue(info, value));
                mSession.Commands().SetNodeParam(nodeId, info.key, value);
                NoteChainEdit();
                // The sections drawn from the values (a curve, the IR alignment) follow the turn.
                Mark(kValues);

                if (stepped)
                {
                    Mark(kPage); // a visibleWhen on this enum may show or hide others
                }
            };
            knob->onReset = [this, nodeId, info] {
                mSession.Commands().SetNodeParam(nodeId, info.key, info.defaultValue);
                NoteChainEdit();
                Mark(kValues);
            };

            if (!stepped)
            {
                // A value typed in where its value text is, as the web UI's knob does.
                knob->editText = [this, info] {
                    const auto* n = SelectedNode();
                    return n != nullptr ? EditableParamText(info, NodeParamValue(*n, info)) : std::string();
                };
                knob->onTyped = [this, nodeId, info](const std::string& entered) {
                    const auto typed = ParseTypedNumber(entered);

                    if (!typed)
                    {
                        Mark(kValues); // the value text comes back as it was
                        return;
                    }

                    mSession.Commands().SetNodeParam(nodeId, info.key, SnapParamValue(info, *typed));
                    NoteChainEdit();
                    Mark(kValues);
                };
            }

            target.cells.emplace_back(kCellWidth, knob);
        }
    }

    // Beside named groups, the controls that have none are titled Other, as the web UI does.
    if (std::any_of(groups.begin(), groups.end(), [](const Group& g) { return !g.name.empty(); }))
    {
        for (auto& g : groups)
        {
            if (g.name.empty())
            {
                g.name = "Other";
            }
        }
    }

    // A group's box: its title over its cells, as wide as they are; an untitled group alone in
    // its row stretches across it with its cells centred (as the Input's Gain and the Doubler's).
    auto groupWidth = [](const Group& g) {
        float w = 2.0f * kGroupPadX;

        for (std::size_t i = 0; i < g.cells.size(); ++i)
        {
            w += g.cells[i].first + (i > 0 ? kCellGap : 0.0f);
        }

        return w;
    };
    // A titled group: 6 above its title, the 14 px title, 4, the cells, 8. An untitled one: 8
    // above the cells and 4 below.
    auto groupHeight = [](const Group& g) {
        return g.name.empty() ? 8.0f + kCellHeight + 4.0f : 6.0f + 14.0f + 4.0f + kCellHeight + 8.0f;
    };
    auto buildGroup = [&](const Group& g, bool stretch) -> Ptr {
        std::vector<Ptr> row;

        if (stretch)
        {
            row.push_back(Stretch());
        }

        for (std::size_t i = 0; i < g.cells.size(); ++i)
        {
            if (i > 0)
            {
                row.push_back(HGap(kCellGap));
            }

            row.push_back(FixedWidth(g.cells[i].first, g.cells[i].second));
        }

        row.push_back(Stretch());
        const float top = g.name.empty() ? 8.0f : 6.0f + 14.0f + 4.0f;
        return Over(Pad(kGroupPadX, top, kGroupPadX, g.name.empty() ? 4.0f : 8.0f, FixedHeight(kCellHeight, Row(row))),
                    std::make_shared<InsetBox>(mTheme, g.name));
    };

    // The panel's width: the model slots take the whole stage; otherwise as wide as its groups,
    // and never under the web UI's minimum.
    float groupsWidth = 0.0f;

    for (const auto& g : groups)
    {
        groupsWidth += groupWidth(g) + (groupsWidth > 0.0f ? kGroupGap : 0.0f);
    }

    // The full layout shows the effect's artwork in a column left of its controls; compact, as
    // in the web UI, leaves it to the chain card.
    std::shared_ptr<artist::image> stockArt;
    std::string artUrl;

    if (mWide && !boundary && type != nullptr && layout == nullptr)
    {
        stockArt = mImages.Find(mPresentation.EquipmentImage(node->type, category));

        for (const auto& ref : node->resources)
        {
            if ((ref.resourceType == "nam" || ref.resourceType == "ir") && !ref.resourceId.empty())
            {
                if (const auto* r = mState.FindLibraryResource(ref.resourceType, ref.resourceId);
                    r != nullptr && artUrl.empty())
                {
                    artUrl = r->metadata.value("imageUrl", std::string());
                }
            }
        }
    }

    const bool hasArt = stockArt != nullptr || !artUrl.empty();
    constexpr float kArtWidth = 300.0f, kArtGap = 12.0f, kArtMinHeight = 200.0f;
    const float artRoom = hasArt ? kArtWidth + kArtGap : 0.0f;

    const float maxPanel = std::max(kPanelMinWidth, stageWidth - 20.0f);
    // The live sections (the analyzer, the spatial panner) want the room too.
    const bool wideSection = !boundary && (node->type == guitarfx::EffectGuids::kInputAnalyzer ||
                                           node->type == guitarfx::EffectGuids::kSpatial3D);
    // A layout asks for its own width, or all there is when it may grow past it.
    const bool layoutGrows = layout != nullptr && layout->scaling && layout->scaling->mode != "fixed" &&
                             layout->scaling->maxScale && *layout->scaling->maxScale > 1.0;
    const float layoutWidth = layout != nullptr ? static_cast<float>(std::round(layout->dimensions.width)) : 0.0f;
    const float panelWidth =
        (hasSlots || hasPlugin || wideSection || layoutGrows)
            ? maxPanel
            : std::clamp(std::max(groupsWidth + artRoom, layoutWidth) + 2.0f * (kPanelPad + kSectionPad),
                         kPanelMinWidth, maxPanel);
    const float sectionInner = panelWidth - 2.0f * (kPanelPad + kSectionPad) - artRoom;

    // ── Header ──
    const auto titleFont = Font(Weight::SemiBold).size(12.5f);
    const std::string titleText = NodeDisplayName(mState, *node);
    std::string kind = boundary ? std::string(node->type == guitarfx::kNodeTypeInput ? "Input" : "Output")
                                : (type != nullptr ? type->name : node->type);
    // The web UI's subtitle reads "DYNAMICS · NOISE GATE": the category as the engine names it.
    std::string shownCategory =
        boundary ? std::string("utility") : (node->category.empty() ? mPresentation.AliasCategory(category) : category);
    std::replace_if(shownCategory.begin(), shownCategory.end(), [](char c) { return c == '-' || c == '_'; }, ' ');
    const auto subtitleText = Upper(shownCategory + " \xC2\xB7 " + kind);
    const auto badgeText = boundary ? std::string() : NodeArchitectureBadge(mState, *node);

    std::vector<Ptr> rightSide;
    float channelsWidth = 0.0f;

    // Which channels the effect takes, where Settings shows the choice (paramsPanel/channelMode.ts).
    if (!boundary && !IsSplitterType(node->type) && !IsMixerType(node->type) &&
        mState.appSettings.value("audio.signalPath.channelModeControl", false))
    {
        const auto& mode = node->channelMode;
        const char* label = mode == guitarfx::kChannelModeMono        ? "Mono sum"
                            : mode == guitarfx::kChannelModeMonoLeft  ? "Mono L"
                            : mode == guitarfx::kChannelModeMonoRight ? "Mono R"
                                                                      : "Follow";
        auto channels = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Chip);
        channels->Size(84, 26).Label(label, 11.0f).Caret(true);
        channels->onClick = [this, nodeId, mode, c = channels.get()] {
            std::vector<MenuItem> items;
            const std::pair<const char*, const char*> choices[] = {{"Follow the input", ""},
                                                                   {"Mono sum", guitarfx::kChannelModeMono},
                                                                   {"Mono left", guitarfx::kChannelModeMonoLeft},
                                                                   {"Mono right", guitarfx::kChannelModeMonoRight}};

            for (const auto& [text, value] : choices)
            {
                items.push_back({text, mode == value, false, [this, nodeId, v = std::string(value)] {
                                     mSession.Commands().SetNodeChannelMode(nodeId, v);
                                     NoteChainEdit();
                                 }});
            }

            OpenDropdown(c->Bounds(), std::move(items), 160.0f);
        };
        rightSide.push_back(channels);
        rightSide.push_back(HGap(8));
        channelsWidth = 84.0f + 8.0f;
    }

    // The standard controls or one of the effect's layouts, when it has layouts to choose from.
    if (!boundary)
    {
        float chooserWidth = 0.0f;

        if (auto chooser = BuildLayoutChooser(*node, layout != nullptr, chooserWidth))
        {
            rightSide.push_back(chooser);
            rightSide.push_back(HGap(8));
            channelsWidth += chooserWidth + 8.0f;
        }
    }

    // The meter shows the effect's status readout under the header, as the web UI's meter badge.
    if (!boundary)
    {
        // The meter icon (.default-effect-shell-dsp-toggle), lit while the readout shows.
        auto dsp = std::make_shared<IconButton>(mTheme, mIcons, "meter", IconButton::Look::Ghost);
        dsp->Size(28, 26).IconSize(16).Ink(mShowDspStatus ? mTheme.Accent() : mTheme.TextSecondary());
        dsp->onClick = [this] {
            mShowDspStatus = !mShowDspStatus;
            mEffectSignature.clear();
            Mark(kPage);
        };
        rightSide.push_back(dsp);
        rightSide.push_back(HGap(8));
        channelsWidth += 28.0f + 8.0f;
    }

    if (!IsSplitterType(node->type) && !IsMixerType(node->type))
    {
        if (type != nullptr || boundary)
        {
            auto presets = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Chip);
            presets->Size(77, 26).Label("Presets", 11.0f).Caret(true);
            presets->Disabled(boundary);
            presets->onClick = [this] { OpenEffectPresets(); };
            rightSide.push_back(presets);
            rightSide.push_back(HGap(8));
        }

        // A neural model whose every model carries its input and output calibration.
        if (!boundary && guitarfx::uiclient::NodeNamFullyCalibrated(mState, *node))
        {
            const std::string text = "CALIBRATED";
            rightSide.push_back(std::make_shared<ArchBadge>(text, 9.0f, 22.0f));
            rightSide.push_back(HGap(8));
            channelsWidth += MeasureTextWidth(text, Font(Weight::SemiBold).size(9.0f)) + 21.0f + 8.0f;
        }

        auto bypass = std::make_shared<PowerSwitch>(mTheme, node->enabled);
        bypass->Enabled(!boundary);
        // Read the node at the time of the tap: the switch outlives the value it was built with.
        bypass->onClick = [this, nodeId, boundary] {
            const auto* n = SelectedNode();

            if (boundary || n == nullptr || n->id != nodeId)
            {
                return;
            }

            mSession.Commands().SetNodeBypassed(nodeId, n->enabled);
        };
        mLive.Bind(
            bypass,
            [this, nodeId] {
                const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;
                return n != nullptr && n->enabled;
            },
            &PowerSwitch::Set);
        rightSide.push_back(bypass);
    }

    float rightWidth = 0.0f;

    if (!rightSide.empty())
    {
        rightWidth = channelsWidth + ((type != nullptr || boundary) ? 77.0f + 8.0f + 40.0f : 40.0f);
    }

    const float pencilWidth = boundary ? 0.0f : 22.0f;
    const float roomForTitle = panelWidth - 2.0f * kPanelPad - 14.0f - 8.0f - pencilWidth - rightWidth - 16.0f;
    const float titleWidth =
        std::clamp(MeasureTextWidth(titleText, titleFont) + 2.0f, 40.0f, std::max(40.0f, roomForTitle));

    auto led = std::make_shared<HeaderLed>(node->enabled, false);
    auto title = std::make_shared<Text>(mTheme, titleText, 12.5f, Weight::SemiBold);
    title->Colour(mTheme.Pick(el::rgba(238, 242, 255, 255), mTheme.Text()));
    auto subtitle = std::make_shared<Text>(mTheme, subtitleText, 9.0f, Weight::SemiBold);
    subtitle->Colour(mTheme.TextMuted()).Fixed(true);

    // The titles (.default-effect-shell-titles): the name, and under it the kind and the badge.
    std::vector<Ptr> titleRow{FixedWidth(titleWidth, title)};

    if (!boundary)
    {
        auto pencil = std::make_shared<IconButton>(mTheme, mIcons, "pencil");
        pencil->Size(20, 20).IconSize(11).Ink(mTheme.TextMuted());
        pencil->onClick = [this, nodeId] { RenameNodeWithPrompt(nodeId); };
        titleRow.push_back(HGap(2));
        titleRow.push_back(Middle(pencil));
    }

    titleRow.push_back(Stretch());
    std::vector<Ptr> subtitleRow{Middle(subtitle)};

    if (!badgeText.empty())
    {
        subtitleRow.push_back(HGap(6));
        subtitleRow.push_back(std::make_shared<ArchBadge>(badgeText));
    }

    subtitleRow.push_back(Stretch());
    std::vector<Ptr> header{Middle(led), HGap(8),
                            Col({FixedHeight(20, Row(titleRow)), FixedHeight(14, Row(subtitleRow)), VGap(2)})};

    for (auto& r : rightSide)
    {
        header.push_back(Middle(r));
    }

    // ── The controls section ──
    std::vector<Ptr> section;
    float sectionHeight = 0.0f;
    auto addToSection = [&](Ptr element, float height) {
        if (!section.empty())
        {
            section.push_back(VGap(12));
            sectionHeight += 12.0f;
        }

        section.push_back(FixedHeight(height, std::move(element)));
        sectionHeight += height;
    };

    auto tabs = [this, anyAdvanced]() -> Ptr {
        if (!anyAdvanced)
        {
            return nullptr;
        }

        auto tab = [this](const char* label, bool advanced) {
            auto t = std::make_shared<UnderlineTab>(mTheme, label, mShowAdvanced == advanced);
            t->onClick = [this, advanced] {
                mShowAdvanced = advanced;
                Mark(kPage);
            };
            return t;
        };
        auto t = std::make_shared<el::htile_composite>();
        t->push_back(tab("Main", false));
        t->push_back(HGap(4));
        t->push_back(tab("Advanced", true));
        return t;
    };

    if (mShowDspStatus && !boundary)
    {
        addToSection(BuildDspStatus(guitarfx::uiclient::EffectDspStatus(), mState.activePresetId + "::" + node->id,
                                    {"preset", node->id, mState.activePresetId}, false),
                     40.0f);
    }

    if (hasSlots && !layoutPlacesSlots)
    {
        const auto& rt = type->resourceType;

        if (node->type == guitarfx::EffectGuids::kCabIr)
        {
            addToSection(BuildResourceSlot(*node, rt, 0, "IR A", false, true), kStackedSlot);
            addToSection(BuildIrList(*node), mShowIrList ? 28.0f + 150.0f : 28.0f);
            addToSection(BuildResourceSlot(*node, rt, 1, "IR B", true, true), kStackedSlot);
        }
        else
        {
            addToSection(BuildResourceSlot(*node, rt, -1,
                                           rt == "nam"  ? "Model"
                                           : rt == "ir" ? "IR"
                                                        : "Resource",
                                           false, true),
                         kStackedSlot);
        }
    }

    if (IsSplitterType(node->type))
    {
        addToSection(BuildSplitBranches(*node), 96.0f);
    }

    if (hasPlugin)
    {
        addToSection(BuildPluginHost(*node), 28.0f + 8.0f + 168.0f);
    }

    if (node->type == guitarfx::EffectGuids::kAmpNamBlend)
    {
        addToSection(BuildBlendInfo(*node), 46.0f);
    }

    if (node->type == guitarfx::EffectGuids::kDynamicsGate)
    {
        addToSection(Over(Pad(10, 22, 10, 6, BuildGateLearn(NodeGateLearnTarget(*node))),
                          std::make_shared<InsetBox>(mTheme, "Noise Floor")),
                     22.0f + 60.0f + 6.0f);
    }

    if (node->type == guitarfx::EffectGuids::kMultibandGate)
    {
        auto target = MultibandGateLearnTarget(*node);
        const float height = GateLearnHeight(target);
        addToSection(Over(Pad(10, 22, 10, 6, BuildGateLearn(std::move(target))),
                          std::make_shared<InsetBox>(mTheme, "Noise Floor by Band")),
                     22.0f + height + 6.0f);
    }

    // The tabs (.node-param-tabs): on their own line, with a rule under them across the section.
    if (auto t = tabs())
    {
        addToSection(Col({FixedHeight(28, Row({t, Stretch()})),
                          FixedHeight(1, Fill(mTheme.Border(), std::make_shared<el::element>()))}),
                     29.0f);
    }

    // An EQ's curve over its input's spectrum, above its knobs, as the web UI shows it.
    if (!boundary && IsEqPlotType(node->type))
    {
        addToSection(MakeEqPlot(NodeEqPlotSource(*node), false), 170.0f);
    }

    if (node->type == guitarfx::EffectGuids::kCabSimple)
    {
        addToSection(BuildCabTools(*node), 28.0f);
    }

    if (node->type == guitarfx::EffectGuids::kPickupMatch)
    {
        addToSection(BuildPickupDesignLines(), 36.0f);
    }

    if (const float h = IrAlignmentHeight(*node); h > 0.0f && !boundary)
    {
        addToSection(BuildIrAlignment(*node), h);
    }

    if (!boundary)
    {
        float liveHeight = 0.0f;

        if (auto live = BuildLiveSection(*node, liveHeight))
        {
            addToSection(live, liveHeight);
        }
    }

    // The groups, laid into rows as they fit `inner`, each row handed to `add`.
    auto layRows = [&](float inner, const std::function<void(Ptr, float)>& add) {
        std::vector<Ptr> row;
        float used = 0.0f;
        float rowHeight = 0.0f;
        bool spans = false;
        auto flush = [&] {
            if (row.empty())
            {
                return;
            }

            if (!spans)
            {
                row.push_back(Stretch());
            }

            add(Row(row), rowHeight);
            row.clear();
            used = 0.0f;
            rowHeight = 0.0f;
            spans = false;
        };

        for (std::size_t i = 0; i < groups.size(); ++i)
        {
            const auto& g = groups[i];
            const float w = std::min(groupWidth(g), inner);

            if (!row.empty() && used + kGroupGap + w > inner)
            {
                flush();
            }

            // An untitled group (the only one, since the rest are titled Other) spans its row.
            const bool alone = groups.size() == 1;
            rowHeight = std::max(rowHeight, groupHeight(g));

            if (!row.empty())
            {
                row.push_back(HGap(kGroupGap));
                used += kGroupGap;
            }

            if (alone && g.name.empty())
            {
                row.push_back(buildGroup(g, true));
                used = inner;
                spans = true;
            }
            else
            {
                row.push_back(FixedWidth(w, buildGroup(g, false)));
                used += w;
            }
        }

        flush();
    };

    if (layout != nullptr)
    {
        // The layout may fill the height left under the header and the sections above it ("fit").
        const float above = 2.0f * kPanelPad + kHeaderHeight + 10.0f + 2.0f * kSectionPad + sectionHeight +
                            (section.empty() ? 0.0f : 12.0f);
        const float roomBelow = std::max(0.0f, stageHeight - above - 8.0f);
        Ptr standard;
        el::extent standardSize{0.0f, 0.0f};

        if (backdrop)
        {
            // The standard controls, wrapped at the layout's own width as the web UI's backdrop
            // wraps them, and placed on it by the layout's offset and scale.
            const float inner = layoutWidth;
            std::vector<Ptr> rows;
            float rowsHeight = 0.0f;
            layRows(inner, [&rows, &rowsHeight](Ptr r, float h) {
                if (!rows.empty())
                {
                    rows.push_back(VGap(8));
                    rowsHeight += 8.0f;
                }

                rows.push_back(FixedHeight(h, std::move(r)));
                rowsHeight += h;
            });
            standard = FixedWidth(inner, FixedHeight(rowsHeight, Col(rows)));
            standardSize = {inner, rowsHeight};
        }

        float layoutHeight = 0.0f;
        auto drawn = BuildEffectLayout(*node, *layout, sectionInner, roomBelow, standard, standardSize, layoutHeight);
        addToSection(drawn, layoutHeight);
    }
    else
    {
        layRows(sectionInner, addToSection);
    }

    if (params.empty() && section.empty())
    {
        auto none = std::make_shared<Text>(mTheme, "This effect has no controls.", 12.0f);
        none->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
        addToSection(none, 40.0f);
    }

    // ── The panel ──
    const float sectionBox = std::max(sectionHeight + 2.0f * kSectionPad, hasArt ? kArtMinHeight : 0.0f);
    const float panelHeight = kPanelPad + kHeaderHeight + 10.0f + sectionBox + kPanelPad;
    Ptr sectionElement =
        Over(Pad(kSectionPad, kSectionPad, kSectionPad, kSectionPad, Col(section)), std::make_shared<InsetBox>(mTheme));

    if (hasArt)
    {
        auto art = std::make_shared<ArtworkPanel>(mTheme, [this, artUrl] { return RemoteImage(artUrl); }, stockArt);
        sectionElement = Row({FixedWidth(kArtWidth, art), HGap(kArtGap), sectionElement});
    }

    auto body = Col({FixedHeight(kHeaderHeight, Row(header)), VGap(10), FixedHeight(sectionBox, sectionElement)});
    auto panel =
        Over(Pad(kPanelPad, kPanelPad, kPanelPad, kPanelPad, body), std::make_shared<EffectPanel>(mTheme, tint));

    Ptr placed = Row({Stretch(), FixedWidth(panelWidth, FixedHeight(panelHeight, panel)), Stretch()});
    Ptr stage;

    if (panelHeight > stageHeight)
    {
        stage = VScroll(Col({placed, VGap(12)}));
    }
    else
    {
        stage = Col({placed, std::make_shared<el::element>()});
    }

    return Over(stage, std::make_shared<EffectBackdrop>(mTheme, panelHeight + 120.0f));
}

void Shell::SyncEffectValues()
{
    // The controls are bound (mLive); what is drawn from the values as a whole follows here.
    if (mEffectPlot)
    {
        SyncEqPlot(*mEffectPlot);
    }

    SyncIrAlignment();
    SyncLiveSection();
}

void Shell::OpenEffectPresets()
{
    const auto* node = SelectedNode();
    const auto* type = node != nullptr ? mState.FindEffectType(node->type) : nullptr;

    if (type == nullptr)
    {
        return;
    }

    const std::string nodeId = node->id;

    // The effect's own, and a factory archive's (these choose its models, IRs and blends), as
    // one list in name order; then the user's.
    auto factory = type->presets;

    if (const auto pack = mState.factoryPackEffectPresets.find(node->type);
        pack != mState.factoryPackEffectPresets.end())
    {
        factory.insert(factory.end(), pack->second.begin(), pack->second.end());
    }

    std::stable_sort(factory.begin(), factory.end(), [](const auto& a, const auto& b) { return a.name < b.name; });

    std::vector<MenuItem> items;

    for (const auto& preset : factory)
    {
        items.push_back({preset.name, false, false,
                         [this, nodeId, preset] { mSession.Commands().ApplyEffectPreset(nodeId, preset); }});
    }

    if (const auto custom = mState.customEffectPresets.find(node->type);
        custom != mState.customEffectPresets.end() && !custom->second.empty())
    {
        items.push_back({"Yours", false, true, {}});

        for (const auto& preset : custom->second)
        {
            items.push_back({preset.name, false, false,
                             [this, nodeId, preset] { mSession.Commands().ApplyEffectPreset(nodeId, preset); }});
        }
    }

    items.push_back({"", false, true, {}});
    items.push_back({"Save current settings as a preset...", false, false, [this, nodeId, name = type->name] {
                         OpenPrompt("Save " + name + " preset", "", "Save", [this, nodeId](const std::string& entered) {
                             mSession.Commands().SaveEffectPreset(nodeId, entered);
                         });
                     }});

    // One of the user's own can be deleted, as the web UI's flyout allows (factory ones cannot).
    if (const auto custom = mState.customEffectPresets.find(node->type);
        custom != mState.customEffectPresets.end() && !custom->second.empty())
    {
        items.push_back({"Delete one of yours...", false, false,
                         [this, effectType = node->type, name = type->name, mine = custom->second] {
                             std::vector<MenuItem> rows;

                             for (const auto& preset : mine)
                             {
                                 rows.push_back({preset.name, false, false, [this, effectType, preset] {
                                                     OpenConfirm("Delete effect preset", "Delete " + preset.name + "?",
                                                                 "Delete", [this, effectType, id = preset.id] {
                                                                     mSession.Commands().DeleteEffectPreset(effectType,
                                                                                                            id);
                                                                 });
                                                 }});
                             }

                             mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), name, rows] {
                                 if (alive.lock())
                                 {
                                     OpenMenu("Delete a " + name + " preset", rows);
                                 }
                             });
                         }});
    }

    OpenMenu(type->name + " presets", std::move(items));
}
} // namespace nanoq::ui
