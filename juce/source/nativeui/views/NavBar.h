#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/ShellActions.h"
#include "nativeui/widgets/IconButton.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace soundshed::nano
{
/// The top-level navigation: a rail down the left in landscape, a tab bar along the bottom in
/// portrait (the web UI's compact rail/stack). Chain, Effect, Rigs, Settings.
class NavBar final : public juce::Component
{
public:
    NavBar (NanoContext& context, ShellActions& actions);

    void setVertical (bool isVertical);
    void setCurrent (Page page);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    NanoContext& context;
    bool vertical = true;
    std::array<std::unique_ptr<IconButton>, 4> buttons;
    std::array<Page, 4> pages { Page::Chain, Page::Effect, Page::Presets, Page::Settings };
};
} // namespace soundshed::nano
