// ResponsePlot.cpp - the response plot and the parametric EQ's handles (see ResponsePlot.h).

#include "ui/effects/ResponsePlot.h"

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

std::string FormatFreq(double hz)
{
    char text[32];

    if (hz >= 1000.0)
    {
        std::snprintf(text, sizeof(text), "%.2fk Hz", hz / 1000.0);
    }
    else
    {
        std::snprintf(text, sizeof(text), "%.0f Hz", hz);
    }

    return text;
}
} // namespace

ResponsePlot::ResponsePlot(const Theme& theme) : mTheme(theme)
{
}

el::view_limits ResponsePlot::limits(const el::basic_context&) const
{
    return {{200.0f, 100.0f}, {el::full_extent, el::full_extent}};
}

el::rect ResponsePlot::Plot(const el::rect& bounds) const
{
    return {bounds.left + kPadding, bounds.top + kPadding, bounds.right - kPadding, bounds.bottom - kPadding};
}

float ResponsePlot::FreqToX(const el::rect& plot, double freq) const
{
    const double t = std::log10(std::clamp(freq, 1.0, 1.0e6) / kMinFreq) / std::log10(kMaxFreq / kMinFreq);
    return plot.left + static_cast<float>(t) * plot.width();
}

double ResponsePlot::XToFreq(const el::rect& plot, float x) const
{
    const double t = (x - plot.left) / std::max(1.0f, plot.width());
    return kMinFreq * std::pow(kMaxFreq / kMinFreq, t);
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
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const auto plot = Plot(b);

    // The well the plot sits in.
    cnv.fill_style(mTheme.Pick(el::rgba(0, 0, 0, 70), el::rgba(255, 255, 255, 120)));
    cnv.add_round_rect(b, 6.0f);
    cnv.fill();

    // The grid: a line every grid step in level, and at the usual frequencies.
    cnv.stroke_style(mTheme.PlotGrid());
    cnv.line_width(1.0f);

    for (double db = std::ceil(mMinDb / mGridStepDb) * mGridStepDb; db <= mMaxDb + 1e-9; db += mGridStepDb)
    {
        const float y = std::round(DbToY(plot, db)) + 0.5f;
        cnv.begin_path();
        cnv.move_to(plot.left, y);
        cnv.line_to(plot.right, y);
        cnv.stroke();
    }

    for (const double freq : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0})
    {
        const float x = std::round(FreqToX(plot, freq)) + 0.5f;
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
        DrawBands(cnv, plot);

        // The reference, dashed: 6 px drawn, 4 px left out.
        if (mReference && mReference->frequencies.size() >= 2)
        {
            cnv.stroke_style(mTheme.TextMuted().opacity(0.8f));
            cnv.line_width(1.25f);

            for (float start = plot.left; start < plot.right; start += 10.0f)
            {
                cnv.begin_path();

                for (float x = start; x <= std::min(start + 6.0f, plot.right); x += 1.0f)
                {
                    const float y = DbToY(plot, ResponseDbAt(*mReference, XToFreq(plot, x)));

                    if (x == start)
                    {
                        cnv.move_to(x, y);
                    }
                    else
                    {
                        cnv.line_to(x, y);
                    }
                }

                cnv.stroke();
            }
        }

        if (mCurve && mCurve->frequencies.size() >= 2)
        {
            cnv.stroke_style(mTheme.PlotCurve());
            cnv.line_width(2.0f);
            cnv.line_join(canvas::round_join);
            cnv.begin_path();

            for (float x = plot.left; x <= plot.right; x += 1.0f)
            {
                const float y = DbToY(plot, ResponseDbAt(*mCurve, XToFreq(plot, x)));

                if (x == plot.left)
                {
                    cnv.move_to(x, y);
                }
                else
                {
                    cnv.line_to(x, y);
                }
            }

            cnv.stroke();
        }
    }

    DrawHandles(cnv, plot);

    // The band being moved or pointed at, named with its values, as the web UI's tooltip.
    const int shown = mDragBand >= 0 ? mDragBand : mHoverBand;

    if (shown >= 0 && shown < static_cast<int>(mBands.size()))
    {
        const auto& band = mBands[static_cast<std::size_t>(shown)];
        char text[96];
        std::snprintf(text, sizeof(text), "%s  %s  %+.1f dB%s", band.label.c_str(), FormatFreq(band.freq).c_str(),
                      band.gainDb, band.hasQ ? ("  Q " + std::to_string(band.q).substr(0, 4)).c_str() : "");
        FillText(cnv, text, Font(Weight::SemiBold).size(10.0f), mTheme.Ink(),
                 el::rect{plot.left + 6.0f, plot.top + 2.0f, plot.right, plot.top + 16.0f}, HAlign::Left);
    }
}

