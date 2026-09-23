#include "nativeui/widgets/TapList.h"

#include "nativeui/debug/NamedTargets.h"

namespace soundshed::nano
{
namespace
{
constexpr int kLongPressMs = 550;
} // namespace

class TapList::Rows final : public juce::Component,
                            public NamedTargets,
                            private juce::Timer
{
public:
    explicit Rows (TapList& ownerIn) : owner (ownerIn) {}

    [[nodiscard]] std::vector<std::pair<juce::String, juce::Rectangle<int>>> namedTargets() const override
    {
        std::vector<std::pair<juce::String, juce::Rectangle<int>>> targets;

        if (owner.rowName && owner.rowCount)
            for (int row = 0, count = owner.rowCount(); row < count; ++row)
                targets.emplace_back (owner.rowName (row), juce::Rectangle<int> (0, row * owner.rowHeight, getWidth(), owner.rowHeight));

        return targets;
    }

    void paint (juce::Graphics& g) override
    {
        if (! owner.paintRow || ! owner.rowCount)
            return;

        const auto clip = g.getClipBounds();
        const int count = owner.rowCount();
        const int first = juce::jmax (0, clip.getY() / owner.rowHeight);
        const int last = juce::jmin (count - 1, clip.getBottom() / owner.rowHeight);

        for (int row = first; row <= last; ++row)
            owner.paintRow (g, row, { 0, row * owner.rowHeight, getWidth(), owner.rowHeight }, row == pressedRow);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        pressedRow = rowAt (e.position.y);
        longPressed = false;

        if (pressedRow < 0)
            return;

        if (e.mods.isPopupMenu())
        {
            fireLongPress();
            return;
        }

        startTimer (kLongPressMs);
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() > 8 && pressedRow >= 0)
        {
            stopTimer();
            pressedRow = -1; // a drag scrolls; it is not a tap
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        stopTimer();
        const int row = pressedRow;
        pressedRow = -1;
        repaint();

        if (row < 0 || longPressed)
            return;

        // A press held long enough ends as a long press even if the timer was starved.
        if (e.getLengthOfMousePress() >= kLongPressMs && e.getDistanceFromDragStart() < 8)
        {
            pressedRow = row;
            fireLongPress();
            pressedRow = -1;
            return;
        }

        if (rowAt (e.position.y) == row && owner.onTap)
            owner.onTap (row);
    }

private:
    [[nodiscard]] int rowAt (float y) const
    {
        const int row = (int) (y / (float) owner.rowHeight);
        return owner.rowCount && row >= 0 && row < owner.rowCount() ? row : -1;
    }

    void timerCallback() override
    {
        stopTimer();

        if (pressedRow >= 0 && isMouseButtonDown())
            fireLongPress();
    }

    void fireLongPress()
    {
        longPressed = true;

        if (owner.onLongPress)
            owner.onLongPress (pressedRow, localAreaToGlobal (juce::Rectangle<int> (0, pressedRow * owner.rowHeight, getWidth(), owner.rowHeight)));
    }

    TapList& owner;
    int pressedRow = -1;
    bool longPressed = false;
};

TapList::TapList (const juce::String& componentId) : rows (std::make_unique<Rows> (*this))
{
    setComponentID (componentId);
    viewport.setViewedComponent (rows.get(), false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (6);
    viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all);
    addAndMakeVisible (viewport);
}

TapList::~TapList() = default;

void TapList::setRowHeight (int height)
{
    rowHeight = juce::jmax (20, height);
    updateContent();
}

void TapList::updateContent()
{
    const int count = rowCount ? rowCount() : 0;
    rows->setSize (juce::jmax (1, viewport.getMaximumVisibleWidth()), juce::jmax (1, count * rowHeight));
    rows->repaint();
}

void TapList::scrollToRow (int row)
{
    const auto view = viewport.getViewArea();
    const int top = row * rowHeight;

    if (top < view.getY() || top + rowHeight > view.getBottom())
        viewport.setViewPosition (0, juce::jmax (0, top - view.getHeight() / 2));
}

void TapList::resized()
{
    viewport.setBounds (getLocalBounds());
    updateContent();
}
} // namespace soundshed::nano
