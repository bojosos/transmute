// Host-only tests of the compression cores: libcompression (compression.cpp),
// bzip2 (bzip2.cpp) and xar (xar.cpp). Split from the QLab data tests.
#include "alchemy/darwin/compression/bzip2.hpp"
#include "alchemy/darwin/compression/compression.hpp"
#include "alchemy/darwin/compression/xar.hpp"
#include "alchemy/darwin/security/keys.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

// ---- libcompression ----
namespace cmp = alchemy::gui::completion_kit::compression;

std::vector<std::uint8_t> bytes_of(std::string_view text) { return std::vector<std::uint8_t>(text.begin(), text.end()); }

void compression_codecs() {
    // RFC 1951 vectors (raw DEFLATE, no zlib header): a stored block and fixed-Huffman blocks.
    const std::vector<std::uint8_t> stored{0x01, 0x05, 0x00, 0xfa, 0xff, 'h', 'e', 'l', 'l', 'o'};
    require(cmp::decode(cmp::zlib, stored, 100) == bytes_of("hello"), "RFC 1951 stored block");
    const std::vector<std::uint8_t> fixed_a{0x4b, 0x04, 0x00};
    require(cmp::decode(cmp::zlib, fixed_a, 100) == bytes_of("a"), "RFC 1951 fixed Huffman 'a'");
    const std::vector<std::uint8_t> fixed_hello{0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00};
    require(cmp::decode(cmp::zlib, fixed_hello, 100) == bytes_of("hello"), "RFC 1951 fixed Huffman 'hello'");
    require(cmp::decode(cmp::zlib, fixed_hello, 3) == bytes_of("hel"), "decoding truncates to the destination");
    const std::vector<std::uint8_t> bad{0xff, 0xff, 0xff};
    require(!cmp::decode(cmp::zlib, bad, 100), "malformed DEFLATE data");
    // LZ4 block vectors: literals only, and an overlapping match.
    const std::vector<std::uint8_t> literal{0x10, 'a'};
    require(cmp::lz4_block_decode(literal, 100) == bytes_of("a"), "LZ4 literals");
    const std::vector<std::uint8_t> overlap{0x44, 'a', 'b', 'c', 'd', 0x04, 0x00};
    require(cmp::lz4_block_decode(overlap, 100) == bytes_of("abcdabcdabcd"), "LZ4 overlapping match");
    const std::vector<std::uint8_t> bad_offset{0x14, 'a', 0x05, 0x00};
    require(!cmp::lz4_block_decode(bad_offset, 100), "LZ4 offset before the output");
    // Round trips of compressible and incompressible data.
    std::vector<std::uint8_t> text;
    for (int i = 0; i < 200000; ++i) text.push_back(static_cast<std::uint8_t>("QLab cue list "[i % 14]));
    std::vector<std::uint8_t> noise;
    std::uint32_t seed = 1;
    for (int i = 0; i < 70000; ++i) { seed = seed * 1664525u + 1013904223u; noise.push_back(static_cast<std::uint8_t>(seed >> 24)); }
    for (const auto algorithm : {cmp::zlib, cmp::lz4, cmp::lz4_raw})
        for (const auto* input : {&text, &noise}) {
            const auto encoded = cmp::encode(algorithm, *input);
            require(encoded.has_value(), "encode");
            if (input == &text) require(encoded->size() < input->size() / 10, "compressible data shrinks");
            require(cmp::decode(algorithm, *encoded, input->size()) == *input, "round trip of algorithm " + std::to_string(algorithm));
        }
    require(cmp::decode(cmp::lz4, cmp::encode(cmp::lz4, std::vector<std::uint8_t>{}).value(), 10)->empty(), "empty LZ4 frame");
    // Streams, fed in small pieces.
    for (const auto algorithm : {cmp::zlib, cmp::lz4, cmp::lz4_raw}) {
        auto encoder = cmp::Stream::create(0, algorithm);
        std::vector<std::uint8_t> encoded;
        for (std::size_t at = 0; at < text.size(); at += 7777) {
            const auto piece = std::span(text).subspan(at, std::min<std::size_t>(7777, text.size() - at));
            std::size_t consumed = 0;
            require(encoder->process(piece, consumed, at + piece.size() == text.size()) && consumed == piece.size(), "stream encode");
            encoded.insert(encoded.end(), encoder->pending.begin(), encoder->pending.end());
            encoder->pending.clear();
        }
        require(encoder->done(), "encoder done after finalize");
        auto decoder = cmp::Stream::create(1, algorithm);
        std::vector<std::uint8_t> decoded;
        std::size_t at = 0;
        while (at < encoded.size() || !decoder->done()) {
            const auto piece = std::span(encoded).subspan(at, std::min<std::size_t>(1000, encoded.size() - at));
            std::size_t consumed = 0;
            require(decoder->process(piece, consumed, at + piece.size() == encoded.size()), "stream decode");
            at += consumed;
            decoded.insert(decoded.end(), decoder->pending.begin(), decoder->pending.end());
            decoder->pending.clear();
            if (consumed == 0 && at == encoded.size() && !decoder->done()) break;
        }
        require(decoder->done() && decoded == text, "stream round trip of algorithm " + std::to_string(algorithm));
    }
    require(!cmp::Stream::create(0, cmp::lzfse) && !cmp::Stream::create(1, cmp::lzma) && !cmp::encode(cmp::lzfse, text), "LZFSE and LZMA are not provided");
}

