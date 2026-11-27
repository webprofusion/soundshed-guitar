#pragma once

/**
 * InputMode.h - the input mode control's choices, as the web UI's inputMode.ts lists them: what
 * the chains are fed (docs/signal-chain.md, "Channel Layout").
 *
 * In the standalone app the player picks among five modes; in a DAW the track decides between mono
 * and stereo, and dual mono is the one choice left. The engine reports what its input and output
 * can carry ("inputModeChanged", ClientState's InputCapabilities); a mode they cannot deliver stays
 * listed, greyed, with what it needs, and the mode actually running is said when it is not the one
 * chosen.
 */

#include "uiclient/ClientState.h"

#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient::input_mode
{
enum class Mode
{
    Mono1,
    Mono2,
    MonoSum,
    Stereo,
    DualMono
};

/// What a mode is stored as (InputModeSettings.h's keys, UiCommands::SetInputMode).
struct Settings
{
    bool monoMode = true;
    int inputChannel = 0;
    bool dualMono = false;
};

/// How a mode is shown: its routing icon (ShellIcons' "input-<icon>"), its name and what it does.
struct Presentation
{
    std::string icon;
    std::string name;
    std::string description;
};

[[nodiscard]] Mode FromSettings(bool monoMode, int inputChannel, bool dualMono);
[[nodiscard]] Settings ToSettings(Mode mode);

/// The modes worth listing: in a DAW the track stands in for the three mono modes and stereo.
[[nodiscard]] std::vector<Mode> Listed(const InputCapabilities& caps);
/// Why `mode` cannot be had with the input and output the engine reports, or none if it can.
[[nodiscard]] std::optional<std::string> UnavailableReason(Mode mode, const InputCapabilities& caps);
/// The mode the trigger shows: in a DAW anything but dual mono is "as the track is".
[[nodiscard]] Mode Displayed(Mode current, const InputCapabilities& caps);
[[nodiscard]] Presentation Present(Mode mode, const InputCapabilities& caps);
/// Whether what runs ("mono", "stereo" or "dualMono") is not what `mode` would run: Stereo on a
/// one-input device, dual mono with one output.
[[nodiscard]] bool FallsBack(Mode mode, const InputCapabilities& caps);
/// "Mono", "Stereo" or "Dual mono", for what runs.
[[nodiscard]] std::string EffectiveName(const InputCapabilities& caps);
/// The one thing worth saying about the input as it stands, or "" when the mode speaks for itself.
[[nodiscard]] std::string StatusNote(Mode current, const InputCapabilities& caps);
/// The trigger's tooltip: "Input: <name>. <description>.", what runs when it falls back, and the note.
[[nodiscard]] std::string Tooltip(Mode current, const InputCapabilities& caps);
} // namespace guitarfx::uiclient::input_mode
