#pragma once

#include "IPluginHost.h"

#include <functional>
#include <string>

namespace nanoq
{
/// Native open and save dialogs, run on a thread of their own so the engine and the view never
/// wait on them. `done` is called from that thread; the caller hands the result on.
/// Windows and macOS (FileDialogs_mac.mm, where `done` comes from the panel's completion handler on
/// the main thread); iOS opens through a document picker (FileDialogs_ios.mm) and cannot save yet;
/// elsewhere the dialog reports that nothing was chosen, and the engine asks the view instead
/// (HasNativeFileDialogs, ui/FilePicker.cpp).
void ShowOpenDialog(guitarfx::BrowseFileType type, const std::string& title,
                    std::function<void(const guitarfx::BrowseFileResult&)> done);

void ShowSaveDialog(guitarfx::BrowseFileType type, const std::string& title, const std::string& defaultName,
                    std::function<void(const guitarfx::BrowseFileResult&)> done);

/// Whether this platform has a dialog to call (a test's hook counts). Without one (Linux, where
/// a kiosk has no desktop to show one on) the file picker is drawn by the view.
[[nodiscard]] bool HasNativeFileDialogs();

/// Tests: stands in for the native dialogs. The hook is given what a dialog would be, and `done`,
/// which it may call from any thread and at any time, as a dialog's own thread does. Null puts
/// the native dialogs back.
using FileDialogHook =
    std::function<void(bool save, guitarfx::BrowseFileType type, const std::string& title,
                       const std::string& defaultName, std::function<void(const guitarfx::BrowseFileResult&)> done)>;
void SetFileDialogHookForTesting(FileDialogHook hook);
} // namespace nanoq
