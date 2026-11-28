#include "ui/widgets/Widgets.h"

#include "ui/widgets/ControlSkin.h"
#include "ui/widgets/Graphics.h"
#include "ui/widgets/Layout.h"

#include <elements/support/text_utils.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>

namespace nanoq::ui
{
namespace
{
using cycfi::artist::canvas;
} // namespace

// ── Text ────────────────────────────────────────────────────────────────────────

Text::Text(const Theme& theme, std::string text, float size, Weight weight)
    : mTheme(theme), mText(std::move(text)), mSize(size), mWeight(weight), mColour(theme.Text())
{
}

el::view_limits Text::limits(const el::basic_context& ctx) const
{
    const auto font = mWeight == Weight::Title ? TitleFont(mSize) : Font(mWeight).size(mSize);
    auto size = el::measure_text(ctx.canvas, mText, font);
    size.x = mTracking > 0.0f ? TrackedWidth(ctx.canvas, mText, font, mTracking) : size.x;
    const float h = std::max(size.y, mSize * 1.3f);
    return mFixed ? el::view_limits{{size.x, h}, {size.x, h}} : el::view_limits{{0.0f, h}, {el::full_extent, h}};
}

void Text::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    const auto font = mWeight == Weight::Title ? TitleFont(mSize) : Font(mWeight).size(mSize);

    if (mTracking > 0.0f && TrackedWidth(ctx.canvas, mText, font, mTracking) <= ctx.bounds.width())
    {
        FillTrackedText(ctx.canvas, mText, font, mColour, ctx.bounds, mAlign, mTracking);
        return;
    }

    const auto shown = Ellipsize(ctx.canvas, mText, font, ctx.bounds.width());
    FillText(ctx.canvas, shown, font, mColour, ctx.bounds, mAlign);
}

Paragraph::Paragraph(const Theme& theme, std::string text, float width, float size)
    : mTheme(theme), mText(std::move(text)), mWidth(width), mSize(size), mColour(theme.TextSecondary())
{
}

std::vector<std::string> Paragraph::Lines(cycfi::artist::canvas& cnv) const
{
    // As many words as fit each line; a word wider than the line has one to itself. A line
    // break in the text starts a new line (an empty one for a blank line), as a description's
    // paragraphs read; the drawing would otherwise break there unmeasured.
    const auto font = Font(mWeight).size(mSize);
    std::vector<std::string> lines;
    std::string text = mText;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    std::size_t from = 0;

    while (from <= text.size())
    {
        const auto end = std::min(text.find('\n', from), text.size());
        const auto segment = text.substr(from, end - from);
        from = end + 1;
        const auto before = lines.size();
        std::string line;
        std::size_t i = 0;

        while (i < segment.size())
        {
            const auto space = segment.find(' ', i);
            const auto word = segment.substr(i, space == std::string::npos ? std::string::npos : space - i);
            i = space == std::string::npos ? segment.size() : space + 1;

            if (word.empty())
            {
                continue;
            }

            // A word wider than a whole line (a path, an id) is broken where it reaches the edge,
            // between characters (overflow-wrap: anywhere), never inside one.
            if (el::measure_text(cnv, word, font).x > mWidth)
            {
                if (!line.empty())
                {
                    lines.push_back(line);
                    line.clear();
                }

                std::string piece;

                for (std::size_t c = 0; c < word.size();)
                {
                    std::size_t next = c + 1;

                    while (next < word.size() && (static_cast<unsigned char>(word[next]) & 0xC0) == 0x80)
                    {
                        ++next;
                    }

                    const auto glyph = word.substr(c, next - c);

                    if (!piece.empty() && el::measure_text(cnv, piece + glyph, font).x > mWidth)
                    {
                        lines.push_back(piece);
                        piece.clear();
                    }

                    piece += glyph;
                    c = next;
                }

                line = piece;
                continue;
            }

            const auto joined = line.empty() ? word : line + " " + word;

            if (!line.empty() && el::measure_text(cnv, joined, font).x > mWidth)
            {
                lines.push_back(line);
                line = word;
            }
            else
            {
                line = joined;
            }
        }

        if (!line.empty())
        {
            lines.push_back(line);
        }

        if (lines.size() == before && end < text.size())
        {
            lines.emplace_back(); // a blank line between paragraphs
        }
    }

    return lines;
}

