#include "nativeui/sheets/ToneSheets.h"

#include "nativeui/views/PresetsPage.h"

namespace soundshed::nano
{
namespace tones = guitarfx::uiclient::tones;

namespace
{
juce::String utf8 (const std::string& text)
{
    return juce::String::fromUTF8 (text.c_str());
}

int buttonHeight (const NanoContext& context)
{
    return context.touch ? 44 : 36;
}
} // namespace

void copyToneLink (NanoContext& context, const tones::Tone& tone)
{
    juce::SystemClipboard::copyTextToClipboard (utf8 (tones::ShareLink (tone)));

    if (context.showToast)
        context.showToast ("Link copied", "Anyone with Soundshed Guitar can open it", false);
}

// ── A tone ──────────────────────────────────────────────────────────────────────

ToneDetailContent::ToneDetailContent (NanoContext& contextIn, ShellActions& actionsIn, tones::Tone toneIn)
    : context (contextIn),
      actions (actionsIn),
      service (*contextIn.tones),
      tone (std::move (toneIn)),
      primaryButton (contextIn, "tone-primary", {}, "Install"),
      linkButton (contextIn, "tone-link", "link", "Copy link")
{
    setComponentID ("tone-detail");
    primaryButton.setPrimary (true);
    primaryButton.setClickingTogglesState (false);
    linkButton.setClickingTogglesState (false);
    primaryButton.onClick = [this] { onPrimary(); };
    linkButton.onClick = [this] { copyToneLink (context, tone); };
    addAndMakeVisible (primaryButton);
    addAndMakeVisible (linkButton);

    service.addListener (this);
    settingsSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Session, [this] { refreshButtons(); });
    installsSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Tones, [this] { refreshButtons(); });

    // A pack lists what is in it.
    if (tone.IsPack())
    {
        service.get (tones::PackPath (tone.id), [safe = juce::Component::SafePointer<ToneDetailContent> (this)] (const auto& response) {
            if (safe == nullptr)
                return;

            safe->pack = response.ok ? tones::ParsePackDetail (response.data) : std::nullopt;
            safe->packError = response.ok ? juce::String() : utf8 (response.error);

            if (safe->pack && safe->tone.description.empty())
                safe->tone.description = safe->pack->pack.description;

            safe->repaint();
        });
    }

    refreshButtons();
}

ToneDetailContent::~ToneDetailContent()
{
    service.removeListener (this);
}

std::string ToneDetailContent::installedPresetId() const
{
    const auto entry = tones::FindInstalled (context.state().appSettings, tones::EntryIdFor (tone));

    if (entry)
        for (const auto& id : entry->presetIds)
            if (context.state().FindPresetSummary (id) != nullptr)
                return id;

    return {};
}

void ToneDetailContent::refreshButtons()
{
    const auto entryId = tones::EntryIdFor (tone);
    const auto& installs = context.state().toneInstalls;
    const auto install = installs.find (entryId);
    const bool installing = service.isDownloading (entryId)
                            || (install != installs.end() && install->second.status == guitarfx::uiclient::ToneInstall::Status::Installing);

    if (installing)
        primaryButton.setText ("Installing...");
    else if (! installedPresetId().empty())
        primaryButton.setText (tone.IsPack() ? "Load first preset" : "Load");
    else
        primaryButton.setText ("Install");

    primaryButton.setEnabled (! installing);
    repaint();
}

void ToneDetailContent::onPrimary()
{
    if (const auto presetId = installedPresetId(); ! presetId.empty())
    {
        if (actions.closeSheet)
            actions.closeSheet();

        PresetsPage::loadWithConfirm (context, actions, presetId);
        return;
    }

    service.install (tone);
    refreshButtons();
}

