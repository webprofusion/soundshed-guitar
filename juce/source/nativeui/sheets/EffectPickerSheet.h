#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/widgets/IconButton.h"
#include "nativeui/widgets/TapList.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace soundshed::nano
{
/// The effect library: categories in the web UI's order (the shared presentation table), a
/// search across all of them, and the effects the web UI's FX library offers from the engine's
/// catalog - the same exclusions (the mixer, blends, experimental effects unless that feature
/// is on). A tap adds the effect after the node the picker was opened for.
class EffectPickerContent final : public juce::Component
{
public:
    EffectPickerContent (NanoContext& context, std::string afterNodeId, std::function<void (const std::string& effectType)> onPick);
    ~EffectPickerContent() override;

    void resized() override;

    /// The catalog entries the picker offers, whatever the category.
    [[nodiscard]] static std::vector<const guitarfx::uiclient::EffectTypeInfo*> offeredEffects (const NanoContext& context);

private:
    void rebuild();

    NanoContext& context;
    std::string afterNodeId;
    std::function<void (const std::string&)> onPick;
    std::string category;

    juce::TextEditor search;
    std::vector<std::unique_ptr<IconButton>> categoryChips;
    juce::Viewport chipViewport;
    juce::Component chipStrip;
    TapList list { "effect-list" };
    std::vector<const guitarfx::uiclient::EffectTypeInfo*> shown;
};
} // namespace soundshed::nano
