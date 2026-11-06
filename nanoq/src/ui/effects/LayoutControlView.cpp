// One control of a custom effect layout (see LayoutView.h): drawn from its look, a knob, toggle or
// slider, and played by click, drag, wheel and keys.

#include "ui/effects/LayoutView.h"

#include "ui/effects/LayoutGeometry.h"
#include "ui/widgets/TextDraw.h"
#include "ui/widgets/Widgets.h"
#include "uiclient/ParamFormat.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;
using namespace layout_geometry;

LayoutControlView::LayoutControlView(const Theme& theme, LayoutImages& images, EffectParamInfo param, Look look,
                                     const LayoutActions& actions)
    : mTheme(theme), mImages(images), mParam(std::move(param)), mLook(std::move(look)), mActions(actions)
{
    mValue = mParam.defaultValue;
    mPosition = ParamValueToPosition(mParam, mValue);
}

el::extent LayoutControlView::Size() const
{
    float h = mLook.control.y;

    if (mLook.labelTop)
    {
        h += LabelLine(mLook.labelSize) + 4.0f;
    }

    if (mLook.labelBottom)
    {
        h += 4.0f + LabelLine(mLook.labelSize);
    }

    if (mLook.style.showValue)
    {
        h += 2.0f + ValueLine(mLook.valueSize);
    }

    return {mLook.width, h};
}

el::view_limits LayoutControlView::limits(const el::basic_context&) const
{
    const auto s = Size();
    return {{s.x, s.y}, {s.x, s.y}};
}

el::rect LayoutControlView::ControlBox(el::rect b) const
{
    const float top = b.top + (mLook.labelTop ? LabelLine(mLook.labelSize) + 4.0f : 0.0f);
    const float cx = (b.left + b.right) * 0.5f;
    return {cx - mLook.control.x * 0.5f, top, cx + mLook.control.x * 0.5f, top + mLook.control.y};
}

el::rect LayoutControlView::ValueBox(el::rect b) const
{
    if (!mLook.style.showValue)
    {
        return {};
    }

    return {b.left, b.bottom - ValueLine(mLook.valueSize), b.right, b.bottom};
}

el::element* LayoutControlView::hit_test(const el::context& ctx, el::point p, bool, bool)
{
    // The control and its value answer; the label, as in the web UI, does not.
    return ControlBox(ctx.bounds).includes(p) || ValueBox(ctx.bounds).includes(p) ? this : nullptr;
}

void LayoutControlView::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const auto labelFont = Font(Weight::SemiBold).size(mLook.labelSize);
    const float labelWidth = std::min(b.width(), mLook.labelMaxWidth);
    const float cx = (b.left + b.right) * 0.5f;
    const auto box = ControlBox(b);

    if (mLook.labelTop)
    {
        FillText(cnv, Ellipsize(cnv, mLook.label, labelFont, labelWidth), labelFont, mLook.labelInk,
                 el::rect{cx - labelWidth * 0.5f, b.top, cx + labelWidth * 0.5f, b.top + LabelLine(mLook.labelSize)},
                 HAlign::Centre);
    }

    switch (mLook.kind)
    {
    case LayoutControlKind::Toggle:
        DrawToggle(cnv, box);
        break;
    case LayoutControlKind::Slider:
        DrawSlider(cnv, box);
        break;
    default:
        DrawKnob(cnv, box);
        break;
    }

    if (mLook.labelBottom)
    {
        FillText(cnv, Ellipsize(cnv, mLook.label, labelFont, labelWidth), labelFont, mLook.labelInk,
                 el::rect{cx - labelWidth * 0.5f, box.bottom + 4.0f, cx + labelWidth * 0.5f,
                          box.bottom + 4.0f + LabelLine(mLook.labelSize)},
                 HAlign::Centre);
    }

    if (mLook.style.showValue)
    {
        const auto valueFont = Font(Weight::SemiBold).size(mLook.valueSize);
        FillText(cnv, Ellipsize(cnv, FormatParamValue(mParam, mValue), valueFont, b.width()), valueFont, mLook.valueInk,
                 ValueBox(b), HAlign::Centre);
    }
}

