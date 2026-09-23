#include "nativeui/debug/NanoDebugServer.h"

#include "nativeui/NanoEditor.h"
#include "nativeui/debug/NamedTargets.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>

namespace soundshed::nano
{
namespace
{
juce::String typeNameOf (const juce::Component& component)
{
    juce::String name (typeid (component).name());
    return name.fromLastOccurrenceOf ("::", false, false).fromLastOccurrenceOf (" ", false, false);
}

nlohmann::json describe (const juce::Component& component, const juce::Component& root)
{
    nlohmann::json node;
    const auto bounds = root.getLocalArea (&component, component.getLocalBounds());
    node["id"] = component.getComponentID().toStdString();
    node["type"] = typeNameOf (component).toStdString();
    node["name"] = component.getName().toStdString();
    node["bounds"] = { bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight() };
    node["visible"] = component.isVisible();

    if (const auto* button = dynamic_cast<const juce::Button*> (&component))
    {
        node["text"] = button->getButtonText().toStdString();
        node["toggled"] = button->getToggleState();
        node["enabled"] = button->isEnabled();
    }
    else if (const auto* label = dynamic_cast<const juce::Label*> (&component))
    {
        node["text"] = label->getText().toStdString();
    }
    else if (const auto* slider = dynamic_cast<const juce::Slider*> (&component))
    {
        node["value"] = slider->getValue();
    }
    else if (const auto* editorText = dynamic_cast<const juce::TextEditor*> (&component))
    {
        node["text"] = editorText->getText().toStdString();
    }
    else if (const auto* combo = dynamic_cast<const juce::ComboBox*> (&component))
    {
        node["text"] = combo->getText().toStdString();
    }

    if (auto* described = dynamic_cast<juce::SettableTooltipClient*> (const_cast<juce::Component*> (&component)))
        if (described->getTooltip().isNotEmpty())
            node["tooltip"] = described->getTooltip().toStdString();

    nlohmann::json children = nlohmann::json::array();

    for (auto* child : component.getChildren())
        if (child->isVisible())
            children.push_back (describe (*child, root));

    if (const auto* named = dynamic_cast<const NamedTargets*> (&component))
    {
        for (const auto& [name, area] : named->namedTargets())
        {
            const auto targetBounds = root.getLocalArea (&component, area);
            children.push_back ({ { "id", name.toStdString() },
                                  { "type", "target" },
                                  { "bounds", { targetBounds.getX(), targetBounds.getY(), targetBounds.getWidth(), targetBounds.getHeight() } } });
        }
    }

    if (! children.empty())
        node["children"] = std::move (children);

    return node;
}

juce::Component* findById (juce::Component& component, const juce::String& id)
{
    if (component.getComponentID() == id && component.isShowing())
        return &component;

    for (auto* child : component.getChildren())
        if (auto* found = findById (*child, id))
            return found;

    return nullptr;
}

struct Target
{
    juce::Component* component = nullptr;
    juce::Point<float> position;
};

/// A child component with this id, or a named target one draws itself (NamedTargets).
std::optional<Target> findTarget (juce::Component& component, const juce::String& id)
{
    if (! component.isShowing())
        return {};

    if (component.getComponentID() == id)
        return Target { &component, component.getLocalBounds().getCentre().toFloat() };

    if (const auto* named = dynamic_cast<const NamedTargets*> (&component))
        for (const auto& [name, area] : named->namedTargets())
            if (name == id)
                return Target { &component, area.getCentre().toFloat() };

    for (auto* child : component.getChildren())
        if (auto found = findTarget (*child, id))
            return found;

    return {};
}

void clickAt (juce::Component& target, juce::Point<float> position, bool longPress)
{
    if (auto* button = dynamic_cast<juce::Button*> (&target); button != nullptr && ! longPress)
    {
        button->triggerClick();
        return;
    }

    auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto now = juce::Time::getCurrentTime();
    const auto downTime = longPress ? now - juce::RelativeTime::milliseconds (800) : now;
    const juce::MouseEvent down (source, position, juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 &target, &target, downTime, position, downTime, 1, false);
    const juce::MouseEvent up (source, position, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                               &target, &target, now, position, downTime, 1, false);
    target.mouseDown (down);
    target.mouseUp (up);
}

/// The open popup menu (a node menu, the more menu, a drop-down's list): a separate desktop
/// window, found through its accessibility role. Lists its items, and presses the one whose
/// text is `wanted` through the same accessibility action a screen reader uses.
nlohmann::json driveOpenMenu (const juce::String& wanted, bool& pressed)
{
    auto items = nlohmann::json::array();
    auto& desktop = juce::Desktop::getInstance();

    for (int i = desktop.getNumComponents(); --i >= 0;)
    {
        auto* window = desktop.getComponent (i);
        auto* handler = window != nullptr && window->isShowing() ? window->getAccessibilityHandler() : nullptr;

        if (handler == nullptr || handler->getRole() != juce::AccessibilityRole::popupMenu)
            continue;

        std::function<void (juce::Component&)> visit = [&] (juce::Component& component) {
            for (auto* child : component.getChildren())
            {
                if (auto* item = child->getAccessibilityHandler(); item != nullptr && item->getRole() == juce::AccessibilityRole::menuItem)
                {
                    const auto title = item->getTitle();
                    const bool enabled = item->getActions().contains (juce::AccessibilityActionType::press);
                    items.push_back ({ { "text", title.toStdString() }, { "enabled", enabled } });

                    if (! pressed && enabled && wanted.isNotEmpty() && title == wanted)
                        pressed = item->getActions().invoke (juce::AccessibilityActionType::press);
                }

                visit (*child);
            }
        };

        visit (*window);
        break;
    }

    return items;
}
} // namespace

NanoDebugServer::NanoDebugServer (NanoEditor& editorIn, int portIn)
    : juce::Thread ("Nano debug server"), editor (editorIn), port (portIn)
{
    listener = std::make_unique<juce::StreamingSocket>();

    if (listener->createListener (port, "127.0.0.1"))
        startThread();
    else
        DBG ("Nano debug server could not listen on port " << port);
}

NanoDebugServer::~NanoDebugServer()
{
    *alive = false;
    signalThreadShouldExit();

    if (listener != nullptr)
        listener->close();

    stopThread (2000);
}

void NanoDebugServer::run()
{
    while (! threadShouldExit())
    {
        std::unique_ptr<juce::StreamingSocket> connection (listener->waitForNextConnection());

        if (connection == nullptr)
            continue;

        juce::String buffer;

        while (! threadShouldExit() && connection->isConnected())
        {
            if (connection->waitUntilReady (true, 200) <= 0)
                continue;

            char chunk[4096];
            const int read = connection->read (chunk, sizeof (chunk) - 1, false);

            if (read <= 0)
                break;

            buffer += juce::String::fromUTF8 (chunk, read);

            while (buffer.containsChar ('\n'))
            {
                const auto line = buffer.upToFirstOccurrenceOf ("\n", false, false).trim();
                buffer = buffer.fromFirstOccurrenceOf ("\n", false, false);

                if (line.isEmpty())
                    continue;

                auto result = std::make_shared<juce::String>();
                auto done = std::make_shared<juce::WaitableEvent>();
                auto aliveFlag = alive;
                juce::MessageManager::callAsync ([this, line, result, done, aliveFlag] {
                    if (*aliveFlag)
                        *result = handleOnMessageThread (line);

                    done->signal();
                });

                if (! done->wait (10000))
                    *result = R"({"ok":false,"error":"timed out"})";

                const auto reply = *result + "\n";
                connection->write (reply.toRawUTF8(), (int) reply.getNumBytesAsUTF8());
            }
        }
    }
}

juce::String NanoDebugServer::handleOnMessageThread (const juce::String& line)
{
    nlohmann::json reply;
    reply["ok"] = true;

    try
    {
        const auto request = nlohmann::json::parse (line.toStdString());
        const auto cmd = request.value ("cmd", std::string {});
        auto& context = editor.getContext();

        if (cmd == "tree")
        {
            reply["tree"] = describe (editor, editor);
        }
        else if (cmd == "click" || cmd == "longpress")
        {
            const auto target = findTarget (editor, juce::String (request.value ("id", std::string {})));

            if (! target)
                throw std::runtime_error ("no visible component or target with that id");

            clickAt (*target->component, target->position, cmd == "longpress");
        }
        else if (cmd == "screenshot")
        {
            const auto file = juce::File (juce::String (request.value ("path", std::string {})));
            const auto scale = request.value ("scale", 1.0f);
            const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, scale);
            file.deleteFile();
            juce::FileOutputStream stream (file);
            juce::PNGImageFormat png;

            if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
                throw std::runtime_error ("could not write the screenshot");

            reply["path"] = file.getFullPathName().toStdString();
            reply["size"] = { image.getWidth(), image.getHeight() };
        }
        else if (cmd == "state")
        {
            const auto& state = context.state();
            reply["haveState"] = state.haveState;
            reply["activePresetId"] = state.activePresetId;
            reply["activePresetName"] = state.activePreset ? state.activePreset->name : std::string {};
            reply["activeSceneId"] = state.activeSceneId;
            reply["dirty"] = state.activePresetDirty;
            reply["nodes"] = state.activePreset ? state.activePreset->graph.nodes.size() : 0u;
            reply["presets"] = state.presetList.size();
            reply["catalog"] = state.catalog.size();
            reply["theme"] = state.theme;
            reply["outputPeakDb"] = state.telemetry.output.peakDb;
            reply["messagesApplied"] = context.client.MessagesApplied();
            reply["lastFullStateApplyMs"] = context.client.LastFullStateApplyMs();
            reply["tunerActive"] = state.tuner.active;
            reply["metronomeEnabled"] = state.metronome.enabled;
            reply["outputMuted"] = state.outputMuted;
        }
        else if (cmd == "send")
        {
            auto message = request.value ("message", nlohmann::json::object());
            const auto type = message.value ("type", std::string {});
            message.erase ("type");
            context.client.Send (type, message);
        }
        else if (cmd == "resize")
        {
            editor.setSize (request.value ("width", editor.getWidth()), request.value ("height", editor.getHeight()));
            reply["size"] = { editor.getWidth(), editor.getHeight() };
        }
        else if (cmd == "theme")
        {
            context.commands.SetTheme (request.value ("name", std::string ("dark")));
        }
        else if (cmd == "menu")
        {
            const auto wanted = juce::String::fromUTF8 (request.value ("item", std::string {}).c_str());
            bool pressed = false;
            auto items = driveOpenMenu (wanted, pressed);

            // JUCE closes a menu whose app is not in front, which is where a test drives it
            // from, so fall back to the copy NanoContext::showMenu kept.
            if (items.empty() && ! pressed)
            {
                std::function<void()> chosen;

                for (juce::PopupMenu::MenuItemIterator iterator (context.lastMenu, true); iterator.next();)
                {
                    const auto& item = iterator.getItem();

                    if (item.isSeparator || item.isSectionHeader)
                        continue;

                    items.push_back ({ { "text", item.text.toStdString() }, { "enabled", item.isEnabled } });

                    if (! chosen && item.isEnabled && wanted.isNotEmpty() && item.text == wanted)
                        chosen = item.action;
                }

                if (chosen)
                {
                    juce::PopupMenu::dismissAllActiveMenus();
                    context.lastMenu = {};
                    chosen();
                    pressed = true;
                }
            }

            reply["items"] = std::move (items);

            if (wanted.isNotEmpty() && ! pressed)
                throw std::runtime_error ("no enabled item with that text in the open or last menu");
        }
        else if (cmd == "slider")
        {
            auto* slider = dynamic_cast<juce::Slider*> (findById (editor, juce::String (request.value ("id", std::string {}))));

            if (slider == nullptr)
                throw std::runtime_error ("no visible slider with that id");

            // As a click on the track would: the value, then the end of the gesture.
            slider->setValue (request.value ("value", slider->getValue()), juce::sendNotificationSync);

            if (slider->onDragEnd)
                slider->onDragEnd();

            reply["value"] = slider->getValue();
        }
        else if (cmd == "type")
        {
            auto* editorText = dynamic_cast<juce::TextEditor*> (findById (editor, juce::String (request.value ("id", std::string {}))));

            if (editorText == nullptr)
                throw std::runtime_error ("no visible text field with that id");

            editorText->setText (juce::String::fromUTF8 (request.value ("text", std::string {}).c_str()), true);

            if (request.value ("enter", false) && editorText->onReturnKey)
                editorText->onReturnKey();
        }
        else if (cmd == "combo")
        {
            auto* combo = dynamic_cast<juce::ComboBox*> (findById (editor, juce::String (request.value ("id", std::string {}))));

            if (combo == nullptr)
                throw std::runtime_error ("no visible drop-down with that id");

            const auto wanted = juce::String::fromUTF8 (request.value ("item", std::string {}).c_str());
            auto options = nlohmann::json::array();
            bool chosen = false;

            for (int i = 0; i < combo->getNumItems(); ++i)
            {
                options.push_back (combo->getItemText (i).toStdString());

                if (! chosen && combo->getItemText (i) == wanted)
                {
                    combo->setSelectedItemIndex (i, juce::sendNotificationSync);
                    chosen = true;
                }
            }

            reply["items"] = std::move (options);

            if (wanted.isNotEmpty() && ! chosen)
                throw std::runtime_error ("no item with that text");
        }
        else
        {
            throw std::runtime_error ("unknown cmd");
        }
    }
    catch (const std::exception& e)
    {
        reply["ok"] = false;
        reply["error"] = e.what();
    }

    return juce::String::fromUTF8 (reply.dump().c_str());
}
} // namespace soundshed::nano
