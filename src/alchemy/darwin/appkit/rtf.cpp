#include "alchemy/darwin/appkit/rtf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

// RTF text extraction and the formatting subset of rtf.hpp.
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

enum class Table { none, fonts, colors };
struct Group {
    bool skip{};
    int unicode_skip{1};   // \ucN: fallback characters after \uN
    Table table{Table::none};
    Format format;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : data_(bytes) {}

    std::vector<Run> runs() {
        static_cast<void>(run());
        return std::move(runs_);
    }

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
            if (current.table != Table::none) { table_character(current, c); continue; }
            if (pending_skip > 0) { --pending_skip; continue; }
            if (!current.skip) put(current, from_cp1252(c));
        }
        if (!stack.empty()) bad("unterminated group");
        return std::move(out_);
    }

private:
    void emit(Group& current, int& pending_skip, char16_t value) {
        if (pending_skip > 0) { --pending_skip; return; }
        if (!current.skip) put(current, value);
    }
    void put(const Group& current, char16_t value) {
        out_.push_back(value);
        if (runs_.empty() || !(runs_.back().format == current.format)) runs_.push_back({{}, current.format});
        runs_.back().text.push_back(value);
    }
    // The font and colour tables: "\fN name;" and "\redR\greenG\blueB;" entries (an empty first colour is automatic).
    void table_character(Group& current, std::uint8_t c) {
        if (c != ';') {
            if (current.table == Table::fonts) font_name_.push_back(static_cast<char>(c));
            return;
        }
        if (current.table == Table::fonts) {
            auto name = font_name_;
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            while (!name.empty() && name.back() == ' ') name.pop_back();
            fonts_[font_number_] = name;
            font_name_.clear();
        } else {
            colors_.push_back(color_given_ ? std::optional<std::array<double, 3>>(color_) : std::nullopt);
            color_ = {0, 0, 0};
            color_given_ = false;
        }
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
        if (word == "fonttbl") { current.skip = true; current.table = Table::fonts; return; }
        if (word == "colortbl") { current.skip = true; current.table = Table::colors; return; }
        if (current.table == Table::fonts) {
            if (word == "f" && parameter) { font_number_ = *parameter; font_name_.clear(); }
            return;
        }
        if (current.table == Table::colors) {
            const auto component = std::clamp(static_cast<double>(parameter.value_or(0)) / 255.0, 0.0, 1.0);
            if (word == "red") { color_[0] = component; color_given_ = true; }
            if (word == "green") { color_[1] = component; color_given_ = true; }
            if (word == "blue") { color_[2] = component; color_given_ = true; }
            return;
        }
        if (is_destination(word)) { current.skip = true; return; }
        auto& f = current.format;
        const bool on = !parameter || *parameter != 0;
        if (word == "plain") {
            Format plain;
            plain.alignment = f.alignment;
            plain.left_indent = f.left_indent;
            plain.first_indent = f.first_indent;
            plain.right_indent = f.right_indent;
            f = plain;
            return;
        }
        if (word == "pard") { f.alignment = 4; f.left_indent = f.first_indent = f.right_indent = 0; return; }
        if (word == "f" && parameter) { const auto found = fonts_.find(*parameter); f.font = found == fonts_.end() ? std::string{} : found->second; return; }
        if (word == "fs" && parameter) { f.size = static_cast<double>(*parameter) / 2; return; }
        if (word == "b") { f.bold = on; return; }
        if (word == "i") { f.italic = on; return; }
        if (word == "ul") { f.underline = on; return; }
        if (word == "ulnone") { f.underline = false; return; }
        if (word == "cf") {
            const auto index = static_cast<std::size_t>(std::max<long>(0, parameter.value_or(0)));
            f.color = index < colors_.size() ? colors_[index] : std::nullopt;
            return;
        }
        if (word == "ql") { f.alignment = 0; return; }
        if (word == "qc") { f.alignment = 1; return; }
        if (word == "qr") { f.alignment = 2; return; }
        if (word == "qj") { f.alignment = 3; return; }
        if (word == "li" && parameter) { f.left_indent = static_cast<double>(*parameter) / 20; return; }
        if (word == "fi" && parameter) { f.first_indent = static_cast<double>(*parameter) / 20; return; }
        if (word == "ri" && parameter) { f.right_indent = static_cast<double>(*parameter) / 20; return; }
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
            if (pending_skip == 0 && !current.skip) put(current, static_cast<char16_t>(value));
            pending_skip = current.unicode_skip;
            return;
        }
        // Every other control word only formats.
    }

    std::span<const std::uint8_t> data_;
    std::size_t at_{};
    std::u16string out_;
    std::vector<Run> runs_;
    std::map<long, std::string> fonts_;
    long font_number_{};
    std::string font_name_;
    std::vector<std::optional<std::array<double, 3>>> colors_;
    std::array<double, 3> color_{};
    bool color_given_{};
};

} // namespace

