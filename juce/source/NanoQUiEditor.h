#pragma once

/**
 * NanoQUiEditor - Sepia's editor drawn by Nano Q's native UI (nanoq/src/ui), not
 * the WebView. A spike: built only with -DSOUNDSHED_NANOQ_UI=ON (juce/cmake/NanoQUi.cmake), on
 * Windows.
 *
 * JUCE gives the window and nothing else. The UI is an Elements view in a child window of the
 * editor's peer (nanoq::ui::EmbeddedUi), placed over the editor's area, and it reaches the
 * engine through the same JSON messages the WebView page sends (nanoq::ui::UiLink), so the
 * engine cannot tell the two editors apart. SoundshedEditorBase does the rest: the
 * controller's idle ticks, the remembered size, the standalone's launch curtain.
 */

#include "editor/SoundshedEditorBase.h"

#include <memory>

namespace nanoq::ui
{
class EmbeddedUi;
}

class NanoQUiEditor final : public soundshed::editor::SoundshedEditorBase
{
  public:
    explicit NanoQUiEditor(PluginProcessorAdapter& processor);
    ~NanoQUiEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    class Link;
    class Placement;

    /// Makes the UI a child of the editor's current peer, or drops it while there is none. A
    /// new peer (the editor moved to another window) gets a new UI.
    void attachToPeer();

    /// Keeps the child window over the editor's area, in the peer's physical pixels.
    void placeChildWindow();

    std::unique_ptr<Link> mLink;
    std::unique_ptr<nanoq::ui::EmbeddedUi> mUi;
    juce::ComponentPeer* mPeer = nullptr;
    std::unique_ptr<Placement> mPlacement;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NanoQUiEditor)
};
