// MultibandPlot.cpp - the multi-band effects' plot (see MultibandPlot.h). The geometry, the
// colours (css/multiband.css) and the crossover maths are the web UI's, value for value.

#include "ui/effects/MultibandPlot.h"

#include "dsp/EffectGuids.h"
#include "ui/effects/ResponsePlot.h"
#include "ui/widgets/ControlSkin.h"
#include "ui/widgets/TextDraw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
namespace
{
using cycfi::artist::canvas;
namespace guids = guitarfx::EffectGuids;

constexpr double kMinFreq = 20.0;
constexpr double kMaxFreq = 20000.0;
constexpr float kPadding = 8.0f;
constexpr float kStripHeight = 16.0f;
constexpr float kStripGap = 6.0f;
constexpr float kGrabDistance = 6.0f;          // MB_GRAB_DISTANCE_PX
constexpr double kDepthScaleDb = 24.0;         // MB_DEPTH_SCALE_DB
constexpr double kLevelMinDb = -60.0;          // the level strip's range, dBFS
constexpr double kLevelMaxDb = 0.0;
constexpr double kDepthFallDbPerSecond = 30.0; // REDUCTION_FALL_DB_PER_SECOND
const double kMinSplitRatio = std::cbrt(2.0);  // kMinSplitRatio in LinkwitzRileyCrossover.h

const char* const kBandLabels[] = {"Low", "Low Mid", "High Mid", "High"};
const char* const kCrossoverKeys[] = {"crossover1", "crossover2", "crossover3"};

/// The crossovers' ranges and defaults, as MultibandCompressorSupport.h and
/// MultibandDistortionSupport.h both declare them.
struct Range
{
    double min, max, fallback;
};
constexpr Range kCrossoverRanges[] = {{40.0, 1000.0, 200.0}, {150.0, 5000.0, 800.0}, {600.0, 12000.0, 3000.0}};

constexpr double kDefaultThresholdDb = -24.0;      // the compressor's Threshold default
constexpr double kCompressorDefaultKneeDb = 6.0;   // its Knee default
constexpr double kDefaultNominalDbfs = -18.0;      // LevelTargets.h, until the engine says otherwise
constexpr double kDistortionKneeAboveNominal = 12; // kKneeAboveNominalDb
constexpr double kDistortionDefaultDriveDb[] = {18.0, 28.0, 36.0, 30.0};

el::color Rgba(int r, int g, int b, float a)
{
    return el::color{static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f,
                     a};
}

el::color Dimmed(el::color c, float by)
{
    c.alpha *= by;
    return c;
}

/// The --mb-* tokens, dark and .theme-light, the distortion's drive in red.
struct Colours
{
    el::color depthFill, depthEdge, crossover, label, level, levelOver, stripBg, knee, unheard, solo;
};

Colours ColoursFor(const Theme& theme, MultibandPlot::Kind kind)
{
    const bool light = theme.IsLight();
    Colours c = light ? Colours{Rgba(181, 82, 30, 0.22f), Rgba(181, 82, 30, 0.95f), Rgba(17, 19, 24, 0.35f),
                                Rgba(17, 19, 24, 0.72f), Rgba(58, 74, 96, 0.42f), Rgba(181, 82, 30, 0.85f),
                                Rgba(17, 19, 24, 0.08f), Rgba(30, 110, 170, 0.22f), Rgba(232, 236, 242, 0.65f),
                                Rgba(150, 100, 0, 0.95f)}
                      : Colours{Rgba(232, 128, 72, 0.3f), Rgba(240, 150, 90, 0.95f), Rgba(255, 255, 255, 0.38f),
                                Rgba(240, 240, 248, 0.72f), Rgba(240, 240, 248, 0.42f), Rgba(240, 150, 90, 0.85f),
                                Rgba(0, 0, 0, 0.28f), Rgba(72, 168, 224, 0.3f), Rgba(0, 0, 0, 0.5f),
                                Rgba(240, 200, 80, 0.95f)};

    if (kind == MultibandPlot::Kind::Distortion)
    {
        c.depthFill = light ? Rgba(176, 36, 36, 0.2f) : Rgba(226, 72, 72, 0.28f);
        c.depthEdge = light ? Rgba(176, 36, 36, 0.95f) : Rgba(240, 100, 100, 0.95f);
        c.levelOver = light ? Rgba(176, 36, 36, 0.85f) : Rgba(240, 100, 100, 0.85f);
    }

    return c;
}

double Param(const std::map<std::string, double>& params, const std::string& key, double fallback)
{
    const auto it = params.find(key);
    return it != params.end() && std::isfinite(it->second) ? it->second : fallback;
}

bool Flag(const std::map<std::string, double>& params, const std::string& key, bool fallback)
{
    const auto it = params.find(key);
    return it != params.end() && std::isfinite(it->second) ? it->second >= 0.5 : fallback;
}

/// Where crossover `index` lands dragged to `freq`: inside its own range and a third of an
/// octave clear of its neighbours, whole hertz below 1 kHz and tens above (draggedCrossover).
double DraggedCrossover(int index, double freq, const std::array<double, 3>& splits)
{
    const auto& range = kCrossoverRanges[index];
    double low = range.min;
    double high = range.max;

    if (index > 0)
    {
        low = std::max(low, splits[static_cast<std::size_t>(index - 1)] * kMinSplitRatio);
    }

    if (index + 1 < 3)
    {
        high = std::min(high, splits[static_cast<std::size_t>(index + 1)] / kMinSplitRatio);
    }

    const double clamped = std::min(std::max(freq, low), std::max(low, high));
    return clamped < 1000.0 ? std::round(clamped) : std::round(clamped / 10.0) * 10.0;
}

std::string FormatHz(double freq)
{
    char text[16];

    if (freq >= 1000.0)
    {
        std::snprintf(text, sizeof(text), freq >= 10000.0 ? "%.0fk" : "%.1fk", freq / 1000.0);
    }
    else
    {
        std::snprintf(text, sizeof(text), "%.0f", std::round(freq));
    }

    return text;
}

el::rect Box(float x, float y, float width, float height)
{
    return {x, y, x + width, y + height};
}
} // namespace

