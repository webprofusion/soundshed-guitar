#pragma once

// Layout helpers over Elements' containers, for building a UI from element pointers.
//
// Elements' own builders (htile, margin, ...) take elements by value, which suits a UI written
// out as one expression. A UI rebuilt from state at run time holds pointers instead, so these
// wrap each pointer once (hold_any) and hand it to the container.
//
// A rule of Elements' tiles worth knowing: a column is as wide as the narrowest *maximum* width
// of its children, so one row made only of fixed-size buttons would cap the whole column at the
// width of that row. Row() and Col() therefore report no maximum width; a row of fixed buttons
// then sits at the left of a wide column instead of narrowing it.

#include "ui/DragScroll.h"

#include <elements.hpp>

#include <initializer_list>
#include <string>
#include <vector>

namespace nanoq::ui
{
namespace el = cycfi::elements;
using Ptr = el::element_ptr;

/// What a swappable region of the UI is: an element that shows another, replaceable one.
using Slot = el::indirect<el::shared_element<el::element>>;
using SlotPtr = std::shared_ptr<Slot>;

[[nodiscard]] inline SlotPtr MakeSlot()
{
    return std::make_shared<Slot>(el::hold_any(std::make_shared<el::element>()));
}

inline void Fill(Slot& slot, Ptr content)
{
    slot = std::move(content);
}

/// Lets its subject grow without limit in the chosen directions: the subject's own minimum, and
/// no maximum. A scroller otherwise reports its content's height as its maximum, which would
/// cap the whole window at the height of the page it is showing.
template <el::concepts::Element Subject> class ExpandElement : public el::proxy<Subject>
{
  public:
    using base_type = el::proxy<Subject>;

    ExpandElement(Subject subject, bool horizontal, bool vertical)
        : base_type(std::move(subject)), mHorizontal(horizontal), mVertical(vertical)
    {
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        auto l = this->subject().limits(ctx);

        if (mHorizontal)
        {
            l.max.x = el::full_extent;
        }

        if (mVertical)
        {
            l.max.y = el::full_extent;
        }

        return l;
    }

  private:
    bool mHorizontal, mVertical;
};

[[nodiscard]] inline Ptr Expand(Ptr content, bool horizontal = true, bool vertical = true)
{
    return el::share(ExpandElement(el::hold_any(std::move(content)), horizontal, vertical));
}

[[nodiscard]] inline Ptr ExpandWidth(Ptr content)
{
    return Expand(std::move(content), true, false);
}

/// Between `lo` and `hi` wide, whatever its content asks: a column that shares a bar's width.
template <el::concepts::Element Subject> class WidthRangeElement : public el::proxy<Subject>
{
  public:
    using base_type = el::proxy<Subject>;

    WidthRangeElement(Subject subject, float lo, float hi) : base_type(std::move(subject)), mLo(lo), mHi(hi)
    {
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        auto l = this->subject().limits(ctx);
        l.min.x = mLo;
        l.max.x = mHi;
        return l;
    }

  private:
    float mLo, mHi;
};

[[nodiscard]] inline Ptr WidthRange(float lo, float hi, Ptr content)
{
    return el::share(WidthRangeElement(el::hold_any(std::move(content)), lo, hi));
}

/// No minimum size: a region built for the window's last size, rebuilt for the new one at the
/// next tick, does not hold the window at the old size meanwhile.
template <el::concepts::Element Subject> class YieldingElement : public el::proxy<Subject>
{
  public:
    using base_type = el::proxy<Subject>;

    explicit YieldingElement(Subject subject) : base_type(std::move(subject))
    {
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        auto l = this->subject().limits(ctx);
        l.min = {0.0f, 0.0f};
        return l;
    }
};

[[nodiscard]] inline Ptr Yielding(Ptr content)
{
    return el::share(YieldingElement(el::hold_any(std::move(content))));
}

/// Never smaller than w x h: the window's floor, which the host is told (clap gui resize hints).
template <el::concepts::Element Subject> class MinSizeElement : public el::proxy<Subject>
{
  public:
    using base_type = el::proxy<Subject>;

    MinSizeElement(Subject subject, float w, float h) : base_type(std::move(subject)), mW(w), mH(h)
    {
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        auto l = this->subject().limits(ctx);
        l.min.x = std::max(l.min.x, mW);
        l.min.y = std::max(l.min.y, mH);
        l.max.x = std::max(l.max.x, l.min.x);
        l.max.y = std::max(l.max.y, l.min.y);
        return l;
    }

  private:
    float mW, mH;
};

[[nodiscard]] inline Ptr MinSize(float w, float h, Ptr content)
{
    return el::share(MinSizeElement(el::hold_any(std::move(content)), w, h));
}

/// Children left to right. A child of fixed width keeps it; the rest share what is left.
[[nodiscard]] inline Ptr Row(const std::vector<Ptr>& children)
{
    auto row = std::make_shared<el::htile_composite>();

    for (const auto& c : children)
    {
        row->push_back(c);
    }

    return ExpandWidth(row);
}

/// Children left to right, as wide as they are together: a group of buttons in a bar.
[[nodiscard]] inline Ptr Tight(const std::vector<Ptr>& children)
{
    auto row = std::make_shared<el::htile_composite>();

    for (const auto& c : children)
    {
        row->push_back(c);
    }

    return row;
}

/// Children top to bottom.
[[nodiscard]] inline Ptr Col(const std::vector<Ptr>& children)
{
    auto col = std::make_shared<el::vtile_composite>();

    for (const auto& c : children)
    {
        col->push_back(c);
    }

    return ExpandWidth(col);
}

/// `top` drawn over `bottom`, both given the same bounds.
[[nodiscard]] inline Ptr Over(Ptr top, Ptr bottom)
{
    auto layers = std::make_shared<el::layer_composite>();
    layers->push_back(std::move(bottom));
    layers->push_back(std::move(top));
    return layers;
}

/// Centred vertically at its own height in whatever height it is given: text in a fixed-height
/// row (a fixed height is no taller than its content allows).
[[nodiscard]] inline Ptr Middle(Ptr content)
{
    return el::share(el::align_middle(el::hold_any(std::move(content))));
}

/// Centred both ways at its own size.
[[nodiscard]] inline Ptr Centred(Ptr content)
{
    return el::share(el::align_center(el::align_middle(el::hold_any(std::move(content)))));
}

[[nodiscard]] inline Ptr Pad(float left, float top, float right, float bottom, Ptr content)
{
    return el::share(el::margin({left, top, right, bottom}, el::hold_any(std::move(content))));
}

[[nodiscard]] inline Ptr Pad(float all, Ptr content)
{
    return Pad(all, all, all, all, std::move(content));
}

[[nodiscard]] inline Ptr PadXY(float x, float y, Ptr content)
{
    return Pad(x, y, x, y, std::move(content));
}

[[nodiscard]] inline Ptr FixedHeight(float h, Ptr content)
{
    return el::share(el::vsize(h, el::hold_any(std::move(content))));
}

[[nodiscard]] inline Ptr FixedWidth(float w, Ptr content)
{
    return el::share(el::hsize(w, el::hold_any(std::move(content))));
}

/// Takes whatever room is left in a row or column. A small weight lets it give way to a
/// neighbour that wants the room more (a title beside a spacer).
[[nodiscard]] inline Ptr Stretch(Ptr content = nullptr, double weight = 1.0)
{
    if (!content)
    {
        content = std::make_shared<el::element>();
    }

    return el::share(el::hstretch(weight, el::hold_any(std::move(content))));
}

[[nodiscard]] inline Ptr Fill(el::color colour, Ptr content)
{
    return Over(std::move(content), el::share(el::box(colour)));
}

[[nodiscard]] inline Ptr Rounded(el::color colour, float radius, Ptr content)
{
    return Over(std::move(content), el::share(el::rbox(colour, radius)));
}

/// A fixed-size spacer in both directions. In a row or column prefer HGap / VGap: a fixed
/// width in a column would fix the whole column's.
[[nodiscard]] inline Ptr Space(float w, float h)
{
    return el::share(el::hsize(w, el::vsize(h, el::element{})));
}

/// A gap in a row: fixed width, any height.
[[nodiscard]] inline Ptr HGap(float w)
{
    return el::share(el::hsize(w, el::element{}));
}

/// A gap in a column: fixed height, any width.
[[nodiscard]] inline Ptr VGap(float h)
{
    return el::share(el::vsize(h, el::element{}));
}

/// Elements' scroller, with a drag on what it holds scrolling it as well (DragScroll.h).
[[nodiscard]] inline std::shared_ptr<DragScroller> MakeScroller(Ptr content, int traits)
{
    using Held = decltype(el::hold_any(content));
    return el::share(el::proxy<Held, DragScroller>{el::hold_any(std::move(content)), traits});
}

[[nodiscard]] inline Ptr VScroll(Ptr content)
{
    return Expand(MakeScroller(std::move(content), el::no_hscroll));
}

/// A VScroll that stays where it was scrolled to when its page is rebuilt: `keep` holds the
/// scroller, and the next one starts where it left off. Reset `keep` to start at the top.
[[nodiscard]] inline Ptr KeptVScroll(Ptr content, std::weak_ptr<el::port_base>& keep)
{
    const auto old = keep.lock();
    auto scroller = MakeScroller(std::move(content), el::no_hscroll);
    scroller->valign(old ? old->valign() : 0.0);
    keep = scroller;
    return Expand(scroller);
}

[[nodiscard]] inline Ptr HScroll(Ptr content)
{
    return Expand(MakeScroller(std::move(content), el::no_vscroll));
}
} // namespace nanoq::ui
