#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <memory>

namespace soundshed::nano
{
class NanoEditor;

/// A localhost JSON-lines port for driving the native UI from scripts and agents: the Nano
/// counterpart of the WebView's remote-debugging port (tools/agent-ui-debug/native).
///
/// Only started when SOUNDSHED_NANO_DEBUG_PORT is set, and only ever bound to 127.0.0.1.
/// Each request line is a JSON object with a "cmd"; each gets one JSON line back:
///
///   {"cmd":"tree"}                         the component tree: ids, types, bounds, text, state
///   {"cmd":"click","id":"<componentId>"}   a click (mouse down and up) at the component's centre
///   {"cmd":"screenshot","path":"<png>"}    the editor rendered to a PNG file
///   {"cmd":"state"}                        a summary of the client state
///   {"cmd":"send","message":{...}}         sends a UI→engine message through the client
///   {"cmd":"resize","width":w,"height":h}  resizes the editor (layout testing)
///   {"cmd":"theme","name":"light|dark|classic"}
///
/// Commands run on the message thread; the socket thread waits for each answer.
class NanoDebugServer final : private juce::Thread
{
public:
    NanoDebugServer (NanoEditor& editor, int port);
    ~NanoDebugServer() override;

private:
    void run() override;
    [[nodiscard]] juce::String handleOnMessageThread (const juce::String& line);

    NanoEditor& editor;
    int port;
    std::unique_ptr<juce::StreamingSocket> listener;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};
} // namespace soundshed::nano
