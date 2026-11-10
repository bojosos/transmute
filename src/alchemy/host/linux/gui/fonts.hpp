#pragma once

// Linux host backend, area "gpu": what fonts.cpp shares with text_layout.cpp and canvas.cpp.
// One FreeType library and one fontconfig configuration serve the whole process; every
// FreeType call on a shared face runs under freetype_mutex().

#include "alchemy/host/gpu.hpp"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace alchemy::host::gpu {

struct Font::Impl {
    std::string path;                                        // a font file ("" for an in-memory face)
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;  // the data of an in-memory face
    int index{};                                             // face index (named-instance bits included)
    FT_Face face{};                                          // opened on first use, under freetype_mutex()
    ~Impl();
};

namespace freetype {

std::recursive_mutex& freetype_mutex();
FT_Library library();
// The process's fontconfig configuration: the system fonts (FcSetSystem) and
// the registered ones (FcSetApplication); nullptr when fontconfig failed.
FcConfig* config();
// Changes each time a font file is registered.
std::uint64_t registration_serial();
// The face of a font file (one shared handle per file and index).
Font font_for_file(const std::string& path, int index);
// The FreeType face of `font`, opened on first use with its Unicode charmap
// selected; nullptr when it cannot be opened. Call under freetype_mutex().
FT_Face face_of(const Font& font);

} // namespace freetype
} // namespace alchemy::host::gpu
