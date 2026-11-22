// ResponsePlot.cpp - the response plot and the parametric EQ's handles (see ResponsePlot.h).

#include "ui/effects/ResponsePlot.h"

#include "ui/widgets/ControlSkin.h"
#include "ui/widgets/Graphics.h"
#include "ui/widgets/TextDraw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
namespace
{
using cycfi::artist::canvas;
using guitarfx::uiclient::ResponseDbAt;

constexpr double kMinFreq = 20.0;
constexpr double kMaxFreq = 20000.0;
constexpr float kPadding = 8.0f;
constexpr float kHandleRadius = 7.0f;
constexpr float kQHandleRadius = 5.0f;
constexpr float kHitSlop = 4.0f;
// Where the spectrum's bins sit on the plot, as eqPlot.ts draws them.
constexpr double kSpectrumFloorDb = -90.0;
constexpr double kSpectrumCeilingDb = -6.0;

// The web UI's band colours (eqCurve.ts BAND_COLORS): warm amber, green, cyan, purple.
el::color BandColour(int band, float alpha)
{
    static const el::color kColours[] = {el::rgba(232, 168, 56, 255), el::rgba(126, 207, 74, 255),
                                         el::rgba(74, 200, 207, 255), el::rgba(207, 106, 232, 255)};
    return kColours[static_cast<std::size_t>(band) % std::size(kColours)].opacity(alpha);
}

/// The octaves either side of a bell's centre at its -3 dB points.
double HalfBandwidthOctaves(double q)
{
    return std::asinh(1.0 / (2.0 * std::max(q, 1.0e-3))) / std::log(2.0);
}

/// The active theme's colours, for the plot tokens Theme has no accessor for (the tooltip's).
const guitarfx::uiclient::theme::ThemeTokens& Tokens(const Theme& theme)
{
    return *guitarfx::uiclient::theme::FindTheme(theme.Name()).tokens;
}

/// `c` through CSS's saturate(`s`) filter.
el::color Saturated(el::color c, float s)
{
    const float r = c.red, g = c.green, b = c.blue;
    const auto channel = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
    return el::color{channel((0.213f + 0.787f * s) * r + (0.715f - 0.715f * s) * g + (0.072f - 0.072f * s) * b),
                     channel((0.213f - 0.213f * s) * r + (0.715f + 0.285f * s) * g + (0.072f - 0.072f * s) * b),
                     channel((0.213f - 0.213f * s) * r + (0.715f - 0.715f * s) * g + (0.072f + 0.928f * s) * b),
                     c.alpha};
}

/// Strokes the line through `points` dashed as canvas setLineDash([on, off]) does: the pattern
/// runs along the line's length from its start.
void StrokeDashed(canvas& cnv, const std::vector<el::point>& points, float on, float off)
{
    if (points.size() < 2)
    {
        return;
    }

    bool drawing = true;
    float left = on; // of the current dash or gap
    cnv.begin_path();
    cnv.move_to(points.front());

    for (std::size_t i = 1; i < points.size(); ++i)
    {
        el::point from = points[i - 1];
        const el::point to = points[i];
        float length = std::hypot(to.x - from.x, to.y - from.y);

        while (length > 0.0f)
        {
            const float step = std::min(left, length);
            const float t = step / length;
            const el::point at{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};

            if (drawing)
            {
                cnv.line_to(at);
            }
            else
            {
                cnv.move_to(at);
            }

            from = at;
            length -= step;
            left -= step;

            if (left <= 0.0f)
            {
                drawing = !drawing;
                left = drawing ? on : off;
            }
        }
    }

    cnv.stroke();
}

void StrokeLine(canvas& cnv, const std::vector<el::point>& points)
{
    if (points.size() < 2)
    {
        return;
    }

    cnv.begin_path();
    cnv.move_to(points.front());

    for (std::size_t i = 1; i < points.size(); ++i)
    {
        cnv.line_to(points[i]);
    }

    cnv.stroke();
}
} // namespace

