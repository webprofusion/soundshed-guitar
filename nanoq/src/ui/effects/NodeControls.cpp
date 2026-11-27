// The effect page's controls that are not parameters: a node's model and IR slots, the list a
// long enum chooses from, a split's branch count, and renaming a node.
//
// A slot is the web UI's resource picker (signalPath/paramsPanel/resourceSelector.ts): what is
// loaded, previous and next through the library, the whole library by category, a file from
// disk (the engine shows the dialog and adds the file to the library), and clearing an optional
// slot such as the IR Cabinet's IR B.

#include "ui/shell/Shell.h"

#include "uiclient/BlendMix.h"
#include "uiclient/ChainLayout.h"
#include "uiclient/NodeLabels.h"
#include "uiclient/ParamFormat.h"
#include "uiclient/ResourceIndex.h"
#include "uiclient/SplitRouting.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <map>
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

/// The theme's --radius-sm and --radius-md: 4 and 6, 2 and 3 in the square-cut Stage, Eleven and Studio 1973.
bool SquareCut(const Theme& theme)
{
    return theme.Name() == "stage" || theme.Name() == "high-gain" || theme.Name() == "studio-1973";
}

float RadiusSm(const Theme& theme)
{
    return SquareCut(theme) ? 2.0f : 4.0f;
}

float RadiusMd(const Theme& theme)
{
    return SquareCut(theme) ? 3.0f : 6.0f;
}

/// The IR list's buttons (irSlotList.ts): its list button and close cross, the two source tabs
/// and a row's A and B. Each an input-coloured box with a medium border in secondary ink, the
/// border lit with the accent and the ink brightened under the pointer; the chosen source tab in
/// the accent.
class IrListButton final : public Pressable
{
  public:
    enum class Kind
    {
        Toggle, // .ir-slot-list-toggle: the list's icon, 30 by 29
        Close,  // .ir-slot-list-toggle.is-close: a cross, 26 by 26
        Source, // .ir-slot-list-source: "IR A · Library", sharing the header's width
        Assign  // .ir-slot-list-assign: A or B, 22 by 20
    };

    IrListButton(const Theme& theme, Icons& icons, Kind kind, std::string label = {}, bool active = false)
        : mTheme(theme), mIcons(icons), mKind(kind), mLabel(std::move(label)), mActive(active)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = mKind == Kind::Source ? AccessRole::Tab : AccessRole::Button;
        info.name = NameOr(mKind == Kind::Toggle ? "Show IR list" : mKind == Kind::Close ? "Hide IR list" : mLabel);
        info.selected = mActive;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        switch (mKind)
        {
        case Kind::Toggle:
            return {{30.0f, 29.0f}, {30.0f, 29.0f}};
        case Kind::Close:
            return {{26.0f, 26.0f}, {26.0f, 26.0f}};
        case Kind::Source:
            return {{24.0f, 22.0f}, {el::full_extent, 22.0f}};
        case Kind::Assign:
            break;
        }

        return {{22.0f, 20.0f}, {22.0f, 20.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float radius = mKind == Kind::Toggle ? RadiusMd(mTheme) : RadiusSm(mTheme);
        const bool lit = mActive || mHover;
        cnv.fill_style(mTheme.Input());
        cnv.fill_round_rect(b, radius);
        cnv.stroke_style(lit ? mTheme.Accent() : mTheme.BorderStrong());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), radius);
        const auto ink = mActive ? mTheme.Accent() : mHover ? mTheme.Text() : mTheme.TextSecondary();

        if (mKind == Kind::Toggle || mKind == Kind::Close)
        {
            const auto c = cycfi::artist::center_point(b);
            mIcons.Draw(cnv, mKind == Kind::Toggle ? "list" : "close",
                        el::rect{c.x - 7.5f, c.y - 7.5f, c.x + 7.5f, c.y + 7.5f}, ink);
            return;
        }

        FillText(cnv, mLabel, Font(Weight::SemiBold).size(10.0f), ink, b.inset(6.0f, 0.0f), HAlign::Centre);
    }

  private:
    const Theme& mTheme;
    Icons& mIcons;
    Kind mKind;
    std::string mLabel;
    bool mActive;
};

