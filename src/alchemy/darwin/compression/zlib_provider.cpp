#include "alchemy/darwin/kit/export_fill/libraries.hpp"
#include "alchemy/darwin/compression/zlib_codec.hpp"

#include <stdexcept>
#include <string>
#include <vector>

// /usr/lib/libz.1.dylib: the zlib 1.2 utility interface (zlib.h) over the
// project-authored codec. A renderer's core libraries import
// compress2/compressBound/crc32/uncompress. The streaming
// z_stream interface is in zlib_stream.cpp.
namespace alchemy::export_fill {
namespace {

using transmute::CpuState;
using transmute::GuestMemory;

// zlib.h return codes.
constexpr std::int64_t z_ok = 0, z_stream_error = -2, z_data_error = -3, z_buf_error = -5;
// Largest buffer one call reads into host memory.
constexpr std::uint64_t max_buffer = 1ULL << 30;

std::uint64_t code(std::int64_t value) { return static_cast<std::uint64_t>(value); }

std::vector<std::uint8_t> read(const GuestMemory& memory, std::uint64_t address, std::uint64_t size) {
    if (size > max_buffer) throw std::runtime_error("Alchemy libz: buffer larger than 1 GiB is unsupported");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size) memory.read_bytes(address, bytes);
    return bytes;
}

std::int64_t compress_into(GuestMemory& memory, std::uint64_t destination, std::uint64_t length_address,
                           std::uint64_t source, std::uint64_t source_length, std::int64_t level) {
    if (level == -1) level = 6;
    if (level < 0 || level > 9) return z_stream_error;
    const auto input = read(memory, source, source_length);
    const auto output = zlib_compress(input, static_cast<int>(level));
    const auto capacity = memory.read64(length_address);
    if (output.size() > capacity) {
        // zlib leaves *destLen at the bytes it managed to write; nothing usable was produced.
        memory.write64(length_address, 0);
        return z_buf_error;
    }
    memory.write_bytes(destination, output);
    memory.write64(length_address, output.size());
    return z_ok;
}

std::int64_t uncompress_into(GuestMemory& memory, std::uint64_t destination, std::uint64_t length_address,
                             std::uint64_t source, std::uint64_t source_length, std::uint64_t consumed_address) {
    const auto input = read(memory, source, source_length);
    const auto capacity = memory.read64(length_address);
    const auto result = zlib_decompress(input, static_cast<std::size_t>(std::min(capacity, max_buffer)));
    if (!result.output.empty()) memory.write_bytes(destination, result.output);
    memory.write64(length_address, result.output.size());
    if (consumed_address) memory.write64(consumed_address, result.status == InflateStatus::ok ? result.consumed : source_length);
    if (result.status == InflateStatus::buffer_error) return z_buf_error;
    return result.status == InflateStatus::ok ? z_ok : z_data_error;
}

} // namespace

void install_zlib(ProcessContext& process) {
    const std::string library(zlib_library);
    const auto function = [&](const char* name, transmute::GuestCallHandler handler) {
        static_cast<void>(process.install_function(name, std::move(handler), library));
    };
    // uLong crc32(uLong crc, const Bytef* buf, uInt len); a null buffer returns the initial value.
    function("_crc32", [](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = 0; return; }
        s.x[0] = crc32_update(static_cast<std::uint32_t>(s.x[0]), read(m, s.x[1], static_cast<std::uint32_t>(s.x[2])));
    });
    function("_adler32", [](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = 1; return; }
        s.x[0] = adler32_update(static_cast<std::uint32_t>(s.x[0]), read(m, s.x[1], static_cast<std::uint32_t>(s.x[2])));
    });
    function("_compressBound", [](CpuState& s, GuestMemory&) { s.x[0] = zlib_compress_bound(s.x[0]); });
    // int compress2(Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen, int level)
    function("_compress2", [](CpuState& s, GuestMemory& m) {
        s.x[0] = code(compress_into(m, s.x[0], s.x[1], s.x[2], s.x[3], static_cast<std::int32_t>(s.x[4])));
    });
    function("_compress", [](CpuState& s, GuestMemory& m) {
        s.x[0] = code(compress_into(m, s.x[0], s.x[1], s.x[2], s.x[3], -1));
    });
    // int uncompress(Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen)
    function("_uncompress", [](CpuState& s, GuestMemory& m) {
        s.x[0] = code(uncompress_into(m, s.x[0], s.x[1], s.x[2], s.x[3], 0));
    });
    // int uncompress2(Bytef* dest, uLongf* destLen, const Bytef* source, uLong* sourceLen)
    function("_uncompress2", [](CpuState& s, GuestMemory& m) {
        const auto length = s.x[3] ? m.read64(s.x[3]) : 0;
        s.x[0] = code(uncompress_into(m, s.x[0], s.x[1], s.x[2], length, s.x[3]));
    });
    // const char* zlibVersion(void): the interface level these functions follow.
    const std::string version = "1.2.12";
    const auto text = process.allocate_guest(version.size() + 1);
    std::vector<std::uint8_t> bytes(version.begin(), version.end());
    bytes.push_back(0);
    process.main_engine().write_bytes(text, bytes);
    function("_zlibVersion", [text](CpuState& s, GuestMemory&) { s.x[0] = text; });
}

} // namespace alchemy::export_fill