PlotToolButton::PlotToolButton(const Theme& theme, std::string label) : mTheme(theme), mLabel(std::move(label))
{
    // padding: 6px 12px and a 1 px border round 11 px bold text.
    mWidth = std::ceil(MeasureTextWidth(mLabel, Font(Weight::SemiBold).size(11.0f))) + 2.0f * 12.0f + 2.0f;
    mAccessName = mLabel;
}

el::view_limits PlotToolButton::limits(const el::basic_context&) const
{
    return {{mWidth, kHeight}, {mWidth, kHeight}};
}

AccessInfo PlotToolButton::Describe() const
{
    AccessInfo info;
    info.name = NameOr(mLabel);
    info.enabled = mEnabled;
    return info;
}

void PlotToolButton::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const bool lit = mEnabled && (mHover || mDown);
    const auto paint = [&] {
        cnv.fill_style(mTheme.Input());
        cnv.fill_round_rect(b, 4.0f);
        cnv.stroke_style(lit ? mTheme.Accent() : mTheme.BorderStrong());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 4.0f);
        FillText(cnv, mLabel, Font(Weight::SemiBold).size(11.0f), lit ? mTheme.Text() : mTheme.TextSecondary(), b,
                 HAlign::Centre);
    };

    // Disabled, the whole button at half strength (opacity: 0.5).
    if (mEnabled)
    {
        paint();
    }
    else
    {
        auto group = cnv.new_group(0.5f);
        paint();
    }
}

ResponsePlot::ResponsePlot(const Theme& theme) : mTheme(theme)
{
}

el::view_limits ResponsePlot::limits(const el::basic_context&) const
{
    return {{200.0f, 100.0f}, {el::full_extent, el::full_extent}};
}

namespace
{
// The card's header line and the gap under it (.eq-visualizer-header, the card's flex gap).
constexpr float kCardHeader = 12.0f;
constexpr float kCardGap = 6.0f;
} // namespace

float ResponsePlot::CardHeight(const Theme& theme, float plotHeight)
{
    return 2.0f * theme.Controls().cardPadding + kCardHeader + kCardGap + plotHeight;
}

el::rect ResponsePlot::CardHeader(const Theme& theme, const el::rect& bounds)
{
    const float pad = theme.Controls().cardPadding;
    return {bounds.left + pad, bounds.top + pad, bounds.right - pad, bounds.top + pad + kCardHeader};
}

el::rect ResponsePlot::CardWell(const Theme& theme, const el::rect& bounds)
{
    const float pad = theme.Controls().cardPadding;
    return {bounds.left + pad, bounds.top + pad + kCardHeader + kCardGap, bounds.right - pad, bounds.bottom - pad};
}

el::rect ResponsePlot::Well(const el::rect& bounds) const
{
    return mCard ? CardWell(mTheme, bounds) : bounds;
}

std::vector<el::point> SpectrumPoints(const std::vector<double>& binsDb, double minHz, double maxHz,
                                      const el::rect& plot, const std::function<float(double)>& toX)
{
    std::vector<el::point> points;

    if (binsDb.size() < 2)
    {
        return points;
    }

    const double logMin = std::log10(std::max(1.0, minHz));
    const double logSpan = std::log10(std::max(minHz + 1.0, maxHz)) - logMin;
    points.reserve(binsDb.size());

    for (std::size_t i = 0; i < binsDb.size(); ++i)
    {
        const double freq =
            std::pow(10.0, logMin + logSpan * static_cast<double>(i) / static_cast<double>(binsDb.size() - 1));
        const double level =
            std::clamp((binsDb[i] - kSpectrumFloorDb) / (kSpectrumCeilingDb - kSpectrumFloorDb), 0.0, 1.0);
        points.push_back({toX(freq), plot.bottom - static_cast<float>(level) * plot.height()});
    }

    return points;
}

void TraceSpectrum(canvas& cnv, const std::vector<el::point>& points)
{
    if (points.empty())
    {
        return;
    }

    cnv.move_to(points.front());

    for (std::size_t i = 1; i + 1 < points.size(); ++i)
    {
        const auto& next = points[i + 1];
        cnv.quadratic_curve_to(points[i], el::point{(points[i].x + next.x) / 2.0f, (points[i].y + next.y) / 2.0f});
    }

    cnv.line_to(points.back());
}