void ResponsePlot::DrawSpectrum(canvas& cnv, const el::rect& plot) const
{
    if (!mSpectrum)
    {
        return;
    }

    const auto& bins = mSpectrum->binsDb;
    const double logMin = std::log10(std::max(1.0, mSpectrum->minHz));
    const double logSpan = std::log10(std::max(mSpectrum->minHz + 1.0, mSpectrum->maxHz)) - logMin;
    const auto point = [&](std::size_t i) {
        const double freq =
            std::pow(10.0, logMin + logSpan * static_cast<double>(i) / static_cast<double>(bins.size() - 1));
        const double level =
            std::clamp((bins[i] - kSpectrumFloorDb) / (kSpectrumCeilingDb - kSpectrumFloorDb), 0.0, 1.0);
        return el::point{FreqToX(plot, freq), plot.bottom - static_cast<float>(level) * plot.height()};
    };

    canvas::linear_gradient fill{el::point{plot.left, plot.top}, el::point{plot.left, plot.bottom}};
    fill.add_color_stop(0.0f, mTheme.PlotSpectrumTop());
    fill.add_color_stop(1.0f, mTheme.PlotSpectrumBottom());
    cnv.fill_style(fill);
    cnv.begin_path();
    cnv.move_to(point(0).x, plot.bottom);

    for (std::size_t i = 0; i < bins.size(); ++i)
    {
        cnv.line_to(point(i));
    }

    cnv.line_to(point(bins.size() - 1).x, plot.bottom);
    cnv.close_path();
    cnv.fill();

    cnv.stroke_style(mTheme.PlotSpectrumLine());
    cnv.line_width(1.0f);
    cnv.begin_path();

    for (std::size_t i = 0; i < bins.size(); ++i)
    {
        if (i == 0)
        {
            cnv.move_to(point(i));
        }
        else
        {
            cnv.line_to(point(i));
        }
    }

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

        const auto& db = mCurve->bandsDb[i];
        const auto trace = [&](bool fill) {
            cnv.begin_path();

            if (fill)
            {
                cnv.move_to(plot.left, zeroY);
            }

            for (float x = plot.left; x <= plot.right; x += 2.0f)
            {
                const float y = DbToY(plot, ResponseDbAt(mCurve->frequencies, db, XToFreq(plot, x)));

                if (!fill && x == plot.left)
                {
                    cnv.move_to(x, y);
                }
                else
                {
                    cnv.line_to(x, y);
                }
            }

            if (fill)
            {
                cnv.line_to(plot.right, zeroY);
                cnv.close_path();
            }
        };
        cnv.fill_style(BandColour(static_cast<int>(i), 0.06f));
        trace(true);
        cnv.fill();
        cnv.stroke_style(BandColour(static_cast<int>(i), 0.2f));
        cnv.line_width(1.0f);
        trace(false);
        cnv.stroke();
    }
}

