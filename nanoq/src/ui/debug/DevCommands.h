#pragma once

// DevCommands - the DevDriver's `cmd` lines: edits sent to the engine the way the UI's own
// controls send them, for a stress test that wants chain, scene and preset changes without
// finding each control on screen (nanoq/tools/stress-ui.ps1). Only reached when
// SOUNDSHED_NANOQ_DRIVE is set.
//
// Indices are taken modulo what exists at the time, so a seeded script can pick at random
// without knowing the state; a fraction is 0..1 across a parameter's range:
//
//   cmd add <type> <after>        an effect from the catalog after node <after> (-1: at the start)
//   cmd remove <node>             cmd bypass <node>           cmd replace <node> <type>
//   cmd move <node> <after>       cmd param <node> <param> <fraction>
//   cmd branches <node> <count>   a splitter's branch count   cmd collapse <node>  a splitter
//   cmd scene <i>                 cmd addscene                cmd removescene <i>
//   cmd preset <delta>            steps through the presets   cmd load <i>   a preset by index
//   cmd setting <key> <json>      as the `setting` verb
//   cmd device <action> <json>    an audioDevice request (Settings > Audio & MIDI), for the
//                                 device-change hang; never saved on a throwaway profile

#include <string>

namespace guitarfx::uiclient
{
class UiCommands;
struct ClientState;
} // namespace guitarfx::uiclient

namespace nanoq::ui
{
/// Plays one `cmd` line (without the "cmd "); false when it was not understood.
bool RunDevCommand(const guitarfx::uiclient::ClientState& state, guitarfx::uiclient::UiCommands& commands,
                   const std::string& line);
} // namespace nanoq::ui
