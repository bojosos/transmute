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
// implementation. Also the streaming compressor (BZ2_bzCompressInit /
// Compress / End) over the in-house encoder, the zlib-style file calls
// (BZ2_bzopen, bzdopen, bzread, bzwrite, bzflush, bzclose, bzerror) over
// stdio and the BZFILE calls (foundation/archives_provider.cpp), as libbzip2
// layers them, and BZ2_bzlibVersion. Fill-only; installed with libSystem's
// Chrome leftovers (fill::install_libsystem_leftovers, libsystem/misc/leftovers.cpp).
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
    // A compressor: the input is kept until BZ_FINISH (the encoder writes whole streams).
    struct Compressor {
        int level{9};
        std::vector<std::uint8_t> input;
        bool finishing{};
        std::vector<std::uint8_t> output;
        std::size_t delivered{};
    };
    std::map<Address, Compressor> compressors_;
    // A BZFILE from BZ2_bzopen / bzdopen: its stdio stream, direction and last error (bzerror).
    struct File {
        Address stream{};
        bool writing{};
        bool standard{};   // stdin/stdout: not closed by bzclose
        std::int32_t error{};
    };
    std::map<Address, File> files_;
    std::map<std::string, Address> texts_;   // cstring_once's copies
    void install_compressor();
    void install_files();
    // One immortal guest copy of `text`, made on first use.
    Address cstring_once(const std::string& text) {
        std::lock_guard lock(streams_mutex_);
        auto& address = texts_[text];
        if (!address) address = cstring(text);
        return address;
    }
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
    install_compressor();
    install_files();
    // const char* BZ2_bzlibVersion(void): the libbzip2 release the format follows.
    const auto version = cstring("1.0.8, 13-Jul-2019");
    fn(libbz2, "_BZ2_bzlibVersion", [version](CpuState&, GuestMemory&) { return version; });
}

void Bzip2Streams::install_compressor() {
    constexpr std::int32_t bz_ok = 0, bz_run_ok = 1, bz_finish_ok = 3, bz_stream_end = 4, bz_sequence_error = -1, bz_param_error = -2;
    constexpr std::uint32_t bz_run = 0, bz_flush = 1, bz_finish = 2;
    const auto add_total = [](GuestMemory& m, Address low, std::uint64_t amount) {
        const auto value = (static_cast<std::uint64_t>(m.read32(low + 4)) << 32 | m.read32(low)) + amount;
        m.write32(low, static_cast<std::uint32_t>(value)); m.write32(low + 4, static_cast<std::uint32_t>(value >> 32));
    };
    // int BZ2_bzCompressInit(bz_stream*, int blockSize100k, int verbosity, int workFactor)
    fn(libbz2, "_BZ2_bzCompressInit", [this](CpuState& s, GuestMemory& m) {
        const auto level = static_cast<std::int32_t>(s.x[1]), verbosity = static_cast<std::int32_t>(s.x[2]),
                   work = static_cast<std::int32_t>(s.x[3]);
        if (!s.x[0] || level < 1 || level > 9 || verbosity < 0 || verbosity > 4 || work < 0 || work > 250) return status(bz_param_error);
        const auto token = process.allocate_guest(16);
        { std::lock_guard lock(streams_mutex_); compressors_[token] = Compressor{level}; }
        for (const Address offset : {12, 16, 36, 40}) m.write32(s.x[0] + offset, 0);
        m.write64(s.x[0] + 48, token);
        return status(bz_ok);
    });
    // int BZ2_bzCompress(bz_stream*, int action): BZ_RUN takes the input; BZ_FLUSH takes it too and reports
    // it flushed, but this encoder writes whole streams, so the compressed bytes come with BZ_FINISH (a reader
    // of the partial output sees nothing before then). BZ_FINISH encodes everything and hands it out,
    // BZ_FINISH_OK while some is left and BZ_STREAM_END with the last of it.
    fn(libbz2, "_BZ2_bzCompress", [this, add_total](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return status(bz_param_error);
        const Address strm = s.x[0];
        const auto action = static_cast<std::uint32_t>(s.x[1]);
        std::lock_guard lock(streams_mutex_);
        const auto found = compressors_.find(m.read64(strm + 48));
        if (found == compressors_.end() || action > bz_finish) return status(bz_param_error);
        auto& compressor = found->second;
        if (compressor.finishing && action != bz_finish) return status(bz_sequence_error);
        if (!compressor.finishing) {
            const auto available = m.read32(strm + 8);
            if (available) {
                const auto at = compressor.input.size();
                compressor.input.resize(at + available);
                m.read_bytes(m.read64(strm), std::span(compressor.input.data() + at, available));
                m.write64(strm, m.read64(strm) + available);
                m.write32(strm + 8, 0);
                add_total(m, strm + 12, available);
            }
            if (action == bz_run || action == bz_flush) return status(bz_run_ok);   // a flush is complete at once
            compressor.finishing = true;
            compressor.output = completion_kit::bzip2::encode(compressor.input, compressor.level);
            compressor.input.clear();
            compressor.input.shrink_to_fit();
        }
        const auto room = m.read32(strm + 32);
        const auto count = std::min<std::size_t>(room, compressor.output.size() - compressor.delivered);
        if (count) {
            m.write_bytes(m.read64(strm + 24), std::span(compressor.output.data() + compressor.delivered, count));
            compressor.delivered += count;
            m.write64(strm + 24, m.read64(strm + 24) + count);
            m.write32(strm + 32, static_cast<std::uint32_t>(room - count));
            add_total(m, strm + 36, count);
        }
        return status(compressor.delivered == compressor.output.size() ? bz_stream_end : bz_finish_ok);
    });
    fn(libbz2, "_BZ2_bzCompressEnd", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return status(bz_param_error);
        const auto token = m.read64(s.x[0] + 48);
        {
            std::lock_guard lock(streams_mutex_);
            if (!compressors_.erase(token)) return status(bz_param_error);
        }
        process.release_guest(token);
        m.write64(s.x[0] + 48, 0);
        return status(bz_ok);
    });
}

