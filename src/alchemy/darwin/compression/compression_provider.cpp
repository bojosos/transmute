#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/compression/compression.hpp"

#include <map>
#include <mutex>

// /usr/lib/libcompression.dylib as a provider identity (Sparkle links it;
// its Autoupdate helper streams with it), over compression.cpp:
//   compression_encode_buffer / compression_decode_buffer: the bytes written,
//     0 when the destination is too small to encode into, the algorithm is
//     not provided, or the input is malformed; decoding truncates to the
//     destination, as documented.
//   compression_encode_scratch_buffer_size / ..._decode_...: the scratch
//     buffer is not used (0 for algorithms that are not provided).
//   compression_stream_init / _process / _destroy: COMPRESSION_STATUS_OK,
//     _END once the stream is complete and delivered, _ERROR for malformed
//     input or an algorithm that is not provided (LZFSE, LZMA, Brotli,
//     LZBITMAP).
// compression_stream {dst_ptr, dst_size, src_ptr, src_size, state}: `state`
// holds a guest token that names the host stream.
namespace alchemy::gui::completion_kit::data {
namespace {

constexpr std::string_view library = "/usr/lib/libcompression.dylib";
constexpr std::uint64_t status_ok = 0, status_end = 1, status_error = static_cast<std::uint64_t>(-1);
constexpr std::uint64_t stream_finalize = 1;
constexpr std::uint64_t scratch_size = 1u << 16;

struct Streams {
    std::mutex mutex;
    std::map<Address, std::unique_ptr<compression::Stream>> live;   // token -> stream
};

std::vector<std::uint8_t> read(GuestMemory& m, Address at, std::uint64_t size) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size) m.read_bytes(at, bytes);
    return bytes;
}

} // namespace

void install_compression(Support& support) {
    ProcessContext::add_provider_library(std::string(library));
    auto streams = std::make_shared<Streams>();
    auto* sp = &support;

    // size_t compression_encode_buffer(uint8_t* dst, size_t dst_size, const uint8_t* src, size_t src_size, void* scratch, compression_algorithm)
    support.fill_function(library, "_compression_encode_buffer", [](CpuState& s, GuestMemory& m) {
        const auto encoded = compression::encode(static_cast<std::uint32_t>(s.x[5]), read(m, s.x[2], s.x[3]));
        if (!encoded || encoded->size() > s.x[1]) { s.x[0] = 0; return; }
        if (!encoded->empty()) m.write_bytes(s.x[0], *encoded);
        s.x[0] = encoded->size();
    });
    // size_t compression_decode_buffer(uint8_t* dst, size_t dst_size, const uint8_t* src, size_t src_size, void* scratch, compression_algorithm)
    support.fill_function(library, "_compression_decode_buffer", [](CpuState& s, GuestMemory& m) {
        const auto decoded = compression::decode(static_cast<std::uint32_t>(s.x[5]), read(m, s.x[2], s.x[3]), static_cast<std::size_t>(s.x[1]));
        if (!decoded) { s.x[0] = 0; return; }
        if (!decoded->empty()) m.write_bytes(s.x[0], *decoded);
        s.x[0] = decoded->size();
    });
    for (const auto* name : {"_compression_encode_scratch_buffer_size", "_compression_decode_scratch_buffer_size"})
        support.fill_function(library, name, [](CpuState& s, GuestMemory&) { s.x[0] = compression::supported(static_cast<std::uint32_t>(s.x[0])) ? scratch_size : 0; });

    // compression_status compression_stream_init(compression_stream*, compression_stream_operation, compression_algorithm)
    support.fill_function(library, "_compression_stream_init", [sp, streams](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = status_error; return; }
        auto stream = compression::Stream::create(static_cast<int>(s.x[1]), static_cast<std::uint32_t>(s.x[2]));
        if (!stream) { m.write64(s.x[0] + 32, 0); s.x[0] = status_error; return; }
        const auto token = sp->process.allocate_guest(8);
        {
            std::lock_guard lock(streams->mutex);
            streams->live.emplace(token, std::move(stream));
        }
        m.write64(s.x[0] + 32, token);
        s.x[0] = status_ok;
    });
    // compression_status compression_stream_process(compression_stream*, int flags)
    support.fill_function(library, "_compression_stream_process", [streams](CpuState& s, GuestMemory& m) {
        const auto at = s.x[0];
        if (!at) { s.x[0] = status_error; return; }
        std::unique_lock lock(streams->mutex);
        const auto found = streams->live.find(m.read64(at + 32));
        if (found == streams->live.end()) { s.x[0] = status_error; return; }
        auto& stream = *found->second;
        lock.unlock();
        auto dst = m.read64(at), dst_size = m.read64(at + 8), src = m.read64(at + 16), src_size = m.read64(at + 24);
        const auto input = read(m, src, src_size);
        std::size_t consumed = 0;
        const bool ok = stream.process(input, consumed, (s.x[1] & stream_finalize) != 0);
        src += consumed;
        src_size -= consumed;
        const auto delivered = std::min<std::uint64_t>(dst_size, stream.pending.size());
        if (delivered) {
            m.write_bytes(dst, std::span(stream.pending.data(), static_cast<std::size_t>(delivered)));
            stream.pending.erase(stream.pending.begin(), stream.pending.begin() + static_cast<std::ptrdiff_t>(delivered));
        }
        dst += delivered;
        dst_size -= delivered;
        m.write64(at, dst);
        m.write64(at + 8, dst_size);
        m.write64(at + 16, src);
        m.write64(at + 24, src_size);
        if (!ok) { s.x[0] = status_error; return; }
        s.x[0] = stream.done() && stream.pending.empty() ? status_end : status_ok;
    });
    // compression_status compression_stream_destroy(compression_stream*)
    support.fill_function(library, "_compression_stream_destroy", [sp, streams](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = status_error; return; }
        const auto token = m.read64(s.x[0] + 32);
        bool known = false;
        {
            std::lock_guard lock(streams->mutex);
            known = streams->live.erase(token) != 0;
        }
        if (known) { sp->process.release_guest(token); m.write64(s.x[0] + 32, 0); }
        s.x[0] = known ? status_ok : status_error;
    });
}

} // namespace alchemy::gui::completion_kit::data
