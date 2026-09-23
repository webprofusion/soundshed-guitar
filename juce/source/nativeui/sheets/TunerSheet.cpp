#include "nativeui/sheets/TunerSheet.h"

namespace soundshed::nano
{
TunerContent::TunerContent (NanoContext& contextIn)
    : context (contextIn),
      referenceDown (contextIn, "tuner-ref-down", "chevron-left"),
      referenceUp (contextIn, "tuner-ref-up", "chevron-right")
{
    setComponentID ("tuner");
    muteToggle.setComponentID ("tuner-mute");
    referenceLabel.setJustificationType (juce::Justification::centred);
    referenceLabel.setFont (context.font (15.0f));

    muteToggle.onClick = [this] { context.commands.SetTunerLiveMode (! muteToggle.getToggleState()); };
    referenceDown.onClick = [this] {
        context.commands.SetTunerReference (juce::jlimit (400.0, 480.0, std::round (context.state().tuner.referenceFrequency) - 1.0));
    };
    referenceUp.onClick = [this] {
        context.commands.SetTunerReference (juce::jlimit (400.0, 480.0, std::round (context.state().tuner.referenceFrequency) + 1.0));
    };

    addAndMakeVisible (muteToggle);
    addAndMakeVisible (referenceDown);
    addAndMakeVisible (referenceUp);
    addAndMakeVisible (referenceLabel);

    tunerSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Tuner, [this] { update(); });
    context.commands.SetTunerActive (true);
    update();
}

TunerContent::~TunerContent()
{
    context.commands.SetTunerActive (false);
}

void TunerContent::update()
{
    const auto& tuner = context.state().tuner;
    reading = model.Push (tuner.detected, tuner.noteName, tuner.octave, tuner.centOffset, tuner.frequency);
    muteToggle.setToggleState (! tuner.liveMode, juce::dontSendNotification);
    referenceLabel.setText ("A = " + juce::String (tuner.referenceFrequency, 0) + " Hz", juce::dontSendNotification);
    repaint();
}

void TunerContent::paint (juce::Graphics& g)
{
    const auto& theme = context.theme;
    auto area = getLocalBounds().withTrimmedBottom (context.touch ? 60 : 50).toFloat();
    const auto inTune = reading.detected && reading.inTune;
    const auto noteColour = ! reading.detected ? theme.textMuted() : (inTune ? juce::Colour (0xff40c080) : theme.accent());

    // The note, with its neighbours either side.
    auto noteArea = area.removeFromTop (area.getHeight() * 0.55f);
    const float noteSize = juce::jlimit (40.0f, 140.0f, noteArea.getHeight() * 0.8f);
    g.setColour (noteColour);
    g.setFont (context.font (noteSize));
    g.drawText (reading.detected ? juce::String (reading.note) : juce::String ("-"), noteArea, juce::Justification::centred);

    g.setColour (theme.textMuted());
    g.setFont (context.font (noteSize * 0.35f));
    const auto sideWidth = noteArea.getWidth() * 0.25f;
    g.drawText (reading.detected ? juce::String (reading.noteLeft) : "", noteArea.removeFromLeft (sideWidth), juce::Justification::centred);
    g.drawText (reading.detected ? juce::String (reading.noteRight) : "", noteArea.removeFromRight (sideWidth), juce::Justification::centred);

    // The needle: ±50 cents across the scale, the centre marked.
    auto scale = area.removeFromTop (juce::jmin (70.0f, area.getHeight() * 0.6f)).reduced (area.getWidth() * 0.06f, 12.0f);
    g.setColour (theme.border());
    g.fillRoundedRectangle (scale.withSizeKeepingCentre (scale.getWidth(), 6.0f), 3.0f);

    for (int cents = -50; cents <= 50; cents += 10)
    {
        const float x = scale.getX() + scale.getWidth() * (0.05f + 0.9f * (float) (cents + 50) / 100.0f);
        const float tick = cents == 0 ? scale.getHeight() : scale.getHeight() * 0.4f;
        g.setColour (cents == 0 ? theme.text() : theme.textMuted());
        g.fillRect (x - 1.0f, scale.getCentreY() - tick * 0.5f, 2.0f, tick);
    }

    if (reading.detected)
    {
        const float x = scale.getX() + scale.getWidth() * (float) reading.needle;
        g.setColour (noteColour);
        g.fillRoundedRectangle (juce::Rectangle<float> (8.0f, scale.getHeight()).withCentre ({ x, scale.getCentreY() }), 4.0f);
    }

    g.setColour (theme.textSecondary());
    g.setFont (context.font (16.0f));
    const auto detail = reading.detected ? juce::String (reading.cents >= 0 ? "+" : "") + juce::String (reading.cents, 1) + " cents   "
                                               + juce::String (reading.frequency, 1) + " Hz"
                                         : juce::String ("Play a string");
    g.drawText (detail, area, juce::Justification::centredTop);
}

void TunerContent::resized()
{
    auto bottom = getLocalBounds().removeFromBottom (context.touch ? 52 : 42);
    auto reference = bottom.removeFromRight (juce::jmin (260, bottom.getWidth() / 2));
    referenceDown.setBounds (reference.removeFromLeft (reference.getHeight()).reduced (2));
    referenceUp.setBounds (reference.removeFromRight (reference.getHeight()).reduced (2));
    referenceLabel.setBounds (reference);
    muteToggle.setBounds (bottom.reduced (2));
}
} // namespace soundshed::nano
