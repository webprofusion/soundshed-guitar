#include "nativeui/views/NavBar.h"

namespace soundshed::nano
{
NavBar::NavBar (NanoContext& contextIn, ShellActions& actions) : context (contextIn)
{
    setComponentID ("nav");
    setTitle ("Navigation");

    const std::array<std::array<const char*, 3>, 4> entries { { { "nav-chain", "link", "Chain" },
                                                                { "nav-effect", "pedal", "Effect" },
                                                                { "nav-tones", "note", "Tones" },
                                                                { "nav-settings", "gear", "Settings" } } };

    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        buttons[i] = std::make_unique<IconButton> (context, entries[i][0], entries[i][1], entries[i][2]);
        buttons[i]->setStacked (true);
        buttons[i]->setFlat (true);
        buttons[i]->setClickingTogglesState (false);
        const auto page = pages[i];
        buttons[i]->onClick = [&actions, page] {
            if (actions.showPage)
                actions.showPage (page);
        };
        addAndMakeVisible (*buttons[i]);
    }
}

void NavBar::setVertical (bool isVertical)
{
    vertical = isVertical;
    resized();
    repaint();
}

void NavBar::setCurrent (Page page)
{
    // The device page sits under Settings.
    const auto shown = page == Page::Device ? Page::Settings : page;

    for (std::size_t i = 0; i < pages.size(); ++i)
        buttons[i]->setToggleState (pages[i] == shown, juce::dontSendNotification);

    repaint();
}

void NavBar::paint (juce::Graphics& g)
{
    g.setColour (context.theme.bar());
    g.fillRect (getLocalBounds());
    g.setColour (context.theme.border());
    g.fillRect (vertical ? getLocalBounds().removeFromRight (1) : getLocalBounds().removeFromTop (1));

    // The current page: a short accent mark at the rail's edge (the tab bar's top), so the
    // button itself can stay a quiet neutral.
    for (const auto& button : buttons)
    {
        if (! button->getToggleState())
            continue;

        const auto b = button->getBounds().toFloat();
        const auto mark = vertical ? juce::Rectangle<float> (0.0f, b.getCentreY() - 12.0f, 3.0f, 24.0f)
                                   : juce::Rectangle<float> (b.getCentreX() - 14.0f, 1.0f, 28.0f, 3.0f);
        g.setColour (context.theme.accent());
        g.fillRoundedRectangle (mark, 1.5f);
    }
}

void NavBar::resized()
{
    auto area = getLocalBounds().reduced (4);

    if (vertical)
    {
        const int height = juce::jmin (64, area.getHeight() / (int) buttons.size());

        for (auto& button : buttons)
            button->setBounds (area.removeFromTop (height).reduced (0, 2));

        return;
    }

    const int width = area.getWidth() / (int) buttons.size();

    for (auto& button : buttons)
        button->setBounds (area.removeFromLeft (width).reduced (2, 0));
}
} // namespace soundshed::nano