void LayoutControlView::DrawKnob(artist::canvas& cnv, el::rect box)
{
    const float d = box.width();
    const float r = d * 0.5f;
    const el::point c{(box.left + box.right) * 0.5f, (box.top + box.bottom) * 0.5f};
    const auto& style = mLook.style;
    const float position = static_cast<float>(std::clamp(mPosition, 0.0, 1.0));

    // An image knob (a turning face or a filmstrip, with its base and glare) wins, then a custom
    // knob's turning face; either one whose face cannot be drawn is drawn as a styled knob.
    if (const auto* knobImage = UsableKnobImage(style))
    {
        if (const auto face = mImages.Find(knobImage->imageId))
        {
            if (const auto base = mImages.Find(knobImage->baseImageId))
            {
                DrawImageFitted(cnv, *base, box);
            }

            if (knobImage->mode == "filmstrip")
            {
                const int frame = FilmstripFrame(position, knobImage->frames);
                const auto cell = FilmstripCellOf(frame, knobImage->frames, knobImage->columns);
                const auto size = face->size();
                const float cw = size.x / static_cast<float>(cell.columns), ch = size.y / static_cast<float>(cell.rows);
                cnv.draw(*face, el::rect{cell.column * cw, cell.row * ch, (cell.column + 1) * cw, (cell.row + 1) * ch},
                         box);
            }
            else
            {
                auto state = cnv.new_state();
                const el::rect faceBox =
                    knobImage->clip ? el::rect{box.left + 4.0f, box.top + 4.0f, box.right - 4.0f, box.bottom - 4.0f}
                                    : box;

                if (knobImage->clip)
                {
                    cnv.begin_path();
                    cnv.add_circle(artist::circle(c, faceBox.width() * 0.5f));
                    cnv.clip();
                }

                cnv.translate(c.x, c.y);
                cnv.rotate(static_cast<float>(KnobFaceAngle(position, knobImage->minAngle, knobImage->maxAngle)) * kPi /
                           180.0f);
                const float half = faceBox.width() * 0.5f;
                DrawImageFitted(cnv, *face, el::rect{-half, -half, half, half});
            }

            if (const auto glare = mImages.Find(knobImage->glareImageId))
            {
                DrawImageFitted(cnv, *glare, box);
            }

            return;
        }
    }

    if (style.knobStyle == "custom" && !style.knobImageId.empty() && !UsableKnobImage(style))
    {
        if (const auto face = mImages.Find(style.knobImageId))
        {
            auto state = cnv.new_state();
            const float half = r - 4.0f;
            cnv.begin_path();
            cnv.add_circle(artist::circle(c, half));
            cnv.clip();
            cnv.translate(c.x, c.y);
            cnv.rotate((position * 270.0f - 135.0f) * kPi / 180.0f);
            DrawImageFitted(cnv, *face, el::rect{-half, -half, half, half});
            return;
        }
    }

    // A styled knob (custom-layouts.css), its pointer at the position on a 270 degree sweep.
    const float angle = (-135.0f + position * 270.0f - 90.0f) * kPi / 180.0f;
    const float ca = std::cos(angle), sa = std::sin(angle);
    const float k = d / 38.0f; // the styles are drawn for a 38 px knob
    auto pointer = [&](el::color ink, float width, float from, float to) {
        auto state = cnv.new_state();
        cnv.stroke_style(ink);
        cnv.line_width(width * k);
        cnv.line_cap(artist::canvas::round);
        cnv.begin_path();
        cnv.move_to(c.x + ca * from * k, c.y + sa * from * k);
        cnv.line_to(c.x + ca * to * k, c.y + sa * to * k);
        cnv.stroke();
    };
    auto body = [&](el::color fill, el::color edge, float edgeWidth) {
        cnv.fill_style(fill);
        cnv.begin_path();
        cnv.add_circle(artist::circle(c, r - edgeWidth * 0.5f));
        cnv.fill();
        cnv.stroke_style(edge);
        cnv.line_width(edgeWidth);
        cnv.begin_path();
        cnv.add_circle(artist::circle(c, r - edgeWidth * 0.5f));
        cnv.stroke();
    };

    if (style.knobStyle == "pedal")
    {
        body(el::rgba(200, 202, 210, 255), el::rgba(136, 136, 136, 255), 1.0f);
        pointer(el::rgba(51, 51, 51, 255), 2.0f, 6.0f, 13.0f);
        return;
    }

    if (style.knobStyle == "amp")
    {
        body(el::rgba(47, 48, 56, 255), el::rgba(102, 102, 102, 255), 1.0f);
        pointer(el::rgba(240, 192, 64, 255), 2.0f, 6.0f, 13.0f);
        return;
    }

    if (style.knobStyle == "minimal")
    {
        cnv.stroke_style(el::rgba(255, 255, 255, 77));
        cnv.line_width(2.0f * k);
        cnv.begin_path();
        cnv.add_circle(artist::circle(c, r - k));
        cnv.stroke();
        pointer(mLook.accent, 2.0f, 2.0f, 15.0f);
        return;
    }

    // The default: Nano Q's own knob face, a travel ring round a dark cap, at this size.
    const float ring = 3.0f * k;
    const float arcR = r - ring * 0.5f;
    {
        auto state = cnv.new_state();
        cnv.line_width(ring);
        cnv.stroke_style(mTheme.KnobRingTrack());
        cnv.begin_path();
        cnv.arc(c.x, c.y, arcR, 0.0f, 2.0f * kPi);
        cnv.stroke();

        if (position > 0.002f)
        {
            cnv.stroke_style(mTheme.KnobRingEnd());
            cnv.begin_path();
            cnv.arc(c.x, c.y, arcR, kPi * 0.75f, kPi * 0.75f + kPi * 1.5f * position);
            cnv.stroke();
        }
    }
    const float capR = r - ring - 1.5f * k;
    {
        auto state = cnv.new_state();
        const bool light = mTheme.IsLight();
        artist::canvas::radial_gradient dome{el::point{c.x, c.y - capR * 0.4f}, 0.0f, c, capR};
        const auto top = light ? el::rgba(250, 250, 252, 255) : el::rgba(58, 58, 68, 255);
        dome.add_color_stop(0.0f, (mDragging || mHover) ? top.level(1.15f) : top);
        dome.add_color_stop(1.0f, light ? el::rgba(214, 216, 224, 255) : el::rgba(22, 22, 28, 255));
        cnv.fill_style(dome);
        cnv.begin_path();
        cnv.add_circle(artist::circle(c, capR));
        cnv.fill();
    }
    pointer(mTheme.IsLight() ? mTheme.Text() : el::rgba(240, 240, 248, 255), 2.0f, capR * 0.40f / k, capR * 0.85f / k);
}

