#pragma once

// Library identities and entries of the export-fill kit: the system
// interfaces a renderer plug-in's own Mach-O images first needed. Its rows are
// installed after the other GUI installers (src/alchemy/gui/install.cpp);
// alchemy-fx only.
//
// Scope: libSystem, libz, CoreFoundation/Foundation and non-GUI framework
// symbols. Security, CFNetwork and NSURLSession belong to the network/security
// provider; CoreGraphics/CoreText/ImageIO drawing to the Quartz provider.
// Every export has real semantics or fails with the interface's documented
// error; absent interfaces stay unresolved and fail at bind.

#include "alchemy/core_foundation.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/process_context.hpp"

#include <string_view>

namespace alchemy::export_fill {

inline constexpr std::string_view zlib_library = "/usr/lib/libz.1.dylib";
inline constexpr std::string_view libsystem_library = "/usr/lib/libSystem.B.dylib";

void install_zlib(ProcessContext& process);
void install_zlib_stream(ProcessContext& process);

} // namespace alchemy::export_fill