void Bzip2Streams::install_files() {
    constexpr std::int32_t bz_ok = 0, bz_stream_end = 4;
    // BZFILE* bzopen / bzdopen (path or descriptor, mode): "r" or "w", an optional block size digit (9),
    // "s" (small) and "f" (no effect, as in libbzip2); a NULL path or a descriptor 0/1 is stdin/stdout.
    const auto open = [this](GuestMemory& m, Address path, std::int32_t descriptor, Address mode_text) -> Address {
        if (!mode_text) return 0;
        bool writing = false, small = false;
        int level = 9;
        for (const char c : read_cstring(m, mode_text, 64)) {
            if (c == 'r') writing = false;
            else if (c == 'w') writing = true;
            else if (c == 's') small = true;
            else if (c >= '1' && c <= '9') level = c - '0';
        }
        const auto stdio_mode = cstring_once(writing ? "wb" : "rb");
        const bool standard = path ? false : descriptor < 0;
        Address stream = 0;
        if (path) stream = call(libsystem, "_fopen", {path, stdio_mode});
        else if (descriptor >= 0) stream = call(libsystem, "_fdopen", {static_cast<std::uint64_t>(descriptor), stdio_mode});
        else stream = m.read64(symbol(libsystem, writing ? "___stdoutp" : "___stdinp"));
        if (!stream) return 0;
        const auto error = process.allocate_guest(4);
        const auto file = writing ? call(libbz2, "_BZ2_bzWriteOpen", {error, stream, static_cast<std::uint64_t>(level), 0, 30})
                                  : call(libbz2, "_BZ2_bzReadOpen", {error, stream, 0, small ? 1u : 0u, 0, 0});
        const auto code = static_cast<std::int32_t>(m.read32(error));
        process.release_guest(error);
        if (!file || code != bz_ok) {
            if (!standard) static_cast<void>(call(libsystem, "_fclose", {stream}));
            return 0;
        }
        std::lock_guard lock(streams_mutex_);
        files_[file] = File{stream, writing, standard, bz_ok};
        return file;
    };
    fn(libbz2, "_BZ2_bzopen", [open](CpuState& s, GuestMemory& m) { return open(m, s.x[0], -1, s.x[1]); });
    fn(libbz2, "_BZ2_bzdopen", [open](CpuState& s, GuestMemory& m) {
        return open(m, 0, static_cast<std::int32_t>(s.x[0]), s.x[1]);
    });
    // int bzread(BZFILE*, void* buf, int len): the bytes read, 0 at the end of the stream, -1 on an error.
    fn(libbz2, "_BZ2_bzread", [this](CpuState& s, GuestMemory& m) {
        {
            std::lock_guard lock(streams_mutex_);
            const auto found = files_.find(s.x[0]);
            if (found == files_.end() || found->second.writing) return status(-1);
            if (found->second.error == bz_stream_end) return std::uint64_t{0};
        }
        const auto error = process.allocate_guest(4);
        const auto count = static_cast<std::int32_t>(call(libbz2, "_BZ2_bzRead", {error, s.x[0], s.x[1], s.x[2]}));
        const auto code = static_cast<std::int32_t>(m.read32(error));
        process.release_guest(error);
        std::lock_guard lock(streams_mutex_);
        if (const auto found = files_.find(s.x[0]); found != files_.end()) found->second.error = code;
        return code == bz_ok || code == bz_stream_end ? status(count) : status(-1);
    });
    // int bzwrite(BZFILE*, void* buf, int len): len, or -1 on an error.
    fn(libbz2, "_BZ2_bzwrite", [this](CpuState& s, GuestMemory& m) {
        {
            std::lock_guard lock(streams_mutex_);
            const auto found = files_.find(s.x[0]);
            if (found == files_.end() || !found->second.writing) return status(-1);
        }
        const auto error = process.allocate_guest(4);
        static_cast<void>(call(libbz2, "_BZ2_bzWrite", {error, s.x[0], s.x[1], s.x[2]}));
        const auto code = static_cast<std::int32_t>(m.read32(error));
        process.release_guest(error);
        std::lock_guard lock(streams_mutex_);
        if (const auto found = files_.find(s.x[0]); found != files_.end()) found->second.error = code;
        return code == bz_ok ? status(static_cast<std::int32_t>(s.x[2])) : status(-1);
    });
    // int bzflush(BZFILE*): nothing to do, as in libbzip2 1.0 (0).
    fn(libbz2, "_BZ2_bzflush", [](CpuState&, GuestMemory&) { return std::uint64_t{0}; });
    // void bzclose(BZFILE*): finishes a written stream, closes the BZFILE and its stdio stream (not stdin/stdout).
    fn(libbz2, "_BZ2_bzclose", [this](CpuState& s, GuestMemory&) {
        File file;
        {
            std::lock_guard lock(streams_mutex_);
            const auto found = files_.find(s.x[0]);
            if (found == files_.end()) return std::uint64_t{0};
            file = found->second;
            files_.erase(found);
        }
        const auto error = process.allocate_guest(4);
        if (file.writing) static_cast<void>(call(libbz2, "_BZ2_bzWriteClose", {error, s.x[0], 0, 0, 0}));
        else static_cast<void>(call(libbz2, "_BZ2_bzReadClose", {error, s.x[0]}));
        process.release_guest(error);
        if (!file.standard) static_cast<void>(call(libsystem, "_fclose", {file.stream}));
        return std::uint64_t{0};
    });
    // const char* bzerror(BZFILE*, int* errnum): the last error of the file's calls, and its name.
    static constexpr std::array<const char*, 16> names{"OK", "SEQUENCE_ERROR", "PARAM_ERROR", "MEM_ERROR", "DATA_ERROR",
        "DATA_ERROR_MAGIC", "IO_ERROR", "UNEXPECTED_EOF", "OUTBUFF_FULL", "CONFIG_ERROR", "???", "???", "???", "???", "???", "???"};
    fn(libbz2, "_BZ2_bzerror", [this](CpuState& s, GuestMemory& m) {
        std::int32_t error = 0;
        {
            std::lock_guard lock(streams_mutex_);
            if (const auto found = files_.find(s.x[0]); found != files_.end()) error = found->second.error;
        }
        if (error > 0) error = 0;   // libbzip2 reports the positive progress codes as OK
        if (s.x[1]) m.write32(s.x[1], static_cast<std::uint32_t>(error));
        return cstring_once(names[static_cast<std::size_t>(std::min(-error, 15))]);
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
