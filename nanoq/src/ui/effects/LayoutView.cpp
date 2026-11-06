// A custom effect layout drawn and played (see LayoutView.h). The web UI's layoutRenderer.ts
// builds it from positioned elements scaled by a CSS transform; here one canvas holds them, laid
// out at their design positions (PlacedLayer) and scaled as one piece (ScaledElement), so a
// control's drag, a click and a redraw all pass through the same transform.

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

namespace
{
constexpr float kCornerRadius = 8.0f;

/// Draws its subject at a scale (x and y apart, as a CSS scale() may be), from its top-left
/// corner, optionally clipped to a rounded rectangle of the subject's own size. Points and
/// bounds are mapped on the way in, so the subject handles events in its own coordinates and a
/// refresh it asks for lands where it is on screen (the view maps it through the canvas).
template <el::concepts::Element Subject> class ScaledElement : public el::proxy<Subject>
{
  public:
    using base_type = el::proxy<Subject>;

    ScaledElement(Subject subject, float sx, float sy, float radius)
        : base_type(std::move(subject)), mSx(sx), mSy(sy), mRadius(radius)
    {
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        auto l = this->subject().limits(ctx);
        l.min.x *= mSx;
        l.min.y *= mSy;
        l.max.x = std::min(l.max.x * mSx, el::full_extent);
        l.max.y = std::min(l.max.y * mSy, el::full_extent);
        return l;
    }

    void draw(const el::context& ctx) override
    {
        mDrawing = true;
        base_type::draw(ctx);
        mDrawing = false;
    }

    void prepare_subject(el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        mState.emplace(cnv);
        cnv.translate(b.left, b.top);
        cnv.scale(mSx, mSy);
        ctx.bounds = el::rect{0.0f, 0.0f, b.width() / mSx, b.height() / mSy};

        // Clipped only while drawing: the canvas an event is handled with is not drawing, and
        // Direct2D clips only between the begin and end of a draw.
        if (mRadius > 0.0f && mDrawing)
        {
            cnv.begin_path();
            cnv.add_round_rect(ctx.bounds, mRadius);
            cnv.clip();
        }
    }

    void prepare_subject(el::context& ctx, el::point& p) override
    {
        const auto b = ctx.bounds;
        p = el::point{(p.x - b.left) / mSx, (p.y - b.top) / mSy};
        prepare_subject(ctx);
    }

    void restore_subject(el::context& ctx) override
    {
        mState.reset();
    }

  private:
    float mSx, mSy, mRadius;
    bool mDrawing = false;
    // Pushed in prepare_subject and popped in restore_subject, so held in between.
    std::optional<el::canvas::state> mState;
};

Ptr Scaled(Ptr content, float sx, float sy, float radius)
{
    return el::share(ScaledElement(el::hold_any(std::move(content)), sx, sy, radius));
}

/// Children at fixed rectangles from its top-left corner, drawn in the order added: the layout's
/// absolute positioning.
class PlacedLayer final : public el::vector_composite<el::layer_element>
{
  public:
    PlacedLayer(float w, float h) : mW(w), mH(h)
    {
    }

    void Add(Ptr child, el::rect at)
    {
        push_back(std::move(child));
        mRects.push_back(at);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{mW, mH}, {mW, mH}};
    }

    el::rect bounds_of(const el::context& ctx, std::size_t index) const override
    {
        const auto r = mRects[index];
        return {ctx.bounds.left + r.left, ctx.bounds.top + r.top, ctx.bounds.left + r.right, ctx.bounds.top + r.bottom};
    }

  private:
    float mW, mH;
    std::vector<el::rect> mRects;
};

el::color ToColour(const LayoutColour& c, double opacity = 1.0)
{
    const double a = std::clamp(c.a * opacity, 0.0, 1.0);
    return el::rgba(c.r, c.g, c.b, static_cast<std::uint8_t>(std::lround(a * 255.0)));
}

/// The text inks a layout's container theme gives, or the app theme's when it sets none.
struct Inks
{
    el::color label, value, text;
};

Inks InksFor(const Theme& theme, const std::string& containerTheme)
{
    if (containerTheme == "light")
    {
        return {el::rgba(70, 74, 86, 255), el::rgba(96, 100, 112, 255), el::rgba(24, 26, 32, 255)};
    }

    if (containerTheme == "dark" || containerTheme == "classic")
    {
        return {el::rgba(201, 209, 225, 210), el::rgba(160, 168, 186, 230), el::rgba(238, 242, 255, 255)};
    }

    return {theme.Pick(el::rgba(201, 209, 225, 184), theme.TextSecondary()), theme.TextMuted(), theme.Text()};
}