el::view_limits Paragraph::limits(const el::basic_context& ctx) const
{
    const float h = static_cast<float>(std::max<std::size_t>(1, Lines(ctx.canvas).size())) * (mSize + 5.0f);
    return {{mWidth, h}, {mWidth, h}};
}

float Paragraph::Height() const
{
    return static_cast<float>(std::max<std::size_t>(1, Lines(MeasureCanvas()).size())) * (mSize + 5.0f);
}

void Paragraph::draw(const el::context& ctx)
{
    const auto font = Font(mWeight).size(mSize);
    float y = ctx.bounds.top;

    for (const auto& line : Lines(ctx.canvas))
    {
        FillText(ctx.canvas, line, font, mColour, el::rect{ctx.bounds.left, y, ctx.bounds.right, y + mSize + 5.0f},
                 HAlign::Left);
        y += mSize + 5.0f;
    }
}

Paragraph& Paragraph::Weighted(Weight weight)
{
    mWeight = weight;
    return *this;
}

Paragraph& Paragraph::Colour(el::color c)
{
    mColour = c;
    return *this;
}

std::optional<AccessInfo> Text::DescribeStatic() const
{
    int level = mHeading;

    // A title is a heading of its own; a section's label says so (AccessHeading), as small
    // capitals also caption knobs and badges.
    if (level < 0)
    {
        level = mWeight == Weight::Title ? 1 : 0;
    }

    if (level <= 0 || mText.empty())
    {
        return std::nullopt;
    }

    AccessInfo info;
    info.role = AccessRole::Heading;
    info.level = level;
    // Small capitals are drawn from the text as written; a screen reader spells out capitals.
    info.name = mText;
    return info;
}

Text& Text::Set(std::string text)
{
    mText = std::move(text);
    return *this;
}

Text& Text::Colour(el::color c)
{
    mColour = c;
    return *this;
}

Text& Text::Alignment(Align a)
{
    mAlign = a;
    return *this;
}

Text& Text::Fixed(bool fixed)
{
    mFixed = fixed;
    return *this;
}

Text& Text::Tracking(float em)
{
    mTracking = std::max(0.0f, em);
    return *this;
}

// ── Button ──────────────────────────────────────────────────────────────────────

Button::Button(const Theme& theme, std::string text, std::function<void()> onClick, Style style)
    : mTheme(theme), mText(std::move(text)), mOnClick(std::move(onClick)), mStyle(style)
{
}

el::view_limits Button::limits(const el::basic_context& ctx) const
{
    const auto font = Font(Weight::Medium).size(mSize);
    const auto size = el::measure_text(ctx.canvas, mText, font);
    const float h = metrics::kRowHeight;

    if (mFlexible)
    {
        return {{mMinWidth, h}, {el::full_extent, h}};
    }

    const float w = std::max(mMinWidth, size.x + 2.0f * metrics::kPad + (mDot ? 14.0f : 0.0f));
    return {{w, h}, {w, h}};
}

