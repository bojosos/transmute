#include "alchemy/host/win32/gui/clipboard_codec.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace alchemy::gui::win32 {
namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("Alchemy window server: clipboard image " + what);
}

// Extracts a channel described by a contiguous bit mask, scaled to 0..255.
std::uint8_t channel(std::uint32_t pixel, std::uint32_t mask) {
    if (mask == 0) return 0;
    const auto shift = static_cast<unsigned>(std::countr_zero(mask));
    const auto bits = static_cast<unsigned>(std::popcount(mask));
    const auto value = (pixel & mask) >> shift;
    const auto maximum = (bits >= 32) ? 0xffffffffU : ((1U << bits) - 1U);
    return static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) * 255U + maximum / 2) / maximum);
}

} // namespace

void append_utf8(std::string& output, char32_t code_point) {
    if (code_point > 0x10ffff || (code_point >= 0xd800 && code_point <= 0xdfff)) code_point = 0xfffd;
    if (code_point < 0x80) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        output.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else if (code_point < 0x10000) {
        output.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
}

std::wstring utf8_to_wide(std::string_view text) {
    std::wstring output;
    output.reserve(text.size());
    std::size_t index = 0;
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    while (index < text.size()) {
        const auto lead = byte(index);
        char32_t code_point = 0xfffd;
        std::size_t length = 1;
        if (lead < 0x80) {
            code_point = lead;
        } else {
            std::size_t expected = 0;
            char32_t minimum = 0;
            if ((lead & 0xe0) == 0xc0) { expected = 2; code_point = lead & 0x1f; minimum = 0x80; }
            else if ((lead & 0xf0) == 0xe0) { expected = 3; code_point = lead & 0x0f; minimum = 0x800; }
            else if ((lead & 0xf8) == 0xf0) { expected = 4; code_point = lead & 0x07; minimum = 0x10000; }
            bool valid = expected != 0 && index + expected <= text.size();
            for (std::size_t continuation = 1; valid && continuation < expected; ++continuation) {
                const auto next = byte(index + continuation);
                if ((next & 0xc0) != 0x80) valid = false;
                else code_point = (code_point << 6) | (next & 0x3f);
            }
            if (valid && (code_point < minimum || code_point > 0x10ffff ||
                          (code_point >= 0xd800 && code_point <= 0xdfff))) valid = false;
            if (valid) length = expected;
            else code_point = 0xfffd;
        }
        index += length;
        if (code_point >= 0x10000) {
            code_point -= 0x10000;
            output.push_back(static_cast<wchar_t>(0xd800 + (code_point >> 10)));
            output.push_back(static_cast<wchar_t>(0xdc00 + (code_point & 0x3ff)));
        } else {
            output.push_back(static_cast<wchar_t>(code_point));
        }
    }
    return output;
}

std::string wide_to_utf8(std::wstring_view text) {
    std::string output;
    output.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        char32_t unit = static_cast<char16_t>(text[index]);
        if (unit >= 0xd800 && unit <= 0xdbff && index + 1 < text.size()) {
            const char32_t low = static_cast<char16_t>(text[index + 1]);
            if (low >= 0xdc00 && low <= 0xdfff) {
                unit = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
                ++index;
            }
        }
        append_utf8(output, unit);
    }
    return output;
}

std::string clipboard_text_from_windows(std::wstring_view text) {
    if (const auto end = text.find(L'\0'); end != std::wstring_view::npos) text = text.substr(0, end);
    std::wstring normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r' && index + 1 < text.size() && text[index + 1] == L'\n') continue;
        normalized.push_back(text[index]);
    }
    return wide_to_utf8(normalized);
}

std::wstring clipboard_text_to_windows(std::string_view text) {
    const auto wide = utf8_to_wide(text);
    std::wstring output;
    output.reserve(wide.size() + wide.size() / 16 + 1);
    for (std::size_t index = 0; index < wide.size(); ++index) {
        if (wide[index] == L'\n' && (index == 0 || wide[index - 1] != L'\r')) output.push_back(L'\r');
        output.push_back(wide[index]);
    }
    return output;
}

std::vector<std::uint8_t> image_to_dibv5(const Image& image) {
    const auto pixel_count = static_cast<std::uint64_t>(image.width) * image.height;
    if (image.width == 0 || image.height == 0 || image.width > 0x7fffffff || image.height > 0x7fffffff)
        fail("has an invalid size");
    if (image.rgba.size() != pixel_count * 4) fail("pixel buffer does not match its size");
    if (pixel_count * 4 > 0x7fffffffULL) fail("is too large");
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = static_cast<LONG>(image.width);
    header.bV5Height = static_cast<LONG>(image.height);   // bottom-up
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = static_cast<DWORD>(pixel_count * 4);
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    header.bV5CSType = LCS_sRGB;
    header.bV5Intent = LCS_GM_IMAGES;
    // BI_BITFIELDS with a V5 header: the masks live in the header, no extra
    // color table follows.
    std::vector<std::uint8_t> output(sizeof(header) + header.bV5SizeImage);
    std::memcpy(output.data(), &header, sizeof(header));
    auto* pixels = output.data() + sizeof(header);
    const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
    for (std::uint32_t row = 0; row < image.height; ++row) {
        const auto* source = image.rgba.data() + static_cast<std::size_t>(row) * stride;
        auto* target = pixels + static_cast<std::size_t>(image.height - 1 - row) * stride;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            target[x * 4 + 0] = source[x * 4 + 2];
            target[x * 4 + 1] = source[x * 4 + 1];
            target[x * 4 + 2] = source[x * 4 + 0];
            target[x * 4 + 3] = source[x * 4 + 3];
        }
    }
    return output;
}

