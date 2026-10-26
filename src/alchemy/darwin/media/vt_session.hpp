#pragma once

// The state every VTSession object carries (pixel transfer sessions, pixel_transfer.cpp, and
// decompression sessions, vt_decompression.cpp): its properties, which VTSessionSetProperty /
// CopyProperty / SetProperties / CopySupportedPropertyDictionary (pixel_transfer.cpp) keep for
// the keys of its kind.

#include "alchemy/darwin/media/media.hpp"

#include <map>
#include <mutex>
#include <span>
#include <string>

namespace alchemy::gui::fill {

struct VTSessionState final : ObjectPayload {
    enum class Kind { pixel_transfer, decompression } kind{Kind::pixel_transfer};
    std::mutex mutex;
    std::map<std::string, Address> properties;   // retained values
    bool invalid{};
};

// The property keys (the strings of the exported kVT...PropertyKey constants) a session of
// `kind` accepts.
std::span<const char* const> vt_session_keys(VTSessionState::Kind kind);

} // namespace alchemy::gui::fill
