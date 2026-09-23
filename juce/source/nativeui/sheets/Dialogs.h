#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/widgets/IconButton.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace soundshed::nano
{
/// A line of text, OK and Cancel: the content of a prompt sheet.
class PromptContent final : public juce::Component
{
public:
    PromptContent (NanoContext& context, const juce::String& initial, std::function<void (const juce::String&)> onOk,
                   std::function<void()> onDone);

    void resized() override;
    void visibilityChanged() override;

private:
    NanoContext& context;
    juce::TextEditor editor;
    IconButton okButton;
    IconButton cancelButton;
};

/// A message and a confirm button: the content of a confirm sheet.
class ConfirmContent final : public juce::Component
{
public:
    ConfirmContent (NanoContext& context, const juce::String& message, const juce::String& confirmText,
                    std::function<void()> onConfirm, std::function<void()> onDone);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    NanoContext& context;
    juce::String message;
    IconButton confirmButton;
    IconButton cancelButton;
};
} // namespace soundshed::nano
