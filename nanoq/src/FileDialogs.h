#pragma once

#include "IPluginHost.h"

#include <functional>
#include <string>

namespace nanoq
{
/// Native open and save dialogs, run on a thread of their own so the engine and the view never
/// wait on them. `done` is called from that thread; the caller hands the result on.
/// Windows only for now: elsewhere the dialog reports that nothing was chosen.
void ShowOpenDialog(guitarfx::BrowseFileType type, const std::string& title,
                    std::function<void(const guitarfx::BrowseFileResult&)> done);

void ShowSaveDialog(guitarfx::BrowseFileType type, const std::string& title, const std::string& defaultName,
                    std::function<void(const guitarfx::BrowseFileResult&)> done);
} // namespace nanoq
