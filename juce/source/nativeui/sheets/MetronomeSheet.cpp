#include "nativeui/sheets/MetronomeSheet.h"

namespace soundshed::nano
{
namespace
{
/// The web UI's beat levels, in the order a tap cycles them.
char nextLevel (char level)
{
    switch (level)
    {
        case 'H': return 'M';
        case 'M': return 'L';
        case 'L': return 'S';
        default:  return 'H';
    }
}

juce::String levelName (char level)
{
    switch (level)
    {
        case 'H': return "Accent";
        case 'M': return "Medium";
        case 'L': return "Normal";
        default:  return "Silent";
    }
}
} // namespace

MetronomeContent::MetronomeContent (NanoContext& contextIn)
    : context (contextIn),
      slower (contextIn, "metronome-slower", "chevron-left"),
      faster (contextIn, "metronome-faster", "chevron-right"),
      tap (contextIn, "metronome-tap", {}, "TAP"),
      startStop (contextIn, "metronome-start", "play", "Start"),
      meterButton (contextIn, "metronome-meter", {}, "4/4"),
      subdivisionButton (contextIn, "metronome-subdivision", {}, "Quarter notes"),
      clickButton (contextIn, "metronome-click", {}, "Click")
{
    setComponentID ("metronome");
    startStop.setClickingTogglesState (false);
    bpmSlider.setComponentID ("metronome-bpm");
    bpmSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    bpmSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    bpmSlider.setRange (30.0, 300.0, 1.0);
    volumeSlider.setComponentID ("metronome-volume");
    volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    volumeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 24);
    volumeSlider.setRange (-60.0, 12.0, 0.5); // the engine's range (MetronomeSupport.h)
    volumeSlider.setTextValueSuffix (" dB");

    for (auto* component : std::initializer_list<juce::Component*> { &slower, &faster, &tap, &startStop, &bpmSlider, &meterButton,
                                                                      &subdivisionButton, &clickButton, &volumeSlider })
        addAndMakeVisible (component);

    const auto setBpm = [this] (double bpm) { context.commands.SetMetronomeBpm (guitarfx::uiclient::ClampBpm (bpm)); };
    slower.onClick = [this, setBpm] { setBpm (context.state().metronome.bpm - 1.0); };
    faster.onClick = [this, setBpm] { setBpm (context.state().metronome.bpm + 1.0); };
    bpmSlider.onDragEnd = [this, setBpm] { setBpm (bpmSlider.getValue()); };
    tap.onClick = [this, setBpm] {
        if (const auto bpm = tapTempo.Tap (NanoContext::now()))
            setBpm (*bpm);
    };
    startStop.onClick = [this] { context.commands.SetMetronomeEnabled (! context.state().metronome.enabled); };
    meterButton.onClick = [this] { showMeterMenu(); };
    subdivisionButton.onClick = [this] { showSubdivisionMenu(); };
    clickButton.onClick = [this] { showClickMenu(); };
    volumeSlider.onDragEnd = [this] { context.commands.SetMetronomeVolume (volumeSlider.getValue()); };

    stateSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::Metronome, [this] { update(); });
    beatSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::MetronomeBeat, [this] {
        const auto& metronome = context.state().metronome;

        for (int i = 0; i < (int) beats.size(); ++i)
            beats[(std::size_t) i]->setToggleState (i == metronome.beatIndex, juce::dontSendNotification);
    });
    update();
}

MetronomeContent::~MetronomeContent() = default;

void MetronomeContent::update()
{
    const auto& metronome = context.state().metronome;
    const bool editable = metronome.editable;

    if (! bpmSlider.isMouseButtonDown())
        bpmSlider.setValue (metronome.bpm, juce::dontSendNotification);

    if (! volumeSlider.isMouseButtonDown())
        volumeSlider.setValue (metronome.volumeDb, juce::dontSendNotification);

    for (auto* component : std::initializer_list<juce::Component*> { &slower, &faster, &tap, &bpmSlider })
        component->setEnabled (editable);

    startStop.setToggleState (metronome.enabled, juce::dontSendNotification);
    startStop.setIcon (metronome.enabled ? "stop" : "play");
    startStop.setText (metronome.enabled ? "Stop" : "Start");
    meterButton.setText (juce::String (metronome.timeSigNum) + "/" + juce::String (metronome.timeSigDen));

    juce::String subdivision = juce::String::fromUTF8 (metronome.subdivision.c_str());
    subdivisionButton.setText (subdivision.isEmpty() ? juce::String ("Subdivision") : subdivision);

    juce::String click = "Click";

    for (const auto& [id, label] : metronome.clickTypes)
        if (id == metronome.clickType)
            click = juce::String::fromUTF8 (label.c_str());

    clickButton.setText (click);
    rebuildBeats();
    repaint (bpmArea);
}