MultibandPlot::MultibandPlot(const Theme& theme, Kind kind) : mTheme(theme), mKind(kind)
{
}

std::optional<MultibandPlot::Kind> MultibandPlot::KindOf(const std::string& effectType)
{
    if (effectType == guids::kMultibandCompressor)
    {
        return Kind::Compressor;
    }

    if (effectType == guids::kMultibandDistortion)
    {
        return Kind::Distortion;
    }

    return std::nullopt;
}

el::view_limits MultibandPlot::limits(const el::basic_context&) const
{
    return {{200.0f, 100.0f}, {el::full_extent, el::full_extent}};
}

el::rect MultibandPlot::Canvas(const el::rect& bounds) const
{
    return ResponsePlot::CardWell(mTheme, bounds);
}

float MultibandPlot::FreqToX(const el::rect& canvas, double freq) const
{
    const float width = std::max(1.0f, canvas.width() - 2.0f * kPadding);
    const double t = std::log10(std::clamp(freq, 1.0, 1.0e6) / kMinFreq) / std::log10(kMaxFreq / kMinFreq);
    return canvas.left + kPadding + static_cast<float>(t) * width;
}

double MultibandPlot::XToFreq(const el::rect& canvas, float x) const
{
    const float width = std::max(1.0f, canvas.width() - 2.0f * kPadding);
    return kMinFreq * std::pow(kMaxFreq / kMinFreq, (x - canvas.left - kPadding) / width);
}