void Button::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = mFaceHeight > 0.0f && metrics::Touch() ? metrics::Face(ctx.bounds, ctx.bounds.width(), mFaceHeight)
                                                          : ctx.bounds;
    const bool lit = mOn || mSelected;

    // A theme that draws its own keys (Studio 1973's cream caps): its plain, latched or
    // confirming key, as the web UI's .btn, .btn.active and .btn-primary.
    if (mStyle != Style::Flat)
    {
        const auto& skin = mTheme.Controls();
        const bool primary = mStyle == Style::Primary;
        const auto& key = primary ? skin.keys.primary : mOn ? skin.keys.on : skin.keys.plain;

        if (key.drawn)
        {
            const auto& face = primary ? skin.backdrops.keyPrimary
                               : mOn   ? skin.backdrops.keyOn
                                       : skin.backdrops.keyPlain;
            // A chosen option (theme, scene) is outlined in the accent, a switch is not.
            const auto r = DrawKey(cnv, mTheme, b, key, face, mHover && mEnabled, mDown,
                                   mSelected && !mOn ? Theme::ArgbOf(mTheme.Accent()) : 0u);
            const el::color ink = Theme::Argb(key.ink).opacity(mEnabled ? 1.0f : 0.4f);
            const auto font = Font(Weight::Medium).size(mSize);
            const auto textBounds = r.inset(metrics::kPad * 0.5f, 0.0f);
            FillText(cnv, Ellipsize(cnv, mText, font, textBounds.width()), font, ink, textBounds, mAlign);
            return;
        }
    }

    el::color fill = mTheme.Input();
    el::color line = mTheme.BorderStrong();
    el::color ink = mTheme.Text();

    switch (mStyle)
    {
    case Style::Primary:
        fill = mTheme.AccentFill();
        line = fill;
        ink = mTheme.OnAccent();
        break;
    case Style::Flat:
        fill = lit ? mTheme.Input() : el::rgba(0, 0, 0, 0);
        line = lit ? mTheme.BorderStrong() : el::rgba(0, 0, 0, 0);
        ink = lit ? mTheme.Text() : mTheme.TextSecondary();
        break;
    case Style::Plain:

        if (lit)
        {
            fill = mTheme.Elevated();

            if (mSelected)
            {
                line = mTheme.Accent(); // a chosen option (theme, scene) is outlined, a switch is not
            }
        }

        break;
    }

    if (mDown)
    {
        fill = fill.opacity(0.7f);
    }
    else if (mHover && mEnabled && fill.alpha > 0.0f)
    {
        fill = fill.level(1.12f);
    }
    else if (mHover && mStyle == Style::Flat)
    {
        fill = mTheme.Elevated().opacity(0.6f);
    }

    if (!mEnabled)
    {
        ink = ink.opacity(0.4f);
    }

    if (fill.alpha > 0.0f)
    {
        cnv.fill_style(fill);
        cnv.fill_round_rect(b, metrics::kRadius);
    }

    if (line.alpha > 0.0f)
    {
        cnv.stroke_style(line);
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), metrics::kRadius);
    }

    auto textBounds = b;

    if (mDot)
    {
        // The state dot: lit when on.
        const float r = 4.0f;
        const el::point c{b.left + metrics::kPad + r, b.top + b.height() * 0.5f};
        cnv.fill_style(mOn ? mTheme.Success() : mTheme.TextMuted().opacity(0.5f));
        cnv.add_circle(cycfi::artist::circle(c, r));
        cnv.fill();
        textBounds.left += metrics::kPad + 2.0f * r + 6.0f;
        textBounds.right -= metrics::kPad * 0.5f;
        FillText(cnv, mText, Font(Weight::Medium).size(mSize), ink, textBounds, Text::Align::Left);
        return;
    }

    const auto font = Font(Weight::Medium).size(mSize);
    textBounds = textBounds.inset(metrics::kPad * 0.5f, 0.0f);
    FillText(cnv, Ellipsize(cnv, mText, font, textBounds.width()), font, ink, textBounds, mAlign);
}

bool Button::click(const el::context& ctx, el::mouse_button btn)
{
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

    const bool wasDown = mDown;
    mDown = false;
    ctx.view.refresh(ctx);

    if (wasDown && ctx.bounds.includes(btn.pos))
    {
        RunFromCopy(mOnClick); // it may rebuild the row this button is in
    }

    return true;
}

bool Button::key(const el::context& ctx, el::key_info k)
{
    return IsPressKey(k) && Activate(ctx.bounds);
}

AccessInfo Button::Describe() const
{
    AccessInfo info;
    info.role = mDot ? AccessRole::Toggle : AccessRole::Button;
    info.name = NameOr(mText);
    info.toggled = mDot ? (mOn ? 1 : 0) : -1;
    info.selected = mSelected;
    info.enabled = mEnabled;
    return info;
}

bool Button::Activate(const el::rect&)
{
    if (!mEnabled || !mOnClick)
    {
        return false;
    }

    const auto run = mOnClick; // it may rebuild the region this sits in
    run();
    return true;
}

bool Button::cursor(const el::context& ctx, el::point, el::cursor_tracking status)
{
    const bool hover = status != el::cursor_tracking::leaving;

    if (hover != mHover)
    {
        mHover = hover;
        ctx.view.refresh(ctx);
    }

    return false;
}

Button& Button::Set(std::string text)
{
    mText = std::move(text);
    return *this;
}

Button& Button::On(bool on)
{
    mOn = on;
    return *this;
}

Button& Button::Selected(bool selected)
{
    mSelected = selected;
    return *this;
}

Button& Button::Enabled(bool enabled)
{
    mEnabled = enabled;
    return *this;
}

Button& Button::MinWidth(float w)
{
    mMinWidth = w;
    return *this;
}

Button& Button::Dot(bool show)
{
    mDot = show;
    return *this;
}

Button& Button::Flexible(bool flexible)
{
    mFlexible = flexible;
    return *this;
}

