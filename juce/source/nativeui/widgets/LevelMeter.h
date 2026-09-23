#pragma once

#include "nativeui/NanoContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace soundshed::nano
{
/// A peak meter with a 2 s peak hold and a clip light, as the web UI's segment meters
/// (core/ui/ts/vuMeter.ts): -48 to 0 dBFS, green to amber from -12, red from -3.
class LevelMeter final : public juce::Component
{
public:
    LevelMeter (NanoContext& context, const juce::String& label);

    void setLevel (double peakDb, bool clipped, double nowSeconds);
    void setVertical (bool isVertical) { vertical = isVertical; repaint(); }

    void paint (juce::Graphics& g) override;

private:
    [[nodiscard]] static float positionFor (double db);

    NanoContext& context;
    juce::String label;
    double peakDb = -120.0;
    double holdDb = -120.0;
    double holdUntil = 0.0;
    double clipUntil = 0.0;
    double now = 0.0;
    bool vertical = false;
};
} // namespace soundshed::nano
