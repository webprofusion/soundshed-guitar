#pragma once

// KeyRouter - the keyboard's way in: the shortcuts, and moving focus between controls.
//
// Elements hands a key only along the focus chain, and a click on anything that does not want
// focus (every button, card and knob here, while the pointer is in use) clears it, so with nothing
// focused a key reaches no element at all. The router sits at the root of each layer (the page, a
// dialog, a drop-down) and wants focus itself; Retake(), called each tick, gives it back after a
// click took it away. A key goes to the focused element first (a text box that is being typed in
// keeps every key), and only what nothing used reaches `onKey`; Esc goes to `onKey` first, so it
// closes a dialog from inside its text box, unless a value is being typed into a knob.
//
// Tab and Shift+Tab move focus through the layer's controls in layout order, round from the last
// to the first, and never out of a dialog (Focus.h). The arrows move between the rows of a list
// and the tabs of a row. The router draws the focus ring, and keeps focus on a control that a
// rebuild replaced by finding the new one with the same role and name.
//
// On a touch screen with no keyboard, the router of the focused layer also hosts the on-screen
// keyboard (OnScreenKeyboard.h): it draws the panel over the layer, takes the presses on it, and
// moves the layer up so the field stays clear of it.

#include "ui/input/Focus.h"
#include "ui/input/OnScreenKeyboard.h"
#include "ui/widgets/LongPress.h"
#include "ui/widgets/TouchHold.h"