void DrawImageInto(artist::canvas& cnv, const artist::image& image, el::rect box, const std::string& fit)
{
    if (fit == "cover")
    {
        DrawImageCovering(cnv, image, box);
    }
    else if (fit == "stretch")
    {
        cnv.draw(image, box);
    }
    else
    {
        DrawImageFitted(cnv, image, box);
    }
}

void DrawBackgroundImage(artist::canvas& cnv, const artist::image& image, const LayoutBackground& bg,
                         el::rect canvasBox)
{
    const auto size = image.size();

    if (size.x <= 0.0f || size.y <= 0.0f)
    {
        return;
    }

    const float w = canvasBox.width(), h = canvasBox.height();
    // CSS background-position: the top-left corner moved by the offsets, for every size mode.
    const float x = canvasBox.left + static_cast<float>(bg.offsetX);
    const float y = canvasBox.top + static_cast<float>(bg.offsetY);

    if (bg.size == "tile")
    {
        // Repeated both ways from the offset at the picture's own size, covering the canvas.
        const float startX = x - std::ceil((x - canvasBox.left) / size.x) * size.x;
        const float startY = y - std::ceil((y - canvasBox.top) / size.y) * size.y;

        for (float ty = startY; ty < canvasBox.bottom; ty += size.y)
        {
            for (float tx = startX; tx < canvasBox.right; tx += size.x)
            {
                cnv.draw(image, el::rect{tx, ty, tx + size.x, ty + size.y});
            }
        }

        return;
    }

    float dw = size.x, dh = size.y; // "auto": its own size, as CSS draws a size it cannot read

    if (bg.size == "cover" || bg.size == "contain")
    {
        const float s = bg.size == "cover" ? std::max(w / size.x, h / size.y) : std::min(w / size.x, h / size.y);
        dw = size.x * s;
        dh = size.y * s;
    }
    else if (bg.size == "stretch")
    {
        dw = w;
        dh = h;
    }
    else if (bg.size == "custom" && bg.scale)
    {
        // A percentage of the canvas's width, the height keeping the picture's aspect.
        dw = static_cast<float>(*bg.scale) * w;
        dh = dw * size.y / size.x;
    }

    cnv.draw(image, el::rect{x, y, x + dw, y + dh});
}

/// A rectangle overlay's paint: its fill (a hex colour takes the opacity, as colorWithAlpha
/// applies it), its picture, and its border inside the edge.
void DrawOverlay(artist::canvas& cnv, LayoutImages& images, const LayoutOverlay& overlay, el::rect r)
{
    const float radius = std::min(static_cast<float>(overlay.borderRadius), std::min(r.width(), r.height()) * 0.5f);
    auto state = cnv.new_state();
    // An unreadable colour falls back to black, which is hex, so it takes the opacity too.
    const auto parsed = ParseLayoutColour(overlay.backgroundColor);
    const auto first = overlay.backgroundColor.find_first_not_of(" \t");
    const bool hex = !parsed || (first != std::string::npos && overlay.backgroundColor[first] == '#');
    cnv.fill_style(ToColour(parsed.value_or(LayoutColour{}), hex ? overlay.backgroundOpacity : 1.0));
    cnv.begin_path();
    cnv.add_round_rect(r, radius);
    cnv.fill();

    if (!overlay.imageId.empty())
    {
        if (const auto image = images.Find(overlay.imageId))
        {
            auto clip = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(r, radius);
            cnv.clip();
            DrawImageInto(cnv, *image, r, overlay.imageFit);
        }
    }

    if (overlay.borderWidth > 0.0)
    {
        const float bw = static_cast<float>(overlay.borderWidth);
        cnv.stroke_style(ToColour(ParseLayoutColour(overlay.borderColor).value_or(LayoutColour{255, 255, 255, 1.0})));
        cnv.line_width(bw);
        cnv.begin_path();
        const el::rect inner{r.left + bw * 0.5f, r.top + bw * 0.5f, r.right - bw * 0.5f, r.bottom - bw * 0.5f};
        cnv.add_round_rect(inner, std::max(0.0f, radius - bw * 0.5f));
        cnv.stroke();
    }
}

