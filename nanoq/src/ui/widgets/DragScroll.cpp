// See DragScroll.h.

#include "ui/widgets/DragScroll.h"

#include <algorithm>
#include <cstdlib>

#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif

namespace nanoq::ui
{
bool DragScrollEnabled()
{
    static const bool enabled = [] {
        if (const char* value = std::getenv("SOUNDSHED_NANOQ_DRAGSCROLL"); value != nullptr && value[0] != '\0')
        {
            return value[0] != '0';
        }

        // iOS as well: a touch reaches the view as the left button there too (base_view.mm), and
        // the host's own scroll gesture takes two fingers.
#if defined(__linux__) || (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
        return true;
#else
        return false;
#endif
    }();
    return enabled;
}

bool TouchFirst()
{
    static const bool touch = [] {
        if (const char* value = std::getenv("SOUNDSHED_NANOQ_TOUCH"); value != nullptr && value[0] != '\0')
        {
            return value[0] != '0';
        }

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        return true;
#else
        return std::getenv("SOUNDSHED_KIOSK") != nullptr;
#endif
    }();
    return touch;
}

float DragScroller::Overflow(const el::context& ctx)
{
    const auto content = subject().limits(ctx);
    return mVertical ? content.min.y - ctx.bounds.height() : content.min.x - ctx.bounds.width();
}

bool DragScroller::OnScrollBar(const el::context& ctx, el::point p) const
{
    const float width = el::get_theme().scrollbar_width;
    return mVertical ? p.x >= ctx.bounds.right - width : p.y >= ctx.bounds.bottom - width;
}

bool DragScroller::PressGivesWay(const el::context& ctx, el::point p)
{
    // Elements' own hit test stops at a scroller, as a scroller takes its clicks itself.
    auto* pressed = el::proxy_base::hit_test(ctx, p, true, true);

    if (pressed == nullptr)
    {
        return true; // a gap, or something that takes no press
    }

    if (const auto* inner = dynamic_cast<const DragScroller*>(pressed))
    {
        // It scrolls first if it can go the same way. Round the other way, the direction decides.
        return inner->mGivesWay && !(inner->mArmed && inner->mVertical == mVertical);
    }

    return dynamic_cast<const TapTarget*>(pressed) != nullptr;
}

void DragScroller::ScrollTo(const el::context& ctx, double align)
{
    align = std::clamp(align, 0.0, 1.0);

    if (mVertical)
    {
        valign(align);
    }
    else
    {
        halign(align);
    }

    on_scroll(el::point(static_cast<float>(halign()), static_cast<float>(valign())));
    ctx.view.refresh(ctx);
}

bool DragScroller::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state != el::mouse_button::left || !DragScrollEnabled())
    {
        return el::scroller_base::click(ctx, btn);
    }

    if (btn.down)
    {
        const bool taken = el::scroller_base::click(ctx, btn);
        const bool scrollBar = Overflow(ctx) > 0.5f && OnScrollBar(ctx, btn.pos);
        mGivesWay = !scrollBar && PressGivesWay(ctx, btn.pos);
        mArmed = mGivesWay && Overflow(ctx) > 0.5f;

        if (mArmed)
        {
            mPressAlign = mVertical ? valign() : halign();
            mGesture.Press(Along(btn.pos), Across(btn.pos));
        }

        // Saying so even over a gap, or the drag would never get here.
        return taken || mArmed;
    }

    const bool scrolled = mArmed && mGesture.Current() == DragGesture::State::Scrolling;
    mArmed = false;

    if (scrolled)
    {
        return true; // what was pressed has been released already
    }

    return el::scroller_base::click(ctx, btn);
}

void DragScroller::drag(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state != el::mouse_button::left || !mArmed)
    {
        el::scroller_base::drag(ctx, btn);
        return;
    }

    const bool wasScrolling = mGesture.Current() == DragGesture::State::Scrolling;

    switch (mGesture.Move(Along(btn.pos), Across(btn.pos)))
    {
    case DragGesture::State::Waiting:
        // Held back: a row that can be dragged out would otherwise start on a tap's wander, and
        // could not be asked to stop.
        return;

    case DragGesture::State::Passed:
        el::scroller_base::drag(ctx, btn);
        return;

    case DragGesture::State::Scrolling:
        break;
    }

    if (!wasScrolling)
    {
        auto away = btn;
        away.down = false;
        away.pos = el::point(kReleaseAway, kReleaseAway);
        el::scroller_base::click(ctx, away);
    }

    ScrollTo(ctx, mPressAlign + mGesture.ScrolledBy(Along(btn.pos)) / Overflow(ctx));
}
} // namespace nanoq::ui