#include <elements.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace nanoq::ui
{
class KeyRouter final : public cycfi::elements::proxy_base
{
  public:
    /// `focusContent`: a dialog's router passes focus on to what it holds (its text box gets the
    /// caret as it opens); the root's keeps it, so taking focus back puts no caret anywhere.
    explicit KeyRouter(cycfi::elements::element_ptr subject, bool focusContent = false)
        : mSubject(std::move(subject)), mFocusContent(focusContent)
    {
    }

    cycfi::elements::element& subject() override
    {
        return *mSubject;
    }

    const cycfi::elements::element& subject() const override
    {
        return *mSubject;
    }

    bool wants_focus() const override
    {
        return true;
    }

    // Taking focus back for the shortcuts must not move it into the content (it would put a caret
    // in the first text box); a window regaining focus restores what was focused before. A dialog
    // opening gives its first text box the caret, and only with no text box its first control,
    // while the keyboard is in use.
    void begin_focus(focus_request req) override
    {
        if (mFocusContent || req == focus_request::restore_previous)
        {
            proxy_base::begin_focus(req);
        }

        if (mFocusContent && req != focus_request::restore_previous)
        {
            const bool hasTextBox = FocusFirstTextBox(subject());

            // Once: Retake comes through here after every tap on the dialog.
            if (keyboard && hasTextBox && !mOfferedKeyboard)
            {
                mOfferedKeyboard = true;
                keyboard->NoteLayerOpened();
            }
        }
    }

    using proxy_base::prepare_subject;

    // The layer moved up for the on-screen keyboard. Everything goes through here, so drawing and
    // hit testing move together.
    void prepare_subject(cycfi::elements::context& ctx) override
    {
        if (HostsKeyboard())
        {
            ctx.bounds.top -= keyboard->Pan();
            ctx.bounds.bottom -= keyboard->Pan();
        }
    }

    void draw(const cycfi::elements::context& ctx) override
    {
        proxy_base::draw(ctx);
        DrawFocusRing(ctx);

        if (HostsKeyboard())
        {
            keyboard->Draw(ctx);
        }
    }

    void DrawFocusRing(const cycfi::elements::context& ctx)
    {
        if (!Focusable::FocusVisible())
        {
            return;
        }

        cycfi::elements::context sctx{ctx, &subject(), ctx.bounds};
        prepare_subject(sctx);
        WithFocusedLeaf(sctx, subject(), [&](const cycfi::elements::context& lctx, cycfi::elements::element& leaf) {
            const auto* f = dynamic_cast<const Focusable*>(&leaf);

            if (f == nullptr || !f->HasFocus())
            {
                return;
            }

            // 2 px wide, 2 px out from the control, as the web UI's focus outline.
            auto& cnv = ctx.canvas;
            const auto ring = lctx.bounds.inset(-3.0f, -3.0f);
            cnv.stroke_style(ringColour ? ringColour() : cycfi::elements::rgba(90, 160, 255, 255));
            cnv.line_width(2.0f);
            cnv.stroke_round_rect(ring, std::min(7.0f, std::min(ring.width(), ring.height()) * 0.5f));
        });
    }

    bool key(const cycfi::elements::context& ctx, cycfi::elements::key_info k) override
    {
        using cycfi::elements::key_code;
        const bool press =
            k.action == cycfi::elements::key_action::press || k.action == cycfi::elements::key_action::repeat;
        const bool editing = TypingInControl();

        // Esc closes the menu or dialog on top first, even from its text box (which would
        // otherwise take it to drop its own focus), as Esc closes a modal in the web UI. A value
        // being typed into a knob is cancelled first.
        if (k.key == key_code::escape && !editing && onKey && onKey(k))
        {
            return true;
        }

        if (press && k.key == key_code::tab &&
            (k.modifiers & (cycfi::elements::mod_control | cycfi::elements::mod_alt | cycfi::elements::mod_super)) == 0)
        {
            Focusable::SetFocusVisible(true);
            MoveFocus(ctx, (k.modifiers & cycfi::elements::mod_shift) ? -1 : 1);
            return true;
        }

        if (proxy_base::key(ctx, k))
        {
            return true;
        }

        // A text box being typed in keeps every other key.
        if (Typing())
        {
            return false;
        }

        if (press && (k.key == key_code::up || k.key == key_code::down || k.key == key_code::left ||
                      k.key == key_code::right || k.key == key_code::home || k.key == key_code::end))
        {
            if (ArrowFocus(ctx, k.key))
            {
                return true;
            }
        }

        return onKey && onKey(k);
    }

    // A press on the page where nothing takes it still reaches the page's router, so it hides
    // the keyboard's ring as a press on a control does (and clears the chain's shortcut target).
    // Only the page's: a press beside a drop-down must still go through to the page under it.
    bool wants_control() const override
    {
        return !mFocusContent || HostsKeyboard() || proxy_base::wants_control();
    }

    cycfi::elements::element* hit_test(const cycfi::elements::context& ctx, cycfi::elements::point p, bool leaf,
                                       bool control) override
    {
        // The router itself: it wants focus, so Elements leaves focus where it is.
        if (HostsKeyboard() && keyboard->Contains(ctx.bounds, p))
        {
            return this;
        }

        if (auto* hit = proxy_base::hit_test(ctx, p, leaf, control))
        {
            return hit;
        }

        return !mFocusContent && ctx.bounds.includes(p) ? this : nullptr;
    }

    bool click(const cycfi::elements::context& ctx, cycfi::elements::mouse_button btn) override
    {
        if (HostsKeyboard() && (keyboard->Tracking() || (btn.down && keyboard->Contains(ctx.bounds, btn.pos))))
        {
            keyboard->Click(ctx, btn);
            return true;
        }

        if (btn.down)
        {
            if (keyboard)
            {
                keyboard->NotePointerDown();
            }

            // The pointer takes over: no ring, and a click leaves no focus on a control.
            if (Focusable::FocusVisible())
            {
                Focusable::SetFocusVisible(false);
                ctx.view.refresh();
            }

            mLastKey.clear();

            if (onPointerDown)
            {
                onPointerDown();
            }

            LongPress::Reset();
            TouchHold::Reset();
        }

        const bool used = proxy_base::click(ctx, btn);

        // The control under it has asked LongPress whether its hold fired; the press is over.
        if (!btn.down)
        {
            LongPress::Reset();
            TouchHold::Reset();
        }

        // The press found a field, if one has focus now: Elements ended it otherwise.
        if (btn.down && keyboard && Typing())
        {
            keyboard->Arm();
        }

        return used;
    }

    void drag(const cycfi::elements::context& ctx, cycfi::elements::mouse_button btn) override
    {
        if (HostsKeyboard() && keyboard->Tracking())
        {
            keyboard->Drag(ctx, btn);
            return;
        }

        LongPress::Move(btn.pos);
        TouchHold::Move(btn.pos.x, btn.pos.y);
        proxy_base::drag(ctx, btn);
    }

    bool cursor(const cycfi::elements::context& ctx, cycfi::elements::point p,
                cycfi::elements::cursor_tracking status) override
    {
        return !(HostsKeyboard() && keyboard->Contains(ctx.bounds, p)) && proxy_base::cursor(ctx, p, status);
    }

    bool scroll(const cycfi::elements::context& ctx, cycfi::elements::point dir, cycfi::elements::point p) override
    {
        return (HostsKeyboard() && keyboard->Contains(ctx.bounds, p)) || proxy_base::scroll(ctx, dir, p);
    }

    // Files dragged in from the desktop are offered to the shell first (a model or IR onto an
    // effect), then to whatever is under them.
    void track_drop(const cycfi::elements::context& ctx, const cycfi::elements::drop_info& info,
                    cycfi::elements::cursor_tracking status) override
    {
        if (onTrackDrop)
        {
            onTrackDrop(info, status == cycfi::elements::cursor_tracking::leaving);
        }

        proxy_base::track_drop(ctx, info, status);
    }

    bool drop(const cycfi::elements::context& ctx, const cycfi::elements::drop_info& info) override
    {
        if (onDrop && onDrop(info))
        {
            return true;
        }

        return proxy_base::drop(ctx, info);
    }

    /// Gives the topmost router focus again if a click took it. UI thread.
    static void Retake(cycfi::elements::view& view)
    {
        auto& content = view.content();

        if (content.focus_index() != -1)
        {
            return;
        }

        // The router on top: a bar added over the page without one (the tone preview's) must not
        // take the keys from the page under it.
        for (auto i = static_cast<int>(content.size()) - 1; i >= 0; --i)
        {
            if (dynamic_cast<KeyRouter*>(&content.at(static_cast<std::size_t>(i))) != nullptr)
            {
                content.focus(static_cast<std::size_t>(i));
                content.at(static_cast<std::size_t>(i)).begin_focus(focus_request::from_top);
                return;
            }
        }
    }

    /// The router of the layer holding focus, or null. UI thread.
    static KeyRouter* Focused(cycfi::elements::view& view)
    {
        auto& content = view.content();
        const int index = content.focus_index();

        if (index < 0 || index >= static_cast<int>(content.size()))
        {
            return nullptr;
        }

        return dynamic_cast<KeyRouter*>(&content.at(static_cast<std::size_t>(index)));
    }

    /// Keeps keyboard focus where it was across a rebuild: a control that went with its region is
    /// found again by its role and name in the new one. Called each tick for the focused layer's
    /// router; does nothing while the pointer is in use.
    void KeepFocus(cycfi::elements::view& view)
    {
        if (!Focusable::FocusVisible())
        {
            return;
        }

        auto* leaf = FocusedLeaf(subject());
        const auto* f = leaf != nullptr ? dynamic_cast<const Focusable*>(leaf) : nullptr;

        if (leaf != nullptr && (f == nullptr || f->HasFocus()))
        {
            if (leaf != mLastLeaf)
            {
                mLastLeaf = leaf;
                Remember(view, leaf);
            }

            mLostTicks = 0;
            return;
        }

        mLastLeaf = nullptr;

        if (mLastKey.empty())
        {
            return;
        }

        bool found = false;
        view.in_context_do(*this, [&](const cycfi::elements::context& ctx) {
            cycfi::elements::context sctx{ctx, &subject(), ctx.bounds};
            prepare_subject(sctx);
            const auto targets = CollectFocusTargets(sctx, subject());
            const FocusTarget* best = nullptr;

            for (const auto& t : targets)
            {
                if (t.key == mLastKey && (best == nullptr || t.ordinal <= mLastOrdinal))
                {
                    best = &t;
                }
            }

            if (best != nullptr)
            {
                FocusTargetUnder(view, subject(), *best);
                found = true;
            }
        });

        // A control that is gone for good (its effect removed) is given up after half a second.
        if (!found && ++mLostTicks > 30)
        {
            mLastKey.clear();
        }
    }

    /// Moves focus to the control named by `key` and `ordinal` (a screen reader's SetFocus), as
    /// CollectFocusTargets names them under this router. UI thread.
    bool FocusByKey(cycfi::elements::view& view, const std::string& key, int ordinal)
    {
        bool found = false;
        view.in_context_do(*this, [&](const cycfi::elements::context& ctx) {
            cycfi::elements::context sctx{ctx, &subject(), ctx.bounds};
            prepare_subject(sctx);

            for (const auto& t : CollectFocusTargets(sctx, subject()))
            {
                if (t.key == key && t.ordinal == ordinal)
                {
                    FocusTargetUnder(view, subject(), t);
                    found = true;
                    return;
                }
            }
        });
        return found;
    }

    /// Whether a text box holds focus with its caret showing, or a value is being typed into a
    /// control.
    [[nodiscard]] bool Typing() const
    {
        return TypingUnder(const_cast<cycfi::elements::element&>(subject()));
    }

    /// What is being typed in, for the on-screen keyboard.
    struct TypingTarget
    {
        cycfi::elements::rect field;
        cycfi::elements::rect layer;
        bool valueField = false; // a knob's value, not a text box
    };

    /// Where the field being typed in is, and the bounds of this layer. Nothing when none is, or
    /// when what holds focus is a text box that cannot be typed in. UI thread.
    [[nodiscard]] std::optional<TypingTarget> TypingField(cycfi::elements::view& view)
    {
        std::optional<TypingTarget> found;

        if (!Typing())
        {
            return found;
        }

        view.in_context_do(*this, [&](const cycfi::elements::context& ctx) {
            cycfi::elements::context sctx{ctx, &subject(), ctx.bounds};
            prepare_subject(sctx);
            WithFocusedLeaf(sctx, subject(), [&](const cycfi::elements::context& lctx, cycfi::elements::element& leaf) {
                const auto* box = dynamic_cast<const cycfi::elements::basic_text_box*>(&leaf);

                if (box == nullptr || box->editable())
                {
                    found = TypingTarget{lctx.bounds, ctx.bounds, box == nullptr};
                }
            });
        });
        return found;
    }

    /// The on-screen keyboard, on a device that has one; all the routers share it.
    std::shared_ptr<OnScreenKeyboard> keyboard;
    /// A key no focused element took. Returns whether it was used.
    std::function<bool(const cycfi::elements::key_info&)> onKey;
    /// Any press of a mouse button, before the element under it hears it.
    std::function<void()> onPointerDown;
    /// Files held over the view (or leaving it), and dropped on it. onDrop returns whether it used them.
    std::function<void(const cycfi::elements::drop_info&, bool leaving)> onTrackDrop;
    std::function<bool(const cycfi::elements::drop_info&)> onDrop;
    /// The focus ring's colour: the theme's accent.
    std::function<cycfi::elements::color()> ringColour;

  private:
    [[nodiscard]] bool HostsKeyboard() const
    {
        return keyboard && keyboard->HostedBy(this);
    }

    [[nodiscard]] bool TypingInControl() const
    {
        const auto* leaf = FocusedLeaf(const_cast<cycfi::elements::element&>(subject()));
        const auto* f = leaf != nullptr ? dynamic_cast<const Focusable*>(leaf) : nullptr;
        return f != nullptr && f->Editing();
    }

    void Remember(cycfi::elements::view& view, cycfi::elements::element* leaf)
    {
        view.in_context_do(*this, [&](const cycfi::elements::context& ctx) {
            cycfi::elements::context sctx{ctx, &subject(), ctx.bounds};
            prepare_subject(sctx);

            for (const auto& t : CollectFocusTargets(sctx, subject()))
            {
                if (t.element == leaf)
                {
                    mLastKey = t.key;
                    mLastOrdinal = t.ordinal;
                    return;
                }
            }
        });
    }

    void Focus(const cycfi::elements::context& ctx, const FocusTarget& target)
    {
        FocusTargetUnder(ctx.view, subject(), target);
        mLastLeaf = target.element;
        mLastKey = target.key;
        mLastOrdinal = target.ordinal;
        mLostTicks = 0;
    }

    /// Tab: the next control round, or the first (the last, going back) when none has focus.
    void MoveFocus(const cycfi::elements::context& ctx, int direction)
    {
        cycfi::elements::context sctx{ctx, &subject(), ctx.bounds};
        prepare_subject(sctx);
        const auto targets = CollectFocusTargets(sctx, subject());

        if (targets.empty())
        {
            return;
        }

        const auto* leaf = FocusedLeaf(subject());
        const auto n = static_cast<int>(targets.size());
        int at = -1;

        for (int i = 0; i < n; ++i)
        {
            if (targets[static_cast<std::size_t>(i)].element == leaf)
            {
                at = i;
                break;
            }
        }

        const int next = at < 0 ? (direction > 0 ? 0 : n - 1) : (at + direction + n) % n;
        Focus(sctx, targets[static_cast<std::size_t>(next)]);
    }

    /// The arrows within a menu (up and down its rows), a list or a row of tabs (any arrow), Home
    /// and End to its first and last: to the next control of the same role that way.
    bool ArrowFocus(const cycfi::elements::context& ctx, cycfi::elements::key_code key)
    {
        using cycfi::elements::key_code;
        const auto* leaf = FocusedLeaf(subject());
        const auto* f = leaf != nullptr ? dynamic_cast<const Focusable*>(leaf) : nullptr;

        if (f == nullptr || !f->HasFocus())
        {
            return false;
        }

        // A menu goes up and down; a list (the chain runs across) and tabs go any way.
        const auto role = f->Describe().role;

        if (role != AccessRole::ListItem && role != AccessRole::MenuItem && role != AccessRole::Tab)
        {
            return false;
        }

        if (role == AccessRole::MenuItem && (key == key_code::left || key == key_code::right))
        {
            return false;
        }

        cycfi::elements::context sctx{ctx, &subject(), ctx.bounds};
        prepare_subject(sctx);
        const auto targets = CollectFocusTargets(sctx, subject());
        std::vector<const FocusTarget*> peers;
        int at = -1;

        for (const auto& t : targets)
        {
            if (t.focusable != nullptr && t.focusable->Describe().role == role)
            {
                if (t.element == leaf)
                {
                    at = static_cast<int>(peers.size());
                }

                peers.push_back(&t);
            }
        }

        if (at < 0)
        {
            return false;
        }

        int next = at;

        if (key == key_code::home)
        {
            next = 0;
        }
        else if (key == key_code::end)
        {
            next = static_cast<int>(peers.size()) - 1;
        }
        else
        {
            next = at + ((key == key_code::up || key == key_code::left) ? -1 : 1);
        }

        Focusable::SetFocusVisible(true);

        if (next >= 0 && next < static_cast<int>(peers.size()) && next != at)
        {
            Focus(sctx, *peers[static_cast<std::size_t>(next)]);
        }

        return true;
    }

    /// A dialog opening puts the caret in its first text box, as it did before its buttons could
    /// take focus. Returns whether it has one.
    static bool FocusFirstTextBox(cycfi::elements::element& root)
    {
        std::vector<std::pair<cycfi::elements::composite_base*, std::size_t>> path;
        cycfi::elements::element* box = FindTextBox(root, path);

        if (box == nullptr)
        {
            return false;
        }

        if (FocusedLeaf(root) == box)
        {
            return true;
        }

        root.end_focus();

        for (const auto& [composite, index] : path)
        {
            composite->focus(index);
        }

        box->begin_focus(focus_request::from_top);
        return true;
    }

    static cycfi::elements::element* FindTextBox(
        cycfi::elements::element& e, std::vector<std::pair<cycfi::elements::composite_base*, std::size_t>>& path)
    {
        if (dynamic_cast<cycfi::elements::basic_text_box*>(&e) != nullptr)
        {
            return &e;
        }

        if (auto* c = dynamic_cast<cycfi::elements::composite_base*>(&e))
        {
            for (std::size_t i = 0; i < c->size(); ++i)
            {
                path.emplace_back(c, i);

                if (auto* found = FindTextBox(c->at(i), path))
                {
                    return found;
                }

                path.pop_back();
            }

            return nullptr;
        }

        if (auto* p = dynamic_cast<cycfi::elements::proxy_base*>(&e))
        {
            return FindTextBox(p->subject(), path);
        }

        if (auto* i = dynamic_cast<cycfi::elements::indirect_base*>(&e))
        {
            return FindTextBox(i->get(), path);
        }

        return nullptr;
    }

    cycfi::elements::element_ptr mSubject;
    bool mFocusContent;
    // The control focus was last on, by its FocusKey and ordinal, so a rebuild can find it again.
    // mLastLeaf is only compared, never followed: it may be gone.
    const cycfi::elements::element* mLastLeaf = nullptr;
    std::string mLastKey;
    int mLastOrdinal = 0;
    int mLostTicks = 0;
    bool mOfferedKeyboard = false;
};
} // namespace nanoq::ui
