#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// BSD libc leftovers (fill-only):
//   <string.h>    memrchr, strchrnul, explicit_bzero
//   <inttypes.h>  wcstoimax, wcstoumax; <wctype.h> wctrans, towctrans
//   <stdlib.h>    psort, psort_r, psort_b (the qsort forms), sranddev, devname_r, getbsize,
//                 rpmatch; arc4random_stir and arc4random_addrandom do nothing (arc4random
//                 reads the host's random source on every call, there is no pool to stir)
//   <stdio.h>     tempnam (over access and mktemp), fgetwln (over fgetln)
//   <unistd.h>    getwd; mkfifoat, mknodat, mkpathat_np (relative to a directory descriptor); setmode, getmode
//   <libproc.h>   proc_track_dirty, proc_set_dirty, proc_get_dirty, proc_clear_dirty (this process's own
//                 state; no launchd acts on it), proc_libversion
//   <membership.h> mbr_gid_to_uuid, mbr_uuid_to_id (the compatibility UUIDs); getpwuuid, getpwuuid_r
//   <execinfo.h>  backtrace_from_fp, backtrace_image_offsets
//   minherit (vm_inherit), statfs64/fstatfs64/getfsstat64/getmntinfo64, atof_l, querylocale
//   <time.h>      timeoff, strptime_l (the "C" locale); ualarm is an explicit stop, as alarm
//                 (no asynchronous SIGALRM)
//   <unistd.h>    getpass; <readpassphrase.h> readpassphrase (echo off on a terminal)
//   <pwd.h>/<grp.h> user_from_uid, group_from_gid, uid_from_user, gid_from_group
//   <execinfo.h>  backtrace_symbols_fd
//   <sys/resource.h> getiopolicy_np, setiopolicy_np (kept per process and per thread;
//                 the host's I/O scheduler gets no hint)
//   <pthread.h>   pthread_is_threaded_np
//   <secure/_*.h> __stpcpy_chk, __stpncpy_chk, __memccpy_chk, __chk_fail_overflow
//   <stdio.h>     fmtcheck
//   <ctype.h>     __istype, __isctype, __tolower_l, __toupper_l, _CurrentRuneLocale (the C
//                 locale, over misc/locale.cpp's rune table)
namespace alchemy::export_fill {
namespace {

constexpr std::uint32_t enotty_value = 25;

[[noreturn]] void buffer_overflow() { throw std::runtime_error("Alchemy: detected buffer overflow (abort)"); }

// A guest buffer per guest thread, kept for the process's life (the "static"
// result buffers of the BSD interfaces, made per thread).
class ThreadBuffers {
public:
    ThreadBuffers(ProcessContext& process, std::uint64_t size) : process_(process), size_(size) {}
    std::uint64_t get() {
        const auto thread = process_.current_thread();
        const auto identity = thread ? thread->identity : 0;
        std::lock_guard lock(mutex_);
        auto& address = buffers_[identity];
        if (!address) address = process_.allocate_guest(size_);
        return address;
    }
private:
    ProcessContext& process_;
    std::uint64_t size_;
    std::mutex mutex_;
    std::map<std::uint64_t, std::uint64_t> buffers_;
};

std::string bounded_string(const GuestMemory& m, std::uint64_t address, std::size_t maximum) {
    if (!address) throw Errno{efault};
    const auto bytes = m.read_string(address, 1, maximum);
    return {bytes.begin(), bytes.end()};
}

// ---- fmtcheck: the argument types a printf format consumes ----
enum class Arg { integer, long_integer, long_long, size, ptrdiff, intmax, character_pointer, wide_pointer, pointer,
                 real, long_real, count_int, count_short, count_char, count_long, count_long_long, count_size,
                 count_intmax, count_ptrdiff, wide_character, invalid };

// nullopt when the format cannot be checked (positional arguments, unknown conversions).
std::optional<std::vector<Arg>> format_arguments(std::string_view format) {
    std::vector<Arg> result;
    for (std::size_t at = 0; at < format.size(); ++at) {
        if (format[at] != '%') continue;
        if (++at >= format.size()) return std::nullopt;
        if (format[at] == '%') continue;
        while (at < format.size() && std::string_view("-+ #0'").find(format[at]) != std::string_view::npos) ++at;
        const auto digits = [&] { while (at < format.size() && format[at] >= '0' && format[at] <= '9') ++at; };
        if (at < format.size() && format[at] == '*') { result.push_back(Arg::integer); ++at; } else {
            const auto start = at;
            digits();
            if (at < format.size() && format[at] == '$' && at != start) return std::nullopt;
        }
        if (at < format.size() && format[at] == '.') {
            ++at;
            if (at < format.size() && format[at] == '*') { result.push_back(Arg::integer); ++at; } else digits();
        }
        enum class Length { none, hh, h, l, ll, j, z, t, big_l } length = Length::none;
        if (at < format.size()) {
            const auto c = format[at];
            const auto next = at + 1 < format.size() ? format[at + 1] : '\0';
            if (c == 'h' && next == 'h') { length = Length::hh; at += 2; }
            else if (c == 'l' && next == 'l') { length = Length::ll; at += 2; }
            else if (c == 'h') { length = Length::h; ++at; }
            else if (c == 'l') { length = Length::l; ++at; }
            else if (c == 'q') { length = Length::ll; ++at; }
            else if (c == 'j') { length = Length::j; ++at; }
            else if (c == 'z') { length = Length::z; ++at; }
            else if (c == 't') { length = Length::t; ++at; }
            else if (c == 'L') { length = Length::big_l; ++at; }
        }
        if (at >= format.size()) return std::nullopt;
        const auto conversion = format[at];
        Arg arg = Arg::invalid;
        switch (conversion) {
        case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
            switch (length) {
            case Length::none: case Length::hh: case Length::h: arg = Arg::integer; break;
            case Length::l: arg = Arg::long_integer; break;
            case Length::ll: arg = Arg::long_long; break;
            case Length::j: arg = Arg::intmax; break;
            case Length::z: arg = Arg::size; break;
            case Length::t: arg = Arg::ptrdiff; break;
            case Length::big_l: break;
            }
            break;
        case 'D': case 'O': case 'U': if (length == Length::none) arg = Arg::long_integer; break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
            if (length == Length::none || length == Length::l) arg = Arg::real;
            else if (length == Length::big_l) arg = Arg::long_real;
            break;
        case 'c': arg = length == Length::l ? Arg::wide_character : (length == Length::none ? Arg::integer : Arg::invalid); break;
        case 'C': if (length == Length::none) arg = Arg::wide_character; break;
        case 's': arg = length == Length::l ? Arg::wide_pointer : (length == Length::none ? Arg::character_pointer : Arg::invalid); break;
        case 'S': if (length == Length::none) arg = Arg::wide_pointer; break;
        case 'p': if (length == Length::none) arg = Arg::pointer; break;
        case 'n':
            switch (length) {
            case Length::none: arg = Arg::count_int; break;
            case Length::hh: arg = Arg::count_char; break;
            case Length::h: arg = Arg::count_short; break;
            case Length::l: arg = Arg::count_long; break;
            case Length::ll: arg = Arg::count_long_long; break;
            case Length::j: arg = Arg::count_intmax; break;
            case Length::z: arg = Arg::count_size; break;
            case Length::t: arg = Arg::count_ptrdiff; break;
            case Length::big_l: break;
            }
            break;
        default: break;
        }
        if (arg == Arg::invalid) return std::nullopt;
        result.push_back(arg);
    }
    return result;
}

} // namespace

void install_libsystem_bsd_extra(Provider& p) {
    auto& process = p.process();

    // ---- <string.h> ----
    // void* memrchr(const void* s, int c, size_t n): the last c in the n bytes.
    p.libsystem("_memrchr", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto c = static_cast<std::uint8_t>(s.x[1]);
        std::array<std::uint8_t, 4096> chunk{};
        for (auto end = s.x[2]; end > 0;) {
            const auto size = std::min<std::uint64_t>(end, chunk.size());
            const auto start = end - size;
            m.read_bytes(s.x[0] + start, std::span(chunk.data(), static_cast<std::size_t>(size)));
            for (auto index = size; index > 0; --index)
                if (chunk[static_cast<std::size_t>(index - 1)] == c) return s.x[0] + start + index - 1;
            end = start;
        }
        return 0;
    }, 0);
    // char* strchrnul(const char* s, int c): the first c, or the terminating NUL.
    p.libsystem("_strchrnul", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto c = static_cast<std::uint8_t>(s.x[1]);
        for (auto at = s.x[0];; ++at) {
            const auto value = m.read8(at);
            if (value == c || value == 0) return at;
        }
    }, 0);
    p.libsystem("_explicit_bzero", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (s.x[1]) m.fill_bytes(s.x[0], 0, s.x[1]);
        return 0;
    });

    // ---- wide characters ----
    p.libsystem("_wcstoimax", [p](CpuState& s, GuestMemory&) -> std::uint64_t { return p.call(libsystem_library, "_wcstoll", {s.x[0], s.x[1], s.x[2]}); });
    p.libsystem("_wcstoumax", [p](CpuState& s, GuestMemory&) -> std::uint64_t { return p.call(libsystem_library, "_wcstoull", {s.x[0], s.x[1], s.x[2]}); });
    // wctrans_t wctrans(const char*): "tolower" 1, "toupper" 2; 0 (EINVAL) for others.
    p.libsystem("_wctrans", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto name = bounded_string(m, s.x[0], 64);
        if (name == "tolower") return 1;
        if (name == "toupper") return 2;
        throw Errno{einval};
    }, 0);
    p.libsystem("_towctrans", [p](CpuState& s, GuestMemory&) -> std::uint64_t {
        if (s.x[1] == 1) return p.call(libsystem_library, "_towlower", {s.x[0]});
        if (s.x[1] == 2) return p.call(libsystem_library, "_towupper", {s.x[0]});
        p.process().set_errno(einval);
        return static_cast<std::uint32_t>(s.x[0]);
    });

    // ---- <stdlib.h> ----
    // psort, psort_r, psort_b: the sorts qsort, qsort_r and qsort_b do (same arguments).
    for (const auto& [name, sort] : std::array<std::pair<const char*, const char*>, 3>{{
             {"_psort", "_qsort"}, {"_psort_r", "_qsort_r"}, {"_psort_b", "_qsort_b"}}}) {
        const std::string target = sort;
        p.libsystem(name, [p, target](CpuState& s, GuestMemory&) -> std::uint64_t {
            static_cast<void>(p.call(libsystem_library, target, {s.x[0], s.x[1], s.x[2], s.x[3], s.x[4]}));
            return 0;
        });
    }
    p.libsystem("_sranddev", [p](CpuState&, GuestMemory&) -> std::uint64_t {
        static_cast<void>(p.call(libsystem_library, "_srand", {p.call(libsystem_library, "_arc4random", {}) & 0xffffffffULL}));
        return 0;
    });
    p.libsystem("_arc4random_stir", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
    p.libsystem("_arc4random_addrandom", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
    // char* devname_r(dev_t, mode_t, char* buf, int len): devname's name in buf (truncated), or NULL.
    p.libsystem("_devname_r", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto length = static_cast<std::int32_t>(s.x[3]);
        if (!s.x[2] || length <= 0) throw Errno{einval};
        const auto name = p.call(libsystem_library, "_devname", {s.x[0], s.x[1]});
        if (!name) return 0;
        auto text = bounded_string(m, name, 1024);
        if (text.size() >= static_cast<std::size_t>(length)) text.resize(static_cast<std::size_t>(length) - 1);
        put_string(m, s.x[2], text);
        return s.x[2];
    }, 0);
    // char* getbsize(int* headerlen, long* blocksize): BLOCKSIZE ("<n>[kmg]", 512 bytes to
    // 1 GB; 512 when unset or not understood, with a warning) and its column header.
    const auto bsize_header = process.allocate_guest(32);
    p.libsystem("_getbsize", [p, bsize_header](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto warn = [&p, &m](std::string_view message) {
            if (!p.process().resolve(libsystem_library, "_warnx")) return;
            const auto text = p.malloc(message.size() + 1);
            put_string(m, text, message);
            static_cast<void>(p.call(libsystem_library, "_warnx", {text}));
            static_cast<void>(p.call(libsystem_library, "_free", {text}));
        };
        std::int64_t count = 512;
        std::string form;
        std::int64_t unit = 1;
        const auto name = p.malloc(10);
        put_string(m, name, "BLOCKSIZE");
        const auto environment = p.call(libsystem_library, "_getenv", {name});
        static_cast<void>(p.call(libsystem_library, "_free", {name}));
        if (environment) {
            const auto text = bounded_string(m, environment, 64);
            std::size_t at = 0;
            std::int64_t parsed = 0;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9' && parsed < (1LL << 40)) parsed = parsed * 10 + (text[at++] - '0');
            if (at == 0) parsed = 1;
            bool valid = true;
            if (at < text.size()) {
                switch (text[at]) {
                case 'k': case 'K': form = "K"; unit = 1024; break;
                case 'm': case 'M': form = "M"; unit = 1024 * 1024; break;
                case 'g': case 'G': form = "G"; unit = 1024 * 1024 * 1024; break;
                default: valid = false; break;
                }
                if (valid && at + 1 != text.size()) valid = false;
            }
            if (!valid) { warn(text + ": unknown blocksize"); parsed = 512; form.clear(); unit = 1; }
            count = parsed;
            if (count * unit > (1LL << 30)) { warn("maximum blocksize is 1G"); count = 1; form = "G"; unit = 1LL << 30; }
            else if (count * unit < 512) { warn("minimum blocksize is 512"); count = 512; form.clear(); unit = 1; }
        }
        const auto header = std::to_string(count) + form + "-blocks";
        put_string(m, bsize_header, header);
        if (s.x[0]) m.write32(s.x[0], static_cast<std::uint32_t>(header.size()));
        if (s.x[1]) m.write64(s.x[1], static_cast<std::uint64_t>(count * unit));
        return bsize_header;
    }, 0);
    // int rpmatch(const char*): 1 for a yes ("y"/"Y"), 0 for a no ("n"/"N"), -1 otherwise ("C" locale).
    p.libsystem("_rpmatch", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto first = m.read8(s.x[0]);
        if (first == 'y' || first == 'Y') return 1;
        if (first == 'n' || first == 'N') return 0;
        return static_cast<std::uint64_t>(-1);
    });

    // char* tempnam(const char* dir, const char* prefix): a malloc'd name that does not exist yet,
    // in the first writable directory of TMPDIR, dir, P_tmpdir ("/var/tmp/") and "/tmp", from up
    // to five prefix characters ("tmp." without one) and mktemp's random ones.
    p.libsystem("_tempnam", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto writable = [&p, &m](const std::string& directory) {
            if (directory.empty()) return false;
            const auto path = p.malloc(directory.size() + 1);
            put_string(m, path, directory);
            const auto result = static_cast<std::int32_t>(p.call(libsystem_library, "_access", {path, 2}));
            static_cast<void>(p.call(libsystem_library, "_free", {path}));
            return result == 0;
        };
        std::vector<std::string> candidates;
        const auto name = p.malloc(7);
        put_string(m, name, "TMPDIR");
        if (const auto environment = p.call(libsystem_library, "_getenv", {name})) candidates.push_back(bounded_string(m, environment, 1024));
        static_cast<void>(p.call(libsystem_library, "_free", {name}));
        if (s.x[0]) candidates.push_back(bounded_string(m, s.x[0], 1024));
        candidates.emplace_back("/var/tmp/");
        candidates.emplace_back("/tmp");
        std::string directory;
        for (const auto& candidate : candidates) if (writable(candidate)) { directory = candidate; break; }
        if (directory.empty()) throw Errno{enoent};
        const auto prefix = s.x[1] ? bounded_string(m, s.x[1], 1024).substr(0, 5) : std::string("tmp.");
        const auto path = directory + (directory.back() == '/' ? "" : "/") + prefix + "XXXXXX";
        const auto result = p.malloc(path.size() + 1);
        put_string(m, result, path);
        if (!p.call(libsystem_library, "_mktemp", {result})) {
            const auto error = p.guest_errno();
            static_cast<void>(p.call(libsystem_library, "_free", {result}));
            throw Errno{error ? error : eexist};
        }
        return result;
    }, 0);

    // char* getwd(char* buf): getcwd into buf (MAXPATHLEN, 1024); on failure the error text is there.
    p.libsystem("_getwd", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (p.call(libsystem_library, "_getcwd", {s.x[0], 1024})) return s.x[0];
        const auto error = p.guest_errno();
        const auto text = bounded_string(m, p.call(libsystem_library, "_strerror", {error}), 1023);
        put_string(m, s.x[0], text);
        p.process().set_errno(error);
        return 0;
    }, 0);
    // The *at forms that take a directory descriptor: the path joined to the directory's own
    // (fcntl F_GETPATH) unless it is absolute or the descriptor is AT_FDCWD (-2).
    const auto at_path = [p](GuestMemory& m, std::uint64_t descriptor, std::uint64_t path) -> std::uint64_t {
        const auto text = bounded_string(m, path, 1024);
        if (static_cast<std::int32_t>(descriptor) == -2 || (!text.empty() && text[0] == '/')) return 0;
        const auto directory = p.malloc(1024);
        CallRegs registers;
        registers.x[0] = descriptor & 0xffffffffULL;
        registers.x[1] = 50;   // F_GETPATH
        registers.push_variadic(directory);
        if (static_cast<std::int32_t>(p.call_regs(libsystem_library, "_fcntl", registers)) < 0) {
            const auto error = p.guest_errno();
            static_cast<void>(p.call(libsystem_library, "_free", {directory}));
            throw Errno{error ? error : ebadf};
        }
        auto joined = bounded_string(m, directory, 1024);
        static_cast<void>(p.call(libsystem_library, "_free", {directory}));
        if (joined.empty() || joined.back() != '/') joined += '/';
        joined += text;
        const auto result = p.malloc(joined.size() + 1);
        put_string(m, result, joined);
        return result;   // the caller frees it
    };
    // int mkfifoat(int fd, const char* path, mode_t) / mknodat(int fd, const char* path, mode_t, dev_t) /
    // int mkpathat_np(int fd, const char* path, mode_t): mkfifo, mknod and mkpath_np relative to fd.
    for (const auto& [name, target] : std::array<std::pair<const char*, const char*>, 3>{{
             {"_mkfifoat", "_mkfifo"}, {"_mknodat", "_mknod"}, {"_mkpathat_np", "_mkpath_np"}}}) {
        const std::string function = target;
        const bool status = function == "_mkpath_np";   // mkpath_np returns an error number
        p.libsystem(name, [p, at_path, function, status](CpuState& s, GuestMemory& m) -> std::uint64_t {
            std::uint64_t joined = 0;
            try { joined = at_path(m, s.x[0], s.x[1]); }
            catch (const Errno& error) { if (status) return error.value; throw; }
            const auto result = p.call(libsystem_library, function, {joined ? joined : s.x[1], s.x[2], s.x[3]});
            const auto error = p.guest_errno();
            if (joined) static_cast<void>(p.call(libsystem_library, "_free", {joined}));
            if (!status && static_cast<std::int32_t>(result) != 0) throw Errno{error};
            return result & 0xffffffffULL;
        });
    }
    // wchar_t* fgetwln(FILE*, size_t* len): fgetln's line widened (C locale), in a per-stream
    // buffer valid until the next call on that stream; not terminated.
    struct WideLines { std::mutex mutex; std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> buffers; };
    auto wide_lines = std::make_shared<WideLines>();
    p.libsystem("_fgetwln", [p, wide_lines](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto length_cell = p.malloc(8);
        const auto line = p.call(libsystem_library, "_fgetln", {s.x[0], length_cell});
        const auto length = m.read64(length_cell);
        static_cast<void>(p.call(libsystem_library, "_free", {length_cell}));
        if (!line) return 0;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        if (length) m.read_bytes(line, bytes);
        std::uint64_t buffer;
        {
            std::lock_guard lock(wide_lines->mutex);
            auto& entry = wide_lines->buffers[s.x[0]];
            if (entry.second < length) {
                const auto grown = p.call(libsystem_library, "_realloc", {entry.first, std::max<std::uint64_t>(length, 64) * 4});
                if (!grown) throw Errno{enomem};
                entry = {grown, std::max<std::uint64_t>(length, 64)};
            }
            buffer = entry.first;
        }
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            if (bytes[index] > 127) throw Errno{92};   // EILSEQ: the C locale is ASCII
            m.write32(buffer + index * 4, bytes[index]);
        }
        if (s.x[1]) m.write64(s.x[1], length);
        return buffer;
    }, 0);

    // ---- libproc ----
    // Dirty tracking (launchd's idle exit): this process's own state, kept here; another
    // process's needs launchd's rights (EPERM). Errors are returned, as libproc does.
    struct Dirty { std::mutex mutex; std::uint32_t tracking{}; bool dirty{}; };
    auto dirty = std::make_shared<Dirty>();
    const auto own = [p](std::uint64_t pid) { return static_cast<std::int32_t>(pid) == static_cast<std::int32_t>(p.call(libsystem_library, "_getpid", {})); };
    // int proc_track_dirty(pid_t, uint32_t flags): PROC_DIRTY_TRACK 1, ALLOW_IDLE_EXIT 2, DEFER 4, LAUNCH_IN_PROGRESS 8.
    p.libsystem("_proc_track_dirty", [dirty, own](CpuState& s, GuestMemory&) -> std::uint64_t {
        if (!own(s.x[0])) return eperm;
        const auto flags = static_cast<std::uint32_t>(s.x[1]);
        if (flags & ~0xfU) return einval;
        std::lock_guard lock(dirty->mutex);
        dirty->tracking = flags;
        return 0;
    });
    // int proc_set_dirty(pid_t, bool dirty): EINVAL until tracking is on.
    p.libsystem("_proc_set_dirty", [dirty, own](CpuState& s, GuestMemory&) -> std::uint64_t {
        if (!own(s.x[0])) return eperm;
        std::lock_guard lock(dirty->mutex);
        if (!(dirty->tracking & 1)) return einval;
        dirty->dirty = (s.x[1] & 0xff) != 0;
        return 0;
    });
    // int proc_get_dirty(pid_t, uint32_t* flags): TRACKED 1, ALLOWS_IDLE_EXIT 2, IS_DIRTY 4, LAUNCH_IS_IN_PROGRESS 8.
    p.libsystem("_proc_get_dirty", [dirty, own](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!own(s.x[0])) return eperm;
        if (!s.x[1]) return einval;
        std::lock_guard lock(dirty->mutex);
        std::uint32_t flags = 0;
        if (dirty->tracking & 1) flags |= 1;
        if (dirty->tracking & 2) flags |= 2;
        if (dirty->dirty) flags |= 4;
        if (dirty->tracking & 8) flags |= 8;
        m.write32(s.x[1], flags);
        return 0;
    });
    // int proc_clear_dirty(pid_t, uint32_t flags): PROC_DIRTY_LAUNCH_IN_PROGRESS (8) ends the launch.
    p.libsystem("_proc_clear_dirty", [dirty, own](CpuState& s, GuestMemory&) -> std::uint64_t {
        if (!own(s.x[0])) return eperm;
        std::lock_guard lock(dirty->mutex);
        dirty->tracking &= ~(static_cast<std::uint32_t>(s.x[1]) & 8U);
        return 0;
    });
    // int proc_libversion(int* major, int* minor): libproc 1.1.
    p.libsystem("_proc_libversion", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (s.x[0]) m.write32(s.x[0], 1);
        if (s.x[1]) m.write32(s.x[1], 1);
        return 0;
    });

    // ---- membership: the compatibility UUIDs (user FFFFEEEE-DDDD-CCCC-BBBB-AAAA<uid>, group
    // ABCDEFAB-CDEF-ABCD-EFAB-CDEF<gid>) ----
    constexpr std::array<std::uint8_t, 12> user_prefix{0xff, 0xff, 0xee, 0xee, 0xdd, 0xdd, 0xcc, 0xcc, 0xbb, 0xbb, 0xaa, 0xaa};
    constexpr std::array<std::uint8_t, 12> group_prefix{0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef};
    // int mbr_gid_to_uuid(gid_t, uuid_t)
    p.libsystem("_mbr_gid_to_uuid", [group_prefix](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[1]) return einval;
        m.write_bytes(s.x[1], group_prefix);
        const auto gid = static_cast<std::uint32_t>(s.x[0]);
        for (unsigned index = 0; index < 4; ++index) m.write8(s.x[1] + 12 + index, static_cast<std::uint8_t>(gid >> (24 - index * 8)));
        return 0;
    });
    // The id and type (ID_TYPE_UID 0, ID_TYPE_GID 1) a compatibility UUID names.
    const auto uuid_id = [user_prefix, group_prefix](GuestMemory& m, std::uint64_t uuid) -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
        std::array<std::uint8_t, 16> bytes{};
        m.read_bytes(uuid, bytes);
        const auto id = static_cast<std::uint32_t>(bytes[12]) << 24 | static_cast<std::uint32_t>(bytes[13]) << 16 |
                        static_cast<std::uint32_t>(bytes[14]) << 8 | bytes[15];
        if (std::equal(user_prefix.begin(), user_prefix.end(), bytes.begin())) return std::pair{id, 0U};
        if (std::equal(group_prefix.begin(), group_prefix.end(), bytes.begin())) return std::pair{id, 1U};
        return std::nullopt;
    };
    // int mbr_uuid_to_id(const uuid_t, id_t* id, int* type): ENOENT for other UUIDs.
    p.libsystem("_mbr_uuid_to_id", [uuid_id](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0] || !s.x[1] || !s.x[2]) return einval;
        const auto found = uuid_id(m, s.x[0]);
        if (!found) return enoent;
        m.write32(s.x[1], found->first);
        m.write32(s.x[2], found->second);
        return 0;
    });
    // struct passwd* getpwuuid(uuid_t) / int getpwuuid_r(uuid_t, struct passwd*, char*, size_t, struct passwd**)
    p.libsystem("_getpwuuid", [p, uuid_id](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0]) return 0;
        const auto found = uuid_id(m, s.x[0]);
        if (!found || found->second != 0) return 0;
        return p.call(libsystem_library, "_getpwuid", {found->first});
    }, 0);
    p.libsystem("_getpwuuid_r", [p, uuid_id](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (s.x[4]) m.write64(s.x[4], 0);
        if (!s.x[0]) return einval;
        const auto found = uuid_id(m, s.x[0]);
        if (!found || found->second != 0) return 0;   // not found: *result NULL, no error
        return p.call(libsystem_library, "_getpwuid_r", {found->first, s.x[1], s.x[2], s.x[3], s.x[4]}) & 0xffffffffULL;
    });

    // ---- <execinfo.h> ----
    // int backtrace_from_fp(void* fp, void** array, int size): the return addresses of the frame
    // chain from fp ({previous fp, lr} records; pointer authentication bits cleared).
    p.libsystem("_backtrace_from_fp", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto size = static_cast<std::int32_t>(s.x[2]);
        std::int32_t count = 0;
        for (auto frame = s.x[0]; frame && !(frame & 15) && count < size && m.query_region(frame);) {
            const auto next = m.read64(frame), address = m.read64(frame + 8) & 0x0000ffffffffffffULL;
            if (!address) break;
            m.write64(s.x[1] + static_cast<std::uint64_t>(count++) * 8, address);
            if (next <= frame) break;
            frame = next;
        }
        return static_cast<std::uint32_t>(count);
    });
    // void backtrace_image_offsets(void* const* array, struct image_offset* offsets, int size):
    // {uuid_t of the image (its LC_UUID), uint32_t offset from its header} per address; zeros for
    // an address in no image.
    p.libsystem("_backtrace_image_offsets", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto size = static_cast<std::int32_t>(s.x[2]);
        const auto info = p.malloc(32);   // Dl_info
        for (std::int32_t index = 0; index < size; ++index) {
            const auto address = m.read64(s.x[0] + static_cast<std::uint64_t>(index) * 8);
            const auto entry = s.x[1] + static_cast<std::uint64_t>(index) * 20;
            m.fill_bytes(entry, 0, 20);
            if (!address || !p.call(libsystem_library, "_dladdr", {address, info})) continue;
            const auto header = m.read64(info + 8);
            if (!header || m.read32(header) != 0xfeedfacfU) continue;
            auto cursor = header + 32;
            for (std::uint32_t command = 0, commands = m.read32(header + 16); command < commands; ++command) {
                const auto kind = m.read32(cursor), length = m.read32(cursor + 4);
                if (length < 8) break;
                if (kind == 0x1b) { m.copy_bytes(entry, cursor + 8, 16); break; }   // LC_UUID
                cursor += length;
            }
            m.write32(entry + 16, static_cast<std::uint32_t>(address - header));
        }
        static_cast<void>(p.call(libsystem_library, "_free", {info}));
        return 0;
    });

    // int minherit(void* address, size_t length, int inherit): vm_inherit on this task's range.
    p.libsystem("_minherit", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto task = p.process().resolve(libsystem_library, "_mach_task_self_");
        const auto inherit = static_cast<std::uint32_t>(s.x[2]);
        if (!task || inherit > 2) throw Errno{einval};
        if (p.call(libsystem_library, "_vm_inherit", {m.read32(task->address), s.x[0], s.x[1], inherit}) & 0xffffffffULL) throw Errno{einval};
        return 0;
    });
    // statfs64 / fstatfs64 / getfsstat64 / getmntinfo64: on arm64 the 64-bit-inode forms are the plain ones.
    for (const auto& [name, target] : std::array<std::pair<const char*, const char*>, 4>{{
             {"_statfs64", "_statfs"}, {"_fstatfs64", "_fstatfs"}, {"_getfsstat64", "_getfsstat"}, {"_getmntinfo64", "_getmntinfo"}}}) {
        const std::string function = target;
        p.libsystem(name, [p, function](CpuState& s, GuestMemory&) -> std::uint64_t {
            return p.call(libsystem_library, function, {s.x[0], s.x[1], s.x[2]});
        });
    }
    // double atof_l(const char*, locale_t): strtod_l's value.
    p.libsystem("_atof_l", [&process](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto strtod = process.resolve(libsystem_library, "_strtod_l");
        if (!strtod) throw std::runtime_error("Darwin atof_l requires strtod_l");
        CallRegs registers;
        registers.x[0] = s.x[0]; registers.x[1] = 0; registers.x[2] = s.x[1];
        const auto result = EngineAdapter(process.calling_engine()).call_regs(strtod->address, registers);
        s.v[0][0] = result.v[0][0]; s.v[0][1] = result.v[0][1];
        return result.x0;
    });
    // const char* querylocale(int mask, locale_t): "C", the only locale (the mask names categories, LC_ALL_MASK 0x3f).
    const auto locale_name = process.allocate_guest(2);
    put_string(static_cast<GuestMemory&>(process.main_engine()), locale_name, "C");
    p.libsystem("_querylocale", [locale_name](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto mask = static_cast<std::uint32_t>(s.x[0]);
        if (!mask || (mask & ~0x3fU)) throw Errno{einval};
        return locale_name;
    }, 0);

    // ---- setmode(3)/getmode(3) ----
    // void* setmode(const char* mode): an octal mode or chmod(1) symbolic clauses ([ugoa]*[+-=][rwxXstugo]*...),
    // checked here and kept (with the umask of the moment) in a malloc'd block; getmode applies it.
    constexpr std::uint32_t mode_magic = 0x31534d41;   // "AMS1"
    p.libsystem("_setmode", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto text = bounded_string(m, s.x[0], 1024);
        if (text.empty()) throw Errno{einval};
        const bool octal = text.find_first_not_of("01234567") == std::string::npos;
        if (octal) { if (text.size() > 5 || std::stoul(text, nullptr, 8) > 07777) throw Errno{einval}; }
        else {
            std::size_t at = 0;
            while (true) {
                while (at < text.size() && std::string_view("ugoa").find(text[at]) != std::string_view::npos) ++at;
                if (at == text.size() || std::string_view("+-=").find(text[at]) == std::string_view::npos) throw Errno{einval};
                while (at < text.size() && std::string_view("+-=").find(text[at]) != std::string_view::npos) {
                    ++at;
                    while (at < text.size() && std::string_view("rwxXstugo").find(text[at]) != std::string_view::npos) ++at;
                }
                if (at == text.size()) break;
                if (text[at] != ',') throw Errno{einval};
                ++at;
            }
        }
        const auto mask = static_cast<std::uint32_t>(p.call(libsystem_library, "_umask", {0}));
        static_cast<void>(p.call(libsystem_library, "_umask", {mask}));
        const auto block = p.malloc(8 + text.size() + 1);
        m.write32(block, mode_magic);
        m.write32(block + 4, mask & 0777);
        put_string(m, block + 8, text);
        return block;
    }, 0);
    // mode_t getmode(const void* set, mode_t mode): the mode with the set's changes; the file type stays.
    p.libsystem("_getmode", [mode_magic](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto original = static_cast<std::uint32_t>(s.x[1] & 0xffff);
        if (!s.x[0] || m.read32(s.x[0]) != mode_magic) return original;
        const auto mask = m.read32(s.x[0] + 4);
        const auto text = bounded_string(m, s.x[0] + 8, 1024);
        if (text.find_first_not_of("01234567") == std::string::npos)
            return (original & ~07777U) | (static_cast<std::uint32_t>(std::stoul(text, nullptr, 8)) & 07777U);
        auto mode = original;
        std::size_t at = 0;
        while (at <= text.size()) {
            std::uint32_t who = 0;
            for (; at < text.size() && std::string_view("ugoa").find(text[at]) != std::string_view::npos; ++at)
                who |= text[at] == 'u' ? 04700U : text[at] == 'g' ? 02070U : text[at] == 'o' ? 01007U : 07777U;
            const bool implicit = who == 0;
            if (implicit) who = 07777U;
            while (at < text.size() && std::string_view("+-=").find(text[at]) != std::string_view::npos) {
                const auto op = text[at++];
                std::uint32_t bits = 0;
                for (; at < text.size() && std::string_view("rwxXstugo").find(text[at]) != std::string_view::npos; ++at) {
                    switch (text[at]) {
                    case 'r': bits |= 0444; break;
                    case 'w': bits |= 0222; break;
                    case 'x': bits |= 0111; break;
                    case 'X': if ((original & 0170000) == 0040000 || (mode & 0111)) bits |= 0111; break;
                    case 's': bits |= 06000; break;
                    case 't': bits |= 01000; break;
                    // Copy another class's current bits to every class.
                    case 'u': { const auto v = (mode >> 6) & 7; bits |= v << 6 | v << 3 | v; break; }
                    case 'g': { const auto v = (mode >> 3) & 7; bits |= v << 6 | v << 3 | v; break; }
                    case 'o': { const auto v = mode & 7; bits |= v << 6 | v << 3 | v; break; }
                    default: break;
                    }
                }
                auto affected = bits & who;
                if (implicit) affected &= ~mask;
                if (op == '+') mode |= affected;
                else if (op == '-') mode &= ~affected;
                else { mode &= ~(who & (implicit ? ~mask & 07777U : 07777U)); mode |= affected; }
            }
            if (at >= text.size()) break;
            ++at;   // ','
        }
        return mode & 0xffff;
    });

    // ---- <time.h> ----
    // time_t timeoff(struct tm*, long offset): tm read as the time `offset` seconds east of UTC.
    p.libsystem("_timeoff", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0]) throw Errno{efault};
        m.write32(s.x[0] + 32, 0);   // tm_isdst
        const auto time = static_cast<std::int64_t>(p.call(libsystem_library, "_timegm", {s.x[0]}));
        if (time == -1) return static_cast<std::uint64_t>(-1);
        return static_cast<std::uint64_t>(time - static_cast<std::int64_t>(s.x[1]));
    });
    p.libsystem("_strptime_l", [p](CpuState& s, GuestMemory&) -> std::uint64_t {
        return p.call(libsystem_library, "_strptime", {s.x[0], s.x[1], s.x[2]});
    }, 0);
    p.libsystem("_ualarm", [](CpuState&, GuestMemory&) -> std::uint64_t {
        throw std::runtime_error("Darwin ualarm requires guest signal delivery, which is unsupported");
    });

    // ---- passphrases ----
    // readpassphrase(prompt, buf, size, flags): the prompt on the terminal (or standard error),
    // one line from it (or standard input) with echo off unless RPP_ECHO_ON; NULL on errors.
    constexpr std::uint64_t rpp_echo_on = 0x01, rpp_require_tty = 0x02, rpp_force_lower = 0x04, rpp_force_upper = 0x08,
                            rpp_seven_bit = 0x10, rpp_stdin = 0x20;
    const auto read_passphrase = [p](GuestMemory& m, std::uint64_t prompt, std::uint64_t buffer, std::uint64_t size,
                                     std::uint64_t flags) -> std::uint64_t {
        if (!buffer || !size) throw Errno{einval};
        const auto call = [&p](std::string_view name, std::initializer_list<std::uint64_t> arguments) {
            return p.call(libsystem_library, name, arguments);
        };
        std::int64_t input = 0, output = 2;
        bool opened = false;
        if (!(flags & rpp_stdin)) {
            const auto path = p.malloc(9);
            put_string(m, path, "/dev/tty");
            const auto descriptor = static_cast<std::int32_t>(call("_open", {path, 2}));
            static_cast<void>(call("_free", {path}));
            if (descriptor >= 0) { input = output = descriptor; opened = true; }
            else if (flags & rpp_require_tty) throw Errno{enotty_value};
        }
        const auto termios = p.malloc(72);
        bool restore = false;
        if (!(flags & rpp_echo_on) && call("_isatty", {static_cast<std::uint64_t>(input)}) == 1 &&
            static_cast<std::int32_t>(call("_tcgetattr", {static_cast<std::uint64_t>(input), termios})) == 0) {
            std::array<std::uint8_t, 72> bytes{};
            m.read_bytes(termios, bytes);
            const auto local = m.read64(termios + 24);
            m.write64(termios + 24, (local & ~0x8ULL) | 0x10ULL);   // ECHO off, ECHONL on
            restore = static_cast<std::int32_t>(call("_tcsetattr", {static_cast<std::uint64_t>(input), 2, termios})) == 0;
            m.write_bytes(termios, bytes);
        }
        if (prompt) {
            const auto text = bounded_string(m, prompt, 4096);
            if (!text.empty()) static_cast<void>(call("_write", {static_cast<std::uint64_t>(output), prompt, text.size()}));
        }
        const auto byte = p.malloc(1);
        std::uint64_t length = 0;
        bool failed = false;
        while (true) {
            const auto got = static_cast<std::int64_t>(call("_read", {static_cast<std::uint64_t>(input), byte, 1}));
            if (got < 0) { failed = true; break; }
            if (got == 0) break;
            auto c = m.read8(byte);
            if (c == '\n' || c == '\r') break;
            if (flags & rpp_seven_bit) c &= 0x7f;
            if ((flags & rpp_force_lower) && c >= 'A' && c <= 'Z') c = static_cast<std::uint8_t>(c + 32);
            if ((flags & rpp_force_upper) && c >= 'a' && c <= 'z') c = static_cast<std::uint8_t>(c - 32);
            if (length + 1 < size) m.write8(buffer + length++, c);
        }
        const auto error = failed ? p.guest_errno() : 0;
        m.write8(buffer + length, 0);
        if (restore) static_cast<void>(call("_tcsetattr", {static_cast<std::uint64_t>(input), 2, termios}));
        static_cast<void>(call("_free", {byte}));
        static_cast<void>(call("_free", {termios}));
        if (opened) static_cast<void>(call("_close", {static_cast<std::uint64_t>(input)}));
        if (failed) throw Errno{error ? error : eio};
        return buffer;
    };
    p.libsystem("_readpassphrase", [read_passphrase](CpuState& s, GuestMemory& m) -> std::uint64_t {
        return read_passphrase(m, s.x[0], s.x[1], s.x[2], s.x[3]);
    }, 0);
    // char* getpass(const char* prompt): at most 128 characters, in a per-thread buffer.
    auto passwords = std::make_shared<ThreadBuffers>(process, 129);
    p.libsystem("_getpass", [read_passphrase, passwords](CpuState& s, GuestMemory& m) -> std::uint64_t {
        return read_passphrase(m, s.x[0], passwords->get(), 129, 0);
    }, 0);

    // ---- users and groups (pwcache) ----
    // user_from_uid(uid, nouser) / group_from_gid(gid, nogroup): the name; else NULL when
    // no* is set, or the number as text (per-thread buffer).
    auto numbers = std::make_shared<ThreadBuffers>(process, 16);
    for (const auto group : {false, true}) {
        p.libsystem(group ? "_group_from_gid" : "_user_from_uid", [p, numbers, group](CpuState& s, GuestMemory& m) -> std::uint64_t {
            const auto id = static_cast<std::uint32_t>(s.x[0]);
            if (const auto entry = p.call(libsystem_library, group ? "_getgrgid" : "_getpwuid", {id})) return m.read64(entry);
            if (s.x[1]) return 0;
            const auto buffer = numbers->get();
            put_string(m, buffer, std::to_string(id));
            return buffer;
        }, 0);
        // uid_from_user(name, uid_t*) / gid_from_group(name, gid_t*): 0, or -1 for an unknown name.
        p.libsystem(group ? "_gid_from_group" : "_uid_from_user", [p, group](CpuState& s, GuestMemory& m) -> std::uint64_t {
            const auto entry = p.call(libsystem_library, group ? "_getgrnam" : "_getpwnam", {s.x[0]});
            if (!entry) return static_cast<std::uint64_t>(-1);
            if (s.x[1]) m.write32(s.x[1], m.read32(entry + 16));   // pw_uid / gr_gid
            return 0;
        });
    }

    // ---- <execinfo.h> ----
    // void backtrace_symbols_fd(void* const* array, int size, int fd): backtrace_symbols' lines on fd.
    p.libsystem("_backtrace_symbols_fd", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto count = static_cast<std::int32_t>(s.x[1]);
        if (count <= 0) return 0;
        const auto lines = p.call(libsystem_library, "_backtrace_symbols", {s.x[0], static_cast<std::uint64_t>(count)});
        if (!lines) return 0;
        const auto newline = p.malloc(1);
        m.write8(newline, '\n');
        for (std::int32_t index = 0; index < count; ++index) {
            const auto line = m.read64(lines + static_cast<std::uint64_t>(index) * 8);
            if (!line) continue;
            const auto text = bounded_string(m, line, 4096);
            static_cast<void>(p.call(libsystem_library, "_write", {static_cast<std::uint32_t>(s.x[2]), line, text.size()}));
            static_cast<void>(p.call(libsystem_library, "_write", {static_cast<std::uint32_t>(s.x[2]), newline, 1}));
        }
        static_cast<void>(p.call(libsystem_library, "_free", {newline}));
        static_cast<void>(p.call(libsystem_library, "_free", {lines}));
        return 0;
    });

    // ---- I/O policies ----
    // getiopolicy_np(type, scope) / setiopolicy_np(type, scope, policy): IOPOL_SCOPE_PROCESS (0)
    // and IOPOL_SCOPE_DARWIN_BG (2) per process, IOPOL_SCOPE_THREAD (1) per thread; unset
    // policies read as 0 (IOPOL_DEFAULT). IOPOL_TYPE_DISK (0) takes IOPOL_DEFAULT ..
    // IOPOL_STANDARD (0..5); the vfs types (2..) take small flag values.
    struct Policies { std::mutex mutex; std::map<std::tuple<std::int32_t, std::int32_t, std::uint64_t>, std::int32_t> values; };
    auto policies = std::make_shared<Policies>();
    const auto policy_key = [&process](std::int32_t type, std::int32_t scope) {
        if (type < 0 || type == 1 || type > 15 || scope < 0 || scope > 2) throw Errno{einval};
        const auto thread = process.current_thread();
        return std::tuple{type, scope, scope == 1 && thread ? thread->identity : 0};
    };
    p.libsystem("_getiopolicy_np", [policies, policy_key](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto key = policy_key(static_cast<std::int32_t>(s.x[0]), static_cast<std::int32_t>(s.x[1]));
        std::lock_guard lock(policies->mutex);
        const auto found = policies->values.find(key);
        return found == policies->values.end() ? 0 : static_cast<std::uint32_t>(found->second);
    });
    p.libsystem("_setiopolicy_np", [policies, policy_key](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto type = static_cast<std::int32_t>(s.x[0]), policy = static_cast<std::int32_t>(s.x[2]);
        const auto key = policy_key(type, static_cast<std::int32_t>(s.x[1]));
        if (policy < 0 || policy > (type == 0 ? 5 : 7)) throw Errno{einval};
        std::lock_guard lock(policies->mutex);
        policies->values[key] = policy;
        return 0;
    });

    // int pthread_is_threaded_np(void): nonzero once the process has had a second thread (also one
    // that has ended).
    p.libsystem("_pthread_is_threaded_np", [&process](CpuState&, GuestMemory&) -> std::uint64_t {
        return process.threads_seen() > 1 ? 1 : 0;
    });

    // ---- fortified string functions ----
    p.libsystem("___chk_fail_overflow", [](CpuState&, GuestMemory&) -> std::uint64_t { buffer_overflow(); });
    // char* __stpcpy_chk(dst, src, dstlen)
    p.libsystem("___stpcpy_chk", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto bytes = m.read_string(s.x[1], 1, 1ULL << 30);
        if (bytes.size() + 1 > s.x[2]) buffer_overflow();
        m.write_bytes(s.x[0], bytes);
        m.write8(s.x[0] + bytes.size(), 0);
        return s.x[0] + bytes.size();
    }, 0);
    // char* __stpncpy_chk(dst, src, n, dstlen): stpncpy (NUL padding to n).
    p.libsystem("___stpncpy_chk", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (s.x[2] > s.x[3]) buffer_overflow();
        const auto bytes = m.read_string(s.x[1], 1, static_cast<std::size_t>(s.x[2]));
        m.write_bytes(s.x[0], bytes);
        if (bytes.size() < s.x[2]) m.fill_bytes(s.x[0] + bytes.size(), 0, s.x[2] - bytes.size());
        return s.x[0] + bytes.size();
    }, 0);
    // void* __memccpy_chk(dst, src, c, n, dstlen): memccpy; the bytes it copies must fit.
    p.libsystem("___memccpy_chk", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto c = static_cast<std::uint8_t>(s.x[2]);
        for (std::uint64_t index = 0; index < s.x[3]; ++index) {
            if (index >= s.x[4]) buffer_overflow();
            const auto value = m.read8(s.x[1] + index);
            m.write8(s.x[0] + index, value);
            if (value == c) return s.x[0] + index + 1;
        }
        return 0;
    }, 0);

    // const char* fmtcheck(const char* suspect, const char* default): suspect when the
    // arguments it consumes are a prefix of default's (same types), else default.
    p.libsystem("_fmtcheck", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0]) return s.x[1];
        const auto suspect = format_arguments(bounded_string(m, s.x[0], 1 << 16));
        const auto fallback = format_arguments(bounded_string(m, s.x[1], 1 << 16));
        if (!suspect || !fallback || suspect->size() > fallback->size()) return s.x[1];
        return std::equal(suspect->begin(), suspect->end(), fallback->begin()) ? s.x[0] : s.x[1];
    }, 0);

    // ---- <ctype.h> out-of-line helpers ("C" locale) ----
    // __istype(c, mask) / __isctype(c, mask) (0 outside 0..255): whether c has any of mask's classes.
    p.libsystem("___istype", [p](CpuState& s, GuestMemory&) -> std::uint64_t {
        return p.call(libsystem_library, "___maskrune", {s.x[0] & 0xffffffffULL, s.x[1] & 0xffffffffULL}) ? 1 : 0;
    });
    p.libsystem("___isctype", [p](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto c = static_cast<std::int32_t>(s.x[0]);
        if (c < 0 || c > 255) return 0;
        return p.call(libsystem_library, "___maskrune", {static_cast<std::uint32_t>(c), s.x[1] & 0xffffffffULL}) ? 1 : 0;
    });
    p.libsystem("___tolower_l", [p](CpuState& s, GuestMemory&) -> std::uint64_t { return p.call(libsystem_library, "___tolower", {s.x[0]}); });
    p.libsystem("___toupper_l", [p](CpuState& s, GuestMemory&) -> std::uint64_t { return p.call(libsystem_library, "___toupper", {s.x[0]}); });
    // _RuneLocale* _CurrentRuneLocale: the C locale's table.
    if (const auto rune = process.resolve(libsystem_library, "__DefaultRuneLocale")) {
        std::array<std::uint8_t, 8> pointer{};
        for (unsigned index = 0; index < 8; ++index) pointer[index] = static_cast<std::uint8_t>(rune->address >> (8 * index));
        static_cast<void>(p.data(libsystem_library, "__CurrentRuneLocale", 8, pointer));
    }
}

} // namespace alchemy::export_fill
