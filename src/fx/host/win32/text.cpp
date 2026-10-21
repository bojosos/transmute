// win32 backend of fx/host/text.hpp.

#include "fx/host/text.hpp"

#include <windows.h>

#include <algorithm>

namespace transmute::fx::host {

std::size_t to_utf8(const wchar_t* text, char* out, std::size_t size) noexcept {
    const int written = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, static_cast<int>(size), nullptr, nullptr);
    return written > 0 ? static_cast<std::size_t>(written) : 0;
}

std::string to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr,
                            nullptr);
    return result;
}

std::wstring from_utf8(std::string_view text) {
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

} // namespace transmute::fx::host
