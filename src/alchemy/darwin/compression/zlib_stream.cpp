#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/compression/zlib_codec.hpp"

#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

// /usr/lib/libz.1.dylib streaming interface (zlib.h z_stream): deflateInit_,
// deflateInit2_, deflate, deflateReset, deflateEnd, deflateBound, inflateInit_,
// inflateInit2_, inflate, inflateReset, inflateReset2, inflateValidate,
// inflateEnd and zError, over the project-authored InflateStream and
// DeflateStream (zlib_codec.hpp). Qt Core/Gui/Network/Svg, Boost.Iostreams and
// OpenVDB in a renderer plug-in import them. The z_stream's `state` holds an
// opaque guest token naming the host state; zalloc/zfree are not called.
// Not provided (unresolved at bind): preset dictionaries
// (inflateSetDictionary/deflateSetDictionary: a stream that needs one fails
// with Z_NEED_DICT), deflateParams, deflateTune, inflateSync, inflateCopy,
// deflateCopy and inflateBack. The gz* file interface is zlib_files.cpp.
namespace alchemy::export_fill {
namespace {

using transmute::CpuState;
using transmute::GuestMemory;

// zlib.h return codes and flush values.
constexpr std::int64_t z_ok = 0, z_stream_end = 1, z_need_dict = 2, z_stream_error = -2, z_data_error = -3,
    z_buf_error = -5, z_version_error = -6;
constexpr std::uint32_t z_finish = 4, z_block = 5;
// struct z_stream_s on LP64 (zlib.h field order).
constexpr std::uint64_t next_in = 0, avail_in = 8, total_in = 16, next_out = 24, avail_out = 32, total_out = 40,
    msg = 48, state_field = 56, data_type = 88, adler_field = 96, z_stream_size = 112;

std::uint64_t code(std::int64_t value) { return static_cast<std::uint64_t>(value); }

struct Stream {
    std::variant<std::unique_ptr<InflateStream>, std::unique_ptr<DeflateStream>> codec;
    std::size_t delivered{};   // bytes of codec output() already copied to the guest
    bool deflate_flushed_finish{};
};

class Streams {
public:
    explicit Streams(ProcessContext& process) : process_(process) {}
    std::uint64_t add(std::shared_ptr<Stream> stream) {
        const auto token = process_.allocate_guest(16, 16);
        std::lock_guard lock(mutex_);
        streams_.emplace(token, std::move(stream));
        return token;
    }
    std::shared_ptr<Stream> find(std::uint64_t token) {
        std::lock_guard lock(mutex_);
        const auto found = streams_.find(token);
        return found == streams_.end() ? nullptr : found->second;
    }
    bool remove(std::uint64_t token) {
        {
            std::lock_guard lock(mutex_);
            if (!streams_.erase(token)) return false;
        }
        process_.release_guest(token);
        return true;
    }
    // A guest copy of `text` that lives as long as the process (z_stream.msg).
    std::uint64_t message(std::string_view text) {
        std::lock_guard lock(mutex_);
        const std::string key(text);
        if (const auto found = messages_.find(key); found != messages_.end()) return found->second;
        const auto address = process_.allocate_guest(key.size() + 1);
        std::vector<std::uint8_t> bytes(key.begin(), key.end());
        bytes.push_back(0);
        process_.main_engine().write_bytes(address, bytes);
        messages_.emplace(key, address);
        return address;
    }

private:
    ProcessContext& process_;
    std::mutex mutex_;
    std::map<std::uint64_t, std::shared_ptr<Stream>> streams_;
    std::map<std::string, std::uint64_t> messages_;
};

bool version_ok(const GuestMemory& memory, std::uint64_t version, std::uint64_t size) {
    return version && memory.read8(version) == '1' && size == z_stream_size;
}

// windowBits -> wrapper (zlib.h): 8..15 zlib, -8..-15 raw, 24..31 gzip,
// 40..47 automatic (inflate only). 0 (inflate) takes the window from the header.
std::optional<Wrapper> wrapper_for(std::int32_t bits, bool inflating) {
    if (inflating && bits == 0) return Wrapper::zlib;
    if (bits >= 8 && bits <= 15) return Wrapper::zlib;
    if (bits <= -8 && bits >= -15) return Wrapper::raw;
    if (bits >= 24 && bits <= 31) return Wrapper::gzip;
    if (inflating && bits >= 40 && bits <= 47) return Wrapper::automatic;
    return std::nullopt;
}

void begin(GuestMemory& memory, std::uint64_t stream, std::uint64_t token, std::uint32_t check) {
    memory.write64(stream + total_in, 0);
    memory.write64(stream + total_out, 0);
    memory.write64(stream + msg, 0);
    memory.write64(stream + state_field, token);
    memory.write32(stream + data_type, 0);
    memory.write64(stream + adler_field, check);
}

// Copies pending codec output to next_out; returns the bytes delivered.
std::uint64_t deliver(GuestMemory& memory, std::uint64_t stream, Stream& state, std::vector<std::uint8_t>& output) {
    const auto room = memory.read32(stream + avail_out);
    const auto available = output.size() - state.delivered;
    const auto count = std::min<std::uint64_t>(room, available);
    if (count) {
        const auto destination = memory.read64(stream + next_out);
        memory.write_bytes(destination, std::span(output.data() + state.delivered, static_cast<std::size_t>(count)));
        memory.write64(stream + next_out, destination + count);
        memory.write32(stream + avail_out, static_cast<std::uint32_t>(room - count));
        memory.write64(stream + total_out, memory.read64(stream + total_out) + count);
        state.delivered += static_cast<std::size_t>(count);
    }
    if (state.delivered == output.size()) { output.clear(); state.delivered = 0; }
    else if (state.delivered > (1U << 20) && state.delivered * 2 > output.size()) {
        output.erase(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(state.delivered));
        state.delivered = 0;
    }
    return count;
}

std::vector<std::uint8_t> take_input(GuestMemory& memory, std::uint64_t stream) {
    const auto count = memory.read32(stream + avail_in);
    std::vector<std::uint8_t> bytes(count);
    if (count) memory.read_bytes(memory.read64(stream + next_in), bytes);
    return bytes;
}

void consume(GuestMemory& memory, std::uint64_t stream, std::uint64_t count) {
    if (!count) return;
    memory.write64(stream + next_in, memory.read64(stream + next_in) + count);
    memory.write32(stream + avail_in, memory.read32(stream + avail_in) - static_cast<std::uint32_t>(count));
    memory.write64(stream + total_in, memory.read64(stream + total_in) + count);
}

} // namespace

void install_zlib_stream(ProcessContext& process) {
    const std::string library(zlib_library);
    auto streams = std::make_shared<Streams>(process);
    const auto function = [&](const char* name, transmute::GuestCallHandler handler) {
        if (provided(process, library, name)) return;
        static_cast<void>(process.install_function(name, std::move(handler), library));
    };
    const auto lookup = [streams](GuestMemory& memory, std::uint64_t stream) -> std::shared_ptr<Stream> {
        if (!stream) return nullptr;
        return streams->find(memory.read64(stream + state_field));
    };

    // ---- inflate ----
    const auto inflate_init = [streams](GuestMemory& m, std::uint64_t stream, std::int32_t bits, std::uint64_t version,
                                        std::uint64_t size) -> std::int64_t {
        if (!version_ok(m, version, size)) return z_version_error;
        if (!stream) return z_stream_error;
        const auto wrapper = wrapper_for(bits, true);
        if (!wrapper) return z_stream_error;
        auto state = std::make_shared<Stream>();
        state->codec = std::make_unique<InflateStream>(*wrapper);
        const auto check = *wrapper == Wrapper::zlib ? 1U : 0U;
        begin(m, stream, streams->add(std::move(state)), check);
        return z_ok;
    };
    // int inflateInit_(z_streamp, const char* version, int stream_size)
    function("_inflateInit_", [inflate_init](CpuState& s, GuestMemory& m) {
        s.x[0] = code(inflate_init(m, s.x[0], 15, s.x[1], static_cast<std::uint32_t>(s.x[2])));
    });
    // int inflateInit2_(z_streamp, int windowBits, const char* version, int stream_size)
    function("_inflateInit2_", [inflate_init](CpuState& s, GuestMemory& m) {
        s.x[0] = code(inflate_init(m, s.x[0], static_cast<std::int32_t>(s.x[1]), s.x[2], static_cast<std::uint32_t>(s.x[3])));
    });
    // int inflate(z_streamp, int flush)
    function("_inflate", [lookup, streams](CpuState& s, GuestMemory& m) {
        const auto stream = s.x[0];
        const auto flush = static_cast<std::uint32_t>(s.x[1]);
        const auto state = lookup(m, stream);
        auto* codec = state ? std::get_if<std::unique_ptr<InflateStream>>(&state->codec) : nullptr;
        if (!codec || flush > z_block || !m.read64(stream + next_out) ||
            (!m.read64(stream + next_in) && m.read32(stream + avail_in))) {
            s.x[0] = code(z_stream_error);
            return;
        }
        auto& inflater = **codec;
        using Status = InflateStream::Status;
        // Like zlib, input is only taken while the output fits: callers such
        // as libpng fetch the next chunk when avail_in reaches 0 and treat a
        // missing chunk as truncated data ("Not enough image data"). The
        // codec takes input in whole spans, so pending output is delivered
        // first, new input is fed only when nothing is pending, and the last
        // input byte is held back until the output of the rest was delivered.
        const auto input = take_input(m, stream);
        std::uint64_t produced = deliver(m, stream, *state, inflater.output());
        const auto pending = [&] { return inflater.output().size() - state->delivered; };
        Status status = inflater.ended() ? Status::end : Status::more;
        std::uint64_t consumed = 0;
        const auto feed = [&](std::span<const std::uint8_t> piece) {
            const auto result = inflater.feed(piece);
            consume(m, stream, result.consumed);
            consumed += result.consumed;
            status = result.status;
            produced += deliver(m, stream, *state, inflater.output());
            return result.consumed == piece.size();
        };
        if (status == Status::more && pending() == 0 && !input.empty()) {
            const std::span<const std::uint8_t> all(input);
            const auto head = all.size() - 1;
            bool whole = true;
            if (head) whole = feed(all.first(head));
            if (whole && status == Status::more && pending() == 0) feed(all.subspan(head));
        }
        m.write64(stream + adler_field, inflater.check());
        if (status == Status::data_error) {
            m.write64(stream + msg, streams->message(inflater.message()));
            s.x[0] = code(z_data_error);
            return;
        }
        if (status == Status::need_dictionary) { s.x[0] = code(z_need_dict); return; }
        if (status == Status::end && pending() == 0) { s.x[0] = code(z_stream_end); return; }
        std::int64_t value = z_ok;
        if ((consumed == 0 && produced == 0) || flush == z_finish) value = z_buf_error;
        s.x[0] = code(value);
    });
    const auto inflate_reset = [lookup](GuestMemory& m, std::uint64_t stream, std::optional<std::int32_t> bits) -> std::int64_t {
        const auto state = lookup(m, stream);
        auto* codec = state ? std::get_if<std::unique_ptr<InflateStream>>(&state->codec) : nullptr;
        if (!codec) return z_stream_error;
        if (bits) {
            const auto chosen = wrapper_for(*bits, true);
            if (!chosen) return z_stream_error;
            (*codec)->reset(*chosen);
        } else {
            (*codec)->reset();   // inflateReset keeps the wrapper chosen at initialization
        }
        state->delivered = 0;
        begin(m, stream, m.read64(stream + state_field), (*codec)->check());
        return z_ok;
    };
    // int inflateReset(z_streamp)
    function("_inflateReset", [inflate_reset](CpuState& s, GuestMemory& m) { s.x[0] = code(inflate_reset(m, s.x[0], std::nullopt)); });
    // int inflateReset2(z_streamp, int windowBits)
    function("_inflateReset2", [inflate_reset](CpuState& s, GuestMemory& m) {
        s.x[0] = code(inflate_reset(m, s.x[0], static_cast<std::int32_t>(s.x[1])));
    });
    // int inflateValidate(z_streamp, int check)
    function("_inflateValidate", [lookup](CpuState& s, GuestMemory& m) {
        const auto state = lookup(m, s.x[0]);
        auto* codec = state ? std::get_if<std::unique_ptr<InflateStream>>(&state->codec) : nullptr;
        if (!codec) { s.x[0] = code(z_stream_error); return; }
        (*codec)->verify(static_cast<std::uint32_t>(s.x[1]) != 0);
        s.x[0] = code(z_ok);
    });
    // int inflateEnd(z_streamp)
    function("_inflateEnd", [lookup, streams](CpuState& s, GuestMemory& m) {
        const auto state = lookup(m, s.x[0]);
        if (!state || !std::holds_alternative<std::unique_ptr<InflateStream>>(state->codec)) { s.x[0] = code(z_stream_error); return; }
        static_cast<void>(streams->remove(m.read64(s.x[0] + state_field)));
        m.write64(s.x[0] + state_field, 0);
        s.x[0] = code(z_ok);
    });

    // ---- deflate ----
    const auto deflate_init = [streams](GuestMemory& m, std::uint64_t stream, std::int32_t level, std::int32_t method,
                                        std::int32_t bits, std::int32_t memory_level, std::int32_t strategy,
                                        std::uint64_t version, std::uint64_t size) -> std::int64_t {
        if (!version_ok(m, version, size)) return z_version_error;
        if (!stream) return z_stream_error;
        if (level == -1) level = 6;
        const auto wrapper = wrapper_for(bits, false);
        if (level < 0 || level > 9 || method != 8 || !wrapper || memory_level < 1 || memory_level > 9 || strategy < 0 || strategy > 4)
            return z_stream_error;
        auto state = std::make_shared<Stream>();
        state->codec = std::make_unique<DeflateStream>(*wrapper, level);
        begin(m, stream, streams->add(std::move(state)), *wrapper == Wrapper::zlib ? 1U : 0U);
        return z_ok;
    };
    // int deflateInit_(z_streamp, int level, const char* version, int stream_size)
    function("_deflateInit_", [deflate_init](CpuState& s, GuestMemory& m) {
        s.x[0] = code(deflate_init(m, s.x[0], static_cast<std::int32_t>(s.x[1]), 8, 15, 8, 0, s.x[2], static_cast<std::uint32_t>(s.x[3])));
    });
    // int deflateInit2_(z_streamp, int level, int method, int windowBits, int memLevel,
    //                   int strategy, const char* version, int stream_size)
    function("_deflateInit2_", [deflate_init](CpuState& s, GuestMemory& m) {
        s.x[0] = code(deflate_init(m, s.x[0], static_cast<std::int32_t>(s.x[1]), static_cast<std::int32_t>(s.x[2]),
                                   static_cast<std::int32_t>(s.x[3]), static_cast<std::int32_t>(s.x[4]),
                                   static_cast<std::int32_t>(s.x[5]), s.x[6], static_cast<std::uint32_t>(s.x[7])));
    });
    // int deflate(z_streamp, int flush)
    function("_deflate", [lookup](CpuState& s, GuestMemory& m) {
        const auto stream = s.x[0];
        const auto flush = static_cast<std::uint32_t>(s.x[1]);
        const auto state = lookup(m, stream);
        auto* codec = state ? std::get_if<std::unique_ptr<DeflateStream>>(&state->codec) : nullptr;
        if (!codec || flush > z_block || !m.read64(stream + next_out) ||
            (!m.read64(stream + next_in) && m.read32(stream + avail_in))) {
            s.x[0] = code(z_stream_error);
            return;
        }
        auto& deflater = **codec;
        if (deflater.finished() && flush != z_finish) { s.x[0] = code(z_stream_error); return; }
        if (!m.read32(stream + avail_out)) { s.x[0] = code(z_buf_error); return; }
        const auto input = take_input(m, stream);
        const bool repeat_finish = deflater.finished();
        if (!repeat_finish) {
            deflater.feed(input, static_cast<DeflateStream::Flush>(flush));
            consume(m, stream, input.size());
        }
        const auto produced = deliver(m, stream, *state, deflater.output());
        m.write64(stream + adler_field, deflater.check());
        if (deflater.finished() && deflater.output().empty()) { s.x[0] = code(z_stream_end); return; }
        s.x[0] = code(input.empty() && produced == 0 && flush != z_finish ? z_buf_error : z_ok);
    });
    // int deflateReset(z_streamp)
    function("_deflateReset", [lookup](CpuState& s, GuestMemory& m) {
        const auto state = lookup(m, s.x[0]);
        auto* codec = state ? std::get_if<std::unique_ptr<DeflateStream>>(&state->codec) : nullptr;
        if (!codec) { s.x[0] = code(z_stream_error); return; }
        (*codec)->reset();
        state->delivered = 0;
        begin(m, s.x[0], m.read64(s.x[0] + state_field), (*codec)->check());
        s.x[0] = code(z_ok);
    });
    // int deflateEnd(z_streamp): Z_DATA_ERROR when output was still pending.
    function("_deflateEnd", [lookup, streams](CpuState& s, GuestMemory& m) {
        const auto state = lookup(m, s.x[0]);
        auto* codec = state ? std::get_if<std::unique_ptr<DeflateStream>>(&state->codec) : nullptr;
        if (!codec) { s.x[0] = code(z_stream_error); return; }
        const bool pending = !(*codec)->output().empty();
        static_cast<void>(streams->remove(m.read64(s.x[0] + state_field)));
        m.write64(s.x[0] + state_field, 0);
        s.x[0] = code(pending ? z_data_error : z_ok);
    });
    // uLong deflateBound(z_streamp, uLong sourceLen): the stored-block worst
    // case of this encoder plus the largest header and trailer (gzip, 18 bytes)
    // and one sync-flush marker.
    function("_deflateBound", [](CpuState& s, GuestMemory&) {
        s.x[0] = zlib_compress_bound(s.x[1]) + 18 + 5;
    });

    // const char* zError(int)
    function("_zError", [streams](CpuState& s, GuestMemory&) {
        const auto value = static_cast<std::int32_t>(s.x[0]);
        std::string_view text = "";
        switch (value) {
        case 2: text = "need dictionary"; break;
        case 1: text = "stream end"; break;
        case 0: text = ""; break;
        case -1: text = "file error"; break;
        case -2: text = "stream error"; break;
        case -3: text = "data error"; break;
        case -4: text = "insufficient memory"; break;
        case -5: text = "buffer error"; break;
        case -6: text = "incompatible version"; break;
        default: text = ""; break;
        }
        s.x[0] = streams->message(text);
    });
}

} // namespace alchemy::export_fill