void LayoutControlView::DrawToggle(artist::canvas& cnv, el::rect box)
{
    const bool on = mValue >= 0.5;
    const auto& style = mLook.style;

    // An image switch needs both of its pictures; with one missing it is the standard switch.
    if (!style.toggleOnImageId.empty() && !style.toggleOffImageId.empty())
    {
        const auto onImage = mImages.Find(style.toggleOnImageId);
        const auto offImage = mImages.Find(style.toggleOffImageId);

        if (onImage && offImage)
        {
            DrawImageFitted(cnv, on ? *onImage : *offImage, box);
            return;
        }
    }

    const el::rect track{box.left, box.top + (box.height() - 20.0f) * 0.5f, box.left + 36.0f,
                         box.top + (box.height() + 20.0f) * 0.5f};
    cnv.fill_style(on ? mTheme.Accent().level(mHover ? 1.05f : 0.92f) : mTheme.ToggleBg().level(mHover ? 1.15f : 1.0f));
    cnv.fill_round_rect(track, 10.0f);
    cnv.fill_style(el::rgba(255, 255, 255, 240));
    cnv.begin_path();
    cnv.add_circle(artist::circle(
        el::point{on ? track.right - 10.0f : track.left + 10.0f, (track.top + track.bottom) * 0.5f}, 7.0f));
    cnv.fill();
}

