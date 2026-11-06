#pragma once
// A waveform from the engine's peaks, for riff capture and the looper (the web UI's
// waveform/render.ts and looper/waveform.ts): 256 buckets of 0..1 drawn mirrored about the
// middle, with what a view needs over them. A take being recorded is drawn only as far as it
// has got (Trace) with the rest of the buffer shaded; a captured take carries a range with a
// handle at each end, dragged to trim it; the looper adds a second series (the take being
// recorded, in red), bar lines and a playhead.

#include "ui/Drawn.h"
#include "ui/widgets/TextDraw.h"
#include "ui/widgets/Theme.h"

#include <elements.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace nanoq::ui
{
namespace el = cycfi::elements;

class Waveform final : public el::element, public Drawn
{
  public:
    explicit Waveform(const Theme& theme, float height = 80.0f) : mTheme(theme), mHeight(height)
    {
    }

    /// The peaks (0..1), and how far along they are real: past `trace` (0..1) the buffer is
    /// still empty and shaded.
    void Peaks(std::vector<float> peaks, float trace = 1.0f)
    {
        mPeaks = std::move(peaks);
        mTrace = std::clamp(trace, 0.0f, 1.0f);
    }

    /// A second series over the first: the take the looper is recording.
    void Overlay(std::vector<float> peaks)
    {
        mOverlay = std::move(peaks);
    }

    /// The playhead's place (0..1), or below 0 for none.
    void Playhead(float ratio)
    {
        mPlayhead = ratio;
    }

    /// Lines across the waveform at these places (0..1): a Bars loop's bars.
    void BarLines(std::vector<float> ratios)
    {
        mBars = std::move(ratios);
    }

    /// A trim range with a handle at each end; `onRange` hears each move, and `done` once the
    /// drag ends. Without a range (start >= end) nothing can be dragged.
    void Range(float start, float end)
    {
        mStart = std::clamp(start, 0.0f, 1.0f);
        mEnd = std::clamp(end, 0.0f, 1.0f);
    }

    void RangeEnabled(bool enabled)
    {
        mRangeEnabled = enabled;
    }

    [[nodiscard]] float RangeStart() const
    {
        return mStart;
    }

    [[nodiscard]] float RangeEnd() const
    {
        return mEnd;
    }

    /// Shown while there are no peaks.
    void Empty(std::string text)
    {
        mEmptyText = std::move(text);
    }

    void Colour(el::color wave)
    {
        mWave = wave;
        mHaveWave = true;
    }

    std::function<void(float start, float end, bool done)> onRange;
    /// A tap with no range to drag: the looper strip seeks nothing, but a view may want the tap.
    std::function<void()> onTap;

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{60.0f, mHeight}, {el::full_extent, mHeight}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.Pick(el::rgba(0, 0, 0, 70), el::rgba(0, 0, 0, 18)));
        cnv.fill_round_rect(b, 6.0f);

        const float mid = b.top + b.height() * 0.5f;
        const float half = b.height() * 0.5f - 4.0f;
        const auto bars = [&](const std::vector<float>& peaks, el::color colour, float limit) {
            if (peaks.empty())
            {
                return;
            }

            const float step = b.width() / static_cast<float>(peaks.size());
            cnv.fill_style(colour);

            for (std::size_t i = 0; i < peaks.size(); ++i)
            {
                const float x = b.left + step * static_cast<float>(i);

                if (x - b.left >= limit * b.width())
                {
                    break;
                }

                const float a = std::max(0.5f, std::clamp(peaks[i], 0.0f, 1.0f) * half);
                cnv.fill_rect(el::rect{x, mid - a, x + std::max(1.0f, step - 0.5f), mid + a});
            }
        };

        const el::color wave = mHaveWave ? mWave : mTheme.Accent().opacity(0.85f);

        if (mPeaks.empty() && mOverlay.empty() && !mEmptyText.empty())
        {
            FillText(cnv, mEmptyText, Font(Weight::Regular).size(11.0f), mTheme.TextMuted(), b, HAlign::Centre);
        }

        bars(mPeaks, wave, mTrace);
        bars(mOverlay, el::rgba(224, 82, 90, 220), 1.0f);

        // The buffer not filled yet.
        if (mTrace < 1.0f && !mPeaks.empty())
        {
            cnv.fill_style(mTheme.Text().opacity(0.05f));
            cnv.fill_rect(el::rect{b.left + mTrace * b.width(), b.top, b.right, b.bottom});
        }

        cnv.stroke_style(mTheme.Text().opacity(0.18f));
        cnv.line_width(1.0f);

        for (const float r : mBars)
        {
            const float x = std::round(b.left + r * b.width()) + 0.5f;
            cnv.begin_path();
            cnv.move_to(x, b.top + 2.0f);
            cnv.line_to(x, b.bottom - 2.0f);
            cnv.stroke();
        }

        // The trim range: outside it shaded, a handle at each end.
        if (mStart < mEnd && mRangeEnabled)
        {
            const float x0 = b.left + mStart * b.width(), x1 = b.left + mEnd * b.width();
            cnv.fill_style(el::rgba(0, 0, 0, 110));
            cnv.fill_rect(el::rect{b.left, b.top, x0, b.bottom});
            cnv.fill_rect(el::rect{x1, b.top, b.right, b.bottom});

            for (const float x : {x0, x1})
            {
                cnv.fill_style(mTheme.Accent());
                cnv.fill_rect(el::rect{x - 1.0f, b.top, x + 1.0f, b.bottom});
                cnv.fill_round_rect(el::rect{x - 4.0f, b.top, x + 4.0f, b.top + 12.0f}, 2.0f);
            }
        }

        if (mPlayhead >= 0.0f && mPlayhead <= 1.0f)
        {
            const float x = std::round(b.left + mPlayhead * b.width()) + 0.5f;
            cnv.stroke_style(mTheme.Text().opacity(0.9f));
            cnv.begin_path();
            cnv.move_to(x, b.top + 1.0f);
            cnv.line_to(x, b.bottom - 1.0f);
            cnv.stroke();
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (!(mRangeEnabled && mStart < mEnd && onRange))
        {
            if (!btn.down && onTap)
            {
                onTap();
            }

            return static_cast<bool>(onTap);
        }

        if (btn.down)
        {
            // The handle nearer the press moves; a press between them takes the nearer one there.
            const float r = Ratio(ctx, btn.pos);
            mDragging = std::abs(r - mStart) <= std::abs(r - mEnd) ? 0 : 1;
            Move(ctx, r, false);
        }
        else if (mDragging >= 0)
        {
            Move(ctx, Ratio(ctx, btn.pos), true);
            mDragging = -1;
        }

        return true;
    }

    void drag(const el::context& ctx, el::mouse_button btn) override
    {
        if (mDragging >= 0)
        {
            Move(ctx, Ratio(ctx, btn.pos), false);
        }
    }

    bool wants_control() const override
    {
        return (mRangeEnabled && static_cast<bool>(onRange)) || static_cast<bool>(onTap);
    }

  private:
    [[nodiscard]] static float Ratio(const el::context& ctx, el::point p)
    {
        return std::clamp((p.x - ctx.bounds.left) / std::max(1.0f, ctx.bounds.width()), 0.0f, 1.0f);
    }

    void Move(const el::context& ctx, float r, bool done)
    {
        // A handle dragged past the other swaps with it, as the web UI's range does.
        constexpr float kMinSpan = 0.001f;

        if (mDragging == 0)
        {
            mStart = r;
        }
        else
        {
            mEnd = r;
        }

        if (mStart > mEnd)
        {
            std::swap(mStart, mEnd);
            mDragging = 1 - mDragging;
        }

        if (mEnd - mStart < kMinSpan)
        {
            if (mDragging == 0)
            {
                mStart = std::max(0.0f, mEnd - kMinSpan);
            }
            else
            {
                mEnd = std::min(1.0f, mStart + kMinSpan);
            }
        }

        ctx.view.refresh(ctx);

        if (onRange)
        {
            onRange(mStart, mEnd, done);
        }
    }

    const Theme& mTheme;
    float mHeight;
    std::vector<float> mPeaks, mOverlay, mBars;
    float mTrace = 1.0f;
    float mPlayhead = -1.0f;
    float mStart = 0.0f, mEnd = 1.0f;
    bool mRangeEnabled = false;
    int mDragging = -1;
    std::string mEmptyText;
    el::color mWave;
    bool mHaveWave = false;
};
} // namespace nanoq::ui
