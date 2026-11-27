#pragma once
// Where an element was last drawn, so it can be redrawn by that rectangle (UiSession::Repaint)
// rather than by Elements' view::refresh(element). That one queues the element and, at the next
// poll, searches the tree for it; past four queued in one poll it gives up and redraws the
// whole window. While audio plays the meters, chain lights, readouts and knobs change together
// on every telemetry tick, so the whole window was redrawn 20 times a second: cheap enough on a
// GPU backend, but on iOS, where Quartz draws on the CPU, it held the main thread and touches
// waited behind it. A widget that changes with the feeds inherits this and calls NoteDrawn in
// its draw().

#include <elements.hpp>

#include <optional>

namespace nanoq::ui
{
namespace el = cycfi::elements;

class Drawn
{
  public:
    virtual ~Drawn() = default;

    /// Remembers the rectangle this draws into, in the view's coordinates (as
    /// view::refresh(context) converts it).
    void NoteDrawn(const el::context& ctx)
    {
        NoteDrawn(ctx, ctx.bounds);
    }

    /// For a widget that draws past its bounds (a chain tile's corner buttons): `area` is all of
    /// what it draws, in the context's coordinates. A redraw is clipped to the rectangle asked
    /// for, so one of the bounds alone leaves the overhang out.
    void NoteDrawn(const el::context& ctx, const el::rect& area)
    {
        const auto tl = ctx.canvas.user_to_device(area.top_left());
        const auto br = ctx.canvas.user_to_device(area.bottom_right());
        mDrawn = el::rect{tl.x, tl.y, br.x, br.y};
    }

    /// Unknown until it has been drawn once.
    [[nodiscard]] const std::optional<el::rect>& DrawnArea() const
    {
        return mDrawn;
    }

  private:
    std::optional<el::rect> mDrawn;
};
} // namespace nanoq::ui