/// "A" or "B" on a row whose IR that slot plays (.ir-slot-list-row-loaded): a pill in the accent.
class LoadedBadge final : public el::element
{
  public:
    LoadedBadge(const Theme& theme, std::string letter) : mTheme(theme), mLetter(std::move(letter))
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{16.0f, 16.0f}, {16.0f, 16.0f}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        cnv.fill_style(mTheme.Accent());
        cnv.fill_round_rect(ctx.bounds, ctx.bounds.height() * 0.5f);
        FillText(cnv, mLetter, Font(Weight::SemiBold).size(9.0f), mTheme.Input(), ctx.bounds, HAlign::Centre);
    }

  private:
    const Theme& mTheme;
    std::string mLetter;
};

/// The IR list's state per cabinet across the page's rebuilds, as the web UI keeps it: whose
/// list it shows (IR A's or IR B's) and the filter. UI thread.
struct IrListState
{
    int sourceSlot = 0;
    std::string query;
};

std::map<std::string, IrListState>& IrListStates()
{
    static std::map<std::string, IrListState> states;
    return states;
}

/// The plugin host's load-error box: a title and a line.
constexpr float kPluginErrorHeight = 46.0f;

/// Rows drawn at once; a filter narrows a longer list (irSlotList.ts MAX_ROWS).
constexpr std::size_t kIrListMaxRows = 300;
} // namespace