Image image_from_dib(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < sizeof(BITMAPINFOHEADER)) fail("header is truncated");
    BITMAPINFOHEADER info{};
    std::memcpy(&info, data, sizeof(info));
    if (info.biSize < sizeof(BITMAPINFOHEADER) || info.biSize > size) fail("header size is invalid");
    if (info.biWidth <= 0 || info.biHeight == 0 || info.biPlanes != 1) fail("dimensions are invalid");
    const bool top_down = info.biHeight < 0;
    const auto width = static_cast<std::uint32_t>(info.biWidth);
    const auto height = static_cast<std::uint32_t>(top_down ? -static_cast<std::int64_t>(info.biHeight) : info.biHeight);
    if (static_cast<std::uint64_t>(width) * height > (1ULL << 28)) fail("is too large");
    const auto bits = info.biBitCount;
    std::uint32_t red_mask = 0, green_mask = 0, blue_mask = 0, alpha_mask = 0;
    std::size_t offset = info.biSize;
    if (info.biCompression == BI_BITFIELDS) {
        if (bits != 16 && bits != 32) fail("uses BI_BITFIELDS with an unsupported depth");
        if (info.biSize >= sizeof(BITMAPV4HEADER)) {
            BITMAPV4HEADER v4{};
            std::memcpy(&v4, data, sizeof(v4));
            red_mask = v4.bV4RedMask;
            green_mask = v4.bV4GreenMask;
            blue_mask = v4.bV4BlueMask;
            alpha_mask = v4.bV4AlphaMask;
        } else {
            if (offset + 12 > size) fail("bit-field masks are truncated");
            std::memcpy(&red_mask, data + offset, 4);
            std::memcpy(&green_mask, data + offset + 4, 4);
            std::memcpy(&blue_mask, data + offset + 8, 4);
            offset += 12;
        }
    } else if (info.biCompression == BI_RGB) {
        if (bits == 16) { red_mask = 0x7c00; green_mask = 0x03e0; blue_mask = 0x001f; }
        else if (bits == 24 || bits == 32) { red_mask = 0xff0000; green_mask = 0xff00; blue_mask = 0xff; }
        if (bits == 32) alpha_mask = 0xff000000;
    } else {
        fail("uses unsupported compression " + std::to_string(info.biCompression));
    }
    std::vector<RGBQUAD> palette;
    if (bits == 1 || bits == 4 || bits == 8) {
        if (info.biCompression != BI_RGB) fail("palette format is compressed");
        const std::size_t entries = info.biClrUsed ? info.biClrUsed : (1U << bits);
        if (entries > 256 || offset + entries * sizeof(RGBQUAD) > size) fail("palette is truncated");
        palette.resize(entries);
        std::memcpy(palette.data(), data + offset, entries * sizeof(RGBQUAD));
        offset += entries * sizeof(RGBQUAD);
    } else if (bits != 16 && bits != 24 && bits != 32) {
        fail("has unsupported depth " + std::to_string(bits));
    } else if (info.biClrUsed && info.biSize < sizeof(BITMAPV4HEADER)) {
        offset += static_cast<std::size_t>(info.biClrUsed) * sizeof(RGBQUAD);   // optional color table
    }
    const std::size_t stride = ((static_cast<std::size_t>(width) * bits + 31) / 32) * 4;
    // A V5 profile may follow the pixels; the pixels start right after the
    // header, masks and color table for packed DIBs. Some producers also
    // repeat the three masks after a V4/V5 header; the exact size tells.
    if (info.biCompression == BI_BITFIELDS && info.biSize >= sizeof(BITMAPV4HEADER) &&
        offset + 12 + stride * height == size)
        offset += 12;
    if (offset + stride * height > size) fail("pixels are truncated");
    const auto* pixels = data + offset;
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    bool any_alpha = false;
    for (std::uint32_t row = 0; row < height; ++row) {
        const auto* source = pixels + static_cast<std::size_t>(top_down ? row : height - 1 - row) * stride;
        auto* target = image.rgba.data() + static_cast<std::size_t>(row) * width * 4;
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t r{}, g{}, b{}, a{255};
            if (!palette.empty()) {
                const auto bit_offset = static_cast<std::size_t>(x) * bits;
                const auto byte = source[bit_offset / 8];
                const auto shift = 8 - bits - static_cast<unsigned>(bit_offset % 8);
                const auto index = (byte >> shift) & ((1U << bits) - 1U);
                if (index >= palette.size()) fail("pixel references a missing palette entry");
                r = palette[index].rgbRed;
                g = palette[index].rgbGreen;
                b = palette[index].rgbBlue;
            } else {
                std::uint32_t pixel = 0;
                std::memcpy(&pixel, source + static_cast<std::size_t>(x) * (bits / 8), bits / 8);
                r = channel(pixel, red_mask);
                g = channel(pixel, green_mask);
                b = channel(pixel, blue_mask);
                if (alpha_mask) {
                    a = channel(pixel, alpha_mask);
                    any_alpha = any_alpha || a != 0;
                }
            }
            target[x * 4 + 0] = r;
            target[x * 4 + 1] = g;
            target[x * 4 + 2] = b;
            target[x * 4 + 3] = a;
        }
    }
    if (alpha_mask && !any_alpha)
        for (std::size_t index = 3; index < image.rgba.size(); index += 4) image.rgba[index] = 255;
    return image;
}

} // namespace alchemy::gui::win32
