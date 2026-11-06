// What floats over the page: a pop-up list, a text prompt, a confirmation and the effect picker.
// One at a time, over a dimmed view; a tap outside, or a choice, closes it.

#include "ui/DialogChrome.h"
#include "ui/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/FeatureFlags.h"
#include "uiclient/FxMarks.h"
#include "uiclient/PresetBrowse.h"
#include "uiclient/PresetInspect.h"
#include "uiclient/ToneSharing.h"
#include "uiclient/NodeLabels.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>
#include <set>

namespace nanoq::ui
{
using guitarfx::uiclient::EffectTypeInfo;

namespace
{
// The chooser's two lists before the categories (not categories of any effect).
const std::string kFavouritesTab = "__favourites__";
const std::string kRecentTab = "__recent__";
} // namespace

namespace
{
std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
} // namespace

void Shell::ApplyElementsTheme()
{
    // Elements draws its own text boxes and scroll bars from one process-wide theme.
    auto t = el::get_theme();
    t.text_box_font = Font(Weight::Regular).size(metrics::kTextSize);
    t.text_box_font_color = mTheme.Text();
    t.text_box_hilite_color = mTheme.Accent().opacity(0.35f);
    t.text_box_caret_color = mTheme.Accent();
    t.inactive_font_color = mTheme.TextMuted();
    t.frame_color = mTheme.BorderStrong();
    t.frame_hilite_color = mTheme.Accent();
    t.scrollbar_color = mTheme.TextMuted().opacity(0.45f);
    t.scrollbar_width = 7.0f;
    t.panel_color = mTheme.Panel();
    el::set_theme(t);
}

void Shell::ShowOverlay(Ptr panel, float width, float height)
{
    CloseOverlay();

    auto face = Over(std::move(panel), el::share(Surface(mTheme.Elevated(), mTheme.BorderStrong(), 12.0f)));
    // Expand: a column is only as wide as its narrowest row allows, and a row of fixed-size
    // buttons allows little; the dialog's width is the one given.
    auto centred = el::share(
        el::align_center(el::align_middle(el::hold_any(FixedWidth(width, FixedHeight(height, Expand(face)))))));
    mOverlay = KeyLayer(Over(centred, el::share(Backdrop([this] { CancelOverlay(); }))));

    mSession.View().add(mOverlay);
}

void Shell::ShowDialog(DialogParts parts)
{
    // The header: the title, anything the dialog adds there, and the close cross.
    auto title = std::make_shared<Text>(mTheme, parts.title, 16.0f, Weight::SemiBold);
    title->Colour(mTheme.ModalText());
    auto close = std::make_shared<CloseCross>(mTheme);
    auto cancel = parts.onClose; // Esc and a tap outside do the same as the cross
    close->onClick = parts.onClose ? std::move(parts.onClose) : std::function<void()>([this] { CloseOverlay(); });
    std::vector<Ptr> header{Stretch(title)};

    for (auto& extra : parts.headerExtras)
    {
        header.push_back(HGap(12));
        header.push_back(Middle(std::move(extra)));
    }

    header.push_back(HGap(10));
    header.push_back(close);
    // Middle: a fixed height is no taller than what it holds allows, and a row of text is short.
    std::vector<Ptr> column{FixedHeight(kDialogHeaderHeight, Pad(20, 0, 12, 0, Middle(Row(header))))};

    const auto view = ViewSize();
    const float footer = parts.actions.empty() && !parts.footerLeft ? 0.0f : kDialogFooterHeight;
    const float height = std::min(parts.height, std::max(200.0f, view.y - 40.0f));
    const float width = std::min(parts.width, std::max(280.0f, view.x - 32.0f));
    Ptr body = Pad(parts.padX, parts.padY, parts.padX, parts.padY, std::move(parts.body));

    // A window too short for the dialog scrolls its body under the header; a body that fills
    // the dialog scrolls in itself.
    if (height < parts.height && !parts.stretch)
    {
        body = VScroll(body);
    }

    column.push_back(body);

    // Takes any height the body leaves, so the card fills the dialog and the footer sits at its
    // foot: a column of fixed rows is otherwise only as tall as its rows.
    if (!parts.stretch)
    {
        column.push_back(std::make_shared<el::element>());
    }

    if (footer > 0.0f)
    {
        std::vector<Ptr> row{parts.footerLeft ? Stretch(Middle(std::move(parts.footerLeft))) : Stretch()};

        for (auto& action : parts.actions)
        {
            row.push_back(HGap(12));
            row.push_back(Middle(std::move(action)));
        }

        column.push_back(FixedHeight(footer, Pad(20, 0, 20, 0, Row(row))));
    }

    ShowOverlay(Over(Col(column), std::make_shared<DialogFace>(mTheme, footer)), width, height);
    mOverlayCancel = std::move(cancel);
}

void Shell::CancelOverlay()
{
    if (mOverlayCancel)
    {
        const auto cancel = mOverlayCancel; // it closes the overlay, which forgets it
        cancel();
        return;
    }

    CloseOverlay();
}

std::shared_ptr<Button> Shell::DialogButton(const std::string& label, std::function<void()> onClick, bool primary)
{
    auto button = std::make_shared<Button>(mTheme, label, std::move(onClick),
                                           primary ? Button::Style::Primary : Button::Style::Plain);
    button->MinWidth(88);
    return button;
}

void Shell::CloseOverlay()
{
    CloseDropdown();
    mOverlayCancel = nullptr;

    // A dialog with live feeds (tuner, metronome) stops listening, and tells the engine it is
    // done, whether it was closed by its button or by a tap outside it.
    mInstrumentSubs.clear();

    if (auto closed = std::move(mOnOverlayClosed))
    {
        mOnOverlayClosed = nullptr;
        closed();
    }

    if (!mOverlay)
    {
        return;
    }

    auto overlay = std::move(mOverlay);
    mOverlay.reset();
    RemoveLayer(std::move(overlay));
}

void Shell::RemoveLayer(Ptr layer)
{
    if (!layer)
    {
        return;
    }

    // Removing it from inside one of its own click handlers would pull it out from under the
    // event that is still being delivered, so it goes once that event is done: in one posted
    // step, which keeps it ahead of a layer the same handler adds (a dialog opened from the
    // Controls strip replaces it). Elements' view::remove posts its own step, so the new layer
    // went in first; and it keeps the focus index only when the layer it takes out had the
    // focus, so taking out one below the focused layer left the index one past it, and the next
    // key was delivered past the end of the layers.
    auto& view = mSession.View();
    view.post([&view, layer] {
        auto& layers = view.content();
        const auto found = std::find(layers.begin(), layers.end(), layer);

        if (found == layers.end())
        {
            return;
        }

        const auto index = static_cast<int>(found - layers.begin());

        if (layers.focus_index() == index)
        {
            view.relinquish_focus();
        }

        const int focus = layers.focus_index();
        layers.erase(found);

        if (focus > index)
        {
            layers.focus(static_cast<std::size_t>(focus - 1));
        }

        view.layout();
        view.refresh();
    });
}

namespace
{
float DialogWidth(const el::extent& view, float wanted)
{
    return std::min(wanted, std::max(240.0f, view.x - 48.0f));
}
} // namespace

void Shell::OpenMenu(const std::string& title, std::vector<MenuItem> items)
{
    std::vector<Ptr> rows;

    for (auto& item : items)
    {
        auto choose = item.onChoose;
        auto row = std::make_shared<ListRow>(mTheme, item.label, item.detail, item.selected, item.heading || !choose,
                                             [this, choose] {
                                                 // Close first: the choice may open another dialog.
                                                 CloseOverlay();

                                                 if (choose)
                                                 {
                                                     choose();
                                                 }
                                             });

        if (!item.icon.empty())
        {
            row->Icon(&mIcons, item.icon, item.tint != 0 ? Theme::Argb(item.tint) : mTheme.TextSecondary());
        }

        rows.push_back(row);
    }

    const auto view = ViewSize();
    const float wanted = kDialogHeaderHeight + 20.0f + static_cast<float>(items.size()) * metrics::kRowHeight;
    const float height = std::clamp(wanted, 140.0f, std::max(140.0f, view.y - 80.0f));
    ShowDialog({.title = title,
                .body = VScroll(Col(rows)),
                .width = DialogWidth(view, 420.0f),
                .height = height,
                .padX = 10.0f,
                .padY = 10.0f});
}

void Shell::OpenPrompt(const std::string& title, const std::string& initial, const std::string& confirmLabel,
                       std::function<void(const std::string&)> onSubmit)
{
    auto [boxElement, box] = el::input_box("Name", Font(Weight::Regular), 1.0f);
    box->set_text(initial);

    auto submit = [this, box, onSubmit] {
        auto text = box->get_utf8();
        // Trim: a name that is only spaces is no name.
        const auto first = text.find_first_not_of(" \t");
        const auto last = text.find_last_not_of(" \t");
        text = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);

        if (text.empty())
        {
            return;
        }

        CloseOverlay();
        onSubmit(text);
    };

