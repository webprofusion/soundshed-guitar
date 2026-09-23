#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/widgets/IconButton.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace soundshed::nano
{
/// A panel over the page: the tuner, the metronome, the input/output controls, the effect
/// picker, a prompt. Full height on small screens, a centred card on large ones; a tap
/// outside it, its close button or Escape closes it. One at a time (NanoShell owns it).
class Sheet final : public juce::Component
{
public:
    Sheet (NanoContext& context, const juce::String& title, std::unique_ptr<juce::Component> content,
           juce::Point<int> preferredSize);
    ~Sheet() override;

    /// Called when the sheet asks to close; the owner destroys it.
    std::function<void()> onClose;

    [[nodiscard]] juce::Component& getContent() noexcept { return *content; }
    void requestClose();

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    [[nodiscard]] juce::Rectangle<int> panelBounds() const;

    NanoContext& context;
    juce::String title;
    std::unique_ptr<juce::Component> content;
    juce::Point<int> preferredSize;
    IconButton closeButton;
};

/// A message that shows for a few seconds at the bottom of the window and fades.
class ToastOverlay final : public juce::Component,
                           private juce::Timer
{
public:
    explicit ToastOverlay (NanoContext& context);

    void show (const juce::String& title, const juce::String& detail, bool isError);

    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override;

    NanoContext& context;
    juce::String title;
    juce::String detail;
    bool error = false;
    double hideAt = 0.0;
};
} // namespace soundshed::nano
