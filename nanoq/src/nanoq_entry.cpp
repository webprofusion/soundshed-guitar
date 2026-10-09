// The four functions QPlug asks every plugin for.

#include "NanoQController.h"
#include "NanoQPresenter.h"
#include "NanoQProcessor.h"

#include <q_plug/plugin.hpp>

namespace cycfi::q_plug
{
controller_ptr make_controller()
{
    return std::make_unique<nanoq::NanoQController>();
}

processor_ptr make_processor(controller& ctl)
{
    return std::make_unique<nanoq::NanoQProcessor>(static_cast<nanoq::NanoQController&>(ctl));
}

presenter_ptr make_presenter(controller& ctl)
{
    return std::make_unique<nanoq::NanoQPresenter>(static_cast<nanoq::NanoQController&>(ctl));
}

plugin_info const& info()
{
    static char const* const features[] = {"audio-effect", "stereo", nullptr};

    static plugin_info const i = {
        NANOQ_ID,
        NANOQ_NAME,
        "Soundshed",
        "https://soundshed.com",
        "",
        "",
        NANOQ_VERSION,
        "Soundshed Nano, without JUCE: the Soundshed engine hosted by QPlug and drawn with Elements.",
        features,
        {960, 600}, // the size the editor opens with
        1           // state version
    };
    return i;
}
} // namespace cycfi::q_plug
