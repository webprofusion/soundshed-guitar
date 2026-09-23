#include "nativeui/widgets/LevelMeter.h"

namespace soundshed::nano
{
namespace
{
constexpr double kFloorDb = -48.0;
constexpr double kHoldSeconds = 2.0;
constexpr double kClipSeconds = 1.5;
} // namespace

LevelMeter::LevelMeter (NanoContext& contextIn, const juce::String& labelIn) : context (contextIn), label (labelIn)
{
    setInterceptsMouseClicks (false, false);
}

float LevelMeter::positionFor (double db)
{
    return (float) juce::jlimit (0.0, 1.0, (db - kFloorDb) / -kFloorDb);
}

void LevelMeter::setLevel (double newPeakDb, bool clipped, double nowSeconds)
{
    now = nowSeconds;
    const bool changed = std::abs (newPeakDb - peakDb) > 0.2;
    peakDb = newPeakDb;

    if (newPeakDb >= holdDb || nowSeconds >= holdUntil)
    {
        holdDb = newPeakDb;
        holdUntil = nowSeconds + kHoldSeconds;
    }

    if (clipped)
        clipUntil = nowSeconds + kClipSeconds;

    if (changed || clipped || nowSeconds >= holdUntil)
        repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    const auto& theme = context.theme;
    auto bounds = getLocalBounds().toFloat();

    if (label.isNotEmpty())
    {
        g.setColour (theme.textMuted());
        g.setFont (context.font (11.0f));

        if (vertical)
            g.drawText (label, bounds.removeFromBottom (14.0f), juce::Justification::centred);
        else
            g.drawText (label, bounds.removeFromLeft (26.0f), juce::Justification::centredLeft);
    }

    const bool clipping = now < clipUntil;
    auto clipLight = vertical ? bounds.removeFromTop (6.0f) : bounds.removeFromRight (6.0f);
    g.setColour (clipping ? theme.error() : theme.border());
    g.fillRoundedRectangle (clipLight.reduced (0.5f), 2.0f);

    auto track = vertical ? bounds.reduced (2.0f, 1.0f) : bounds.reduced (1.0f, 2.0f).withTrimmedRight (2.0f);
    g.setColour (NanoTheme::argb (theme.t().knobRingTrack));
    g.fillRoundedRectangle (track, 2.0f);

    const auto level = positionFor (peakDb);
    const auto colourFor = [&theme] (double db) {
        if (db >= -3.0)
            return theme.error();

        if (db >= -12.0)
            return juce::Colour (0xffe0b040);

        return theme.success();
    };

    if (level > 0.0f)
    {
        auto fill = vertical ? track.withTop (track.getBottom() - track.getHeight() * level)
                             : track.withWidth (track.getWidth() * level);
        g.setColour (colourFor (peakDb));
        g.fillRoundedRectangle (fill, 2.0f);
    }

    const auto hold = positionFor (holdDb);

    if (hold > 0.0f)
    {
        g.setColour (colourFor (holdDb).brighter (0.2f));

        if (vertical)
            g.fillRect (track.getX(), track.getBottom() - track.getHeight() * hold, track.getWidth(), 2.0f);
        else
            g.fillRect (track.getX() + track.getWidth() * hold - 2.0f, track.getY(), 2.0f, track.getHeight());
    }
}
} // namespace soundshed::nano