/// The canvas under the controls: background layers, bottom first, and the rectangles that are
/// not bypass switches. It takes no events.
class LayoutBackdropView final : public el::element
{
  public:
    LayoutBackdropView(LayoutImages& images, std::shared_ptr<const EffectLayout> layout, bool bypassed)
        : mImages(images), mLayout(std::move(layout)), mBypassed(bypassed)
    {
    }

    el::element* hit_test(const el::context&, el::point, bool, bool) override
    {
        return nullptr;
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;

        for (const auto& bg : mLayout->backgrounds)
        {
            DrawLayer(cnv, bg, b);
        }

        for (const auto& overlay : mLayout->overlays)
        {
            if (!overlay.toggleBypassOnClick && LayoutOverlayVisible(overlay, mBypassed))
            {
                DrawOverlay(cnv, mImages, overlay, Place(b, overlay));
            }
        }
    }

    static el::rect Place(el::rect b, const LayoutOverlay& overlay)
    {
        const float x = b.left + std::round(static_cast<float>(overlay.position.x));
        const float y = b.top + std::round(static_cast<float>(overlay.position.y));
        return {x, y, x + std::round(static_cast<float>(overlay.size.width)),
                y + std::round(static_cast<float>(overlay.size.height))};
    }

  private:
    void DrawLayer(artist::canvas& cnv, const LayoutBackground& bg, el::rect b)
    {
        const double opacity = std::clamp(bg.opacity, 0.0, 1.0);
        auto state = cnv.new_state();

        if (bg.type == "image")
        {
            // Artist draws pictures without an opacity, so a faded picture is drawn whole and
            // one faded out entirely is left out.
            if (opacity <= 0.01)
            {
                return;
            }

            if (const auto image = mImages.Find(bg.value))
            {
                DrawBackgroundImage(cnv, *image, bg, b);
            }

            return;
        }

        const auto paint = ParseLayoutPaint(bg.value);

        if (paint.kind == LayoutPaint::Kind::Solid)
        {
            cnv.fill_style(ToColour(paint.colour, opacity));
            cnv.fill_rect(b);
        }
        else if (paint.kind == LayoutPaint::Kind::Linear && !paint.stops.empty())
        {
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            LinearGradientLine(paint.angleDegrees, b.width(), b.height(), x0, y0, x1, y1);
            artist::canvas::linear_gradient gradient{
                el::point{b.left + static_cast<float>(x0), b.top + static_cast<float>(y0)},
                el::point{b.left + static_cast<float>(x1), b.top + static_cast<float>(y1)}};

            for (const auto& stop : paint.stops)
            {
                gradient.add_color_stop(static_cast<float>(stop.offset), ToColour(stop.colour, opacity));
            }

            cnv.fill_style(gradient);
            cnv.fill_rect(b);
        }
    }

    LayoutImages& mImages;
    std::shared_ptr<const EffectLayout> mLayout;
    bool mBypassed;
};

/// The layout's own text, over the controls. It takes no events.
class LayoutTextView final : public el::element
{
  public:
    LayoutTextView(std::shared_ptr<const EffectLayout> layout, el::color ink) : mLayout(std::move(layout)), mInk(ink)
    {
    }

    el::element* hit_test(const el::context&, el::point, bool, bool) override
    {
        return nullptr;
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;

        for (const auto& label : mLayout->textLabels)
        {
            if (label.text.empty())
            {
                continue;
            }

            // A span shrinks to its text, so its alignment moves nothing: each starts at its position.
            std::string text = label.text;
            std::replace(text.begin(), text.end(), '\n', ' ');
            const float size = std::clamp(static_cast<float>(label.fontSize), 1.0f, 512.0f);
            const auto font = Font(label.bold ? Weight::SemiBold : Weight::Regular).size(size);
            const float x = b.left + std::round(static_cast<float>(label.position.x));
            const float y = b.top + std::round(static_cast<float>(label.position.y));
            const auto colour = ParseLayoutColour(label.color);
            FillText(cnv, text, font, colour ? ToColour(*colour) : mInk,
                     el::rect{x, y, std::max(b.right, x + 4096.0f), y + size * 1.25f}, HAlign::Left);
        }
    }

  private:
    std::shared_ptr<const EffectLayout> mLayout;
    el::color mInk;
};