el::rect ResponsePlot::Plot(const el::rect& bounds) const
{
    const auto well = Well(bounds);
    return {well.left + kPadding, well.top + kPadding, well.right - kPadding, well.bottom - kPadding};
}

float ResponsePlot::FreqToX(const el::rect& plot, double freq) const
{
    const double logFreq = std::log10(std::clamp(freq, 1.0, 1.0e6));

    if (mAxis.size() >= 2)
    {
        // Between the anchors either side, or carried past the ends at the nearest pair's rate.
        std::size_t i = 0;

        while (i + 2 < mAxis.size() && logFreq > mAxis[i + 1].first)
        {
            ++i;
        }

        const auto& [aLog, aX] = mAxis[i];
        const auto& [bLog, bX] = mAxis[i + 1];
        return aX + static_cast<float>((logFreq - aLog) * (bX - aX) / (bLog - aLog));
    }

    const double t = (logFreq - std::log10(kMinFreq)) / std::log10(kMaxFreq / kMinFreq);
    return plot.left + static_cast<float>(t) * plot.width();
}

double ResponsePlot::XToFreq(const el::rect& plot, float x) const
{
    if (mAxis.size() >= 2)
    {
        std::size_t i = 0;

        while (i + 2 < mAxis.size() && x > mAxis[i + 1].second)
        {
            ++i;
        }

        const auto& [aLog, aX] = mAxis[i];
        const auto& [bLog, bX] = mAxis[i + 1];
        return std::pow(10.0, aLog + (x - aX) * (bLog - aLog) / (bX - aX));
    }

    const double t = (x - plot.left) / std::max(1.0f, plot.width());
    return kMinFreq * std::pow(kMaxFreq / kMinFreq, t);
}

el::color ResponsePlot::Tone(el::color c) const
{
    return mSaturation < 1.0f ? Saturated(c, mSaturation) : c;
}

std::vector<el::point> ResponsePlot::Trace(const el::rect& plot, const std::function<double(double freq)>& dbAt) const
{
    // A pixel at a time across the plot; kept inside the audio band, since an anchored axis can
    // run past 20 Hz-20 kHz at the edges.
    std::vector<el::point> points;
    const int width = static_cast<int>(std::floor(plot.width()));
    points.reserve(static_cast<std::size_t>(std::max(0, width)) + 1);

    for (int i = 0; i <= width; ++i)
    {
        const float x = plot.left + static_cast<float>(i);
        const double db = dbAt(std::clamp(XToFreq(plot, x), 10.0, 21000.0));

        if (std::isfinite(db))
        {
            points.push_back({x, DbToY(plot, db)});
        }
    }

    return points;
}

float ResponsePlot::DbToY(const el::rect& plot, double db) const
{
    const double clamped = std::clamp(db, mMinDb, mMaxDb);
    return plot.top + static_cast<float>((mMaxDb - clamped) / (mMaxDb - mMinDb)) * plot.height();
}

double ResponsePlot::YToDb(const el::rect& plot, float y) const
{
    return mMaxDb - (y - plot.top) / std::max(1.0f, plot.height()) * (mMaxDb - mMinDb);
}

bool ResponsePlot::SetSpectrum(const guitarfx::uiclient::NodeSpectrum* spectrum)
{
    if (spectrum == nullptr || spectrum->binsDb.size() < 2)
    {
        const bool had = mSpectrum.has_value();
        mSpectrum.reset();
        return had;
    }

    if (mSpectrum && mSpectrum->receivedAt == spectrum->receivedAt && mSpectrum->binsDb == spectrum->binsDb)
    {
        return false;
    }

    mSpectrum = *spectrum;
    return true;
}

void ResponsePlot::SetBands(std::vector<PlotBand> bands)
{
    if (mDragBand >= 0 && bands.size() == mBands.size())
    {
        return;
    }

    mBands = std::move(bands);

    if (mDragBand >= static_cast<int>(mBands.size()))
    {
        mDragBand = -1;
    }
}

