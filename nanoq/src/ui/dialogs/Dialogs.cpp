// What floats over the page: a pop-up list, a text prompt and a confirmation (the effect picker is
// chain/EffectPicker.cpp's). One at a time, over a dimmed view; a tap outside, or a choice, closes it.
// A confirm or prompt opened from a dialog stacks over it, and Cancel goes back to that dialog.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"
#include "ui/input/AccessBridge.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace nanoq::ui
{
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
    const bool interrupts = std::exchange(mNextOverlayInterrupts, false);

    if (interrupts && mOverlay && !mSuspendedOverlay && !mOptionsOpen)
    {
        // Kept as it is, live feeds and all, under the confirm's backdrop, which takes the clicks.
        CloseDropdown();
        mSuspendedOverlay = SuspendedOverlay{std::exchange(mOverlay, nullptr), std::exchange(mOverlayCancel, nullptr),
                                             std::exchange(mOnOverlayClosed, nullptr)};
    }
    else
    {
        CloseOverlay();
    }

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
    auto title = std::make_shared<Text>(mTheme, parts.title, 16.0f, Weight::Title);
    title->Colour(mTheme.ModalText());
    auto close = std::make_shared<CloseCross>(mTheme);
    auto cancel = parts.onClose; // Esc and a tap outside do the same as the cross
    close->onClick = parts.onClose ? std::move(parts.onClose) : std::function<void()>([this] { DismissOverlay(); });
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
    // A short window (800x480) keeps 8 px round a tall dialog rather than 20: a list or a form
    // shows a row more.
    const float height = std::min(parts.height, std::max(200.0f, view.y - (view.y < 560.0f ? 16.0f : 40.0f)));
    const float width = std::min(parts.width, std::max(280.0f, view.x - 32.0f));
    Ptr body = Pad(parts.padX, parts.padY, parts.padX, parts.padY, std::move(parts.body));

    // A window too short for the dialog scrolls its body under the header; a body that fills
    // the dialog scrolls in itself.
    const bool scrolls = height < parts.height && !parts.stretch;

    if (scrolls)
    {
        body = VScroll(body);
    }

    column.push_back(body);

    // Takes any height the body leaves, so the card fills the dialog and the footer sits at its
    // foot: a column of fixed rows is otherwise only as tall as its rows. Not beside a body that
    // scrolls: that one takes the height itself, and the two would share it, leaving the body
    // half the dialog to scroll in over an empty half.
    if (!parts.stretch && !scrolls)
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

    // Told to a screen reader as a dialog with its title and message (role=dialog).
    if (auto* router = dynamic_cast<KeyRouter*>(mOverlay.get()))
    {
        router->accessKind = KeyRouter::AccessKind::Dialog;
        router->accessTitle = parts.title;
        router->accessDescription = parts.description;
    }
}

void Shell::CancelOverlay()
{
    if (mOverlayCancel)
    {
        const auto cancel = mOverlayCancel; // it closes the overlay, which forgets it
        cancel();
        return;
    }

    DismissOverlay();
}

