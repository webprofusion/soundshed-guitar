#pragma once

#include <juce_core/juce_core.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace soundshed::editor
{
/// The WebView editor's resource handler runs on the message thread, one request at a time,
/// and a page load asks it for some 330 files: index.html, every stylesheet, every compiled
/// module. Opening each one there cost about 0.4 ms warm, and far more when the files are not
/// in the OS cache or an antivirus scanner checks each open (4.4 s inside the handler on the
/// first launch of a new build, measured). This reads them on a background thread ahead of
/// the requests instead: the standalone starts it before the engine starts up, an editor in a
/// plugin host as it is created.
///
/// Each file is handed out once, and whatever the page has not asked for when it has loaded is
/// let go, so nothing here outlives a page load or serves a file edited since.
void prefetchUiResources (const juce::File& uiFolder);

/// The file's bytes if the prefetch has read it and nothing has taken them yet.
std::optional<std::vector<std::byte>> takePrefetchedUiResource (const juce::File& file);

/// Lets go of whatever is left, and stops a prefetch still reading. Once the page has loaded.
void releasePrefetchedUiResources();
} // namespace soundshed::editor
