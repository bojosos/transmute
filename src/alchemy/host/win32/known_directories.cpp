// Windows host backend, area "system": per-user known directories, read from what the macOS layer
// read directly before (USERPROFILE, LOCALAPPDATA, std::filesystem::temp_directory_path).
#include "alchemy/host/process.hpp"
#include "alchemy/host/system.hpp"

#include "alchemy/host/win32/win32.hpp"

#include <filesystem>
#include <system_error>

namespace alchemy::host::system {

Result<std::string> known_directory(Directory which) {
    switch (which) {
    case Directory::home:
        if (auto profile = process::environment("USERPROFILE"); profile && !profile->empty()) return std::move(*profile);
        return Error::not_found;
    case Directory::data:
    case Directory::cache:
        if (auto local = process::environment("LOCALAPPDATA"); local && !local->empty()) {
            if (local->back() != '\\' && local->back() != '/') local->push_back('\\');
            return *local + "Alchemy";
        }
        return Error::not_found;
    case Directory::temporary: {   // GetTempPath2W or GetTempPathW, through the standard library
        std::error_code error;
        const auto path = std::filesystem::temp_directory_path(error);
        if (error || path.empty()) return Error::not_found;
        std::wstring value = path.native();
        if (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
        return win32::narrow(value);
    }
    }
    return Error::invalid_argument;
}

} // namespace alchemy::host::system