void Shell::DismissOverlay()
{
    if (!mSuspendedOverlay)
    {
        CloseOverlay();
        return;
    }

    // Only the confirm goes; the dialog under it is the overlay again, with the keys.
    CloseDropdown();
    auto parent = std::move(*mSuspendedOverlay);
    mSuspendedOverlay.reset();
    RemoveLayer(std::exchange(mOverlay, nullptr));
    mOverlay = parent.layer;
    mOverlayCancel = std::move(parent.cancel);
    mOnOverlayClosed = std::move(parent.onClosed);

    auto& view = mSession.View();
    view.post([&view, layer = parent.layer] {
        auto& layers = view.content();
        const auto found = std::find(layers.begin(), layers.end(), layer);

        if (found != layers.end())
        {
            layers.focus(static_cast<std::size_t>(found - layers.begin()));
            view.refresh();
        }
    });
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
    CloseThemeFlyout(); // opened from the Options sheet, it goes with it
    mOverlayCancel = nullptr;

    // A dialog with live feeds (tuner, metronome) stops listening, and tells the engine it is
    // done, whether it was closed by its button or by a tap outside it.
    mInstrumentSubs.clear();

    if (auto closed = std::move(mOnOverlayClosed))
    {
        mOnOverlayClosed = nullptr;
        closed();
    }

    if (mOverlay)
    {
        auto overlay = std::move(mOverlay);
        mOverlay.reset();
        RemoveLayer(std::move(overlay));
    }

    // A confirm opened over a dialog is done with: the dialog goes too (what it confirmed may
    // reopen it, up to date).
    if (mSuspendedOverlay)
    {
        auto parent = std::move(*mSuspendedOverlay);
        mSuspendedOverlay.reset();

        if (parent.onClosed)
        {
            parent.onClosed();
        }

        RemoveLayer(std::move(parent.layer));
    }
}

void Shell::ShowBar(const SlotPtr& slot, Ptr bar)
{
    auto& view = mSession.View();
    view.post([&view, slot, bar = std::move(bar)] {
        Fill(*slot, bar ? bar : std::make_shared<NoHit>());
        view.layout();
        view.refresh();
    });
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
    // Until then it is closed in every other way: it draws nothing and takes no input, so a
    // second click or a paint in the gap never reaches what its controls point at, which closing
    // let go (found by nanoq/tools/stress-ui.ps1 under ASan: the effect picker's freed state).
    if (auto* router = dynamic_cast<KeyRouter*>(layer.get()))
    {
        router->Retire();
    }

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
                .padY = 10.0f,
                // The list scrolls in itself: it takes the whole body, or it shares it with the
                // filler under it and scrolls in half the dialog over an empty half.
                .stretch = true});
}

void Shell::OpenPrompt(const std::string& title, const std::string& initial, const std::string& confirmLabel,
                       std::function<void(const std::string&)> onSubmit)
{
    OpenPrompt(title, initial, confirmLabel, PromptOptions{}, std::move(onSubmit));
}

void Shell::OpenPrompt(const std::string& title, const std::string& initial, const std::string& confirmLabel,
                       PromptOptions options, std::function<void(const std::string&)> onSubmit)
{
    const float width = DialogWidth(ViewSize(), 420.0f);
    std::vector<Ptr> rows;
    float bodyHeight = 0.0f;

    // What to enter, over the field (the web's prompt message).
    if (!options.message.empty())
    {
        auto message = std::make_shared<Paragraph>(mTheme, options.message, width - 40.0f, metrics::kTextSize);
        message->Colour(mTheme.ModalTextSecondary());
        bodyHeight += message->Height() + 8.0f;
        rows.push_back(message);
        rows.push_back(VGap(8));
    }

    // One line, or several in a box that scrolls; either starts with its text selected, so
    // typing replaces it (the web's field.select()).
    std::function<std::string()> read;
    std::shared_ptr<el::basic_input_box> line; // Enter there is OK, once OK is made

    if (options.multiline)
    {
        auto box = el::share(el::basic_text_box{initial, Font(Weight::Regular)});
        box->select_all();
        read = [box] { return box->get_utf8(); };
        auto field = VScroll(el::share(el::margin(el::rect{10.0f, 8.0f, 10.0f, 8.0f}, el::hold(box))));
        rows.push_back(FixedHeight(110, Rounded(mTheme.Tertiary(), 3.0f, field)));
        bodyHeight += 110.0f;
    }
    else
    {
        auto [boxElement, box] = el::input_box(options.placeholder, Font(Weight::Regular), 1.0f);
        box->set_text(initial);
        box->select_all();
        read = [weak = std::weak_ptr<el::basic_input_box>(box)] {
            const auto b = weak.lock();
            return b ? b->get_utf8() : std::string();
        };
        box->on_escape = [this] { DismissOverlay(); };
        // Named from the prompt's title, as the web's field labelled by its heading.
        rows.push_back(FixedHeight(40, Labelled(title, el::share(std::move(boxElement)))));
        bodyHeight += 40.0f;
        line = box;
    }

    // Under the field: why OK did nothing, when nothing was entered.
    auto note = std::make_shared<Text>(mTheme, "", metrics::kSmallText);
    note->Colour(mTheme.Error());
    rows.push_back(FixedHeight(22, Middle(note)));
    bodyHeight += 22.0f;
    rows.push_back(Stretch());

    auto submit = [this, read, note, allowEmpty = options.allowEmpty, multiline = options.multiline, onSubmit] {
        auto text = read();

        // Trim: a name that is only spaces is no name. Several lines keep their own breaks.
        const char* blank = multiline ? " \t\r\n" : " \t";
        const auto first = text.find_first_not_of(blank);
        const auto last = text.find_last_not_of(blank);
        text = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);

        if (text.empty() && !allowEmpty)
        {
            note->Set("Enter something first, or Cancel.");
            mSession.View().refresh();
            return;
        }

        CloseOverlay();
        onSubmit(text);
    };

    if (line)
    {
        line->on_enter = [submit](std::string_view) {
            submit();
            return true;
        };
    }

    mNextOverlayInterrupts = true;
    ShowDialog({.title = title,
                .body = Col(rows),
                .actions = {DialogButton(
                                "Cancel", [this] { DismissOverlay(); }, false),
                            DialogButton(confirmLabel, submit, true)},
                .width = width,
                .height = kDialogHeaderHeight + kDialogFooterHeight + 36.0f + bodyHeight,
                .description = options.message});
}