void MetronomeContent::rebuildBeats()
{
    const auto& metronome = context.state().metronome;
    const auto pattern = metronome.beatPattern;

    if ((int) beats.size() != (int) pattern.size())
    {
        beats.clear();

        for (std::size_t i = 0; i < pattern.size(); ++i)
        {
            auto beat = std::make_unique<IconButton> (context, "metronome-beat-" + juce::String ((int) i + 1));
            beat->setClickingTogglesState (false);
            beat->onClick = [this, i] {
                auto next = context.state().metronome.beatPattern;

                if (i < next.size())
                {
                    next[i] = nextLevel (next[i]);
                    context.commands.SetMetronomeBeatPattern (next);
                }
            };
            addAndMakeVisible (*beat);
            beats.push_back (std::move (beat));
        }

        resized();
    }

    for (std::size_t i = 0; i < beats.size(); ++i)
    {
        const char level = pattern[i];
        beats[i]->setText (juce::String ((int) i + 1) + (level == 'H' ? ">" : level == 'S' ? "-" : ""));
        beats[i]->setTooltip ("Beat " + juce::String ((int) i + 1) + ": " + levelName (level));
        beats[i]->setIconColour (level == 'S' ? std::optional<juce::Colour> (context.theme.textMuted()) : std::nullopt);
    }
}

void MetronomeContent::showMeterMenu()
{
    juce::PopupMenu menu;
    const auto& metronome = context.state().metronome;
    const std::pair<int, int> meters[] = { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 4 }, { 7, 4 },
                                           { 3, 8 }, { 5, 8 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 } };

    for (const auto& [num, den] : meters)
        menu.addItem (juce::String (num) + "/" + juce::String (den), true, metronome.timeSigNum == num && metronome.timeSigDen == den,
                      [this, num = num, den = den] { context.commands.SetMetronomeTimeSignature (num, den); });

    context.showMenu (menu, &meterButton);
}

void MetronomeContent::showSubdivisionMenu()
{
    juce::PopupMenu menu;
    const auto& metronome = context.state().metronome;

    for (const auto& [id, ticks] : metronome.subdivisions)
        menu.addItem (juce::String::fromUTF8 (id.c_str()), true, id == metronome.subdivision,
                      [this, id = id] { context.commands.SetMetronomeSubdivision (id); });

    context.showMenu (menu, &subdivisionButton);
}

void MetronomeContent::showClickMenu()
{
    juce::PopupMenu menu;
    const auto& metronome = context.state().metronome;

    for (const auto& [id, label] : metronome.clickTypes)
        menu.addItem (juce::String::fromUTF8 (label.c_str()), true, id == metronome.clickType,
                      [this, id = id] { context.commands.SetMetronomeClickType (id); });

    context.showMenu (menu, &clickButton);
}

void MetronomeContent::paint (juce::Graphics& g)
{
    const auto& metronome = context.state().metronome;
    g.setColour (context.theme.text());
    g.setFont (context.font (juce::jmin (56.0f, (float) bpmArea.getHeight() * 0.7f)));
    g.drawText (juce::String (metronome.bpm, metronome.bpm == std::round (metronome.bpm) ? 0 : 1), bpmArea,
                juce::Justification::centred);

    g.setColour (context.theme.textMuted());
    g.setFont (context.font (12.0f));
    g.drawText (metronome.editable ? "BPM" : "BPM (from the host)", bpmArea.withTrimmedTop (bpmArea.getHeight() - 16),
                juce::Justification::centred);
}

void MetronomeContent::resized()
{
    auto area = getLocalBounds();
    const int row = context.touch ? 48 : 38;

    auto top = area.removeFromTop (juce::jmin (90, area.getHeight() / 4));
    slower.setBounds (top.removeFromLeft (row).withSizeKeepingCentre (row, row));
    faster.setBounds (top.removeFromRight (row).withSizeKeepingCentre (row, row));
    bpmArea = top;

    bpmSlider.setBounds (area.removeFromTop (row).reduced (4, 0));
    area.removeFromTop (6);

    auto buttons = area.removeFromTop (row);
    const int half = buttons.getWidth() / 2;
    tap.setBounds (buttons.removeFromLeft (half).reduced (3));
    startStop.setBounds (buttons.reduced (3));
    area.removeFromTop (8);

    if (! beats.empty())
    {
        auto beatRow = area.removeFromTop (row);
        const int width = juce::jmin (64, beatRow.getWidth() / (int) beats.size());

        for (auto& beat : beats)
            beat->setBounds (beatRow.removeFromLeft (width).reduced (2));

        area.removeFromTop (8);
    }

    auto pickers = area.removeFromTop (row);
    const int third = pickers.getWidth() / 3;
    meterButton.setBounds (pickers.removeFromLeft (third).reduced (3));
    subdivisionButton.setBounds (pickers.removeFromLeft (third).reduced (3));
    clickButton.setBounds (pickers.reduced (3));
    area.removeFromTop (8);
    volumeSlider.setBounds (area.removeFromTop (row).reduced (4, 0));
}
} // namespace soundshed::nano