Button& Button::Alignment(HAlign align)
{
    mAlign = align;
    return *this;
}

Button& Button::TextSize(float size)
{
    mSize = size;
    return *this;
}

// ── GroupHeading ────────────────────────────────────────────────────────────────

GroupHeading::GroupHeading(const Theme& theme, std::string text) : mTheme(theme), mText(std::move(text))
{
    for (auto& ch : mText)
    {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
}

el::view_limits GroupHeading::limits(const el::basic_context&) const
{
    return {{0.0f, 24.0f}, {el::full_extent, 24.0f}};
}

void GroupHeading::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const auto font = Font(Weight::SemiBold).size(11.0f);
    const float w = el::measure_text(cnv, mText, font).x;

    FillText(cnv, mText, font, mTheme.TextMuted(), el::rect{b.left, b.top + 4.0f, b.left + w + 2.0f, b.bottom},
             Text::Align::Left);

    const float y = b.top + 4.0f + (b.height() - 4.0f) * 0.5f;

    if (b.left + w + 14.0f < b.right)
    {
        cnv.stroke_style(mTheme.Border());
        cnv.line_width(1.0f);
        cnv.begin_path();
        cnv.move_to(b.left + w + 12.0f, y);
        cnv.line_to(b.right, y);
        cnv.stroke();
    }
}

// ── MeterBar ────────────────────────────────────────────────────────────────────

MeterBar::MeterBar(const Theme& theme) : mTheme(theme)
{
}

std::optional<AccessInfo> MeterBar::DescribeStatic() const
{
    AccessInfo info;
    info.role = AccessRole::Meter;
    info.name = mName.empty() ? std::string("Level") : mName;
    // Whole decibels: a reader polling it hears a number that holds still long enough to say.
    const double db = std::round(mPeakDb);
    info.value = db <= -60.0 ? std::string("silent") : std::to_string(static_cast<int>(db)) + " dB";

    if (mClipped)
    {
        info.value += ", clipping";
    }

    info.position = std::clamp((mPeakDb + 60.0) / 60.0, 0.0, 1.0);
    return info;
}

namespace
{
// The gauge face's geometry, in its own pixels (66 x 48), from
// tools/studio-1973-theme-assets/vu-face.mjs: the bezel, the needle's pivot below the window,
// its length and its sweep either side of upright, and the radius of the red peak scale's line,
// which the peak dot rides. The web UI's skin repeats them.
constexpr float kGaugeFaceW = 66.0f;
constexpr float kGaugeBezel = 2.0f;
constexpr float kGaugePivotX = 33.0f;
constexpr float kGaugePivotY = 52.0f;
constexpr float kGaugeNeedle = 41.0f;
constexpr float kGaugeSweep = 42.0f * 3.14159265f / 180.0f;
constexpr float kGaugePeakR = 31.5f;
// 0 VU at -18 dBFS RMS, the end stop at +3 VU (ts/vuMeter.ts, vuGaugeDeflection).
constexpr double kGaugeZeroDbfs = -18.0;
// A VU needle's time constant (99% of a step in about 300 ms), and the smallest move redrawn.
constexpr double kGaugeTau = 0.065;
constexpr float kGaugeMinStep = 0.004f;
// The red peak dot: the sample peak on the same sweep with 0 dBFS at the end stop, so -3 dBFS
// sits under 0 VU, falling 20 dB in 1.7 s (ts/vuMeter.ts, vuPeakDeflection).
constexpr double kGaugePeakFallDbPerS = 20.0 / 1.7;

float GaugeDeflection(double rmsDb)
{
    if (!std::isfinite(rmsDb))
    {
        return 0.0f;
    }

    return static_cast<float>(std::min(1.0, std::pow(10.0, (rmsDb - kGaugeZeroDbfs) / 20.0) / std::sqrt(2.0)));
}

float GaugePeakDeflection(double peakDb)
{
    if (!std::isfinite(peakDb))
    {
        return 0.0f;
    }

    const float d = static_cast<float>(std::min(1.0, std::pow(10.0, peakDb / 20.0)));
    return d < kGaugeMinStep ? 0.0f : d;
}
} // namespace

bool MeterBar::ThemeGauge(const Theme& theme)
{
    const auto& skin = theme.Controls();
    return !skin.gaugeFace.empty() && skin.gaugeWidth > 0.0f && skin.gaugeHeight > 0.0f;
}

float MeterBar::ColumnWidth(const Theme& theme)
{
    return ThemeGauge(theme) ? std::ceil(theme.Controls().gaugeWidth) : 36.0f;
}