void ResponsePlot::draw(const el::context& ctx)
{
    NoteDrawn(ctx);

    // Behind other controls, the whole plot is drawn faint (opacity), as one layer.
    if (mOpacity < 1.0f)
    {
        auto group = ctx.canvas.new_group(mOpacity);
        DrawContent(ctx);
    }
    else
    {
        DrawContent(ctx);
    }
}

void ResponsePlot::DrawContent(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const auto plot = Plot(b);

    // The axis pinned to the anchors as they sit now (a graphic EQ's sliders), when they rise in
    // both frequency and x (anchoredFrequencyAxis).
    mAxis.clear();

    if (mAnchorsFor)
    {
        for (const auto& anchor : mAnchorsFor(b))
        {
            if (std::isfinite(anchor.freq) && anchor.freq > 0.0 && std::isfinite(anchor.x))
            {
                mAxis.emplace_back(std::log10(anchor.freq), anchor.x);
            }
        }

        std::sort(mAxis.begin(), mAxis.end(), [](const auto& a, const auto& c) { return a.second < c.second; });
        bool rising = mAxis.size() >= 2;

        for (std::size_t i = 1; rising && i < mAxis.size(); ++i)
        {
            rising = mAxis[i].second > mAxis[i - 1].second && mAxis[i].first > mAxis[i - 1].first;
        }

        if (!rising)
        {
            mAxis.clear();
        }
    }

    // The card, its header, and the well the plot sits in, as the theme draws them; without a
    // card (the EQ dialog's plot), the well alone; behind other controls, neither.
    const auto& skin = mTheme.Controls();

    if (mCard)
    {
        DrawControlBox(cnv, b.inset(0.5f, 0.5f), skin.card);
        DrawControlText(cnv, mCardTitle, skin.cardTitle, CardHeader(mTheme, b), HAlign::Left);
        DrawControlText(cnv, mCardRange, skin.cardRange, CardHeader(mTheme, b), HAlign::Right);
        DrawControlBox(cnv, Well(b), skin.plot);
    }
    else if (!mBehind)
    {
        DrawControlBox(cnv, b, skin.plot);
    }

    // The grid: a line every grid step in level, and at the usual frequencies from 20 Hz to
    // 20 kHz where they fall on the plot (the ends kept a pixel inside it).
    cnv.stroke_style(Tone(mTheme.PlotGrid()));
    cnv.line_width(1.0f);

    for (double db = std::ceil(mMinDb / mGridStepDb) * mGridStepDb; db <= mMaxDb + 1e-9; db += mGridStepDb)
    {
        const float y = std::clamp(std::round(DbToY(plot, db)) + 0.5f, plot.top + 0.5f, plot.bottom - 0.5f);
        cnv.begin_path();
        cnv.move_to(plot.left, y);
        cnv.line_to(plot.right, y);
        cnv.stroke();
    }

    for (const double freq : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0})
    {
        const float exact = FreqToX(plot, freq);

        if (exact < plot.left - 0.01f || exact > plot.right + 0.01f)
        {
            continue;
        }

        const float x = std::clamp(std::round(exact) + 0.5f, plot.left + 0.5f, plot.right - 0.5f);
        cnv.begin_path();
        cnv.move_to(x, plot.top);
        cnv.line_to(x, plot.bottom);
        cnv.stroke();
    }

    {
        auto state = cnv.new_state();
        cnv.add_rect(plot);
        cnv.clip();
        DrawSpectrum(cnv, plot);
    }

    cnv.line_join(canvas::round_join);

    // The reference, thin and dashed under the curve (4 drawn, 3 left out).
    if (mReference && mReference->frequencies.size() >= 2)
    {
        cnv.stroke_style(Tone(mTheme.PlotSpectrumLine()));
        cnv.line_width(1.0f);
        StrokeDashed(cnv, Trace(plot, [this](double freq) { return ResponseDbAt(*mReference, freq); }), 4.0f, 3.0f);
    }

    if (mCurve && mCurve->frequencies.size() >= 2)
    {
        cnv.stroke_style(Tone(mTheme.PlotCurve()));
        cnv.line_width(2.0f);
        StrokeLine(cnv, Trace(plot, [this](double freq) { return ResponseDbAt(*mCurve, freq); }));
    }

    // Each band's own part of the curve over it, then the handles and the tooltip.
    DrawBands(cnv, plot);
    DrawHandles(cnv, plot);
    DrawTooltip(cnv, Well(b), plot);
}