Ptr Shell::BuildResourceSlot(const guitarfx::GraphNode& node, const std::string& resourceType, int resourceIndex,
                             const std::string& label, bool optional, bool stacked, bool allowBrowse)
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
    heading->Colour(mTheme.DarkOr(el::rgba(220, 227, 243, 173), mTheme.TextSecondary())).Fixed(true);

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

    const bool library = resourceType == "nam" || resourceType == "ir";
    const std::string what = resourceType == "nam" ? "model" : resourceType == "ir" ? "impulse response" : "resource";
    auto slate = [this](const char* icon, std::function<void()> onClick, bool enabled) {
        auto b = std::make_shared<IconButton>(mTheme, mIcons, icon, IconButton::Look::Slate);
        b->Size(26, 26).IconSize(13).Disabled(!enabled);
        b->onClick = std::move(onClick);
        return b;
    };

    auto choose = std::make_shared<SelectField>(mTheme, mIcons, missing ? name + "  (file missing)" : name,
                                                at >= 0 || ref != nullptr, missing);
    choose->AccessName(what == "model" ? "Model" : what == "resource" ? label : "Impulse response");
    choose->onClick = [this, nodeId, resourceType, resourceIndex] {
        OpenResourceBrowser(nodeId, resourceType, resourceIndex);
    };

    // A Custom Effect's or composite's own kind of file (a module, a data blob) has no browser:
    // the library's files of that kind and Browse, as the web UI's drop-down and folder button
    // (resourceSelector.ts).
    if (!library)
    {
        choose->onClick = [this, nodeId, resourceType, resourceIndex, label, allowBrowse] {
            const auto* n = SelectedNode();

            if (n == nullptr || n->id != nodeId)
            {
                return;
            }

            const auto* current = SlotRef(*n, resourceIndex);
            std::vector<MenuItem> items;
            items.push_back({"None", current == nullptr || current->resourceId.empty(), false,
                             [this, nodeId, resourceType, resourceIndex] {
                                 mSession.Commands().SetNodeResource(nodeId, resourceType, {}, resourceIndex);
                             }});

            for (const auto* entry : OrderedResources(mState, resourceType))
            {
                items.push_back({entry->name.empty() ? entry->id : entry->name,
                                 current != nullptr && current->resourceId == entry->id, false,
                                 [this, nodeId, resourceType, resourceIndex, id = entry->id] {
                                     mSession.Commands().SetNodeResource(nodeId, resourceType, id, resourceIndex);
                                 }});
            }

            if (allowBrowse)
            {
                MenuItem browse{"Browse for file...", false, false, [this, nodeId, resourceType, resourceIndex] {
                                    mSession.Commands().BrowseNodeResource(nodeId, resourceType, resourceIndex);
                                }};
                browse.icon = "folder";
                items.push_back(std::move(browse));
            }

            OpenMenu(label.empty() ? std::string("Resource") : label, std::move(items));
        };
    }

    auto clear = slate(
        "x",
        [&commands, nodeId, resourceType, resourceIndex] {
            commands.SetNodeResource(nodeId, resourceType, {}, resourceIndex);
        },
        ref != nullptr);
    clear->AccessName("Clear " + what);
    // A model or IR walks the list it was last picked from (the browser keeps it); stepping a
    // Tone3000 set stops at its ends.
    const auto stepper = [this, nodeId, resourceType, resourceIndex, step,
                          library](int delta) -> std::function<void()> {
        if (!library)
        {
            return step(delta);
        }

        return [this, nodeId, resourceType, resourceIndex, delta] {
            StepSlotResource(nodeId, resourceType, resourceIndex, delta);
        };
    };
    const auto canStep = [this, nodeId, resourceType, resourceIndex, library, &list](int delta) {
        return library ? CanStepSlotResource(nodeId, resourceType, resourceIndex, delta) : !list.empty();
    };
    auto previous = slate("ui-prev", stepper(-1), canStep(-1));
    previous->AccessName("Previous " + what);
    auto next = slate("ui-next", stepper(1), canStep(1));
    next->AccessName("Next " + what);

    // Nothing loaded: the field is the slot's call to action (resourcePickerLabel.ts), and a
    // clear that has nothing to clear goes.
    const bool empty = ref == nullptr || (ref->resourceId.empty() && !ref->IsFilePath() && ref->embeddedId.empty());
    Ptr field = choose;

    if (empty && !missing)
    {
        const std::string prompt = resourceType == "ir"     ? (optional ? "Add a second IR" : "Choose an IR")
                                   : resourceType == "nam"  ? "Choose a model"
                                   : resourceType == "wasm" ? "Choose a module"
                                                            : "Choose " + Lower(label.empty() ? "a file" : label);
        const std::string hint = resourceType == "ir"    ? "or drop a .wav file here"
                                 : resourceType == "nam" ? "or drop a .nam file here"
                                                         : "";
        auto zone = std::make_shared<EmptySlotField>(mTheme, prompt, hint, optional);
        zone->onClick = choose->onClick;
        field = zone;
    }

    auto controls = empty && !missing ? Row({previous, HGap(6), field, HGap(6), next})
                                      : Row({previous, HGap(6), field, HGap(6), next, HGap(6), clear});

    // On a card that names the slot itself (the IR Cabinet's A and B), the row alone.
    if (stacked && label.empty())
    {
        return controls;
    }

    // Stacked, as the effect editor's model card (.neural-model-resource-selector): the label
    // over the row, which takes the card's width.
    if (stacked)
    {
        heading->Colour(mTheme.DarkOr(el::rgba(220, 227, 243, 200), mTheme.TextSecondary()));
        Ptr headingRow = heading;

        // A file loaded from outside the library shows where it is (.resource-path-info), on the
        // label's line, cut short when long.
        if (ref != nullptr && !ref->filePath.empty() && !metrics::Touch())
        {
            heading->Fixed(true);
            auto path = std::make_shared<Text>(mTheme, guitarfx::util::PathToUtf8(ref->filePath), 9.0f);
            path->Colour(mTheme.TextMuted()).Alignment(Text::Align::Right);
            headingRow = Row({heading, HGap(12), path});
        }

        // Touch sizes: the row's buttons and field are hit over 36 px (EffectPage.cpp StackedSlot).
        return Col({FixedHeight(16, headingRow), VGap(6), FixedHeight(metrics::Touch() ? 36 : 26, controls)});
    }

    return Row(
        {FixedWidth(std::max(46.0f, MeasureTextWidth(Upper(label), Font(Weight::SemiBold).size(9.0f)) + 4.0f), heading),
         HGap(10), controls});
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

    // What the branches get, when the splitter routes them, then how branches come and go
    // (splitBranches.ts).
    std::vector<Ptr> column{FixedHeight(20, heading), VGap(4), FixedHeight(metrics::kRowHeight, Row(actions)), VGap(8)};
    const auto hintLine = [&](const std::string& text) {
        auto hint = std::make_shared<Text>(mTheme, text, metrics::kSmallText);
        hint->Colour(mTheme.TextMuted());
        column.push_back(FixedHeight(18, hint));
    };

    if (const auto routing = guitarfx::uiclient::DescribeSplitRouting(splitter, count); !routing.empty())
    {
        hintLine(routing);
    }

    hintLine("New branches start empty. Only an empty branch can be removed.");
    return Col(std::move(column));
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

std::string Shell::PluginLoadFailureText(const guitarfx::GraphNode& node) const
{
    const auto failed = mState.pluginLoadFailures.find(node.id);

    if (failed == mState.pluginLoadFailures.end())
    {
        return {};
    }

    // Only while the plugin that failed is still the one picked (getHostedPluginLoadFailureForResource).
    const auto& f = failed->second;
    const auto* ref = SlotRef(node, 0);
    const bool current = ref == nullptr || (f.resourceId.empty() && f.filePath.empty()) ||
                         (!f.resourceId.empty() && f.resourceId == ref->resourceId) ||
                         (!f.filePath.empty() && f.filePath == guitarfx::util::PathToUtf8(ref->filePath));

    if (!current)
    {
        return {};
    }

    const auto message =
        f.message.empty() ? std::string("The selected plugin cannot be hosted by this build.") : f.message;
    return f.errorCode.empty() ? message : message + " (code: " + f.errorCode + ")";
}