namespace
{
// The web UI's .vu-meter: a 10 px readout, then 4 px down the ladder of eight 5 px segments
// 2 px apart, 14 px wide.
constexpr float kLadderTop = 14.0f;
constexpr float kSegmentHeight = 5.0f;
constexpr float kSegmentPitch = 7.0f;
constexpr float kSegmentWidth = 14.0f;
constexpr int kLadderSegments = 8;
} // namespace

float MeterBar::ColumnHeight(const Theme& theme)
{
    return ThemeGauge(theme) ? 84.0f
                             : kLadderTop + kSegmentPitch * (kLadderSegments - 1) + kSegmentHeight;
}

el::view_limits MeterBar::limits(const el::basic_context&) const
{
    if (mVertical)
    {
        const float w = ColumnWidth(mTheme);
        const float h = ColumnHeight(mTheme);
        return {{w, h}, {w, h}};
    }

    return {{60.0f, 8.0f}, {el::full_extent, 8.0f}};
}

void MeterBar::DrawGauge(canvas& cnv, el::rect face) const
{
    if (const auto image = mTheme.Picture(mTheme.Controls().gaugeFace))
    {
        cnv.draw(*image, face);
    }

    const float s = face.width() / kGaugeFaceW;
    const el::rect window = face.inset(kGaugeBezel * s, kGaugeBezel * s);
    const el::point pivot{face.left + kGaugePivotX * s, face.top + kGaugePivotY * s};
    const float length = kGaugeNeedle * s;

    auto state = cnv.new_state();
    cnv.begin_path();
    cnv.add_round_rect(window, 1.4f * s);
    cnv.clip();

    // The red peak dot on the peak scale's line, then the black needle over it, each with its
    // shadow on the card a little down and right first.
    const float peakA = -kGaugeSweep + 2.0f * kGaugeSweep * mPeakDotDrawn;
    const el::point dot{pivot.x + std::sin(peakA) * kGaugePeakR * s, pivot.y - std::cos(peakA) * kGaugePeakR * s};
    cnv.begin_path();
    cnv.add_circle(cycfi::artist::circle(el::point{dot.x + 0.6f * s, dot.y + 0.8f * s}, 1.5f * s));
    cnv.fill_style(el::rgba(70, 40, 10, 60));
    cnv.fill();
    const el::point glint{dot.x - 0.3f * s, dot.y - 0.3f * s};
    canvas::radial_gradient bead{glint, 0.0f, glint, 1.5f * s};
    bead.add_color_stop(0.0f, el::rgba(240, 100, 76, 255));
    bead.add_color_stop(0.55f, el::rgba(195, 40, 28, 255));
    bead.add_color_stop(1.0f, el::rgba(142, 24, 16, 255));
    cnv.begin_path();
    cnv.add_circle(cycfi::artist::circle(dot, 1.3f * s));
    cnv.fill_style(bead);
    cnv.fill();

    const float a = -kGaugeSweep + 2.0f * kGaugeSweep * mNeedleDrawn;
    const float dx = std::sin(a), dy = -std::cos(a);
    cnv.line_cap(canvas::butt);
    cnv.begin_path();
    cnv.move_to(el::point{pivot.x + 1.5f * s, pivot.y + 1.0f * s});
    cnv.line_to(el::point{pivot.x + 1.5f * s + dx * length, pivot.y + 1.0f * s + dy * length});
    cnv.stroke_style(el::rgba(70, 40, 10, 72));
    cnv.line_width(1.6f * s);
    cnv.stroke();
    cnv.begin_path();
    cnv.move_to(pivot);
    cnv.line_to(el::point{pivot.x + dx * length, pivot.y + dy * length});
    cnv.stroke_style(el::rgba(26, 18, 12, 255));
    cnv.line_width(0.9f * s);
    cnv.stroke();

    // The glass: a sheen across the top of the window.
    canvas::linear_gradient sheen{window.top_left(), el::point{window.left, window.top + window.height() * 0.4f}};
    sheen.add_color_stop(0.0f, el::rgba(255, 255, 255, 60));
    sheen.add_color_stop(0.9f, el::rgba(255, 255, 255, 14));
    sheen.add_color_stop(1.0f, el::rgba(255, 255, 255, 0));
    cnv.fill_style(sheen);
    cnv.fill_rect(el::rect{window.left, window.top, window.right, window.top + window.height() * 0.4f});
}