void ResponsePlot::DrawSpectrum(canvas& cnv, const el::rect& plot) const
{
    if (!mSpectrum)
    {
        return;
    }

    const auto points = SpectrumPoints(mSpectrum->binsDb, mSpectrum->minHz, mSpectrum->maxHz, plot,
                                       [this, &plot](double freq) { return FreqToX(plot, freq); });

    if (points.size() < 2)
    {
        return;
    }

    canvas::linear_gradient fill{el::point{plot.left, plot.top}, el::point{plot.left, plot.bottom}};
    fill.add_color_stop(0.0f, Tone(mTheme.PlotSpectrumTop()));
    fill.add_color_stop(1.0f, Tone(mTheme.PlotSpectrumBottom()));
    cnv.fill_style(fill);
    cnv.begin_path();
    TraceSpectrum(cnv, points);
    cnv.line_to(points.back().x, plot.bottom);
    cnv.line_to(points.front().x, plot.bottom);
    cnv.close_path();
    cnv.fill();

    cnv.stroke_style(Tone(mTheme.PlotSpectrumLine()));
    cnv.line_width(1.0f);
    cnv.begin_path();
    TraceSpectrum(cnv, points);
    cnv.stroke();
}

void ResponsePlot::DrawBands(canvas& cnv, const el::rect& plot) const
{
    if (!mCurve || mBands.empty() || mCurve->bandsDb.size() != mBands.size())
    {
        return;
    }

    const float zeroY = DbToY(plot, 0.0);

    for (std::size_t i = 0; i < mBands.size(); ++i)
    {
        if (std::abs(mBands[i].gainDb) < 0.05)
        {
            continue;
        }

        // Every pixel across, as the web UI samples them (drawBandOverlays).
        const auto& db = mCurve->bandsDb[i];
        const auto points = Trace(plot, [this, &db](double freq) { return ResponseDbAt(mCurve->frequencies, db, freq); });

        if (points.size() < 2)
        {
            continue;
        }

        cnv.fill_style(BandColour(static_cast<int>(i), 0.06f));
        cnv.begin_path();
        cnv.move_to(points.front().x, zeroY);

        for (const auto& point : points)
        {
            cnv.line_to(point);
        }

        cnv.line_to(points.back().x, zeroY);
        cnv.close_path();
        cnv.fill();
        cnv.stroke_style(BandColour(static_cast<int>(i), 0.2f));
        cnv.line_width(1.0f);
        StrokeLine(cnv, points);
    }
}

std::vector<ResponsePlot::Handle> ResponsePlot::Handles(const el::rect& plot) const
{
    // Each band's main handle, then its Q wings when it has a Q (computeHandles).
    std::vector<Handle> handles;

    for (std::size_t i = 0; i < mBands.size(); ++i)
    {
        const auto& band = mBands[i];
        const int index = static_cast<int>(i);
        const float y = DbToY(plot, band.gainDb);
        handles.push_back({index, Grip::Main, {FreqToX(plot, band.freq), y}, kHandleRadius});

        if (band.hasQ && band.q > 0.0)
        {
            const double half = HalfBandwidthOctaves(band.q);
            handles.push_back({index, Grip::QLeft,
                               {FreqToX(plot, std::max(kMinFreq, band.freq / std::pow(2.0, half))), y},
                               kQHandleRadius});
            handles.push_back({index, Grip::QRight,
                               {FreqToX(plot, std::min(kMaxFreq, band.freq * std::pow(2.0, half))), y},
                               kQHandleRadius});
        }
    }

    return handles;
}

