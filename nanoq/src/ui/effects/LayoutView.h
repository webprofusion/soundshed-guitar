#pragma once

// An effect drawn with its custom layout, as the web UI's layoutRenderer.ts draws one: a canvas of
// the layout's own size, scaled as one piece, with its backgrounds, rectangles, controls and text
// placed in design pixels from its top-left corner. Which layout, how big, and the arithmetic of
// its colours and image knobs are the shared client's (core/src/uiclient/EffectLayouts.h); this
// is the drawing and the gestures. Designing layouts stays in Soundshed Guitar.

#include "ui/Drawn.h"
#include "ui/widgets/Graphics.h"
#include "ui/widgets/Layout.h"
#include "ui/widgets/Theme.h"
#include "uiclient/ClientState.h"
#include "uiclient/EffectLayouts.h"

#include <elements.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nanoq::ui
{
/// The pictures layouts draw on, by image id, each loaded the first time it is drawn from the
/// file the engine resolves the id to. Cleared when the library or its image list changes.
class LayoutImages
{
  public:
    using Resolver = std::function<std::optional<std::filesystem::path>(const std::string& imageId)>;

    explicit LayoutImages(Resolver resolve) : mResolve(std::move(resolve))
    {
    }

    /// The picture, or null when the id names no file or the file cannot be decoded.
    [[nodiscard]] std::shared_ptr<artist::image> Find(const std::string& imageId);

    void Clear()
    {
        mImages.clear();
    }

  private:
    Resolver mResolve;
    std::map<std::string, std::shared_ptr<artist::image>> mImages; // a null entry: tried, failed
};

/// What a layout's controls do when used: the shell sends the requests.
struct LayoutActions
{
    std::function<void(const guitarfx::uiclient::EffectParamInfo& param, double value)> setParam;
    std::function<void(const guitarfx::uiclient::EffectParamInfo& param)> editValue;          // typed entry
    std::function<void(const guitarfx::uiclient::EffectParamInfo& param, el::point at)> menu; // MIDI Learn
    std::function<void()> toggleBypass;
};

/// One placed control: a parameter's knob, switch or slider with its label and value.
class LayoutControlView final : public el::element, public Drawn
{
  public:
    struct Look
    {
        guitarfx::uiclient::LayoutControlKind kind = guitarfx::uiclient::LayoutControlKind::Knob;
        guitarfx::uiclient::LayoutControlStyle style;
        std::string label;
        bool labelTop = false, labelBottom = false;
        float labelSize = 10.0f, valueSize = 9.0f, labelMaxWidth = 80.0f;
        el::color labelInk, valueInk, accent;
        float width = 80.0f;              // the wrapper's
        el::extent control{38.0f, 38.0f}; // the knob, switch or slider itself
    };

    LayoutControlView(const Theme& theme, LayoutImages& images, guitarfx::uiclient::EffectParamInfo param, Look look,
                      const LayoutActions& actions);

    /// The wrapper's size: label, control, label, value, each centred.
    [[nodiscard]] el::extent Size() const;

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    el::element* hit_test(const el::context& ctx, el::point p, bool leaf, bool control) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;
    void drag(const el::context& ctx, el::mouse_button btn) override;
    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override;
    bool scroll(const el::context& ctx, el::point dir, el::point p) override;

    bool wants_control() const override
    {
        return true;
    }

    /// Moves the control to the node's value, unless it is being dragged. True when it changed.
    bool Update(const guitarfx::GraphNode& node);

    [[nodiscard]] bool Dragging() const
    {
        return mDragging;
    }

  private:
    [[nodiscard]] el::rect ControlBox(el::rect bounds) const;
    [[nodiscard]] el::rect ValueBox(el::rect bounds) const;
    void SetPosition(double position, const el::context& ctx);
    void DrawKnob(artist::canvas& cnv, el::rect box);
    void DrawToggle(artist::canvas& cnv, el::rect box);
    void DrawSlider(artist::canvas& cnv, el::rect box);

    const Theme& mTheme;
    LayoutImages& mImages;
    guitarfx::uiclient::EffectParamInfo mParam;
    Look mLook;
    LayoutActions mActions;
    double mValue = 0.0;
    double mPosition = 0.0;
    double mDragStartPosition = 0.0;
    float mDragStartY = 0.0f;
    bool mDragging = false;
    bool mHover = false;
};

/// What BuildLayoutView makes.
struct LayoutViewParts
{
    Ptr element;     // drawn size: the layout's at `scale`
    el::extent size; // that size
    std::vector<std::shared_ptr<LayoutControlView>> controls;
};

struct LayoutViewInput
{
    const guitarfx::uiclient::EffectLayout* layout = nullptr;
    const guitarfx::GraphNode* node = nullptr;
    const guitarfx::uiclient::EffectTypeInfo* type = nullptr;
    double scale = 1.0;
    /// A backdrop layout's standard controls, `defaultControlsSize` big at their own scale.
    Ptr defaultControls;
    el::extent defaultControlsSize{0.0f, 0.0f};
    /// A model or IR slot placed by the layout: the slot's row, `width` wide, or null to leave it out.
    std::function<Ptr(const guitarfx::uiclient::LayoutResourceSlot& slot, const std::string& label, float width)>
        resourceSlot;
};

[[nodiscard]] LayoutViewParts BuildLayoutView(const Theme& theme, LayoutImages& images, const LayoutViewInput& input,
                                              const LayoutActions& actions);
} // namespace nanoq::ui
