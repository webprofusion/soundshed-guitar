#include "nativeui/NanoContext.h"

#include "nativeui/tones/ToneSharingService.h"

#include "util/PathEncoding.h"

namespace soundshed::nano
{
namespace
{
juce::File findUiRoot (PluginProcessorAdapter& processor)
{
    const auto root = processor.GetBundledAssetsPath();

    if (! root.empty())
        return juce::File (juce::String::fromUTF8 (guitarfx::util::PathToUtf8 (root).c_str())).getChildFile ("ui");

    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("resources").getChildFile ("ui");
}
} // namespace

NanoContext::NanoContext (PluginProcessorAdapter& processorIn)
    : processor (processorIn),
      client ([this] (const std::string& json) { processor.handleWebMessage (juce::String::fromUTF8 (json.c_str(), (int) json.size())); }),
      uiRoot (findUiRoot (processorIn))
{
    presentation = guitarfx::uiclient::EffectPresentation::Load (
        guitarfx::util::PathFromUtf8 (uiRoot.getChildFile ("data").getChildFile ("effect-presentation.json").getFullPathName().toStdString()));
    lookAndFeel = std::make_unique<NanoLookAndFeel> (theme, uiRoot.getChildFile ("fonts"));
    icons = std::make_unique<IconCache> (uiRoot.getChildFile ("images").getChildFile ("icons"));
    tones = std::make_unique<ToneSharingService> (*this);

#if JUCE_ANDROID || JUCE_IOS
    touch = true;
#else
    touch = juce::Desktop::getInstance().getMainMouseSource().isTouch();
#endif
}

NanoContext::~NanoContext() = default;

double NanoContext::now()
{
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

void NanoContext::showMenu (juce::PopupMenu menu, juce::Component* target)
{
    const auto options = juce::PopupMenu::Options().withStandardItemHeight (touch ? 40 : 28);
    showMenu (std::move (menu), target != nullptr ? options.withTargetComponent (target) : options.withMousePosition());
}

void NanoContext::showMenu (juce::PopupMenu menu, juce::PopupMenu::Options options)
{
    // A menu without a target component would otherwise take JUCE's default look.
    menu.setLookAndFeel (lookAndFeel.get());
    lastMenu = menu;
    menu.showMenuAsync (options);
}
} // namespace soundshed::nano
