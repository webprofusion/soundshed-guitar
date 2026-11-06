#pragma once

// A switch's thumb slides between off and on, as the web UI's .toggle-switch does (a 0.2 s
// transition), and its track's colour goes with it.
//
// A switch whose state changes in place (Set() on a bound one) slides from where it was drawn. A
// tap usually rebuilds the region instead, and the new switch is drawn at once in its new state;
// so a switch notes where it was and how far on as it is pressed (NoteToggled), and a new one
// drawn in that place soon after starts its slide from there. Only a press does that, so turning
// to another effect never slides its switches from the last one's.
//
// The slide is drawn by redrawing the switch's area each tick until it ends (Tick). With Windows'
// "Show animations" off (reduced motion), switches jump, as the web UI's do under
// prefers-reduced-motion.

#include "ui/Drawn.h"

#include <elements.hpp>

#include <optional>

namespace nanoq::ui
{
class SwitchMotion
{
  public:
    /// How far on to draw the switch now, 0 (off) to 1 (on). `area` is where it is drawn, in
    /// view coordinates (Drawn::DrawnArea after NoteDrawn).
    float Position(bool on, const std::optional<el::rect>& area);

    /// The switch was pressed: a switch drawn in its place soon after slides from here.
    void NoteToggled(const std::optional<el::rect>& area) const;

    /// Redraws every switch that is sliding. Each tick, on the UI thread.
    static void Tick(el::view& view);

    /// The colour between `off` and `on` at `t`.
    [[nodiscard]] static el::color Mix(el::color off, el::color on, float t);

  private:
    bool mStarted = false;
    float mFrom = 0.0f, mTo = 0.0f;
    double mStart = 0.0;
};
} // namespace nanoq::ui
