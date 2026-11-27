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
#include "uiclient/PitchShiftRange.h"
#include "uiclient/SplitRouting.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <set>

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

// The IR Cabinet's A/B section (the web UI's paramsPanel/irAbPanel.ts): each slot's own
// controls on its card, Level, Pan, then the mic's Radial and Proximity, and Invert as a Ø in
// its header; what is between the two IRs in the middle.
constexpr const char* kIrAbSlotKeys[2][5] = {{"slotAGain", "slotAPan", "micRadialA", "micProximityA", "slotAPolarity"},
                                             {"slotBGain", "slotBPan", "micRadialB", "micProximityB", "slotBPolarity"}};
constexpr const char* kIrAbCentreKeys[3] = {"irBlend", "lrSplit", "micEmulation"};

bool IsIrAbParam(const std::string& key)
{
    for (const auto& slot : kIrAbSlotKeys)
    {
        if (std::find(std::begin(slot), std::end(slot), key) != std::end(slot))
        {
            return true;
        }
    }

    return std::find(std::begin(kIrAbCentreKeys), std::end(kIrAbCentreKeys), key) != std::end(kIrAbCentreKeys);
}

/// "IR A Level" is "Level" on IR A's card, "Mic B Radial" is "Radial" on IR B's.
std::string NameInSlot(const std::string& name)
{
    for (const char* prefix : {"IR A ", "IR B ", "Mic A ", "Mic B "})
    {
        if (name.rfind(prefix, 0) == 0)
        {
            return name.substr(std::char_traits<char>::length(prefix));
        }
    }

    return name;
}

/// A slot's label over its row, whose controls touch sizes give 36 px (NodeControls.cpp).
float StackedSlot()
{
    return metrics::Touch() ? kStackedSlot + 10.0f : kStackedSlot;
}

/// A control's name in its group's box: inside a group named for a band ("Low-Mid"), the band's
/// name is the box's title, so "Low-Mid Gain" reads "Gain" under it.
std::string NameInGroup(const std::string& name, const std::string& group)
{
    if (group.empty() || name.size() <= group.size() + 1)
    {
        return name;
    }

    // A hyphen matches a space: the Parametric EQ's "Low Mid" group holds "Low-Mid Gain".
    const auto fold = [](char c) {
        return c == '-' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };

    for (std::size_t i = 0; i < group.size(); ++i)
    {
        if (fold(name[i]) != fold(group[i]))
        {
            return name;
        }
    }

    return name[group.size()] == ' ' ? name.substr(group.size() + 1) : name;
}

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
        // The web UI's .default-effect-shell as the theme draws it, washed at the top with the
        // effect's colour on the dark themes.
        const auto& skin = mTheme.Controls();
        DrawControlBox(cnv, mTheme, b, skin.shell, skin.backdrops.shell);

        if (!mTheme.IsLight())
        {
            auto state = cnv.new_state();
            artist::canvas::linear_gradient wash{b.top_left(), b.bottom_left()};
            wash.add_color_stop(0.0f, mTint.opacity(0.12f));
            wash.add_color_stop(std::min(1.0f, 60.0f / std::max(1.0f, b.height())), mTint.opacity(0.02f));
            wash.add_color_stop(1.0f, mTint.opacity(0.0f));
            cnv.fill_style(wash);
            cnv.begin_path();
            cnv.add_round_rect(b, mTheme.Controls().shell.radius);
            cnv.fill();
        }
    }

  private:
    const Theme& mTheme;
    el::color mTint;
};

/// A cheek beside the panel, outside it (Studio 1973's walnut: the web UI's ::before and ::after
/// on .node-params-panel), rounded on its outer side only.
class PanelCheek final : public el::element
{
  public:
    PanelCheek(const Theme& theme, bool left) : mTheme(theme), mLeft(left)
    {
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto& skin = mTheme.Controls();
        const float r = skin.cheekRadius;
        auto state = cnv.new_state();
        // Clipped to the cheek, and to a rounded box reaching past its inner side, so only the
        // outer corners round.
        cnv.begin_path();
        cnv.add_rect(b);
        cnv.clip();
        cnv.begin_path();
        cnv.add_round_rect(
            mLeft ? el::rect{b.left, b.top, b.right + r, b.bottom} : el::rect{b.left - r, b.top, b.right, b.bottom}, r);
        cnv.clip();
        DrawBackdrop(cnv, mTheme, b, mLeft ? skin.backdrops.cheekLeft : skin.backdrops.cheekRight, true);
    }

  private:
    const Theme& mTheme;
    bool mLeft;
};

