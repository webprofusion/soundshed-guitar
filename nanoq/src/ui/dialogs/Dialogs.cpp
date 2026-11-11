// What floats over the page: a pop-up list, a text prompt and a confirmation (the effect picker is
// chain/EffectPicker.cpp's). One at a time, over a dimmed view; a tap outside, or a choice, closes it.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

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
} // namespace nanoq::ui
