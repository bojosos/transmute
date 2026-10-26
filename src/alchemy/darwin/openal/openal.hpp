#pragma once

// OpenAL.framework (group-internal header).
//   install.cpp  alchemy.exe: no output device
//   mixer.cpp    alchemy-fx: OpenAL 1.1 over the default output device (install_openal_gui)
//   enums.cpp    the enum names, for both

#include "alchemy/gui/install.hpp"

#include <cstdint>
#include <string_view>

namespace alchemy::openal {

inline constexpr std::string_view library = "/System/Library/Frameworks/OpenAL.framework/Versions/A/OpenAL";

// The value of an AL or ALC enum name (alGetEnumValue, alcGetEnumValue); 0 for none. The
// extensions' names (AL_EXT_float32, AL_EXT_MCFORMATS, ALC_ENUMERATION_EXT) only with `extensions`.
std::uint32_t enum_value(std::string_view name, bool extensions = true);

// alchemy-fx's installer row (mixer.cpp).
void install_openal_gui(gui::InstallContext& context);

} // namespace alchemy::openal
