// libz (zlib_*.cpp): the codec against streams produced by reference zlib, and the
// guest-visible exports as a renderer plug-in's images call them (split from the export-fill tests).
#include "alchemy/darwin/kit/export_fill/tests/export_fill_test_kit.hpp"

namespace {

// zlib.compress(b'Alchemy zlib interoperability vector. ' * 20, 9) (fixed Huffman).
constexpr std::uint8_t fixed_stream[] = {0x78,0xda,0x73,0xcc,0x49,0xce,0x48,0xcd,0xad,0x54,0xa8,0xca,0xc9,0x4c,0x52,0xc8,
    0xcc,0x2b,0x49,0x2d,0xca,0x2f,0x48,0x2d,0x4a,0x4c,0xca,0xcc,0xc9,0x2c,0xa9,0x54,0x28,0x4b,0x4d,0x2e,0xc9,0x2f,0xd2,0x53,
    0x70,0x1c,0x55,0x35,0xaa,0x6a,0x58,0xa9,0x02,0x00,0xcf,0x8a,0x21,0xac};
// zlib.compress(dynamic_text(), 9): a dynamic-Huffman block.
constexpr std::uint8_t dynamic_stream[] = {
    0x78,0xda,0x6d,0x94,0x57,0xb6,0xe4,0x20,0x0c,0x05,0xb7,0xc4,0x45,0x08,0x04,0xac,
    0x66,0x72,0x7e,0x93,0xe3,0xea,0xe7,0x0a,0xbb,0x1d,0xf9,0x70,0x9f,0x6e,0x1b,0xdb,
    0x2a,0x4e,0x57,0x85,0xf6,0xac,0xa3,0x3d,0x7f,0xf1,0xf2,0xd5,0xeb,0x37,0x6f,0x7b,
    0x6c,0xcb,0x97,0x77,0xef,0x3f,0x7c,0xfc,0xf4,0xf4,0xb9,0x4b,0x3b,0xfe,0xfc,0xf2,
    0xf5,0xdb,0xf7,0x1f,0x3f,0x7f,0xf5,0xd4,0xba,0xb6,0x71,0xba,0xe7,0x76,0xb8,0xda,
    0x4b,0xbb,0x2c,0xfd,0xfd,0xa7,0x5b,0xeb,0xb5,0xf1,0x64,0x47,0x68,0xdb,0xa5,0x0e,
    0xb4,0xd3,0xb2,0xbf,0xff,0x3a,0x62,0xeb,0x90,0xf6,0xd4,0x91,0xda,0x7a,0xa1,0x43,
    0x79,0x2e,0xf3,0x28,0x3c,0xac,0x8d,0xb5,0x1d,0xb5,0xf5,0x18,0x78,0x80,0x47,0x6c,
    0xbc,0xb9,0x47,0xe1,0x57,0x8e,0x15,0xb9,0x3e,0x66,0x22,0xc5,0xb2,0x33,0xd9,0x05,
    0x2a,0xd6,0x29,0x95,0xf0,0x91,0x82,0x95,0x4b,0xe2,0x09,0x4c,0x64,0x42,0x26,0x7c,
    0xa1,0xe8,0x60,0x93,0x7c,0x60,0x93,0x72,0x63,0x13,0xee,0x82,0x54,0xb2,0xa5,0xb0,
    0xb1,0x25,0x8e,0x9f,0xc8,0x9c,0x38,0x7b,0x4a,0x2b,0x5b,0x22,0x40,0x22,0x70,0x22,
    0x70,0xb2,0xc1,0x96,0x88,0xab,0x9c,0x4d,0xb9,0x5e,0x23,0xd9,0x54,0x36,0x36,0x4d,
    0x17,0x36,0xd5,0x29,0x9b,0xf2,0x91,0x5a,0x56,0x36,0xb5,0x13,0x9b,0xd6,0x09,0x5b,
    0xe6,0x0b,0x33,0x06,0x5b,0x8e,0x07,0xb6,0x2c,0x37,0xb6,0xcc,0x6d,0xc8,0x4a,0xb6,
    0x9c,0x37,0xb6,0xcc,0xf1,0x33,0x99,0x33,0x67,0x2f,0x61,0x65,0x2b,0x04,0x28,0x04,
    0x2e,0x04,0x2e,0x69,0xb0,0x15,0xe2,0x16,0xce,0x56,0xb8,0xbe,0x18,0xd9,0x4a,0xdd,
    0xd8,0x2c,0x5c,0xd8,0x0c,0x53,0x36,0xe3,0x23,0x4d,0x56,0x36,0x4b,0x27,0x36,0xd3,
    0xd9,0x3f,0x92,0x2f,0xb4,0x32,0xd8,0xcc,0x0e,0x6c,0x56,0x6f,0x6c,0x95,0xdb,0x50,
    0x41,0xb6,0x1a,0x37,0xb6,0xca,0xf1,0x2b,0x99,0x2b,0x67,0xaf,0x79,0x65,0xab,0x04,
    0xa8,0xfe,0x57,0x27,0x30,0x42,0x18,0x70,0x08,0xf0,0x1f,0xfe,0xb7,0x0e,0xe2,0x1f,
    0xc9,0x5d,0x0b,0xba,0x11,0x22,0xe4,0x0b,0x22,0x42,0x99,0x32,0x22,0x98,0x3f,0xa0,
    0xae,0x94,0xa0,0x4b,0x47,0x4c,0x50,0xa7,0x3b,0x27,0x86,0x51,0x54,0x6a,0xd8,0x47,
    0xab,0x8e,0xfa,0xe9,0xdd,0xbf,0x21,0x1a,0x4d,0xa3,0x81,0x74,0x6d,0x53,0xd0,0x75,
    0x83,0xfb,0x06,0x17,0x0e,0x34,0x6e,0xd5,0xd0,0xa5,0x83,0x5b,0x07,0xd7,0x0e,0xf4,
    0x6e,0x50,0x47,0x77,0x35,0xfa,0xbc,0xd1,0xef,0xa4,0x56,0xa4,0x96,0xbd,0x31,0x90,
    0x6b,0x65,0x20,0xf3,0xce,0xc0,0x0d,0x83,0x3c,0x5a,0x03,0x39,0xd7,0x06,0x32,0xeb,
    0x0d,0xdc,0x35,0xc8,0xda,0x9c,0x74,0x8a,0x4e,0x9a,0x54,0xc7,0x15,0x44,0x5a,0xba,
    0x73,0x08,0x8f,0x8b,0x08,0x37,0x11,0xae,0x22,0xd2,0x16,0x1f,0xd7,0x11,0xee,0x23,
    0x5c,0x48,0xe8,0x12,0x20,0xa8,0x6f,0x86,0xfa,0xbc,0xea,0x77,0xaa,0x47,0x08,0xba,
    0x57,0x08,0x7a,0xcd,0x10,0x74,0xde,0x21,0xb8,0x7b,0xc8,0x8f,0x12,0x21,0x9f,0x53,
    0x84,0x3c,0x6b,0x11,0xdc,0x42,0xe4,0xa5,0x46,0xc8,0xc7,0x1c,0x21,0xdf,0x7b,0x04,
    0x97,0x13,0xd9,0x8b,0x84,0xb2,0x27,0x09,0xae,0x28,0xdc,0x51,0xb8,0xa4,0x28,0x8f,
    0x2c,0xc1,0x45,0x45,0x19,0x25,0xf6,0xed,0x28,0x4b,0x9a,0x50,0x7c,0x33,0xcc,0xe7,
    0x35,0xbf,0xd3,0x3c,0x4f,0xb0,0xbd,0x4f,0xb0,0x6b,0xa0,0x60,0xf3,0x42,0xc1,0xad,
    0x84,0x3d,0x1a,0x05,0x3b,0x47,0x0a,0x36,0xab,0x14,0xdc,0x4f,0xd4,0xa5,0x53,0xa8,
    0xc7,0x50,0xa1,0xde,0x4b,0x05,0xd7,0x16,0xd5,0x5b,0x85,0xba,0xc7,0x0a,0x2e,0x2f,
    0xdc,0x5e,0xb8,0xbe,0xff,0x01,0xa7,0x6d,0x49,0x92,};
std::vector<std::uint8_t> dynamic_text() {
    std::string text;
    const std::string alphabet = "abcdefghijklmnopqrstuvwxyz";
    for (int i = 0; i < 200; ++i) {
        const auto start = static_cast<std::size_t>(i % 26), end = static_cast<std::size_t>((i * 7) % 26 + 1);
        text += std::to_string(i) + ":" + (start < end ? alphabet.substr(start, end - start) : std::string()) + ";";
    }
    return bytes(text);
}

void test_codec() {
    require(vd::crc32_update(0, bytes("123456789")) == 0xcbf43926U, "CRC-32 check value");
    require(vd::adler32_update(1, bytes("Wikipedia")) == 0x11e60398U, "Adler-32 check value");
    // Streams from reference zlib.
    const auto fixed = vd::zlib_decompress(fixed_stream, 1 << 20);
    std::string expected;
    for (int i = 0; i < 20; ++i) expected += "Alchemy zlib interoperability vector. ";
    require(fixed.status == vd::InflateStatus::ok && fixed.output == bytes(expected) && fixed.consumed == sizeof(fixed_stream),
            "fixed-Huffman reference stream");
    const auto dynamic = vd::zlib_decompress(dynamic_stream, 1 << 20);
    require(dynamic.status == vd::InflateStatus::ok && dynamic.output == dynamic_text(), "dynamic-Huffman reference stream");
    require(vd::crc32_update(0, dynamic.output) == 0x0e0bd7deU, "dynamic stream CRC");
    // Errors: truncation, corruption, too small output.
    require(vd::zlib_decompress(std::span(fixed_stream, sizeof(fixed_stream) - 5), 1 << 20).status == vd::InflateStatus::data_error, "truncated stream");
    auto corrupt = std::vector<std::uint8_t>(std::begin(fixed_stream), std::end(fixed_stream));
    corrupt.back() ^= 1;
    require(vd::zlib_decompress(corrupt, 1 << 20).status == vd::InflateStatus::data_error, "bad Adler-32 accepted");
    require(vd::zlib_decompress(fixed_stream, 100).status == vd::InflateStatus::buffer_error, "small output buffer");
    // Round trips at every level, including incompressible data.
    std::vector<std::vector<std::uint8_t>> inputs{{}, bytes("a"), dynamic_text(), bytes(expected)};
    std::vector<std::uint8_t> noise(200000);
    std::uint32_t state = 1;
    for (auto& byte : noise) { state = state * 1103515245U + 12345U; byte = static_cast<std::uint8_t>(state >> 24); }
    inputs.push_back(noise);
    std::vector<std::uint8_t> runs(300000, 'x');
    inputs.push_back(runs);
    for (const auto& input : inputs) {
        for (int level = 0; level <= 9; ++level) {
            const auto packed = vd::zlib_compress(input, level);
            require(packed.size() <= vd::zlib_compress_bound(input.size()), "compressBound exceeded");
            const auto unpacked = vd::zlib_decompress(packed, input.size() + 1);
            require(unpacked.status == vd::InflateStatus::ok && unpacked.output == input, "round trip at level " + std::to_string(level));
        }
    }
    require(vd::zlib_compress(runs, 6).size() < 4000, "runs are not compressed");
}


// gzip.compress(b'Alchemy gzip member ' * 10, mtime=0)
constexpr std::uint8_t gzip_member[] = {0x1f,0x8b,0x08,0x00,0x00,0x00,0x00,0x00,0x02,0x0a,0x73,0xcc,0x49,0xce,0x48,0xcd,
    0xad,0x54,0x48,0xaf,0xca,0x2c,0x50,0xc8,0x4d,0xcd,0x4d,0x4a,0x2d,0x52,0x70,0x1c,0x82,0x62,0x00,0x72,0x82,0x40,0x4e,
    0xc8,0x00,0x00,0x00};
// zlib.compressobj(6, DEFLATED, -15): b'raw deflate text ' * 5
constexpr std::uint8_t raw_stream[] = {0x2b,0x4a,0x2c,0x57,0x48,0x49,0x4d,0xcb,0x49,0x2c,0x49,0x55,0x28,0x49,0xad,0x28,
    0x51,0x28,0xa2,0x82,0x00,0x00};

std::string repeat(std::string_view text, int count) {
    std::string value;
    for (int i = 0; i < count; ++i) value += text;
    return value;
}

// Feeds `input` in `chunk`-byte pieces; returns the output and the bytes consumed.
std::pair<std::vector<std::uint8_t>, std::size_t> inflate_chunked(vd::InflateStream& stream, std::span<const std::uint8_t> input,
                                                                  std::size_t chunk, vd::InflateStream::Status& status) {
    std::vector<std::uint8_t> output;
    std::size_t used = 0;
    status = vd::InflateStream::Status::more;
    for (std::size_t at = 0; at < input.size() && status == vd::InflateStream::Status::more; at += chunk) {
        const auto piece = input.subspan(at, std::min(chunk, input.size() - at));
        const auto result = stream.feed(piece);
        status = result.status;
        used += result.consumed;
        output.insert(output.end(), stream.output().begin(), stream.output().end());
        stream.output().clear();
    }
    return {output, used};
}

void test_streams() {
    using Status = vd::InflateStream::Status;
    Status status{};
    // Reference streams in every split, including one byte at a time.
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
        vd::InflateStream zlib(vd::Wrapper::zlib);
        const auto [output, used] = inflate_chunked(zlib, dynamic_stream, chunk, status);
        require(status == Status::end && output == dynamic_text() && used == sizeof(dynamic_stream), "chunked zlib inflate");
        require(zlib.check() == vd::adler32_update(1, output), "running Adler-32");
        vd::InflateStream gzip(vd::Wrapper::automatic);
        const auto [text, gzip_used] = inflate_chunked(gzip, gzip_member, chunk, status);
        require(status == Status::end && text == bytes(repeat("Alchemy gzip member ", 10)) && gzip_used == sizeof(gzip_member),
                "chunked gzip inflate");
        vd::InflateStream raw(vd::Wrapper::raw);
        const auto [raw_text, raw_used] = inflate_chunked(raw, raw_stream, chunk, status);
        require(status == Status::end && raw_text == bytes(repeat("raw deflate text ", 5)) && raw_used == sizeof(raw_stream),
                "chunked raw inflate");
    }
    // Bytes after the stream end are not consumed.
    {
        std::vector<std::uint8_t> input(std::begin(fixed_stream), std::end(fixed_stream));
        input.insert(input.end(), {1, 2, 3});
        vd::InflateStream stream(vd::Wrapper::zlib);
        const auto result = stream.feed(input);
        require(result.status == Status::end && result.consumed == sizeof(fixed_stream), "trailing bytes consumed");
    }
    // Corruption and checksum verification (inflateValidate).
    {
        std::vector<std::uint8_t> corrupt(std::begin(gzip_member), std::end(gzip_member));
        corrupt[corrupt.size() - 6] ^= 0x40;   // CRC-32
        vd::InflateStream checked(vd::Wrapper::gzip);
        require(checked.feed(corrupt).status == Status::data_error && !checked.message().empty(), "gzip CRC mismatch accepted");
        vd::InflateStream unchecked(vd::Wrapper::gzip);
        unchecked.verify(false);
        require(unchecked.feed(corrupt).status == Status::end, "inflateValidate(0) still checks");
        const std::vector<std::uint8_t> bad{0x78, 0x9c, 0xff, 0xff};
        vd::InflateStream invalid(vd::Wrapper::zlib);
        require(invalid.feed(bad).status == Status::data_error, "invalid block type accepted");
        const std::uint8_t dictionary[] = {0x78, 0xbb, 0x01, 0x02, 0x03, 0x04, 0x00};
        vd::InflateStream preset(vd::Wrapper::zlib);
        require(preset.feed(dictionary).status == Status::need_dictionary && preset.check() == 0x01020304U, "Z_NEED_DICT");
    }
    // Deflate: a sync flush makes everything so far decodable; finish ends
    // the stream; every wrapper round-trips.
    for (const auto wrapper : {vd::Wrapper::zlib, vd::Wrapper::gzip, vd::Wrapper::raw}) {
        for (const int level : {0, 1, 6, 9}) {
            vd::DeflateStream deflater(wrapper, level);
            const auto first = bytes(repeat("first part of the message; ", 40));
            const auto second = dynamic_text();
            deflater.feed(first, vd::DeflateStream::Flush::sync);
            auto packed = deflater.output();
            deflater.output().clear();
            vd::InflateStream inflater(wrapper);
            const auto partial = inflater.feed(packed);
            require(partial.status == Status::more && inflater.output() == first, "sync flush output does not decode completely");
            inflater.output().clear();
            deflater.feed(second, vd::DeflateStream::Flush::none);
            deflater.feed({}, vd::DeflateStream::Flush::finish);
            require(deflater.finished(), "deflate finish");
            const auto rest = deflater.output();
            const auto end = inflater.feed(rest);
            require(end.status == Status::end && inflater.output() == second && end.consumed == rest.size(),
                    "deflate stream round trip");
            if (wrapper == vd::Wrapper::zlib) {
                packed.insert(packed.end(), rest.begin(), rest.end());
                auto whole = first;
                whole.insert(whole.end(), second.begin(), second.end());
                const auto reference = vd::zlib_decompress(packed, whole.size() + 1);
                require(reference.status == vd::InflateStatus::ok && reference.output == whole, "deflate stream is a valid zlib stream");
            }
        }
    }
}

