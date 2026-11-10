// linux backend of fx/host/text.hpp, and the errno to Win32 error mapping.
// wchar_t is 32 bits here: wide strings hold UTF-32 code points.
#include "fx/host/text.hpp"
#include "fx/host/linux/internal.hpp"

#include <cerrno>
#include <cstring>

namespace transmute::fx::host {

namespace {

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xc0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xe0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    }
}

bool valid_code_point(char32_t c) { return c < 0x110000 && (c < 0xd800 || c > 0xdfff); }

// Decodes `text`; false on malformed UTF-8.
bool decode(std::string_view text, std::wstring& out) {
    out.clear();
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        unsigned extra = 0;
        char32_t c = 0;
        if (lead < 0x80) c = lead;
        else if ((lead & 0xe0) == 0xc0) { c = lead & 0x1f; extra = 1; }
        else if ((lead & 0xf0) == 0xe0) { c = lead & 0x0f; extra = 2; }
        else if ((lead & 0xf8) == 0xf0) { c = lead & 0x07; extra = 3; }
        else return false;
        if (i + extra >= text.size() && extra) return false;
        for (unsigned k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xc0) != 0x80) return false;
            c = (c << 6) | (next & 0x3f);
        }
        static constexpr char32_t kSmallest[] = {0, 0x80, 0x800, 0x10000};
        if (c < kSmallest[extra] || !valid_code_point(c)) return false;
        out += static_cast<wchar_t>(c);
        i += extra + 1;
    }
    return true;
}

struct ErrorPair {
    int error;
    std::uint32_t win32;
};
constexpr ErrorPair kErrors[] = {
    {ENOENT, 2},  {ENOTDIR, 3},  {EMFILE, 4},   {EACCES, 5},  {EBADF, 6},   {ENOMEM, 8},  {EINVAL, 87},
    {EEXIST, 80}, {ENOSPC, 112}, {EBUSY, 170},  {EAGAIN, 32}, {EWOULDBLOCK, 32}, {EPERM, 5}, {ENAMETOOLONG, 206},
    {ENOTEMPTY, 145}, {EXDEV, 17}, {EROFS, 19},
};

} // namespace

std::size_t to_utf8(const wchar_t* text, char* out, std::size_t size) noexcept {
    try {
        const auto narrow = linux_internal::narrow(text);
        if (narrow.size() + 1 > size) return 0;
        std::memcpy(out, narrow.c_str(), narrow.size() + 1);
        return narrow.size() + 1;
    } catch (...) {
        return 0;
    }
}

std::string to_utf8(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (const auto c : text)
        if (valid_code_point(static_cast<char32_t>(c))) append_utf8(out, static_cast<char32_t>(c));
        else return {};
    return out;
}

std::wstring from_utf8(std::string_view text) {
    std::wstring out;
    if (!decode(text, out)) return {};
    return out;
}

namespace linux_internal {

std::string narrow(const std::wstring& text) { return to_utf8(std::wstring_view(text)); }
std::wstring widen(const std::string& text) { return from_utf8(text); }

std::uint32_t win32_error(int error) noexcept {
    if (error == 0) return 0;
    for (const auto& pair : kErrors)
        if (pair.error == error) return pair.win32;
    return 0x20000000u | static_cast<std::uint32_t>(error);
}

int errno_of(std::uint32_t win32) noexcept {
    if (win32 & 0x20000000u) return static_cast<int>(win32 & ~0x20000000u);
    for (const auto& pair : kErrors)
        if (pair.win32 == win32) return pair.error;
    return win32 == 0 ? 0 : EINVAL;
}

} // namespace linux_internal

} // namespace transmute::fx::host