bool looks_like_rtf(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 5 && std::memcmp(bytes.data(), "{\\rtf", 5) == 0;
}

std::u16string plain_text(std::span<const std::uint8_t> bytes) { return Reader(bytes).run(); }

std::vector<Run> runs(std::span<const std::uint8_t> bytes) { return Reader(bytes).runs(); }

std::string write(const std::vector<Run>& runs) {
    std::vector<std::string> fonts{"Helvetica"};
    std::vector<std::array<double, 3>> colors;
    const auto font_index = [&](const std::string& name) {
        const auto family = name.empty() ? std::string("Helvetica") : name;
        for (std::size_t i = 0; i < fonts.size(); ++i) if (fonts[i] == family) return i;
        fonts.push_back(family);
        return fonts.size() - 1;
    };
    const auto color_index = [&](const std::optional<std::array<double, 3>>& color) -> std::size_t {
        if (!color) return 0;
        for (std::size_t i = 0; i < colors.size(); ++i) if (colors[i] == *color) return i + 1;
        colors.push_back(*color);
        return colors.size();
    };
    for (const auto& run : runs) { static_cast<void>(font_index(run.format.font)); static_cast<void>(color_index(run.format.color)); }
    std::string out = "{\\rtf1\\ansi\\ansicpg1252\\uc1\\deff0\n{\\fonttbl";
    for (std::size_t i = 0; i < fonts.size(); ++i) out += "\\f" + std::to_string(i) + "\\fnil " + fonts[i] + ";";
    out += "}\n{\\colortbl;";
    for (const auto& c : colors) {
        char entry[64];
        std::snprintf(entry, sizeof entry, "\\red%d\\green%d\\blue%d;", static_cast<int>(std::lround(c[0] * 255)),
                      static_cast<int>(std::lround(c[1] * 255)), static_cast<int>(std::lround(c[2] * 255)));
        out += entry;
    }
    out += "}\n";
    const auto twips = [](double points) { return std::to_string(static_cast<long>(std::lround(points * 20))); };
    bool paragraph_start = true;
    std::optional<std::array<double, 4>> written;   // the paragraph properties last written (alignment, indents)
    for (const auto& run : runs) {
        const auto& f = run.format;
        const std::array<double, 4> paragraph{static_cast<double>(f.alignment), f.left_indent, f.first_indent, f.right_indent};
        if (written && *written != paragraph) paragraph_start = true;   // they change within the paragraph
        out += "\\f" + std::to_string(font_index(f.font)) + "\\fs" + std::to_string(static_cast<long>(std::lround(f.size * 2)));
        out += f.bold ? "\\b" : "\\b0";
        out += f.italic ? "\\i" : "\\i0";
        out += f.underline ? "\\ul" : "\\ulnone";
        out += "\\cf" + std::to_string(color_index(f.color)) + " ";
        for (const auto c : run.text) {
            if (paragraph_start) {
                static constexpr const char* aligned[] = {"\\ql", "\\qc", "\\qr", "\\qj", ""};
                out += "\\pard";
                out += aligned[std::clamp(f.alignment, 0, 4)];
                out += "\\li" + twips(f.left_indent) + "\\fi" + twips(f.first_indent) + "\\ri" + twips(f.right_indent) + " ";
                paragraph_start = false;
                written = paragraph;
            }
            if (c == u'\n' || c == 0x2029) { out += "\\par\n"; paragraph_start = true; continue; }
            if (c == 0x2028) { out += "\\line "; continue; }
            if (c == u'\t') { out += "\\tab "; continue; }
            if (c == u'\\' || c == u'{' || c == u'}') { out += '\\'; out += static_cast<char>(c); continue; }
            if (c >= 0x20 && c < 0x80) { out += static_cast<char>(c); continue; }
            out += "\\u" + std::to_string(static_cast<int>(static_cast<std::int16_t>(c))) + "?";
        }
    }
    out += "}";
    return out;
}

} // namespace alchemy::gui::rtf