/// A rectangle that switches the effect's bypass: always there to click, drawn only while its
/// visibility mode says so, over everything else.
class BypassHotspot final : public el::element
{
  public:
    BypassHotspot(LayoutImages& images, LayoutOverlay overlay, bool visible, std::function<void()> onClick)
        : mImages(images), mOverlay(std::move(overlay)), mVisible(visible), mOnClick(std::move(onClick))
    {
    }

    bool wants_control() const override
    {
        return true;
    }

    void draw(const el::context& ctx) override
    {
        if (mVisible)
        {
            DrawOverlay(ctx.canvas, mImages, mOverlay, ctx.bounds);
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (btn.down)
        {
            mDown = true;
        }
        else if (mDown && ctx.bounds.includes(btn.pos))
        {
            mDown = false;

            if (mOnClick)
            {
                mOnClick();
            }
        }

        return true;
    }

  private:
    LayoutImages& mImages;
    LayoutOverlay mOverlay;
    bool mVisible;
    std::function<void()> mOnClick;
    bool mDown = false;
};

/// The control itself, by kind: a knob's diameter, a switch's size, a slider's length and the
/// height its margins give it (layoutControlImages.ts).
el::extent ControlExtent(LayoutControlKind kind, const LayoutControlStyle& style, bool imageToggle, bool imageSlider)
{
    switch (kind)
    {
    case LayoutControlKind::Toggle:

        if (imageToggle)
        {
            const auto size = LayoutToggleSize(style);
            return {static_cast<float>(size.width), static_cast<float>(size.height)};
        }

        return {36.0f, 20.0f};
    case LayoutControlKind::Slider:
        return {static_cast<float>(LayoutSliderLength(style)), imageSlider ? 32.0f : 20.0f};
    default: {
        const float d = static_cast<float>(LayoutKnobDiameter(style));
        return {d, d};
    }
    }
}
} // namespace

// ── LayoutImages ───────────────────────────────────────────────────────────────────────

std::shared_ptr<artist::image> LayoutImages::Find(const std::string& imageId)
{
    if (imageId.empty())
    {
        return nullptr;
    }

    if (const auto it = mImages.find(imageId); it != mImages.end())
    {
        return it->second;
    }

    std::shared_ptr<artist::image> image;

    if (const auto file = mResolve ? mResolve(imageId) : std::nullopt)
    {
        try
        {
            image = std::make_shared<artist::image>(*file);

            if (image->size().x <= 0.0f || image->size().y <= 0.0f)
            {
                image.reset();
            }
        }
        catch (...)
        {
            image.reset(); // a format the backend cannot decode (an SVG) draws nothing
        }
    }

    mImages[imageId] = image;
    return image;
}

// ── Building ───────────────────────────────────────────────────────────────────────────

namespace
{
LayoutControlView::Look MakeLook(const Theme& theme, const EffectLayout& layout, const LayoutControl& control,
                                 const EffectParamInfo& param, const Inks& inks)
{
    LayoutControlView::Look look;
    look.kind = ClassifyLayoutControl(control, &param);
    look.style = control.style;
    look.label = LayoutControlLabel(control, &param);
    look.labelTop = !control.style.hideLabel && control.style.labelPosition == "top";
    look.labelBottom = !control.style.hideLabel && control.style.labelPosition == "bottom";
    look.labelSize = static_cast<float>(LayoutLabelFontSize(layout, control));
    look.valueSize = static_cast<float>(LayoutValueFontSize(layout, control));
    look.labelMaxWidth = static_cast<float>(LayoutLabelMaxWidth(layout));
    const auto labelColour = LayoutLabelColour(layout, control);
    const auto valueColour = LayoutValueColour(layout, control);
    look.labelInk = labelColour ? ToColour(*labelColour) : inks.label;
    look.valueInk = valueColour ? ToColour(*valueColour) : inks.value;
    look.accent = theme.Accent();

    const bool imageToggle = !control.style.toggleOnImageId.empty() && !control.style.toggleOffImageId.empty();
    const bool imageSlider = !control.style.sliderTrackImageId.empty() || !control.style.sliderThumbImageId.empty();
    look.control = ControlExtent(look.kind, control.style, imageToggle, imageSlider);

    // The wrapper: its own width when the layout gives one, else at least 80 and as wide as
    // what it holds (the web UI's column of centred items).
    if (control.size && control.size->width > 0.0)
    {
        look.width = std::round(static_cast<float>(control.size->width));
    }
    else
    {
        float content = look.control.x;

        if (look.labelTop || look.labelBottom)
        {
            content =
                std::max(content, std::min(look.labelMaxWidth,
                                           MeasureTextWidth(look.label, Font(Weight::SemiBold).size(look.labelSize))));
        }

        if (control.style.showValue)
        {
            content = std::max(content, MeasureTextWidth(FormatParamValue(param, param.maxValue),
                                                         Font(Weight::SemiBold).size(look.valueSize)));
        }

        look.width = std::max(80.0f, std::ceil(content));
    }

    return look;
}
} // namespace

