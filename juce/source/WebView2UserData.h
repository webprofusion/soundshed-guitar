#pragma once

#include <juce_core/juce_core.h>

#include <functional>

// Where the Windows editor keeps its WebView2 profile (the Chromium "user data
// folder": cache, storage, and the browser process's lock), and the clean-up of
// the profiles older builds left behind.
//
// Builds up to September 2026 created a fresh profile under %TEMP% on every editor
// open, named after the launch time, and never deleted any of them: one Chromium
// profile per launch, 785 of them (2.5 GB) on one machine after two months. The
// profile is now one stable per-user folder, and the leftovers are swept once per
// process at startup.
namespace guitarfx::webview2
{
    inline constexpr const char* kAppDataFolderName = "Soundshed Guitar";
    inline constexpr const char* kProfileFolderName = "WebView2";

    // The %TEMP% folders the per-launch scheme wrote into: one profile per launch
    // under the first, and a single probe profile for areOptionsSupported.
    inline constexpr const char* kLegacyProfileRoot = "SoundshedGuitarWebView2";
    inline constexpr const char* kLegacyProbeFolder = "SoundshedGuitarWebView2Check";

    /** The profile folder for this process.

        `localAppData` is %LOCALAPPDATA%: a browser cache is per-machine state, and
        the roaming %APPDATA% the rest of the app lives in would sync it between
        machines. `additionalBrowserArguments` is WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS
        as the runtime reads it. Every process that opens a user data folder shares
        the one browser process behind it, and the runtime refuses to join that
        process with different options (ERROR_INVALID_STATE, which JUCE turns into a
        silent fall-back to the IE control). So a launch with non-default arguments,
        such as the agent-ui-debug tooling's --remote-debugging-port, gets a sibling
        folder keyed on those arguments instead of a blank editor whenever a normal
        instance is already open.
    */
    inline juce::File resolveUserDataFolder (const juce::File& localAppData,
        const juce::String& additionalBrowserArguments)
    {
        auto name = juce::String (kProfileFolderName);
        const auto arguments = additionalBrowserArguments.trim();

        if (arguments.isNotEmpty())
            name << "-" << juce::String::toHexString (arguments.hashCode());

        return localAppData.getChildFile (kAppDataFolderName).getChildFile (name);
    }

    struct LegacySweepResult
    {
        int removed = 0;
        int skipped = 0; // still open in another process, or not fully deletable
    };

    /** Deletes the per-launch profiles an older build left under `tempDir`.

        A folder is deleted only once it has been renamed out of the way. Windows
        refuses to rename a directory while any file inside it is open, and a live
        WebView2 profile keeps its lock file and databases open, so the rename is
        the in-use test: a profile that an older build (or a crashed instance whose
        browser process is still up) holds is skipped and picked up by a later
        pass. Nothing is ever deleted from under a running browser.

        `shouldStop`, when given, is asked before each folder: a sweep that has to stop
        early leaves the rest for a later pass.
    */
    inline LegacySweepResult sweepLegacyProfileFolders (const juce::File& tempDir,
        const std::function<bool()>& shouldStop = {})
    {
        LegacySweepResult result;
        const auto stopping = [&shouldStop] { return shouldStop && shouldStop(); };

        const auto removeIfIdle = [&result] (const juce::File& dir) {
            if (!dir.isDirectory())
                return;

            const auto parked = dir.getSiblingFile (dir.getFileName() + ".sweep");

            // A ".sweep" twin is an earlier pass interrupted after its rename.
            if (parked.exists() && !parked.deleteRecursively())
            {
                ++result.skipped;
                return;
            }

            if (!dir.moveFileTo (parked))
            {
                ++result.skipped;
                return;
            }

            if (parked.deleteRecursively())
                ++result.removed;
            else
                ++result.skipped;
        };

        const auto root = tempDir.getChildFile (kLegacyProfileRoot);

        if (root.isDirectory())
        {
            for (const auto& child : root.findChildFiles (juce::File::findDirectories, false))
            {
                if (stopping())
                    return result;

                removeIfIdle (child);
            }

            if (root.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
                root.deleteFile();
        }

        const auto probe = tempDir.getChildFile (kLegacyProbeFolder);

        if (probe.isDirectory() && !stopping())
            removeIfIdle (probe);

        return result;
    }
}
