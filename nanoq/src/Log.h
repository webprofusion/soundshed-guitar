#pragma once

#include <string>

namespace nanoq
{
/// Appends one line to nanoq.log in the profile's logs folder (next to the engine's own
/// session log). Any thread; failures are ignored: a log must never break the audio path.
void Log(const std::string& line);
} // namespace nanoq
