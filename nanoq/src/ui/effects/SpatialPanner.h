#pragma once

// SpatialPanner - the 3D Spatial node's panner (the web UI's spatialPanner.ts): a top-down radar
// for direction and distance around the listener's head, and a side-on arc for height. The
// distance is on a log scale, so the near field (where the cues change fastest) gets most of the
// radar. The source is drawn where it is heard (spatialPosition, while the node moves it) with a
// fading trail, and the set position as a ring; behind the listener it takes the second colour.
// In speaker mode the height and front/back cues are scaled back, so the arc is dimmed.
//
// Drag on the radar for direction and distance, on the arc for height; a double click puts the
// source back in front at 1.5 m.

#include "ui/Drawn.h"
#include "ui/widgets/Theme.h"
#include "uiclient/ClientState.h"

#include <elements.hpp>

#include <deque>
#include <functional>
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

class SpatialPanner : public el::element, public Drawn
{
  public:
    explicit SpatialPanner(const Theme& theme) : mTheme(theme)
    {
    }

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    void drag(const el::context& ctx, el::mouse_button btn) override;

    bool wants_control() const override
    {
        return true;
    }

    /// The set position (the node's parameters); ignored while dragging.
    void SetPosition(const SpatialPoint& position);
    /// Where the source is heard now, or nothing when the node is not moving it.
    void SetLive(const guitarfx::uiclient::SpatialPosition* live);

    void SetSpeakerMode(bool speakers)
    {
        mSpeakers = speakers;
    }

    /// `commit` is false while dragging and true once on release.
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

    [[nodiscard]] Layout ComputeLayout(const el::rect& b) const;
    void MoveTo(const Layout& layout, el::point p);

    const Theme& mTheme;
    SpatialPoint mPosition;
    std::optional<SpatialPoint> mLive;
    std::deque<SpatialPoint> mTrail;
    bool mSpeakers = false;
    Target mDragging = Target::None;
};
} // namespace nanoq::ui