void LayoutControlView::DrawSlider(artist::canvas& cnv, el::rect box)
{
    const auto& style = mLook.style;
    const float position = static_cast<float>(std::clamp(mPosition, 0.0, 1.0));
    const float cy = (box.top + box.bottom) * 0.5f;
    const auto track = mImages.Find(style.sliderTrackImageId);
    const auto thumb = mImages.Find(style.sliderThumbImageId);

    if (!style.sliderTrackImageId.empty() || !style.sliderThumbImageId.empty())
    {
        // An image slider: a 16 px track, the picture stretched along it, and a 24 px thumb.
        const el::rect bar{box.left, cy - 8.0f, box.right, cy + 8.0f};

        if (track)
        {
            cnv.draw(*track, bar);
        }
        else
        {
            cnv.fill_style(el::rgba(255, 255, 255, 38));
            cnv.fill_rect(bar);
        }

        const float x = box.left + 12.0f + position * (box.width() - 24.0f);
        const el::rect knob{x - 12.0f, cy - 12.0f, x + 12.0f, cy + 12.0f};

        if (thumb)
        {
            DrawImageFitted(cnv, *thumb, knob);
        }
        else
        {
            cnv.fill_style(mLook.accent);
            cnv.fill_rect(knob);
        }

        return;
    }

    cnv.fill_style(mTheme.IsLight() ? el::rgba(0, 0, 0, 38) : el::rgba(255, 255, 255, 38));
    cnv.fill_round_rect(el::rect{box.left, cy - 2.0f, box.right, cy + 2.0f}, 2.0f);
    const float x = box.left + 7.0f + position * (box.width() - 14.0f);
    cnv.fill_style(mLook.accent.level(mHover || mDragging ? 1.1f : 1.0f));
    cnv.begin_path();
    cnv.add_circle(artist::circle(el::point{x, cy}, 7.0f));
    cnv.fill();
    cnv.stroke_style(el::rgba(255, 255, 255, 77));
    cnv.line_width(2.0f);
    cnv.begin_path();
    cnv.add_circle(artist::circle(el::point{x, cy}, 6.0f));
    cnv.stroke();
}

void LayoutControlView::SetPosition(double position, const el::context& ctx)
{
    const double value = SnapParamValue(mParam, ParamPositionToValue(mParam, std::clamp(position, 0.0, 1.0)));

    if (value != mValue)
    {
        mValue = value;

        if (mActions.setParam)
        {
            mActions.setParam(mParam, value);
        }
    }

    // A stepped control sits on its steps; a continuous one follows the pointer.
    mPosition = mParam.step > 0.0 || IsEnumParam(mParam) ? ParamValueToPosition(mParam, mValue)
                                                         : std::clamp(position, 0.0, 1.0);
    ctx.view.refresh(ctx);
}