std::array<double, 3> MultibandPlot::Crossovers(const std::map<std::string, double>& params) const
{
    // orderedCrossovers: each in its range, then pushed a third of an octave above the one below.
    std::array<double, 3> splits{};

    for (std::size_t i = 0; i < splits.size(); ++i)
    {
        const auto& range = kCrossoverRanges[i];
        splits[i] = std::clamp(Param(params, kCrossoverKeys[i], range.fallback), range.min, range.max);
    }

    for (std::size_t i = 1; i < splits.size(); ++i)
    {
        splits[i] = std::max(splits[i], splits[i - 1] * kMinSplitRatio);
    }

    return splits;
}

int MultibandPlot::CrossoverAt(const el::rect& canvas, float x) const
{
    const auto splits = Crossovers(read ? read() : std::map<std::string, double>{});
    int best = -1;
    float bestDistance = kGrabDistance;

    for (std::size_t i = 0; i < splits.size(); ++i)
    {
        const float distance = std::abs(FreqToX(canvas, splits[i]) - x);

        if (distance <= bestDistance)
        {
            best = static_cast<int>(i);
            bestDistance = distance;
        }
    }

    return best;
}

bool MultibandPlot::SetSpectrum(const guitarfx::uiclient::NodeSpectrum* spectrum, double now)
{
    if (spectrum != nullptr && spectrum->binsDb.size() < 2)
    {
        spectrum = nullptr;
    }

    if (spectrum == nullptr ? !mSpectrum.has_value()
                            : mSpectrum.has_value() && mSpectrum->receivedAt == spectrum->receivedAt)
    {
        return false;
    }

    // DepthDisplay: a new peak shows at once; a band letting go falls back at a steady rate.
    const double elapsed = mDepthAt > 0.0 ? std::min(0.5, now - mDepthAt) : 0.0;
    mDepthAt = now;

    for (std::size_t band = 0; band < mShownDepth.size(); ++band)
    {
        const double fallen = std::max(0.0, mShownDepth[band] - kDepthFallDbPerSecond * elapsed);
        const double peak = spectrum != nullptr && band < spectrum->bands.size()
                                ? spectrum->bands[band].gainReductionDb
                                : 0.0;
        mShownDepth[band] = std::max(peak, fallen);
    }

    if (spectrum != nullptr)
    {
        mSpectrum = *spectrum;
    }
    else
    {
        mSpectrum.reset();
    }

    return true;
}

void MultibandPlot::DrawLegend(canvas& cnv, const el::rect& header) const
{
    // .mb-legend: In, Out and the depth, each after its swatch (10 by 3, the depth's 10 by 6),
    // 10 px apart, at the header's right.
    const auto& range = mTheme.Controls().cardRange;
    const auto colours = ColoursFor(mTheme, mKind);
    struct Item
    {
        const char* text;
        el::color swatch;
        float swatchHeight;
    };
    const Item items[] = {{"In", mTheme.PlotSpectrumLine(), 3.0f},
                          {"Out", mTheme.PlotCurve(), 3.0f},
                          {mKind == Kind::Compressor ? "Reduction" : "Drive", colours.depthEdge, 6.0f}};
    float x = header.right;
    const float middle = (header.top + header.bottom) / 2.0f;

    for (auto it = std::rbegin(items); it != std::rend(items); ++it)
    {
        const float textWidth = std::ceil(MeasureControlText(it->text, range)) + 1.0f;
        x -= textWidth;
        DrawControlText(cnv, it->text, range, el::rect{x, header.top, x + textWidth, header.bottom}, HAlign::Left);
        x -= 4.0f + 10.0f;
        cnv.fill_style(it->swatch);
        cnv.add_round_rect(Box(x, middle - it->swatchHeight / 2.0f, 10.0f, it->swatchHeight), 1.0f);
        cnv.fill();
        x -= 10.0f;
    }
}

