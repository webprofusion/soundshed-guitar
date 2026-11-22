#pragma once

// SpatialPanner - the 3D Spatial node's panner (the web UI's spatialPanner.ts): a top-down radar
// for direction and distance around the listener's head, and a side-on arc for height. The
// distance is on a log scale, so the near field (where the cues change fastest) gets most of the
// radar. The source is drawn where it is heard (spatialPosition) with a fading trail while the
// node moves it, and the set position as a dashed ring; behind the listener it takes the second
// colour. In speaker mode the height and front/back cues are scaled back, so the rear half of the
// radar is shaded and the arc dimmed.
//
// Drag on the radar for direction and distance, on the arc for height, with Shift for fine; a
// double click resets the axis it lands on. With focus the arrows pan and tilt, Alt with up or
// down changes the distance, and Home re-centres.
//
// SpatialCard is the web UI's .spatial-visualizer round it: the "Source Position" header with the
// listening hint, and the help line under the panner.

#include "ui/Drawn.h"
#include "ui/input/Focus.h"
#include "ui/widgets/Theme.h"
#include "uiclient/ClientState.h"

#include <elements.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>

namespace nanoq::ui
{
namespace el = cycfi::elements;

struct SpatialPoint
{
    double azimuth = 0.0;
    double elevation = 0.0;
    double distance = 1.5;
};

class SpatialPanner : public el::element, public Drawn, public Focusable
{
  public:
    /// The web UI's .spatial-panner-canvas height.
    static constexpr float kHeight = 190.0f;

    SpatialPanner() = default;

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    void drag(const el::context& ctx, el::mouse_button btn) override;
    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override;
    bool key(const el::context& ctx, el::key_info k) override;

    bool wants_control() const override
    {
        return true;
    }

    bool wants_focus() const override
    {
        return FocusWanted(true);
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override
    {
        NoteFocus(false);
        return true;
    }

    [[nodiscard]] AccessInfo Describe() const override;

    /// The set position (the node's parameters); ignored while dragging.
    void SetAnchor(const SpatialPoint& position);
    /// Where the source is heard now, or nothing when the node sends no position.
    void SetLive(const guitarfx::uiclient::SpatialPosition* live);

    /// Returns whether the mode changed.
    bool SetSpeakerMode(bool speakers)
    {
        const bool changed = mSpeakers != speakers;
        mSpeakers = speakers;
        return changed;
    }

    [[nodiscard]] bool Speakers() const
    {
        return mSpeakers;
    }

    /// `commit` is false while dragging and true once on release (and for a key or a reset).
    std::function<void(const SpatialPoint& position, bool commit)> onChange;

  private:
    enum class Target
    {
        None,
        Radar,
        Arc
    };

    struct Layout
    {
        float radarCx, radarCy, radarInner, radarOuter, arcX, arcCy, arcR, arcPaneLeft;
    };

    struct Live
    {
        SpatialPoint position;
        double itdUs = 0.0;
        double ildDb = 0.0;
        bool moving = false;
    };

    [[nodiscard]] static Layout ComputeLayout(const el::rect& b);
    [[nodiscard]] Target HitTest(const Layout& layout, el::point p) const;
    [[nodiscard]] SpatialPoint FromRadar(const Layout& layout, el::point p, bool fine) const;
    [[nodiscard]] SpatialPoint FromArc(const Layout& layout, el::point p, bool fine) const;
    void ApplyDrag(const el::context& ctx, el::point p, bool fine, bool commit);
    void Commit(const el::context& ctx);

    void DrawRadar(cycfi::artist::canvas& cnv, const Layout& layout, const SpatialPoint& shown) const;
    void DrawElevation(cycfi::artist::canvas& cnv, const Layout& layout, const SpatialPoint& shown) const;
    void DrawReadouts(cycfi::artist::canvas& cnv, const el::rect& b, const SpatialPoint& shown) const;

    SpatialPoint mPosition;
    std::optional<Live> mLive;
    std::uint64_t mLiveSerial = 0;
    std::deque<SpatialPoint> mTrail;
    bool mSpeakers = false;
    Target mDragging = Target::None;
    Target mHover = Target::None;
};

/// The card round a SpatialPanner (.spatial-visualizer): lay the panner over it, inset by
/// PannerInsets().
class SpatialCard final : public el::element, public Drawn
{
  public:
    explicit SpatialCard(const Theme& theme) : mTheme(theme), mPanner(std::make_shared<SpatialPanner>())
    {
    }

    void draw(const el::context& ctx) override;

    [[nodiscard]] SpatialPanner& Panner() const
    {
        return *mPanner;
    }

    [[nodiscard]] const std::shared_ptr<SpatialPanner>& PannerPtr() const
    {
        return mPanner;
    }

    /// The card's height at `width`, its help line wrapped to fit.
    [[nodiscard]] static float HeightFor(const Theme& theme, float width);

    /// Where the panner sits in the card: its left, top and right insets (the bottom is the
    /// card's height less these and the panner's own).
    static constexpr float kSide = 13.0f;      // 1 px border, 12 px padding
    static constexpr float kPannerTop = 28.0f; // border, padding, the header's line, the gap

  private:
    const Theme& mTheme;
    std::shared_ptr<SpatialPanner> mPanner;
};
} // namespace nanoq::ui