bool LayoutControlView::click(const el::context& ctx, el::mouse_button btn)
{
    if (btn.state == el::mouse_button::right)
    {
        if (btn.down && mActions.menu)
        {
            mActions.menu(mParam, ToView(ctx, btn.pos));
        }

        return true;
    }

    if (btn.state != el::mouse_button::left)
    {
        return false;
    }

    const auto box = ControlBox(ctx.bounds);

    if (!btn.down)
    {
        const bool wasDragging = mDragging;
        mDragging = false;

        // A switch flips on the release over it, as a click on its checkbox does.
        if (mLook.kind == LayoutControlKind::Toggle && box.includes(btn.pos) && mActions.setParam)
        {
            // The checkbox sends 1 or 0, as the web UI's does.
            mValue = std::clamp(mValue >= 0.5 ? 0.0 : 1.0, mParam.minValue, mParam.maxValue);
            mActions.setParam(mParam, mValue);
        }

        if (wasDragging || mLook.kind == LayoutControlKind::Toggle)
        {
            ctx.view.refresh(ctx);
        }

        return true;
    }

    if (mLook.kind == LayoutControlKind::Toggle)
    {
        return true;
    }

    if (btn.num_clicks == 2)
    {
        mDragging = false;

        // A double click on the value types one in; anywhere else on the control resets it.
        if (ValueBox(ctx.bounds).includes(btn.pos) && !IsEnumParam(mParam) && mActions.editValue)
        {
            mActions.editValue(mParam);
        }
        else
        {
            SetPosition(ParamValueToPosition(mParam, mParam.defaultValue), ctx);
        }

        return true;
    }

    if (!box.includes(btn.pos))
    {
        return true;
    }

    mDragging = true;
    // In view pixels, so a knob drawn small on a scaled layout turns no faster than a big one,
    // as the web UI's knob measures the pointer on the screen.
    mDragStartY = ToView(ctx, btn.pos).y;
    mDragStartPosition = mPosition;

    // A slider's thumb jumps to where its track is pressed, as a range input's does.
    if (mLook.kind == LayoutControlKind::Slider)
    {
        const float inset =
            mLook.style.sliderThumbImageId.empty() && mLook.style.sliderTrackImageId.empty() ? 7.0f : 12.0f;
        SetPosition((btn.pos.x - box.left - inset) / std::max(1.0f, box.width() - 2.0f * inset), ctx);
    }

    return true;
}

void LayoutControlView::drag(const el::context& ctx, el::mouse_button btn)
{
    if (!mDragging)
    {
        return;
    }

    if (mLook.kind == LayoutControlKind::Slider)
    {
        const auto box = ControlBox(ctx.bounds);
        const float inset =
            mLook.style.sliderThumbImageId.empty() && mLook.style.sliderTrackImageId.empty() ? 7.0f : 12.0f;
        SetPosition((btn.pos.x - box.left - inset) / std::max(1.0f, box.width() - 2.0f * inset), ctx);
        return;
    }

    // Up turns it up; Shift slows the turn to a fifth, as ParamKnob does.
    const float slow = btn.modifiers & el::mod_shift ? 5.0f : 1.0f;
    SetPosition(mDragStartPosition + static_cast<double>(mDragStartY - ToView(ctx, btn.pos).y) / (kDragPixels * slow),
                ctx);
}

bool LayoutControlView::cursor(const el::context& ctx, el::point, el::cursor_tracking status)
{
    const bool hover = status != el::cursor_tracking::leaving;

    if (hover != mHover)
    {
        mHover = hover;
        ctx.view.refresh(ctx);
    }

    return false;
}

bool LayoutControlView::scroll(const el::context& ctx, el::point dir, el::point)
{
    if (dir.y == 0.0f || !mHover || !ParamKnob::sWheelAdjusts || mLook.kind == LayoutControlKind::Toggle)
    {
        return false;
    }

    // One label at a time for a list, else a fiftieth of the travel.
    if (IsEnumParam(mParam))
    {
        const double step = mParam.step > 0.0 ? mParam.step : 1.0;
        SetPosition(ParamValueToPosition(
                        mParam, std::clamp(mValue + (dir.y > 0.0f ? step : -step), mParam.minValue, mParam.maxValue)),
                    ctx);
    }
    else
    {
        SetPosition(mPosition + (dir.y > 0.0f ? 0.02 : -0.02), ctx);
    }

    return true;
}

bool LayoutControlView::Update(const guitarfx::GraphNode& node)
{
    if (mDragging)
    {
        return false;
    }

    const double value = NodeParamValue(node, mParam);

    if (value == mValue)
    {
        return false;
    }

    mValue = value;
    mPosition = ParamValueToPosition(mParam, value);
    return true;
}
} // namespace nanoq::ui
