#include "alchemy/darwin/coregraphics/pdf/pdf_writer.hpp"

#include "alchemy/darwin/compression/zlib_codec.hpp"

#include <cmath>
#include <cstdio>

namespace alchemy::gui::quartz2d::pdf {

int Writer::reserve() {
    bodies_.emplace_back();
    return static_cast<int>(bodies_.size());
}

void Writer::set(int number, std::string body) { bodies_.at(static_cast<std::size_t>(number - 1)) = std::move(body); }

int Writer::add(std::string body) {
    const auto number = reserve();
    set(number, std::move(body));
    return number;
}

std::vector<std::uint8_t> Writer::finish(int root, int info, std::string_view version) const {
    std::string out = "%PDF-" + std::string(version) + "\n%\xe2\xe3\xcf\xd3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + (bodies_[i].empty() ? std::string("null") : bodies_[i]) + "\nendobj\n";
    }
    const auto xref = out.size();
    out += "xref\n0 " + std::to_string(bodies_.size() + 1) + "\n0000000000 65535 f \n";
    for (const auto offset : offsets) {
        char line[24];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", offset);
        out += line;
    }
    // The file identifier: a hash of the content (two equal halves, as for a new file).
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto c : out) hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    char id[40];
    std::snprintf(id, sizeof id, "%016llx%016llx", static_cast<unsigned long long>(hash), static_cast<unsigned long long>(hash * 31 + 7));
    out += "trailer\n<< /Size " + std::to_string(bodies_.size() + 1) + " /Root " + std::to_string(root) + " 0 R";
    if (info) out += " /Info " + std::to_string(info) + " 0 R";
    out += " /ID [<" + std::string(id) + "> <" + std::string(id) + ">] >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return {out.begin(), out.end()};
}

std::string stream_body(std::string_view dictionary_entries, std::span<const std::uint8_t> data, bool compress) {
    std::vector<std::uint8_t> packed;
    if (compress) packed = export_fill::zlib_compress(data, 6);
    const std::span<const std::uint8_t> written = compress ? std::span<const std::uint8_t>(packed) : data;
    std::string out = "<< " + std::string(dictionary_entries);
    if (compress) out += " /Filter /FlateDecode";
    out += " /Length " + std::to_string(written.size()) + " >>\nstream\n";
    out.append(reinterpret_cast<const char*>(written.data()), written.size());
    out += "\nendstream";
    return out;
}

std::string stream_body(std::string_view dictionary_entries, std::string_view data, bool compress) {
    return stream_body(dictionary_entries, std::span(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()), compress);
}

std::string number(double value) {
    if (!std::isfinite(value)) value = 0;
    if (std::abs(value) < 0.00005) return "0";
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.4f", value);
    std::string text = buffer;
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text == "-0" ? "0" : text;
}

std::string name(std::string_view text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out = "/";
    for (const auto ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        const bool plain = c > 32 && c < 127 && c != '#' && c != '/' && c != '(' && c != ')' && c != '<' && c != '>' && c != '[' &&
                           c != ']' && c != '{' && c != '}' && c != '%';
        if (plain) out.push_back(static_cast<char>(c));
        else {
            out.push_back('#');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 15]);
        }
    }
    return out;
}

std::string literal(std::string_view bytes) {
    std::string out = "(";
    for (const auto c : bytes) {
        if (c == '(' || c == ')' || c == '\\') out.push_back('\\');
        if (c == '\r') { out += "\\r"; continue; }
        out.push_back(c);
    }
    return out + ")";
}

std::string text(std::u16string_view units) {
    bool ascii = true;
    for (const auto u : units) if (u < 0x20 || u > 0x7e) { ascii = false; break; }
    if (ascii) {
        std::string bytes;
        for (const auto u : units) bytes.push_back(static_cast<char>(u));
        return literal(bytes);
    }
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out = "<FEFF";
    for (const auto u : units)
        for (int shift = 12; shift >= 0; shift -= 4) out.push_back(digits[(u >> shift) & 15]);
    return out + ">";
}

std::string date(double absolute_seconds) {
    // Civil date from days since 1970-01-01 (Howard Hinnant's civil-from-days).
    const auto unix_seconds = static_cast<std::int64_t>(std::floor(absolute_seconds + 978307200.0));
    auto days = unix_seconds / 86400;
    auto rest = unix_seconds % 86400;
    if (rest < 0) { rest += 86400; --days; }
    days += 719468;
    const auto era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const auto mp = (5 * doy + 2) / 153;
    const auto day = doy - (153 * mp + 2) / 5 + 1;
    const auto month = mp < 10 ? mp + 3 : mp - 9;
    const auto year = static_cast<std::int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "(D:%04lld%02u%02u%02lld%02lld%02lldZ)", static_cast<long long>(year), month, day,
                  static_cast<long long>(rest / 3600), static_cast<long long>(rest / 60 % 60), static_cast<long long>(rest % 60));
    return buffer;
}

} // namespace alchemy::gui::quartz2d::pdf
