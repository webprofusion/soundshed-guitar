#pragma once

// MultibandPlot - the multi-band effects' plot (the web UI's multibandPlot.ts and
// signalPath/paramsPanel/multibandPanel.ts), for the Multi-band Compressor and Distortion: their
// four bands over the live spectrum, so the player can see what each band is doing and how hard.
//
//   - the node's input spectrum, shaded, as behind an EQ curve
//   - its output spectrum, as a line over it
//   - a column per band, divided at the crossovers, with how hard the band is working hanging
//     from the top and its number: the compressor's gain reduction, the distortion's drive past
//     its clipper's knee
//   - under the plot, each band's level against where its curve starts to act, with the
//     compressor's knee as a shaded span around that mark
//   - a band turned off, or silenced by another band's solo, dimmed
//
// Drawn in the EQ plot's card ("Bands", with a legend for what is drawn). A crossover can be
// dragged; the drag sets crossover1..3 as their knobs would.

#include "ui/Drawn.h"
#include "ui/widgets/Theme.h"
#include "uiclient/ClientState.h"

#include <elements.hpp>

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace nanoq::ui
{
namespace el = cycfi::elements;

class MultibandPlot : public el::element, public Drawn
{
  public:
    enum class Kind
    {
        Compressor,
        Distortion
    };

    static constexpr int kBands = 4;
    /// The plot's height in the card (.mb-section .mb-canvas).
    static constexpr float kPlotHeight = 200.0f;

    MultibandPlot(const Theme& theme, Kind kind);

    /// The kind for an effect type, or nothing for any other effect.
    [[nodiscard]] static std::optional<Kind> KindOf(const std::string& effectType);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    void drag(const el::context& ctx, el::mouse_button btn) override;
    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override;

    bool wants_control() const override
    {
        return true;
    }

    /// The live spectrum in and out with the band meters, or null once it stops; `now` in
    /// seconds, for how fast a band's drawn depth falls back. Returns true when it changed.
    bool SetSpectrum(const guitarfx::uiclient::NodeSpectrum* spectrum, double now);

    /// The node's parameters, read as the plot is drawn.
    std::function<std::map<std::string, double>()> read;
    /// A crossover dragged: `commit` is false while dragging and true once on release.
    std::function<void(const std::string& key, double value, bool commit)> write;

  private:
    struct BandState
    {
        bool on = true;
        bool soloed = false;
        bool heard = true;
    };

    [[nodiscard]] el::rect Canvas(const el::rect& bounds) const;
    [[nodiscard]] float FreqToX(const el::rect& canvas, double freq) const;
    [[nodiscard]] double XToFreq(const el::rect& canvas, float x) const;
    [[nodiscard]] std::array<double, 3> Crossovers(const std::map<std::string, double>& params) const;
    [[nodiscard]] int CrossoverAt(const el::rect& canvas, float x) const;
    void DrawLegend(cycfi::artist::canvas& cnv, const el::rect& header) const;

    const Theme& mTheme;
    Kind mKind;
    std::optional<guitarfx::uiclient::NodeSpectrum> mSpectrum;
    std::array<double, kBands> mShownDepth{}; // DepthDisplay: how hard each band works, as drawn
    double mDepthAt = 0.0;
    int mActive = -1;   // the crossover dragged or pointed at
    int mDragging = -1; // the crossover being dragged
};
} // namespace nanoq::ui
