#pragma once

// What every clickable piece of the UI is built on: hover and press tracking, the click, and the
// keyboard's Enter, Space and Menu keys.

#include "ui/Drawn.h"
#include "ui/input/Focus.h"
#include "ui/widgets/Layout.h"
#include "ui/widgets/Widgets.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace nanoq::ui
{
/// Hover and press tracking shared by the clickable pieces here: a left click released inside
/// runs `onClick`. With focus (Focus.h), Enter or Space runs it too, and the Menu key or
/// Shift+F10 opens `onMenu`. Read as a button unless a subclass says what it is.
class Pressable : public el::element, public Drawn, public Focusable, public TapTarget
{
  public:
    std::function<void()> onClick;
    /// A right click (a long press on a touch screen), at a point in view coordinates: the
    /// control's context menu, such as MIDI Learn.
    std::function<void(el::point at)> onMenu;

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.name = mAccessName;
        info.enabled = mEnabled;
        return info;
    }

    bool Activate(const el::rect& at) override
    {
        if (!mEnabled || !onClick)
        {
            return false;
        }

        mBounds = at;
        Pressed();
        const auto run = onClick; // it may rebuild the region this sits in
        run();
        return true;
    }

    bool wants_focus() const override
    {
        return FocusWanted(mEnabled && (onClick != nullptr || onMenu != nullptr));
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override
    {
        NoteFocus(false);
        return true;
    }

    bool key(const el::context& ctx, el::key_info k) override
    {
        if (IsMenuKey(k) && onMenu)
        {
            mBounds = ctx.bounds;
            onMenu(cycfi::artist::center_point(ctx.bounds));
            return true;
        }

        if (IsPressKey(k))
        {
            return Activate(ctx.bounds);
        }

        return false;
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::right && onMenu)
        {
            if (btn.down)
            {
                onMenu(btn.pos);
            }

            return true;
        }

        if (!mEnabled || btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (btn.down)
        {
            mDown = true;
            ctx.view.refresh(ctx);
            return true;
        }

        const bool fire = mDown && ctx.bounds.includes(btn.pos);
        mDown = false;
        mBounds = ctx.bounds;
        ctx.view.refresh(ctx);

        if (fire && onClick)
        {
            Pressed();
            onClick();
        }

        return true;
    }

    /// Where it was when last clicked, in view coordinates: a menu it opens hangs from it.
    [[nodiscard]] el::rect Bounds() const
    {
        return mBounds;
    }

    bool cursor(const el::context& ctx, el::point, el::cursor_tracking status) override
    {
        const bool hover = status != el::cursor_tracking::leaving;

        if (hover != mHover)
        {
            mHover = hover;
            ctx.view.refresh(ctx);
        }

        return false;
    }

    bool wants_control() const override
    {
        return true;
    }

    void Enabled(bool enabled)
    {
        mEnabled = enabled;
    }

  protected:
    /// Just before `onClick` runs, from a click or a key: a switch notes where it was for the
    /// slide to its new state (SwitchMotion).
    virtual void Pressed()
    {
    }

    bool mHover = false, mDown = false, mEnabled = true;
    el::rect mBounds;
};
} // namespace nanoq::ui
