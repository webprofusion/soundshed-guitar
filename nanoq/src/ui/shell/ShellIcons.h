#pragma once

#include "ui/widgets/Graphics.h"

namespace nanoq::ui
{
/// Registers the shell's own icons ("ui-chain", "ui-prev", "ui-undo", ...) with `icons`.
void AddShellIcons(Icons& icons);
} // namespace nanoq::ui