void MeterBar::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;

    if (mVertical)
    {
        // The peak over a column of eight segments, lit from the bottom: green, then amber in
        // the top two, red when it clips. Or the theme's needle gauge under the same number.
        std::string text = "-\xE2\x88\x9E dB"; // -inf

        if (mPeakDb > -60.0)
        {
            char number[16];
            std::snprintf(number, sizeof(number), "%.0f dB", mPeakDb);
            text = number;
        }

        FillText(cnv, text, Font(Weight::SemiBold).size(9.0f), mTheme.TextSecondary(),
                 el::rect{b.left - 6.0f, b.top, b.right + 6.0f, b.top + (Gauge() ? 12.0f : 10.0f)},
                 Text::Align::Centre);

        if (Gauge())
        {
            const auto& skin = mTheme.Controls();
            const float left = std::round((b.left + b.right - skin.gaugeWidth) * 0.5f);
            DrawGauge(cnv, el::rect{left, b.top + 16.0f, left + skin.gaugeWidth, b.top + 16.0f + skin.gaugeHeight});
            return;
        }

        constexpr int kSegments = kLadderSegments;
        const float level = static_cast<float>(std::clamp((mPeakDb + 60.0) / 60.0, 0.0, 1.0));
        const int lit = static_cast<int>(std::ceil(level * kSegments - 0.01f));
        // From the readout down, the lowest segment at the ladder's foot: a taller column than the
        // ladder (the compact Controls sheet's) leaves its room under it.
        const float foot = b.top + ColumnHeight(mTheme);
        const float cx = std::round((b.left + b.right) * 0.5f);

        for (int i = 0; i < kSegments; ++i)
        {
            const float bottom = foot - static_cast<float>(i) * kSegmentPitch;
            const el::rect seg{cx - kSegmentWidth * 0.5f, bottom - kSegmentHeight, cx + kSegmentWidth * 0.5f, bottom};
            el::color colour = mTheme.DarkOr(el::rgba(46, 46, 56, 255), mTheme.KnobRingTrack());

            if (i < lit)
            {
                colour = mClipped ? mTheme.Error() : (i >= kSegments - 2 ? mTheme.Warning() : mTheme.Success());
            }

            cnv.fill_style(colour);
            cnv.fill_round_rect(seg, 1.5f);
        }

        return;
    }

    cnv.fill_style(mTheme.Input());
    cnv.fill_round_rect(b, b.height() * 0.5f);

    const float level = static_cast<float>(std::clamp((mPeakDb + 60.0) / 60.0, 0.0, 1.0));

    if (level <= 0.0f)
    {
        return;
    }

    auto lit = b;
    lit.right = b.left + std::max(b.height(), b.width() * level);
    cnv.fill_style(mClipped ? mTheme.Error() : (mPeakDb > -6.0 ? mTheme.Warning() : mTheme.Success()));
    cnv.fill_round_rect(lit, b.height() * 0.5f);
}

bool MeterBar::Set(double peakDb, bool clipped, double rmsDb)
{
    // Quarter-decibel steps are invisible (a whole decibel, read as a number); skipping them
    // saves a redraw per frame.
    bool changed = std::abs(peakDb - mPeakDb) >= (mVertical ? 1.0 : 0.25) || clipped != mClipped;

    if (changed)
    {
        mPeakDb = peakDb;
        mClipped = clipped;
    }

    if (Gauge())
    {
        // The needle closes on the level by the share of the gap a VU needle covers in the
        // time since the last frame, and is redrawn once it has moved a third of a degree.
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const double dt = mNeedleAt < 0.0 ? 0.05 : std::clamp(now - mNeedleAt, 0.0, 1.0);
        mNeedleAt = now;
        const float target = GaugeDeflection(rmsDb);
        mNeedle += (target - mNeedle) * static_cast<float>(1.0 - std::exp(-dt / kGaugeTau));

        if (target == 0.0f && mNeedle < kGaugeMinStep)
        {
            mNeedle = 0.0f;
        }

        if (std::abs(mNeedle - mNeedleDrawn) >= kGaugeMinStep || (mNeedle == 0.0f && mNeedleDrawn != 0.0f))
        {
            mNeedleDrawn = mNeedle;
            changed = true;
        }

        // The peak needle rises to a new peak at once and otherwise falls at its own rate.
        mPeakDotDb = std::max(peakDb, mPeakDotDb - kGaugePeakFallDbPerS * dt);
        const float peak = GaugePeakDeflection(mPeakDotDb);

        if (std::abs(peak - mPeakDotDrawn) >= kGaugeMinStep || (peak == 0.0f && mPeakDotDrawn != 0.0f))
        {
            mPeakDotDrawn = peak;
            changed = true;
        }
    }

    return changed;
}