    box->on_enter = [submit](std::string_view) {
        submit();
        return true;
    };
    box->on_escape = [this] { CloseOverlay(); };

    ShowDialog({.title = title,
                .body = Col({FixedHeight(40, el::share(std::move(boxElement))), Stretch()}),
                .actions = {DialogButton(
                                "Cancel", [this] { CloseOverlay(); }, false),
                            DialogButton(confirmLabel, submit, true)},
                .width = DialogWidth(ViewSize(), 420.0f),
                .height = kDialogHeaderHeight + kDialogFooterHeight + 76.0f});
}

void Shell::OpenConfirm(const std::string& title, const std::string& detail, const std::string& confirmLabel,
                        std::function<void()> onYes)
{
    auto body =
        std::make_shared<Paragraph>(mTheme, detail, DialogWidth(ViewSize(), 460.0f) - 40.0f, metrics::kTextSize);
    body->Colour(mTheme.ModalTextSecondary());
    auto yes = [this, onYes] {
        CloseOverlay();
        onYes();
    };
    ShowDialog({.title = title,
                .body = Col({body, Stretch()}),
                .actions = {DialogButton(
                                "Cancel", [this] { CloseOverlay(); }, false),
                            DialogButton(confirmLabel, yes, true)},
                .width = DialogWidth(ViewSize(), 460.0f),
                .height = kDialogHeaderHeight + kDialogFooterHeight + 86.0f});
}

