#pragma once

// Dragging in the preset library, as the web UI's (presets/drag.ts, folderDrag.ts, setlists.ts,
// setlistPadDrop.ts): a preset (a card of the chooser, or the preset selector's name) onto a
// folder, the setlist editor or a setlist pad; a folder onto another folder; a setlist slot onto
// another slot. Shell::DragLibraryItem (PresetDrag.cpp) follows the pointer and acts on the drop.
//
// What a drag can land on is a DropArea round the element that takes it. It notes where it was
// last drawn and, while the drag is over it, outlines it in the accent. A drag only weighs the
// areas drawn since it began (it redraws the window as it begins), so an area on a page not on
// show, or scrolled out of its list, is not under the pointer. Where they overlap, one in the
// chooser is over one that is not (it is a layer over the window), and one inside another over
// that one: a folder over the chooser's own face, and that face over a pad under it. (Not the
// order they were drawn in: a caret's blink redraws the face without the folders in it.)
//
// What can be dragged is wrapped in a DragSource. The press still goes to the control inside, so
// a click is a click; once the pointer has moved kDragSlop the drag is the source's, and the
// control lets go far away (kReleaseAway), so its release is no click.

#include "ui/Drawn.h"
#include "ui/widgets/DragGesture.h"
#include "ui/widgets/DragScroll.h"
#include "ui/widgets/LongPress.h"
#include "ui/widgets/Theme.h"

#include <elements.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace nanoq::ui
{
namespace el = cycfi::elements;

/// What is being dragged in the preset library.
struct LibraryDrag
{
    enum class What
    {
        Preset,      // onto a folder, the setlist editor or a setlist pad
        Folder,      // onto another folder: nested there
        SetlistSlot, // onto another slot of its setlist: moved there
    };

    What what = What::Preset;
    std::string id;   // the preset's or folder's id, or the slot's setlist
    std::string name; // what the ghost by the pointer reads
    int index = -1;   // the slot's place
    bool fromChooser = false;
};

/// Somewhere a drag can land, as its DropArea last drew it.
struct DropSpot
{
    enum class Kind
    {
        Folder,       // a folder of the chooser's column (or All Presets, Recents, Favourites)
        SetlistPanel, // the setlist editor: a preset dropped there is added to the setlist
        SetlistSlot,  // a slot of the setlist editor: another slot dropped there moves there
        Pad,          // a setlist pad: a preset dropped there goes on that pad
        Cover,        // the chooser's face: nothing lands on it, but what is under it is covered
    };

    Kind kind = Kind::Cover;
    std::string id;       // the folder's id, or the setlist's
    std::string parentId; // a folder's parent ("" at the top level)
    int depth = 0;        // a folder's nesting
    int index = -1;       // a slot's or pad's place
    bool inChooser = false;

    el::rect bounds;
    int nesting = 0;    // how many areas it is inside
    bool drawn = false; // drawn since the drag began
    bool lit = false;

    /// Whether this is over `other` where they overlap.
    [[nodiscard]] bool Over(const DropSpot& other) const
    {
        return inChooser != other.inChooser ? inChooser : nesting > other.nesting;
    }
};

class DropArea final : public el::proxy_base
{
  public:
    DropArea(const Theme& theme, std::shared_ptr<DropSpot> spot, el::element_ptr subject)
        : mTheme(theme), mSpot(std::move(spot)), mSubject(std::move(subject))
    {
    }

    el::element& subject() override
    {
        return *mSubject;
    }

    const el::element& subject() const override
    {
        return *mSubject;
    }

    void draw(const el::context& ctx) override
    {
        // The areas drawn inside this one count it.
        static int nesting = 0;
        mSpot->bounds = ctx.bounds;
        mSpot->nesting = nesting;
        mSpot->drawn = true;
        ++nesting;
        proxy_base::draw(ctx);
        --nesting;

        // Where the drag would land (.drag-over): an accent edge and a faint wash.
        if (mSpot->lit)
        {
            auto& cnv = ctx.canvas;
            const auto b = ctx.bounds.inset(1.0f, 1.0f);
            cnv.fill_style(mTheme.Accent().opacity(0.12f));
            cnv.fill_round_rect(b, 6.0f);
            cnv.stroke_style(mTheme.Accent());
            cnv.line_width(2.0f);
            cnv.stroke_round_rect(b, 6.0f);
        }
    }

  private:
    const Theme& mTheme;
    std::shared_ptr<DropSpot> mSpot;
    el::element_ptr mSubject;
};

class DragSource final : public el::proxy_base
{
  public:
    explicit DragSource(el::element_ptr subject) : mSubject(std::move(subject))
    {
    }

    /// Gone mid-drag (the chooser rebuilt, or closed): the release will never come here, so the
    /// drag ends now, landing nowhere, and takes its ghost and fade with it.
    ~DragSource() override
    {
        if (mDragging && onDrag)
        {
            const auto end = onDrag;
            end({kReleaseAway, kReleaseAway}, true);
        }
    }

    /// Where the pointer is as it is dragged, and where it was let go (`done`), in the view's
    /// coordinates. A drag a list took over for its scroll ends far away.
    std::function<void(el::point to, bool done)> onDrag;

    el::element& subject() override
    {
        return *mSubject;
    }

    const el::element& subject() const override
    {
        return *mSubject;
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::left)
        {
            if (btn.down)
            {
                mDownAt = btn.pos;
                mPressed = true;
                mDragging = false;
            }
            else
            {
                const bool dragged = mDragging;
                mPressed = false;
                mDragging = false;

                if (dragged)
                {
                    // A drop is no click: the control was let go of when the drag began.
                    if (onDrag)
                    {
                        const auto drop = onDrag; // the drop may rebuild what this sits in
                        drop(btn.pos, true);
                    }

                    return true;
                }
            }
        }

        return proxy_base::click(ctx, btn);
    }

    void drag(const el::context& ctx, el::mouse_button btn) override
    {
        if (mPressed && onDrag && !mDragging && std::hypot(btn.pos.x - mDownAt.x, btn.pos.y - mDownAt.y) >= kDragSlop)
        {
            mDragging = true;
            LongPress::Cancel();
            el::mouse_button away = btn;
            away.down = false;
            away.pos = {kReleaseAway, kReleaseAway};
            proxy_base::click(ctx, away);
        }

        if (mDragging)
        {
            onDrag(btn.pos, false);
            return;
        }

        proxy_base::drag(ctx, btn);
    }

  private:
    el::element_ptr mSubject;
    el::point mDownAt;
    bool mPressed = false, mDragging = false;
};

/// A layer that fades while a drag is over what it covers (the chooser, once a preset dragged out
/// of it has left it), as the web UI's .drag-released: drawn at `*alpha`, fully when 1.
class FadeLayer final : public el::proxy_base
{
  public:
    FadeLayer(std::shared_ptr<float> alpha, el::element_ptr subject)
        : mAlpha(std::move(alpha)), mSubject(std::move(subject))
    {
    }

    el::element& subject() override
    {
        return *mSubject;
    }

    const el::element& subject() const override
    {
        return *mSubject;
    }

    void draw(const el::context& ctx) override
    {
        if (*mAlpha >= 1.0f)
        {
            proxy_base::draw(ctx);
            return;
        }

        auto group = ctx.canvas.new_group(*mAlpha);
        proxy_base::draw(ctx);
    }

  private:
    std::shared_ptr<float> mAlpha;
    el::element_ptr mSubject;
};
} // namespace nanoq::ui
