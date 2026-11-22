#pragma once

// ResponsePlot - the web UI's response plot (eqPlot.ts, eqCurve.ts): a log-frequency axis from
// 20 Hz to 20 kHz, a grid, the live spectrum of the node's input behind, and the curve the
// engine reports for the effect (getEffectResponse, through uiclient's Analysis). Nano Q keeps no
// copy of any filter design: the curve and each band's shading are the engine's.
//
// With bands set it is the parametric EQ's editor: a handle per band (frequency across, gain up
// and down), Q wings either side of a bell to drag for Q, and a double click to put a band back.
// Changes are reported through onBandChange; the owner sets the parameters and asks for the new
// curve.

#include "ui/Drawn.h"
#include "ui/widgets/Pressable.h"
#include "ui/widgets/Theme.h"
#include "uiclient/Analysis.h"
#include "uiclient/ClientState.h"

#include <elements.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace nanoq::ui
{
namespace el = cycfi::elements;

/// One band of an EQ the plot edits.
struct PlotBand
{
    std::string label;
    double freq = 1000.0;
    double gainDb = 0.0;
    double q = 1.0;
    bool hasQ = true;
    double freqMin = 20.0, freqMax = 20000.0;
    double gainMin = -12.0, gainMax = 12.0;
    double qMin = 0.1, qMax = 10.0;
};

/// Where a frequency sits across a plot, in the plot's coordinates: a graphic EQ's slider for its
/// band (eqPlot.ts anchoredFrequencyAxis).
struct AxisAnchor
{
    double freq = 1000.0;
    float x = 0.0f;
};

/// A small button in a toolbar under or over a plot, as the web UI draws them (.graphic-eq-reset-btn,
/// .cab-response-tools button): its label in a box, the accent round it under the pointer, and
/// faded while it is disabled.
class PlotToolButton final : public Pressable
{
  public:
    PlotToolButton(const Theme& theme, std::string label);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;

    [[nodiscard]] AccessInfo Describe() const override;

    /// Its width: the label and the padding either side of it.
    [[nodiscard]] float Width() const
    {
        return mWidth;
    }

    static constexpr float kHeight = 26.0f;

  private:
    const Theme& mTheme;
    std::string mLabel;
    float mWidth = 0.0f;
};

/// A spectrum's bins (log-spaced from `minHz` to `maxHz`, both ends included) as points on
/// `plot`, across by `toX` and up from the floor to the ceiling eqPlot.ts draws them between
/// (eqSpectrumPlotPoints).
[[nodiscard]] std::vector<el::point> SpectrumPoints(const std::vector<double>& binsDb, double minHz, double maxHz,
                                                    const el::rect& plot, const std::function<float(double)>& toX);

/// Adds the spectrum's top edge through `points` to the path, smoothed through the midpoints
/// between bins as eqPlot.ts traceSpectrum does; starts with a move to the first point.
void TraceSpectrum(cycfi::artist::canvas& cnv, const std::vector<el::point>& points);

class ResponsePlot : public el::element, public Drawn
{
  public:
    explicit ResponsePlot(const Theme& theme);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    void drag(const el::context& ctx, el::mouse_button btn) override;
    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override;

    bool wants_control() const override
    {
        return !mBands.empty();
    }

    /// The level at the bottom and top of the plot, and a grid line at every multiple of `step`.
    ResponsePlot& Range(double minDb, double maxDb, double step)
    {
        mMinDb = minDb;
        mMaxDb = maxDb;
        mGridStepDb = step;
        return *this;
    }

    /// Drawn in the web UI's plot card (.eq-visualizer), as the theme draws it: its box, with
    /// `title` over the plot's left and `range` over its right.
    ResponsePlot& Card(std::string title, std::string range)
    {
        mCardTitle = std::move(title);
        mCardRange = std::move(range);
        mCard = true;
        return *this;
    }

    /// Drawn behind other controls, as the graphic EQ's curve is behind its sliders
    /// (.graphic-eq-curve-canvas): no card and no well, the whole plot at `opacity` and its
    /// colours at `saturation` (the web UI's opacity and saturate()).
    ResponsePlot& Behind(float opacity, float saturation)
    {
        mBehind = true;
        mOpacity = opacity;
        mSaturation = saturation;
        return *this;
    }

    /// The frequency axis pinned to `anchors(bounds)` rather than spread evenly in log frequency:
    /// log frequency runs straight between neighbouring anchors and on past the ends at the
    /// nearest pair's rate. Anchors that do not rise in both frequency and x leave the plain axis.
    ResponsePlot& AnchorAxis(std::function<std::vector<AxisAnchor>(const el::rect& bounds)> anchors)
    {
        mAnchorsFor = std::move(anchors);
        return *this;
    }

    /// The height a card takes around a plot `plotHeight` high, for the theme's padding.
    [[nodiscard]] static float CardHeight(const Theme& theme, float plotHeight);

    /// The card's header line and its well (where the plot is drawn) inside a card at `bounds`,
    /// for another plot drawn in the same card (MultibandPlot).
    [[nodiscard]] static el::rect CardHeader(const Theme& theme, const el::rect& bounds);
    [[nodiscard]] static el::rect CardWell(const Theme& theme, const el::rect& bounds);

    /// The engine's curve; nothing draws no curve (an effect without one, or none yet).
    void SetCurve(std::optional<guitarfx::uiclient::ResponseCurve> curve)
    {
        mCurve = std::move(curve);
    }

    /// A second curve drawn dashed behind the first, to compare it with (the IR Cabinet's two
    /// IRs added without interfering); nothing draws none.
    void SetReference(std::optional<guitarfx::uiclient::ResponseCurve> reference)
    {
        mReference = std::move(reference);
    }

    /// The input's spectrum behind the curve, or null to clear it. Returns true when it changed.
    bool SetSpectrum(const guitarfx::uiclient::NodeSpectrum* spectrum);

    /// The bands to edit; ignored while one is being dragged (the drag is the newer value).
    void SetBands(std::vector<PlotBand> bands);

    [[nodiscard]] bool Dragging() const
    {
        return mDragBand >= 0;
    }

    /// A band moved: `commit` is false while dragging and true once on release.
    std::function<void(int band, double freq, double gainDb, double q, bool commit)> onBandChange;
    /// A double click on a band's handle.
    std::function<void(int band)> onBandReset;

  private:
    enum class Grip
    {
        None,
        Main,
        QLeft,
        QRight
    };

    struct Hit
    {
        int band = -1;
        Grip grip = Grip::None;
    };

    /// A handle where it is drawn: its band, which grip, its centre and its size unhovered.
    struct Handle
    {
        int band = -1;
        Grip grip = Grip::None;
        el::point at;
        float radius = 0.0f;
    };

    [[nodiscard]] el::rect Plot(const el::rect& bounds) const;
    [[nodiscard]] float FreqToX(const el::rect& plot, double freq) const;
    [[nodiscard]] double XToFreq(const el::rect& plot, float x) const;
    [[nodiscard]] float DbToY(const el::rect& plot, double db) const;
    [[nodiscard]] double YToDb(const el::rect& plot, float y) const;
    [[nodiscard]] std::vector<Handle> Handles(const el::rect& plot) const;
    [[nodiscard]] Hit HitTest(const el::rect& bounds, el::point p) const;
    void DrawContent(const el::context& ctx);
    void DrawSpectrum(cycfi::artist::canvas& cnv, const el::rect& plot) const;
    void DrawBands(cycfi::artist::canvas& cnv, const el::rect& plot) const;
    void DrawHandles(cycfi::artist::canvas& cnv, const el::rect& plot) const;
    void DrawTooltip(cycfi::artist::canvas& cnv, const el::rect& bounds, const el::rect& plot) const;
    void Report(int band, bool commit);
    /// The pointer over a handle shows it can be moved, as the web UI's grab cursor.
    void ShowCursor(bool overHandle);
    /// A colour at the plot's saturation (Behind).
    [[nodiscard]] el::color Tone(el::color c) const;
    /// The curve traced across the plot a pixel at a time, through `dbAt`.
    [[nodiscard]] std::vector<el::point> Trace(const el::rect& plot,
                                               const std::function<double(double freq)>& dbAt) const;

    /// The plot's well: the bounds, or inside the card under its header.
    [[nodiscard]] el::rect Well(const el::rect& bounds) const;

    const Theme& mTheme;
    bool mCard = false;
    std::string mCardTitle;
    std::string mCardRange;
    bool mBehind = false;
    float mOpacity = 1.0f;
    float mSaturation = 1.0f;
    double mMinDb = -18.0;
    double mMaxDb = 18.0;
    double mGridStepDb = 9.0;
    std::optional<guitarfx::uiclient::ResponseCurve> mCurve;
    std::optional<guitarfx::uiclient::ResponseCurve> mReference;
    std::optional<guitarfx::uiclient::NodeSpectrum> mSpectrum;
    std::vector<PlotBand> mBands;
    int mDragBand = -1;
    Grip mDragGrip = Grip::None;
    Hit mHover;
    bool mGrabCursor = false;
    std::function<std::vector<AxisAnchor>(const el::rect& bounds)> mAnchorsFor;
    // The anchored axis as last drawn, as log10 frequency and x: empty for the plain axis.
    std::vector<std::pair<double, float>> mAxis;
};
} // namespace nanoq::ui
