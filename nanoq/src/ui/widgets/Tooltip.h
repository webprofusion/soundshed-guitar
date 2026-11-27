#pragma once

// Hover tooltips, as the web UI's title= attributes: a control under a resting pointer says what
// it is in a small box under it (above it, where there is no room below). One at a time; the
// pointer rests 600 ms first, and a press hides it until the pointer leaves the control.
//
// A control calls Hover() as the pointer enters or moves over it, with its bounds in view
// coordinates (as a click's), and Leave() as it goes; Press() on a press anywhere. The shell's
// tick calls Tick(), which says when the tip came up or went, and the root's KeyRouter draws it
// over everything with Draw().

#include "ui/widgets/TextDraw.h"
#include "ui/widgets/Theme.h"

#include <elements.hpp>

#include <algorithm>
#include <chrono>
#include <string>

namespace nanoq::ui
{
namespace el = cycfi::elements;

class Tooltips
{
  public:
    static void Hover(const void* owner, const el::rect& at, const std::string& text)
    {
        auto& s = State();

        if (text.empty())
        {
            Leave(owner);
            return;
        }

        if (s.owner != owner || s.text != text)
        {
            s.owner = owner;
            s.text = text;
            s.since = Clock::now();
            s.pressed = false;
        }

        s.at = at;
    }

    static void Leave(const void* owner)
    {
        auto& s = State();

        if (s.owner == owner)
        {
            s.owner = nullptr;
            s.text.clear();
            s.pressed = false;
        }
    }

    /// A press, a key or a scroll: the tip goes, and stays away while the pointer stays.
    static void Press()
    {
        State().pressed = true;
    }

    /// The pointer, in view coordinates, from the root (KeyRouter): `inside` false once it has
    /// left the window.
    static void Pointer(el::point at, bool inside)
    {
        auto& s = State();
        s.pointer = at;
        s.inside = inside;
    }

    /// Whether the tip came up or went since the last tick: the window is redrawn when it did.
    static bool Tick()
    {
        auto& s = State();

        // Its control was rebuilt, or the pointer left the window, with no Leave().
        if (s.owner != nullptr && (!s.inside || !s.at.inset(-1.0f, -1.0f).includes(s.pointer)))
        {
            s.owner = nullptr;
            s.text.clear();
        }

        const bool show = s.owner != nullptr && !s.pressed && Clock::now() - s.since >= std::chrono::milliseconds(600);

        if (show == s.shown)
        {
            return false;
        }

        s.shown = show;
        return true;
    }

    /// The tip (.tooltip-ish: the elevated surface, a 1 px border, 11 px text, radius 4), centred
    /// under its control and kept inside `view`.
    static void Draw(const el::context& ctx, const Theme& theme)
    {
        auto& s = State();

        if (!s.shown || s.text.empty())
        {
            return;
        }

        auto& cnv = ctx.canvas;
        const auto view = ctx.bounds;
        const auto font = Font(Weight::Medium).size(11.0f);
        const float w = std::min(TrackedWidth(cnv, s.text, font, 0.0f) + 16.0f, std::max(40.0f, view.width() - 16.0f));
        constexpr float kHeight = 22.0f;
        float left = (s.at.left + s.at.right) * 0.5f - w * 0.5f;
        left = std::clamp(left, view.left + 8.0f, std::max(view.left + 8.0f, view.right - 8.0f - w));
        float top = s.at.bottom + 6.0f;

        if (top + kHeight > view.bottom - 4.0f)
        {
            top = s.at.top - 6.0f - kHeight;
        }

        const el::rect box{left, top, left + w, top + kHeight};
        auto state = cnv.new_state();
        cnv.shadow_style(el::point{0.0f, 2.0f}, 8.0f, el::rgba(0, 0, 0, 110));
        cnv.fill_style(theme.Elevated());
        cnv.add_round_rect(box, 4.0f);
        cnv.fill();
        cnv.shadow_style(el::point{0.0f, 0.0f}, 0.0f, el::rgba(0, 0, 0, 0));
        cnv.stroke_style(theme.BorderStrong());
        cnv.line_width(1.0f);
        cnv.add_round_rect(box.inset(0.5f, 0.5f), 3.5f);
        cnv.stroke();
        FillText(cnv, s.text, font, theme.Text(), box.inset(8.0f, 0.0f), HAlign::Centre);
    }

  private:
    using Clock = std::chrono::steady_clock;

    struct Shared
    {
        const void* owner = nullptr;
        std::string text;
        el::rect at;
        Clock::time_point since;
        el::point pointer;
        bool inside = false;
        bool pressed = false;
        bool shown = false;
    };

    static Shared& State()
    {
        static Shared shared;
        return shared;
    }
};
} // namespace nanoq::ui