// ── ListRow, Surface, Backdrop ──────────────────────────────────────────────────

ListRow::ListRow(const Theme& theme, std::string text, std::string secondary, bool selected, bool heading,
                 std::function<void()> onClick)
    : mTheme(theme), mText(std::move(text)), mSecondary(std::move(secondary)), mSelected(selected), mHeading(heading),
      mOnClick(std::move(onClick))
{
}

el::view_limits ListRow::limits(const el::basic_context&) const
{
    const float h = mHeading ? 30.0f : !mNote.empty() ? std::max(44.0f, metrics::kRowHeight) : metrics::kRowHeight;
    return {{160.0f, h}, {el::full_extent, h}};
}

void ListRow::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds.inset(0.0f, 1.0f);

    if (mHeading)
    {
        FillText(cnv, mText, Font(Weight::SemiBold).size(11.0f), mTheme.TextMuted(), b.inset(12.0f, 0.0f),
                 Text::Align::Left);
        return;
    }

    if (mSelected)
    {
        cnv.fill_style(mTheme.AccentTint());
    }
    else if (mHover)
    {
        cnv.fill_style(mTheme.Elevated().opacity(0.7f));
    }
    else
    {
        cnv.fill_style(el::rgba(0, 0, 0, 0));
    }

    cnv.fill_round_rect(b, metrics::kRadius);

    float secondaryWidth = 0.0f;
    const auto secondaryFont = Font(Weight::Regular).size(metrics::kSmallText);

    if (!mSecondary.empty())
    {
        secondaryWidth = el::measure_text(cnv, mSecondary, secondaryFont).x;
        FillText(cnv, mSecondary, secondaryFont, mTheme.TextMuted(), b.inset(12.0f, 0.0f), Text::Align::Right);
    }

    const auto textFont = Font(mSelected ? Weight::SemiBold : Weight::Medium).size(metrics::kTextSize);
    auto textBounds = b.inset(12.0f, 0.0f);
    textBounds.right -= secondaryWidth + (secondaryWidth > 0.0f ? 12.0f : 0.0f);

    if (mIcons != nullptr && !mIcon.empty())
    {
        const float mid = b.top + b.height() * 0.5f;
        mIcons->Draw(cnv, mIcon, el::rect{textBounds.left, mid - 9.0f, textBounds.left + 18.0f, mid + 9.0f}, mTint);
        textBounds.left += 28.0f;
    }

    const el::color ink = mTextInk.value_or(mTheme.Text());

    if (mNote.empty())
    {
        FillText(cnv, Ellipsize(cnv, mText, textFont, textBounds.width()), textFont, ink, textBounds,
                 Text::Align::Left);
        return;
    }

    // The name over its note (.input-calibration-toolbar-item-copy): 13 over 11, 2 apart.
    const float mid = b.top + b.height() * 0.5f;
    const auto nameFont = Font(Weight::SemiBold).size(13.0f);
    const auto noteFont = Font(Weight::Regular).size(11.0f);
    FillText(cnv, Ellipsize(cnv, mText, nameFont, textBounds.width()), nameFont, ink,
             el::rect{textBounds.left, mid - 17.0f, textBounds.right, mid}, Text::Align::Left);
    FillText(cnv, Ellipsize(cnv, mNote, noteFont, textBounds.width()), noteFont, mTheme.TextMuted(),
             el::rect{textBounds.left, mid + 1.0f, textBounds.right, mid + 15.0f}, Text::Align::Left);
}

bool ListRow::key(const el::context& ctx, el::key_info k)
{
    if (IsMenuKey(k) && onMenu)
    {
        onMenu();
        return true;
    }

    return IsPressKey(k) && Activate(ctx.bounds);
}

AccessInfo ListRow::Describe() const
{
    AccessInfo info;
    info.role = AccessRole::ListItem;
    info.name = NameOr(mText);
    info.value = mNote.empty() ? mSecondary : mNote + (mSecondary.empty() ? "" : ", " + mSecondary);
    info.selected = mSelected;
    return info;
}