void MultibandPlot::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const auto& skin = mTheme.Controls();

    // The EQ plot's card (.eq-visualizer.mb-visualizer): "Bands", the legend, the well.
    DrawControlBox(cnv, b.inset(0.5f, 0.5f), skin.card);
    const auto header = ResponsePlot::CardHeader(mTheme, b);
    DrawControlText(cnv, "Bands", skin.cardTitle, header, HAlign::Left);
    DrawLegend(cnv, header);
    const auto c = Canvas(b);
    DrawControlBox(cnv, c, skin.plot);

    const auto params = read ? read() : std::map<std::string, double>{};
    const auto colours = ColoursFor(mTheme, mKind);
    const float plotWidth = std::max(1.0f, c.width() - 2.0f * kPadding);
    const float plotHeight = std::max(1.0f, c.height() - 2.0f * kPadding - kStripHeight - kStripGap);
    const float top = c.top + kPadding;
    const float plotLeft = c.left + kPadding;
    const float plotBottom = top + plotHeight;
    const float stripTop = plotBottom + kStripGap;
    const el::rect plot{plotLeft, top, plotLeft + plotWidth, plotBottom};
    const auto toX = [this, &c](double freq) { return FreqToX(c, freq); };

    // Grid: the EQ's frequency lines.
    cnv.stroke_style(mTheme.PlotGrid());
    cnv.line_width(1.0f);

    for (const double freq : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0})
    {
        const float x = std::round(toX(freq)) + 0.5f;
        cnv.begin_path();
        cnv.move_to(x, top);
        cnv.line_to(x, plotBottom);
        cnv.stroke();
    }

    // The input's spectrum shaded, as behind an EQ curve; the output's as a line over it.
    if (mSpectrum)
    {
        auto state = cnv.new_state();
        cnv.add_rect(plot);
        cnv.clip();
        const auto input = SpectrumPoints(mSpectrum->binsDb, mSpectrum->minHz, mSpectrum->maxHz, plot, toX);

        if (input.size() >= 2)
        {
            canvas::linear_gradient fill{el::point{plot.left, plot.top}, el::point{plot.left, plot.bottom}};
            fill.add_color_stop(0.0f, mTheme.PlotSpectrumTop());
            fill.add_color_stop(1.0f, mTheme.PlotSpectrumBottom());
            cnv.fill_style(fill);
            cnv.begin_path();
            TraceSpectrum(cnv, input);
            cnv.line_to(input.back().x, plot.bottom);
            cnv.line_to(input.front().x, plot.bottom);
            cnv.close_path();
            cnv.fill();
            cnv.stroke_style(mTheme.PlotSpectrumLine());
            cnv.line_width(1.0f);
            cnv.begin_path();
            TraceSpectrum(cnv, input);
            cnv.stroke();
        }

        const auto output = SpectrumPoints(mSpectrum->outputBinsDb, mSpectrum->minHz, mSpectrum->maxHz, plot, toX);

        if (output.size() >= 2)
        {
            cnv.stroke_style(mTheme.PlotCurve());
            cnv.line_width(1.0f);
            cnv.begin_path();
            TraceSpectrum(cnv, output);
            cnv.stroke();
        }
    }

    const auto splits = Crossovers(params);
    const float edges[] = {plotLeft, toX(splits[0]), toX(splits[1]), toX(splits[2]), plotLeft + plotWidth};
    const auto* meters = mSpectrum ? &mSpectrum->bands : nullptr;
    const auto font = Font().size(10.0f);
    bool anySolo = false;

    for (int band = 0; band < kBands; ++band)
    {
        anySolo = anySolo || Flag(params, "solo" + std::to_string(band + 1), false);
    }

    for (int band = 0; band < kBands; ++band)
    {
        const auto n = std::to_string(band + 1);
        const auto i = static_cast<std::size_t>(band);
        const float left = edges[i];
        const float right = edges[i + 1];
        const float columnWidth = std::max(0.0f, right - left);
        BandState state;
        state.on = Flag(params, "on" + n, true);
        state.soloed = Flag(params, "solo" + n, false);
        state.heard = !anySolo || state.soloed;
        // A band turned off does nothing, whatever the meter is still falling back from.
        const double depthDb = state.on ? std::clamp(mShownDepth[i], 0.0, kDepthScaleDb) : 0.0;

        // How hard the band works hangs from the top, over half the plot at full scale.
        const float depth = static_cast<float>(depthDb / kDepthScaleDb) * plotHeight * 0.5f;

        if (depthDb > 0.05)
        {
            cnv.fill_style(colours.depthFill);
            cnv.fill_rect(Box(left + 1.0f, top, columnWidth - 2.0f, depth));
            cnv.fill_style(colours.depthEdge);
            cnv.fill_rect(Box(left + 1.0f, top + depth - 1.5f, columnWidth - 2.0f, 2.0f));
        }

        // A band silenced by another's solo is shaded over: it still works, but none of it is heard.
        if (!state.heard)
        {
            cnv.fill_style(colours.unheard);
            cnv.fill_rect(Box(left, top, columnWidth, plotHeight));
        }

        // The band's name, with Solo or Off when it fits, and its depth while it is working.
        const float alpha = state.on && state.heard ? 1.0f : 0.55f;
        const float centre = left + columnWidth / 2.0f;

        if (columnWidth > 36.0f)
        {
            const std::string name = kBandLabels[i];
            const std::string marked = state.soloed ? name + " \xC2\xB7 Solo" : state.on ? name : name + " \xC2\xB7 Off";
            const auto& text = el::measure_text(cnv, marked, font).x <= columnWidth - 4.0f ? marked : name;
            FillText(cnv, text, font, Dimmed(state.soloed ? colours.solo : colours.label, alpha),
                     el::rect{centre - columnWidth / 2.0f, top + 3.0f, centre + columnWidth / 2.0f, top + 15.0f},
                     HAlign::Centre);
        }

        if (depthDb >= 0.5 && columnWidth > 30.0f)
        {
            // Under the bar's edge, so the edge never strikes through it.
            char text[32];
            std::snprintf(text, sizeof(text), "%s%.1f dB", mKind == Kind::Compressor ? "\xE2\x88\x92" : "+", depthDb);
            const float y = std::max(top + 16.0f, top + depth + 3.0f);
            FillText(cnv, text, font, Dimmed(colours.depthEdge, alpha),
                     el::rect{centre - columnWidth / 2.0f, y, centre + columnWidth / 2.0f, y + 12.0f}, HAlign::Centre);
        }

        // The level strip: the band's level from the left, its threshold as a mark.
        const auto toStripX = [&](double db) {
            const double fraction = (db - kLevelMinDb) / (kLevelMaxDb - kLevelMinDb);
            return left + 2.0f +
                   static_cast<float>(std::clamp(fraction, 0.0, 1.0)) * std::max(0.0f, columnWidth - 4.0f);
        };
        cnv.fill_style(colours.stripBg);
        cnv.fill_rect(Box(left + 1.0f, stripTop, columnWidth - 2.0f, kStripHeight));

        double threshold = kDefaultThresholdDb;
        double kneeDb = 0.0;

        if (mKind == Kind::Compressor)
        {
            // From the knobs, so the mark follows a turn at once.
            threshold = Param(params, "threshold" + n, kDefaultThresholdDb);
            kneeDb = Param(params, "knee" + n, kCompressorDefaultKneeDb);
        }
        else if (meters != nullptr && i < meters->size() && (*meters)[i].thresholdDb)
        {
            // The engine's, which knows the nominal level.
            threshold = *(*meters)[i].thresholdDb;
        }
        else
        {
            // Worked out from Drive until the first frame.
            threshold = kDefaultNominalDbfs + kDistortionKneeAboveNominal -
                        Param(params, "drive" + n, kDistortionDefaultDriveDb[i]);
        }

        if (kneeDb > 0.0)
        {
            const float kneeLeft = toStripX(threshold - kneeDb / 2.0);
            cnv.fill_style(colours.knee);
            cnv.fill_rect(Box(kneeLeft, stripTop, toStripX(threshold + kneeDb / 2.0) - kneeLeft, kStripHeight));
        }

        if (meters != nullptr && i < meters->size() && (*meters)[i].levelDb > kLevelMinDb)
        {
            const float levelX = toStripX((*meters)[i].levelDb);
            // Off, nothing acts on the band, so none of its level is "over".
            const float thresholdX = state.on ? toStripX(threshold) : levelX;
            cnv.fill_style(colours.level);
            cnv.fill_rect(Box(left + 2.0f, stripTop + 3.0f, std::min(levelX, thresholdX) - left - 2.0f,
                              kStripHeight - 6.0f));

            if (levelX > thresholdX)
            {
                cnv.fill_style(colours.levelOver);
                cnv.fill_rect(Box(thresholdX, stripTop + 3.0f, levelX - thresholdX, kStripHeight - 6.0f));
            }
        }

        cnv.fill_style(mTheme.PlotCurve());
        cnv.fill_rect(Box(toStripX(threshold) - 1.0f, stripTop, 2.0f, kStripHeight));
    }

    // The crossovers, over everything, with their frequency at the foot.
    for (std::size_t i = 0; i < splits.size(); ++i)
    {
        const float x = std::round(toX(splits[i])) + 0.5f;
        const bool active = static_cast<int>(i) == mActive;
        const auto ink = active ? mTheme.PlotCurve() : colours.label;
        cnv.stroke_style(active ? mTheme.PlotCurve() : colours.crossover);
        cnv.line_width(active ? 2.0f : 1.0f);
        cnv.begin_path();
        cnv.move_to(x, top);
        cnv.line_to(x, stripTop + kStripHeight);
        cnv.stroke();
        cnv.fill_style(ink);
        cnv.fill_rect(Box(x - 3.0f, plotBottom - 14.0f, 6.0f, 10.0f));
        FillText(cnv, FormatHz(splits[i]), font, ink,
                 el::rect{x - 30.0f, plotBottom - 16.0f - 12.0f, x + 30.0f, plotBottom - 16.0f}, HAlign::Centre);
    }
}

