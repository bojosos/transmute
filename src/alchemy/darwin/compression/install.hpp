#pragma once

#include "alchemy/gui/install.hpp"
#include "alchemy/darwin/kit/completion/data/providers.hpp"

// Compression's rows of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only).
namespace alchemy::compression {

// libz (zlib_provider.cpp, zlib_stream.cpp; fill-only), at the position of the
// former gap-fill group. libcompression is installed with the QLab
// data group (compression_provider.cpp).
void install_zlib(gui::InstallContext& context);
// libbz2's streaming decompressor (bzip2_stream.cpp; fill-only), the one bz2
// streaming implementation. Installed first by fill::install_libsystem_leftovers
// (libsystem/misc/leftovers.cpp).
void install_bzip2_stream(gui::InstallContext& context);

// Installed by the shared QLab kit's data aggregator, in its order:
using gui::completion_kit::data::install_compression;  // compression_provider.cpp (kit/completion/data/install.cpp)

} // namespace alchemy::compression
