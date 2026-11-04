#pragma once

// PracticeTabMessages — the Practice Tool's tablature files.
//
// The tab view (core/ui/ts/practiceTool/tab/) renders scores with alphaTab in the
// WebView, so the engine never parses one. It only keeps the file: a score the user
// opens is stored in the document store under its content hash (type "practice-tab"),
// and the UI keeps that id with its sync points and saved projects. Recalling a
// project then asks for the file back by id, wherever the original went.
//
//   browsePracticeToolTab           opens a file dialog; answers practiceToolTabLoaded
//   importPracticeToolTabData       {fileName, data: base64}, for a dropped file (its
//                                   path is never visible to the WebView)
//   requestPracticeToolTab          {id}; answers practiceToolTabLoaded, or
//                                   practiceToolTabFailed {id, error} if it is gone
//
// practiceToolTabLoaded carries {id, fileName, data: base64}. A file over
// kMaxPracticeTabBytes is refused: a score is kilobytes, and anything near the cap is
// a Guitar Pro file with audio embedded, which the tab view does not use.
//
// Nothing here holds state, so it is a registration rather than a service.

#include "IPluginHost.h"

#include <cstddef>
#include <functional>
#include <string>

namespace guitarfx
{
class MessageHandlerRegistry;

namespace storage
{
class JsonStore;
}

inline constexpr std::size_t kMaxPracticeTabBytes = 16U * 1024U * 1024U;

struct PracticeTabMessageDeps
{
    /// Opens the platform's file dialog for a score; calls back on the message thread.
    std::function<void(std::function<void(const BrowseFileResult&)>)> browseForTab;
    std::function<storage::JsonStore&()> store;
    std::function<void(const std::string&)> sendToUI;
    std::function<void(const std::string&, const std::string&)> reportError;
};

void RegisterPracticeTabMessages(MessageHandlerRegistry& registry, PracticeTabMessageDeps deps);
} // namespace guitarfx