void ResponsePlot::DrawHandles(canvas& cnv, const el::rect& plot) const
{
    const auto handles = Handles(plot);

    // The Q wings' connectors first, behind the handles: dashed, in the band's colour.
    for (const auto& handle : handles)
    {
        if (handle.grip == Grip::Main)
        {
            continue;
        }

        const auto main = std::find_if(handles.begin(), handles.end(), [&handle](const Handle& h) {
            return h.band == handle.band && h.grip == Grip::Main;
        });

        if (main != handles.end())
        {
            cnv.stroke_style(BandColour(handle.band, 0.3f));
            cnv.line_width(1.5f);
            StrokeDashed(cnv, {main->at, handle.at}, 3.0f, 3.0f);
        }
    }

    // Each handle: only the one pointed at or dragged grows and glows.
    for (const auto& handle : handles)
    {
        const bool hovered = mHover.band == handle.band && mHover.grip == handle.grip;
        const bool dragged = mDragBand == handle.band && mDragGrip == handle.grip;
        const float radius = hovered || dragged ? handle.radius + 2.0f : handle.radius;
        auto state = cnv.new_state();

        if (dragged || hovered)
        {
            cnv.shadow_style(dragged ? 12.0f : 6.0f, BandColour(handle.band, 0.9f));
        }

        if (handle.grip == Grip::Main)
        {
            cnv.add_circle(cycfi::artist::circle(handle.at, radius));
            cnv.fill_style(BandColour(handle.band, 0.9f));
            cnv.fill_preserve();
            cnv.stroke_style(mTheme.PlotHandleStroke());
            cnv.line_width(dragged ? 2.0f : 1.5f);
            cnv.stroke();
        }
        else
        {
            // A semi-translucent square for a Q wing.
            const el::rect square{handle.at.x - radius, handle.at.y - radius, handle.at.x + radius,
                                  handle.at.y + radius};
            cnv.fill_style(BandColour(handle.band, 0.35f));
            cnv.fill_rect(square);
            cnv.stroke_style(mTheme.PlotHandleStroke());
            cnv.line_width(dragged ? 2.0f : 1.5f);
            cnv.stroke_rect(square);
        }
    }
}

void ResponsePlot::DrawTooltip(canvas& cnv, const el::rect& bounds, const el::rect& plot) const
{
    // The handle being dragged, else the one pointed at, with its values over it.
    const Hit shown = mDragBand >= 0 ? Hit{mDragBand, mDragGrip} : mHover;

    if (shown.band < 0 || shown.band >= static_cast<int>(mBands.size()))
    {
        return;
    }

    const auto handles = Handles(plot);
    const auto handle = std::find_if(handles.begin(), handles.end(), [&shown](const Handle& h) {
        return h.band == shown.band && h.grip == shown.grip;
    });

    if (handle == handles.end())
    {
        return;
    }

    const auto& band = mBands[static_cast<std::size_t>(shown.band)];
    char text[128];

    if (shown.grip == Grip::Main)
    {
        std::snprintf(text, sizeof(text), "%s: %.0f Hz  %s%.1f dB", band.label.c_str(), std::round(band.freq),
                      band.gainDb >= 0.0 ? "+" : "", band.gainDb);
    }
    else
    {
        std::snprintf(text, sizeof(text), "Q: %.1f", band.q);
    }

    // 18 px high, 5 either side of the text, its baseline 10 over the handle and never above 20.
    const auto font = Font(Weight::Regular).size(11.0f);
    const float width = MeasureTextWidth(text, font) + 2.0f * 5.0f;
    const float baseline = std::max(bounds.top + 20.0f, handle->at.y - handle->radius - 10.0f);
    const el::rect box{handle->at.x - width / 2.0f, baseline - 12.0f, handle->at.x + width / 2.0f, baseline + 6.0f};
    const auto& tokens = Tokens(mTheme);
    cnv.fill_style(Theme::Argb(tokens.eqCurveTooltipBg));
    cnv.fill_round_rect(box, 4.0f);
    FillText(cnv, text, font, Theme::Argb(tokens.eqCurveTooltipText), box, HAlign::Centre);
}

ResponsePlot::Hit ResponsePlot::HitTest(const el::rect& bounds, el::point p) const
{
    // The nearest handle within its radius and a little more.
    Hit best;
    float bestDistance = 1.0e9f;

    for (const auto& handle : Handles(Plot(bounds)))
    {
        const float d = std::hypot(p.x - handle.at.x, p.y - handle.at.y);

        if (d <= handle.radius + kHitSlop && d < bestDistance)
        {
            bestDistance = d;
            best = {handle.band, handle.grip};
        }
    }

    return best;
}