void ResponsePlot::DrawHandles(canvas& cnv, const el::rect& plot) const
{
    for (std::size_t i = 0; i < mBands.size(); ++i)
    {
        const auto& band = mBands[i];
        const int index = static_cast<int>(i);
        const bool active = index == mDragBand || index == mHoverBand;
        const float x = FreqToX(plot, band.freq);
        const float y = DbToY(plot, band.gainDb);

        if (band.hasQ)
        {
            const double half = HalfBandwidthOctaves(band.q);

            for (const double side : {-1.0, 1.0})
            {
                const float wx = FreqToX(plot, std::clamp(band.freq * std::pow(2.0, side * half), kMinFreq, kMaxFreq));
                cnv.stroke_style(BandColour(index, 0.45f));
                cnv.line_width(1.0f);
                cnv.begin_path();
                cnv.move_to(x, y);
                cnv.line_to(wx, y);
                cnv.stroke();
                // A small square, as the web UI draws a band's width handles (eqCurve.ts).
                const float r = kQHandleRadius - 0.5f;
                const el::rect square{wx - r, y - r, wx + r, y + r};
                cnv.fill_style(BandColour(index, active ? 0.55f : 0.3f));
                cnv.fill_rect(square);
                cnv.stroke_style(BandColour(index, active ? 1.0f : 0.85f));
                cnv.line_width(1.5f);
                cnv.stroke_rect(square);
            }
        }

        cnv.fill_style(BandColour(index, 0.9f));
        cnv.add_circle(cycfi::artist::circle(el::point{x, y}, active ? kHandleRadius + 1.0f : kHandleRadius));
        cnv.fill();
        cnv.stroke_style(mTheme.PlotHandleStroke());
        cnv.line_width(active ? 2.0f : 1.5f);
        cnv.add_circle(cycfi::artist::circle(el::point{x, y}, active ? kHandleRadius + 1.0f : kHandleRadius));
        cnv.stroke();
    }
}

ResponsePlot::Hit ResponsePlot::HitTest(const el::rect& bounds, el::point p) const
{
    const auto plot = Plot(bounds);
    Hit best;
    float bestDistance = 1.0e9f;

    for (std::size_t i = 0; i < mBands.size(); ++i)
    {
        const auto& band = mBands[i];
        const float x = FreqToX(plot, band.freq);
        const float y = DbToY(plot, band.gainDb);
        const auto consider = [&](float cx, float radius, Grip grip) {
            const float d = std::hypot(p.x - cx, p.y - y);

            if (d <= radius + kHitSlop && d < bestDistance)
            {
                bestDistance = d;
                best = {static_cast<int>(i), grip};
            }
        };
        consider(x, kHandleRadius, Grip::Main);

        if (band.hasQ)
        {
            const double half = HalfBandwidthOctaves(band.q);
            consider(FreqToX(plot, std::clamp(band.freq / std::pow(2.0, half), kMinFreq, kMaxFreq)), kQHandleRadius,
                     Grip::QLeft);
            consider(FreqToX(plot, std::clamp(band.freq * std::pow(2.0, half), kMinFreq, kMaxFreq)), kQHandleRadius,
                     Grip::QRight);
        }
    }

    return best;
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
        ctx.view.refresh(ctx);
        return true;
    }

    if (mDragBand >= 0)
    {
        const int band = mDragBand;
        mDragBand = -1;
        mDragGrip = Grip::None;
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
        band.freq = std::clamp(XToFreq(plot, btn.pos.x), band.freqMin, band.freqMax);
        band.gainDb = std::clamp(YToDb(plot, btn.pos.y), band.gainMin, band.gainMax);
    }
    else
    {
        // A wing sets the bandwidth: its distance from the centre is half of it, in octaves.
        const double half = std::max(0.02, std::abs(std::log2(XToFreq(plot, btn.pos.x) / band.freq)));
        band.q = std::clamp(1.0 / (2.0 * std::sinh(std::log(2.0) * half)), band.qMin, band.qMax);
    }

    Report(mDragBand, false);
    ctx.view.refresh(ctx);
}

bool ResponsePlot::cursor(const el::context& ctx, el::point p, el::cursor_tracking status)
{
    const int hover = status == el::cursor_tracking::leaving ? -1 : HitTest(ctx.bounds, p).band;

    if (hover != mHoverBand)
    {
        mHoverBand = hover;
        ctx.view.refresh(ctx);
    }

    return hover >= 0;
}

bool ResponsePlot::scroll(const el::context& ctx, el::point dir, el::point p)
{
    // The wheel over a band narrows or widens it, as the web UI's curve does.
    const auto hit = HitTest(ctx.bounds, p);

    if (dir.y == 0.0f || hit.band < 0)
    {
        return false;
    }

    auto& band = mBands[static_cast<std::size_t>(hit.band)];

    if (!band.hasQ)
    {
        return false;
    }

    band.q = std::clamp(band.q * (dir.y > 0.0f ? 1.1 : 1.0 / 1.1), band.qMin, band.qMax);
    Report(hit.band, true);
    ctx.view.refresh(ctx);
    return true;
}
} // namespace nanoq::ui
