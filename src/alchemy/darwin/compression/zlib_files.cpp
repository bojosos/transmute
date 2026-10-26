#include "alchemy/darwin/compression/install.hpp"
#include "alchemy/darwin/compression/zlib_codec.hpp"
#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// /usr/lib/libz.1.dylib gz* file interface (zlib.h "gzip file access functions") over the
// project-authored codec and the guest's open/read/write/lseek/close:
//   gzopen/gzopen64/gzdopen with "r", "w" or "a", a level digit and "T" (no compression), "x",
//   "e"; strategy letters are accepted (the encoder has one strategy).
//   Reading decodes concatenated gzip members and passes a file that is not gzip through
//   unchanged (gzdirect). gzread, gzfread, gzgetc/gzgetc_, gzungetc, gzgets.
//   Writing: gzwrite, gzfwrite, gzputc, gzputs, gzprintf/gzvprintf (over vasprintf), gzflush
//   (Z_FINISH ends the member; the next write starts another).
//   gzseek/gzseek64 (reading: forward by decoding, backward by starting over; writing: forward
//   with zeros; SEEK_END is unsupported, -1), gzrewind, gztell, gzoffset, gzeof, gzdirect,
//   gzbuffer, gzsetparams, gzerror, gzclearerr, gzclose/gzclose_r/gzclose_w.
// The gzFile is a guest struct gzFile_s {unsigned have; unsigned char* next; z_off64_t pos;}
// whose `have` stays 0, so zlib.h's gzgetc macro always calls gzgetc_.
namespace alchemy::compression {
namespace {

using transmute::CpuState;
using transmute::GuestMemory;
using export_fill::DeflateStream;
using export_fill::InflateStream;
using export_fill::Wrapper;

constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
constexpr std::int64_t z_ok = 0, z_errno = -1, z_stream_error = -2, z_data_error = -3, z_buf_error = -5;
constexpr std::int32_t z_finish = 4;
constexpr std::uint64_t chunk = 64 * 1024;
constexpr std::uint32_t o_wronly = 1, o_append = 8, o_creat = 0x200, o_trunc = 0x400, o_excl = 0x800, o_cloexec = 0x1000000;

std::uint64_t code(std::int64_t value) { return static_cast<std::uint64_t>(value); }

struct GzFile {
    std::mutex mutex;
    std::int32_t descriptor{-1};
    bool writing{}, transparent{}, direct{}, sniffed{}, raw_end{}, eof{}, started{};
    int level{6};
    std::int64_t position{};      // uncompressed bytes read or written
    std::int64_t start{};         // file offset where reading began (gzrewind)
    std::vector<std::uint8_t> raw;            // read but not decoded
    std::vector<std::uint8_t> decoded;        // decoded, not delivered (pushed-back bytes in front)
    std::unique_ptr<InflateStream> inflater;
    std::unique_ptr<DeflateStream> deflater;
    std::int64_t error{z_ok};
    std::string message;
    std::uint64_t buffer{}, message_text{};   // guest I/O buffer and gzerror's text
};

class Files {
public:
    explicit Files(ProcessContext& process) : process_(process) {}
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> arguments) {
        const auto found = process_.resolve(libsystem, name);
        if (!found) throw std::runtime_error("Alchemy libz: gz functions require " + std::string(name.substr(1)));
        return EngineAdapter(process_.calling_engine()).call(found->address, std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t call_regs(std::string_view name, const CallRegs& registers) {
        const auto found = process_.resolve(libsystem, name);
        if (!found) throw std::runtime_error("Alchemy libz: gz functions require " + std::string(name.substr(1)));
        return EngineAdapter(process_.calling_engine()).call_regs(found->address, registers).x0;
    }
    std::uint32_t guest_errno() {
        const auto thread = process_.current_thread();
        return thread && thread->errno_address ? process_.calling_engine().read32(thread->errno_address) : 0;
    }
    void set_errno(std::uint32_t value) { process_.set_errno(value); }
    ProcessContext& process() { return process_; }

    std::uint64_t add(std::shared_ptr<GzFile> file) {
        const auto handle = process_.allocate_guest(32);
        process_.calling_engine().fill_bytes(handle, 0, 32);
        file->buffer = process_.allocate_guest(chunk);
        file->message_text = process_.allocate_guest(256);
        process_.calling_engine().fill_bytes(file->message_text, 0, 256);
        std::lock_guard lock(mutex_);
        files_[handle] = std::move(file);
        return handle;
    }
    std::shared_ptr<GzFile> find(std::uint64_t handle) {
        std::lock_guard lock(mutex_);
        const auto found = files_.find(handle);
        return found == files_.end() ? nullptr : found->second;
    }
    void remove(std::uint64_t handle) {
        std::shared_ptr<GzFile> file;
        { std::lock_guard lock(mutex_); const auto found = files_.find(handle); if (found == files_.end()) return; file = found->second; files_.erase(found); }
        process_.release_guest(file->buffer);
        process_.release_guest(file->message_text);
        process_.release_guest(handle);
    }

    void fail(GuestMemory& m, GzFile& file, std::int64_t error, std::string message) {
        file.error = error;
        file.message = std::move(message);
        const auto text = file.message.substr(0, 255);
        export_fill::put_string(m, file.message_text, text);
    }
    // Raw bytes from the file (false at its end or on an error).
    bool read_raw(GuestMemory& m, GzFile& file) {
        if (file.raw_end) return false;
        const auto got = static_cast<std::int64_t>(call("_read", {static_cast<std::uint32_t>(file.descriptor), file.buffer, chunk}));
        if (got < 0) { fail(m, file, z_errno, "read error"); file.raw_end = true; return false; }
        if (got == 0) { file.raw_end = true; return false; }
        const auto old = file.raw.size();
        file.raw.resize(old + static_cast<std::size_t>(got));
        m.read_bytes(file.buffer, std::span(file.raw.data() + old, static_cast<std::size_t>(got)));
        return true;
    }
    // Decodes until `wanted` bytes are waiting or the input ends.
    void fill(GuestMemory& m, GzFile& file, std::size_t wanted) {
        while (file.decoded.size() < wanted && file.error == z_ok) {
            if (!file.sniffed) {
                while (file.raw.size() < 2 && read_raw(m, file)) {}
                file.sniffed = true;
                file.direct = !(file.raw.size() >= 2 && file.raw[0] == 0x1f && file.raw[1] == 0x8b);
                if (!file.direct) file.inflater = std::make_unique<InflateStream>(Wrapper::gzip);
            }
            if (file.direct) {
                if (file.raw.empty() && !read_raw(m, file)) return;
                file.decoded.insert(file.decoded.end(), file.raw.begin(), file.raw.end());
                file.raw.clear();
                continue;
            }
            if (file.inflater->ended()) {
                // Another member, or the end (anything else after a member is ignored).
                while (file.raw.size() < 2 && read_raw(m, file)) {}
                if (file.raw.size() >= 2 && file.raw[0] == 0x1f && file.raw[1] == 0x8b) { file.inflater->reset(Wrapper::gzip); continue; }
                file.raw.clear();
                return;
            }
            if (file.raw.empty() && !read_raw(m, file)) {
                if (file.error == z_ok) fail(m, file, z_buf_error, "unexpected end of file");
                return;
            }
            const auto result = file.inflater->feed(file.raw);
            file.raw.erase(file.raw.begin(), file.raw.begin() + static_cast<std::ptrdiff_t>(result.consumed));
            auto& output = file.inflater->output();
            file.decoded.insert(file.decoded.end(), output.begin(), output.end());
            output.clear();
            if (result.status == InflateStream::Status::data_error || result.status == InflateStream::Status::need_dictionary) {
                fail(m, file, z_data_error, std::string(file.inflater->message().empty() ? "invalid compressed data" : file.inflater->message()));
                return;
            }
        }
    }
    // Delivers up to `size` decoded bytes to guest `address`.
    std::uint64_t read(GuestMemory& m, GzFile& file, std::uint64_t address, std::uint64_t size) {
        std::uint64_t total = 0;
        while (total < size) {
            fill(m, file, static_cast<std::size_t>(std::min<std::uint64_t>(size - total, chunk)));
            if (file.decoded.empty()) { file.eof = true; break; }
            const auto count = std::min<std::uint64_t>(size - total, file.decoded.size());
            m.write_bytes(address + total, std::span(file.decoded.data(), static_cast<std::size_t>(count)));
            file.decoded.erase(file.decoded.begin(), file.decoded.begin() + static_cast<std::ptrdiff_t>(count));
            total += count;
        }
        file.position += static_cast<std::int64_t>(total);
        return total;
    }
    bool write_raw(GuestMemory& m, GzFile& file, std::span<const std::uint8_t> bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto count = std::min<std::size_t>(bytes.size() - done, chunk);
            m.write_bytes(file.buffer, bytes.subspan(done, count));
            const auto wrote = static_cast<std::int64_t>(call("_write", {static_cast<std::uint32_t>(file.descriptor), file.buffer, count}));
            if (wrote <= 0) { fail(m, file, z_errno, "write error"); return false; }
            done += static_cast<std::size_t>(wrote);
        }
        return true;
    }
    // Compresses (or, transparent, copies) host bytes to the file.
    bool write(GuestMemory& m, GzFile& file, std::span<const std::uint8_t> bytes, DeflateStream::Flush flush) {
        if (file.transparent) return write_raw(m, file, bytes);
        if (!file.deflater) file.deflater = std::make_unique<DeflateStream>(Wrapper::gzip, file.level);
        file.deflater->feed(bytes, flush);
        auto& output = file.deflater->output();
        const auto ok = write_raw(m, file, output);
        output.clear();
        if (flush == DeflateStream::Flush::finish) file.deflater.reset();
        return ok;
    }
    // Starts reading over from where the file was opened.
    bool rewind(GuestMemory& m, GzFile& file) {
        const auto offset = static_cast<std::int64_t>(call("_lseek", {static_cast<std::uint32_t>(file.descriptor), static_cast<std::uint64_t>(file.start), 0}));
        if (offset < 0) { fail(m, file, z_errno, "seek error"); return false; }
        file.raw.clear(); file.decoded.clear(); file.inflater.reset();
        file.sniffed = file.raw_end = file.eof = false;
        file.position = 0;
        file.error = z_ok;
        return true;
    }

private:
    ProcessContext& process_;
    std::mutex mutex_;
    std::map<std::uint64_t, std::shared_ptr<GzFile>> files_;
};

struct Mode { bool ok{}, writing{}, append{}, transparent{}, exclusive{}, cloexec{}; int level{6}; };
Mode parse_mode(std::string_view text) {
    Mode mode;
    bool kind = false;
    for (const auto c : text) {
        if (c == 'r' || c == 'w' || c == 'a') { kind = true; mode.writing = c != 'r'; mode.append = c == 'a'; }
        else if (c >= '0' && c <= '9') mode.level = c - '0';
        else if (c == 'T') mode.transparent = true;
        else if (c == 'x') mode.exclusive = true;
        else if (c == 'e') mode.cloexec = true;
        else if (c == '+') return mode;   // reading and writing at once is not supported by zlib either
    }
    mode.ok = kind;
    return mode;
}

} // namespace

void install_zlib_files(ProcessContext& process) {
    const std::string library(export_fill::zlib_library);
    auto files = std::make_shared<Files>(process);
    const auto function = [&](const char* name, transmute::GuestCallHandler handler) {
        if (export_fill::provided(process, library, name)) return;
        static_cast<void>(process.install_function(name, std::move(handler), library));
    };
    const auto open_file = [files](GuestMemory& m, std::int64_t descriptor, std::uint64_t path, std::uint64_t mode_text) -> std::uint64_t {
        const auto mode = parse_mode(export_fill::guest_string(m, mode_text, 64));
        if (!mode.ok) { files->set_errno(export_fill::einval); return 0; }
        if (descriptor < 0) {
            std::uint32_t flags = mode.writing ? (o_wronly | o_creat | (mode.append ? o_append : o_trunc)) : 0;
            if (mode.exclusive && mode.writing) flags |= o_excl;
            if (mode.cloexec) flags |= o_cloexec;
            CallRegs registers;
            registers.x[0] = path;
            registers.x[1] = flags;
            registers.push_variadic(std::uint64_t{0666});
            descriptor = static_cast<std::int32_t>(files->call_regs("_open", registers));
            if (descriptor < 0) return 0;
        }
        auto file = std::make_shared<GzFile>();
        file->descriptor = static_cast<std::int32_t>(descriptor);
        file->writing = mode.writing;
        file->transparent = mode.transparent && mode.writing;
        file->level = mode.level;
        if (!mode.writing) {
            const auto offset = static_cast<std::int64_t>(files->call("_lseek", {static_cast<std::uint32_t>(descriptor), 0, 1}));
            file->start = offset < 0 ? 0 : offset;
        }
        return files->add(std::move(file));
    };
    // gzFile gzopen(const char* path, const char* mode) / gzopen64 / gzdopen(int fd, const char* mode)
    for (const auto* name : {"_gzopen", "_gzopen64"})
        function(name, [open_file](CpuState& s, GuestMemory& m) { s.x[0] = open_file(m, -1, s.x[0], s.x[1]); });
    function("_gzdopen", [open_file](CpuState& s, GuestMemory& m) {
        const auto descriptor = static_cast<std::int32_t>(s.x[0]);
        s.x[0] = descriptor < 0 ? 0 : open_file(m, descriptor, 0, s.x[1]);
    });

    // Runs `body` on the file under its lock; `failure` for an unknown handle.
    const auto with = [files](std::uint64_t handle, std::uint64_t failure, auto body) -> std::uint64_t {
        const auto file = files->find(handle);
        if (!file) return failure;
        std::lock_guard lock(file->mutex);
        return body(*file);
    };

    // ---- reading ----
    // int gzread(gzFile, voidp buf, unsigned len)
    function("_gzread", [files, with](CpuState& s, GuestMemory& m) {
        s.x[0] = with(s.x[0], code(-1), [&](GzFile& file) -> std::uint64_t {
            if (file.writing) { files->fail(m, file, z_stream_error, "file not open for reading"); return code(-1); }
            const auto size = static_cast<std::uint32_t>(s.x[2]);
            if (size > static_cast<std::uint32_t>(INT32_MAX)) { files->fail(m, file, z_stream_error, "request does not fit in an int"); return code(-1); }
            file.started = true;
            const auto count = files->read(m, file, s.x[1], size);
            if (!count && file.error != z_ok && file.error != z_buf_error) return code(-1);
            return count;
        });
    });
    // z_size_t gzfread(voidp buf, z_size_t size, z_size_t nitems, gzFile)
    function("_gzfread", [files, with](CpuState& s, GuestMemory& m) {
        s.x[0] = with(s.x[3], 0, [&](GzFile& file) -> std::uint64_t {
            if (file.writing || !s.x[1] || !s.x[2] || s.x[2] > UINT64_MAX / s.x[1]) return 0;
            file.started = true;
            return files->read(m, file, s.x[0], s.x[1] * s.x[2]) / s.x[1];
        });
    });
    // int gzgetc(gzFile) / gzgetc_(gzFile): a byte, or -1.
    for (const auto* name : {"_gzgetc", "_gzgetc_"})
        function(name, [files, with](CpuState& s, GuestMemory& m) {
            s.x[0] = with(s.x[0], code(-1), [&](GzFile& file) -> std::uint64_t {
                if (file.writing) return code(-1);
                file.started = true;
                files->fill(m, file, 1);
                if (file.decoded.empty()) { file.eof = true; return code(-1); }
                const auto value = file.decoded.front();
                file.decoded.erase(file.decoded.begin());
                ++file.position;
                return value;
            }) & 0xffffffffULL;
        });
    // int gzungetc(int c, gzFile): c back in front of the data.
    function("_gzungetc", [with](CpuState& s, GuestMemory&) {
        s.x[0] = with(s.x[1], code(-1), [&](GzFile& file) -> std::uint64_t {
            const auto c = static_cast<std::int32_t>(s.x[0]);
            if (file.writing || c < 0) return code(-1);
            file.decoded.insert(file.decoded.begin(), static_cast<std::uint8_t>(c));
            --file.position;
            file.eof = false;
            return static_cast<std::uint8_t>(c);
        }) & 0xffffffffULL;
    });
    // char* gzgets(gzFile, char* buf, int len): a line with its newline, NUL-terminated; NULL at the end.
    function("_gzgets", [files, with](CpuState& s, GuestMemory& m) {
        s.x[0] = with(s.x[0], 0, [&](GzFile& file) -> std::uint64_t {
            const auto length = static_cast<std::int32_t>(s.x[2]);
            if (file.writing || !s.x[1] || length < 1) return 0;
            file.started = true;
            std::uint64_t count = 0;
            while (count + 1 < static_cast<std::uint64_t>(length)) {
                files->fill(m, file, 1);
                if (file.decoded.empty()) { file.eof = true; break; }
                const auto c = file.decoded.front();
                file.decoded.erase(file.decoded.begin());
                ++file.position;
                m.write8(s.x[1] + count++, c);
                if (c == '\n') break;
            }
            if (!count) return 0;
            m.write8(s.x[1] + count, 0);
            return s.x[1];
        });
    });

    // ---- writing ----
    const auto write_bytes = [files](GuestMemory& m, GzFile& file, std::uint64_t address, std::uint64_t size) -> bool {
        file.started = true;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (size) m.read_bytes(address, bytes);
        if (!files->write(m, file, bytes, DeflateStream::Flush::none)) return false;
        file.position += static_cast<std::int64_t>(size);
        return true;
    };
    // int gzwrite(gzFile, voidpc buf, unsigned len): len, or 0 on an error.
    function("_gzwrite", [files, with, write_bytes](CpuState& s, GuestMemory& m) {
        s.x[0] = with(s.x[0], 0, [&](GzFile& file) -> std::uint64_t {
            const auto size = static_cast<std::uint32_t>(s.x[2]);
            if (!file.writing) { files->fail(m, file, z_stream_error, "file not open for writing"); return 0; }
            if (size > static_cast<std::uint32_t>(INT32_MAX)) { files->fail(m, file, z_data_error, "requested length does not fit in int"); return 0; }
            return write_bytes(m, file, s.x[1], size) ? size : 0;
        });
    });
    // z_size_t gzfwrite(voidpc buf, z_size_t size, z_size_t nitems, gzFile)
    function("_gzfwrite", [with, write_bytes](CpuState& s, GuestMemory& m) {
        s.x[0] = with(s.x[3], 0, [&](GzFile& file) -> std::uint64_t {
            if (!file.writing || !s.x[1] || !s.x[2] || s.x[2] > UINT64_MAX / s.x[1]) return 0;
            return write_bytes(m, file, s.x[0], s.x[1] * s.x[2]) ? s.x[2] : 0;
        });
    });
    // int gzputc(gzFile, int c) / int gzputs(gzFile, const char* s)
    function("_gzputc", [files, with](CpuState& s, GuestMemory& m) {
        s.x[0] = with(s.x[0], code(-1), [&](GzFile& file) -> std::uint64_t {
            if (!file.writing) return code(-1);
            file.started = true;
            const std::array<std::uint8_t, 1> byte{static_cast<std::uint8_t>(s.x[1])};
            if (!files->write(m, file, byte, DeflateStream::Flush::none)) return code(-1);
            ++file.position;
            return byte[0];
        }) & 0xffffffffULL;
    });
    function("_gzputs", [with, write_bytes](CpuState& s, GuestMemory& m) {
        s.x[0] = with(s.x[0], code(-1), [&](GzFile& file) -> std::uint64_t {
            if (!file.writing) return code(-1);
            const auto text = m.read_string(s.x[1], 1, 1ULL << 30);
            return write_bytes(m, file, s.x[1], text.size()) ? text.size() : code(-1);
        }) & 0xffffffffULL;
    });
    // int gzprintf(gzFile, const char* format, ...) / gzvprintf(gzFile, const char* format, va_list)
    for (const auto va : {false, true})
        function(va ? "_gzvprintf" : "_gzprintf", [files, with, write_bytes, va](CpuState& s, GuestMemory& m) {
            s.x[0] = with(s.x[0], code(z_stream_error), [&](GzFile& file) -> std::uint64_t {
                if (!file.writing) return code(z_stream_error);
                const auto cell = files->process().allocate_guest(8);
                const auto length = static_cast<std::int32_t>(files->call("_vasprintf", {cell, s.x[1], va ? s.x[2] : s.sp}));
                const auto text = m.read64(cell);
                files->process().release_guest(cell);
                if (length < 0) return code(z_errno);
                const auto ok = write_bytes(m, file, text, static_cast<std::uint64_t>(length));
                static_cast<void>(files->call("_free", {text}));
                return ok ? static_cast<std::uint64_t>(length) : code(z_errno);
            }) & 0xffffffffULL;
        });
    // int gzflush(gzFile, int flush): Z_FINISH ends the gzip member.
    function("_gzflush", [files, with](CpuState& s, GuestMemory& m) {
        s.x[0] = with(s.x[0], code(z_stream_error), [&](GzFile& file) -> std::uint64_t {
            const auto flush = static_cast<std::int32_t>(s.x[1]);
            if (!file.writing || flush < 0 || flush > z_finish) return code(z_stream_error);
            if (!file.deflater && flush != z_finish) return code(z_ok);
            const auto kind = flush == z_finish ? DeflateStream::Flush::finish : flush == 0 ? DeflateStream::Flush::none : DeflateStream::Flush::sync;
            if (!file.transparent && (file.deflater || flush != z_finish) && !files->write(m, file, {}, kind)) return code(file.error);
            return code(z_ok);
        }) & 0xffffffffULL;
    });

    // ---- positions and state ----
    // z_off_t gzseek(gzFile, z_off_t offset, int whence) / gzseek64
    for (const auto* name : {"_gzseek", "_gzseek64"})
        function(name, [files, with, write_bytes](CpuState& s, GuestMemory& m) {
            s.x[0] = with(s.x[0], code(-1), [&](GzFile& file) -> std::uint64_t {
                const auto whence = static_cast<std::int32_t>(s.x[2]);
                if (whence != 0 && whence != 1) return code(-1);
                auto target = static_cast<std::int64_t>(s.x[1]) + (whence == 1 ? file.position : 0);
                if (target < 0) return code(-1);
                if (file.writing) {
                    if (target < file.position) return code(-1);
                    std::vector<std::uint8_t> zeros(static_cast<std::size_t>(std::min<std::int64_t>(target - file.position, static_cast<std::int64_t>(chunk))));
                    while (file.position < target) {
                        const auto count = std::min<std::int64_t>(target - file.position, static_cast<std::int64_t>(zeros.size()));
                        file.started = true;
                        if (!files->write(m, file, std::span(zeros.data(), static_cast<std::size_t>(count)), DeflateStream::Flush::none)) return code(-1);
                        file.position += count;
                    }
                    return static_cast<std::uint64_t>(file.position);
                }
                if (target < file.position && !files->rewind(m, file)) return code(-1);
                while (file.position < target) {
                    files->fill(m, file, static_cast<std::size_t>(std::min<std::int64_t>(target - file.position, static_cast<std::int64_t>(chunk))));
                    if (file.decoded.empty()) break;
                    const auto count = std::min<std::int64_t>(target - file.position, static_cast<std::int64_t>(file.decoded.size()));
                    file.decoded.erase(file.decoded.begin(), file.decoded.begin() + static_cast<std::ptrdiff_t>(count));
                    file.position += count;
                }
                return static_cast<std::uint64_t>(file.position);
            });
        });
    function("_gzrewind", [files, with](CpuState& s, GuestMemory& m) {
        s.x[0] = with(s.x[0], code(-1), [&](GzFile& file) -> std::uint64_t {
            if (file.writing) return code(-1);
            return files->rewind(m, file) ? 0 : code(-1);
        }) & 0xffffffffULL;
    });
    for (const auto* name : {"_gztell", "_gztell64"})
        function(name, [with](CpuState& s, GuestMemory&) {
            s.x[0] = with(s.x[0], code(-1), [](GzFile& file) -> std::uint64_t { return static_cast<std::uint64_t>(file.position); });
        });
    // z_off_t gzoffset(gzFile): the raw file offset (less the input not yet decoded).
    for (const auto* name : {"_gzoffset", "_gzoffset64"})
        function(name, [files, with](CpuState& s, GuestMemory&) {
            s.x[0] = with(s.x[0], code(-1), [&](GzFile& file) -> std::uint64_t {
                const auto offset = static_cast<std::int64_t>(files->call("_lseek", {static_cast<std::uint32_t>(file.descriptor), 0, 1}));
                if (offset < 0) return code(-1);
                return static_cast<std::uint64_t>(offset - static_cast<std::int64_t>(file.raw.size()));
            });
        });
    function("_gzeof", [with](CpuState& s, GuestMemory&) {
        s.x[0] = with(s.x[0], 0, [](GzFile& file) -> std::uint64_t { return !file.writing && file.eof && file.decoded.empty() ? 1 : 0; });
    });
    // int gzdirect(gzFile): 1 when reading a file that is not gzip, or writing with "T".
    function("_gzdirect", [files, with](CpuState& s, GuestMemory& m) {
        s.x[0] = with(s.x[0], 0, [&](GzFile& file) -> std::uint64_t {
            if (file.writing) return file.transparent ? 1 : 0;
            if (!file.sniffed) files->fill(m, file, 1);   // decodes ahead without delivering
            return file.direct ? 1 : 0;
        });
    });
    // int gzbuffer(gzFile, unsigned size): before the first read or write only.
    function("_gzbuffer", [with](CpuState& s, GuestMemory&) {
        s.x[0] = with(s.x[0], code(-1), [&](GzFile& file) -> std::uint64_t {
            return file.started || static_cast<std::uint32_t>(s.x[1]) < 2 ? code(-1) : 0;
        }) & 0xffffffffULL;
    });
    // int gzsetparams(gzFile, int level, int strategy): the level of the data that follows
    // (the data so far is ended as its own deflate blocks first).
    function("_gzsetparams", [files, with](CpuState& s, GuestMemory& m) {
        s.x[0] = with(s.x[0], code(z_stream_error), [&](GzFile& file) -> std::uint64_t {
            auto level = static_cast<std::int32_t>(s.x[1]);
            if (level == -1) level = 6;
            if (!file.writing || level < 0 || level > 9 || static_cast<std::uint32_t>(s.x[2]) > 4) return code(z_stream_error);
            if (file.deflater && !files->write(m, file, {}, DeflateStream::Flush::finish)) return code(file.error);
            file.level = level;
            return code(z_ok);
        }) & 0xffffffffULL;
    });
    // const char* gzerror(gzFile, int* errnum)
    function("_gzerror", [with](CpuState& s, GuestMemory& m) {
        const auto errnum = s.x[1];
        s.x[0] = with(s.x[0], 0, [&](GzFile& file) -> std::uint64_t {
            if (errnum) m.write32(errnum, static_cast<std::uint32_t>(file.error));
            if (file.error == z_ok) m.write8(file.message_text, 0);
            return file.message_text;
        });
    });
    function("_gzclearerr", [with](CpuState& s, GuestMemory& m) {
        static_cast<void>(with(s.x[0], 0, [&](GzFile& file) -> std::uint64_t {
            file.error = z_ok; file.eof = false; file.message.clear();
            m.write8(file.message_text, 0);
            return 0;
        }));
    });
    // int gzclose(gzFile) / gzclose_r / gzclose_w: finishes the member, closes the descriptor.
    for (const auto& [name, which] : std::array<std::pair<const char*, int>, 3>{{{"_gzclose", 0}, {"_gzclose_r", 1}, {"_gzclose_w", 2}}}) {
        const int kind = which;
        function(name, [files, kind](CpuState& s, GuestMemory& m) {
            const auto handle = s.x[0];
            const auto file = files->find(handle);
            if (!file || (kind == 1 && file->writing) || (kind == 2 && !file->writing)) { s.x[0] = code(z_stream_error) & 0xffffffffULL; return; }
            std::int64_t result = z_ok;
            {
                std::lock_guard lock(file->mutex);
                if (file->writing && !file->transparent && (file->deflater || !file->started)) {
                    // An empty "w" file is still one valid (empty) gzip member.
                    if (!files->write(m, *file, {}, DeflateStream::Flush::finish)) result = file->error;
                }
                if (static_cast<std::int32_t>(files->call("_close", {static_cast<std::uint32_t>(file->descriptor)})) != 0 && result == z_ok) result = z_errno;
                if (result == z_ok && file->error != z_ok && file->error != z_buf_error) result = file->error;
            }
            files->remove(handle);
            s.x[0] = code(result) & 0xffffffffULL;
        });
    }
}

} // namespace alchemy::compression
