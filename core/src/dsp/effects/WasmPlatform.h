#pragma once

// WasmPlatform.h - Whether this process may run the machine code Wasmtime generates.
//
// Wasmtime compiles a module to native code in memory it then makes executable, without MAP_JIT.
// Under macOS's Hardened Runtime that needs the com.apple.security.cs.allow-unsigned-executable-
// memory entitlement (allow-jit is not enough: measured, a hardened process with only allow-jit
// is killed as with none). The standalone app carries it (core/macos.standalone.entitlements).
// A plugin runs under its host's entitlements, and a hardened host without it does not refuse
// the code: the kernel kills the whole process the moment it runs. So the effect asks first, and
// in such a host it reports why it cannot run instead of taking the DAW down.
//
// Elsewhere (Windows, Linux, a macOS process without the Hardened Runtime) the answer is yes.

#include <string>

namespace guitarfx
{
/// True when generated code can run here. Otherwise false, with a reason for the user in `why`.
[[nodiscard]] bool WasmGeneratedCodeAllowed(std::string* why = nullptr);
} // namespace guitarfx