void test_guest_zlib(Fixture& f) {
    const std::string input = "scene data, scene data, scene data, scene data";
    const auto source = f.text(input);
    const auto bound = f.call(libz, "_compressBound", {input.size()});
    const auto packed = f.process.allocate_guest(bound), packed_length = f.process.allocate_guest(8);
    f.engine.write64(packed_length, bound);
    require(f.call(libz, "_compress2", {packed, packed_length, source, input.size(), 9}) == 0, "compress2");
    const auto output = f.process.allocate_guest(256), output_length = f.process.allocate_guest(8);
    f.engine.write64(output_length, 256);
    require(f.call(libz, "_uncompress", {output, output_length, packed, f.engine.read64(packed_length)}) == 0, "uncompress");
    require(f.engine.read64(output_length) == input.size() && f.read(output).substr(0, input.size()) == input, "uncompress output");
    f.engine.write64(output_length, 4);
    require(static_cast<std::int32_t>(f.call(libz, "_uncompress", {output, output_length, packed, f.engine.read64(packed_length)})) == -5,
            "Z_BUF_ERROR");
    require(f.call(libz, "_crc32", {0, f.text("123456789"), 9}) == 0xcbf43926U, "guest crc32");
    require(static_cast<std::int32_t>(f.call(libz, "_compress2", {packed, packed_length, source, input.size(), 12})) == -2, "Z_STREAM_ERROR");
}