LayoutViewParts BuildLayoutView(const Theme& theme, LayoutImages& images, const LayoutViewInput& input,
                                const LayoutActions& actions)
{
    LayoutViewParts parts;

    if (input.layout == nullptr || input.node == nullptr)
    {
        return parts;
    }

    const auto layout = std::make_shared<const EffectLayout>(*input.layout);
    const auto& node = *input.node;
    const float w = std::round(static_cast<float>(layout->dimensions.width));
    const float h = std::round(static_cast<float>(layout->dimensions.height));
    const auto inks = InksFor(theme, layout->containerTheme);
    const bool bypassed = !node.enabled;

    auto placed = std::make_shared<PlacedLayer>(w, h);
    placed->Add(std::make_shared<LayoutBackdropView>(images, layout, bypassed), el::rect{0.0f, 0.0f, w, h});

    if (layout->useDefaultControls)
    {
        // The standard controls on the backdrop, moved and scaled as one block.
        if (input.defaultControls)
        {
            const auto finite = [](double v) { return std::isfinite(v) && v > 0.0 ? static_cast<float>(v) : 1.0f; };
            const float sx = finite(layout->defaultControlsScale.x), sy = finite(layout->defaultControlsScale.y);
            const float x = std::round(static_cast<float>(layout->defaultControlsOffset.x));
            const float y = std::round(static_cast<float>(layout->defaultControlsOffset.y));
            placed->Add(Scaled(input.defaultControls, sx, sy, 0.0f),
                        el::rect{x, y, x + input.defaultControlsSize.x * sx, y + input.defaultControlsSize.y * sy});
        }
    }
    else
    {
        for (const auto& control : layout->controls)
        {
            const float x = std::round(static_cast<float>(control.position.x));
            const float y = std::round(static_cast<float>(control.position.y));

            if (IsLayoutResourceControl(control))
            {
                const auto slot = LayoutControlResourceSlot(control);

                if (!slot || !input.resourceSlot)
                {
                    continue;
                }

                // A slot as wide as the layout gives it, else to the layout's right edge.
                const float width = control.size && control.size->width > 0.0 ? static_cast<float>(control.size->width)
                                                                              : std::min(360.0f, w - x);
                const std::string label = control.labelOverride.empty() ? std::string() : control.labelOverride;

                if (auto row = input.resourceSlot(*slot, label, width))
                {
                    placed->Add(row, el::rect{x, y, x + width, y + 26.0f});
                }

                continue;
            }

            const auto* param = input.type != nullptr ? input.type->FindParam(control.paramKey) : nullptr;

            if (param == nullptr)
            {
                continue; // a parameter this effect no longer has
            }

            auto view = std::make_shared<LayoutControlView>(theme, images, *param,
                                                            MakeLook(theme, *layout, control, *param, inks), actions);
            view->Update(node);
            const auto size = view->Size();
            placed->Add(view, el::rect{x, y, x + size.x, y + size.y});
            parts.controls.push_back(view);
        }
    }

    placed->Add(std::make_shared<LayoutTextView>(layout, inks.text), el::rect{0.0f, 0.0f, w, h});

    for (const auto& overlay : layout->overlays)
    {
        if (overlay.toggleBypassOnClick)
        {
            placed->Add(std::make_shared<BypassHotspot>(images, overlay, LayoutOverlayVisible(overlay, bypassed),
                                                        actions.toggleBypass),
                        LayoutBackdropView::Place(el::rect{0.0f, 0.0f, w, h}, overlay));
        }
    }

    const float scale = static_cast<float>(input.scale > 0.0 ? input.scale : 1.0);
    parts.element = Scaled(placed, scale, scale, kCornerRadius);
    parts.size = {w * scale, h * scale};
    return parts;
}
} // namespace nanoq::ui
