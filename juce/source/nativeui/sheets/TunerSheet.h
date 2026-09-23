#pragma once

#include "nativeui/NanoContext.h"
#include "nativeui/widgets/IconButton.h"
#include "uiclient/Instruments.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace soundshed::nano
{
/// The tuner, full screen on a small display: the note, a needle over ±50 cents, the
/// neighbouring notes and the frequency, smoothed as the web UI smooths them
/// (uiclient::TunerModel). The tuner runs while this is open; "Mute while tuning" is the
/// engine's live mode off, and the reference pitch steps by 1 Hz.
class TunerContent final : public juce::Component
{
public:
    explicit TunerContent (NanoContext& context);
    ~TunerContent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void update();

    NanoContext& context;
    guitarfx::uiclient::TunerModel model;
    guitarfx::uiclient::TunerModel::Reading reading;
    juce::ToggleButton muteToggle { "Mute while tuning" };
    IconButton referenceDown;
    IconButton referenceUp;
    juce::Label referenceLabel;
    guitarfx::uiclient::Subscription tunerSubscription;
};
} // namespace soundshed::nano
