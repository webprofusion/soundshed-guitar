// The effect page's controls that are not parameters: a node's model and IR slots, the list a
// long enum chooses from, a split's branch count, and renaming a node.
//
// A slot is the web UI's resource picker (signalPath/paramsPanel/resourceSelector.ts): what is
// loaded, previous and next through the library, the whole library by category, a file from
// disk (the engine shows the dialog and adds the file to the library), and clearing an optional
// slot such as the IR Cabinet's IR B.

#include "ui/Shell.h"

#include "uiclient/ChainLayout.h"
#include "uiclient/NodeLabels.h"
#include "uiclient/ParamFormat.h"
#include "uiclient/ResourceIndex.h"
#include "uiclient/SplitRouting.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

namespace
{
/// The library's entries of one type in the order the chooser lists them, which previous/next
/// steps through (uiclient::ResourceIndex, as the web UI's library navigation): each file once,
/// favourites first, by name, packs together; within the category of what the slot plays, as
/// the web UI steps within the slot's category.
std::vector<const LibraryResource*> OrderedResources(const ClientState& state, const std::string& resourceType,
                                                     const std::string& currentId = {})
{
    ResourceFilter filter;

    if (const auto* current = currentId.empty() ? nullptr : state.FindLibraryResource(resourceType, currentId))
    {
        const auto first = current->category.find_first_not_of(" 	");

        if (first != std::string::npos)
        {
            filter.category = current->category.substr(first, current->category.find_last_not_of(" 	") - first + 1);
        }
    }

    auto list = NavigationOrder(state, FilterLibrary(state, resourceType, filter, currentId));

    if (list.empty() && filter.category != "all")
    {
        list = NavigationOrder(state, FilterLibrary(state, resourceType, ResourceFilter{}, currentId));
    }

    return list;
}

/// The slot's resource reference, or null when it is empty.
const guitarfx::ResourceRef* SlotRef(const guitarfx::GraphNode& node, int resourceIndex)
{
    const auto index = static_cast<std::size_t>(std::max(0, resourceIndex));

    if (index >= node.resources.size())
    {
        return nullptr;
    }

    const auto& ref = node.resources[index];
    return ref.IsLibraryRef() || ref.IsFilePath() ? &ref : nullptr;
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// Where the slot's resource sits in `list`, or -1.
int IndexIn(const std::vector<const LibraryResource*>& list, const guitarfx::ResourceRef* ref)
{
    if (ref == nullptr)
    {
        return -1;
    }

    for (std::size_t i = 0; i < list.size(); ++i)
    {
        if ((!ref->resourceId.empty() && list[i]->id == ref->resourceId) ||
            (ref->IsFilePath() && list[i]->filePath == guitarfx::util::PathToUtf8(ref->filePath)))
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}
} // namespace

Ptr Shell::BuildResourceSlot(const guitarfx::GraphNode& node, const std::string& resourceType, int resourceIndex,
                             const std::string& label, bool optional, bool stacked)
{
    auto& commands = mSession.Commands();
    const std::string nodeId = node.id;
    const auto* ref = SlotRef(node, resourceIndex);
    const auto list = OrderedResources(mState, resourceType, ref != nullptr ? ref->resourceId : std::string());
    const int at = IndexIn(list, ref);
    const bool missing =
        ref != nullptr && std::find(mState.missingNodeResources.begin(), mState.missingNodeResources.end(), nodeId) !=
                              mState.missingNodeResources.end();

    std::string name = ref != nullptr
                           ? NodeResourceName(mState, node, static_cast<std::size_t>(std::max(0, resourceIndex)))
                           : std::string();

    if (name.empty())
    {
        name = optional ? "None" : "Choose...";
    }

    // The row the web UI draws (.resource-slot-row): the slot's name in small capitals, previous,
    // the loaded resource (a tap opens the library, where Browse is too), next, and clear.
    auto heading = std::make_shared<Text>(mTheme, Upper(label), 9.0f, Weight::SemiBold);
    heading->Colour(mTheme.Pick(el::rgba(220, 227, 243, 173), mTheme.TextSecondary())).Fixed(true);

    // Previous and next wrap round the library; with nothing loaded they start at either end.
    auto step = [this, nodeId, resourceType, resourceIndex](int delta) {
        return [this, nodeId, resourceType, resourceIndex, delta] {
            const auto* n = SelectedNode();

            if (n == nullptr || n->id != nodeId)
            {
                return;
            }

            const auto* current = SlotRef(*n, resourceIndex);
            const auto ordered =
                OrderedResources(mState, resourceType, current != nullptr ? current->resourceId : std::string());

            if (ordered.empty())
            {
                return;
            }

            const int count = static_cast<int>(ordered.size());
            const int now = IndexIn(ordered, SlotRef(*n, resourceIndex));
            const int next = now < 0 ? (delta > 0 ? 0 : count - 1) : ((now + delta) % count + count) % count;
            mSession.Commands().SetNodeResource(nodeId, resourceType, ordered[static_cast<std::size_t>(next)]->id,
                                                resourceIndex);
        };
    };

    const std::string what = resourceType == "nam" ? "model" : "impulse response";
    auto slate = [this](const char* icon, std::function<void()> onClick, bool enabled) {
        auto b = std::make_shared<IconButton>(mTheme, mIcons, icon, IconButton::Look::Slate);
        b->Size(26, 26).IconSize(13).Disabled(!enabled);
        b->onClick = std::move(onClick);
        return b;
    };

    auto choose = std::make_shared<SelectField>(mTheme, mIcons, missing ? name + "  (file missing)" : name,
                                                at >= 0 || ref != nullptr, missing);
    choose->AccessName(what == "model" ? "Model" : "Impulse response");
    choose->onClick = [this, nodeId, resourceType, resourceIndex] {
        OpenResourceBrowser(nodeId, resourceType, resourceIndex);
    };

    auto clear = slate(
        "x",
        [&commands, nodeId, resourceType, resourceIndex] {
            commands.SetNodeResource(nodeId, resourceType, {}, resourceIndex);
        },
        ref != nullptr);
    clear->AccessName("Clear " + what);
    auto previous = slate("ui-prev", step(-1), !list.empty());
    previous->AccessName("Previous " + what);
    auto next = slate("ui-next", step(1), !list.empty());
    next->AccessName("Next " + what);

    auto controls = Row({previous, HGap(6), choose, HGap(6), next, HGap(6), clear});

    // Stacked, as the effect editor's model card (.neural-model-resource-selector): the label
    // over the row, which takes the card's width.
    if (stacked)
    {
        heading->Colour(mTheme.Pick(el::rgba(220, 227, 243, 200), mTheme.TextSecondary()));
        return Col({FixedHeight(16, heading), VGap(6), FixedHeight(26, controls)});
    }

    return Row(
        {FixedWidth(std::max(46.0f, MeasureTextWidth(Upper(label), Font(Weight::SemiBold).size(9.0f)) + 4.0f), heading),
         HGap(10), controls});
}

void Shell::OpenEnumPicker(const std::string& nodeId, const EffectParamInfo& info)
{
    const auto* node = SelectedNode();

    if (node == nullptr || node->id != nodeId)
    {
        return;
    }

    const int current = EnumLabelIndex(info, NodeParamValue(*node, info));
    std::vector<MenuItem> items;
    std::string group = "\x01";

    for (std::size_t i = 0; i < info.labels.size(); ++i)
    {
        // The engine's labelInfo groups a long list (Pickup Match's pickups by maker).
        if (i < info.labelGroups.size() && info.labelGroups[i] != group)
        {
            group = info.labelGroups[i];

            if (!group.empty())
            {
                items.push_back({group, false, true, {}});
            }
        }

        MenuItem item{info.labels[i], static_cast<int>(i) == current, false, [this, nodeId, info, i] {
                          mSession.Commands().SetNodeParam(nodeId, info.key, EnumLabelValue(info, i));
                          NoteChainEdit();
                          Mark(kValues | kPage);
                      }};

        if (i < info.labelSubgroups.size())
        {
            item.detail = info.labelSubgroups[i];
        }

        items.push_back(std::move(item));
    }

    OpenMenu(info.name, std::move(items));
}

Ptr Shell::BuildSplitBranches(const guitarfx::GraphNode& splitter)
{
    // The count is the splitter's outgoing edges; a branch holding effects cannot be removed,
    // so a count below those is offered disabled (splitBranches.ts).
    int count = 0;
    int holding = 0;

    if (mState.activePreset)
    {
        for (const auto& edge : mState.activePreset->graph.edges)
        {
            if (edge.from == splitter.id)
            {
                ++count;
            }
        }

        const auto layout = BuildChainLayout(mState.activePreset->graph);

        // At any depth: a split nested in a branch has its Branches control too.
        if (const auto* item = layout.FindParallel(splitter.id); item != nullptr && item->splitNodeId == splitter.id)
        {
            holding = static_cast<int>(std::count_if(item->branches.begin(), item->branches.end(),
                                                     [](const ChainBranch& b) { return !b.nodeIds.empty(); }));
        }
    }

    auto heading = std::make_shared<Text>(mTheme, "BRANCHES", 11.0f, Weight::SemiBold);
    heading->Colour(mTheme.TextSecondary());

    std::vector<Ptr> row;

    for (int n = 2; n <= 4; ++n)
    {
        auto b = std::make_shared<Button>(
            mTheme, std::to_string(n),
            [this, id = splitter.id, n, count] {
                if (n != count)
                {
                    mSession.Commands().SetSplitBranchCount(id, n);
                }
            },
            Button::Style::Plain);
        b->MinWidth(52).Selected(n == count).Enabled(n >= holding);

        if (!row.empty())
        {
            row.push_back(HGap(6));
        }

        row.push_back(b);
    }

    std::vector<Ptr> actions{Row(row), HGap(16)};

    if (holding <= 1)
    {
        auto collapse = std::make_shared<Button>(
            mTheme, "Back to one lane", [this, id = splitter.id] { mSession.Commands().CollapseSplit(id); },
            Button::Style::Plain);
        collapse->TextSize(metrics::kSmallText);
        actions.push_back(collapse);
    }

    actions.push_back(Stretch());

    // What the branches get, when the splitter routes them (splitBranches.ts).
    const auto routing = guitarfx::uiclient::DescribeSplitRouting(splitter, count);
    auto hint = std::make_shared<Text>(
        mTheme,
        routing.empty() ? "Each branch runs in parallel and meets the others at the mixer, whose levels set the blend."
                        : routing,
        metrics::kSmallText);
    hint->Colour(mTheme.TextMuted());
    return Col({FixedHeight(20, heading), VGap(4), FixedHeight(metrics::kRowHeight, Row(actions)), VGap(8),
                FixedHeight(18, hint)});
}

void Shell::RenameNodeWithPrompt(const std::string& nodeId)
{
    const auto* node = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;

    if (node == nullptr || IsBoundaryNode(*node))
    {
        return;
    }

    OpenPrompt("Rename effect", NodeDisplayName(mState, *node), "Rename",
               [this, nodeId](const std::string& entered) { mSession.Commands().RenameNode(nodeId, entered); });
}

Ptr Shell::BuildPluginHost(const guitarfx::GraphNode& node)
{
    // The web UI's plugin host (signalPath/hostedPlugins.ts): the scanned plugins, favourites
    // first then by name, a click loads one; the star keeps it at the top (plugins.hostFavorites,
    // shared); Open Plugin shows the plugin's own window; Browse... loads one from a file.
    constexpr const char* kFavouritesSetting = "plugins.hostFavorites";
    const std::string nodeId = node.id;
    const auto* ref = SlotRef(node, 0);
    const bool loading = mState.pluginLoadsPending.count(nodeId) > 0;

    std::set<std::string> favourites;

    if (const auto it = mState.appSettings.find(kFavouritesSetting); it != mState.appSettings.end() && it->is_array())
    {
        for (const auto& v : *it)
        {
            if (v.is_string())
            {
                favourites.insert(v.get<std::string>());
            }
        }
    }

    std::vector<const LibraryResource*> plugins;

    if (const auto it = mState.resources.find("plugin"); it != mState.resources.end())
    {
        for (const auto& r : it->second)
        {
            plugins.push_back(&r);
        }
    }

    std::stable_sort(plugins.begin(), plugins.end(), [&favourites](const LibraryResource* a, const LibraryResource* b) {
        const bool fa = favourites.count(a->id) > 0, fb = favourites.count(b->id) > 0;

        if (fa != fb)
        {
            return fa;
        }

        return Lower(a->name) < Lower(b->name);
    });

    // What is loaded, and the two actions.
    std::string current = "No plugin loaded";

    if (ref != nullptr)
    {
        current = guitarfx::util::PathToUtf8(ref->filePath.filename());

        for (const auto* p : plugins)
        {
            if (p->id == ref->resourceId)
            {
                current = p->name;
            }
        }
    }

    auto name = std::make_shared<Text>(mTheme, loading ? "Loading plugin..." : current, 12.0f, Weight::SemiBold);
    name->Colour(loading ? mTheme.TextMuted() : mTheme.Ink());
    auto open = std::make_shared<Button>(
        mTheme, "Open Plugin", [this, nodeId] { mSession.Commands().SetNodeConfig(nodeId, "showPluginEditor", "1"); });
    open->Enabled(ref != nullptr && !loading);
    auto browse = std::make_shared<Button>(
        mTheme, "Browse...", [this, nodeId] { mSession.Commands().BrowseNodeResource(nodeId, "plugin", 0); });
    auto top = Row({name, HGap(8), open, HGap(6), browse});

    std::vector<Ptr> rows;

    for (const auto* p : plugins)
    {
        const bool selected = ref != nullptr && ref->resourceId == p->id;
        std::string format = guitarfx::util::PathToUtf8(std::filesystem::path(p->filePath).extension());

        if (!format.empty() && format.front() == '.')
        {
            format.erase(0, 1);
        }

        auto row =
            std::make_shared<ListRow>(mTheme, p->name, Upper(format), selected, false, [this, nodeId, id = p->id] {
                mSession.Commands().SetNodeResource(nodeId, "plugin", id, 0);
                NoteChainEdit();
                Mark(kPage);
            });
        const bool fav = favourites.count(p->id) > 0;
        auto star = std::make_shared<IconButton>(mTheme, mIcons, "star");
        star->Size(26, 26).IconSize(13).Ink(fav ? mTheme.Accent() : mTheme.TextMuted());
        star->onClick = [this, id = p->id, favourites] {
            auto next = favourites;

            if (!next.erase(id))
            {
                next.insert(id);
            }

            mSession.Commands().SetSetting(kFavouritesSetting,
                                           nlohmann::json(std::vector<std::string>(next.begin(), next.end())));
            Mark(kPage);
        };
        rows.push_back(FixedHeight(30, Row({row, HGap(4), FixedWidth(26, star)})));
    }

    if (rows.empty())
    {
        auto none = std::make_shared<Text>(
            mTheme, "No plugins found. Soundshed Guitar scans for them; or Browse... for one.", 11.5f);
        none->Colour(mTheme.TextMuted());
        rows.push_back(FixedHeight(30, none));
    }

    return Col({FixedHeight(28, top), VGap(8), FixedHeight(168, VScroll(Col(rows)))});
}

Ptr Shell::BuildBlendInfo(const guitarfx::GraphNode& node)
{
    // The web UI's blend block (signalPathBlend.ts renderBlendInfoHtml): the blend's name and
    // how many models it holds, and the node's own choice of blend mode beside the blend's.
    const std::string nodeId = node.id;
    const auto blendIt = node.config.find("blendId");
    const std::string blendId = blendIt != node.config.end() ? blendIt->second : std::string();
    const BlendSummary* blend = nullptr;

    for (const auto& b : mState.blends)
    {
        if (b.id == blendId)
        {
            blend = &b;
        }
    }

    const auto overrideIt = node.config.find("blendModeOverride");
    const std::string modeOverride =
        overrideIt != node.config.end() && (overrideIt->second == "snap" || overrideIt->second == "interpolate")
            ? overrideIt->second
            : std::string();
    const auto modeName = [](const std::string& mode) {
        return mode == "snap" ? std::string("Snap") : std::string("Interpolate");
    };
    const std::string definitionMode = blend != nullptr ? blend->blendMode : "interpolate";

    auto heading = std::make_shared<Text>(mTheme, "BLEND", 9.0f, Weight::SemiBold);
    heading->Colour(mTheme.TextMuted()).Fixed(true);
    std::string title = blend != nullptr ? blend->name : "Blend not found";

    if (blend != nullptr)
    {
        title += "  Â·  " + std::to_string(blend->modelCount) + (blend->modelCount == 1 ? " model" : " models");
    }

    auto name = std::make_shared<Text>(mTheme, title, 12.0f, Weight::SemiBold);
    name->Colour(blend != nullptr ? mTheme.Ink() : mTheme.Error());

    std::vector<Ptr> row{heading, HGap(10), name};

    if (blend != nullptr)
    {
        auto mode = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Chip);
        mode->Size(150, 26)
            .Label(modeOverride.empty() ? "Default (" + modeName(definitionMode) + ")" : modeName(modeOverride), 11.0f)
            .Caret(true);
        mode->onClick = [this, nodeId, modeOverride, definitionMode, modeName, m = mode.get()] {
            std::vector<MenuItem> items;
            const std::pair<std::string, std::string> choices[] = {
                {"Default (" + modeName(definitionMode) + ")", ""}, {"Interpolate", "interpolate"}, {"Snap", "snap"}};

            for (const auto& [text, value] : choices)
            {
                items.push_back({text, modeOverride == value, false, [this, nodeId, value] {
                                     mSession.Commands().SetNodeConfig(nodeId, "blendModeOverride", value);
                                     NoteChainEdit();
                                 }});
            }

            OpenDropdown(m->Bounds(), std::move(items), 170.0f);
        };
        row.push_back(HGap(8));
        row.push_back(FixedWidth(150, mode));
    }

    auto note = std::make_shared<Text>(mTheme,
                                       blend != nullptr ? "Its blends are edited in Soundshed Guitar."
                                                        : "This node plays its input dry until it is given a blend.",
                                       11.0f);
    note->Colour(mTheme.TextMuted());
    return Col({FixedHeight(26, Row(row)), VGap(4), FixedHeight(16, note)});
}

Ptr Shell::BuildIrList(const guitarfx::GraphNode& node)
{
    // The web UI's IR list between the IR Cabinet's slots (paramsPanel/irSlotList.ts): the list
    // IR A steps through (the library, in its category), each row with A and B to load it into
    // either slot; the IRs playing are marked. Hidden or shown for every cabinet at once.
    const std::string nodeId = node.id;
    const auto* a = SlotRef(node, 0);
    const auto* b = SlotRef(node, 1);
    const auto list = OrderedResources(mState, "ir", a != nullptr ? a->resourceId : std::string());

    auto toggle = std::make_shared<Button>(
        mTheme, mShowIrList ? "Hide the IR list" : "Show the IR list (" + std::to_string(list.size()) + ")",
        [this] {
            mShowIrList = !mShowIrList;
            mEffectSignature.clear();
            Mark(kPage);
        },
        Button::Style::Flat);
    toggle->TextSize(metrics::kSmallText);

    if (!mShowIrList)
    {
        return FixedHeight(28, Row({toggle, Stretch()}));
    }

    std::vector<Ptr> rows;

    for (const auto* r : list)
    {
        const bool inA = a != nullptr && a->resourceId == r->id;
        const bool inB = b != nullptr && b->resourceId == r->id;
        auto name = std::make_shared<Text>(mTheme, r->name.empty() ? r->id : r->name, 11.5f,
                                           inA || inB ? Weight::SemiBold : Weight::Regular);
        name->Colour(inA || inB ? mTheme.Ink() : mTheme.TextSecondary());
        const auto slotButton = [this, nodeId, id = r->id](const char* label, int index, bool on) {
            auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Chip);
            button->Size(26, 22).Label(label, 10.0f).Ink(on ? mTheme.Accent() : mTheme.TextSecondary());
            button->onClick = [this, nodeId, id, index] {
                mSession.Commands().SetNodeResource(nodeId, "ir", id, index);
                NoteChainEdit();
            };
            return button;
        };
        rows.push_back(FixedHeight(
            26, Row({HGap(8), name, HGap(6), slotButton("A", 0, inA), HGap(4), slotButton("B", 1, inB), HGap(6)})));
    }

    if (rows.empty())
    {
        auto none = std::make_shared<Text>(mTheme, "The library has no IRs yet.", 11.5f);
        none->Colour(mTheme.TextMuted());
        rows.push_back(FixedHeight(26, PadXY(8, 0, none)));
    }

    return Col({FixedHeight(28, Row({toggle, Stretch()})), FixedHeight(150, VScroll(Col(rows)))});
}
} // namespace nanoq::ui