void test_guest_zstream(Fixture& f) {
    // z_stream on LP64: next_in 0, avail_in 8, total_in 16, next_out 24,
    // avail_out 32, total_out 40, msg 48, state 56, adler 96; 112 bytes.
    const auto version = f.text("1.2.12");
    const auto text = repeat("Streaming zlib text. ", 300);
    const auto source = f.text(text);
    const auto packed = f.process.allocate_guest(16384), unpacked = f.process.allocate_guest(16384);
    const auto deflater = f.process.allocate_guest(112), inflater = f.process.allocate_guest(112);
    f.engine.fill_bytes(deflater, 0, 112);
    f.engine.fill_bytes(inflater, 0, 112);
    require(static_cast<std::int32_t>(f.call(libz, "_deflateInit2_", {deflater, 6, 8, 31, 8, 0, version, 112})) == 0, "deflateInit2_ gzip");
    require(static_cast<std::int32_t>(f.call(libz, "_deflateInit_", {inflater, 6, version, 100})) == -6, "Z_VERSION_ERROR for a bad size");
    f.engine.write64(deflater + 0, source);
    f.engine.write32(deflater + 8, static_cast<std::uint32_t>(text.size()));
    f.engine.write64(deflater + 24, packed);
    f.engine.write32(deflater + 32, 16384);
    require(static_cast<std::int32_t>(f.call(libz, "_deflate", {deflater, 4})) == 1, "deflate Z_FINISH -> Z_STREAM_END");
    const auto packed_size = f.engine.read64(deflater + 40);
    require(f.engine.read64(deflater + 16) == text.size() && packed_size > 18 && packed_size < text.size() / 4, "deflate totals");
    require(static_cast<std::int32_t>(f.call(libz, "_deflateEnd", {deflater})) == 0, "deflateEnd");
    // Inflate with automatic header detection, 100 bytes in and 64 bytes out at a time.
    require(static_cast<std::int32_t>(f.call(libz, "_inflateInit2_", {inflater, 47, version, 112})) == 0, "inflateInit2_ automatic");
    f.engine.write64(inflater + 0, packed);
    f.engine.write64(inflater + 24, unpacked);
    std::int32_t result = 0;
    for (int round = 0; round < 100000 && result == 0; ++round) {
        const auto consumed = f.engine.read64(inflater + 16);
        if (f.engine.read32(inflater + 8) == 0)
            f.engine.write32(inflater + 8, static_cast<std::uint32_t>(std::min<std::uint64_t>(100, packed_size - consumed)));
        f.engine.write32(inflater + 32, 64);
        result = static_cast<std::int32_t>(f.call(libz, "_inflate", {inflater, 0}));
    }
    require(result == 1, "inflate did not reach Z_STREAM_END");
    std::vector<std::uint8_t> inflated(text.size());
    f.engine.read_bytes(unpacked, inflated);
    require(f.engine.read64(inflater + 40) == text.size() && inflated == bytes(text), "inflate output");
    require(static_cast<std::uint32_t>(f.engine.read64(inflater + 96)) == vd::crc32_update(0, bytes(text)), "gzip strm->adler is the CRC-32");
    require(static_cast<std::int32_t>(f.call(libz, "_inflateEnd", {inflater})) == 0, "inflateEnd");
    require(static_cast<std::int32_t>(f.call(libz, "_inflate", {inflater, 0})) == -2, "inflate after inflateEnd");
    // A corrupt stream reports Z_DATA_ERROR with a message.
    require(static_cast<std::int32_t>(f.call(libz, "_inflateInit_", {inflater, version, 112})) == 0, "inflateInit_");
    const std::vector<std::uint8_t> junk_bytes{0x78, 0x9c, 0xff, 0xff, 0xff};
    const auto junk = f.process.allocate_guest(8);
    f.engine.write_bytes(junk, junk_bytes);
    f.engine.write64(inflater + 0, junk);
    f.engine.write32(inflater + 8, 5);
    f.engine.write64(inflater + 24, unpacked);
    f.engine.write32(inflater + 32, 64);
    require(static_cast<std::int32_t>(f.call(libz, "_inflate", {inflater, 0})) == -3 && f.engine.read64(inflater + 48) != 0, "Z_DATA_ERROR");
    require(f.read(f.call(libz, "_zError", {static_cast<std::uint64_t>(-3)})) == "data error", "zError");
    static_cast<void>(f.call(libz, "_inflateEnd", {inflater}));
}

