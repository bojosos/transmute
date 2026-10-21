#include "alchemy/runtime_libraries.hpp"
#include "alchemy/path.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace alchemy {
namespace {

constexpr std::string_view llvm_source = "https://github.com/llvm/llvm-project";
constexpr std::string_view expat_source = "https://github.com/libexpat/libexpat";

constexpr std::string_view swift_source = "https://github.com/swiftlang/swift";

constexpr std::array<RuntimeLibraryIdentity, 14> identities{{
    {"/usr/lib/libc++.1.dylib", llvm_source, "scripts/build_alchemy_llvm.ps1", true},
    {"/usr/lib/libc++abi.dylib", llvm_source, "scripts/build_alchemy_llvm.ps1", true},
    {"/usr/lib/libunwind.dylib", llvm_source, "scripts/build_alchemy_llvm.ps1", true},
    {"/usr/lib/libexpat.1.dylib", expat_source, "scripts/build_alchemy_expat.ps1", false},
    // The Swift runtime and standard library (docs/ALCHEMY_SWIFT.md).
    {"/usr/lib/swift/libswiftCore.dylib", swift_source, "scripts/build_alchemy_swift.ps1", false},
    // Project-authored overlays built by the same script (runtime/swift/overlays).
    {"/usr/lib/swift/libswiftFoundation.dylib", swift_source, "scripts/build_alchemy_swift.ps1", false},
    {"/usr/lib/swift/libswiftos.dylib", swift_source, "scripts/build_alchemy_swift.ps1", false},
    // QLab's Swift overlays (mac/qlab-g6-graphics, runtime/swift/overlays).
    {"/System/Library/Frameworks/CryptoKit.framework/Versions/A/CryptoKit", swift_source, "scripts/build_alchemy_swift.ps1", false},
    {"/usr/lib/swift/libswiftDispatch.dylib", swift_source, "scripts/build_alchemy_swift.ps1", false},
    {"/usr/lib/swift/libswiftOSLog.dylib", swift_source, "scripts/build_alchemy_swift.ps1", false},
    {"/usr/lib/swift/libswiftAVFoundation.dylib", swift_source, "scripts/build_alchemy_swift.ps1", false},
    {"/usr/lib/swift/libswiftCoreAudio.dylib", swift_source, "scripts/build_alchemy_swift.ps1", false},
    {"/usr/lib/swift/libswiftCoreMIDI.dylib", swift_source, "scripts/build_alchemy_swift.ps1", false},
    {"/usr/lib/swift/libswiftCoreMedia.dylib", swift_source, "scripts/build_alchemy_swift.ps1", false},
}};

std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// FIPS 180-4 SHA-256.
class Sha256 {
public:
    void update(const unsigned char* data, std::size_t size) {
        for (std::size_t index = 0; index < size; ++index) {
            block_[used_++] = data[index];
            if (used_ == 64) { compress(); used_ = 0; }
        }
        length_ += size;
    }
    std::string hex() {
        const auto bits = length_ * 8;
        const unsigned char one = 0x80, zero = 0;
        update(&one, 1);
        while (used_ != 56) update(&zero, 1);
        for (int shift = 56; shift >= 0; shift -= 8) {
            const auto byte = static_cast<unsigned char>(bits >> shift);
            update(&byte, 1);
        }
        static constexpr char digits[] = "0123456789abcdef";
        std::string text;
        for (const auto word : state_) for (int shift = 28; shift >= 0; shift -= 4) text.push_back(digits[(word >> shift) & 15U]);
        return text;
    }

private:
    static std::uint32_t rotate(std::uint32_t value, int count) { return (value >> count) | (value << (32 - count)); }
    void compress() {
        static constexpr std::array<std::uint32_t, 64> k{
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::array<std::uint32_t, 64> w{};
        for (int index = 0; index < 16; ++index) {
            w[index] = (std::uint32_t{block_[index * 4]} << 24) | (std::uint32_t{block_[index * 4 + 1]} << 16) |
                       (std::uint32_t{block_[index * 4 + 2]} << 8) | std::uint32_t{block_[index * 4 + 3]};
        }
        for (int index = 16; index < 64; ++index) {
            const auto s0 = rotate(w[index - 15], 7) ^ rotate(w[index - 15], 18) ^ (w[index - 15] >> 3);
            const auto s1 = rotate(w[index - 2], 17) ^ rotate(w[index - 2], 19) ^ (w[index - 2] >> 10);
            w[index] = w[index - 16] + s0 + w[index - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, h] = state_;
        for (int index = 0; index < 64; ++index) {
            const auto t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + k[index] + w[index];
            const auto t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        const std::array<std::uint32_t, 8> add{a, b, c, d, e, f, g, h};
        for (std::size_t index = 0; index < 8; ++index) state_[index] += add[index];
    }
    std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> block_{};
    std::size_t used_{};
    std::uint64_t length_{};
};

// The JSON subset provenance.json uses: objects, arrays, strings, numbers,
// booleans and null. Only strings, objects and arrays are retained.
struct Json {
    enum class Kind { other, string, object, array };
    Kind kind{Kind::other};
    std::string text;
    std::vector<std::string> keys; // object member names, parallel to items
    std::vector<Json> items;       // object member values or array elements
    const Json* member(std::string_view name) const {
        if (kind != Kind::object) return nullptr;
        const Json* found = nullptr; // the last duplicate wins
        for (std::size_t index = 0; index < keys.size(); ++index) if (keys[index] == name) found = &items[index];
        return found;
    }
    const std::string* string() const { return kind == Kind::string ? &text : nullptr; }
    const std::vector<Json>* array() const { return kind == Kind::array ? &items : nullptr; }
};

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {
        if (text_.starts_with("\xEF\xBB\xBF")) text_.remove_prefix(3);
    }
    Json document() {
        auto result = value(0);
        skip();
        if (position_ != text_.size()) fail("trailing data");
        return result;
    }

private:
    [[noreturn]] void fail(const char* reason) const {
        throw std::runtime_error(std::string("malformed provenance.json: ") + reason);
    }
    void skip() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
               text_[position_] == '\r' || text_[position_] == '\n')) ++position_;
    }
    char peek() { skip(); if (position_ >= text_.size()) fail("unexpected end"); return text_[position_]; }
    void expect(char character) { if (peek() != character) fail("unexpected character"); ++position_; }
    void literal(std::string_view word) {
        if (text_.substr(position_, word.size()) != word) fail("invalid literal");
        position_ += word.size();
    }
    unsigned hex4() {
        if (position_ + 4 > text_.size()) fail("truncated escape");
        unsigned code = 0;
        for (int index = 0; index < 4; ++index) {
            const char c = text_[position_++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') code |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= static_cast<unsigned>(c - 'A' + 10);
            else fail("invalid escape");
        }
        return code;
    }
    std::string string() {
        expect('"');
        std::string result;
        while (true) {
            if (position_ >= text_.size()) fail("unterminated string");
            const char c = text_[position_++];
            if (c == '"') return result;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') { result.push_back(c); continue; }
            if (position_ >= text_.size()) fail("unterminated escape");
            const char escape = text_[position_++];
            switch (escape) {
            case '"': case '\\': case '/': result.push_back(escape); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                auto code = hex4();
                if (code >= 0xd800 && code < 0xdc00) {
                    literal("\\u");
                    const auto low = hex4();
                    if (low < 0xdc00 || low >= 0xe000) fail("invalid surrogate pair");
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                } else if (code >= 0xdc00 && code < 0xe000) fail("invalid surrogate");
                if (code < 0x80) result.push_back(static_cast<char>(code));
                else if (code < 0x800) {
                    result.push_back(static_cast<char>(0xc0 | (code >> 6)));
                    result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                } else if (code < 0x10000) {
                    result.push_back(static_cast<char>(0xe0 | (code >> 12)));
                    result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                    result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                } else {
                    result.push_back(static_cast<char>(0xf0 | (code >> 18)));
                    result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
                    result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                    result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                }
                break;
            }
            default: fail("invalid escape");
            }
        }
    }
    Json value(int depth) {
        if (depth > 32) fail("nesting too deep");
        const char c = peek();
        Json result;
        if (c == '{') {
            ++position_;
            result.kind = Json::Kind::object;
            if (peek() == '}') { ++position_; return result; }
            while (true) {
                result.keys.push_back(string());
                expect(':');
                result.items.push_back(value(depth + 1));
                if (peek() == ',') { ++position_; continue; }
                expect('}');
                break;
            }
        } else if (c == '[') {
            ++position_;
            result.kind = Json::Kind::array;
            if (peek() == ']') { ++position_; return result; }
            while (true) {
                result.items.push_back(value(depth + 1));
                if (peek() == ',') { ++position_; continue; }
                expect(']');
                break;
            }
        } else if (c == '"') { result.kind = Json::Kind::string; result.text = string(); }
        else if (c == 't') literal("true");
        else if (c == 'f') literal("false");
        else if (c == 'n') literal("null");
        else if (c == '-' || (c >= '0' && c <= '9')) {
            const auto begin = position_;
            while (position_ < text_.size() && std::string_view("+-.eE0123456789").find(text_[position_]) != std::string_view::npos) ++position_;
            if (position_ == begin) fail("invalid number");
        } else fail("unexpected value");
        return result;
    }

