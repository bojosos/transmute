#include "alchemy/darwin/appkit/rtf.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

// RTF text extraction; see rtf.hpp.
namespace alchemy::gui::rtf {
namespace {

[[noreturn]] void bad(std::string_view why) { throw std::runtime_error("RTF: " + std::string(why)); }

// Windows-1252 bytes 0x80-0x9F (the rest map to the same code point).
constexpr std::array<char16_t, 32> cp1252_high{
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

char16_t from_cp1252(std::uint8_t byte) {
    if (byte >= 0x80 && byte <= 0x9F) return cp1252_high[byte - 0x80];
    return byte;
}

bool is_destination(std::string_view word) {
    static constexpr std::string_view skipped[] = {
        "fonttbl", "colortbl", "stylesheet", "info", "pict", "header", "headerl", "headerr", "headerf", "footer", "footerl",
        "footerr", "footerf", "footnote", "listtable", "listoverridetable", "rsidtbl", "generator", "xmlnstbl", "themedata",
        "colorschememapping", "latentstyles", "datastore", "object", "fldinst", "filetbl", "revtbl", "pgdsctbl"};
    for (const auto entry : skipped) if (entry == word) return true;
    return false;
}

struct Group {
    bool skip{};
    int unicode_skip{1};   // \ucN: fallback characters after \uN
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : data_(bytes) {}

    std::u16string run() {
        if (!looks_like_rtf(data_)) bad("not an RTF document");
        std::vector<Group> stack;
        Group current;
        int pending_skip = 0;   // fallback characters still to drop after \uN
        bool started = false;
        while (at_ < data_.size()) {
            const auto c = data_[at_];
            if (c == '{') {
                ++at_;
                stack.push_back(current);
                started = true;
                // "{\*\dest ...}": an ignorable destination.
                if (at_ + 1 < data_.size() && data_[at_] == '\\' && data_[at_ + 1] == '*') { current.skip = true; at_ += 2; }
                continue;
            }
            if (c == '}') {
                ++at_;
                if (stack.empty()) bad("unbalanced '}'");
                current = stack.back();
                stack.pop_back();
                pending_skip = 0;
                if (stack.empty()) break;   // the document group ended
                continue;
            }
            if (!started) bad("text before the document group");
            if (c == '\\') { control(current, pending_skip); continue; }
            ++at_;
            if (c == '\r' || c == '\n') continue;
            if (pending_skip > 0) { --pending_skip; continue; }
            if (!current.skip) out_.push_back(from_cp1252(c));
        }
        if (!stack.empty()) bad("unterminated group");
        return std::move(out_);
    }

private:
    void emit(Group& current, int& pending_skip, char16_t value) {
        if (pending_skip > 0) { --pending_skip; return; }
        if (!current.skip) out_.push_back(value);
    }

    void control(Group& current, int& pending_skip) {
        ++at_;   // backslash
        if (at_ >= data_.size()) bad("truncated control");
        const auto c = data_[at_];
        // Control symbols.
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            ++at_;
            switch (c) {
            case '\\': case '{': case '}': emit(current, pending_skip, c); return;
            case '\n': case '\r': emit(current, pending_skip, u'\n'); return;   // Cocoa's paragraph break
            case '~': emit(current, pending_skip, 0x00A0); return;
            case '_': emit(current, pending_skip, 0x2011); return;
            case '-': return;                                               // optional hyphen
            case '*': current.skip = true; return;
            case '\'': {
                if (at_ + 2 > data_.size()) bad("truncated \\' escape");
                const auto hex = [](std::uint8_t h) -> int {
                    if (h >= '0' && h <= '9') return h - '0';
                    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                    return -1;
                };
                const auto high = hex(data_[at_]), low = hex(data_[at_ + 1]);
                if (high < 0 || low < 0) bad("bad \\' escape");
                at_ += 2;
                emit(current, pending_skip, from_cp1252(static_cast<std::uint8_t>(high * 16 + low)));
                return;
            }
            default: return;   // other symbols (\|, \:) carry no text
            }
        }
        // Control word: letters, optional signed number, optional space delimiter.
        const auto start = at_;
        while (at_ < data_.size() && ((data_[at_] >= 'a' && data_[at_] <= 'z') || (data_[at_] >= 'A' && data_[at_] <= 'Z'))) ++at_;
        const std::string word(reinterpret_cast<const char*>(data_.data() + start), at_ - start);
        std::optional<long> parameter;
        if (at_ < data_.size() && (data_[at_] == '-' || (data_[at_] >= '0' && data_[at_] <= '9'))) {
            const bool negative = data_[at_] == '-';
            if (negative) ++at_;
            long value = 0;
            const auto digits = at_;
            while (at_ < data_.size() && data_[at_] >= '0' && data_[at_] <= '9') {
                value = value * 10 + (data_[at_] - '0');
                if (value > 10'000'000) bad("control word parameter too large");
                ++at_;
            }
            if (at_ == digits) bad("'-' without digits");
            parameter = negative ? -value : value;
        }
        if (at_ < data_.size() && data_[at_] == ' ') ++at_;
        if (is_destination(word)) { current.skip = true; return; }
        if (word == "ansicpg" && parameter && *parameter != 1252) bad("code page " + std::to_string(*parameter) + " is not supported");
        if (word == "mac" || word == "pc" || word == "pca") bad("\\" + word + " character sets are not supported");
        if (word == "par" || word == "line" || word == "sect" || word == "page") { emit(current, pending_skip, u'\n'); return; }
        if (word == "tab") { emit(current, pending_skip, u'\t'); return; }
        if (word == "emdash") { emit(current, pending_skip, 0x2014); return; }
        if (word == "endash") { emit(current, pending_skip, 0x2013); return; }
        if (word == "bullet") { emit(current, pending_skip, 0x2022); return; }
        if (word == "lquote") { emit(current, pending_skip, 0x2018); return; }
        if (word == "rquote") { emit(current, pending_skip, 0x2019); return; }
        if (word == "ldblquote") { emit(current, pending_skip, 0x201C); return; }
        if (word == "rdblquote") { emit(current, pending_skip, 0x201D); return; }
        if (word == "uc" && parameter) { current.unicode_skip = static_cast<int>(*parameter); return; }
        if (word == "u" && parameter) {
            const auto value = *parameter < 0 ? *parameter + 65536 : *parameter;
            if (pending_skip == 0 && !current.skip) out_.push_back(static_cast<char16_t>(value));
            pending_skip = current.unicode_skip;
            return;
        }
        // Every other control word only formats.
    }

    std::span<const std::uint8_t> data_;
    std::size_t at_{};
    std::u16string out_;
};

} // namespace

bool looks_like_rtf(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 5 && std::memcmp(bytes.data(), "{\\rtf", 5) == 0;
}

std::u16string plain_text(std::span<const std::uint8_t> bytes) { return Reader(bytes).run(); }

} // namespace alchemy::gui::rtf
