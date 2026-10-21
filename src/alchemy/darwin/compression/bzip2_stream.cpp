#include "alchemy/darwin/compression/install.hpp"
#include "alchemy/darwin/compression/bzip2.hpp"
#include "alchemy/gui/fill_provider.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

// libbz2's streaming decompressor (BZ2_bzDecompressInit / Decompress / End)
// over the in-house bzip2 decoder (bzip2.cpp): the one bz2 streaming
// implementation. Fill-only; installed with libSystem's Chrome leftovers
// (fill::install_libsystem_leftovers, libsystem/misc/leftovers.cpp).
namespace alchemy::gui::fill {
namespace {

constexpr std::string_view libbz2 = "/usr/lib/libbz2.1.0.dylib";

class Bzip2Streams final : public Provider {
public:
    using Provider::Provider;
    void install();

private:
    struct Stream {
        std::vector<std::uint8_t> input;
        std::size_t tried{};                   // input size of the last unfinished decode
        bool decoded{};
        std::vector<std::uint8_t> output;
        std::size_t delivered{};
    };
    std::mutex streams_mutex_;
    std::map<Address, Stream> streams_;
};

void Bzip2Streams::install() {
    constexpr std::int32_t bz_ok = 0, bz_stream_end = 4, bz_param_error = -2, bz_data_error = -4, bz_data_error_magic = -5;
    const auto add_total = [](GuestMemory& m, Address low, std::uint64_t amount) {
        const auto value = (static_cast<std::uint64_t>(m.read32(low + 4)) << 32 | m.read32(low)) + amount;
        m.write32(low, static_cast<std::uint32_t>(value)); m.write32(low + 4, static_cast<std::uint32_t>(value >> 32));
    };
    // int BZ2_bzDecompressInit(bz_stream*, int verbosity, int small)
    fn(libbz2, "_BZ2_bzDecompressInit", [this](CpuState& s, GuestMemory& m) {
        const auto verbosity = static_cast<std::int32_t>(s.x[1]), small = static_cast<std::int32_t>(s.x[2]);
        if (!s.x[0] || verbosity < 0 || verbosity > 4 || (small != 0 && small != 1)) return status(bz_param_error);
        const auto token = process.allocate_guest(16);
        { std::lock_guard lock(streams_mutex_); streams_[token] = {}; }
        for (const Address offset : {12, 16, 36, 40}) m.write32(s.x[0] + offset, 0);
        m.write64(s.x[0] + 48, token);
        return status(bz_ok);
    });
    // int BZ2_bzDecompress(bz_stream*): the input is collected and decoded
    // once it holds the whole stream; bytes after the stream stay in next_in.
    fn(libbz2, "_BZ2_bzDecompress", [this, add_total](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return status(bz_param_error);
        const auto strm = s.x[0], token = m.read64(strm + 48);
        std::lock_guard lock(streams_mutex_);
        const auto found = streams_.find(token);
        if (found == streams_.end()) return status(bz_param_error);
        auto& stream = found->second;
        if (!stream.decoded) {
            const auto available = m.read32(strm + 8);
            if (available) {
                const auto at = stream.input.size();
                stream.input.resize(at + available);
                m.read_bytes(m.read64(strm), std::span(stream.input.data() + at, available));
                m.write64(strm, m.read64(strm) + available);
                m.write32(strm + 8, 0);
                add_total(m, strm + 12, available);
            }
            if (stream.input.size() == stream.tried) return status(bz_ok);   // nothing new to decode
            auto decoded = completion_kit::bzip2::decode(stream.input);
            if (decoded.error == completion_kit::bzip2::Error::unexpected_end) { stream.tried = stream.input.size(); return status(bz_ok); }
            if (decoded.error == completion_kit::bzip2::Error::magic) return status(bz_data_error_magic);
            if (decoded.error != completion_kit::bzip2::Error::none) return status(bz_data_error);
            // Hand back the bytes after the end of the stream (all from the last chunk).
            const auto extra = stream.input.size() - decoded.consumed;
            m.write64(strm, m.read64(strm) - extra);
            m.write32(strm + 8, static_cast<std::uint32_t>(extra));
            add_total(m, strm + 12, static_cast<std::uint64_t>(-static_cast<std::int64_t>(extra)));
            stream.decoded = true;
            stream.output = std::move(decoded.output);
            stream.input.clear();
            stream.input.shrink_to_fit();
        }
        const auto room = m.read32(strm + 32);
        const auto count = std::min<std::size_t>(room, stream.output.size() - stream.delivered);
        if (count) {
            m.write_bytes(m.read64(strm + 24), std::span(stream.output.data() + stream.delivered, count));
            stream.delivered += count;
            m.write64(strm + 24, m.read64(strm + 24) + count);
            m.write32(strm + 32, static_cast<std::uint32_t>(room - count));
            add_total(m, strm + 36, count);
        }
        return status(stream.delivered == stream.output.size() ? bz_stream_end : bz_ok);
    });
    fn(libbz2, "_BZ2_bzDecompressEnd", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return status(bz_param_error);
        const auto token = m.read64(s.x[0] + 48);
        {
            std::lock_guard lock(streams_mutex_);
            if (!streams_.erase(token)) return status(bz_param_error);
        }
        process.release_guest(token);
        m.write64(s.x[0] + 48, 0);
        return status(bz_ok);
    });
}

} // namespace
} // namespace alchemy::gui::fill

namespace alchemy::compression {

void install_bzip2_stream(gui::InstallContext& context) {
    ProcessContext::add_provider_library(std::string(gui::fill::libbz2));
    std::make_shared<gui::fill::Bzip2Streams>(context)->install();
}

} // namespace alchemy::compression
