#include "nativeui/views/SceneStrip.h"

namespace soundshed::nano
{
SceneStrip::SceneStrip (NanoContext& contextIn, ShellActions& actionsIn)
    : context (contextIn), actions (actionsIn), addButton (contextIn, "scene-add", "plus")
{
    setComponentID ("scene-strip");
    addButton.setTooltip ("Add a scene");
    addButton.setFlat (true);
    addButton.onClick = [this] { context.commands.AddScene(); };
    addAndMakeVisible (addButton);
    presetSubscription = context.client.Subscribe (guitarfx::uiclient::Topic::ActivePreset, [this] { refresh(); });
    refresh();
}

SceneStrip::~SceneStrip() = default;

void SceneStrip::refresh()
{
    const auto& state = context.state();
    const auto scenes = state.activePreset ? state.activePreset->scenes : std::vector<guitarfx::PresetScene> {};
    const auto active = state.ActiveScene();
    const auto activeId = active != nullptr ? active->id : std::string {};

    // Rebuilt only when the scene list itself changes; otherwise just the highlight.
    bool same = tabs.size() == scenes.size();

    for (std::size_t i = 0; same && i < scenes.size(); ++i)
        same = tabs[i]->getComponentID() == juce::String ("scene:" + scenes[i].id)
               && tabs[i]->getTooltip() == juce::String::fromUTF8 (scenes[i].title.c_str());

    if (! same)
    {
        tabs.clear();

        for (const auto& scene : scenes)
        {
            auto tab = std::make_unique<IconButton> (context, juce::String ("scene:" + scene.id), juce::String(),
                                                     juce::String::fromUTF8 (scene.title.c_str()));
            tab->setTooltip (juce::String::fromUTF8 (scene.title.c_str()));
            const auto id = scene.id;
            tab->onClick = [this, id] { context.commands.SelectScene (id); };
            tab->onLongPress = [this, id, raw = tab.get()] { showSceneMenu (id, raw); };
            addAndMakeVisible (*tab);
            tabs.push_back (std::move (tab));
        }
    }

    for (std::size_t i = 0; i < tabs.size() && i < scenes.size(); ++i)
        tabs[i]->setToggleState (scenes[i].id == activeId, juce::dontSendNotification);

    addButton.setVisible (state.activePreset.has_value());
    resized();
}

void SceneStrip::showSceneMenu (const std::string& sceneId, juce::Component* target)
{
    const auto& state = context.state();
    const auto* scene = state.activePreset ? guitarfx::FindPresetScene (*state.activePreset, sceneId) : nullptr;

    if (scene == nullptr)
        return;

    const auto title = juce::String::fromUTF8 (scene->title.c_str());
    juce::PopupMenu menu;
    menu.addSectionHeader (title);
    menu.addItem ("Rename...", [this, sceneId, title] {
        if (actions.promptText)
            actions.promptText ("Rename scene", title, [this, sceneId] (const juce::String& newTitle) {
                context.commands.RenameScene (sceneId, newTitle.toStdString());
            });
    });
    menu.addItem ("Remove", state.activePreset->scenes.size() > 1, false, [this, sceneId, title] {
        if (actions.confirm)
            actions.confirm ("Remove scene", "Remove \"" + title + "\" from this preset?", "Remove",
                             [this, sceneId] { context.commands.RemoveScene (sceneId); });
    });
    context.showMenu (menu, target);
}

void SceneStrip::resized()
{
    auto area = getLocalBounds().reduced (4, 2);
    const int addWidth = juce::jmin (area.getHeight() + 8, 44);
    addButton.setBounds (area.removeFromRight (addWidth));

    if (tabs.empty())
        return;

    const int width = juce::jlimit (56, 140, area.getWidth() / (int) tabs.size());

    for (auto& tab : tabs)
        tab->setBounds (area.removeFromLeft (width).reduced (2, 0));
}
} // namespace soundshed::nano