// ---- bzip2 and xar ----
namespace bz = alchemy::gui::completion_kit::bzip2;
namespace xa = alchemy::gui::completion_kit::xar;

void bzip2_streams() {
    // CRC-32/BZIP2 check value.
    const auto digits = bytes_of("123456789");
    require(~bz::crc(digits) == 0xfc891918u, "CRC-32/BZIP2 of 123456789");
    // The empty stream: "BZh9", the end-of-stream magic and a zero CRC.
    const std::vector<std::uint8_t> empty{0x42, 0x5a, 0x68, 0x39, 0x17, 0x72, 0x45, 0x38, 0x50, 0x90, 0x00, 0x00, 0x00, 0x00};
    const auto e = bz::decode(empty);
    require(e.error == bz::Error::none && e.output.empty() && e.consumed == empty.size(), "decode the empty stream");
    require(bz::encode({}) == empty, "encode the empty stream");
    // Round trips: text, runs (the initial run-length stage), noise, one byte, periodic data, several blocks.
    std::vector<std::vector<std::uint8_t>> inputs;
    inputs.push_back(bytes_of("hello, hello, hello world\n"));
    inputs.push_back(std::vector<std::uint8_t>(1000, 'a'));
    inputs.push_back(bytes_of("x"));
    std::vector<std::uint8_t> mixed;
    for (int i = 0; i < 5000; ++i) { mixed.insert(mixed.end(), static_cast<std::size_t>(i % 300), static_cast<std::uint8_t>('a' + i % 7)); mixed.push_back('|'); }
    inputs.push_back(mixed);
    std::vector<std::uint8_t> noise;
    std::uint32_t seed = 7;
    for (int i = 0; i < 150000; ++i) { seed = seed * 1103515245u + 12345u; noise.push_back(static_cast<std::uint8_t>(seed >> 16)); }
    inputs.push_back(noise);
    std::vector<std::uint8_t> periodic;
    for (int i = 0; i < 3000; ++i) periodic.push_back(static_cast<std::uint8_t>("ab"[i % 2]));
    inputs.push_back(periodic);
    for (const auto& input : inputs) {
        const auto encoded = bz::encode(input, 1);   // level 1: 100k blocks, so the larger inputs span several
        const auto decoded = bz::decode(encoded);
        require(decoded.error == bz::Error::none && decoded.output == input, "bzip2 round trip of " + std::to_string(input.size()) + " bytes: " + decoded.message);
    }
    // Streams written by another bzip2 implementation (libbzip2, level 9).
    const std::vector<std::uint8_t> hello{0x42, 0x5a, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0xcc, 0x2e, 0x03, 0xa1, 0x00, 0x00, 0x06, 0x51,
                                          0x80, 0x00, 0x10, 0x40, 0x04, 0x06, 0x44, 0x90, 0x80, 0x20, 0x00, 0x21, 0x90, 0x32, 0x04, 0x00, 0xc2, 0xa8,
                                          0x32, 0xd9, 0x41, 0x6e, 0x9c, 0x07, 0x9a, 0xf1, 0x77, 0x24, 0x53, 0x85, 0x09, 0x0c, 0xc2, 0xe0, 0x3a, 0x10};
    const auto h = bz::decode(hello);
    require(h.error == bz::Error::none && h.output == bytes_of("hello, hello, hello world\n"), "decode a libbzip2 stream: " + h.message);
    const std::vector<std::uint8_t> run{0x42, 0x5a, 0x68, 0x39, 0x31, 0x41, 0x59, 0x26, 0x53, 0x59, 0x49, 0xdc, 0x4f, 0x63, 0x00, 0x00, 0x01, 0x81,
                                        0x01, 0xa0, 0x00, 0x00, 0x80, 0x00, 0x08, 0x20, 0x00, 0x20, 0xaa, 0x6d, 0x41, 0x98, 0xba, 0x83, 0xc5, 0xdc,
                                        0x91, 0x4e, 0x14, 0x24, 0x12, 0x77, 0x13, 0xd8, 0xc0};
    const auto r = bz::decode(run);
    require(r.error == bz::Error::none && r.output == std::vector<std::uint8_t>(1000, 'a'), "decode a libbzip2 run-length stream: " + r.message);
    if (std::getenv("ALCHEMY_TEST_DUMP_BZIP2")) {   // cross-check aid: the encoder's output for another decoder
        for (const auto& input : inputs) {
            std::string line;
            for (const auto b : bz::encode(input, 1)) { char hex[4]; std::snprintf(hex, sizeof hex, "%02x", b); line += hex; }
            std::cout << "bzip2 " << input.size() << " " << line << "\n";
        }
    }
    // Errors.
    require(bz::decode(bytes_of("BZx9")).error == bz::Error::magic, "bad magic");
    auto truncated = bz::encode(bytes_of("some text to compress, some text to compress"));
    truncated.resize(truncated.size() - 6);
    require(bz::decode(truncated).error == bz::Error::unexpected_end, "truncated stream");
    auto corrupt = bz::encode(bytes_of("some text to compress, some text to compress"));
    corrupt[12] ^= 0x40;
    require(bz::decode(corrupt).error != bz::Error::none, "corrupt stream");
}