// libpng's IDAT reader (png_read_IDAT_data): one row of output per inflate
// call series, Z_NO_FLUSH, and a new IDAT chunk only when avail_in is 0. A
// decoder that swallows a chunk while output is still pending makes libpng
// ask for an IDAT that does not exist ("Not enough image data"). Two images
// share one z_stream through inflateReset2, as libpng does per image.
void test_guest_zstream_libpng(Fixture& f) {
    const auto version = f.text("1.2.12");
    const auto inflater = f.process.allocate_guest(112);
    f.engine.fill_bytes(inflater, 0, 112);
    require(static_cast<std::int32_t>(f.call(libz, "_inflateInit_", {inflater, version, 112})) == 0, "inflateInit_ (libpng)");
    for (int image = 0; image < 2; ++image) {
        constexpr std::size_t rows = 96, row_bytes = 1 + 4 * 37;   // filter byte + 37 RGBA pixels
        std::vector<std::uint8_t> pixels(rows * row_bytes);
        for (std::size_t index = 0; index < pixels.size(); ++index)
            pixels[index] = static_cast<std::uint8_t>((index / row_bytes) % 16 == 3 ? (index * 7 + image) & 0xff : 0);   // mostly transparent, like an icon
        // The reference dynamic stream for the first image's chunk test is
        // replaced by both encoders' outputs: stored (level 0) and fixed (6).
        const auto packed_bytes = vd::zlib_compress(pixels, image == 0 ? 6 : 0);
        const auto packed = f.process.allocate_guest(packed_bytes.size() + 16), row = f.process.allocate_guest(row_bytes);
        f.engine.write_bytes(packed, packed_bytes);
        if (image == 1) require(static_cast<std::int32_t>(f.call(libz, "_inflateReset2", {inflater, 15})) == 0, "inflateReset2");
        constexpr std::size_t chunk = 100;   // small IDAT chunks
        std::size_t next_chunk = 0;
        std::int32_t result = 0;
        for (std::size_t r = 0; r < rows; ++r) {
            f.engine.write64(inflater + 24, row);
            f.engine.write32(inflater + 32, static_cast<std::uint32_t>(row_bytes));
            while (f.engine.read32(inflater + 32) > 0) {
                if (f.engine.read32(inflater + 8) == 0) {
                    require(next_chunk < packed_bytes.size(), "libpng: Not enough image data (input consumed while output was pending)");
                    const auto size = std::min(chunk, packed_bytes.size() - next_chunk);
                    f.engine.write64(inflater + 0, packed + next_chunk);
                    f.engine.write32(inflater + 8, static_cast<std::uint32_t>(size));
                    next_chunk += size;
                }
                result = static_cast<std::int32_t>(f.call(libz, "_inflate", {inflater, 0}));
                if (result == 1) break;
                require(result == 0, "libpng: inflate error");
            }
            std::vector<std::uint8_t> got(row_bytes);
            f.engine.read_bytes(row, got);
            require(std::equal(got.begin(), got.end(), pixels.begin() + static_cast<std::ptrdiff_t>(r * row_bytes)), "libpng: row data");
            if (result == 1) require(r + 1 == rows && f.engine.read32(inflater + 32) == 0, "libpng: stream ended early");
        }
        // The trailer arrives with the last chunk (libpng then sees Z_STREAM_END).
        while (result == 0) {
            if (f.engine.read32(inflater + 8) == 0) {
                if (next_chunk >= packed_bytes.size()) break;
                const auto size = std::min(chunk, packed_bytes.size() - next_chunk);
                f.engine.write64(inflater + 0, packed + next_chunk);
                f.engine.write32(inflater + 8, static_cast<std::uint32_t>(size));
                next_chunk += size;
            }
            f.engine.write32(inflater + 32, 0);
            result = static_cast<std::int32_t>(f.call(libz, "_inflate", {inflater, 0}));
            if (result == -5) result = 0;   // Z_BUF_ERROR without output space is not fatal
        }
        require(result == 1, "libpng: Z_STREAM_END after the last row");
    }
    static_cast<void>(f.call(libz, "_inflateEnd", {inflater}));
}

