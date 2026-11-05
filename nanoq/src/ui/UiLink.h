#pragma once

// UiLink - everything the native UI needs from whatever hosts the engine.
//
// The UI talks to the engine in the web UI's JSON messages, so a host is little more than a
// mailbox: requests go one way, the engine's messages come back the other. Nano Q's Engine is
// one; Soundshed Guitar's JUCE editor is another (juce/source/NativeUiEditor.cpp). Nothing here
// names a plugin framework, which is what lets the same UI run in either.

#include "IPluginHost.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace nanoq::ui
{
/// A file dialog for the view to draw, from a host with no native one to call (Linux).
struct FileDialogRequest
{
    bool save = false;
    guitarfx::BrowseFileType type = guitarfx::BrowseFileType::Any;
    std::string title;
    std::string defaultName; // a save's suggested file name
    /// Called once, on the UI thread, whatever becomes of the dialog: no success is a cancel.
    std::function<void(const guitarfx::BrowseFileResult&)> done;
};

class UiLink
{
  public:
    virtual ~UiLink() = default;

    /// A view is open (or not). Only while one is does the host collect messages for it.
    virtual void SetViewAttached(bool attached) = 0;

    /// What the engine has sent since the last call. The UI thread calls it at every pump.
    [[nodiscard]] virtual std::vector<std::string> TakeMessagesForView() = 0;

    /// Passes one JSON request to the engine. Never handled inside this call: the UI applies
    /// the answer at a later pump, as the web UI does.
    virtual void SendRequest(std::string json) = 0;

    /// The engine's runtime data, holding ui/ (icons, images, effect presentation).
    [[nodiscard]] virtual std::filesystem::path AssetRoot() const = 0;

    /// A custom layout's image, by id. Called from the UI thread while drawing: thread-safe.
    [[nodiscard]] virtual std::optional<std::filesystem::path> ResolveLayoutImageFile(const std::string& imageId) = 0;

    /// Per-pump work the host has for the UI thread (Nano Q's standalone device controls).
    virtual void PumpOnUiThread()
    {
    }

    /// The next file dialog the host wants drawn, if any. The UI thread calls it at every pump; a
    /// host with dialogs of its own never has one.
    [[nodiscard]] virtual std::optional<FileDialogRequest> TakeFileDialogRequest()
    {
        return std::nullopt;
    }
};
} // namespace nanoq::ui