bool ListRow::Activate(const el::rect&)
{
    if (mHeading || !mOnClick)
    {
        return false;
    }

    const auto run = mOnClick; // it may rebuild the list this sits in
    run();
    return true;
}

ListRow& ListRow::Note(std::string note)
{
    mNote = std::move(note);
    return *this;
}

ListRow& ListRow::TextInk(el::color ink)
{
    mTextInk = ink;
    return *this;
}

ListRow& ListRow::Icon(Icons* icons, std::string name, el::color tint)
{
    mIcons = icons;
    mIcon = std::move(name);
    mTint = tint;
    return *this;
}

bool ListRow::click(const el::context& ctx, el::mouse_button btn)
{
    if (mHeading)
    {
        return false;
    }

    if (btn.state == el::mouse_button::right)
    {
        if (btn.down && onMenu)
        {
            onMenu();
        }

        return true;
    }

    if (btn.state != el::mouse_button::left)
    {
        return false;
    }

    if (btn.down)
    {
        mDown = true;
        return true;
    }

    const bool wasDown = mDown;
    mDown = false;

    if (wasDown && ctx.bounds.includes(btn.pos) && mOnClick)
    {
        mOnClick();
    }

    return true;
}

bool ListRow::cursor(const el::context& ctx, el::point, el::cursor_tracking status)
{
    const bool hover = status != el::cursor_tracking::leaving && !mHeading;

    if (hover != mHover)
    {
        mHover = hover;
        ctx.view.refresh(ctx);
    }

    return false;
}

void Surface::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    cnv.fill_style(mFill);
    cnv.fill_round_rect(ctx.bounds, mRadius);
    cnv.stroke_style(mLine);
    cnv.line_width(1.0f);
    cnv.stroke_round_rect(ctx.bounds.inset(0.5f, 0.5f), mRadius);
}

void Backdrop::draw(const el::context& ctx)
{
    ctx.canvas.fill_style(el::rgba(0, 0, 0, 140));
    ctx.canvas.fill_rect(ctx.bounds);
}

bool Backdrop::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state != el::mouse_button::left)
    {
        return false;
    }

    if (btn.down)
    {
        mDown = true;
        return true;
    }

    const bool wasDown = mDown;
    mDown = false;

    if (wasDown && mOnClick)
    {
        mOnClick();
    }

    (void)ctx;
    return true;
}

// ── Toast ───────────────────────────────────────────────────────────────────────

Toast::Toast(const Theme& theme, std::string title, std::string detail, bool isError)
    : mTheme(theme), mTitle(std::move(title)), mDetail(std::move(detail)), mError(isError)
{
}

el::view_limits Toast::limits(const el::basic_context&) const
{
    const float h = mDetail.empty() ? 48.0f : 68.0f;
    return {{480.0f, h}, {480.0f, h}};
}

void Toast::draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;

    cnv.fill_style(mTheme.Elevated());
    cnv.fill_round_rect(b, 10.0f);
    cnv.stroke_style(mError ? mTheme.Error() : mTheme.BorderStrong());
    cnv.line_width(mError ? 2.0f : 1.0f);
    cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 10.0f);

    const auto titleFont = Font(Weight::SemiBold).size(metrics::kTextSize);
    const auto detailFont = Font(Weight::Regular).size(metrics::kSmallText);
    auto inner = b.inset(16.0f, 0.0f);

    if (mDetail.empty())
    {
        FillText(cnv, Ellipsize(cnv, mTitle, titleFont, inner.width()), titleFont, mTheme.Text(), inner,
                 Text::Align::Left);
        return;
    }

    auto top = inner;
    top.bottom = b.top + b.height() * 0.5f - 1.0f;
    auto bottom = inner;
    bottom.top = b.top + b.height() * 0.5f - 1.0f;
    bottom.bottom = b.bottom - 4.0f;
    FillText(cnv, Ellipsize(cnv, mTitle, titleFont, inner.width()), titleFont, mTheme.Text(), top, Text::Align::Left);
    FillText(cnv, Ellipsize(cnv, mDetail, detailFont, inner.width()), detailFont, mTheme.TextSecondary(), bottom,
             Text::Align::Left);
}

// ── Helpers ─────────────────────────────────────────────────────────────────────

el::element_ptr Panel(el::color fill, float radius, el::element_ptr content)
{
    return Rounded(fill, radius, std::move(content));
}

el::element_ptr Gap(float width, float height)
{
    return Space(width, height);
}
} // namespace nanoq::ui
