#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace soundshed::nano
{
/// A scrolling list of rows drawn by the owner, for touch first: a tap picks a row, a long
/// press (or a right click) opens the row's menu, and a drag scrolls. Rows are painted rather
/// than being components, so a library of hundreds of presets costs nothing to show.
class TapList final : public juce::Component
{
public:
    explicit TapList (const juce::String& componentId);
    ~TapList() override;

    std::function<int()> rowCount;
    std::function<void (juce::Graphics&, int row, juce::Rectangle<int> bounds, bool pressed)> paintRow;
    std::function<void (int row)> onTap;
    std::function<void (int row, juce::Rectangle<int> screenBounds)> onLongPress;

    /// Names a row for the debug server ("fx:Heavy American"), so a test can tap it by name.
    std::function<juce::String (int row)> rowName;

    void setRowHeight (int height);
    void updateContent();

    /// Scrolls so the row is in view.
    void scrollToRow (int row);

    void resized() override;

private:
    class Rows;

    juce::Viewport viewport;
    std::unique_ptr<Rows> rows;
    int rowHeight = 48;
};
} // namespace soundshed::nano
