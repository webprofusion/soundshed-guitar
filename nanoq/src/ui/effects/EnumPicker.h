#pragma once

// The enum picker (EnumPicker.cpp): the trigger an effect page shows in place of a knob for an enum
// with too many choices to turn through, and the pictures its choices are drawn with. The flyout
// itself is Shell::OpenEnumPicker.

#include "ui/widgets/Graphics.h"
#include "ui/widgets/Pressable.h"
#include "ui/widgets/Theme.h"

#include "uiclient/ClientState.h"

#include <string>

namespace nanoq::ui
{
/// A choice's picture by its labelInfo icon name (the web UI's pickerIcons.ts), 32 x 20 fitted
/// into `r`: the pickups' covers and poles; a dashed outline for a name it does not know.
void DrawPickerIcon(artist::canvas& cnv, el::rect r, const std::string& name, el::color colour);

/// The current choice (.enum-picker-trigger): its picture, its name over its kind, and a caret.
class EnumPickerTrigger final : public Pressable
{
  public:
    EnumPickerTrigger(const Theme& theme, Icons& icons, guitarfx::uiclient::EffectParamInfo info);

    /// The label index chosen, or -1.
    void Set(int index)
    {
        mIndex = index;
    }

    [[nodiscard]] AccessInfo Describe() const override;
    el::view_limits limits(const el::basic_context&) const override;
    void draw(const el::context& ctx) override;

  private:
    [[nodiscard]] std::string Label() const;

    const Theme& mTheme;
    Icons& mIcons;
    guitarfx::uiclient::EffectParamInfo mInfo;
    int mIndex = -1;
};
} // namespace nanoq::ui