void xar_archives() {
    const auto sha1 = [](std::span<const std::uint8_t> bytes) { return *alchemy::gui::security::digest("SHA1", bytes); };
    for (const auto* compression : {"gzip", "bzip2", "none"}) {
        xa::Archive archive;
        archive.compression = compression;
        auto& folder = archive.add(nullptr, "Contents");
        folder.set("type", "directory");
        auto& file = archive.add(&folder, "Info.plist");
        file.set("type", "file");
        file.set("binary-delta", "true");
        file.contents = bytes_of("<plist>QLab & <friends></plist>");
        auto& empty = archive.add(&folder, "empty");
        empty.set("type", "file");
        empty.contents = std::vector<std::uint8_t>{};
        auto sub = std::make_unique<xa::Subdoc>();
        sub->name = "binary-delta-attributes";
        sub->set("major-version", "2");
        sub->set("before-sha1", "abc");
        archive.subdocs.push_back(std::move(sub));
        const auto bytes = xa::write(archive, sha1);
        require(bytes.size() > 28 && bytes[0] == 'x' && bytes[1] == 'a' && bytes[2] == 'r' && bytes[3] == '!', "xar header");
        std::string error;
        const auto back = xa::read(bytes, error);
        require(back != nullptr, std::string("read a written xar (") + compression + "): " + error);
        const auto all = back->all();
        require(all.size() == 3 && all[1]->path() == "Contents/Info.plist" && *all[1]->property("binary-delta") == "true", "files, paths and properties");
        bool not_provided = false;
        const auto data = xa::extract(*back, *all[1], error, not_provided);
        require(data && *data == bytes_of("<plist>QLab & <friends></plist>"), std::string("extract (") + compression + ")");
        require(xa::extract(*back, *all[2], error, not_provided)->empty(), "empty file");
        require(back->subdocs.size() == 1 && back->subdocs[0]->name == "binary-delta-attributes" && *back->subdocs[0]->property("major-version") == "2", "subdocs");
        // The TOC checksum in the heap is the SHA-1 of the compressed TOC.
        const auto toc_length = static_cast<std::size_t>(bytes[15]) | static_cast<std::size_t>(bytes[14]) << 8;
        const auto expected = sha1(std::span(bytes).subspan(28, toc_length));
        require(std::equal(expected.begin(), expected.end(), bytes.begin() + 28 + static_cast<std::ptrdiff_t>(toc_length)), "TOC checksum");
    }
    std::string error;
    require(!xa::read(bytes_of("not a xar archive at all, really"), error), "not a xar archive");
}

struct Test { const char* name; std::function<void()> run; };

} // namespace

int main() {
    const std::vector<Test> tests = {
        {"libcompression codecs", compression_codecs},
        {"bzip2 streams", bzip2_streams},
        {"xar archives", xar_archives},
    };
    int failed = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "ok   " << test.name << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << test.name << ": " << e.what() << '\n';
        }
    }
    return failed ? 1 : 0;
}
