#pragma once

// DevDriver - lets a script drive the UI, for testing it the way the JUCE Nano's debug server
// does, without the operating system's input (which a background window on macOS ignores, and
// which would take the user's own pointer).
//
// Off unless SOUNDSHED_NANOQ_DRIVE names a file. Each tick, if that file exists, its lines are
// played into the view and the file is deleted, so a script writes a file and waits for it to
// go. Coordinates are the view's own (logical pixels from its top-left, below any title bar):
//
//   click x y          a left click             double x y    a double click
//   right x y          a right click            move x y      the pointer moves there
//   drag x1 y1 x2 y2   press, move, release     scroll x y dy a wheel turn (dy > 0 is up)
//   shot <file.png>    draws the view offscreen into a PNG (works while the window is hidden)
//   key <spec>         a key press and release: "space", "escape", "delete", "f2", "ctrl+shift+z"
//   type <text>        characters, as typed into whatever has focus
//   drop x y <path>    a file dropped there from the desktop
//   hold x y ms        a press held still for ms milliseconds, then let go (a touch's long press);
//                      the lines after it wait for the release
//   setting <key> <json>  sets an app setting, as the UI would (to seed a test profile)

#include <elements.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <optional>
#include <vector>

namespace nanoq::ui
{
class DevDriver
{
  public:
    /// The driver the environment asks for, or nullopt (the normal case).
    static std::optional<DevDriver> FromEnvironment();

    /// Plays and deletes the command file, if one is waiting. UI thread.
    void Poll(cycfi::elements::view& view);

    /// Called with `setting` lines' key and JSON value; the session sends them to the engine.
    std::function<void(const std::string& key, const std::string& json)> onSetting;

  private:
    explicit DevDriver(std::filesystem::path file) : mFile(std::move(file))
    {
    }

    /// Plays one line into the view; false for a hold, which ends the batch until it is let go.
    bool Play(cycfi::elements::view& view, const std::string& line);

    std::filesystem::path mFile;
    /// A hold under way: where, until when, and the lines waiting for its release.
    std::optional<std::chrono::steady_clock::time_point> mHoldUntil;
    cycfi::elements::point mHoldAt{0.0f, 0.0f};
    std::vector<std::string> mAfterHold;
};
} // namespace nanoq::ui
