#pragma once

// What LayoutView.cpp, which lays a custom layout out, and LayoutControlView.cpp, which draws and
// plays its controls, both measure with.

#include <elements.hpp>

#include <cmath>

namespace nanoq::ui::layout_geometry
{
inline constexpr float kPi = 3.14159265f;
inline constexpr float kDragPixels = 200.0f; // a knob's full travel, as the web UI's and ParamKnob's

/// A point in view coordinates (where menus are placed), from one in a scaled layout's.
inline el::point ToView(const el::context& ctx, el::point p)
{
    const auto device = ctx.canvas.user_to_device(p);
    const float scale = ctx.view.scale() > 0.0f ? ctx.view.scale() : 1.0f;
    return {device.x / scale, device.y / scale};
}

inline float LabelLine(float size)
{
    return std::ceil(size * 1.25f);
}

inline float ValueLine(float size)
{
    return std::ceil(size * 1.3f);
}
} // namespace nanoq::ui::layout_geometry