    std::string_view text_;
    std::size_t position_{};
};

std::string read_file(const std::filesystem::path& file, std::uintmax_t limit) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error) throw std::runtime_error("cannot read " + utf8(file));
    if (size > limit) throw std::runtime_error("file exceeds the provenance size limit: " + utf8(file));
    std::ifstream input(file, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(text.size()))) throw std::runtime_error("cannot read " + utf8(file));
    return text;
}

bool same_directory(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code error;
    return std::filesystem::equivalent(left, right, error);
}

} // namespace

const RuntimeLibraryIdentity* runtime_library_identity(std::string_view install_name) noexcept {
    for (const auto& identity : identities) if (identity.install_name == install_name) return &identity;
    return nullptr;
}

std::filesystem::path find_runtime_library(const RuntimeLibraryIdentity& identity,
    std::span<const std::filesystem::path> runtime_directories) {
    const auto filename = utf8_path(identity.install_name).filename();
    for (const auto& directory : runtime_directories) {
        const auto candidate = directory / filename;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) return std::filesystem::canonical(candidate);
    }
    return {};
}

void verify_runtime_library(const RuntimeLibraryIdentity& identity, const std::filesystem::path& file,
    std::span<const std::filesystem::path> runtime_directories) {
    const auto prefix = "guest runtime " + std::string(identity.install_name) + " at " + utf8(file) + ": ";
    const auto directory = file.parent_path();
    bool listed = false;
    for (const auto& candidate : runtime_directories) listed = listed || same_directory(candidate, directory);
    if (!listed) throw std::runtime_error(prefix + "an Apple system identity is accepted only from a --runtime-dir directory");
    if (file.filename() != utf8_path(identity.install_name).filename()) {
        throw std::runtime_error(prefix + "file name does not match the install name");
    }
    const auto provenance_path = directory / "provenance.json";
    std::error_code error;
    if (!std::filesystem::is_regular_file(provenance_path, error)) {
        throw std::runtime_error(prefix + "missing provenance.json (build it with " + std::string(identity.build_script) + ")");
    }
    Json provenance;
    try { provenance = JsonReader(read_file(provenance_path, 4 * 1024 * 1024)).document(); }
    catch (const std::exception& failure) { throw std::runtime_error(prefix + failure.what()); }
    const auto* source = provenance.member("source");
    if (!source || !source->string() || *source->string() != identity.upstream) {
        throw std::runtime_error(prefix + "provenance.json does not record upstream " + std::string(identity.upstream));
    }
    const auto* outputs = provenance.member("outputs");
    const auto name = utf8(file.filename());
    const std::string* recorded = nullptr;
    if (outputs && outputs->array()) for (const auto& output : *outputs->array()) {
        const auto* output_name = output.member("name");
        const auto* hash = output.member("sha256");
        if (output_name && output_name->string() && *output_name->string() == name && hash && hash->string()) recorded = hash->string();
    }
    if (!recorded) throw std::runtime_error(prefix + "provenance.json does not list " + name);
    if (*recorded != sha256_file(file)) throw std::runtime_error(prefix + "SHA-256 differs from provenance.json");
}

std::string sha256_file(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read " + utf8(file));
    Sha256 hash;
    std::vector<char> buffer(1 << 16);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) hash.update(reinterpret_cast<const unsigned char*>(buffer.data()), static_cast<std::size_t>(count));
    }
    if (!input.eof()) throw std::runtime_error("cannot read " + utf8(file));
    return hash.hex();
}

} // namespace alchemy
