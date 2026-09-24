#include "nativeui/widgets/Sheet.h"

namespace soundshed::nano
{
namespace
{
constexpr int kTitleHeight = 48;
constexpr double kToastSeconds = 3.5;
} // namespace

Sheet::Sheet (NanoContext& contextIn, const juce::String& titleIn, std::unique_ptr<juce::Component> contentIn,
              juce::Point<int> preferredSizeIn)
    : context (contextIn),
      title (titleIn),
      content (std::move (contentIn)),
      preferredSize (preferredSizeIn),
      closeButton (contextIn, "sheet-close", "x")
{
    setComponentID ("sheet");
    setTitle (titleIn);
    setWantsKeyboardFocus (true);
    closeButton.setFlat (true);
    closeButton.setIconSize (NanoTheme::iconSmall);
    closeButton.setTooltip ("Close");
    closeButton.onClick = [this] { requestClose(); };
    addAndMakeVisible (*content);
    addAndMakeVisible (closeButton);
}

Sheet::~Sheet() = default;

void Sheet::requestClose()
{
    if (onClose)
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Sheet> (this)] {
            if (safe != nullptr && safe->onClose)
                safe->onClose();
        });
}

juce::Rectangle<int> Sheet::panelBounds() const
{
    auto area = getLocalBounds();

    // Small screens: the sheet is the screen, less a sliver that says "tap out to close".
    if (area.getWidth() <= preferredSize.x + 32 || area.getHeight() <= preferredSize.y + 32)
        return area.reduced (juce::jmin (12, area.getWidth() / 40), juce::jmin (12, area.getHeight() / 40));

    return juce::Rectangle<int> (preferredSize.x, preferredSize.y).withCentre (area.getCentre());
}

void Sheet::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.45f));

    const auto panel = panelBounds().toFloat();
    g.setColour (context.theme.elevated());
    g.fillRoundedRectangle (panel, 12.0f);
    g.setColour (context.theme.border());
    g.drawRoundedRectangle (panel, 12.0f, 1.0f);

    g.setColour (context.theme.text());
    g.setFont (context.font (NanoTheme::textHeading + 1.0f, FontWeight::semibold));
    g.drawText (title, panel.toNearestInt().removeFromTop (kTitleHeight).reduced (16, 0), juce::Justification::centredLeft);
}

void Sheet::resized()
{
    auto panel = panelBounds();
    auto header = panel.removeFromTop (kTitleHeight);
    closeButton.setBounds (header.removeFromRight (kTitleHeight).reduced (6));
    content->setBounds (panel.reduced (12, 4).withTrimmedBottom (8));
}

void Sheet::mouseUp (const juce::MouseEvent& e)
{
    if (! panelBounds().contains (e.getPosition()))
        requestClose();
}

bool Sheet::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        requestClose();
        return true;
    }

    return false;
}

ToastOverlay::ToastOverlay (NanoContext& contextIn) : context (contextIn)
{
    setComponentID ("toast");
    setInterceptsMouseClicks (false, false);
    setVisible (false);
}

void ToastOverlay::show (const juce::String& newTitle, const juce::String& newDetail, bool isError)
{
    title = newTitle;
    detail = newDetail;
    error = isError;
    hideAt = NanoContext::now() + kToastSeconds;
    setVisible (true);
    toFront (false);
    repaint();
    startTimerHz (10);
}

void ToastOverlay::timerCallback()
{
    if (NanoContext::now() >= hideAt)
    {
        stopTimer();
        setVisible (false);
    }
}

void ToastOverlay::paint (juce::Graphics& g)
{
    const auto& theme = context.theme;
    const auto bounds = getLocalBounds().toFloat().reduced (4.0f);
    g.setColour (theme.elevated().withAlpha (0.97f));
    g.fillRoundedRectangle (bounds, 10.0f);
    g.setColour (error ? theme.error() : theme.accent());
    g.fillRoundedRectangle (bounds.withWidth (5.0f), 2.5f);

    auto text = bounds.reduced (16.0f, 6.0f).toNearestInt();
    g.setColour (theme.text());
    g.setFont (context.font (NanoTheme::textBody, FontWeight::medium));

    if (detail.isEmpty())
    {
        g.drawFittedText (title, text, juce::Justification::centredLeft, 2);
        return;
    }

    g.drawFittedText (title, text.removeFromTop (text.getHeight() / 2), juce::Justification::bottomLeft, 1);
    g.setColour (theme.textSecondary());
    g.setFont (context.font (NanoTheme::textLabel));
    g.drawFittedText (detail, text, juce::Justification::topLeft, 1);
}
} // namespace soundshed::nano
