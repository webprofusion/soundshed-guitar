#pragma once

// ResponsePlot - the web UI's response plot (eqPlot.ts, eqCurve.ts): a log-frequency axis from
// 20 Hz to 20 kHz, a grid, the live spectrum of the node's input behind, and the curve the
// engine reports for the effect (getEffectResponse, through uiclient's Analysis). Nano Q keeps no
// copy of any filter design: the curve and each band's shading are the engine's.
//
// With bands set it is the parametric EQ's editor: a handle per band (frequency across, gain up
// and down), Q wings either side of a bell, the wheel over a handle for Q, and a double click to
// put a band back. Changes are reported through onBandChange; the owner sets the parameters and
// asks for the new curve.

#include "ui/Drawn.h"
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

class ResponsePlot : public el::element, public Drawn
{
  public:
    explicit ResponsePlot(const Theme& theme);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    void drag(const el::context& ctx, el::mouse_button btn) override;
    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override;
    bool scroll(const el::context& ctx, el::point dir, el::point p) override;

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

    [[nodiscard]] el::rect Plot(const el::rect& bounds) const;
    [[nodiscard]] float FreqToX(const el::rect& plot, double freq) const;
    [[nodiscard]] double XToFreq(const el::rect& plot, float x) const;
    [[nodiscard]] float DbToY(const el::rect& plot, double db) const;
    [[nodiscard]] double YToDb(const el::rect& plot, float y) const;
    [[nodiscard]] Hit HitTest(const el::rect& bounds, el::point p) const;
    void DrawSpectrum(cycfi::artist::canvas& cnv, const el::rect& plot) const;
    void DrawBands(cycfi::artist::canvas& cnv, const el::rect& plot) const;
    void DrawHandles(cycfi::artist::canvas& cnv, const el::rect& plot) const;
    void Report(int band, bool commit);

    const Theme& mTheme;
    double mMinDb = -18.0;
    double mMaxDb = 18.0;
    double mGridStepDb = 9.0;
    std::optional<guitarfx::uiclient::ResponseCurve> mCurve;
    std::optional<guitarfx::uiclient::ResponseCurve> mReference;
    std::optional<guitarfx::uiclient::NodeSpectrum> mSpectrum;
    std::vector<PlotBand> mBands;
    int mDragBand = -1;
    Grip mDragGrip = Grip::None;
    int mHoverBand = -1;
};
} // namespace nanoq::ui
