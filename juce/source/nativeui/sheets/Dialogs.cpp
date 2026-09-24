#include "nativeui/sheets/Dialogs.h"

namespace soundshed::nano
{
PromptContent::PromptContent (NanoContext& contextIn, const juce::String& initial, std::function<void (const juce::String&)> onOk,
                              std::function<void()> onDone)
    : context (contextIn), okButton (contextIn, "prompt-ok", {}, "OK"), cancelButton (contextIn, "prompt-cancel", {}, "Cancel")
{
    editor.setComponentID ("prompt-text");
    editor.setText (initial, false);
    editor.setFont (context.font (NanoTheme::textHeading));
    editor.setIndents (12, 0);
    editor.setJustification (juce::Justification::centredLeft);
    editor.setSelectAllWhenFocused (true);
    okButton.setPrimary (true);
    okButton.setClickingTogglesState (false);

    const auto accept = [this, onOk, onDone] {
        const auto text = editor.getText().trim();

        if (text.isEmpty())
            return;

        onDone();
        onOk (text);
    };

    okButton.onClick = accept;
    editor.onReturnKey = accept;
    cancelButton.onClick = onDone;
    editor.onEscapeKey = onDone;

    addAndMakeVisible (editor);
    addAndMakeVisible (okButton);
    addAndMakeVisible (cancelButton);
}

void PromptContent::visibilityChanged()
{
    if (isShowing())
        editor.grabKeyboardFocus();
}

void PromptContent::resized()
{
    auto area = getLocalBounds();
    const int height = context.touch ? 48 : 38;
    editor.setBounds (area.removeFromTop (height));
    area.removeFromTop (12);
    auto buttons = area.removeFromTop (height);
    okButton.setBounds (buttons.removeFromRight (110).reduced (2));
    cancelButton.setBounds (buttons.removeFromRight (110).reduced (2));
}

ConfirmContent::ConfirmContent (NanoContext& contextIn, const juce::String& messageIn, const juce::String& confirmText,
                                std::function<void()> onConfirm, std::function<void()> onDone)
    : context (contextIn),
      message (messageIn),
      confirmButton (contextIn, "confirm-ok", {}, confirmText),
      cancelButton (contextIn, "confirm-cancel", {}, "Cancel")
{
    confirmButton.setPrimary (true);
    confirmButton.setClickingTogglesState (false);
    confirmButton.onClick = [onConfirm, onDone] {
        onDone();
        onConfirm();
    };
    cancelButton.onClick = onDone;
    addAndMakeVisible (confirmButton);
    addAndMakeVisible (cancelButton);
}

void ConfirmContent::paint (juce::Graphics& g)
{
    g.setColour (context.theme.textSecondary());
    g.setFont (context.font (NanoTheme::textBody));
    g.drawFittedText (message, getLocalBounds().withTrimmedBottom (context.touch ? 64 : 52), juce::Justification::topLeft, 5);
}

void ConfirmContent::resized()
{
    auto buttons = getLocalBounds().removeFromBottom (context.touch ? 48 : 38);
    confirmButton.setBounds (buttons.removeFromRight (160).reduced (2));
    cancelButton.setBounds (buttons.removeFromRight (110).reduced (2));
}
} // namespace soundshed::nano