namespace
{
/// The height a dialog of text takes: its lines, but no less than two.
float TextDialogHeight(const Paragraph& body)
{
    return kDialogHeaderHeight + kDialogFooterHeight + 36.0f + std::max(50.0f, body.Height());
}
} // namespace

void Shell::OpenConfirm(const std::string& title, const std::string& detail, const std::string& confirmLabel,
                        std::function<void()> onYes)
{
    // As tall as its text; a window too short for it scrolls it (ShowDialog).
    const float width = DialogWidth(ViewSize(), 460.0f);
    auto body = std::make_shared<Paragraph>(mTheme, detail, width - 40.0f, metrics::kTextSize);
    body->Colour(mTheme.ModalTextSecondary());
    auto yes = [this, onYes] {
        CloseOverlay();
        onYes();
    };
    mNextOverlayInterrupts = true;
    ShowDialog({.title = title,
                .body = Col({body, Stretch()}),
                .actions = {DialogButton(
                                "Cancel", [this] { DismissOverlay(); }, false),
                            DialogButton(confirmLabel, yes, true)},
                .width = width,
                .height = TextDialogHeight(*body),
                .description = detail});
}

void Shell::OpenAlert(const std::string& title, const std::string& message, std::function<void()> onClose)
{
    const float width = DialogWidth(ViewSize(), 460.0f);
    auto body = std::make_shared<Paragraph>(mTheme, message, width - 40.0f, metrics::kTextSize);
    body->Colour(mTheme.ModalTextSecondary());
    // OK, the cross, Esc and a tap outside all close it and go on.
    auto done = [this, onClose] {
        DismissOverlay();

        if (onClose)
        {
            onClose();
        }
    };
    mNextOverlayInterrupts = true;
    ShowDialog({.title = title,
                .body = Col({body, Stretch()}),
                .actions = {DialogButton("OK", done, true)},
                .width = width,
                .height = TextDialogHeight(*body),
                .onClose = done,
                .description = message});
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
    // Said too, as the web's toast region (aria-live): an error at once.
    AccessBridge::Announce(detail.empty() ? title : title + ". " + detail, isError);
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
} // namespace nanoq::ui