void ToneDetailContent::paint (juce::Graphics& g)
{
    const auto& theme = context.theme;
    auto area = textArea;

    // The pack's picture, or an icon on a tile, beside who shared it.
    auto header = area.removeFromTop (72);
    const auto tile = header.removeFromLeft (72).toFloat();
    const auto picture = tone.thumbnailUrl.empty() ? juce::Image() : service.image (tone.thumbnailUrl);

    if (picture.isValid())
    {
        juce::Graphics::ScopedSaveState save (g);
        juce::Path clip;
        clip.addRoundedRectangle (tile, (float) NanoTheme::radius);
        g.reduceClipRegion (clip);
        g.drawImage (picture, tile, juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
    }
    else
    {
        g.setColour (theme.card().overlaidWith (theme.selectedFill()));
        g.fillRoundedRectangle (tile, (float) NanoTheme::radius);
        context.icons->draw (g, tone.IsPack() ? "package" : "note", tile.withSizeKeepingCentre (30.0f, 30.0f), theme.textSecondary());
    }

    header.removeFromLeft (14);
    juce::String creator = tone.creatorName.empty() ? utf8 (tone.creatorHandle) : utf8 (tone.creatorName);

    if (! tone.creatorName.empty() && ! tone.creatorHandle.empty())
        creator << "  " << utf8 (tone.creatorHandle);

    juce::StringArray facts;
    facts.add (tone.IsPack() ? "Pack" : "Preset");

    if (pack)
        facts.add (juce::String ((int) pack->items.size()) + (pack->items.size() == 1 ? " preset" : " presets"));

    if (tone.downloads > 0)
        facts.add (utf8 (tones::FormatCount (tone.downloads)) + (tone.downloads == 1 ? " download" : " downloads"));

    auto lines = header.withSizeKeepingCentre (header.getWidth(), 54);
    g.setColour (theme.text());
    g.setFont (context.font (NanoTheme::textBody, FontWeight::medium));
    g.drawFittedText (creator.isEmpty() ? juce::String ("Shared on Soundshed") : "by " + creator, lines.removeFromTop (20),
                      juce::Justification::centredLeft, 1);
    g.setColour (theme.textMuted());
    g.setFont (context.font (NanoTheme::textCaption));
    g.drawFittedText (facts.joinIntoString (juce::String::fromUTF8 ("  \xc2\xb7  ")), lines.removeFromTop (17),
                      juce::Justification::centredLeft, 1);

    if (! tone.tags.empty())
    {
        juce::StringArray tags;

        for (const auto& tag : tone.tags)
            tags.add (utf8 (tag));

        g.drawFittedText (tags.joinIntoString (", "), lines, juce::Justification::centredLeft, 1);
    }

    area.removeFromTop (14);

    // What it says about itself, then what a pack holds.
    if (! tone.description.empty())
    {
        g.setColour (theme.textSecondary());
        g.setFont (context.font (NanoTheme::textBody));
        const int lineCount = tone.IsPack() ? 3 : 7;
        const auto block = area.removeFromTop (juce::jmin (area.getHeight(), lineCount * 19));
        g.drawFittedText (utf8 (tone.description), block, juce::Justification::topLeft, lineCount);
        area.removeFromTop (10);
    }

    if (! tone.IsPack() || area.getHeight() < 30)
        return;

    g.setColour (theme.textMuted());
    g.setFont (context.font (NanoTheme::textOverline, FontWeight::semibold).withExtraKerningFactor (0.06f));
    g.drawText ("IN THIS PACK", area.removeFromTop (20), juce::Justification::centredLeft);
    g.setFont (context.font (NanoTheme::textBody));

    if (! pack)
    {
        g.setColour (packError.isEmpty() ? theme.textMuted() : theme.error());
        g.drawText (packError.isEmpty() ? juce::String ("Loading...") : packError, area.removeFromTop (22), juce::Justification::centredLeft);
        return;
    }

    const int perLine = 22;
    const int fits = juce::jmax (1, area.getHeight() / perLine);

    for (std::size_t i = 0; i < pack->items.size() && (int) i < fits; ++i)
    {
        const bool last = (int) i == fits - 1 && pack->items.size() > (std::size_t) fits;
        g.setColour (last ? theme.textMuted() : theme.text());
        g.drawFittedText (last ? "and " + juce::String ((int) (pack->items.size() - i)) + " more"
                               : utf8 (pack->items[i].title.empty() ? pack->items[i].itemId : pack->items[i].title),
                          area.removeFromTop (perLine), juce::Justification::centredLeft, 1);
    }
}

void ToneDetailContent::resized()
{
    auto area = getLocalBounds().reduced (6, 4);
    auto buttons = area.removeFromBottom (buttonHeight (context));
    primaryButton.setBounds (buttons.removeFromRight (juce::jmin (180, buttons.getWidth() / 2)).reduced (2));
    buttons.removeFromRight (6);
    linkButton.setBounds (buttons.removeFromRight (juce::jmin (150, buttons.getWidth())).reduced (2));
    area.removeFromBottom (10);
    textArea = area;
}

// ── Signing in ──────────────────────────────────────────────────────────────────

SignInContent::SignInContent (NanoContext& contextIn, ShellActions& actionsIn)
    : context (contextIn),
      actions (actionsIn),
      service (*contextIn.tones),
      sendButton (contextIn, "signin-send", {}, "Send code"),
      signInButton (contextIn, "signin-verify", {}, "Sign in")
{
    setComponentID ("sign-in");

    for (auto* field : { &emailField, &codeField })
    {
        field->setFont (context.font (NanoTheme::textHeading));
        field->setIndents (12, 0);
        field->setJustification (juce::Justification::centredLeft);
    }

    emailField.setComponentID ("signin-email");
    emailField.setTextToShowWhenEmpty ("you@example.com", context.theme.textMuted());
    emailField.setKeyboardType (juce::TextInputTarget::emailAddressKeyboard);
    emailField.onReturnKey = [this] { sendCode(); };

    codeField.setComponentID ("signin-code");
    codeField.setTextToShowWhenEmpty ("6-digit code", context.theme.textMuted());
    codeField.setInputRestrictions (6, "0123456789");
    codeField.setKeyboardType (juce::TextInputTarget::numericKeyboard);
    codeField.onReturnKey = [this] { signIn(); };

    sendButton.setClickingTogglesState (false);
    signInButton.setClickingTogglesState (false);
    signInButton.setPrimary (true);
    sendButton.onClick = [this] { sendCode(); };
    signInButton.onClick = [this] { signIn(); };

    addAndMakeVisible (emailField);
    addChildComponent (codeField);
    addAndMakeVisible (sendButton);
    addChildComponent (signInButton);
    setBusy (false);
}

void SignInContent::visibilityChanged()
{
    if (isShowing())
        (codeSent ? codeField : emailField).grabKeyboardFocus();
}

void SignInContent::setBusy (bool isBusy, const juce::String& message)
{
    busy = isBusy;
    status = message;
    statusIsError = false;
    sendButton.setPrimary (! codeSent);
    sendButton.setText (codeSent ? "Send again" : "Send code");
    sendButton.setEnabled (! busy);
    signInButton.setEnabled (! busy);
    signInButton.setVisible (codeSent);
    codeField.setVisible (codeSent);
    resized();
    repaint();
}

void SignInContent::sendCode()
{
    const auto typed = emailField.getText().trim();

    if (busy)
        return;

    if (! typed.containsChar ('@') || typed.length() < 5)
    {
        status = "Enter your email address.";
        statusIsError = true;
        repaint();
        return;
    }

    email = typed.toStdString();
    setBusy (true, "Sending a code...");

    service.requestCode (email, [safe = juce::Component::SafePointer<SignInContent> (this)] (const auto& response) {
        if (safe == nullptr)
            return;

        if (! response.ok)
        {
            safe->setBusy (false);
            safe->status = utf8 (response.error);
            safe->statusIsError = true;
            safe->repaint();
            return;
        }

        safe->codeSent = true;
        safe->setBusy (false, "We emailed you a code. It works for 15 minutes.");
        safe->codeField.grabKeyboardFocus();
    });
}

void SignInContent::signIn()
{
    const auto code = codeField.getText().trim();

    if (busy || ! codeSent)
        return;

    if (code.length() < 6)
    {
        status = "Enter the 6-digit code from the email.";
        statusIsError = true;
        repaint();
        return;
    }

    setBusy (true, "Signing in...");

    service.verifyCode (email, code.toStdString(), [safe = juce::Component::SafePointer<SignInContent> (this)] (const auto& response) {
        if (safe == nullptr)
            return;

        if (! response.ok)
        {
            safe->setBusy (false);
            safe->status = utf8 (response.error);
            safe->statusIsError = true;
            safe->repaint();
            return;
        }

        auto& context = safe->context;
        const auto name = safe->service.user() ? utf8 (safe->service.user()->Label()) : juce::String();

        if (safe->actions.closeSheet)
            safe->actions.closeSheet();

        if (context.showToast)
            context.showToast ("Signed in", name.isEmpty() ? juce::String() : "as " + name, false);
    });
}

void SignInContent::paint (juce::Graphics& g)
{
    const auto& theme = context.theme;
    auto area = getLocalBounds().reduced (6, 4);

    g.setColour (theme.textSecondary());
    g.setFont (context.font (NanoTheme::textBody));
    g.drawFittedText (codeSent ? "Type the code we sent to " + utf8 (email) + "."
                               : juce::String ("Sign in with your email address. There is no password: Soundshed emails you a code."),
                      area.removeFromTop (42), juce::Justification::topLeft, 2);

    if (status.isNotEmpty())
    {
        g.setColour (statusIsError ? theme.error() : theme.textMuted());
        g.setFont (context.font (NanoTheme::textCaption));
        auto statusArea = getLocalBounds().reduced (6, 4).withTrimmedBottom (buttonHeight (context) + 8);
        g.drawFittedText (status, statusArea.removeFromBottom (20), juce::Justification::centredLeft, 1);
    }
}

void SignInContent::resized()
{
    auto area = getLocalBounds().reduced (6, 4);
    area.removeFromTop (48);
    const int height = buttonHeight (context);

    emailField.setBounds (area.removeFromTop (height));
    area.removeFromTop (8);

    if (codeSent)
        codeField.setBounds (area.removeFromTop (height));

    auto buttons = getLocalBounds().reduced (6, 4).removeFromBottom (height);

    if (codeSent)
    {
        signInButton.setBounds (buttons.removeFromRight (140).reduced (2));
        buttons.removeFromRight (6);
    }

    sendButton.setBounds (buttons.removeFromRight (140).reduced (2));
}
} // namespace soundshed::nano
