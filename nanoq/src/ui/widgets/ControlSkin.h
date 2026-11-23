#pragma once

// The effect editor's controls as each theme draws them in the web UI (a knob's face, pointer and
// picture, a switch, a group's box, letter-spaced labels): read from its CSS by
// tools/capture-native-controls.mjs into ThemeControls.generated.h, and drawn here.

#include "ui/widgets/Layout.h"
#include "ui/widgets/TextDraw.h"
#include "ui/widgets/Theme.h"
#include "uiclient/ThemeControls.generated.h"

#include <string>

namespace nanoq::ui
{
using SurfaceBackdrop = guitarfx::uiclient::theme::Backdrop;
using guitarfx::uiclient::theme::BackdropLayer;
using guitarfx::uiclient::theme::Backdrops;
using guitarfx::uiclient::theme::ControlBox;
using guitarfx::uiclient::theme::ControlSkin;
using guitarfx::uiclient::theme::ControlText;
using guitarfx::uiclient::theme::DisplaySkin;
using guitarfx::uiclient::theme::KeySkin;
using guitarfx::uiclient::theme::KnobSkin;
using guitarfx::uiclient::theme::NodeSkin;
using guitarfx::uiclient::theme::SwitchSkin;

/// Draws `r` as the web UI draws the box: its drop shadow and hard ring outside, its background
/// top to bottom, its inset lines along the top and bottom edges, then its border. Lengths
/// (border, ring, shadow) are multiplied by `scale`; `radius` replaces the box's own when not
/// negative (the web's 50% is a circle: half the shorter side).
void DrawControlBox(cycfi::artist::canvas& cnv, el::rect r, const ControlBox& box, float scale = 1.0f,
                    float radius = -1.0f);

/// The same box with `fill` (its colour and every layer: pictures, gradients) in place of the
/// box's own top to bottom background.
void DrawControlBox(cycfi::artist::canvas& cnv, const Theme& theme, el::rect r, const ControlBox& box,
                    const SurfaceBackdrop& fill, float scale = 1.0f, float radius = -1.0f);

/// Draws a surface's background over `r` as the web UI's CSS does, clipped to `r` rounded by
/// `radius`: its colour when `withColour`, then its layers from the bottom one up - pictures
/// placed, sized and tiled, linear and radial gradients.
void DrawBackdrop(cycfi::artist::canvas& cnv, const Theme& theme, el::rect r, const SurfaceBackdrop& backdrop,
                  bool withColour, float radius = 0.0f);

/// The face for `text` at `size` (its own size when 0): Inter at the nearest weight Nano draws.
[[nodiscard]] el::font_descr ControlFont(const ControlText& text, float size = 0.0f);

/// The width of `text` in `font` with `tracking` ems between its letters.
[[nodiscard]] float TrackedWidth(cycfi::artist::canvas& cnv, const std::string& text, const el::font_descr& font,
                                 float size, float tracking);

/// The width `text` takes in its skin's face, case and letter spacing at `size` (its own when
/// 0), measured off screen: for sizing a cell before it is drawn. UI thread.
[[nodiscard]] float MeasureControlText(const std::string& text, const ControlText& skin, float size = 0.0f);

/// One line of `text` in its skin's colour, case and letter spacing, at `size` (its own when 0),
/// cut with an ellipsis to fit `bounds` and placed across it by `align`.
void DrawControlText(cycfi::artist::canvas& cnv, std::string text, const ControlText& skin, el::rect bounds,
                     HAlign align, float size = 0.0f);

/// The same, in `colour` in place of the skin's.
void DrawControlText(cycfi::artist::canvas& cnv, std::string text, const ControlText& skin, el::color colour,
                     el::rect bounds, HAlign align, float size = 0.0f);

/// A knob `diameter` across at `centre`, as the theme draws it: the value ring round its edge
/// (filled from its start to `value`, 0..1 of its 270 degree sweep, as the web UI's conic ring),
/// its face (flat, a radial gradient or the theme's picture), the still tick under it and the
/// pointer that turns.
void DrawKnob(cycfi::artist::canvas& cnv, const Theme& theme, el::point centre, float diameter, float value);

/// A key standing in `bounds` as the theme draws it (a KeySkin that is `drawn`): its side wall
/// along the foot, the face over it with `face` (its Backdrops entry) and its box, lightened when
/// `hover`, pushed down its wall when `down` (and a latched key by its own press). `border`, when
/// visible, replaces the box's border colour (a module's category). Returns where the face is,
/// for its legend.
el::rect DrawKey(cycfi::artist::canvas& cnv, const Theme& theme, el::rect bounds, const KeySkin& key,
                 const SurfaceBackdrop& face, bool hover, bool down, std::uint32_t border = 0);

/// A display's lettering (the preset's name, the tempo) in `colour`, with the theme's glow round
/// it where it has one.
void DrawDisplayText(cycfi::artist::canvas& cnv, const std::string& text, const el::font_descr& font,
                     const DisplaySkin& display, el::color colour, el::rect bounds, HAlign align);

/// A switch in `track` (the web's 36 by 20 slot, scaled to it), `on` from 0 (off) to 1 (on): the
/// slot and thumb blend between the two states as it slides.
void DrawSwitch(cycfi::artist::canvas& cnv, el::rect track, const ControlSkin& skin, float on, bool hover);

/// An area in `colour` with the theme's layers for one of its surfaces over it (`which`: the
/// bars' grain or plate, the page's, a dialog's), as an element to lay behind content.
class BackdropBox final : public el::element
{
  public:
    BackdropBox(const Theme& theme, el::color colour, SurfaceBackdrop Backdrops::* which)
        : mTheme(theme), mColour(colour), mWhich(which)
    {
    }

    void draw(const el::context& ctx) override;

  private:
    const Theme& mTheme;
    el::color mColour;
    SurfaceBackdrop Backdrops::* mWhich;
};

/// `content` over a BackdropBox: Fill(colour, content) with the theme's layers for `which`.
[[nodiscard]] inline Ptr Dressed(const Theme& theme, el::color colour, SurfaceBackdrop Backdrops::* which, Ptr content)
{
    return Over(std::move(content), std::make_shared<BackdropBox>(theme, colour, which));
}

/// A box in one of the theme's control styles (`which`: the plot card, the controls' section),
/// as an element to lay behind content.
class SkinBox final : public el::element
{
  public:
    SkinBox(const Theme& theme, ControlBox ControlSkin::* which) : mTheme(theme), mWhich(which)
    {
    }

    void draw(const el::context& ctx) override
    {
        DrawControlBox(ctx.canvas, ctx.bounds.inset(0.5f, 0.5f), mTheme.Controls().*mWhich);
    }

  private:
    const Theme& mTheme;
    ControlBox ControlSkin::* mWhich;
};

/// A line of text in one of the theme's control styles (`which`: a card's title), as an element.
class SkinText final : public el::element
{
  public:
    SkinText(const Theme& theme, std::string text, ControlText ControlSkin::* which, HAlign align = HAlign::Left)
        : mTheme(theme), mText(std::move(text)), mWhich(which), mAlign(align)
    {
    }

    void draw(const el::context& ctx) override
    {
        DrawControlText(ctx.canvas, mText, mTheme.Controls().*mWhich, ctx.bounds, mAlign);
    }

  private:
    const Theme& mTheme;
    std::string mText;
    ControlText ControlSkin::* mWhich;
    HAlign mAlign;
};
} // namespace nanoq::ui
