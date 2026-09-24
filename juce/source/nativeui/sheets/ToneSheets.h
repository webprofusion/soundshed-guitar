#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/ShellActions.h"
#include "nativeui/tones/ToneSharingService.h"
#include "nativeui/widgets/IconButton.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>
#include <string>

namespace soundshed::nano
{
/// Copies a tone's share link (the one Soundshed Guitar's Share button copies) and says so.
void copyToneLink (NanoContext& context, const guitarfx::uiclient::tones::Tone& tone);

/// One shared preset or pack: who shared it, what it is, what a pack holds, and the button
/// that installs it - or, once installed, loads it.
class ToneDetailContent final : public juce::Component,
                                private ToneSharingService::Listener
{
public:
    ToneDetailContent (NanoContext& context, ShellActions& actions, guitarfx::uiclient::tones::Tone tone);
    ~ToneDetailContent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void refreshButtons();
    void onPrimary();
    [[nodiscard]] std::string installedPresetId() const;

    void imagesChanged() override { repaint(); }
    void downloadsChanged() override { refreshButtons(); }

    NanoContext& context;
    ShellActions& actions;
    ToneSharingService& service;
    guitarfx::uiclient::tones::Tone tone;
    std::optional<guitarfx::uiclient::tones::PackDetail> pack;
    juce::String packError;

    IconButton primaryButton;
    IconButton linkButton;
    juce::Rectangle<int> textArea;

    guitarfx::uiclient::Subscription settingsSubscription;
    guitarfx::uiclient::Subscription installsSubscription;
};

/// Signing in to Soundshed with an emailed code, as the web UI does: the email, then the code.
class SignInContent final : public juce::Component
{
public:
    SignInContent (NanoContext& context, ShellActions& actions);

    void paint (juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

private:
    void sendCode();
    void signIn();
    void setBusy (bool isBusy, const juce::String& message = {});

    NanoContext& context;
    ShellActions& actions;
    ToneSharingService& service;
    bool codeSent = false;
    bool busy = false;
    juce::String status;
    bool statusIsError = false;
    std::string email;

    juce::TextEditor emailField;
    juce::TextEditor codeField;
    IconButton sendButton;
    IconButton signInButton;
};
} // namespace soundshed::nano