/// The stage behind the panel: the page colour and its texture, shaded darker under the panel,
/// or the theme's own play page (#panel-visualizer: a stage, a grille).
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
        const auto& backdrops = mTheme.Controls().backdrops;
        DrawBackdrop(cnv, mTheme, b, backdrops.main, false);

        // A theme that dresses the play page (a stage, a grille) shades it in its own layers.
        if (backdrops.stage.count > 0)
        {
            DrawBackdrop(cnv, mTheme, b, backdrops.stage, true);
            return;
        }

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

        // The web UI's equipment panel: its box as the theme draws it, the picture filling it from
        // the top (object-fit: cover), and the shade over the picture's foot that settles it in.
        const auto& box = mTheme.Controls().art;
        DrawControlBox(cnv, b, box);

        if (image)
        {
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(b, box.radius);
            cnv.clip();
            DrawImageCovering(cnv, *image, b, 0.0f);
            artist::canvas::linear_gradient foot{b.top_left(), b.bottom_left()};
            foot.add_color_stop(0.55f, el::rgba(8, 10, 16, 0));
            foot.add_color_stop(1.0f, el::rgba(8, 10, 16, 87));
            cnv.fill_style(foot);
            cnv.fill_rect(b);
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

/// A NAM node playing a capture that includes its cabinet (chainRules.ts
/// nodeUsesFullRigNamCategory): filed as a full rig, or with "cab" in its gear_type.
bool PlaysFullRigNam(const ClientState& state, const guitarfx::GraphNode& node)
{
    namespace guids = guitarfx::EffectGuids;

    if (node.type != guids::kAmpNamOptimized && node.type != guids::kAmpNam && node.type != guids::kFxNam &&
        node.type != guids::kAmpNamBlend)
    {
        return false;
    }

    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto first = text.find_first_not_of(" 	");
        const auto last = text.find_last_not_of(" 	");
        return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    };

    for (const auto& ref : node.resources)
    {
        const auto* resource = ref.resourceId.empty() ? nullptr : state.FindLibraryResource("nam", ref.resourceId);

        if (resource == nullptr)
        {
            continue;
        }

        auto category = lower(resource->category);

        if (category.empty() && resource->metadata.is_object())
        {
            category = lower(resource->metadata.value("gear", std::string()));
        }

        const auto gearType =
            resource->metadata.is_object() ? lower(resource->metadata.value("gear_type", std::string())) : "";

        if (category == "full-rig" || gearType.find("cab") != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

/// An IR Cabinet whose signal path (everything connected to it, either way) already has a full-rig
/// capture (chainRules.ts shouldShowFullRigCabModelNote).
bool ShowsFullRigCabNote(const ClientState& state, const guitarfx::GraphNode& node)
{
    if (node.type != guitarfx::EffectGuids::kCabIr || !state.activePreset)
    {
        return false;
    }

    const auto& graph = state.activePreset->graph;
    std::vector<std::string> queue{node.id};
    std::set<std::string> visited{node.id};

    while (!queue.empty())
    {
        const auto current = queue.back();
        queue.pop_back();

        for (const auto& edge : graph.edges)
        {
            const std::string next = edge.from == current ? edge.to : edge.to == current ? edge.from : std::string();

            if (next.empty() || !visited.insert(next).second)
            {
                continue;
            }

            if (const auto* other = graph.FindNode(next); other != nullptr && PlaysFullRigNam(state, *other))
            {
                return true;
            }

            queue.push_back(next);
        }
    }

    return false;
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
        std::to_string(static_cast<int>(StageHeight() / 24.0f)) + "|" + NodeArchitectureBadge(mState, *node) + "|cm" +
        node->channelMode + (mState.appSettings.value("audio.signalPath.channelModeControl", false) ? "+" : "-");

    // Split, the editor has what the chain leaves (Rebuild builds the chain first).
    if (PlayStageShown())
    {
        s += "|split" + std::to_string(static_cast<int>(mChainAreaHeight));
    }

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

    // The IR Cabinet's cards fade what L/R Split and Mics leave doing nothing.
    if (node->type == guitarfx::EffectGuids::kCabIr)
    {
        s += "|ab";

        for (const char* key : {"lrSplit", "micEmulation"})
        {
            const auto it = node->params.find(key);
            s += it != node->params.end() && it->second >= 0.5 ? "1" : "0";
        }

        s += ShowsFullRigCabNote(mState, *node) ? "|fullrig" : "";
    }

    // The model and IR slots show what is loaded and what the library holds.
    for (const auto& ref : node->resources)
    {
        s += "|r" + ref.resourceType + ":" + ref.resourceId + ":" + guitarfx::util::PathToUtf8(ref.filePath);
    }

    if (type != nullptr && !type->resourceType.empty())
    {
        const auto it = mState.resources.find(type->resourceType);
        s += "|lib" + std::to_string(it != mState.resources.end() ? it->second.size() : 0);

        // A plugin host also shows whether it is loading, why it failed, and which plugins are
        // starred.
        if (type->resourceType == "plugin")
        {
            s += std::string(mState.pluginLoadsPending.count(node->id) > 0 ? "|loading" : "") + "|fav" +
                 mState.appSettings.value("plugins.hostFavorites", nlohmann::json::array()).dump() + "|fail" +
                 PluginLoadFailureText(*node);
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

        // And its routing line follows its mode.
        int outgoing = 0;

        for (const auto& edge : mState.activePreset->graph.edges)
        {
            outgoing += edge.from == node->id ? 1 : 0;
        }

        s += "|route" + guitarfx::uiclient::DescribeSplitRouting(*node, outgoing);
    }

    s += EqSectionSignature(*node);
    return s;
}

Ptr Shell::BuildEffectPage()
{
    mEffectPlot.reset();
    mMultibandPlot.reset();
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

    // Split (the full layout, the compact frame's split), the editor is centred under the chain in
    // what the chain and the bar under it leave: as wide as its controls, as the web UI's panel,
    // never under half the stage nor past it.
    const bool split = PlayStageShown();
    const float stageWidth = StageWidth();
    const float stageHeight = split ? StageHeight() - mChainAreaHeight - 10.0f : StageHeight();

    // The IR Cabinet's IR A and IR B side by side, each with its own controls, the blend between.
    const bool irAb = hasSlots && !layoutPlacesSlots && node->type == guitarfx::EffectGuids::kCabIr && !customControls;

    // ── Which controls: this tab's, and only those whose condition holds ──
    std::vector<EffectParamInfo> params;
    bool anyAdvanced = false;
    bool anyMain = false;

    // Each node opens on Main, as the web UI's panel does.
    if (mShowAdvancedFor != node->id)
    {
        mShowAdvancedFor = node->id;
        mShowAdvanced = false;
    }

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

        const auto shown = [&](const EffectParamInfo& param) {
            // The IR Cabinet's A and B controls are on its slots' cards, whichever tab is shown.
            // The graphic EQ's own sliders stand for all its controls, and Pickup Match's design
            // controls are in its design blocks, as the web UI shows them.
            return !(irAb && IsIrAbParam(param.key)) && node->type != guitarfx::EffectGuids::kEqGraphic &&
                   !(node->type == guitarfx::EffectGuids::kPickupMatch && IsPickupDesignParam(param.key));
        };

        for (const auto& param : type->parameters)
        {
            if (shown(param))
            {
                anyAdvanced = anyAdvanced || param.advanced;
                anyMain = anyMain || !param.advanced;
            }
        }

        // With no main controls there is no Advanced tab: every control shows (panel.ts).
        if (!anyMain)
        {
            anyAdvanced = false;
        }

        for (const auto& param : type->parameters)
        {
            if (!shown(param) || (branches > 0 && !guitarfx::uiclient::IsSplitterParamInUse(param.key, branches)))
            {
                continue;
            }

            if ((!anyAdvanced || param.advanced == mShowAdvanced) && IsParamVisible(param, *node, type))
            {
                params.push_back(param);
            }
        }
    }

    if (!anyAdvanced)
    {
        mShowAdvanced = false;
    }

    // Pitch Shift's Semitones knob spans the node's own range, in whole semitones only while Snap
    // to Semitone is on (pitchShiftRange.ts), and its default is held inside that range, as the
    // engine holds the value.
    const bool pitchShift = !boundary && IsPitchShiftType(node->type);

    if (pitchShift)
    {
        const auto range = SemitoneKnobRange(node->params);

        for (auto& info : params)
        {
            if (info.key == "semitones")
            {
                info.minValue = range.min;
                info.maxValue = range.max;
                info.step = range.step;
                info.defaultValue = std::clamp(info.defaultValue, range.min, range.max);
            }
        }
    }

    // ── The groups: each a box of cells ──
    struct Group
    {
        std::string name;
        std::vector<std::pair<float, Ptr>> cells; // width, cell
        std::vector<std::string> controls;        // the names of the controls in it
        std::string title;                        // what its box shows over them
    };

    std::vector<Group> groups;

    // The node's value of a parameter now, which the controls follow (automation, a preset recalled,
    // another editor). By id, not the selection, which may move on before this page is rebuilt.
    const auto valueOf = [this, nodeId, pitchShift](const EffectParamInfo& info) {
        const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;
        const double value = n != nullptr ? NodeParamValue(*n, info) : info.defaultValue;
        // The engine holds a pitch shift inside its range, so the knob shows the held value.
        return pitchShift && info.key == "semitones" ? std::clamp(value, info.minValue, info.maxValue) : value;
    };

    // After a pitch shift range setting is set, the settings it drags along follow it, sent as the
    // web UI's syncPitchShiftRange sends them, and the page is rebuilt (once any drag is over) for
    // the Semitones knob's new range.
    const auto syncPitchRange = [this, nodeId, pitchShift](const std::string& key) {
        if (!pitchShift || !IsPitchShiftRangeSetting(key))
        {
            return;
        }

        const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;

        if (n == nullptr)
        {
            return;
        }

        for (const auto& [k, v] : ReconcilePitchShiftParams(n->params, key))
        {
            mSession.Commands().SetNodeParam(nodeId, k, v);
        }

        mEffectSignature.clear();
        Mark(kPage);
    };

    // A knob's or switch's cell: as wide as its label, as the web UI's group sizes to it, and
    // never under the usual cell.
    const auto cellFor = [this](const std::string& name) {
        const auto& label = mTheme.Controls().label;
        return std::max(kCellWidth,
                        std::ceil(MeasureControlText(name, label, metrics::Readable(label.size + 1.0f))) + 4.0f);
    };

    // A control's cell, its width and the cell, named `shown`: a list's chip, a switch, an A-B
    // slider or a knob. `blendAsKnob`: an A/B blend drawn as a knob, as the IR cards draw theirs.
    const auto makeCell = [&](const EffectParamInfo& info, const std::string& shown,
                              bool blendAsKnob = false) -> std::pair<float, Ptr> {
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
            auto label = std::make_shared<Text>(mTheme, Upper(shown), metrics::Readable(9.0f), Weight::SemiBold);
            label->Colour(Theme::Argb(mTheme.Controls().label.colour)).Alignment(Text::Align::Centre);
            // Touch sizes: the chip is hit over 36 px.
            const bool touch = metrics::Touch();
            return {kPickerWidth, Sized(kPickerWidth, kCellHeight,
                                        Col({FixedHeight(12, label), VGap(touch ? 2 : 12),
                                             FixedHeight(touch ? 36 : 26, picker), std::make_shared<el::element>()}))};
        }

        if (IsToggleParam(info))
        {
            auto toggle = std::make_shared<ParamToggle>(mTheme, shown);
            toggle->AccessName(info.name);
            toggle->onMenu = menu;
            mLive.Bind(
                toggle, [valueOf, info] { return valueOf(info) > 0.5 * (info.minValue + info.maxValue); },
                &ParamToggle::Set);
            toggle->onClick = [this, nodeId, info, syncPitchRange] {
                const auto* n = SelectedNode();

                if (n == nullptr)
                {
                    return;
                }

                const double now = NodeParamValue(*n, info);
                const double next = now > 0.5 * (info.minValue + info.maxValue) ? info.minValue : info.maxValue;
                mSession.Commands().SetNodeParam(nodeId, info.key, next);
                syncPitchRange(info.key);
                NoteChainEdit();
                Mark(kValues | kPage);
            };
            return {cellFor(shown), toggle};
        }

        if (info.unit == "blend" && !blendAsKnob)
        {
            // An A/B blend (panel.ts, .node-param-blend-group): A, the slider, B, and "A", "B" or
            // the share of B under it.
            constexpr float kBlendCellWidth = 156.0f;
            const auto blendText = [](double v) {
                return v <= 0.01   ? std::string("A")
                       : v >= 0.99 ? std::string("B")
                                   : std::to_string(std::lround(v * 100.0)) + "%";
            };
            const auto span = [info](double v) {
                return info.maxValue > info.minValue ? (v - info.minValue) / (info.maxValue - info.minValue) : 0.0;
            };
            auto slider = std::make_shared<ParamSlider>(mTheme, "");
            slider->AccessName(info.name);
            mLive.Bind(
                slider, [valueOf, info] { return valueOf(info); },
                [span, blendText](ParamSlider& sl, double v) { sl.Update(span(v), blendText(v)); });
            slider->onChange = [this, nodeId, info, blendText, sl = slider.get()](double position) {
                const double value =
                    std::round((info.minValue + position * (info.maxValue - info.minValue)) * 100.0) / 100.0;
                sl->Update(position, blendText(value));
                mSession.Commands().SetNodeParam(nodeId, info.key, value);
                NoteChainEdit();
                Mark(kValues);
            };
            slider->onReset = [this, nodeId, info] {
                mSession.Commands().SetNodeParam(nodeId, info.key, info.defaultValue);
                NoteChainEdit();
                Mark(kValues);
            };
            const auto small = [this](const std::string& text, Text::Align align) {
                auto t = std::make_shared<Text>(mTheme, text, metrics::Readable(9.0f), Weight::SemiBold);
                t->Colour(Theme::Argb(mTheme.Controls().label.colour)).Alignment(align);
                return t;
            };
            return {kBlendCellWidth,
                    Sized(kBlendCellWidth, kCellHeight,
                          Col({FixedHeight(12, small(Upper(shown), Text::Align::Centre)), FixedHeight(44, slider),
                               FixedHeight(14, Pad(6, 0, 6, 0,
                                                   Row({small("A", Text::Align::Left), Stretch(),
                                                        small("B", Text::Align::Right)})))}))};
        }

        {
            // A knob: continuous values, and an enum short enough to turn through, as the web
            // UI's Mode knobs are.
            auto knob = std::make_shared<ParamKnob>(mTheme, shown);
            knob->AccessName(info.name);
            knob->Large(true);
            knob->onMenu = menu;
            mLive.Bind(
                knob, [valueOf, info] { return valueOf(info); },
                [info](ParamKnob& k, double v) { k.Update(ParamValueToPosition(info, v), FormatParamValue(info, v)); });
            const bool stepped = IsEnumParam(info);

            if (stepped)
            {
                knob->KeySteps(static_cast<int>(info.labels.size()));
            }

            knob->onChange = [this, nodeId, info, stepped, syncPitchRange, k = knob.get()](double position) {
                const double value = SnapParamValue(info, ParamPositionToValue(info, position));
                k->SetValueText(FormatParamValue(info, value));
                mSession.Commands().SetNodeParam(nodeId, info.key, value);
                syncPitchRange(info.key);
                NoteChainEdit();
                // The sections drawn from the values (a curve, the IR alignment) follow the turn.
                Mark(kValues);

                if (stepped)
                {
                    Mark(kPage); // a visibleWhen on this enum may show or hide others
                }
            };
            knob->onReset = [this, nodeId, info, syncPitchRange] {
                mSession.Commands().SetNodeParam(nodeId, info.key, info.defaultValue);
                syncPitchRange(info.key);
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
                knob->onTyped = [this, nodeId, info, syncPitchRange](const std::string& entered) {
                    const auto typed = ParseTypedNumber(entered);

                    if (!typed)
                    {
                        Mark(kValues); // the value text comes back as it was
                        return;
                    }

                    mSession.Commands().SetNodeParam(nodeId, info.key, SnapParamValue(info, *typed));
                    syncPitchRange(info.key);
                    NoteChainEdit();
                    Mark(kValues);
                };
            }

            const float cell = cellFor(shown);
            knob->CellWidth(cell);
            return {cell, knob};
        }
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

        group->controls.push_back(info.name);
        group->cells.push_back(makeCell(info, NameInGroup(info.name, info.group)));
    }

    // ── The IR Cabinet's IR A and IR B side by side (the web UI's irAbPanel.ts) ──
    // Each slot's card: its key, the share of the sound it plays and its Ø (Invert) over its
    // picker and its own knobs; between the cards, the blend, L/R Split and Mics. A control that
    // does nothing as things stand is faded where it is, as the web UI's are (the page is rebuilt
    // when L/R Split, Mics or a slot changes).
    constexpr float kAbPickerMin = 220.0f, kAbDivider = 13.0f, kAbHeadHeight = 22.0f, kAbPadTop = 10.0f,
                    kAbPadBottom = 8.0f;
    // The IR list (NodeControls.cpp BuildIrList): closed, its 30 by 29 button at the top of the
    // middle column (or the end of the blend's row); open, its panel under the cards.
    constexpr float kIrListButton = 30.0f, kIrListClosed = 29.0f, kIrListOpen = 318.0f;
    const auto abParam = [type](const char* key) -> const EffectParamInfo* {
        if (type == nullptr)
        {
            return nullptr;
        }

        const auto it = std::find_if(type->parameters.begin(), type->parameters.end(),
                                     [key](const EffectParamInfo& p) { return p.key == key; });
        return it != type->parameters.end() ? &*it : nullptr;
    };
    // The knobs' widths four abreast and two by two, and the middle's.
    float abKnobRow = 0.0f, abKnobPair = 0.0f, abCentre = 0.0f;

    if (irAb)
    {
        float w[4] = {};

        for (std::size_t i = 0; i < 4; ++i)
        {
            const auto* info = abParam(kIrAbSlotKeys[0][i]);
            w[i] = info != nullptr ? cellFor(NameInSlot(info->name)) : 0.0f;
        }

        abKnobRow = w[0] + w[1] + w[2] + w[3] + 3.0f * kCellGap + kAbDivider;
        abKnobPair = std::max(w[0] + w[1], w[2] + w[3]) + kCellGap;
        // Blend, L/R Split and Mics one over another (.ir-ab-centre), 10 px inside its sides.
        abCentre = std::max({cellFor("Blend"), cellFor("L/R Split"), cellFor("Mics"), kIrListButton}) + 2.0f * 10.0f;
    }

    // The section's width with the cards' knobs `knobs` wide: a card is never narrower than its
    // picker needs.
    const auto abWidth = [&](float knobs) {
        return 2.0f * (std::max(knobs, kAbPickerMin) + 2.0f * kGroupPadX) + abCentre + 2.0f * kGroupGap;
    };

    // A slot's Invert as a console's phase button (.ir-ab-invert): a small Ø in an input-coloured
    // box with a medium border, its border lit with the accent under the pointer; on, the accent's
    // border and a fifth of it behind an accent Ø.
    class PhaseButton final : public Pressable
    {
      public:
        explicit PhaseButton(const Theme& theme) : mTheme(theme)
        {
        }

        void Set(bool on)
        {
            mOn = on;
        }

        [[nodiscard]] AccessInfo Describe() const override
        {
            AccessInfo info = Pressable::Describe();
            info.role = AccessRole::Toggle;
            info.toggled = mOn ? 1 : 0;
            return info;
        }

        el::view_limits limits(const el::basic_context&) const override
        {
            return {{26.0f, 22.0f}, {26.0f, 22.0f}};
        }

        void draw(const el::context& ctx) override
        {
            NoteDrawn(ctx);
            auto& cnv = ctx.canvas;
            const auto b = ctx.bounds;
            // --radius-sm: 4, and 2 in the square-cut Stage, Eleven and Studio 1973 themes.
            const float radius =
                mTheme.Name() == "stage" || mTheme.Name() == "high-gain" || mTheme.Name() == "studio-1973" ? 2.0f
                                                                                                           : 4.0f;
            cnv.fill_style(mOn ? mTheme.Accent().opacity(0.22f) : mTheme.Input());
            cnv.fill_round_rect(b, radius);
            cnv.stroke_style(mOn || mHover ? mTheme.Accent() : mTheme.BorderStrong());
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(b.inset(0.5f, 0.5f), radius);
            FillText(cnv, "\xC3\x98", Font(Weight::SemiBold).size(13.0f),
                     mOn ? mTheme.Accent() : mTheme.TextSecondary(), b, HAlign::Centre);
        }

      private:
        const Theme& mTheme;
        bool mOn = false;
    };

    // Whether a slot holds an IR: from the library, a file, or embedded in the preset (irAbPanel.ts
    // slotFilled).
    const auto slotFilled = [](const guitarfx::GraphNode& n, std::size_t index) {
        if (index >= n.resources.size())
        {
            return false;
        }

        const auto& ref = n.resources[index];
        return !ref.resourceId.empty() || ref.IsFilePath() || !ref.embeddedId.empty();
    };

    const auto buildIrAb = [&](float inner, float& height) -> Ptr {
        const auto on = [node](const char* key) {
            const auto it = node->params.find(key);
            return it != node->params.end() && it->second >= 0.5;
        };
        const auto filled = [node, slotFilled](std::size_t index) { return slotFilled(*node, index); };
        const bool lrSplit = on("lrSplit");
        const bool mics = on("micEmulation");
        const bool both = filled(0) && filled(1);

        // Pan and Blend under L/R Split, the mic knobs while Mics is off, Blend and L/R Split
        // with one IR (irAbPanel.ts updateIrAbVisualization).
        const auto idle = [&](const std::string& key) {
            const bool pan = key == "slotAPan" || key == "slotBPan";
            const bool mic = key.rfind("mic", 0) == 0 && key != "micEmulation";
            return (pan && lrSplit) || (mic && !mics) || (key == "irBlend" && (lrSplit || !both)) ||
                   (key == "lrSplit" && !both);
        };
        const auto cell = [&](const char* key, const std::string& name, bool emptySlot) -> std::pair<float, Ptr> {
            const auto* info = abParam(key);

            if (info == nullptr)
            {
                return {0.0f, nullptr};
            }

            auto [w, c] = makeCell(*info, name, true);
            const float opacity = idle(key) ? 0.4f : emptySlot ? 0.45f : 1.0f;
            return {w, opacity < 1.0f ? Fade(opacity, std::move(c)) : std::move(c)};
        };
        // Cells in a row, centred; a rule between the slot's level and its mic after `divideAt`.
        const auto cellsRow = [this](const std::vector<std::pair<float, Ptr>>& cells, std::size_t divideAt) {
            std::vector<Ptr> row{Stretch()};

            for (std::size_t i = 0; i < cells.size(); ++i)
            {
                if (!cells[i].second)
                {
                    continue;
                }

                if (i == divideAt)
                {
                    row.push_back(HGap(6));
                    row.push_back(
                        FixedWidth(1, Fill(Theme::Argb(mTheme.Controls().tabs.rule), std::make_shared<el::element>())));
                    row.push_back(HGap(6));
                }
                else if (row.size() > 1)
                {
                    row.push_back(HGap(kCellGap));
                }

                row.push_back(FixedWidth(cells[i].first, cells[i].second));
            }

            row.push_back(Stretch());
            return Row(row);
        };
        // What share of the sound a slot plays, as its card's header says it.
        const auto shareOf = [this, nodeId](std::size_t index) -> std::string {
            const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;

            if (n == nullptr)
            {
                return {};
            }

            const auto has = [n](std::size_t i) {
                return i < n->resources.size() && (n->resources[i].IsLibraryRef() || n->resources[i].IsFilePath());
            };

            if (!has(index))
            {
                return "EMPTY";
            }

            if (!has(0) || !has(1))
            {
                return "ALONE";
            }

            const auto param = [n](const char* key, double fallback) {
                const auto it = n->params.find(key);
                return it != n->params.end() && std::isfinite(it->second) ? it->second : fallback;
            };

            if (param("lrSplit", 0.0) >= 0.5)
            {
                return index == 0 ? "LEFT" : "RIGHT";
            }

            const double blend = std::clamp(param("irBlend", 0.0), 0.0, 1.0);
            return std::to_string(std::lround((index == 0 ? 1.0 - blend : blend) * 100.0)) + "%";
        };
        const float slotHeight = metrics::Touch() ? 36.0f : 26.0f;
        const auto& groupTitle = mTheme.Controls().groupTitle;

        const auto card = [&](std::size_t index, bool pairs, float& h) -> Ptr {
            const auto& keys = kIrAbSlotKeys[index];
            const bool empty = !filled(index);
            const std::string slot = index == 0 ? "IR A" : "IR B";

            // The header: the slot's colour (the Alignment section's, darker on the light themes)
            // and name in the group title's letter-spaced capitals, its share, Ø.
            const el::color key = index == 0
                                      ? (mTheme.IsLight() ? el::rgba(40, 104, 160, 242) : el::rgba(72, 168, 224, 242))
                                      : (mTheme.IsLight() ? el::rgba(190, 96, 20, 242) : el::rgba(240, 160, 64, 242));
            auto bar = Rounded(key, 1.5f, std::make_shared<el::element>());
            auto name = std::make_shared<SkinText>(mTheme, slot, &ControlSkin::groupTitle);
            auto share = std::make_shared<Text>(mTheme, "", metrics::Readable(10.0f), Weight::SemiBold);
            // An empty slot's share fades with its controls (.ir-ab-slot.is-empty).
            share->Colour(mTheme.TextSecondary().opacity(mTheme.TextSecondary().alpha * (empty ? 0.45f : 1.0f)))
                .Alignment(Text::Align::Right)
                .Fixed(true);
            mLive.Bind(share, [shareOf, index] { return shareOf(index); }, &Text::Set);
            std::vector<Ptr> head{
                Middle(FixedWidth(14, FixedHeight(3, bar))), HGap(6),
                FixedWidth(MeasureControlText(slot, groupTitle) + 2.0f, Middle(FixedHeight(14, name))), Stretch(),
                Middle(share)};

            if (const auto* invert = abParam(keys[4]))
            {
                // Invert as a console's phase button: a small Ø, lit while the slot is inverted.
                auto button = std::make_shared<PhaseButton>(mTheme);
                button->AccessName("Invert " + slot);
                button->onClick = [this, nodeId, info = *invert] {
                    const auto* n = SelectedNode();

                    if (n == nullptr || n->id != nodeId)
                    {
                        return;
                    }

                    const bool now = NodeParamValue(*n, info) >= 0.5;
                    mSession.Commands().SetNodeParam(nodeId, info.key, now ? info.minValue : info.maxValue);
                    NoteChainEdit();
                    Mark(kValues);
                };
                button->onMenu = [this, key = invert->key](el::point at) {
                    if (const auto* n = SelectedNode())
                    {
                        OpenNodeParamMidiMenu(n->type, key, at);
                    }
                };
                mLive.Bind(button, [valueOf, info = *invert] { return valueOf(info) >= 0.5; }, &PhaseButton::Set);
                head.push_back(HGap(8));
                head.push_back(Middle(button));
            }

            // The picker, without its own label: the header names the slot.
            auto picker = BuildResourceSlot(*node, type->resourceType, static_cast<int>(index), "", index == 1, true);

            std::vector<std::pair<float, Ptr>> knobs;

            for (std::size_t i = 0; i < 4; ++i)
            {
                const auto* info = abParam(keys[i]);
                knobs.push_back(cell(keys[i], info != nullptr ? NameInSlot(info->name) : std::string(), empty));
            }

            std::vector<Ptr> rows{FixedHeight(kAbHeadHeight, Row(head)), VGap(8), FixedHeight(slotHeight, picker),
                                  VGap(10)};
            h = kAbPadTop + kAbHeadHeight + 8.0f + slotHeight + 10.0f + kCellHeight + kAbPadBottom;

            if (pairs)
            {
                // Level and Pan over the mic's Radial and Proximity.
                rows.push_back(FixedHeight(kCellHeight, cellsRow({knobs[0], knobs[1]}, SIZE_MAX)));
                rows.push_back(VGap(6));
                rows.push_back(FixedHeight(kCellHeight, cellsRow({knobs[2], knobs[3]}, SIZE_MAX)));
                h += 6.0f + kCellHeight;
            }
            else
            {
                rows.push_back(FixedHeight(kCellHeight, cellsRow(knobs, 2)));
            }

            rows.push_back(std::make_shared<el::element>());
            return Over(Pad(kGroupPadX, kAbPadTop, kGroupPadX, kAbPadBottom, Col(rows)),
                        std::make_shared<InsetBox>(mTheme));
        };

        // Between the cards (.ir-ab-centre): Blend, L/R Split and Mics one over another, 12 px
        // apart, 48 px down, under the IR list's button while the list is closed. Stacked, the
        // three in a row, 24 px apart, with the list's button at the row's end.
        const auto centre = [&](bool inRow, float& h) -> Ptr {
            auto blend = cell("irBlend", "Blend", false);
            auto lr = cell("lrSplit", "L/R Split", false);
            auto mic = cell("micEmulation", "Mics", false);
            const Ptr list = mShowIrList ? nullptr : BuildIrList(*node);
            Ptr content;

            if (inRow)
            {
                h = 8.0f + kCellHeight + 8.0f;
                std::vector<Ptr> row{Stretch()};

                for (const auto& c : {blend, lr, mic})
                {
                    if (c.second)
                    {
                        if (row.size() > 1)
                        {
                            row.push_back(HGap(24));
                        }

                        row.push_back(FixedWidth(c.first, c.second));
                    }
                }

                row.push_back(Stretch());
                row.push_back(HGap(12));
                row.push_back(list ? FixedWidth(kIrListButton, Middle(FixedHeight(kIrListClosed, list)))
                                   : HGap(kIrListButton));
                content = Pad(12, 8, 10, 8, FixedHeight(kCellHeight, Row(row)));
            }
            else
            {
                h = 48.0f + 3.0f * kCellHeight + 2.0f * 12.0f + 12.0f;
                std::vector<Ptr> column{list ? Col({VGap(10), FixedHeight(kIrListClosed, list), VGap(9)}) : VGap(48)};

                for (const auto& c : {blend, lr, mic})
                {
                    if (c.second)
                    {
                        if (column.size() > 1)
                        {
                            column.push_back(VGap(12));
                        }

                        column.push_back(FixedHeight(kCellHeight, cellsRow({c}, SIZE_MAX)));
                    }
                }

                column.push_back(std::make_shared<el::element>());
                content = Pad(10, 0, 10, 12, Col(column));
            }

            return Over(content, std::make_shared<InsetBox>(mTheme));
        };

        float hA = 0.0f, hB = 0.0f, hC = 0.0f;
        const bool four = inner >= abWidth(abKnobRow);
        Ptr cards;

        if (four || inner >= abWidth(abKnobPair))
        {
            auto a = card(0, !four, hA);
            auto b = card(1, !four, hB);
            auto c = centre(false, hC);
            height = std::max({hA, hB, hC});
            const float cardWidth = std::floor((inner - abCentre - 2.0f * kGroupGap) * 0.5f);
            cards = Row({FixedWidth(cardWidth, a), HGap(kGroupGap), FixedWidth(abCentre, c), HGap(kGroupGap),
                         FixedWidth(cardWidth, b), Stretch()});
        }
        else
        {
            // Too narrow for the three abreast: IR A, the blend, IR B, one over another.
            const bool pairs = inner < abKnobRow + 2.0f * kGroupPadX;
            auto a = card(0, pairs, hA);
            auto c = centre(true, hC);
            auto b = card(1, pairs, hB);
            height = hA + 8.0f + hC + 8.0f + hB;
            cards = Col({FixedHeight(hA, a), VGap(8), FixedHeight(hC, c), VGap(8), FixedHeight(hB, b)});
        }

        if (!mShowIrList)
        {
            return cards;
        }

        // Open, the IR list takes the width under the cards.
        const float cardsHeight = height;
        height += 8.0f + kIrListOpen;
        return Col({FixedHeight(cardsHeight, cards), VGap(8), FixedHeight(kIrListOpen, BuildIrList(*node))});
    };

    // A group's title, as the web UI's: none for the controls that have no group (it was "Other",
    // which names nothing), and none where it would only repeat its one control's name ("SWELL"
    // over "SWELL").
    for (auto& g : groups)
    {
        const bool repeats = g.controls.size() == 1 && Upper(g.controls.front()) == Upper(g.name);
        g.title = repeats ? std::string() : g.name;
    }

    // Where any box has a title, every box keeps the room for one, so the knobs of boxes side by
    // side stay in line.
    const bool titled = std::any_of(groups.begin(), groups.end(), [](const Group& g) { return !g.title.empty(); });

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
    auto groupHeight = [titled](const Group&) {
        return !titled ? 8.0f + kCellHeight + 4.0f : 6.0f + 14.0f + 4.0f + kCellHeight + 8.0f;
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
        const float top = !titled ? 8.0f : 6.0f + 14.0f + 4.0f;
        return Over(Pad(kGroupPadX, top, kGroupPadX, !titled ? 4.0f : 8.0f, FixedHeight(kCellHeight, Row(row))),
                    std::make_shared<InsetBox>(mTheme, g.title));
    };

    // The panel's width: as wide as its groups (see panelWidth below).
    float groupsWidth = 0.0f;

    for (const auto& g : groups)
    {
        groupsWidth += groupWidth(g) + (groupsWidth > 0.0f ? kGroupGap : 0.0f);
    }

    // The graphic EQ asks for its bands in one row.
    if (!boundary && node->type == guitarfx::EffectGuids::kEqGraphic)
    {
        groupsWidth = std::max(groupsWidth, GraphicEqNaturalWidth(*node));
    }

    // The IR Cabinet asks for room for its two cards and the blend abreast, knobs four abreast.
    if (irAb)
    {
        groupsWidth = std::max(groupsWidth, abWidth(abKnobRow));
    }

    // Split, the effect's artwork stands in a column left of its controls where the stage leaves
    // the controls room beside it (the full layout always does); narrower, it shrinks to a
    // thumbnail beside the title, as the web UI's does at 900 px; staged, as in the web UI's
    // compact layout, it is left to the chain card.
    std::shared_ptr<artist::image> stockArt;
    std::string artUrl;
    const bool artColumn = split && stageWidth >= 860.0f;

    if (split && !boundary && type != nullptr && layout == nullptr)
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

    const bool anyArt = stockArt != nullptr || !artUrl.empty();
    const bool hasArt = anyArt && artColumn;
    const bool artThumb = anyArt && !artColumn;
    // The web UI's column is minmax(220px, 300px) with a 16 px gap; 296 keeps the controls the
    // room they had beside a 300 px column 12 px off.
    constexpr float kArtWidth = 296.0f, kArtGap = 16.0f, kArtMinHeight = 200.0f;
    constexpr float kThumbWidth = 52.0f, kThumbHeight = 34.0f;
    const float artRoom = hasArt ? kArtWidth + kArtGap : 0.0f;

    // A theme's cheeks stand either side of the panel, outside it, where the stage has room for
    // them beside the narrowest panel (the web UI drops them at compact density).
    const float cheekSkin = mTheme.Controls().cheekWidth;
    const float cheek = cheekSkin > 0.0f && stageWidth - 20.0f - 2.0f * cheekSkin >= kPanelMinWidth ? cheekSkin : 0.0f;
    const float maxPanel = std::max(kPanelMinWidth, stageWidth - 20.0f - 2.0f * cheek);
    const float minPanel = split ? std::clamp((stageWidth - 20.0f) * 0.5f, kPanelMinWidth, maxPanel) : kPanelMinWidth;
    // The spatial panner and the multi-band plots want the room too. The analyzer and the
    // graphic EQ size as any other panel, as the web UI's do (params-panel.css: as wide as its
    // content, at least half the stage): the analyzer's tiles wrap to the width they get.
    const bool wideSection =
        !boundary && (node->type == guitarfx::EffectGuids::kSpatial3D || MultibandPlot::KindOf(node->type).has_value());
    // A layout asks for its own width, or all there is when it may grow past it.
    const bool layoutGrows = layout != nullptr && layout->scaling && layout->scaling->mode != "fixed" &&
                             layout->scaling->maxScale && *layout->scaling->maxScale > 1.0;
    const float layoutWidth = layout != nullptr ? static_cast<float>(std::round(layout->dimensions.width)) : 0.0f;
    // The model slots take the stage when it is narrow (compact); split, half of it is room for
    // their stacked rows, so they size as any other panel.
    const float panelWidth =
        ((hasSlots && !split) || hasPlugin || wideSection || layoutGrows)
            ? maxPanel
            : std::clamp(std::max(groupsWidth + artRoom, layoutWidth) + 2.0f * (kPanelPad + kSectionPad), minPanel,
                         maxPanel);
    const float sectionInner = panelWidth - 2.0f * (kPanelPad + kSectionPad) - artRoom;

    // ── Header ──
    // The web UI's .default-effect-shell-title (11 px, bold) and -subtitle (8 px bold capitals,
    // 0.12 em apart).
    constexpr float kTitleSize = 11.0f;
    const auto titleFont = TitleFont(kTitleSize);
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
        channelsWidth += metrics::Hit(28.0f) + 8.0f;

        // The node's menu (move, replace, turn off, rename, remove), which is otherwise only a
        // right click or a held press on its card away: a visible way to every one of them.
        auto more = std::make_shared<IconButton>(mTheme, mIcons, "ui-more", IconButton::Look::Ghost);
        more->Size(28, 26).IconSize(14).Ink(mTheme.TextSecondary());
        more->AccessName("Effect menu");
        more->onClick = [this, nodeId] { OpenNodeMenu(nodeId, true); };
        rightSide.push_back(more);
        rightSide.push_back(HGap(8));
        channelsWidth += metrics::Hit(28.0f) + 8.0f;
    }

    // Whenever the type has params, the splitter and mixer too, as the web UI's panel has it.
    {
        if ((type != nullptr && !type->parameters.empty()) || boundary)
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
        const float power = metrics::Hit(40.0f);
        rightWidth = channelsWidth + ((type != nullptr || boundary) ? 77.0f + 8.0f + power : power);
    }

    // With touch sizes the pencil is hit over 36 px, more than the title's row has: it goes to
    // the head of the controls at the right, hit over 44 by 36.
    const bool touchPencil = metrics::Touch() && !boundary;
    const float pencilWidth = boundary ? 0.0f : touchPencil ? metrics::Hit(20.0f) + 8.0f : 22.0f;
    const float roomForTitle = panelWidth - 2.0f * kPanelPad - 14.0f - 8.0f - pencilWidth - rightWidth - 16.0f -
                               (artThumb ? kThumbWidth + 10.0f : 0.0f);
    const float titleWidth =
        std::clamp(MeasureTextWidth(titleText, titleFont) + 2.0f, 40.0f, std::max(40.0f, roomForTitle));

    auto led = std::make_shared<HeaderLed>(node->enabled, false);
    auto title = std::make_shared<Text>(mTheme, titleText, kTitleSize, Weight::Title);
    title->Colour(mTheme.EffectTitleInk());
    auto subtitle = std::make_shared<Text>(mTheme, subtitleText, metrics::Readable(8.0f, 10.0f), Weight::SemiBold);
    subtitle->Colour(mTheme.TextMuted()).Fixed(true).Tracking(0.12f);

    // The titles (.default-effect-shell-titles): the name, and under it the kind and the badge.
    std::vector<Ptr> titleRow{FixedWidth(titleWidth, title)};

    if (!boundary)
    {
        auto pencil = std::make_shared<IconButton>(mTheme, mIcons, "pencil");
        pencil->Size(20, 20).IconSize(11).Ink(mTheme.TextMuted());
        pencil->onClick = [this, nodeId] { RenameNodeWithPrompt(nodeId); };

        if (touchPencil)
        {
            rightSide.insert(rightSide.begin(), {pencil, HGap(8)});
        }
        else
        {
            titleRow.push_back(HGap(2));
            titleRow.push_back(Middle(pencil));
        }
    }

    titleRow.push_back(Stretch());
    std::vector<Ptr> subtitleRow{Middle(subtitle)};

    if (!badgeText.empty())
    {
        subtitleRow.push_back(HGap(6));
        subtitleRow.push_back(std::make_shared<ArchBadge>(badgeText));
    }

    subtitleRow.push_back(Stretch());
    std::vector<Ptr> header{Middle(led), HGap(8)};

    if (artThumb)
    {
        auto thumb = std::make_shared<ArtworkPanel>(mTheme, [this, artUrl] { return RemoteImage(artUrl); }, stockArt);
        header.push_back(Middle(Sized(kThumbWidth, kThumbHeight, thumb)));
        header.push_back(HGap(10));
    }

    header.push_back(Col({FixedHeight(20, Row(titleRow)), FixedHeight(14, Row(subtitleRow)), VGap(2)}));

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

    // The DSP status floats in the controls' top right corner, over them, as the web UI's
    // .effect-dsp-status does: it takes no room of its own. Clicks where it has no tile reach
    // what is under it (Elements hands a click only to an element that wants it).
    Ptr dspStatus;

    if (mShowDspStatus && !boundary)
    {
        dspStatus = BuildDspStatus(guitarfx::uiclient::EffectDspStatus(), mState.activePresetId + "::" + node->id,
                                   {"preset", node->id, mState.activePresetId}, false);
    }

    if (!boundary && ShowsFullRigCabNote(mState, *node))
    {
        auto note = std::make_shared<Text>(mTheme, "Signal chain already includes a Cabinet Model (a \"Full Rig\").",
                                           metrics::kSmallText);
        note->Colour(mTheme.TextSecondary());
        addToSection(Middle(note), 18.0f);
    }

    if (hasSlots && !layoutPlacesSlots)
    {
        const auto& rt = type->resourceType;

        if (irAb)
        {
            float abHeight = 0.0f;
            auto ab = buildIrAb(sectionInner, abHeight);
            addToSection(ab, abHeight);
        }
        else if (node->type == guitarfx::EffectGuids::kCabIr)
        {
            addToSection(BuildResourceSlot(*node, rt, 0, "IR A", false, true), StackedSlot());
            addToSection(BuildIrList(*node), mShowIrList ? 318.0f : 29.0f);
            addToSection(BuildResourceSlot(*node, rt, 1, "IR B", true, true), StackedSlot());
        }
        else
        {
            addToSection(BuildResourceSlot(*node, rt, -1,
                                           rt == "nam"  ? "Model"
                                           : rt == "ir" ? "IR"
                                                        : "Resource",
                                           false, true),
                         StackedSlot());
        }
    }

    if (hasPlugin)
    {
        addToSection(BuildPluginHost(*node), PluginHostHeight(*node));
    }

    if (node->type == guitarfx::EffectGuids::kAmpNamBlend)
    {
        addToSection(BuildBlendInfo(*node), 46.0f);
    }

    // The visualisations, above the tabs and the knobs, in the web UI's order (panel.ts): the
    // analyzer, an EQ's curve, the spatial panner, the cabinet and pickup tools, the gates'
    // Learn, the multi-band plot; then the splitter's branches.
    float liveHeight = 0.0f;
    const Ptr live = boundary ? nullptr : BuildLiveSection(*node, liveHeight);

    if (live && node->type == guitarfx::EffectGuids::kInputAnalyzer)
    {
        addToSection(live, liveHeight);
    }

    // An EQ's curve over its input's spectrum; the graphic EQ's behind its sliders.
    if (!boundary && node->type == guitarfx::EffectGuids::kEqGraphic)
    {
        float graphicHeight = 0.0f;
        addToSection(BuildGraphicEq(*node, sectionInner, graphicHeight), graphicHeight);
    }
    else if (!boundary && IsEqPlotType(node->type))
    {
        addToSection(MakeEqPlot(NodeEqPlotSource(*node), false), ResponsePlot::CardHeight(mTheme, 140.0f));
    }

    if (live && node->type != guitarfx::EffectGuids::kInputAnalyzer)
    {
        addToSection(live, liveHeight);
    }

    if (node->type == guitarfx::EffectGuids::kCabSimple)
    {
        addToSection(BuildCabTools(*node), 28.0f);
    }

    // The gates' learn meters, in a card under a card title as gateLearn.ts lays them out.
    const auto addGateLearn = [&](GateLearnTarget target, const char* titleText) {
        const float pad = mTheme.Controls().cardPadding;
        const float learn = GateLearnHeight(target);
        auto title = std::make_shared<SkinText>(mTheme, titleText, &ControlSkin::cardTitle);
        addToSection(Over(Pad(pad, pad, pad, pad,
                              Col({FixedHeight(12.0f, title), VGap(8.0f),
                                   FixedHeight(learn, BuildGateLearn(std::move(target)))})),
                          std::make_shared<SkinBox>(mTheme, &ControlSkin::card)),
                     2.0f * pad + 12.0f + 8.0f + learn);
    };

    if (node->type == guitarfx::EffectGuids::kDynamicsGate)
    {
        addGateLearn(NodeGateLearnTarget(*node), "Noise Floor");
    }

    if (node->type == guitarfx::EffectGuids::kMultibandGate)
    {
        addGateLearn(MultibandGateLearnTarget(*node), "Noise Floor by Band");
    }

    // The multi-band effects' bands over the spectrum in and out (multibandPanel.ts).
    if (!boundary && MultibandPlot::KindOf(node->type))
    {
        addToSection(BuildMultibandPlot(*node), ResponsePlot::CardHeight(mTheme, MultibandPlot::kPlotHeight));
    }

    if (IsSplitterType(node->type))
    {
        // A line more when the splitter's mode routes the branches (BuildSplitBranches).
        int outgoing = 0;

        for (const auto& edge : mState.activePreset->graph.edges)
        {
            outgoing += edge.from == node->id ? 1 : 0;
        }

        const bool routed = !guitarfx::uiclient::DescribeSplitRouting(*node, outgoing).empty();
        addToSection(BuildSplitBranches(*node), routed ? 114.0f : 96.0f);
    }

    // The tabs (.node-param-tabs): on their own line, with a rule under them across the section.
    // A backdrop layout draws them on itself, above its controls, as the web UI's backdrop takes
    // the params panel's markup tabs and all.
    // Touch sizes give the tabs a 40 px row to be hit in.
    const float tabRow = metrics::Touch() ? 40.0f : 28.0f;
    auto tabLine = [&]() -> Ptr {
        auto t = tabs();
        return t ? Col({FixedHeight(tabRow, Row({t, Stretch()})),
                        FixedHeight(1,
                                    Fill(Theme::Argb(mTheme.Controls().tabs.rule), std::make_shared<el::element>()))})
                 : nullptr;
    };

    if (!backdrop)
    {
        if (auto line = tabLine())
        {
            addToSection(line, tabRow + 1.0f);
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

            // The controls with no group, when nothing else is grouped, span their row.
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
        float controlsFit = 1.0f;

        if (backdrop)
        {
            // The standard controls, wrapped at the layout's own width as the web UI's backdrop
            // wraps them, and placed on it by the layout's offset and scale. A layout with a
            // controls area wraps them at its width instead, and shrinks them, keeping the
            // scale's shape, until they are no taller than it (layoutControlsArea.ts).
            float inner = layoutWidth;

            if (layout->defaultControlsArea)
            {
                const auto positive = [](double v) {
                    return std::isfinite(v) && v > 0.0 ? static_cast<float>(v) : 1.0f;
                };
                const float scaleX = positive(layout->defaultControlsScale.x);
                const float scaleY = positive(layout->defaultControlsScale.y);
                const float areaW = static_cast<float>(layout->defaultControlsArea->width);
                const float areaH = static_cast<float>(layout->defaultControlsArea->height);

                // layRows' wrapping, measured without building anything.
                const auto heightAt = [&](float width) {
                    float total = 0.0f, used = 0.0f, rowHeight = 0.0f;
                    bool any = false;

                    for (const auto& g : groups)
                    {
                        const float w = std::min(groupWidth(g), width);

                        if (any && used + kGroupGap + w > width)
                        {
                            total += rowHeight + 8.0f;
                            used = 0.0f;
                            rowHeight = 0.0f;
                            any = false;
                        }

                        used += (any ? kGroupGap : 0.0f) + w;
                        rowHeight = std::max(rowHeight, groupHeight(g));
                        any = true;
                    }

                    return total + rowHeight + (anyAdvanced ? tabRow + 1.0f + 8.0f : 0.0f);
                };
                const auto drawnHeight = [&](float fit) { return heightAt(areaW / (scaleX * fit)) * scaleY * fit; };

                if (drawnHeight(1.0f) > areaH)
                {
                    float low = 0.2f, high = 1.0f;

                    for (int step = 0; step < 8; ++step)
                    {
                        const float mid = 0.5f * (low + high);
                        (drawnHeight(mid) <= areaH ? low : high) = mid;
                    }

                    controlsFit = low;
                }

                controlsFit = std::floor(controlsFit * 10000.0f) / 10000.0f;
                inner = areaW / (scaleX * controlsFit);
            }

            std::vector<Ptr> rows;
            float rowsHeight = 0.0f;

            if (auto line = tabLine())
            {
                rows.push_back(FixedHeight(tabRow + 1.0f, line));
                rowsHeight += tabRow + 1.0f;
            }

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
        auto drawn = BuildEffectLayout(*node, *layout, sectionInner, roomBelow, standard, standardSize, layoutHeight,
                                       controlsFit);
        addToSection(drawn, layoutHeight);
    }
    else
    {
        layRows(sectionInner, addToSection);
    }

    // Pickup Match's design blocks, under the tabs as the web UI places them.
    if (!boundary && node->type == guitarfx::EffectGuids::kPickupMatch)
    {
        float designHeight = 0.0f;

        if (auto designs = BuildPickupDesignBlocks(*node, sectionInner, makeCell, designHeight))
        {
            addToSection(designs, designHeight);
        }
    }

    // The IR Cabinet's alignment, under its controls as the web UI places it, in its group box.
    if (const float h = IrAlignmentHeight(*node); h > 0.0f && !boundary)
    {
        addToSection(BuildIrAlignment(*node, !irAb), h);
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
    Ptr sectionElement = Over(Pad(kSectionPad, kSectionPad, kSectionPad, kSectionPad, Col(section)),
                              std::make_shared<SkinBox>(mTheme, &ControlSkin::section));

    if (dspStatus)
    {
        sectionElement = Over(
            el::share(el::align_top(el::align_right(el::margin({0.0f, 2.0f, 2.0f, 0.0f}, el::hold_any(dspStatus))))),
            sectionElement);
    }

    if (hasArt)
    {
        auto art = std::make_shared<ArtworkPanel>(mTheme, [this, artUrl] { return RemoteImage(artUrl); }, stockArt);
        sectionElement = Row({FixedWidth(kArtWidth, art), HGap(kArtGap), sectionElement});
    }

    auto body = Col({FixedHeight(kHeaderHeight, Row(header)), VGap(10), FixedHeight(sectionBox, sectionElement)});
    auto panel =
        Over(Pad(kPanelPad, kPanelPad, kPanelPad, kPanelPad, body), std::make_shared<EffectPanel>(mTheme, tint));

    Ptr unit = FixedWidth(panelWidth, FixedHeight(panelHeight, panel));

    if (cheek > 0.0f)
    {
        unit = Row({FixedWidth(cheek, FixedHeight(panelHeight, std::make_shared<PanelCheek>(mTheme, true))), unit,
                    FixedWidth(cheek, FixedHeight(panelHeight, std::make_shared<PanelCheek>(mTheme, false)))});
    }

    Ptr placed = Row({Stretch(), unit, Stretch()});
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

    if (mMultibandPlot)
    {
        mSession.Repaint(*mMultibandPlot);
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
