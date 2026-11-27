// The effect picker, as the web UI's FX Library: the categories down the side, a search, and the
// effects to add or swap in, with favourites and recents before the categories.

#include "ui/dialogs/DialogChrome.h"
#include "ui/input/Focus.h"
#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/FeatureFlags.h"
#include "uiclient/EffectLayouts.h"
#include "uiclient/FxLibrary.h"
#include "uiclient/FxMarks.h"
#include "uiclient/PresetBrowse.h"
#include "uiclient/NodeLabels.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <utility>

namespace nanoq::ui
{
using guitarfx::uiclient::EffectTypeInfo;
using guitarfx::uiclient::FxLibraryItem;

namespace
{
// The chooser's two lists before the categories (not categories of any effect).
const std::string kFavouritesTab = "__favourites__";
const std::string kRecentTab = "__recent__";
} // namespace

std::vector<FxLibraryItem> Shell::OfferedEffects() const
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
    std::vector<FxLibraryItem> offered;

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

        offered.push_back(guitarfx::uiclient::FxItemForEffect(info));
    }

    // Then each blend, and each saved Custom Effect where Custom Effects are on (catalog.ts).
    for (auto& item : guitarfx::uiclient::BlendFxItems(mState, mPresentation))
    {
        offered.push_back(std::move(item));
    }

    if (showCustom)
    {
        for (auto& item : guitarfx::uiclient::CustomEffectFxItems(mState))
        {
            offered.push_back(std::move(item));
        }
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

void Shell::OpenEffectPickerOnEdge(const guitarfx::GraphEdge& edge, std::optional<el::rect> plus)
{
    OpenEffectPicker(edge.from, {}, plus);
    mPickerEdge = edge; // after: opening the picker forgets any edge a previous one had
}

void Shell::OpenEffectPicker(const std::string& afterNodeId, const std::string& replaceNodeId,
                             std::optional<el::rect> plus)
{
    const bool flyout = plus.has_value() && replaceNodeId.empty();
    mPickerEdge.reset();
    const auto offered = OfferedEffects();

    // Replacing opens on the node's own category, with its effect marked (fxSelector.ts): the
    // same blend or saved Custom Effect, not only the same type.
    std::string currentKey;

    if (!replaceNodeId.empty() && mState.activePreset)
    {
        const auto* node = mState.activePreset->graph.FindNode(replaceNodeId);

        if (node == nullptr || guitarfx::uiclient::IsSplitterType(node->type) ||
            guitarfx::uiclient::IsMixerType(node->type))
        {
            return;
        }

        const auto config = [node](const char* key) {
            const auto it = node->config.find(key);
            return it == node->config.end() ? std::string() : it->second;
        };
        currentKey = guitarfx::uiclient::FxItemKey(node->type, config("blendId"), config("customEffectId"));

        const auto mine = std::find_if(offered.begin(), offered.end(),
                                       [&currentKey](const FxLibraryItem& item) { return item.Key() == currentKey; });

        if (mine != offered.end())
        {
            mPickerCategory = mine->category;
        }
        else if (const auto* type = mState.FindEffectType(node->type))
        {
            mPickerCategory = type->category;
        }
    }

    // Categories that have something in them, in the shared table's order.
    std::vector<std::string> categories;

    for (const auto& id : mPresentation.CategoryOrder())
    {
        if (std::any_of(offered.begin(), offered.end(), [&id](const FxLibraryItem& e) { return e.category == id; }))
        {
            categories.push_back(id);
        }
    }

    for (const auto& effect : offered)
    {
        if (std::find(categories.begin(), categories.end(), effect.category) == categories.end())
        {
            categories.push_back(effect.category);
        }
    }

    if (categories.empty())
    {
        OpenMenu("Add effect", {{"No effects are available", false, true, {}}});
        return;
    }

    // Amps first, as the web's docked library opens (fxSelector.ts).
    if (mPickerCategory.empty() ||
        (mPickerCategory != kFavouritesTab && mPickerCategory != kRecentTab &&
         std::find(categories.begin(), categories.end(), mPickerCategory) == categories.end()))
    {
        mPickerCategory =
            std::find(categories.begin(), categories.end(), "amp") != categories.end() ? "amp" : categories.front();
    }

    // The web UI's Favourites and Recent tabs come first (fxBrowser.ts).
    categories.insert(categories.begin(), {kFavouritesTab, kRecentTab});

    // The flyout opens on Favourites when there are any, else on Recent when it holds anything,
    // else where it was last (addEffectDropdown.ts preferredMarkTab).
    if (flyout)
    {
        const auto anyOffered = [&offered](const std::vector<std::string>& keys) {
            return std::any_of(keys.begin(), keys.end(), [&offered](const std::string& key) {
                return std::any_of(offered.begin(), offered.end(),
                                   [&key](const FxLibraryItem& item) { return item.Key() == key; });
            });
        };

        if (anyOffered(guitarfx::uiclient::FxFavoriteKeys(mState)))
        {
            mPickerCategory = kFavouritesTab;
        }
        else if (anyOffered(guitarfx::uiclient::FxRecentKeys(mState)))
        {
            mPickerCategory = kRecentTab;
        }
        else if (mPickerCategory == kFavouritesTab || mPickerCategory == kRecentTab)
        {
            mPickerCategory = categories[2];
        }
    }

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
        // The cards shown, in order, for Up and Down; the first's effect, for Enter in the search.
        std::vector<std::weak_ptr<EffectRow>> rows;
        std::optional<FxLibraryItem> first;
        std::weak_ptr<el::element> searchBox;
    };

    auto state = std::make_shared<PickerState>();
    auto* s = state.get();
    s->self = state;

    auto choose = [this, weak = s->self, afterNodeId, replaceNodeId, currentKey](const FxLibraryItem& item) {
        if (weak.expired())
        {
            return; // a second click on the closed chooser, before its layer has gone
        }

        CloseOverlay();

        if (item.Key() == currentKey)
        {
            return; // a model or settings are not reset by accident
        }

        guitarfx::uiclient::RecordFxRecent(mSession.Commands(), mState, item.Key());
        ExpectAddedNode();

        if (!replaceNodeId.empty())
        {
            // The replacement is a new node; its controls come up.
            mSession.Commands().ReplaceNode(replaceNodeId, item.type, item.options);
            return;
        }

        if (mPickerEdge)
        {
            mSession.Commands().AddNodeOnEdge(item.type, *mPickerEdge, item.options);
        }
        else
        {
            mSession.Commands().AddNode(item.type, afterNodeId, item.options);
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
    const float height = overEditor ? std::clamp(mEditorArea.height() - 24.0f, 300.0f, 400.0f)
                                    : std::min(460.0f, view.y - metrics::TopBarHeight() - 8.0f - 48.0f);
    // An editor shorter than the panel (the wide frame with the looper strip open, 1024x768):
    // the panel's foot stays on the editor's and it rises over the chain, not over the footer.
    const float top = overEditor ? std::min(mEditorArea.top + 8.0f, mEditorArea.bottom - 16.0f - height)
                                 : metrics::TopBarHeight() + 8.0f;
    const float x = overEditor ? mEditorArea.left + (mEditorArea.width() - width) * 0.5f
                               : StageLeft() + (stageWidth - width) * 0.5f;
    // The effects in columns of cards (.fx-items grid), as many as fit beside the categories;
    // the flyout lists one effect per row.
    const int columns = flyout ? 1 : std::max(1, static_cast<int>((width - 196.0f) / 200.0f));
    const bool replacing = !replaceNodeId.empty();

    s->fill = [this, s, offered, categories, currentKey, choose, columns] {
        const auto query = Lower(s->search);
        s->rows.clear();
        s->first.reset();

        // What the Favourites and Recent tabs list: offered effects, in the lists' own order.
        const auto marked = [&offered](const std::vector<std::string>& keys) {
            std::vector<const FxLibraryItem*> effects;

            for (const auto& key : keys)
            {
                for (const auto& e : offered)
                {
                    if (e.Key() == key)
                    {
                        effects.push_back(&e);
                    }
                }
            }

            return effects;
        };
        std::vector<const FxLibraryItem*> all;

        for (const auto& e : offered)
        {
            all.push_back(&e);
        }

        const auto favourites = marked(guitarfx::uiclient::FxFavoriteKeys(mState));
        const auto recents = marked(guitarfx::uiclient::FxRecentKeys(mState));

        std::vector<Ptr> left;

        for (const auto& id : categories)
        {
            const bool fav = id == kFavouritesTab, recent = id == kRecentTab;
            const auto count =
                fav ? favourites.size()
                : recent
                    ? recents.size()
                    : static_cast<std::size_t>(std::count_if(
                          offered.begin(), offered.end(), [&id](const FxLibraryItem& e) { return e.category == id; }));
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
            // The rows hold the state weakly: it goes when the chooser closes, while the closed
            // layer can still take a click until its posted removal (a second click in the same
            // batch wrote to the freed search text).
            row->onClick = [this, weak = s->self, id] {
                const auto live = weak.lock();

                if (!live)
                {
                    return;
                }

                mPickerCategory = id;
                live->search.clear();
                // Refilled once this click is over: the row being clicked is in the column that
                // the refill replaces, and must outlive its own handler.
                mSession.View().post([alive = std::weak_ptr<bool>(mAlive), this, weak] {
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
                                                                                : all;
        const bool marksTab = &listed != &all;

        for (const auto* listedEffect : listed)
        {
            const auto& effect = *listedEffect;

            // A search looks through every tab, by name, type, category and description
            // (fxBrowser.ts).
            const auto* category = mPresentation.Category(effect.category);
            const bool match = marksTab        ? true
                               : query.empty() ? effect.category == mPickerCategory
                                               : Lower(effect.name + " " + effect.type + " " + effect.category + " " +
                                                       (category != nullptr ? category->name : std::string()) + " " +
                                                       effect.description)
                                                         .find(query) != std::string::npos;

            if (!match)
            {
                continue;
            }

            const auto* look = mPresentation.Category(effect.category);
            std::shared_ptr<cycfi::artist::image> thumb;

            // Its layout's picture when it has one, else a saved Custom Effect's own
            // (fxBrowser.ts itemThumbnail), else its gear's.
            const auto* layout =
                guitarfx::uiclient::DefaultLibraryLayout(mState.layouts.library, effect.type, effect.blendId);
            const auto& picture = layout != nullptr && !layout->thumbnailDataUrl.empty() ? layout->thumbnailDataUrl
                                                                                         : effect.thumbnailDataUrl;

            if (!picture.empty())
            {
                const auto bytes = guitarfx::uiclient::ImageDataUrlBytes(picture);
                thumb = mRemoteImages.Find("thumb:" + std::to_string(std::hash<std::string>{}(picture)), &bytes);
            }

            if (!thumb && (effect.requiresResource || !effect.resourceType.empty()))
            {
                thumb = mImages.Find(mPresentation.EquipmentImage(effect.type, effect.category));
            }

            auto row = std::make_shared<EffectRow>(mTheme, mIcons, mPresentation.IconFor(effect.type, effect.category),
                                                   thumb, effect.name, look != nullptr ? look->name : effect.category,
                                                   Theme::Argb(mPresentation.CategoryColour(effect.category)),
                                                   effect.Key() == currentKey);
            row->onClick = [choose, item = effect] { choose(item); };

            // Dragged onto the chain, it goes where it is let go; replacing too, as the web's.
            row->onDrag = [this, item = effect](el::point, el::point to, bool done) {
                DragLibraryEffect(item, to, done);
            };

            // Up and Down along the cards; Up from the first goes back to the search.
            row->onStep = [this, weak = s->self, index = s->rows.size()](int step) {
                mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), weak, index, step] {
                    const auto live = weak.lock();

                    if (!alive.lock() || !live)
                    {
                        return;
                    }

                    const auto to = static_cast<std::ptrdiff_t>(index) + step;

                    if (to < 0)
                    {
                        if (const auto box = live->searchBox.lock())
                        {
                            Focusable::SetFocusVisible(true);
                            FocusInView(mSession.View(), *box);
                        }
                    }
                    else if (static_cast<std::size_t>(to) < live->rows.size())
                    {
                        if (const auto next = live->rows[static_cast<std::size_t>(to)].lock())
                        {
                            Focusable::SetFocusVisible(true);
                            FocusInView(mSession.View(), *next);
                        }
                    }
                });
            };
            s->rows.push_back(row);

            if (!s->first)
            {
                s->first = effect;
            }

            // What it is beyond an effect (.fx-item-badge): a composite, a blend, a saved Custom
            // Effect, or an effect that needs a model or IR.
            if (effect.type.rfind("composite:", 0) == 0)
            {
                row->SetBadge(EffectRow::Badge::Composite);
            }
            else if (!effect.blendId.empty())
            {
                row->SetBadge(EffectRow::Badge::Blend);
            }
            else if (!effect.customEffectId.empty())
            {
                row->SetBadge(EffectRow::Badge::Custom);
            }
            else if (effect.requiresResource)
            {
                row->SetBadge(EffectRow::Badge::Resource);
            }

            const auto key = effect.Key();
            row->Star(guitarfx::uiclient::IsFxFavorite(mState, key), [this, weak = s->self, key] {
                if (weak.expired())
                {
                    return; // the chooser has closed
                }

                guitarfx::uiclient::ToggleFxFavorite(mSession.Commands(), mState, key);
                // Refilled once this click is over (the row is in the column being replaced).
                mSession.View().post([alive = std::weak_ptr<bool>(mAlive), weak] {
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
                                               !query.empty() ? "No effects match your search"
                                               : !marks       ? "No effects in this category"
                                               : mPickerCategory == kFavouritesTab
                                                   ? "Star an effect to keep it here."
                                                   : "Effects you add to the chain show up here.",
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
    // Replacing, the title is in the accent (.is-replacing).
    auto heading = std::make_shared<Text>(mTheme, title, 13.0f, Weight::SemiBold);
    heading->Colour(replacing ? mTheme.Accent() : mTheme.Ink());
    auto hint =
        std::make_shared<Text>(mTheme,
                               replacing ? "Click an effect to use it instead, or drag one anywhere in the chain."
                                         : "Drag an effect into the signal chain, or click one to add it.",
                               11.5f);
    hint->Colour(mTheme.TextSecondary());
    auto close = std::make_shared<Button>(mTheme, "\xC3\x97  Close", [this] { CloseOverlay(); }, Button::Style::Plain);
    close->MinWidth(80).TextSize(12.0f);

    auto [searchElement, searchBox] = el::input_box("Search effects...", Font(Weight::Regular), 1.0f);
    s->searchBox = searchBox;
    // Enter takes the first match; Down goes into the list (fxBrowser.ts onSearchKey).
    searchBox->on_enter = [weak = s->self, choose](std::string_view) {
        const auto live = weak.lock();

        if (live && !live->search.empty() && live->first)
        {
            choose(*live->first);
        }

        return true;
    };
    auto searchHook = std::make_shared<KeyHook>(el::share(std::move(searchElement)));
    searchHook->onKey = [this, weak = s->self](el::key_info k) {
        const auto live = weak.lock();

        if (!live || k.key != el::key_code::down || k.action == el::key_action::release || live->rows.empty())
        {
            return false;
        }

        mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), weak] {
            const auto state = weak.lock();

            if (alive.lock() && state && !state->rows.empty())
            {
                if (const auto first = state->rows.front().lock())
                {
                    Focusable::SetFocusVisible(true);
                    FocusInView(mSession.View(), *first);
                }
            }
        });
        return true;
    };
    searchBox->on_text = [weak = s->self](std::string_view text) {
        const auto live = weak.lock();

        if (live && live->fill)
        {
            live->search = std::string(text);
            live->fill();
        }
    };

    auto search = Over(PadXY(10, 0, Middle(searchHook)), el::share(Surface(mTheme.Input(), mTheme.Border(), 6.0f)));

    // From a cable's +: the small flyout there (.fx-flyout), its title and search in one band
    // over a narrower rail and one effect per row. Under the +, or over it where there is no
    // room below.
    if (flyout)
    {
        const float flyWidth = std::min(560.0f, view.x - 16.0f);
        const float flyHeight = std::min(440.0f, view.y - 16.0f);
        const bool above = plus->bottom + 5.0f + flyHeight > view.y - 8.0f;
        auto flyTitle = std::make_shared<Text>(mTheme, "ADD EFFECT", 12.0f, Weight::Title);
        flyTitle->Colour(mTheme.Ink()).Tracking(0.08f).Fixed(true);
        auto band = Over(Pad(14, 6, 8, 6, Row({Middle(flyTitle), HGap(10), Stretch(FixedHeight(30, search))})),
                         el::share(Surface(mTheme.Bar(), mTheme.ModalRule(), 0.0f)));
        auto flyPanel =
            Col({FixedHeight(44, band), Row({FixedWidth(164, Pad(6, 6, 6, 6, s->left)), Pad(8, 8, 8, 0, s->right)})});
        ShowPopover(flyPanel, *plus, flyWidth, flyHeight, above);
        mOnOverlayClosed = [state] { state->fill = nullptr; };
        return;
    }

    auto header =
        Row({Col({FixedHeight(18, heading), FixedHeight(18, hint)}), Stretch(), Middle(FixedHeight(32, close))});
    auto panel =
        Pad(14, 10, 12, 12,
            Col({FixedHeight(40, header), VGap(10),
                 Row({FixedWidth(208, s->left), HGap(12), Col({FixedHeight(34, search), VGap(10), s->right})})}));
    ShowPopover(panel, el::rect{x, top, x + width, top}, width, height);
    // The state lives as long as the chooser.
    mOnOverlayClosed = [state] { state->fill = nullptr; };
}
} // namespace nanoq::ui
