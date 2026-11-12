// The effect picker, as the web UI's FX Library: the categories down the side, a search, and the
// effects to add or swap in, with favourites and recents before the categories.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/FeatureFlags.h"
#include "uiclient/FxMarks.h"
#include "uiclient/PresetBrowse.h"
#include "uiclient/NodeLabels.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

namespace nanoq::ui
{
using guitarfx::uiclient::EffectTypeInfo;

namespace
{
// The chooser's two lists before the categories (not categories of any effect).
const std::string kFavouritesTab = "__favourites__";
const std::string kRecentTab = "__recent__";
} // namespace

std::vector<const EffectTypeInfo*> Shell::OfferedEffects() const
{
    namespace guids = guitarfx::EffectGuids;
    const std::set<std::string> experimental{guids::kTransposeStft, guids::kTransposeHybrid, guids::kEqPitchTracking};
    using guitarfx::uiclient::Feature;
    using guitarfx::uiclient::IsFeatureEnabled;
    const bool showExperimental = IsFeatureEnabled(mState, Feature::ExperimentalEffects);
    const bool showCustom = IsFeatureEnabled(mState, Feature::CustomEffects);
    const bool showPickupMatch = IsFeatureEnabled(mState, Feature::PickupMatch);

    // Guitar to MIDI only plays a plugin host's instrument: offered where the engine has one.
    const bool canHostPlugins = mState.FindEffectType(guids::kPluginHost) != nullptr;
    std::vector<const EffectTypeInfo*> offered;

    for (const auto& info : mState.catalog)
    {
        // As core/ui/ts/fxLibrary/catalog.ts: the catalog's effects and its composites (a
        // composite channel strip is added as any effect). The plugin host (and Guitar to MIDI,
        // which only plays a plugin host's instrument) is offered where the engine can host
        // plugins: the catalog lists it only there.
        if (info.type == guids::kMixer || info.type == guids::kAmpNamBlend)
        {
            continue;
        }

        if (info.type == guids::kGuitarToMidi && !canHostPlugins)
        {
            continue;
        }

        if (info.type == guids::kWasmHost && !showCustom)
        {
            continue;
        }

        // Pickup Match is experimental, behind a feature of its own.
        if (info.type == guids::kPickupMatch && !showPickupMatch)
        {
            continue;
        }

        if (experimental.count(info.type) > 0 && !showExperimental)
        {
            continue;
        }

        offered.push_back(&info);
    }

    return offered;
}

void Shell::ExpectAddedNode()
{
    // The node that appears next is the one to show.
    mSelectAddedUntil = UiSession::Now() + 5.0;
    mNodesBeforeAdd.clear();

    if (mState.activePreset)
    {
        for (const auto& n : mState.activePreset->graph.nodes)
        {
            mNodesBeforeAdd.insert(n.id);
        }
    }
}

void Shell::OpenEffectPickerOnEdge(const guitarfx::GraphEdge& edge)
{
    OpenEffectPicker(edge.from);
    mPickerEdge = edge; // after: opening the picker forgets any edge a previous one had
}

void Shell::OpenEffectPicker(const std::string& afterNodeId, const std::string& replaceNodeId)
{
    mPickerEdge.reset();
    const auto offered = OfferedEffects();

    // Replacing opens on the node's own category, with its effect marked (fxSelector.ts).
    std::string currentType;

    if (!replaceNodeId.empty() && mState.activePreset)
    {
        const auto* node = mState.activePreset->graph.FindNode(replaceNodeId);

        if (node == nullptr || guitarfx::uiclient::IsSplitterType(node->type) ||
            guitarfx::uiclient::IsMixerType(node->type))
        {
            return;
        }

        currentType = node->type;

        if (const auto* type = mState.FindEffectType(node->type))
        {
            mPickerCategory = type->category;
        }
    }

    // Categories that have something in them, in the shared table's order.
    std::vector<std::string> categories;

    for (const auto& id : mPresentation.CategoryOrder())
    {
        if (std::any_of(offered.begin(), offered.end(), [&id](const EffectTypeInfo* e) { return e->category == id; }))
        {
            categories.push_back(id);
        }
    }

    for (const auto* effect : offered)
    {
        if (std::find(categories.begin(), categories.end(), effect->category) == categories.end())
        {
            categories.push_back(effect->category);
        }
    }

    if (categories.empty())
    {
        OpenMenu("Add effect", {{"No effects are available", false, true, {}}});
        return;
    }

    if (mPickerCategory.empty() ||
        (mPickerCategory != kFavouritesTab && mPickerCategory != kRecentTab &&
         std::find(categories.begin(), categories.end(), mPickerCategory) == categories.end()))
    {
        mPickerCategory = categories.front();
    }

    // The web UI's Favourites and Recent tabs come first (fxBrowser.ts).
    categories.insert(categories.begin(), {kFavouritesTab, kRecentTab});

    // The web UI's chooser (fx-selector-panel.html): a title and a search over two columns, the
    // categories with their counts at the left and the chosen one's effects at the right. Both
    // columns are slots refilled in place, so the search box keeps its focus while typing.
    struct PickerState
    {
        std::string search;
        SlotPtr left = MakeSlot();
        SlotPtr right = MakeSlot();
        std::function<void()> fill;
        std::weak_ptr<PickerState> self;
    };

    auto state = std::make_shared<PickerState>();
    auto* s = state.get();
    s->self = state;

    auto choose = [this, afterNodeId, replaceNodeId, currentType](const std::string& type) {
        CloseOverlay();

        if (type == currentType)
        {
            return; // a model or settings are not reset by accident
        }

        guitarfx::uiclient::RecordFxRecent(mSession.Commands(), mState, guitarfx::uiclient::FxItemKey(type));
        ExpectAddedNode();

        if (!replaceNodeId.empty())
        {
            // The replacement is a new node; its controls come up.
            mSession.Commands().ReplaceNode(replaceNodeId, type);
            return;
        }

        if (mPickerEdge)
        {
            mSession.Commands().AddNodeOnEdge(type, *mPickerEdge);
        }
        else
        {
            mSession.Commands().AddNode(type, afterNodeId);
        }
    };

    // The panel's size: over a split stage's editor (the web UI's FX Library), else under the
    // preset bar, filling the stage.
    const auto view = ViewSize();
    const float stageWidth = view.x - StageLeft();
    // The compact frame's split stage has a stage-high panel to fall back on, so its editor must
    // hold the panel's least height (300) to be used: a shorter one put the panel over the footer.
    const bool overEditor = PlayStageShown() && mEditorArea.height() > (mWide ? 200.0f : 324.0f);
    const float width =
        overEditor ? std::min(860.0f, mEditorArea.width() - 24.0f) : std::min(860.0f, stageWidth - 24.0f);
    const float top = overEditor ? mEditorArea.top + 8.0f : metrics::TopBarHeight() + 8.0f;
    const float height =
        overEditor ? std::clamp(mEditorArea.height() - 24.0f, 300.0f, 400.0f) : std::min(460.0f, view.y - top - 48.0f);
    const float x = overEditor ? mEditorArea.left + (mEditorArea.width() - width) * 0.5f
                               : StageLeft() + (stageWidth - width) * 0.5f;
    // The effects in columns of cards (.fx-items grid), as many as fit beside the categories.
    const int columns = std::max(1, static_cast<int>((width - 196.0f) / 200.0f));
    const bool replacing = !replaceNodeId.empty();

    s->fill = [this, s, offered, categories, currentType, choose, columns, replacing] {
        const auto query = Lower(s->search);

        // What the Favourites and Recent tabs list: offered effects, in the lists' own order.
        const auto marked = [&offered](const std::vector<std::string>& keys) {
            std::vector<const EffectTypeInfo*> effects;

            for (const auto& key : keys)
            {
                for (const auto* e : offered)
                {
                    if (guitarfx::uiclient::FxItemKey(e->type) == key)
                    {
                        effects.push_back(e);
                    }
                }
            }

            return effects;
        };
        const auto favourites = marked(guitarfx::uiclient::FxFavoriteKeys(mState));
        const auto recents = marked(guitarfx::uiclient::FxRecentKeys(mState));

        std::vector<Ptr> left;

        for (const auto& id : categories)
        {
            const bool fav = id == kFavouritesTab, recent = id == kRecentTab;
            const auto count = fav      ? favourites.size()
                               : recent ? recents.size()
                                        : static_cast<std::size_t>(std::count_if(
                                              offered.begin(), offered.end(),
                                              [&id](const EffectTypeInfo* e) { return e->category == id; }));
            const auto* look = (fav || recent) ? nullptr : mPresentation.Category(id);
            auto row = std::make_shared<CategoryRow>(mTheme, mIcons,
                                                     fav               ? std::string("star")
                                                     : recent          ? std::string("ui-clock")
                                                     : look != nullptr ? look->icon
                                                                       : std::string("gear"),
                                                     fav               ? std::string("Favourites")
                                                     : recent          ? std::string("Recent")
                                                     : look != nullptr ? look->name
                                                                       : id,
                                                     static_cast<int>(count),
                                                     (fav || recent) ? mTheme.Accent()
                                                                     : Theme::Argb(mPresentation.CategoryColour(id)),
                                                     query.empty() && id == mPickerCategory);
            row->onClick = [this, s, id] {
                mPickerCategory = id;
                s->search.clear();
                // Refilled once this click is over: the row being clicked is in the column that
                // the refill replaces, and must outlive its own handler.
                mSession.View().post([alive = std::weak_ptr<bool>(mAlive), this, weak = s->self] {
                    const auto state = weak.lock();

                    if (alive.lock() && state && state->fill)
                    {
                        state->fill();
                    }
                });
            };
            left.push_back(row);
            left.push_back(VGap(2));
        }

        std::vector<Ptr> right;
        const auto& listed = query.empty() && mPickerCategory == kFavouritesTab ? favourites
                             : query.empty() && mPickerCategory == kRecentTab   ? recents
                                                                                : offered;
        const bool marksTab = &listed != &offered;

        for (const auto* effect : listed)
        {
            const bool match = marksTab        ? true
                               : query.empty() ? effect->category == mPickerCategory
                                               : Lower(effect->name).find(query) != std::string::npos;

            if (!match)
            {
                continue;
            }

            const auto* look = mPresentation.Category(effect->category);
            std::shared_ptr<cycfi::artist::image> thumb;

            if (effect->requiresResource || !effect->resourceType.empty())
            {
                thumb = mImages.Find(mPresentation.EquipmentImage(effect->type, effect->category));
            }

            auto row = std::make_shared<EffectRow>(
                mTheme, mIcons, mPresentation.IconFor(effect->type, effect->category), thumb, effect->name,
                look != nullptr ? look->name : effect->category,
                Theme::Argb(mPresentation.CategoryColour(effect->category)), effect->type == currentType);
            row->onClick = [choose, type = effect->type] { choose(type); };

            // Dragged onto the chain, it goes where it is let go.
            if (!replacing)
            {
                row->onDrag = [this, type = effect->type, name = effect->name](el::point, el::point to, bool done) {
                    DragLibraryEffect(type, name, to, done);
                };
            }

            if (effect->type.rfind("composite:", 0) == 0)
            {
                row->SetBadge(EffectRow::Badge::Composite);
            }
            else if (effect->requiresResource)
            {
                row->SetBadge(EffectRow::Badge::Resource);
            }

            const auto key = guitarfx::uiclient::FxItemKey(effect->type);
            row->Star(guitarfx::uiclient::IsFxFavorite(mState, key), [this, s, key] {
                guitarfx::uiclient::ToggleFxFavorite(mSession.Commands(), mState, key);
                // Refilled once this click is over (the row is in the column being replaced).
                mSession.View().post([alive = std::weak_ptr<bool>(mAlive), weak = s->self] {
                    const auto state = weak.lock();

                    if (alive.lock() && state && state->fill)
                    {
                        state->fill();
                    }
                });
            });
            right.push_back(row);
        }

        // In rows of `columns` cards.
        std::vector<Ptr> grid;

        for (std::size_t i = 0; i < right.size(); i += static_cast<std::size_t>(columns))
        {
            std::vector<Ptr> cells;

            for (std::size_t j = i; j < i + static_cast<std::size_t>(columns); ++j)
            {
                if (!cells.empty())
                {
                    cells.push_back(HGap(8));
                }

                cells.push_back(j < right.size() ? right[j] : Ptr(std::make_shared<el::element>()));
            }

            grid.push_back(Row(cells));
            grid.push_back(VGap(8));
        }

        right = std::move(grid);

        if (right.empty())
        {
            const bool marks = query.empty() && (mPickerCategory == kFavouritesTab || mPickerCategory == kRecentTab);
            auto none = std::make_shared<Text>(mTheme,
                                               !marks                              ? "No effects match."
                                               : mPickerCategory == kFavouritesTab ? "Star an effect to keep it here."
                                                                                   : "The effects you add show here.",
                                               12.0f);
            none->Colour(mTheme.TextMuted());
            right.push_back(FixedHeight(30, PadXY(8, 0, none)));
        }

        Fill(*s->left, VScroll(Col(left)));
        Fill(*s->right, VScroll(Pad(0, 0, 10, 0, Col(right))));
        mSession.View().layout();
        mSession.View().refresh();
    };
    s->fill();

    std::string title = "FX LIBRARY";

    if (!replaceNodeId.empty() && mState.activePreset)
    {
        if (const auto* node = mState.activePreset->graph.FindNode(replaceNodeId))
        {
            title = Upper("Replace " + guitarfx::uiclient::NodeDisplayName(mState, *node));
        }
    }

    // The header (.fx-library-header): the title, what to do, and Close.
    auto heading = std::make_shared<Text>(mTheme, title, 13.0f, Weight::SemiBold);
    heading->Colour(mTheme.Ink());
    auto hint = std::make_shared<Text>(mTheme,
                                       replacing ? "Click an effect to put it in its place."
                                                 : "Drag an effect into the signal chain, or click one to add it.",
                                       11.5f);
    hint->Colour(mTheme.TextSecondary());
    auto close = std::make_shared<Button>(mTheme, "\xC3\x97  Close", [this] { CloseOverlay(); }, Button::Style::Plain);
    close->MinWidth(80).TextSize(12.0f);

    auto [searchElement, searchBox] = el::input_box("Search effects...", Font(Weight::Regular), 1.0f);
    searchBox->on_text = [s](std::string_view text) {
        s->search = std::string(text);
        s->fill();
    };

    auto header =
        Row({Col({FixedHeight(18, heading), FixedHeight(18, hint)}), Stretch(), Middle(FixedHeight(32, close))});
    auto search = Over(PadXY(10, 0, Middle(el::share(std::move(searchElement)))),
                       el::share(Surface(mTheme.Input(), mTheme.Border(), 6.0f)));
    auto panel =
        Pad(14, 10, 12, 12,
            Col({FixedHeight(40, header), VGap(10),
                 Row({FixedWidth(176, s->left), HGap(12), Col({FixedHeight(34, search), VGap(10), s->right})})}));
    ShowPopover(panel, el::rect{x, top, x + width, top}, width, height);
    // The state lives as long as the chooser.
    mOnOverlayClosed = [state] { state->fill = nullptr; };
}
} // namespace nanoq::ui
