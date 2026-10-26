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

    // int pthread_is_threaded_np(void): nonzero once the process has had a second thread.
    auto threaded = std::make_shared<std::atomic<bool>>(false);
    p.libsystem("_pthread_is_threaded_np", [&process, threaded](CpuState&, GuestMemory&) -> std::uint64_t {
        if (!*threaded && process.thread_identities().size() > 1) *threaded = true;
        return *threaded ? 1 : 0;
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