void ResponsePlot::ShowCursor(bool overHandle)
{
    if (overHandle)
    {
        el::set_cursor(el::cursor_type::hand);
    }
    else if (mGrabCursor)
    {
        el::set_cursor(el::cursor_type::arrow);
    }

    mGrabCursor = overHandle;
}

void ResponsePlot::Report(int band, bool commit)
{
    if (!onBandChange || band < 0 || band >= static_cast<int>(mBands.size()))
    {
        return;
    }

    const auto& b = mBands[static_cast<std::size_t>(band)];
    onBandChange(band, b.freq, b.gainDb, b.q, commit);
}

bool ResponsePlot::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state != el::mouse_button::left || mBands.empty())
    {
        return false;
    }

    if (btn.down)
    {
        const auto hit = HitTest(ctx.bounds, btn.pos);

        if (hit.band < 0)
        {
            return false;
        }

        if (btn.num_clicks == 2 && hit.grip == Grip::Main)
        {
            mDragBand = -1;

            if (onBandReset)
            {
                onBandReset(hit.band);
            }

            return true;
        }

        mDragBand = hit.band;
        mDragGrip = hit.grip;
        ShowCursor(true);
        ctx.view.refresh(ctx);
        return true;
    }

    if (mDragBand >= 0)
    {
        const int band = mDragBand;
        mDragBand = -1;
        mDragGrip = Grip::None;
        // Hovering again whatever is under the pointer where the drag let go.
        mHover = HitTest(ctx.bounds, btn.pos);
        ShowCursor(mHover.band >= 0);
        Report(band, true);
        ctx.view.refresh(ctx);
    }

    return true;
}

void ResponsePlot::drag(const el::context& ctx, el::mouse_button btn)
{
    if (mDragBand < 0 || mDragBand >= static_cast<int>(mBands.size()))
    {
        return;
    }

    const auto plot = Plot(ctx.bounds);
    auto& band = mBands[static_cast<std::size_t>(mDragBand)];

    if (mDragGrip == Grip::Main)
    {
        // Frequency to the hertz; gain to a tenth of a dB, and to 0 within 0.3 of it.
        band.freq = std::round(std::clamp(XToFreq(plot, btn.pos.x), band.freqMin, band.freqMax));
        double gain = std::clamp(YToDb(plot, btn.pos.y), band.gainMin, band.gainMax);

        if (std::abs(gain) < 0.3)
        {
            gain = 0.0;
        }

        band.gainDb = std::round(gain * 10.0) / 10.0;
    }
    else
    {
        // A wing sets the bandwidth: its distance from the centre is half of it, in octaves, and
        // never under a 1.05 ratio. Q to a tenth.
        const double dragged = XToFreq(plot, btn.pos.x);
        const double ratio = mDragGrip == Grip::QRight ? std::max(1.05, dragged / band.freq)
                                                       : std::max(1.05, band.freq / std::max(1.0, dragged));
        const double bandwidth = 2.0 * std::log2(ratio);
        const double q =
            bandwidth > 0.001 ? 1.0 / (2.0 * std::sinh(bandwidth * std::log(2.0) / 2.0)) : band.qMax;
        band.q = std::round(std::clamp(q, band.qMin, band.qMax) * 10.0) / 10.0;
    }

    ShowCursor(true);
    Report(mDragBand, false);
    ctx.view.refresh(ctx);
}

bool ResponsePlot::cursor(const el::context& ctx, el::point p, el::cursor_tracking status)
{
    // Hover follows the pointer only while nothing is being dragged, as the web UI's does.
    if (mDragBand >= 0)
    {
        ShowCursor(true);
        return true;
    }

    const Hit hover = status == el::cursor_tracking::leaving ? Hit{} : HitTest(ctx.bounds, p);

    if (hover.band != mHover.band || hover.grip != mHover.grip)
    {
        mHover = hover;
        ctx.view.refresh(ctx);
    }

    ShowCursor(hover.band >= 0);
    return hover.band >= 0;
}
} // namespace nanoq::ui
