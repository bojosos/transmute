#include "alchemy/bundle.hpp"
#include "alchemy/alchemy_root.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy {
namespace {

std::vector<std::uint8_t> read_plist(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("application bundle has no readable Contents/Info.plist");
    const auto length = input.tellg();
    if (length <= 0 || length > 8 * 1024 * 1024) throw std::runtime_error("invalid bundle plist size");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), length)) {
        throw std::runtime_error("truncated bundle plist");
    }
    return bytes;
}

void append_utf8(std::string& result, std::uint32_t code) {
    if (code <= 0x7f) result.push_back(static_cast<char>(code));
    else if (code <= 0x7ff) {
        result.push_back(static_cast<char>(0xc0 | (code >> 6)));
        result.push_back(static_cast<char>(0x80 | (code & 63)));
    } else if (code <= 0xffff) {
        result.push_back(static_cast<char>(0xe0 | (code >> 12)));
        result.push_back(static_cast<char>(0x80 | ((code >> 6) & 63)));
        result.push_back(static_cast<char>(0x80 | (code & 63)));
    } else {
        result.push_back(static_cast<char>(0xf0 | (code >> 18)));
        result.push_back(static_cast<char>(0x80 | ((code >> 12) & 63)));
        result.push_back(static_cast<char>(0x80 | ((code >> 6) & 63)));
        result.push_back(static_cast<char>(0x80 | (code & 63)));
    }
}

class BinaryPlist {
public:
    explicit BinaryPlist(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {
        if (bytes_.size() < 40) fail();
        const auto trailer = bytes_.size() - 32;
        offset_size_ = bytes_[trailer + 6];
        reference_size_ = bytes_[trailer + 7];
        count_ = integer(trailer + 8, 8);
        root_ = integer(trailer + 16, 8);
        offsets_ = integer(trailer + 24, 8);
        if (offset_size_ == 0 || offset_size_ > 8 || reference_size_ == 0 || reference_size_ > 8 ||
            count_ == 0 || count_ > 1'000'000 || root_ >= count_ || offsets_ < 8 ||
            offsets_ > trailer || count_ > (trailer - offsets_) / offset_size_) fail();
    }

    std::string executable() const {
        auto position = object(root_);
        const auto marker = bytes_[position++];
        if ((marker >> 4) != 13) fail();
        const auto count = length(marker & 15, position);
        if (count > count_ || count > (offsets_ - position) / reference_size_ / 2) fail();
        std::string result;
        bool found = false;
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto key = integer(position + index * reference_size_, reference_size_);
            if (string(key) != "CFBundleExecutable") continue;
            if (found) throw std::runtime_error("duplicate CFBundleExecutable in bundle plist");
            const auto value = integer(position + (count + index) * reference_size_, reference_size_);
            result = string(value);
            found = true;
        }
        if (!found) throw std::runtime_error("bundle plist has no CFBundleExecutable");
        return result;
    }

private:
    [[noreturn]] static void fail() { throw std::runtime_error("malformed binary bundle plist"); }
    std::uint64_t integer(std::uint64_t position, unsigned size) const {
        if (size == 0 || size > 8 || position > bytes_.size() || size > bytes_.size() - position) fail();
        std::uint64_t value = 0;
        for (unsigned index = 0; index < size; ++index) value = (value << 8) | bytes_[static_cast<std::size_t>(position + index)];
        return value;
    }
    std::uint64_t object(std::uint64_t reference) const {
        if (reference >= count_) fail();
        const auto position = integer(offsets_ + reference * offset_size_, offset_size_);
        if (position < 8 || position >= offsets_) fail();
        return position;
    }
    std::uint64_t length(unsigned small, std::uint64_t& position) const {
        if (small != 15) return small;
        if (position >= offsets_) fail();
        const auto marker = bytes_[static_cast<std::size_t>(position++)];
        if ((marker >> 4) != 1 || (marker & 15) > 3) fail();
        const auto size = 1U << (marker & 15);
        if (size > offsets_ - position) fail();
        const auto value = integer(position, size);
        position += size;
        return value;
    }
    std::string string(std::uint64_t reference) const {
        auto position = object(reference);
        const auto marker = bytes_[static_cast<std::size_t>(position++)];
        const auto kind = marker >> 4;
        if (kind != 5 && kind != 6) fail();
        const auto count = length(marker & 15, position);
        const auto width = kind == 6 ? 2U : 1U;
        if (count > (offsets_ - position) / width) fail();
        std::string result;
        for (std::uint64_t index = 0; index < count; ++index) {
            auto code = static_cast<std::uint32_t>(integer(position + index * width, width));
            if (kind == 5 && code > 127) fail();
            if (code >= 0xd800 && code <= 0xdbff) {
                if (++index >= count) fail();
                const auto low = integer(position + index * width, width);
                if (low < 0xdc00 || low > 0xdfff) fail();
                code = 0x10000 + ((code - 0xd800) << 10) + static_cast<std::uint32_t>(low - 0xdc00);
            } else if (code >= 0xdc00 && code <= 0xdfff) fail();
            append_utf8(result, code);
        }
        return result;
    }
    const std::vector<std::uint8_t>& bytes_;
    unsigned offset_size_{}, reference_size_{};
    std::uint64_t count_{}, root_{}, offsets_{};
};