float Shell::PluginHostHeight(const guitarfx::GraphNode& node) const
{
    return 28.0f + 8.0f + 168.0f + (PluginLoadFailureText(node).empty() ? 0.0f : kPluginErrorHeight + 8.0f);
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

    const auto formatOf = [](const std::string& path) {
        std::string format = guitarfx::util::PathToUtf8(std::filesystem::u8path(path).extension());

        if (!format.empty() && format.front() == '.')
        {
            format.erase(0, 1);
        }

        return Upper(format);
    };
    // Each entry's file under its name (.plugin-host-item-path).
    const auto withPath = [this](Ptr row, const std::string& path) -> Ptr {
        if (path.empty())
        {
            return FixedHeight(30, std::move(row));
        }

        auto where = std::make_shared<Text>(mTheme, path, 9.0f);
        where->Colour(mTheme.TextMuted());
        return FixedHeight(42,
                           Col({FixedHeight(28, std::move(row)), FixedHeight(12, Pad(12, 0, 8, 0, where)), VGap(2)}));
    };

    std::vector<Ptr> rows;

    // A plugin loaded from a file rather than the library leads the list, picked
    // (.plugin-host-item.is-custom).
    if (ref != nullptr && ref->resourceId.empty() && !ref->filePath.empty())
    {
        const auto path = guitarfx::util::PathToUtf8(ref->filePath);
        auto custom = std::make_shared<ListRow>(mTheme, guitarfx::util::PathToUtf8(ref->filePath.filename()),
                                                formatOf(path), true, false, [] {});
        rows.push_back(withPath(custom, path));
    }

    for (const auto* p : plugins)
    {
        const bool selected = ref != nullptr && ref->resourceId == p->id && ref->filePath.empty();
        auto row = std::make_shared<ListRow>(mTheme, p->name, formatOf(p->filePath), selected, false,
                                             [this, nodeId, id = p->id] {
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
        star->AccessName(fav ? "Remove from favourites" : "Add to favourites");

        // Out of the library, the file left where it is (.plugin-host-remove-btn).
        auto remove = std::make_shared<IconButton>(mTheme, mIcons, "x");
        remove->Size(26, 26).IconSize(11).Ink(mTheme.TextMuted());
        remove->AccessName("Remove " + p->name + " from library");
        remove->onClick = [this, nodeId, id = p->id, pluginName = p->name] {
            OpenConfirm("Remove Plugin",
                        "Remove \"" + pluginName +
                            "\" from your plugin library? The plugin file on disk will not be deleted.",
                        "Remove", [this, nodeId, id] {
                            const auto* n = SelectedNode();
                            const auto* now = n != nullptr && n->id == nodeId ? SlotRef(*n, 0) : nullptr;

                            // Not in use by the preset when the library lets it go.
                            if (now != nullptr && now->resourceId == id)
                            {
                                mSession.Commands().SetNodeResource(nodeId, "plugin", {}, 0);
                            }

                            mSession.Commands().ForgetLibraryResource("plugin", id);
                        });
        };
        rows.push_back(
            withPath(Row({row, HGap(4), FixedWidth(26, star), HGap(2), FixedWidth(26, remove)}), p->filePath));
    }

    if (rows.empty())
    {
        auto none = std::make_shared<Text>(
            mTheme, "No plugins found. Soundshed Guitar scans for them; or Browse... for one.", 11.5f);
        none->Colour(mTheme.TextMuted());
        rows.push_back(FixedHeight(30, none));
    }

    std::vector<Ptr> column{FixedHeight(28, top), VGap(8)};

    // Why the plugin did not load, over the list (.plugin-host-load-error), while it is still
    // the one picked.
    if (const auto failure = PluginLoadFailureText(node); !failure.empty())
    {
        auto title = std::make_shared<Text>(mTheme, "Plugin Load Error", 11.0f, Weight::SemiBold);
        title->Colour(el::rgba(255, 170, 160, 255));
        auto detail = std::make_shared<Text>(mTheme, failure, 10.5f);
        detail->Colour(mTheme.TextSecondary());
        column.push_back(FixedHeight(
            kPluginErrorHeight, Over(Pad(10, 6, 10, 6, Col({FixedHeight(16, title), FixedHeight(16, detail)})),
                                     el::share(Surface(el::rgba(196, 59, 59, 40), el::rgba(196, 59, 59, 140), 6.0f)))));
        column.push_back(VGap(8));
    }

    column.push_back(FixedHeight(168, VScroll(Col(rows))));
    return Col(std::move(column));
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
        title += "  ·  " + std::to_string(blend->modelCount) + (blend->modelCount == 1 ? " model" : " models");
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

    // Under it, the model playing or the two being mixed, as the knobs turn
    // (formatBlendMatchLine); missing, what the node does meanwhile.
    auto note = std::make_shared<Text>(mTheme, "", 11.0f);
    note->Colour(mTheme.TextMuted());
    mLive.Bind(
        note,
        [this, nodeId] {
            const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;
            const auto* b = n != nullptr ? FindNodeBlend(mState, *n) : nullptr;
            return b != nullptr ? BlendMatchLine(mState, *n, *b)
                                : std::string("This node plays its input dry until it is given a blend.");
        },
        &Text::Set);
    return Col({FixedHeight(26, Row(row)), VGap(4), FixedHeight(16, note)});
}

Ptr Shell::BuildIrList(const guitarfx::GraphNode& node)
{
    // The web UI's IR list between the IR Cabinet's slots (paramsPanel/irSlotList.ts). Closed, a
    // button with the list's icon. Open: whose list it is (IR A's or IR B's: the library, in the
    // category of the IR that slot plays, as its previous and next step through it), the list's
    // title and count, a filter, and the rows, each marked with the slots that play it and with
    // A and B to load it into either. Hidden or shown for every cabinet at once; whose list and
    // the filter are kept per cabinet. Its height is the one EffectPage.cpp lays it out at:
    // 29 closed, 8 + 26 + 6 + 14 + 6 + 24 + 6 + 220 + 8 = 318 open.
    const std::string nodeId = node.id;
    auto& listState = IrListStates()[nodeId];
    const auto toggleList = [this, nodeId] {
        mShowIrList = !mShowIrList;

        if (mShowIrList)
        {
            IrListStates()[nodeId].sourceSlot = 0; // every slot's list is the library's here
        }

        mEffectSignature.clear();
        Mark(kPage);
    };

    if (!mShowIrList)
    {
        auto toggle = std::make_shared<IrListButton>(mTheme, mIcons, IrListButton::Kind::Toggle);
        toggle->onClick = toggleList;
        return Row({Stretch(), toggle, Stretch()});
    }

    const auto listFor = [this](const guitarfx::GraphNode& n, int slot) {
        const auto* ref = SlotRef(n, slot);
        return OrderedResources(mState, "ir", ref != nullptr ? ref->resourceId : std::string());
    };
    const int source = std::clamp(listState.sourceSlot, 0, 1);

    // The header: whose list, then the cross that closes it.
    std::vector<Ptr> header;

    for (int slot = 0; slot < 2; ++slot)
    {
        auto tab = std::make_shared<IrListButton>(mTheme, mIcons, IrListButton::Kind::Source,
                                                  std::string(slot == 0 ? "IR A" : "IR B") + " \xC2\xB7 Library",
                                                  slot == source);
        tab->onClick = [this, nodeId, slot] {
            IrListStates()[nodeId].sourceSlot = slot;
            mEffectSignature.clear();
            Mark(kPage);
        };
        header.push_back(tab);
        header.push_back(HGap(slot == 0 ? 4.0f : 6.0f));
    }

    auto close = std::make_shared<IrListButton>(mTheme, mIcons, IrListButton::Kind::Close);
    close->onClick = toggleList;
    header.push_back(close);

    const auto list = listFor(node, source);
    auto title = std::make_shared<Text>(mTheme, "Library", 11.0f);
    title->Colour(mTheme.TextSecondary()).Fixed(true);
    auto count = std::make_shared<Text>(mTheme, std::to_string(list.size()), 11.0f);
    count->Colour(mTheme.TextSecondary().opacity(mTheme.TextSecondary().alpha * 0.7f));

    // The rows, refilled in place as the filter is typed into, so the field keeps its focus.
    auto rowsSlot = MakeSlot();
    const auto fillRows = [this, nodeId, listFor, rowsSlot = std::weak_ptr<Slot>(rowsSlot)](bool relayout) {
        const auto slot = rowsSlot.lock();
        const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;

        if (!slot || n == nullptr)
        {
            return;
        }

        const auto& state = IrListStates()[nodeId];
        const auto items = listFor(*n, std::clamp(state.sourceSlot, 0, 1));
        const auto needle = Lower(state.query);
        const auto* a = SlotRef(*n, 0);
        const auto* b = SlotRef(*n, 1);
        const auto plays = [](const guitarfx::ResourceRef* ref, const LibraryResource& r) {
            return ref != nullptr && ((!ref->resourceId.empty() && ref->resourceId == r.id) ||
                                      (ref->IsFilePath() && guitarfx::util::PathToUtf8(ref->filePath) == r.filePath));
        };
        const auto message = [this](const std::string& text) -> Ptr {
            auto t = std::make_shared<Text>(mTheme, text, 11.0f);
            t->Colour(mTheme.TextSecondary());
            return FixedHeight(30, PadXY(8, 0, t));
        };

        std::vector<Ptr> rows;
        std::size_t shown = 0;

        for (const auto* r : items)
        {
            const std::string label = r->name.empty() ? r->id : r->name;

            if (!needle.empty() && Lower(label).find(needle) == std::string::npos)
            {
                continue;
            }

            if (++shown > kIrListMaxRows)
            {
                continue; // counted for the message under the rows
            }

            const bool inA = plays(a, *r);
            const bool inB = plays(b, *r);
            auto name = std::make_shared<Text>(mTheme, label, 11.0f);
            name->Colour(mTheme.Text());
            std::vector<Ptr> cells{name};

            for (const auto& [letter, loaded] : {std::pair{"A", inA}, std::pair{"B", inB}})
            {
                if (loaded)
                {
                    cells.push_back(HGap(4));
                    cells.push_back(Middle(std::make_shared<LoadedBadge>(mTheme, letter)));
                }
            }

            for (int slot = 0; slot < 2; ++slot)
            {
                const std::string slotName = slot == 0 ? "IR A" : "IR B";
                auto assign =
                    std::make_shared<IrListButton>(mTheme, mIcons, IrListButton::Kind::Assign, slot == 0 ? "A" : "B");
                assign->AccessName("Load into " + slotName + ": " + label);
                assign->onClick = [this, nodeId, id = r->id, slot] {
                    mSession.Commands().SetNodeResource(nodeId, "ir", id, slot);
                    NoteChainEdit();
                };
                cells.push_back(HGap(4));
                cells.push_back(Middle(assign));
            }

            Ptr row = Pad(8, 2, 4, 2, Row(cells));

            // A row a slot plays takes the accent's tint.
            if (inA || inB)
            {
                row = Over(row,
                           Rounded(mTheme.Accent().opacity(0.14f), RadiusSm(mTheme), std::make_shared<el::element>()));
            }

            rows.push_back(FixedHeight(26, row));
            rows.push_back(VGap(2));
        }

        if (shown == 0)
        {
            rows.push_back(message(items.empty() ? "No IRs in this list." : "No IRs match the filter."));
        }
        else if (shown > kIrListMaxRows)
        {
            rows.push_back(message("Showing " + std::to_string(kIrListMaxRows) + " of " + std::to_string(shown) +
                                   ". Filter to narrow the list."));
        }

        Fill(*slot, VScroll(Col(rows)));

        if (relayout)
        {
            mSession.View().layout();
            mSession.View().refresh();
        }
    };
    fillRows(false);

    auto [filterElement, filterBox] = el::input_box("Filter", Font(Weight::Regular).size(11.0f), 1.0f);
    filterBox->set_text(listState.query);
    filterBox->on_text = [nodeId, fillRows](std::string_view text) {
        IrListStates()[nodeId].query = std::string(text);
        fillRows(true);
    };
    auto filter = Over(PadXY(8, 0, Middle(el::share(std::move(filterElement)))),
                       el::share(Surface(mTheme.Input(), mTheme.BorderStrong(), RadiusSm(mTheme))));

    // The open list (.ir-slot-list.is-open): an inset panel, 8 px inside, its parts 6 apart.
    return Over(Pad(8, 8, 8, 8,
                    Col({FixedHeight(26, Row(header)), VGap(6), FixedHeight(14, Row({title, HGap(4), count})), VGap(6),
                         FixedHeight(24, filter), VGap(6), FixedHeight(220, rowsSlot)})),
                std::make_shared<SkinBox>(mTheme, &ControlSkin::group));
}
} // namespace nanoq::ui
