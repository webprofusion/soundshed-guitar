#pragma once

// Bindings - what the UI shows that moves without a rebuild: a knob that follows automation, a
// fader that follows a recalled project, a meter, a playhead, a setting changed elsewhere.
//
// Bind pairs a widget with `read`, which returns a cheap key for what the widget should show (a
// number, a bool, a small struct with ==), and `show`, which puts it on the widget: a lambda
// taking (Widget&, key), or a setter such as &Button::On. Each tick, Sync() reads every key and
// calls `show` and redraws the widget (by its own area, UiSession::Repaint) only when the key
// changed, so formatting text and redrawing cost nothing while nothing moves.
//
// Feed is for a stream (a meter, a light): `update(Widget&)` takes the latest reading and returns
// whether the widget changed enough to redraw.
//
// A widget the user is dragging is left alone until the drag ends. The shell keeps one Bindings
// for everything it draws: a region binds as it builds, and a binding goes with its widget (it
// holds the widget weakly), so nothing keeps lists of handles. The shell also holds back every
// rebuild while a bound widget is being dragged, since a rebuild would drop the drag.

#include "ui/UiSession.h"

#include <elements.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace nanoq::ui
{
class Bindings
{
  public:
    explicit Bindings(UiSession& session) : mSession(session)
    {
    }

    Bindings(const Bindings&) = delete;
    Bindings& operator=(const Bindings&) = delete;

    /// Binds `widget` and shows its value at once (it is not drawn yet, so nothing is redrawn).
    template <class Widget, class Read, class Show>
    void Bind(const std::shared_ptr<Widget>& widget, Read read, Show show)
    {
        Add(std::make_unique<BindOf<Widget, Read, Show>>(widget, std::move(read), std::move(show)));
    }

    /// Feeds `widget` from a stream, at once and then each tick: `update` returns true when it
    /// changed enough to redraw.
    template <class Widget, class Update> void Feed(const std::shared_ptr<Widget>& widget, Update update)
    {
        Add(std::make_unique<FeedOf<Widget, Update>>(widget, std::move(update)));
    }

    /// Shows what changed, and forgets the bindings whose widgets are gone. UI thread, per tick.
    void Sync()
    {
        std::erase_if(mBindings, [this](const auto& b) { return !b->Sync(mSession, true); });
    }

    /// Whether the user is dragging a bound widget: nothing is rebuilt meanwhile.
    [[nodiscard]] bool Dragging() const
    {
        return std::any_of(mBindings.begin(), mBindings.end(), [](const auto& b) { return b->Dragging(); });
    }

  private:
    struct Binding
    {
        virtual ~Binding() = default;
        /// False once the widget is gone.
        virtual bool Sync(UiSession& session, bool repaint) = 0;
        [[nodiscard]] virtual bool Dragging() const = 0;
    };

    template <class Widget> struct On : Binding
    {
        explicit On(const std::shared_ptr<Widget>& w) : widget(w)
        {
        }

        bool Sync(UiSession& session, bool repaint) final
        {
            const auto w = widget.lock();

            if (!w)
            {
                return false;
            }

            if (!IsDragging(*w) && Apply(*w) && repaint)
            {
                session.Repaint(*w);
            }

            return true;
        }

        [[nodiscard]] bool Dragging() const final
        {
            const auto w = widget.lock();
            return w && IsDragging(*w);
        }

        /// Puts what changed on the widget; true when it needs a redraw.
        virtual bool Apply(Widget& w) = 0;

        static bool IsDragging(const Widget& w)
        {
            if constexpr (requires { w.Dragging(); })
            {
                return w.Dragging();
            }
            else
            {
                return false;
            }
        }

        std::weak_ptr<Widget> widget;
    };

    template <class Widget, class Read, class Show> struct BindOf final : On<Widget>
    {
        using Key = std::decay_t<std::invoke_result_t<Read&>>;

        BindOf(const std::shared_ptr<Widget>& w, Read r, Show s) : On<Widget>(w), read(std::move(r)), show(std::move(s))
        {
        }

        bool Apply(Widget& w) override
        {
            auto key = read();

            if (shown && *shown == key)
            {
                return false;
            }

            std::invoke(show, w, key);
            shown = std::move(key);
            return true;
        }

        Read read;
        Show show;
        std::optional<Key> shown;
    };

    template <class Widget, class Update> struct FeedOf final : On<Widget>
    {
        FeedOf(const std::shared_ptr<Widget>& w, Update u) : On<Widget>(w), update(std::move(u))
        {
        }

        bool Apply(Widget& w) override
        {
            return update(w);
        }

        Update update;
    };

    void Add(std::unique_ptr<Binding> binding)
    {
        binding->Sync(mSession, false);
        mBindings.push_back(std::move(binding));
    }

    UiSession& mSession;
    std::vector<std::unique_ptr<Binding>> mBindings;
};
} // namespace nanoq::ui