std::string xml_executable(std::string_view xml) {
    constexpr std::string_view key = "<key>CFBundleExecutable</key>";
    const auto key_position = xml.find(key);
    if (key_position == std::string_view::npos) throw std::runtime_error("bundle plist has no CFBundleExecutable");
    if (xml.find(key, key_position + key.size()) != std::string_view::npos) {
        throw std::runtime_error("duplicate CFBundleExecutable in bundle plist");
    }
    auto position = key_position + key.size();
    while (position < xml.size() && (xml[position] == ' ' || xml[position] == '\t' ||
           xml[position] == '\r' || xml[position] == '\n')) ++position;
    if (!xml.substr(position).starts_with("<string>")) throw std::runtime_error("invalid CFBundleExecutable value");
    position += 8;
    const auto end = xml.find("</string>", position);
    if (end == std::string_view::npos) throw std::runtime_error("unterminated bundle executable string");
    std::string result;
    for (auto index = position; index < end; ++index) {
        if (xml[index] != '&') { result.push_back(xml[index]); continue; }
        const auto semicolon = xml.find(';', index);
        if (semicolon == std::string_view::npos || semicolon >= end) throw std::runtime_error("invalid plist XML entity");
        const auto entity = xml.substr(index, semicolon - index + 1);
        if (entity == "&amp;") result.push_back('&');
        else if (entity == "&lt;") result.push_back('<');
        else if (entity == "&gt;") result.push_back('>');
        else if (entity == "&quot;") result.push_back('"');
        else if (entity == "&apos;") result.push_back('\'');
        else throw std::runtime_error("unsupported plist XML entity");
        index = semicolon;
    }
    return result;
}

bool macho_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 4> magic{};
    if (!input.read(reinterpret_cast<char*>(magic.data()), magic.size())) return false;
    return magic == std::array<unsigned char, 4>{0xcf, 0xfa, 0xed, 0xfe} ||
           magic == std::array<unsigned char, 4>{0xca, 0xfe, 0xba, 0xbe} ||
           magic == std::array<unsigned char, 4>{0xca, 0xfe, 0xba, 0xbf};
}

} // namespace

std::filesystem::path discover_program(const std::filesystem::path& path) {
    if (std::filesystem::is_regular_file(path)) return std::filesystem::canonical(path);
    if (!std::filesystem::is_directory(path)) throw std::runtime_error("guest executable path does not exist");
    const auto root = std::filesystem::canonical(path);
    const auto bytes = read_plist(root / "Contents" / "Info.plist");
    const auto text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto executable = text.starts_with("bplist00") ? BinaryPlist(bytes).executable() : xml_executable(text);
    if (executable.empty() || executable == "." || executable == ".." ||
        executable.find_first_of("/\\:\0", 0, 4) != std::string::npos) {
        throw std::runtime_error("bundle executable must be a plain file name");
    }
    const auto selected = root / "Contents" / "MacOS" / utf8_path(executable);
    if (!std::filesystem::is_regular_file(selected)) throw std::runtime_error("bundle executable is missing");
    return std::filesystem::canonical(selected);
}

std::vector<std::filesystem::path> bundle_macho_files(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::is_directory(path)) return {discover_program(path)};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path,
             std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file() && macho_file(entry.path())) files.push_back(installed_real_path(entry.path()));
        if (files.size() > 4096) throw std::runtime_error("bundle Mach-O image limit exceeded");
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

} // namespace alchemy
