#include "alchemy/darwin/coregraphics/pdf/pdf_parser.hpp"

#include "alchemy/darwin/compression/zlib_codec.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <functional>

namespace alchemy::gui::quartz2d::pdf {
namespace {

constexpr int max_depth = 256;
constexpr std::size_t max_decoded = std::size_t{1} << 30;

bool is_space(std::uint8_t c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
bool is_delimiter(std::uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}
bool is_regular(std::uint8_t c) { return !is_space(c) && !is_delimiter(c); }
int hex_value(std::uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::shared_ptr<Object> make(Kind kind) {
    auto made = std::make_shared<Object>();
    made->kind = kind;
    return made;
}

// The first occurrence of `needle` in `bytes` at or after `from`.
std::size_t find(std::span<const std::uint8_t> bytes, std::string_view needle, std::size_t from) {
    if (needle.empty() || bytes.size() < needle.size()) return std::string_view::npos;
    const auto* begin = reinterpret_cast<const char*>(bytes.data());
    const std::string_view haystack(begin, bytes.size());
    return haystack.find(needle, from);
}
std::size_t rfind(std::span<const std::uint8_t> bytes, std::string_view needle) {
    const std::string_view haystack(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return haystack.rfind(needle);
}

} // namespace

ObjectPtr Dictionary::find(std::string_view key) const {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
        if (it->first == key) return it->second;
    return nullptr;
}

ObjectPtr make_null() {
    static const auto shared = make(Kind::null);
    return shared;
}
ObjectPtr make_integer(std::int64_t value) {
    auto made = make(Kind::integer);
    made->integer = value;
    return made;
}
ObjectPtr make_name(std::string name) {
    auto made = make(Kind::name);
    made->text = std::move(name);
    return made;
}

// ---- lexer ---------------------------------------------------------------------------
bool Lexer::at_end() {
    skip_space();
    return position_ >= bytes_.size();
}

void Lexer::skip_space() {
    while (position_ < bytes_.size()) {
        const auto c = bytes_[position_];
        if (is_space(c)) { ++position_; continue; }
        if (c == '%') {
            while (position_ < bytes_.size() && bytes_[position_] != '\n' && bytes_[position_] != '\r') ++position_;
            continue;
        }
        break;
    }
}

std::optional<ObjectPtr> Lexer::next(bool references) { return token(references, 0); }

std::optional<ObjectPtr> Lexer::token(bool references, int depth) {
    if (depth > max_depth) return std::nullopt;
    skip_space();
    if (position_ >= bytes_.size()) return std::nullopt;
    const auto c = bytes_[position_];
    // Names.
    if (c == '/') {
        ++position_;
        std::string name;
        while (position_ < bytes_.size() && is_regular(bytes_[position_])) {
            const auto ch = bytes_[position_];
            if (ch == '#' && position_ + 2 < bytes_.size() && hex_value(bytes_[position_ + 1]) >= 0 && hex_value(bytes_[position_ + 2]) >= 0) {
                name.push_back(static_cast<char>(hex_value(bytes_[position_ + 1]) * 16 + hex_value(bytes_[position_ + 2])));
                position_ += 3;
                continue;
            }
            name.push_back(static_cast<char>(ch));
            ++position_;
        }
        return make_name(std::move(name));
    }
    // Literal strings.
    if (c == '(') {
        ++position_;
        std::string value;
        int nesting = 1;
        while (position_ < bytes_.size()) {
            auto ch = bytes_[position_++];
            if (ch == '\\') {
                if (position_ >= bytes_.size()) break;
                const auto e = bytes_[position_++];
                switch (e) {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case '\r':   // line continuation
                    if (position_ < bytes_.size() && bytes_[position_] == '\n') ++position_;
                    break;
                case '\n': break;
                default:
                    if (e >= '0' && e <= '7') {
                        int code = e - '0';
                        for (int i = 0; i < 2 && position_ < bytes_.size() && bytes_[position_] >= '0' && bytes_[position_] <= '7'; ++i)
                            code = code * 8 + (bytes_[position_++] - '0');
                        value.push_back(static_cast<char>(code & 0xff));
                    } else {
                        value.push_back(static_cast<char>(e));   // \( \) \\ and unknown escapes
                    }
                }
                continue;
            }
            if (ch == '(') ++nesting;
            else if (ch == ')' && --nesting == 0) break;
            if (ch == '\r') {   // an end of line is a line feed
                if (position_ < bytes_.size() && bytes_[position_] == '\n') ++position_;
                ch = '\n';
            }
            value.push_back(static_cast<char>(ch));
        }
        auto made = make(Kind::string);
        made->text = std::move(value);
        return made;
    }
    if (c == '<') {
        // Dictionaries.
        if (position_ + 1 < bytes_.size() && bytes_[position_ + 1] == '<') {
            position_ += 2;
            auto made = make(Kind::dictionary);
            for (;;) {
                skip_space();
                if (position_ + 1 < bytes_.size() && bytes_[position_] == '>' && bytes_[position_ + 1] == '>') { position_ += 2; break; }
                if (position_ >= bytes_.size()) break;   // unterminated: what was read
                auto key = token(references, depth + 1);
                if (!key) break;
                if ((*key)->kind != Kind::name) continue;   // garbage: skip it
                skip_space();
                if (position_ + 1 < bytes_.size() && bytes_[position_] == '>' && bytes_[position_ + 1] == '>') {
                    made->dictionary.entries.emplace_back((*key)->text, make_null());
                    continue;
                }
                auto value = token(references, depth + 1);
                if (!value) break;
                made->dictionary.entries.emplace_back((*key)->text, *value);
            }
            return made;
        }
        // Hexadecimal strings.
        ++position_;
        std::string value;
        int high = -1;
        while (position_ < bytes_.size() && bytes_[position_] != '>') {
            const auto v = hex_value(bytes_[position_++]);
            if (v < 0) continue;
            if (high < 0) high = v;
            else { value.push_back(static_cast<char>(high * 16 + v)); high = -1; }
        }
        if (high >= 0) value.push_back(static_cast<char>(high * 16));
        if (position_ < bytes_.size()) ++position_;
        auto made = make(Kind::string);
        made->text = std::move(value);
        return made;
    }
    if (c == '[') {
        ++position_;
        auto made = make(Kind::array);
        for (;;) {
            skip_space();
            if (position_ >= bytes_.size()) break;
            if (bytes_[position_] == ']') { ++position_; break; }
            auto value = token(references, depth + 1);
            if (!value) break;
            made->array.push_back(*value);
        }
        return made;
    }
    if (c == ']' || c == '>' || c == ')' || c == '{' || c == '}') {
        // Stray delimiters: a keyword of one character (operators "{" "}" of PostScript functions).
        ++position_;
        auto made = make(Kind::keyword);
        made->text = std::string(1, static_cast<char>(c));
        return made;
    }
    // Numbers.
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
        const auto start = position_;
        bool real = false;
        ++position_;
        while (position_ < bytes_.size()) {
            const auto ch = bytes_[position_];
            if (ch == '.') { real = true; ++position_; continue; }
            if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+') { ++position_; continue; }   // "--1" and "1-2" occur in the wild
            break;
        }
        std::string text(reinterpret_cast<const char*>(bytes_.data() + start), position_ - start);
        if (c == '.') real = true;
        // Signs past the first character are dropped (a lenient reader's choice).
        std::string clean;
        for (std::size_t i = 0; i < text.size(); ++i)
            if (i == 0 || (text[i] != '-' && text[i] != '+')) clean.push_back(text[i]);
        if (real) {
            auto made = make(Kind::real);
            try { made->real = std::stod(clean == "-" || clean == "+" || clean == "." || clean == "-." ? "0" : clean); } catch (...) { made->real = 0; }
            return made;
        }
        std::int64_t value{};
        const auto parsed = std::from_chars(clean.data() + (clean[0] == '+' ? 1 : 0), clean.data() + clean.size(), value);
        if (parsed.ec != std::errc()) value = 0;
        // "n g R": a reference.
        if (references && value >= 0 && clean[0] != '+' && clean[0] != '-') {
            const auto save = position_;
            skip_space();
            const auto g_start = position_;
            while (position_ < bytes_.size() && bytes_[position_] >= '0' && bytes_[position_] <= '9') ++position_;
            if (position_ > g_start && (position_ >= bytes_.size() || !is_regular(bytes_[position_]) || bytes_[position_] == 'R')) {
                std::uint32_t generation{};
                std::from_chars(reinterpret_cast<const char*>(bytes_.data() + g_start), reinterpret_cast<const char*>(bytes_.data() + position_), generation);
                skip_space();
                if (position_ < bytes_.size() && bytes_[position_] == 'R' && (position_ + 1 >= bytes_.size() || !is_regular(bytes_[position_ + 1]))) {
                    ++position_;
                    auto made = make(Kind::reference);
                    made->number = static_cast<std::uint32_t>(value);
                    made->generation = generation;
                    return made;
                }
            }
            position_ = save;
        }
        auto made = make(Kind::integer);
        made->integer = value;
        return made;
    }
    // Keywords.
    const auto start = position_;
    while (position_ < bytes_.size() && is_regular(bytes_[position_])) ++position_;
    if (position_ == start) { ++position_; return token(references, depth); }   // an unexpected byte
    std::string word(reinterpret_cast<const char*>(bytes_.data() + start), position_ - start);
    if (word == "true" || word == "false") {
        auto made = make(Kind::boolean);
        made->boolean = word == "true";
        return made;
    }
    if (word == "null") return make_null();
    auto made = make(Kind::keyword);
    made->text = std::move(word);
    return made;
}

// ---- decoders ----------------------------------------------------------------------------
std::optional<std::vector<std::uint8_t>> decode_flate(std::span<const std::uint8_t> input) {
    auto whole = export_fill::zlib_decompress(input, max_decoded);
    if (whole.status == export_fill::InflateStatus::ok) return std::move(whole.output);
    // Damaged or truncated streams: what decodes.
    export_fill::InflateStream stream(export_fill::Wrapper::automatic);
    stream.verify(false);
    static_cast<void>(stream.feed(input));
    if (!stream.output().empty()) return std::move(stream.output());
    auto raw = export_fill::inflate_raw(input, max_decoded);
    if (!raw.output.empty()) return std::move(raw.output);
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> decode_lzw(std::span<const std::uint8_t> input, bool early_change) {
    std::vector<std::uint8_t> out;
    std::vector<std::vector<std::uint8_t>> table;
    const auto reset = [&] {
        table.clear();
        for (int i = 0; i < 256; ++i) table.push_back({static_cast<std::uint8_t>(i)});
        table.emplace_back();   // 256: clear
        table.emplace_back();   // 257: end
    };
    reset();
    int width = 9;
    std::uint32_t buffer = 0;
    int bits = 0;
    std::size_t at = 0;
    std::optional<std::uint32_t> previous;
    for (;;) {
        while (bits < width && at < input.size()) {
            buffer = (buffer << 8) | input[at++];
            bits += 8;
        }
        if (bits < width) break;
        const auto code = (buffer >> (bits - width)) & ((1u << width) - 1);
        bits -= width;
        if (code == 256) { reset(); width = 9; previous.reset(); continue; }
        if (code == 257) break;
        std::vector<std::uint8_t> entry;
        if (code < table.size()) entry = table[code];
        else if (previous && code == table.size()) {
            entry = table[*previous];
            entry.push_back(entry.front());
        } else return out.empty() ? std::nullopt : std::optional(out);
        out.insert(out.end(), entry.begin(), entry.end());
        if (out.size() > max_decoded) return std::nullopt;
        if (previous && table.size() < 4096) {
            auto added = table[*previous];
            added.push_back(entry.front());
            table.push_back(std::move(added));
        }
        previous = code;
        const auto limit = table.size() + (early_change ? 1 : 0);
        if (limit >= 4096) width = 12;
        else if (limit >= 2048) width = 12;
        else if (limit >= 1024) width = 11;
        else if (limit >= 512) width = 10;
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> decode_ascii_hex(std::span<const std::uint8_t> input) {
    std::vector<std::uint8_t> out;
    int high = -1;
    for (const auto c : input) {
        if (c == '>') break;
        const auto v = hex_value(c);
        if (v < 0) {
            if (is_space(c)) continue;
            return std::nullopt;
        }
        if (high < 0) high = v;
        else { out.push_back(static_cast<std::uint8_t>(high * 16 + v)); high = -1; }
    }
    if (high >= 0) out.push_back(static_cast<std::uint8_t>(high * 16));
    return out;
}

std::optional<std::vector<std::uint8_t>> decode_ascii85(std::span<const std::uint8_t> input) {
    std::vector<std::uint8_t> out;
    std::array<std::uint32_t, 5> group{};
    int count = 0;
    std::size_t at = 0;
    if (input.size() >= 2 && input[0] == '<' && input[1] == '~') at = 2;
    for (; at < input.size(); ++at) {
        const auto c = input[at];
        if (is_space(c)) continue;
        if (c == '~') break;
        if (c == 'z' && count == 0) { out.insert(out.end(), 4, 0); continue; }
        if (c < '!' || c > 'u') return std::nullopt;
        group[static_cast<std::size_t>(count++)] = c - '!';
        if (count == 5) {
            std::uint64_t value = 0;
            for (const auto g : group) value = value * 85 + g;
            for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
            count = 0;
        }
    }
    if (count > 1) {
        for (int i = count; i < 5; ++i) group[static_cast<std::size_t>(i)] = 84;
        std::uint64_t value = 0;
        for (const auto g : group) value = value * 85 + g;
        for (int i = 0; i < count - 1; ++i) out.push_back(static_cast<std::uint8_t>((value >> (8 * (3 - i))) & 0xff));
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> decode_run_length(std::span<const std::uint8_t> input) {
    std::vector<std::uint8_t> out;
    std::size_t at = 0;
    while (at < input.size()) {
        const auto length = input[at++];
        if (length == 128) break;
        if (length < 128) {
            const auto n = std::min<std::size_t>(length + 1u, input.size() - at);
            out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(at), input.begin() + static_cast<std::ptrdiff_t>(at + n));
            at += n;
        } else {
            if (at >= input.size()) break;
            out.insert(out.end(), 257u - length, input[at++]);
        }
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> unpredict(std::vector<std::uint8_t> data, int predictor, int colors, int bits, int columns) {
    if (predictor <= 1) return data;
    colors = std::max(1, colors);
    bits = std::max(1, bits);
    columns = std::max(1, columns);
    const auto row = static_cast<std::size_t>((colors * bits * columns + 7) / 8);
    const auto pixel = static_cast<std::size_t>(std::max(1, colors * bits / 8));
    if (predictor == 2) {
        if (bits != 8) return data;   // TIFF prediction of other depths: left as is
        for (std::size_t start = 0; start + row <= data.size(); start += row)
            for (std::size_t i = pixel; i < row; ++i) data[start + i] = static_cast<std::uint8_t>(data[start + i] + data[start + i - pixel]);
        return data;
    }
    // PNG: each row starts with its filter type.
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> previous(row, 0), current(row, 0);
    std::size_t at = 0;
    while (at < data.size()) {
        const auto type = data[at++];
        const auto n = std::min(row, data.size() - at);
        std::fill(current.begin(), current.end(), 0);
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(at), n, current.begin());
        at += n;
        for (std::size_t i = 0; i < row; ++i) {
            const int left = i >= pixel ? current[i - pixel] : 0;
            const int up = previous[i];
            const int corner = i >= pixel ? previous[i - pixel] : 0;
            int value = current[i];
            switch (type) {
            case 1: value += left; break;
            case 2: value += up; break;
            case 3: value += (left + up) / 2; break;
            case 4: {
                const int p = left + up - corner;
                const int pa = std::abs(p - left), pb = std::abs(p - up), pc = std::abs(p - corner);
                value += pa <= pb && pa <= pc ? left : pb <= pc ? up : corner;
                break;
            }
            default: break;
            }
            current[i] = static_cast<std::uint8_t>(value & 0xff);
        }
        out.insert(out.end(), current.begin(), current.begin() + static_cast<std::ptrdiff_t>(n));
        previous = current;
    }
    return out;
}

// ---- text and dates --------------------------------------------------------------------------
std::u16string text_string(std::string_view bytes) {
    std::u16string out;
    const auto* u = reinterpret_cast<const unsigned char*>(bytes.data());
    if (bytes.size() >= 2 && u[0] == 0xfe && u[1] == 0xff) {
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) out.push_back(static_cast<char16_t>((u[i] << 8) | u[i + 1]));
        return out;
    }
    if (bytes.size() >= 2 && u[0] == 0xff && u[1] == 0xfe) {
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) out.push_back(static_cast<char16_t>(u[i] | (u[i + 1] << 8)));
        return out;
    }
    if (bytes.size() >= 3 && u[0] == 0xef && u[1] == 0xbb && u[2] == 0xbf) {
        for (std::size_t i = 3; i < bytes.size();) {
            std::uint32_t code = u[i];
            int extra = code >= 0xf0 ? 3 : code >= 0xe0 ? 2 : code >= 0xc0 ? 1 : 0;
            if (extra) code &= (0x3fu >> extra);
            ++i;
            for (; extra > 0 && i < bytes.size(); --extra, ++i) code = (code << 6) | (u[i] & 0x3f);
            if (code >= 0x10000) {
                code -= 0x10000;
                out.push_back(static_cast<char16_t>(0xd800 + (code >> 10)));
                out.push_back(static_cast<char16_t>(0xdc00 + (code & 0x3ff)));
            } else out.push_back(static_cast<char16_t>(code));
        }
        return out;
    }
    // PDFDocEncoding (ISO 32000-1, Annex D).
    static constexpr std::array<char16_t, 8> low{0x02d8, 0x02c7, 0x02c6, 0x02d9, 0x02dd, 0x02db, 0x02da, 0x02dc};   // 0x18-0x1f
    static constexpr std::array<char16_t, 33> high{
        0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044, 0x2039, 0x203a, 0x2212, 0x2030, 0x201e, 0x201c, 0x201d, 0x2018,
        0x2019, 0x201a, 0x2122, 0xfb01, 0xfb02, 0x0141, 0x0152, 0x0160, 0x0178, 0x017d, 0x0131, 0x0142, 0x0153, 0x0161, 0x017e, 0xfffd,
        0x20ac};   // 0x80-0xa0
    for (const auto ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x18 && c <= 0x1f) out.push_back(low[c - 0x18]);
        else if (c == 0x7f) out.push_back(0xfffd);
        else if (c >= 0x80 && c <= 0xa0) out.push_back(high[c - 0x80]);
        else out.push_back(c);
    }
    return out;
}

std::optional<double> date_seconds(std::string_view text) {
    if (text.starts_with("D:")) text.remove_prefix(2);
    std::size_t at = 0;
    const auto digits = [&](std::size_t count, int fallback) -> std::optional<int> {
        if (at + count > text.size()) return fallback;
        int value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto c = text[at + i];
            if (c < '0' || c > '9') return i == 0 ? std::optional<int>(fallback) : std::nullopt;
            value = value * 10 + (c - '0');
        }
        at += count;
        return value;
    };
    if (text.size() < 4) return std::nullopt;
    const auto year = digits(4, -1);
    if (!year || *year < 0) return std::nullopt;
    const auto month = digits(2, 1), day = digits(2, 1), hour = digits(2, 0), minute = digits(2, 0), second = digits(2, 0);
    if (!month || !day || !hour || !minute || !second) return std::nullopt;
    int offset_minutes = 0;
    if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
        const int sign = text[at] == '-' ? -1 : 1;
        ++at;
        const auto oh = digits(2, 0);
        if (at < text.size() && text[at] == '\'') ++at;
        const auto om = digits(2, 0);
        offset_minutes = sign * ((oh ? *oh : 0) * 60 + (om ? *om : 0));
    }
    // Days from 1970-01-01 (Howard Hinnant's days-from-civil).
    const int y = *year - (*month <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = static_cast<unsigned>((*month + 9) % 12);
    const unsigned doy = (153 * mp + 2) / 5 + static_cast<unsigned>(*day) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const auto days = static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
    const auto unix_seconds = static_cast<double>(days) * 86400 + *hour * 3600 + *minute * 60 + *second - offset_minutes * 60;
    return unix_seconds - 978307200.0;
}

// ---- documents ----------------------------------------------------------------------------
std::shared_ptr<Document> Document::open(std::shared_ptr<const std::vector<std::uint8_t>> bytes, std::string* error) {
    if (!bytes) { if (error) *error = "no data"; return nullptr; }
    auto document = std::shared_ptr<Document>(new Document());
    document->bytes_ = std::move(bytes);
    const auto data = document->bytes();
    const auto header = find(data.first(std::min<std::size_t>(data.size(), 1024)), "%PDF-", 0);
    if (header == std::string_view::npos) { if (error) *error = "no %PDF- header"; return nullptr; }
    if (header + 8 <= data.size()) {
        document->major_ = std::max(1, data[header + 5] - '0');
        if (data[header + 6] == '.' && data[header + 7] >= '0' && data[header + 7] <= '9') document->minor_ = data[header + 7] - '0';
    }
    if (!document->load()) { if (error) *error = "no document catalog"; return nullptr; }
    return document;
}

bool Document::load() {
    const auto data = bytes();
    bool loaded = false;
    const auto tail = data.size() > 4096 ? data.size() - 4096 : 0;
    const auto marker = rfind(data.subspan(tail), "startxref");
    if (marker != std::string_view::npos) {
        Lexer lexer(data, tail + marker + 9);
        if (const auto offset = lexer.next(false); offset && (*offset)->kind == Kind::integer && (*offset)->integer >= 0)
            loaded = read_xref_chain(static_cast<std::size_t>((*offset)->integer));
    }
    if (!loaded || !trailer_ || !get(trailer_, "Root") || get(trailer_, "Root")->kind != Kind::dictionary) {
        entries_.clear();
        cache_.clear();
        object_streams_.clear();
        trailer_.reset();
        reconstruct();
        reconstructed_ = true;
    }
    if (!trailer_) return false;
    const auto root = catalog();
    if (!root || root->kind != Kind::dictionary) return false;
    encrypted_ = trailer_->dictionary.find("Encrypt") != nullptr;
    if (const auto version = get(root, "Version"); version && version->kind == Kind::name && version->text.size() >= 3) {
        const int major = version->text[0] - '0', minor = version->text[2] - '0';
        if (major > major_ || (major == major_ && minor > minor_)) { major_ = major; minor_ = minor; }
    }
    collect_pages();
    return true;
}

bool Document::read_xref_chain(std::size_t start) {
    std::set<std::size_t> visited;
    std::optional<std::size_t> next = start;
    bool any = false;
    while (next && !visited.contains(*next) && *next < bytes().size()) {
        const auto offset = *next;
        visited.insert(offset);
        next.reset();
        ObjectPtr section_trailer;
        Lexer probe(bytes(), offset);
        probe.skip_space();
        const bool table = find(bytes(), "xref", probe.position()) == probe.position();
        if (!(table ? read_xref_table(probe.position(), section_trailer) : read_xref_stream(offset, section_trailer))) break;
        any = true;
        if (!trailer_) trailer_ = section_trailer;
        // A hybrid file's cross-reference stream (entries not given by the table).
        if (const auto hybrid = section_trailer->dictionary.find("XRefStm"); hybrid && hybrid->kind == Kind::integer && hybrid->integer >= 0) {
            ObjectPtr ignored;
            static_cast<void>(read_xref_stream(static_cast<std::size_t>(hybrid->integer), ignored));
        }
        if (const auto previous = section_trailer->dictionary.find("Prev"); previous && previous->kind == Kind::integer && previous->integer >= 0)
            next = static_cast<std::size_t>(previous->integer);
    }
    return any;
}

bool Document::read_xref_table(std::size_t offset, ObjectPtr& trailer_out) {
    Lexer lexer(bytes(), offset + 4);
    for (;;) {
        auto first = lexer.next(false);
        if (!first) return false;
        if ((*first)->kind == Kind::keyword && (*first)->text == "trailer") break;
        auto count = lexer.next(false);
        if ((*first)->kind != Kind::integer || !count || (*count)->kind != Kind::integer) return false;
        for (std::int64_t i = 0; i < (*count)->integer; ++i) {
            auto where = lexer.next(false), generation = lexer.next(false), type = lexer.next(false);
            if (!where || !generation || !type || (*where)->kind != Kind::integer || (*type)->kind != Kind::keyword) return false;
            const auto number = static_cast<std::uint32_t>((*first)->integer + i);
            if (entries_.contains(number)) continue;
            Entry entry;
            if ((*type)->text == "n") { entry.type = Entry::Type::offset; entry.value = static_cast<std::uint64_t>((*where)->integer); }
            entries_[number] = entry;
        }
    }
    auto dictionary = lexer.next(true);
    if (!dictionary || (*dictionary)->kind != Kind::dictionary) return false;
    trailer_out = *dictionary;
    return true;
}

bool Document::read_xref_stream(std::size_t offset, ObjectPtr& trailer_out) {
    const auto stream = parse_indirect_at(offset, 0);
    if (!stream || stream->kind != Kind::stream) return false;
    const auto type = stream->dictionary.find("Type");
    if (!type || type->kind != Kind::name || type->text != "XRef") return false;
    const auto data = stream_data(*stream);
    const auto widths = stream->dictionary.find("W");
    if (!data || !widths || widths->kind != Kind::array || widths->array.size() < 3) return false;
    std::array<std::size_t, 3> w{};
    for (std::size_t i = 0; i < 3; ++i) w[i] = static_cast<std::size_t>(std::max<std::int64_t>(0, widths->array[i]->integer));
    std::vector<std::pair<std::int64_t, std::int64_t>> ranges;
    if (const auto index = stream->dictionary.find("Index"); index && index->kind == Kind::array) {
        for (std::size_t i = 0; i + 1 < index->array.size(); i += 2) ranges.emplace_back(index->array[i]->integer, index->array[i + 1]->integer);
    } else if (const auto size = stream->dictionary.find("Size"); size && size->kind == Kind::integer) {
        ranges.emplace_back(0, size->integer);
    }
    const auto field = [&](std::size_t at, std::size_t width) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < width; ++i) value = (value << 8) | (*data)[at + i];
        return value;
    };
    const auto record = w[0] + w[1] + w[2];
    std::size_t at = 0;
    for (const auto& [first, count] : ranges)
        for (std::int64_t i = 0; i < count && at + record <= data->size(); ++i, at += record) {
            const auto number = static_cast<std::uint32_t>(first + i);
            const auto type_value = w[0] ? field(at, w[0]) : 1;
            if (entries_.contains(number)) continue;
            Entry entry;
            if (type_value == 1) { entry.type = Entry::Type::offset; entry.value = field(at + w[0], w[1]); }
            else if (type_value == 2) {
                entry.type = Entry::Type::compressed;
                entry.value = field(at + w[0], w[1]);
                entry.index = static_cast<std::uint32_t>(field(at + w[0] + w[1], w[2]));
            }
            entries_[number] = entry;
        }
    // The stream's dictionary holds the trailer entries.
    auto dictionary = make(Kind::dictionary);
    dictionary->dictionary = stream->dictionary;
    trailer_out = dictionary;
    return true;
}

std::size_t Document::stream_length(const Dictionary& dictionary, std::size_t data_start) {
    const auto data = bytes();
    std::optional<std::size_t> declared;
    if (auto length = dictionary.find("Length")) {
        if (length->kind == Kind::reference) length = resolve(length);
        if (length && length->kind == Kind::integer && length->integer >= 0) declared = static_cast<std::size_t>(length->integer);
    }
    if (declared && data_start + *declared <= data.size()) {
        Lexer check(data, data_start + *declared);
        check.skip_space();
        if (find(data, "endstream", check.position()) == check.position()) return *declared;
    }
    // Search for the end instead.
    const auto end = find(data, "endstream", data_start);
    if (end == std::string_view::npos) return declared ? std::min(*declared, data.size() - data_start) : data.size() - data_start;
    auto length = end - data_start;
    if (length > 0 && data[data_start + length - 1] == '\n') --length;
    if (length > 0 && data[data_start + length - 1] == '\r') --length;
    return length;
}

ObjectPtr Document::parse_indirect_at(std::size_t offset, std::uint32_t expected) {
    const auto data = bytes();
    if (offset >= data.size()) return nullptr;
    Lexer lexer(data, offset);
    const auto number = lexer.next(false), generation = lexer.next(false), keyword = lexer.next(false);
    if (!number || !generation || !keyword || (*number)->kind != Kind::integer || (*keyword)->kind != Kind::keyword || (*keyword)->text != "obj")
        return nullptr;
    if (expected && static_cast<std::uint32_t>((*number)->integer) != expected) return nullptr;
    auto value = lexer.next(true);
    if (!value) return make_null();
    auto following = lexer.next(false);
    if ((*value)->kind == Kind::dictionary && following && (*following)->kind == Kind::keyword && (*following)->text == "stream") {
        auto position = lexer.position();
        if (position < data.size() && data[position] == '\r') ++position;
        if (position < data.size() && data[position] == '\n') ++position;
        auto stream = make(Kind::stream);
        stream->dictionary = (*value)->dictionary;
        stream->data_offset = position;
        stream->data_length = stream_length(stream->dictionary, position);
        stream->number = static_cast<std::uint32_t>((*number)->integer);
        stream->generation = static_cast<std::uint32_t>((*generation)->integer);
        return stream;
    }
    return *value;
}

ObjectPtr Document::from_object_stream(std::uint32_t stream_number, std::uint32_t index, std::uint32_t number) {
    auto found = object_streams_.find(stream_number);
    if (found == object_streams_.end()) {
        std::vector<ObjectPtr> objects;
        const auto stream = indirect(stream_number);
        if (stream && stream->kind == Kind::stream) {
            const auto count = stream->dictionary.find("N"), first = stream->dictionary.find("First");
            const auto data = stream_data(*stream);
            if (data && count && first && count->kind == Kind::integer && first->kind == Kind::integer) {
                Lexer header(*data);
                std::vector<std::pair<std::uint32_t, std::size_t>> offsets;
                for (std::int64_t i = 0; i < count->integer; ++i) {
                    const auto n = header.next(false), o = header.next(false);
                    if (!n || !o || (*n)->kind != Kind::integer || (*o)->kind != Kind::integer) break;
                    offsets.emplace_back(static_cast<std::uint32_t>((*n)->integer), static_cast<std::size_t>((*o)->integer));
                }
                for (const auto& [object_number, offset] : offsets) {
                    Lexer body(*data, static_cast<std::size_t>(first->integer) + offset);
                    auto value = body.next(true);
                    auto copy = value ? std::make_shared<Object>(**value) : std::make_shared<Object>();
                    copy->number = object_number;   // which object it is (for lookups by number)
                    objects.push_back(std::move(copy));
                }
            }
        }
        found = object_streams_.emplace(stream_number, std::move(objects)).first;
    }
    const auto& objects = found->second;
    if (index < objects.size() && objects[index]->number == number) return objects[index];
    for (const auto& object : objects) if (object->number == number) return object;
    return nullptr;
}

ObjectPtr Document::indirect(std::uint32_t number) {
    if (const auto cached = cache_.find(number); cached != cache_.end()) return cached->second;
    if (loading_.contains(number)) return make_null();
    const auto entry = entries_.find(number);
    if (entry == entries_.end() || entry->second.type == Entry::Type::free) return make_null();
    loading_.insert(number);
    ObjectPtr value;
    try {
        if (entry->second.type == Entry::Type::offset) {
            value = parse_indirect_at(static_cast<std::size_t>(entry->second.value), number);
            if (!value) {
                // A wrong offset: look for the object's header near it, then anywhere.
                const auto header = std::to_string(number) + " ";
                for (std::size_t at = find(bytes(), header, 0); at != std::string_view::npos && !value; at = find(bytes(), header, at + 1))
                    if (at == 0 || !is_regular(bytes()[at - 1])) value = parse_indirect_at(at, number);
            }
        } else {
            value = from_object_stream(static_cast<std::uint32_t>(entry->second.value), entry->second.index, number);
        }
    } catch (...) {
        loading_.erase(number);
        throw;
    }
    loading_.erase(number);
    if (!value) value = make_null();
    cache_[number] = value;
    return value;
}

ObjectPtr Document::resolve(const ObjectPtr& object) {
    if (!object) return nullptr;
    auto current = object;
    for (int hops = 0; current && current->kind == Kind::reference && hops < 32; ++hops) current = indirect(current->number);
    return current && current->kind == Kind::reference ? make_null() : current;
}

ObjectPtr Document::get(const ObjectPtr& dictionary, std::string_view key) {
    if (!dictionary || (dictionary->kind != Kind::dictionary && dictionary->kind != Kind::stream)) return nullptr;
    const auto value = dictionary->dictionary.find(key);
    return value ? resolve(value) : nullptr;
}

ObjectPtr Document::inherited(const ObjectPtr& page, std::string_view key) {
    auto node = page;
    for (int level = 0; node && level < 64; ++level) {
        if (const auto value = get(node, key)) return value;
        node = get(node, "Parent");
    }
    return nullptr;
}

ObjectPtr Document::catalog() { return trailer_ ? get(trailer_, "Root") : nullptr; }
ObjectPtr Document::info() { return trailer_ ? get(trailer_, "Info") : nullptr; }
ObjectPtr Document::file_id() { return trailer_ ? get(trailer_, "ID") : nullptr; }

void Document::collect_pages() {
    pages_.clear();
    std::set<const Object*> visited;
    const std::function<void(const ObjectPtr&, int)> walk = [&](const ObjectPtr& node, int depth) {
        if (!node || node->kind != Kind::dictionary || depth > 64 || !visited.insert(node.get()).second) return;
        const auto kids = get(node, "Kids");
        const auto type = get(node, "Type");
        if (kids && kids->kind == Kind::array && !(type && type->kind == Kind::name && type->text == "Page")) {
            for (const auto& kid : kids->array) walk(resolve(kid), depth + 1);
            return;
        }
        pages_.push_back(node);
    };
    walk(get(catalog(), "Pages"), 0);
}

void Document::reconstruct() {
    const auto data = bytes();
    // "n g obj" headers: the last definition of each number wins.
    for (std::size_t at = find(data, "obj", 0); at != std::string_view::npos; at = find(data, "obj", at + 3)) {
        if (at + 3 < data.size() && is_regular(data[at + 3])) continue;
        std::size_t p = at;
        while (p > 0 && is_space(data[p - 1])) --p;
        const auto g_end = p;
        while (p > 0 && data[p - 1] >= '0' && data[p - 1] <= '9') --p;
        if (p == g_end) continue;
        while (p > 0 && is_space(data[p - 1])) --p;
        const auto n_end = p;
        while (p > 0 && data[p - 1] >= '0' && data[p - 1] <= '9') --p;
        if (p == n_end || (p > 0 && is_regular(data[p - 1]))) continue;
        std::uint32_t number{};
        std::from_chars(reinterpret_cast<const char*>(data.data() + p), reinterpret_cast<const char*>(data.data() + n_end), number);
        Entry entry;
        entry.type = Entry::Type::offset;
        entry.value = p;
        entries_[number] = entry;
    }
    // Object streams' contents (objects not defined at top level).
    std::vector<std::uint32_t> streams;
    for (const auto& [number, entry] : entries_) streams.push_back(number);
    for (const auto number : streams) {
        const auto object = indirect(number);
        if (!object || object->kind != Kind::stream) continue;
        const auto type = object->dictionary.find("Type");
        if (!type || type->kind != Kind::name) continue;
        if (type->text == "XRef" && !trailer_) {
            auto dictionary = make(Kind::dictionary);
            dictionary->dictionary = object->dictionary;
            if (dictionary->dictionary.find("Root")) trailer_ = dictionary;
        }
        if (type->text != "ObjStm") continue;
        const auto count = object->dictionary.find("N");
        const auto contents = stream_data(*object);
        if (!count || count->kind != Kind::integer || !contents) continue;
        Lexer header(*contents);
        for (std::int64_t i = 0; i < count->integer; ++i) {
            const auto n = header.next(false), o = header.next(false);
            if (!n || !o || (*n)->kind != Kind::integer) break;
            const auto inner = static_cast<std::uint32_t>((*n)->integer);
            if (entries_.contains(inner)) continue;
            Entry entry;
            entry.type = Entry::Type::compressed;
            entry.value = number;
            entry.index = static_cast<std::uint32_t>(i);
            entries_[inner] = entry;
        }
    }
    // The last trailer with a catalog.
    for (std::size_t at = find(data, "trailer", 0); at != std::string_view::npos; at = find(data, "trailer", at + 7)) {
        Lexer lexer(data, at + 7);
        if (const auto dictionary = lexer.next(true); dictionary && (*dictionary)->kind == Kind::dictionary && (*dictionary)->dictionary.find("Root"))
            trailer_ = *dictionary;
    }
    if (trailer_ && get(trailer_, "Root") && get(trailer_, "Root")->kind == Kind::dictionary) return;
    // No trailer: the catalog by its type.
    for (const auto& [number, entry] : entries_) {
        const auto object = indirect(number);
        const auto type = get(object, "Type");
        if (object && object->kind == Kind::dictionary && type && type->kind == Kind::name && type->text == "Catalog") {
            auto dictionary = make(Kind::dictionary);
            auto root = make(Kind::reference);
            root->number = number;
            dictionary->dictionary.entries.emplace_back("Root", root);
            trailer_ = dictionary;
            return;
        }
    }
}

std::optional<std::vector<std::uint8_t>> Document::stream_data(const Object& stream, DataFormat* format) {
    if (format) *format = DataFormat::raw;
    std::vector<std::uint8_t> data;
    if (stream.own_data) data = *stream.own_data;
    else {
        const auto all = bytes();
        const auto start = std::min(stream.data_offset, all.size());
        const auto length = std::min(stream.data_length, all.size() - start);
        data.assign(all.begin() + static_cast<std::ptrdiff_t>(start), all.begin() + static_cast<std::ptrdiff_t>(start + length));
    }
    auto filters = stream.dictionary.find("Filter");
    if (!filters) filters = stream.dictionary.find("F");
    auto parameters = stream.dictionary.find("DecodeParms");
    if (!parameters) parameters = stream.dictionary.find("DP");
    filters = resolve(filters);
    parameters = resolve(parameters);
    std::vector<ObjectPtr> chain, chain_parameters;
    if (filters && filters->kind == Kind::name) chain.push_back(filters);
    else if (filters && filters->kind == Kind::array) for (const auto& f : filters->array) chain.push_back(resolve(f));
    if (parameters && parameters->kind == Kind::dictionary) chain_parameters.push_back(parameters);
    else if (parameters && parameters->kind == Kind::array) for (const auto& p : parameters->array) chain_parameters.push_back(resolve(p));
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (!chain[i] || chain[i]->kind != Kind::name) return std::nullopt;
        const auto& name = chain[i]->text;
        const auto parameter = i < chain_parameters.size() ? chain_parameters[i] : nullptr;
        const auto integer = [&](std::string_view key, int fallback) {
            const auto value = get(parameter, key);
            return value && value->kind == Kind::integer ? static_cast<int>(value->integer) : fallback;
        };
        std::optional<std::vector<std::uint8_t>> next;
        if (name == "FlateDecode" || name == "Fl") next = decode_flate(data);
        else if (name == "LZWDecode" || name == "LZW") next = decode_lzw(data, integer("EarlyChange", 1) != 0);
        else if (name == "ASCIIHexDecode" || name == "AHx") next = decode_ascii_hex(data);
        else if (name == "ASCII85Decode" || name == "A85") next = decode_ascii85(data);
        else if (name == "RunLengthDecode" || name == "RL") next = decode_run_length(data);
        else if (name == "DCTDecode" || name == "DCT" || name == "JPXDecode") {
            if (format) *format = name == "JPXDecode" ? DataFormat::jpeg2000 : DataFormat::jpeg;
            return data;   // left encoded
        } else return std::nullopt;   // CCITTFaxDecode, JBIG2Decode, Crypt: not decoded here
        if (!next) return std::nullopt;
        if (name == "FlateDecode" || name == "Fl" || name == "LZWDecode" || name == "LZW") {
            const auto predictor = integer("Predictor", 1);
            if (predictor > 1) next = unpredict(std::move(*next), predictor, integer("Colors", 1), integer("BitsPerComponent", 8), integer("Columns", 1));
            if (!next) return std::nullopt;
        }
        data = std::move(*next);
    }
    return data;
}

} // namespace alchemy::gui::quartz2d::pdf
