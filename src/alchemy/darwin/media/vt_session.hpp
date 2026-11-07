#pragma once

// The state every VTSession object carries (pixel transfer and pixel rotation sessions,
// pixel_transfer.cpp, decompression sessions, vt_decompression.cpp, and compression sessions,
// vt_compression.cpp): its properties, which VTSessionSetProperty / CopyProperty / SetProperties /
// CopySupportedPropertyDictionary (pixel_transfer.cpp) keep for the keys of its kind.

#include "alchemy/darwin/media/media.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>

namespace alchemy::gui::fill {

struct VTSessionState final : ObjectPayload {
    enum class Kind { pixel_transfer, decompression, pixel_rotation, compression } kind{Kind::pixel_transfer};
    std::mutex mutex;
    std::map<std::string, Address> properties;   // retained values
    bool invalid{};
    // Compression sessions: a value's check before VTSessionSetProperty keeps it (0, or the status to answer),
    // and the values a session computes for VTSessionCopyProperty (+1, or 0: the kept value). Called without
    // `mutex` held.
    std::function<std::int32_t(const std::string& key, Address value)> check_set;
    std::function<Address(const std::string& key)> computed;
};

// The property keys (the strings of the exported kVT...PropertyKey constants) a session of
// `kind` accepts.
std::span<const char* const> vt_session_keys(VTSessionState::Kind kind);

} // namespace alchemy::gui::fill