bool MultibandPlot::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state != el::mouse_button::left)
    {
        return false;
    }

    const auto c = Canvas(ctx.bounds);

    if (btn.down)
    {
        const int index = CrossoverAt(c, btn.pos.x);

        if (index < 0)
        {
            return false;
        }

        mDragging = index;
        mActive = index;
        ctx.view.refresh(ctx);
        return true;
    }

    if (mDragging >= 0)
    {
        const int index = mDragging;
        mDragging = -1;

        if (write && read)
        {
            const auto splits = Crossovers(read());
            write(kCrossoverKeys[index], splits[static_cast<std::size_t>(index)], true);
        }

        mActive = CrossoverAt(c, btn.pos.x);
        ctx.view.refresh(ctx);
    }

    return true;
}

void MultibandPlot::drag(const el::context& ctx, el::mouse_button btn)
{
    if (mDragging < 0 || !read || !write)
    {
        return;
    }

    const auto c = Canvas(ctx.bounds);
    const auto params = read();
    const auto splits = Crossovers(params);
    const double value = DraggedCrossover(mDragging, XToFreq(c, btn.pos.x), splits);
    const char* key = kCrossoverKeys[mDragging];

    if (Param(params, key, std::nan("")) != value)
    {
        write(key, value, false);
        ctx.view.refresh(ctx);
    }
}

bool MultibandPlot::cursor(const el::context& ctx, el::point p, el::cursor_tracking status)
{
    const int hover =
        mDragging >= 0 ? mDragging : status == el::cursor_tracking::leaving ? -1 : CrossoverAt(Canvas(ctx.bounds), p.x);

    if (hover != mActive)
    {
        mActive = hover;
        ctx.view.refresh(ctx);
    }

    if (hover >= 0)
    {
        el::set_cursor(el::cursor_type::h_resize);
    }

    return hover >= 0;
}
} // namespace nanoq::ui