// The gz* file interface (zlib_files.cpp).
void test_guest_gzfiles(Fixture& f) {
    const auto file = std::filesystem::temp_directory_path() / "alchemy-gzfile-test.gz";
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    const auto path = f.text(alchemy::guest_path_string(file));
    const auto writer = f.call(libz, "_gzopen", {path, f.text("wb9")});
    require(writer != 0, "gzopen wb9");
    require(f.call(libz, "_gzwrite", {writer, f.text("hello gz\n"), 9}) == 9, "gzwrite");
    alchemy::CallRegs registers;
    registers.x[0] = writer; registers.x[1] = f.text("%d-%s\n");
    registers.push_variadic(Address{42});
    registers.push_variadic(f.text("x"));
    require(alchemy::engine_call_regs(f.engine, f.symbol(libz, "_gzprintf"), registers).x0 == 5, "gzprintf");
    require(f.call(libz, "_gzputs", {writer, f.text("line3\n")}) == 6 && f.call(libz, "_gzputc", {writer, '!'}) == '!', "gzputs / gzputc");
    require(f.call(libz, "_gztell", {writer}) == 21 && f.call(libz, "_gzclose", {writer}) == 0, "gzclose");
    {
        std::ifstream in(file, std::ios::binary);
        const auto first = in.get(), second = in.get();
        require(first == 0x1f && second == 0x8b, "the file is gzip");
    }
    const auto appender = f.call(libz, "_gzopen", {path, f.text("ab")});
    require(f.call(libz, "_gzwrite", {appender, f.text("more"), 4}) == 4 && f.call(libz, "_gzclose", {appender}) == 0, "a second member");
    const auto reader = f.call(libz, "_gzopen", {path, f.text("rb")});
    require(reader != 0 && f.call(libz, "_gzdirect", {reader}) == 0, "gzopen rb, not direct");
    const auto line = f.process.allocate_guest(64);
    require(f.call(libz, "_gzgets", {reader, line, 64}) == line && f.read(line) == "hello gz\n", "gzgets");
    require(f.call(libz, "_gzread", {reader, line, 3}) == 3 && f.read(line).substr(0, 3) == "42-", "gzread");
    require(f.call(libz, "_gzgetc_", {reader}) == 'x' && f.call(libz, "_gzungetc", {'x', reader}) == 'x' && f.call(libz, "_gzgetc", {reader}) == 'x',
            "gzgetc / gzungetc");
    require(f.call(libz, "_gzseek", {reader, 0, 0}) == 0, "gzseek back to the start");
    const auto all = f.process.allocate_guest(64);
    f.engine.fill_bytes(all, 0, 64);
    require(f.call(libz, "_gzread", {reader, all, 63}) == 25 && f.read(all) == "hello gz\n42-x\nline3\n!more", "both members");
    require(f.call(libz, "_gzeof", {reader}) == 1, "gzeof");
    const auto error = f.process.allocate_guest(4);
    f.call(libz, "_gzerror", {reader, error});
    require(f.engine.read32(error) == 0 && f.call(libz, "_gzclose", {reader}) == 0, "no error");
    // A file that is not gzip reads through unchanged.
    { std::ofstream out(file, std::ios::binary | std::ios::trunc); out << "plain text"; }
    const auto plain = f.call(libz, "_gzopen", {path, f.text("r")});
    f.engine.fill_bytes(all, 0, 64);
    require(f.call(libz, "_gzdirect", {plain}) == 1 && f.call(libz, "_gzread", {plain, all, 63}) == 10 && f.read(all) == "plain text", "gzdirect");
    f.call(libz, "_gzclose", {plain});
    require(f.call(libz, "_gzopen", {path, f.text("q")}) == 0, "gzopen without r/w/a");
    std::filesystem::remove(file, ignored);
}

} // namespace

int main() {
    try {
        const auto stage = [](std::string_view name) { std::cout << "[stage] " << name << std::endl; };
        stage("codec"); test_codec();
        stage("streams"); test_streams();
        stage("fixture"); Fixture f;
        stage("guest zlib"); test_guest_zlib(f);
        stage("guest z_stream"); test_guest_zstream(f);
        stage("guest z_stream libpng"); test_guest_zstream_libpng(f);
        stage("guest gz files"); test_guest_gzfiles(f);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "zlib tests passed\n";
    return 0;
}