void Shell::ShowToast(const std::string& title, const std::string& detail, bool isError)
{
    auto& view = mSession.View();

    if (mToast)
    {
        RemoveLayer(std::exchange(mToast, nullptr));
    }

    // At the bottom, centred, above the transport bar, or above the on-screen keyboard. It takes
    // no clicks.
    const float above = mKeyboard && mKeyboard->Showing() ? OnScreenKeyboard::PanelHeight(ViewSize().y) + 8.0f : 64.0f;
    auto toast = std::make_shared<Toast>(mTheme, title, detail, isError);
    mToast = el::share(el::align_bottom(el::align_center(el::margin({0.0f, 0.0f, 0.0f, above}, el::hold_any(toast)))));
    view.add(mToast, false);

    view.post(std::chrono::seconds(isError ? 8 : 4), [&view, shown = mToast, alive = mAlive, this] {
        if (!*alive)
        {
            return;
        }

        RemoveLayer(shown);

        if (mToast == shown)
        {
            mToast = nullptr;
        }
    });
}

// ── The effect picker ───────────────────────────────────────────────────────────

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

    // The panel's size: over the full layout's editor (the web UI's FX Library), else under the
    // preset bar, filling the stage.
    const auto view = ViewSize();
    const float stageWidth = view.x - StageLeft();
    const bool overEditor = mWide && mEditorArea.height() > 200.0f;
    const float width =
        overEditor ? std::min(860.0f, mEditorArea.width() - 24.0f) : std::min(860.0f, stageWidth - 24.0f);
    const float top = overEditor ? mEditorArea.top + 8.0f : metrics::kTopBarHeight + 8.0f;
    const float height =
        overEditor ? std::clamp(mEditorArea.height() - 24.0f, 300.0f, 400.0f) : std::min(460.0f, view.y - top - 48.0f);
    const float x = overEditor ? mEditorArea.left + (mEditorArea.width() - width) * 0.5f
                               : StageLeft() + (stageWidth - width) * 0.5f;
    // The effects in columns of cards (.fx-items grid), as many as fit beside the categories.
    const int columns = std::max(1, static_cast<int>((width - 196.0f) / 200.0f));
    const bool replacing = !replaceNodeId.empty();

    s->fill = [this, s, offered, categories, currentType, choose, columns, replacing] {
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        const auto query = lower(s->search);

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
                                               : lower(effect->name).find(query) != std::string::npos;

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

void Shell::EditActivePreset()
{
    if (mState.activePreset && guitarfx::uiclient::CanModifyActivePreset(mState))
    {
        OpenPresetDetails(false);
    }
}

void Shell::OpenPresetDetails(bool asNew)
{
    // The web UI's save dialog (save-preset-modal.html). Details: the folder it is filed in, its
    // name, category, description and tags, and the input peak it was designed at. Advanced: the
    // preset's JSON with Validate and Clean Up (presets/inspectModal.ts). Saving as new names the
    // copy; otherwise it saves over the preset playing.
    if (!mState.activePreset)
    {
        return;
    }

    using guitarfx::uiclient::PresetFolder;
    const auto& preset = *mState.activePreset;
    const auto* summary = mState.FindPresetSummary(mState.activePresetId);
    const bool userPreset = summary != nullptr && summary->source == "user";

    struct DetailsState
    {
        std::vector<std::string> tags;
        std::string folderId; // "" for none
        std::optional<double> stagedPeak;
        std::optional<nlohmann::json> cleaned;
        nlohmann::json json;
        guitarfx::uiclient::PeakHold hold;
        int tab = 0;
    };

    auto st = std::make_shared<DetailsState>();
    st->tags = preset.tags;
    st->json = guitarfx::uiclient::InspectablePresetJson(preset);

    // Saved as new it goes in the folder being browsed; otherwise it stays in its own.
    const std::function<const PresetFolder*(const std::vector<PresetFolder>&)> holding =
        [&](const std::vector<PresetFolder>& folders) -> const PresetFolder* {
        for (const auto& f : folders)
        {
            if (std::find(f.presetIds.begin(), f.presetIds.end(), mState.activePresetId) != f.presetIds.end())
            {
                return &f;
            }

            if (const auto* inner = holding(f.children))
            {
                return inner;
            }
        }

        return nullptr;
    };
    const auto* current = asNew ? guitarfx::uiclient::FindPresetFolder(mState.presetFolders, mQuery.folderId)
                                : holding(mState.presetFolders);

    if (current != nullptr)
    {
        st->folderId = current->id;
    }

    const auto field = [](const char* placeholder, const std::string& text) {
        auto [element, box] = el::input_box(placeholder, Font(Weight::Regular), 1.0f);
        box->set_text(text);
        return std::make_pair(el::share(std::move(element)), box);
    };
    auto [nameElement, nameBox] = field("My Custom Preset", asNew && userPreset ? preset.name + " copy" : preset.name);
    auto [categoryElement, categoryBox] = field("User", preset.category);
    auto [descriptionElement, descriptionBox] = field("Optional description...", preset.description);

    // A form label (.form-group label): small, bold, in capitals.
    const auto label = [this](const std::string& text) {
        auto t = std::make_shared<Text>(mTheme, Upper(text), 11.0f, Weight::SemiBold);
        t->Colour(mTheme.ModalText());
        return FixedHeight(22, Middle(t));
    };

    // ── Save Folder ──
    auto folder = std::make_shared<PickerField>(mTheme, mIcons);
    folder->AccessName("Save Folder");
    folder->Set(current != nullptr ? current->name : std::string("No folder"));
    folder->onClick = [this, st, weak = std::weak_ptr<PickerField>(folder)] {
        const auto picker = weak.lock();

        if (!picker)
        {
            return;
        }

        const auto choose = [this, st, weak](const std::string& id, const std::string& name) {
            st->folderId = id;

            if (const auto p = weak.lock())
            {
                p->Set(name);
                mSession.Repaint(*p);
            }
        };
        std::vector<MenuItem> items;
        items.push_back({"No folder", st->folderId.empty(), false, [choose] { choose({}, "No folder"); }});
        std::function<void(const std::vector<PresetFolder>&, int)> add = [&](const std::vector<PresetFolder>& folders,
                                                                             int depth) {
            for (const auto& f : folders)
            {
                items.push_back({std::string(static_cast<std::size_t>(depth) * 2, ' ') + f.name, f.id == st->folderId,
                                 false, [choose, id = f.id, name = f.name] { choose(id, name); }});
                add(f.children, depth + 1);
            }
        };
        add(mState.presetFolders, 0);
        OpenDropdown(picker->Bounds(), std::move(items), std::max(260.0f, picker->Bounds().width()));
    };

    // ── Tags: the standard ones, and any others the preset already has ──
    auto tagSlot = MakeSlot();
    auto fillTags = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakFill = fillTags;
    *fillTags = [this, st, tagSlot, weakFill] {
        auto all = guitarfx::uiclient::tones::StandardTags();

        for (const auto& t : st->tags)
        {
            if (std::find(all.begin(), all.end(), t) == all.end())
            {
                all.push_back(t);
            }
        }

        std::vector<Ptr> rows, row;
        float used = 0.0f;

        for (const auto& tag : all)
        {
            const bool on = std::find(st->tags.begin(), st->tags.end(), tag) != st->tags.end();
            auto chip = std::make_shared<Button>(
                mTheme, tag,
                [this, st, tag, on, weakFill] {
                    if (on)
                    {
                        st->tags.erase(std::remove(st->tags.begin(), st->tags.end(), tag), st->tags.end());
                    }
                    else
                    {
                        st->tags.push_back(tag);
                    }

                    mSession.View().post([alive = std::weak_ptr<bool>(mAlive), weakFill] {
                        if (const auto fill = weakFill.lock(); fill && alive.lock())
                        {
                            (*fill)();
                        }
                    });
                },
                Button::Style::Flat);
            chip->Selected(on).TextSize(metrics::kSmallText);
            const float w = MeasureTextWidth(tag, Font(Weight::Medium).size(metrics::kSmallText)) + 26.0f;

            if (used + w > 460.0f && !row.empty())
            {
                row.push_back(Stretch());
                rows.push_back(FixedHeight(30, Row(row)));
                row.clear();
                used = 0.0f;
            }

            row.push_back(chip);
            row.push_back(HGap(4));
            used += w + 4.0f;
        }

        if (!row.empty())
        {
            row.push_back(Stretch());
            rows.push_back(FixedHeight(30, Row(row)));
        }

        Fill(*tagSlot, Col(rows));
        mSession.View().layout();
        mSession.View().refresh();
    };
    (*fillTags)();
    mDetailsTags = fillTags; // kept while the dialog is open: the chips hold it only weakly

    // ── Input Peak Level: the raw input's peak over ten seconds, and the one the preset keeps ──
    using guitarfx::uiclient::FormatPeakDb;
    const auto cell = [this](const std::string& text, bool value) {
        auto t = std::make_shared<Text>(mTheme, text, metrics::kSmallText, value ? Weight::SemiBold : Weight::Medium);
        t->Colour(value ? mTheme.ModalText() : mTheme.ModalTextSecondary());

        if (value)
        {
            t->Alignment(HAlign::Right);
        }

        return t;
    };
    auto rawPeak = cell(FormatPeakDb(std::nullopt), true);
    auto designedPeak = cell(FormatPeakDb(preset.designedPeakInputDbfs), true);
    auto apply = std::make_shared<Button>(mTheme, "Apply Current", std::function<void()>{}, Button::Style::Plain);
    apply->TextSize(11.0f).MinWidth(104);
    apply->OnClick([this, st, designedPeak] {
        const auto peak = st->hold.Peak();

        if (!peak)
        {
            ShowToast("No peak data available yet", "", false);
            return;
        }

        st->stagedPeak = *peak;
        designedPeak->Set(FormatPeakDb(peak));
        mSession.Repaint(*designedPeak);
    });
    auto peakBox =
        Over(PadXY(14, 8,
                   Col({FixedHeight(26, Row({cell("Raw Input Peak (10s)", false), rawPeak})),
                        FixedHeight(32, Row({cell("Designed Peak", false), designedPeak, HGap(10), Middle(apply)}))})),
             el::share(Surface(mTheme.ModalRaised(), mTheme.ModalRule(), 8.0f)));

    const auto box = [](Ptr element) { return FixedHeight(36, element); };
    auto details =
        Col({label("Save Folder"), FixedHeight(34, folder), VGap(12), label("Preset Name"), box(nameElement), VGap(12),
             label("Category"), box(categoryElement), VGap(12), label("Description"), box(descriptionElement), VGap(12),
             label("Tags"), tagSlot, VGap(12), label("Input Peak Level"), FixedHeight(74, peakBox)});

    // ── Advanced: the JSON, read-only, with Validate and Clean Up ──
    auto code = std::make_shared<CodeBlock>(mTheme);
    code->Set(st->json.dump(2));
    auto report = std::make_shared<CodeBlock>(mTheme);
    auto reportSlot = MakeSlot();
    const auto showReport = [this, report, reportSlot](const std::vector<std::string>& lines) {
        std::string text;

        for (const auto& line : lines)
        {
            text += (text.empty() ? "" : "\n") + line;
        }

        report->Set(text);
        Fill(*reportSlot, Col({report, VGap(10)}));
        mSession.View().layout();
        mSession.View().refresh();
    };
    auto validate = std::make_shared<Button>(
        mTheme, "Validate",
        [st, showReport] {
            auto issues = guitarfx::uiclient::ValidatePresetJson(st->cleaned ? *st->cleaned : st->json);
            showReport(issues.empty() ? std::vector<std::string>{"No issues found."} : issues);
        },
        Button::Style::Plain);
    auto cleanUp = std::make_shared<Button>(
        mTheme, "Clean Up",
        [this, st, code, showReport] {
            auto result = guitarfx::uiclient::CleanupPresetJson(st->json);
            code->Set(result.cleaned.dump(2));
            st->cleaned = std::move(result.cleaned);
            showReport(guitarfx::uiclient::CleanupReport(result));
            mSession.View().refresh();
        },
        Button::Style::Plain);
    validate->MinWidth(88);
    cleanUp->MinWidth(88);
    auto advanced =
        Col({label("Preset JSON (read-only)"), FixedHeight(34, Row({validate, HGap(8), cleanUp, Stretch()})), VGap(10),
             reportSlot, FixedHeight(380, VScroll(code))});

    // ── The tabs (.preset-modal-tabs) ──
    auto panel = MakeSlot();
    auto detailsTab = std::make_shared<Button>(mTheme, "Details", std::function<void()>{}, Button::Style::Flat);
    auto advancedTab = std::make_shared<Button>(mTheme, "Advanced", std::function<void()>{}, Button::Style::Flat);
    const auto show = [this, st, panel, details, advanced, d = detailsTab.get(), a = advancedTab.get()](int tab) {
        st->tab = tab;
        d->Selected(tab == 0);
        a->Selected(tab == 1);
        Fill(*panel, tab == 0 ? details : advanced);
        mSession.View().layout();
        mSession.View().refresh();
    };
    detailsTab->OnClick([show] { show(0); });
    advancedTab->OnClick([show] { show(1); });
    detailsTab->Selected(true);
    Fill(*panel, details);

    auto save = [this, st, asNew, nameBox = nameBox, categoryBox = categoryBox, descriptionBox = descriptionBox] {
        const auto trim = [](std::string text) {
            const auto first = text.find_first_not_of(" \t");
            const auto last = text.find_last_not_of(" \t");
            return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        };
        guitarfx::uiclient::UiCommands::PresetDetails details;
        details.name = trim(nameBox->get_utf8());

        if (details.name.empty())
        {
            return;
        }

        details.category = trim(categoryBox->get_utf8());
        details.description = trim(descriptionBox->get_utf8());
        details.tags = st->tags;
        details.folderId = st->folderId;
        details.designedPeakInputDbfs = st->stagedPeak;
        details.preset = st->cleaned;
        CloseOverlay();
        mSession.Commands().SavePresetWithDetails(details, asNew);
    };
    nameBox->on_enter = [save](std::string_view) {
        save();
        return true;
    };

    ShowDialog({.title = asNew ? "Save Preset As" : "Overwrite Preset",
                .body = Col({FixedHeight(32, Row({detailsTab, HGap(6), advancedTab, Stretch()})), VGap(12), panel}),
                .actions = {DialogButton(
                                "Cancel", [this] { CloseOverlay(); }, false),
                            DialogButton(asNew ? "Save New Preset" : "Overwrite Preset", save, true)},
                .width = 520.0f,
                .height = kDialogHeaderHeight + kDialogFooterHeight + 36.0f + 44.0f + 504.0f});

    // The raw input's peak follows the meters while the dialog is open.
    mInstrumentSubs.push_back(mSession.Client().Subscribe(guitarfx::uiclient::Topic::Telemetry, [this, st, rawPeak] {
        st->hold.Push(mState.telemetry.rawInput.peakDb, UiSession::Now());
        const auto text = FormatPeakDb(st->hold.Peak());

        if (text != rawPeak->Value())
        {
            rawPeak->Set(text);
            mSession.Repaint(*rawPeak);
        }
    }));
}
} // namespace nanoq::ui
