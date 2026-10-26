#include "alchemy/darwin_support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// libc names the core lacked (both builds): <string.h> (stpcpy, strsep, memmem, strcasestr,
// strnstr, index/rindex, ffs/fls, bcmp, timingsafe_bcmp, memset_s, strmode, swab, ...),
// <wchar.h> (wcscat, wcsdup, wcsspn, wcwidth, wcslcpy, ...), <search.h> (hsearch, tsearch,
// lsearch, insque), <stdlib.h> (putenv, strtonum, reallocarray, getsubopt, heapsort,
// mergesort, radixsort, quick_exit, a64l/l64a, mkostemp, mkdtemps), <time.h> (timelocal,
// timespec_get, posix2time), <ctype.h> (digittoint, ishexnumber, isnumber, ...) and the BSD
// and System V signal calls over sigaction/sigprocmask (sigset, sighold, sigblock,
// siginterrupt, psignal, sys_siglist; sigpause is process/process_model.cpp's), dprintf, the
// _unlocked stdio forms, fdatasync, dirname_r/basename_r, mkpath_np, ftw/nftw and ether_ntoa/aton. Comparators and callbacks are guest functions, called
// on the calling guest thread. The C locale is ASCII (as misc/locale.cpp's).
namespace alchemy {
namespace {

constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";
constexpr std::uint64_t string_limit = 1ULL << 20;
constexpr std::uint32_t enoent = 2, einval = 22, erange = 34, enomem = 12, eexist = 17;
struct LibcError { std::uint32_t value; };

std::uint64_t add(std::uint64_t address, std::uint64_t amount) {
    if (amount > UINT64_MAX - address) throw LibcError{14};
    return address + amount;
}
std::string text(const transmute::GuestMemory& m, std::uint64_t address, std::uint64_t maximum = string_limit) {
    if (!address) throw LibcError{14};
    std::string result;
    for (std::uint64_t at = 0;; ++at) {
        if (at >= maximum) throw std::runtime_error("Darwin string exceeds 1 MiB");
        const auto c = m.read8(add(address, at));
        if (!c) return result;
        result.push_back(static_cast<char>(c));
    }
}
void put(transmute::GuestMemory& m, std::uint64_t address, std::string_view value, bool terminate = true) {
    if (!value.empty()) m.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    if (terminate) m.write8(add(address, value.size()), 0);
}
std::u32string wide(const transmute::GuestMemory& m, std::uint64_t address, std::uint64_t maximum = string_limit / 4) {
    if (!address) throw LibcError{14};
    std::u32string result;
    for (std::uint64_t at = 0;; ++at) {
        if (at >= maximum) throw std::runtime_error("Darwin wide string exceeds limit");
        const auto c = m.read32(add(address, at * 4));
        if (!c) return result;
        result.push_back(static_cast<char32_t>(c));
    }
}
void put_wide(transmute::GuestMemory& m, std::uint64_t address, std::u32string_view value, bool terminate = true) {
    for (std::size_t at = 0; at < value.size(); ++at) m.write32(add(address, at * 4), static_cast<std::uint32_t>(value[at]));
    if (terminate) m.write32(add(address, value.size() * 4), 0);
}
std::uint8_t lower(std::uint8_t c) { return static_cast<std::uint8_t>(c >= 'A' && c <= 'Z' ? c + 32 : c); }
std::uint32_t lower_wide(std::uint32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

struct State {
    explicit State(ProcessContext& value) : context(value) {}
    ProcessContext& context;
    std::mutex mutex;
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> arguments) {
        const auto found = context.resolve(system_library, name);
        if (!found) throw std::runtime_error("Darwin libc: " + std::string(name.substr(1)) + " is not provided");
        return context.calling_engine().invoke_callback(found->address, std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t invoke(std::uint64_t function, std::initializer_list<std::uint64_t> arguments) {
        return context.calling_engine().invoke_callback(function, std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t malloc(std::uint64_t size) {
        const auto value = call("_malloc", {std::max<std::uint64_t>(size, 1)});
        if (!value) throw LibcError{enomem};
        return value;
    }
    std::uint32_t guest_errno() { return context.calling_engine().read32(context.current_thread()->errno_address); }
    // <search.h>: the one hsearch table, quick_exit's functions.
    struct Hash { std::vector<std::uint64_t> entries; std::uint64_t table{}, capacity{}, used{}; bool live{}; } hash;
    std::vector<std::uint64_t> quick_exit_functions;
};
using Operation = std::function<std::uint64_t(transmute::CpuState&, transmute::GuestMemory&)>;
void install(const std::shared_ptr<State>& state, std::string symbol, Operation operation, std::uint64_t failure = UINT64_MAX) {
    static_cast<void>(state->context.install_function(std::move(symbol), [state, operation = std::move(operation), failure](auto& s, auto& m) {
        try { s.x[0] = operation(s, m); }
        catch (const LibcError& error) { state->context.set_errno(error.value); s.x[0] = failure; }
        catch (const std::bad_alloc&) { state->context.set_errno(enomem); s.x[0] = failure; }
    }));
}
std::int32_t compared(std::uint64_t value) { return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value)); }

void install_strings(const std::shared_ptr<State>& state) {
    // stpcpy / stpncpy: the end of the copy.
    install(state, "_stpcpy", [](auto& s, auto& m) { const auto value = text(m, s.x[1]); put(m, s.x[0], value); return s.x[0] + value.size(); });
    install(state, "_stpncpy", [](auto& s, auto& m) {
        const auto length = s.x[2];
        std::uint64_t at = 0;
        for (; at < length; ++at) { const auto c = m.read8(add(s.x[1], at)); if (!c) break; m.write8(add(s.x[0], at), c); }
        const auto end = s.x[0] + at;
        for (auto rest = at; rest < length; ++rest) m.write8(add(s.x[0], rest), 0);
        return end;
    });
    // char* strsep(char** stringp, const char* delim)
    install(state, "_strsep", [](auto& s, auto& m) {
        const auto start = m.read64(s.x[0]);
        if (!start) return std::uint64_t{0};
        const auto delimiters = text(m, s.x[1]);
        for (std::uint64_t at = 0; at < string_limit; ++at) {
            const auto c = m.read8(add(start, at));
            if (delimiters.find(static_cast<char>(c)) != std::string::npos || (!c && delimiters.empty())) {
                if (!c) { m.write64(s.x[0], 0); return start; }
                m.write8(start + at, 0); m.write64(s.x[0], start + at + 1); return start;
            }
            if (!c) { m.write64(s.x[0], 0); return start; }
        }
        throw std::runtime_error("Darwin strsep exceeds 1 MiB");
    }, 0);
    // void* memmem(const void* big, size_t big_len, const void* little, size_t little_len)
    install(state, "_memmem", [](auto& s, auto& m) -> std::uint64_t {
        if (!s.x[3]) return s.x[0];
        if (s.x[3] > s.x[1] || s.x[1] > (1ULL << 30)) return 0;
        std::vector<std::uint8_t> big(static_cast<std::size_t>(s.x[1])), little(static_cast<std::size_t>(s.x[3]));
        m.read_bytes(s.x[0], big); m.read_bytes(s.x[2], little);
        const auto found = std::search(big.begin(), big.end(), little.begin(), little.end());
        return found == big.end() ? 0 : s.x[0] + static_cast<std::uint64_t>(found - big.begin());
    }, 0);
    // void* memccpy(dst, src, int c, size_t n): just past the copied c, or NULL.
    install(state, "_memccpy", [](auto& s, auto& m) -> std::uint64_t {
        const auto stop = static_cast<std::uint8_t>(s.x[2]);
        for (std::uint64_t at = 0; at < s.x[3]; ++at) {
            const auto c = m.read8(add(s.x[1], at));
            m.write8(add(s.x[0], at), c);
            if (c == stop) return s.x[0] + at + 1;
        }
        return 0;
    }, 0);
    // strcasestr / strnstr: the first occurrence.
    const auto search = [](transmute::GuestMemory& m, std::uint64_t big, std::string little, std::uint64_t limit, bool fold) -> std::uint64_t {
        if (little.empty()) return big;
        std::string haystack;
        for (std::uint64_t at = 0; at < limit; ++at) { const auto c = m.read8(add(big, at)); if (!c) break; haystack.push_back(static_cast<char>(c)); }
        if (fold) {
            for (auto& c : haystack) c = static_cast<char>(lower(static_cast<std::uint8_t>(c)));
            for (auto& c : little) c = static_cast<char>(lower(static_cast<std::uint8_t>(c)));
        }
        const auto at = haystack.find(little);
        return at == std::string::npos ? 0 : big + at;
    };
    install(state, "_strcasestr", [search](auto& s, auto& m) { return search(m, s.x[0], text(m, s.x[1]), string_limit, true); }, 0);
    install(state, "_strcasestr_l", [search](auto& s, auto& m) { return search(m, s.x[0], text(m, s.x[1]), string_limit, true); }, 0);
    install(state, "_strnstr", [search](auto& s, auto& m) { return search(m, s.x[0], text(m, s.x[1]), std::min(s.x[2], string_limit), false); }, 0);
    // index / rindex are strchr / strrchr.
    for (const bool reverse : {false, true})
        install(state, reverse ? "_rindex" : "_index", [reverse](auto& s, auto& m) {
            const auto wanted = static_cast<std::uint8_t>(s.x[1]);
            std::uint64_t found = 0;
            for (std::uint64_t at = 0; at < string_limit; ++at) {
                const auto c = m.read8(add(s.x[0], at));
                if (c == wanted) { found = s.x[0] + at; if (!reverse) return found; }
                if (!c) return found;
            }
            throw std::runtime_error("Darwin index exceeds 1 MiB");
        }, 0);
    // ffs / fls: the first / last set bit, counted from 1 (0 for none).
    for (const auto& [name, bits] : std::array<std::pair<const char*, unsigned>, 3>{{{"_ffs", 32}, {"_ffsl", 64}, {"_ffsll", 64}}})
        install(state, name, [bits = bits](auto& s, auto&) {
            const auto value = bits == 32 ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.x[0])) : s.x[0];
            return value ? static_cast<std::uint64_t>(std::countr_zero(value) + 1) : 0;
        });
    for (const auto& [name, bits] : std::array<std::pair<const char*, unsigned>, 3>{{{"_fls", 32}, {"_flsl", 64}, {"_flsll", 64}}})
        install(state, name, [bits = bits](auto& s, auto&) {
            const auto value = bits == 32 ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.x[0])) : s.x[0];
            return value ? static_cast<std::uint64_t>(64 - std::countl_zero(value)) : 0;
        });
    // bcmp: 0 when equal; timingsafe_bcmp reads every byte.
    for (const bool timing : {false, true})
        install(state, timing ? "_timingsafe_bcmp" : "_bcmp", [timing](auto& s, auto& m) {
            if (s.x[2] > (1ULL << 30)) throw LibcError{einval};
            std::vector<std::uint8_t> left(static_cast<std::size_t>(s.x[2])), right(left.size());
            if (!left.empty()) { m.read_bytes(s.x[0], left); m.read_bytes(s.x[1], right); }
            std::uint8_t difference = 0;
            for (std::size_t at = 0; at < left.size(); ++at) {
                difference |= static_cast<std::uint8_t>(left[at] ^ right[at]);
                if (!timing && difference) return std::uint64_t{1};
            }
            return std::uint64_t{difference ? 1U : 0U};
        });
    // errno_t memset_s(void* s, rsize_t smax, int c, rsize_t n): EINVAL for NULL, E2BIG when
    // smax exceeds RSIZE_MAX; otherwise fills min(n, smax) bytes, EOVERFLOW when n > smax.
    install(state, "_memset_s", [](auto& s, auto& m) -> std::uint64_t {
        constexpr std::uint64_t rsize_max = UINT64_MAX >> 1, e2big = 7, eoverflow = 84;
        if (!s.x[0]) return einval;
        if (s.x[1] > rsize_max) return e2big;
        const auto count = std::min(s.x[1], s.x[3]);
        if (count) m.fill_bytes(s.x[0], static_cast<std::uint8_t>(s.x[2]), count);
        return s.x[3] > s.x[1] ? eoverflow : 0;
    });
    install(state, "_memset_pattern8", [](auto& s, auto& m) {
        if (s.x[2] > (1ULL << 30)) throw LibcError{einval};
        std::array<std::uint8_t, 8> pattern{};
        m.read_bytes(s.x[1], pattern);
        for (std::uint64_t at = 0; at < s.x[2]; at += 8)
            m.write_bytes(add(s.x[0], at), std::span(pattern).first(static_cast<std::size_t>(std::min<std::uint64_t>(8, s.x[2] - at))));
        return s.x[0];
    });
    // void strmode(mode_t mode, char* bp): "drwxr-xr-x " (eleven characters and a NUL).
    install(state, "_strmode", [](auto& s, auto& m) {
        const auto mode = static_cast<std::uint32_t>(s.x[0]);
        std::string value(11, '-');
        switch (mode & 0170000) {
        case 0040000: value[0] = 'd'; break; case 0020000: value[0] = 'c'; break; case 0060000: value[0] = 'b'; break;
        case 0120000: value[0] = 'l'; break; case 0140000: value[0] = 's'; break; case 0010000: value[0] = 'p'; break;
        case 0160000: value[0] = 'w'; break; default: break;
        }
        const char letters[] = "rwxrwxrwx";
        for (unsigned bit = 0; bit < 9; ++bit) if (mode & (0400U >> bit)) value[1 + bit] = letters[bit];
        if (mode & 04000) value[3] = (mode & 0100) ? 's' : 'S';
        if (mode & 02000) value[6] = (mode & 0010) ? 's' : 'S';
        if (mode & 01000) value[9] = (mode & 0001) ? 't' : 'T';
        value[10] = ' ';
        put(m, s.x[1], value);
        return std::uint64_t{0};
    });
    // void swab(const void* from, void* to, ssize_t len): byte pairs swapped.
    install(state, "_swab", [](auto& s, auto& m) {
        const auto length = static_cast<std::int64_t>(s.x[2]);
        for (std::int64_t at = 0; at + 1 < length; at += 2) {
            const auto first = m.read8(add(s.x[0], at)), second = m.read8(add(s.x[0], at + 1));
            m.write8(add(s.x[1], at), second); m.write8(add(s.x[1], at + 1), first);
        }
        return std::uint64_t{0};
    });
}

void install_wide(const std::shared_ptr<State>& state) {
    install(state, "_wcscat", [](auto& s, auto& m) { const auto head = wide(m, s.x[0]); put_wide(m, s.x[0] + head.size() * 4, wide(m, s.x[1])); return s.x[0]; });
    install(state, "_wcsncat", [](auto& s, auto& m) {
        const auto head = wide(m, s.x[0]);
        auto tail = wide(m, s.x[1]);
        if (tail.size() > s.x[2]) tail.resize(static_cast<std::size_t>(s.x[2]));
        put_wide(m, s.x[0] + head.size() * 4, tail);
        return s.x[0];
    });
    for (const bool complement : {false, true})
        install(state, complement ? "_wcscspn" : "_wcsspn", [complement](auto& s, auto& m) {
            const auto value = wide(m, s.x[0]), set = wide(m, s.x[1]);
            std::uint64_t at = 0;
            for (; at < value.size(); ++at) if ((set.find(value[static_cast<std::size_t>(at)]) != std::u32string::npos) == complement) break;
            return at;
        });
    install(state, "_wcsdup", [state](auto& s, auto& m) {
        const auto value = wide(m, s.x[0]);
        const auto copy = state->malloc((value.size() + 1) * 4);
        put_wide(m, copy, value);
        return copy;
    }, 0);
    install(state, "_wcsnlen", [](auto& s, auto& m) {
        std::uint64_t at = 0;
        for (; at < s.x[1]; ++at) if (!m.read32(add(s.x[0], at * 4))) break;
        return at;
    });
    for (const bool bounded : {false, true})
        install(state, bounded ? "_wcsncasecmp" : "_wcscasecmp", [bounded](auto& s, auto& m) {
            const auto limit = bounded ? s.x[2] : string_limit;
            for (std::uint64_t at = 0; at < limit; ++at) {
                const auto a = lower_wide(m.read32(add(s.x[0], at * 4))), b = lower_wide(m.read32(add(s.x[1], at * 4)));
                if (a != b) return static_cast<std::uint64_t>(static_cast<std::int64_t>(a < b ? -1 : 1));
                if (!a) return std::uint64_t{0};
            }
            return std::uint64_t{0};
        });
    // wcwidth / wcswidth in the C locale: printable ASCII is one column, NUL none, the rest -1.
    const auto width = [](std::uint32_t c) -> std::int64_t { return c == 0 ? 0 : (c >= 0x20 && c < 0x7f) ? 1 : -1; };
    install(state, "_wcwidth", [width](auto& s, auto&) { return static_cast<std::uint64_t>(width(static_cast<std::uint32_t>(s.x[0]))); });
    install(state, "_wcswidth", [width](auto& s, auto& m) {
        std::int64_t total = 0;
        for (std::uint64_t at = 0; at < s.x[1]; ++at) {
            const auto c = m.read32(add(s.x[0], at * 4));
            if (!c) break;
            const auto one = width(c);
            if (one < 0) return UINT64_MAX;
            total += one;
        }
        return static_cast<std::uint64_t>(total);
    });
    // wcslcpy / wcslcat: the length the full result would have.
    install(state, "_wcslcpy", [](auto& s, auto& m) {
        const auto source = wide(m, s.x[1]);
        if (s.x[2]) put_wide(m, s.x[0], std::u32string_view(source).substr(0, static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), s.x[2] - 1))));
        return static_cast<std::uint64_t>(source.size());
    });
    install(state, "_wcslcat", [](auto& s, auto& m) {
        const auto source = wide(m, s.x[1]);
        std::uint64_t head = 0;
        while (head < s.x[2] && m.read32(add(s.x[0], head * 4))) ++head;
        if (head == s.x[2]) return head + source.size();
        put_wide(m, s.x[0] + head * 4, std::u32string_view(source).substr(0, static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), s.x[2] - head - 1))));
        return head + source.size();
    });
    install(state, "_wcpcpy", [](auto& s, auto& m) { const auto value = wide(m, s.x[1]); put_wide(m, s.x[0], value); return s.x[0] + value.size() * 4; });
    install(state, "_wcpncpy", [](auto& s, auto& m) {
        std::uint64_t at = 0;
        for (; at < s.x[2]; ++at) { const auto c = m.read32(add(s.x[1], at * 4)); if (!c) break; m.write32(add(s.x[0], at * 4), c); }
        const auto end = s.x[0] + at * 4;
        for (auto rest = at; rest < s.x[2]; ++rest) m.write32(add(s.x[0], rest * 4), 0);
        return end;
    });
}

void install_search(const std::shared_ptr<State>& state) {
    // <search.h> hash table: hcreate(nel) / hdestroy / hsearch(ENTRY {char* key; void* data}, ACTION).
    // Open addressing over guest ENTRY slots (so hsearch returns a stable ENTRY*); keys compare
    // as strings; a full table makes ENTER fail (ENOMEM).
    install(state, "_hcreate", [state](auto& s, auto& m) -> std::uint64_t {
        std::lock_guard lock(state->mutex);
        if (state->hash.live) return 0;
        const auto capacity = std::max<std::uint64_t>(16, std::bit_ceil(std::max<std::uint64_t>(s.x[0], 1) * 2));
        if (capacity > (1ULL << 24)) throw LibcError{enomem};
        state->hash.table = state->malloc(capacity * 16);
        m.fill_bytes(state->hash.table, 0, capacity * 16);
        state->hash.capacity = capacity; state->hash.used = 0; state->hash.live = true;
        return 1;
    }, 0);
    install(state, "_hdestroy", [state](auto&, auto&) {
        std::uint64_t table{};
        { std::lock_guard lock(state->mutex); if (!state->hash.live) return std::uint64_t{0}; table = state->hash.table; state->hash = {}; }
        static_cast<void>(state->call("_free", {table}));
        return std::uint64_t{0};
    });
    install(state, "_hsearch", [state](auto& s, auto& m) -> std::uint64_t {
        const auto key = s.x[0], data = s.x[1];
        const auto action = static_cast<std::uint32_t>(s.x[2]);   // FIND 0, ENTER 1
        const auto wanted = text(m, key);
        std::lock_guard lock(state->mutex);
        auto& hash = state->hash;
        if (!hash.live) throw LibcError{einval};
        std::uint64_t index = std::hash<std::string>{}(wanted) & (hash.capacity - 1);
        for (std::uint64_t probes = 0; probes < hash.capacity; ++probes, index = (index + 1) & (hash.capacity - 1)) {
            const auto slot = hash.table + index * 16;
            const auto present = m.read64(slot);
            if (!present) {
                if (action != 1) throw LibcError{3};   // ESRCH: not found
                if (hash.used + 1 >= hash.capacity) throw LibcError{enomem};
                m.write64(slot, key); m.write64(slot + 8, data); ++hash.used;
                return slot;
            }
            if (text(m, present) == wanted) return slot;
        }
        throw LibcError{action == 1 ? enomem : 3};
    }, 0);
    // Binary search trees: node {const void* key; node* left; node* right} (malloc'd), unbalanced
    // as BSD's. tsearch adds, tfind looks up, tdelete removes (returns the parent, or the root
    // pointer for the root), twalk visits (preorder 0, postorder 1, endorder 2, leaf 3), tdestroy frees.
    install(state, "_tsearch", [state](auto& s, auto& m) -> std::uint64_t {
        if (!s.x[1]) return 0;
        auto link = s.x[1];
        for (auto node = m.read64(link); node; node = m.read64(link)) {
            const auto order = compared(state->invoke(s.x[2], {s.x[0], m.read64(node)}));
            if (!order) return node;
            link = node + (order < 0 ? 8 : 16);
        }
        const auto node = state->malloc(24);
        m.write64(node, s.x[0]); m.write64(node + 8, 0); m.write64(node + 16, 0);
        m.write64(link, node);
        return node;
    }, 0);
    install(state, "_tfind", [state](auto& s, auto& m) -> std::uint64_t {
        if (!s.x[1]) return 0;
        for (auto node = m.read64(s.x[1]); node;) {
            const auto order = compared(state->invoke(s.x[2], {s.x[0], m.read64(node)}));
            if (!order) return node;
            node = m.read64(node + (order < 0 ? 8 : 16));
        }
        return 0;
    }, 0);
    install(state, "_tdelete", [state](auto& s, auto& m) -> std::uint64_t {
        if (!s.x[1] || !m.read64(s.x[1])) return 0;
        auto link = s.x[1];
        std::uint64_t parent = s.x[1];   // the root's "parent" is the root pointer, as BSD returns it
        for (auto node = m.read64(link);;) {
            const auto order = compared(state->invoke(s.x[2], {s.x[0], m.read64(node)}));
            if (!order) {
                const auto left = m.read64(node + 8), right = m.read64(node + 16);
                std::uint64_t replacement{};
                if (!left) replacement = right;
                else if (!right) replacement = left;
                else {
                    // The right subtree's smallest node takes the deleted node's place.
                    auto successor_link = node + 16;
                    auto successor = right;
                    while (m.read64(successor + 8)) { successor_link = successor + 8; successor = m.read64(successor + 8); }
                    m.write64(successor_link, m.read64(successor + 16));
                    m.write64(successor + 8, left); m.write64(successor + 16, m.read64(node + 16));
                    replacement = successor;
                }
                m.write64(link, replacement);
                static_cast<void>(state->call("_free", {node}));
                return parent;
            }
            parent = node;
            link = node + (order < 0 ? 8 : 16);
            node = m.read64(link);
            if (!node) return 0;
        }
    }, 0);
    install(state, "_twalk", [state](auto& s, auto& m) {
        std::function<void(std::uint64_t, std::uint64_t)> walk = [&](std::uint64_t node, std::uint64_t level) {
            const auto left = m.read64(node + 8), right = m.read64(node + 16);
            if (!left && !right) { static_cast<void>(state->invoke(s.x[1], {node, 3, level})); return; }
            static_cast<void>(state->invoke(s.x[1], {node, 0, level}));
            if (left) walk(left, level + 1);
            static_cast<void>(state->invoke(s.x[1], {node, 1, level}));
            if (right) walk(right, level + 1);
            static_cast<void>(state->invoke(s.x[1], {node, 2, level}));
        };
        if (s.x[0] && s.x[1]) walk(s.x[0], 0);
        return std::uint64_t{0};
    });
    install(state, "_tdestroy", [state](auto& s, auto& m) {
        std::function<void(std::uint64_t)> destroy = [&](std::uint64_t node) {
            if (!node) return;
            destroy(m.read64(node + 8)); destroy(m.read64(node + 16));
            if (s.x[1]) static_cast<void>(state->invoke(s.x[1], {m.read64(node)}));
            static_cast<void>(state->call("_free", {node}));
        };
        destroy(s.x[0]);
        return std::uint64_t{0};
    });
    // lfind / lsearch(key, base, size_t* nelp, width, compar): linear; lsearch appends a miss.
    for (const bool append : {false, true})
        install(state, append ? "_lsearch" : "_lfind", [state, append](auto& s, auto& m) -> std::uint64_t {
            const auto count = m.read64(s.x[2]), width = s.x[3];
            for (std::uint64_t at = 0; at < count; ++at) {
                const auto element = s.x[1] + at * width;
                if (!compared(state->invoke(s.x[4], {s.x[0], element}))) return element;
            }
            if (!append) return 0;
            const auto element = s.x[1] + count * width;
            if (width > (1ULL << 24)) throw LibcError{einval};
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width));
            m.read_bytes(s.x[0], bytes); m.write_bytes(element, bytes);
            m.write64(s.x[2], count + 1);
            return element;
        }, 0);
    // insque(element, prev) / remque(element): {next, prev} at the start of each element.
    install(state, "_insque", [](auto& s, auto& m) {
        const auto element = s.x[0], previous = s.x[1];
        if (!previous) { m.write64(element, 0); m.write64(element + 8, 0); return std::uint64_t{0}; }
        const auto next = m.read64(previous);
        m.write64(element, next); m.write64(element + 8, previous);
        m.write64(previous, element);
        if (next) m.write64(next + 8, element);
        return std::uint64_t{0};
    });
    install(state, "_remque", [](auto& s, auto& m) {
        const auto next = m.read64(s.x[0]), previous = m.read64(s.x[0] + 8);
        if (next) m.write64(next + 8, previous);
        if (previous) m.write64(previous, next);
        return std::uint64_t{0};
    });
}

void install_stdlib(const std::shared_ptr<State>& state) {
    auto& context = state->context;
    // int putenv(char* string): "name=value" (setenv's copy; the string itself is not kept).
    install(state, "_putenv", [state](auto& s, auto& m) {
        const auto value = text(m, s.x[0]);
        const auto equals = value.find('=');
        if (equals == std::string::npos || equals == 0) throw LibcError{einval};
        const auto name = state->malloc(value.size() + 1);
        put(m, name, value);
        m.write8(name + equals, 0);
        const auto result = state->call("_setenv", {name, name + equals + 1, 1});
        const auto error = state->guest_errno();
        static_cast<void>(state->call("_free", {name}));
        if (static_cast<std::int32_t>(result) != 0) throw LibcError{error};
        return std::uint64_t{0};
    });
    // long long strtonum(const char* s, long long minval, long long maxval, const char** errstr)
    const auto messages = std::make_shared<std::array<std::uint64_t, 3>>();
    for (std::size_t index = 0; index < 3; ++index) {
        static constexpr std::array<std::string_view, 3> words{"invalid", "too small", "too large"};
        (*messages)[index] = context.allocate_guest(16);
        put(context.calling_engine(), (*messages)[index], words[index]);
    }
    install(state, "_strtonum", [messages](auto& s, auto& m) -> std::uint64_t {
        const auto minimum = static_cast<std::int64_t>(s.x[1]), maximum = static_cast<std::int64_t>(s.x[2]);
        const auto fail = [&](std::size_t which, std::uint32_t error) -> std::uint64_t {
            if (s.x[3]) m.write64(s.x[3], (*messages)[which]);
            throw LibcError{error};
        };
        if (minimum > maximum) return fail(0, einval);
        const auto value = text(m, s.x[0], 4096);
        std::size_t at = 0;
        while (at < value.size() && std::isspace(static_cast<unsigned char>(value[at]))) ++at;
        bool negative = false;
        if (at < value.size() && (value[at] == '+' || value[at] == '-')) negative = value[at++] == '-';
        if (at == value.size() || !std::isdigit(static_cast<unsigned char>(value[at]))) return fail(0, einval);
        std::uint64_t magnitude = 0;
        bool overflow = false;
        for (; at < value.size() && std::isdigit(static_cast<unsigned char>(value[at])); ++at) {
            const auto digit = static_cast<std::uint64_t>(value[at] - '0');
            if (magnitude > (UINT64_MAX - digit) / 10) overflow = true; else magnitude = magnitude * 10 + digit;
        }
        if (at != value.size()) return fail(0, einval);
        constexpr std::uint64_t smallest = 1ULL << 63;   // |INT64_MIN|
        if (overflow || magnitude > (negative ? smallest : smallest - 1)) return fail(negative ? 1 : 2, erange);
        const auto signed_value = negative ? (magnitude == smallest ? INT64_MIN : -static_cast<std::int64_t>(magnitude))
                                           : static_cast<std::int64_t>(magnitude);
        if (signed_value < minimum) return fail(1, erange);
        if (signed_value > maximum) return fail(2, erange);
        if (s.x[3]) m.write64(s.x[3], 0);
        return static_cast<std::uint64_t>(signed_value);
    }, 0);
    // reallocarray(ptr, nmemb, size) / reallocarrayf (frees ptr on failure): ENOMEM on overflow.
    for (const bool frees : {false, true})
        install(state, frees ? "_reallocarrayf" : "_reallocarray", [state, frees](auto& s, auto&) -> std::uint64_t {
            if (s.x[2] && s.x[1] > UINT64_MAX / s.x[2]) {
                if (frees && s.x[0]) static_cast<void>(state->call("_free", {s.x[0]}));
                throw LibcError{enomem};
            }
            const auto result = state->call(frees ? "_reallocf" : "_realloc", {s.x[0], s.x[1] * s.x[2]});
            if (!result) throw LibcError{enomem};
            return result;
        }, 0);
    // int getsubopt(char** optionp, char* const* tokens, char** valuep)
    install(state, "_getsubopt", [](auto& s, auto& m) -> std::uint64_t {
        m.write64(s.x[2], 0);
        auto cursor = m.read64(s.x[0]);
        if (!cursor || !m.read8(cursor)) return UINT64_MAX;
        while (m.read8(cursor) == ',' || std::isspace(m.read8(cursor))) ++cursor;
        const auto option = cursor;
        std::uint64_t equals{};
        for (;; ++cursor) {
            const auto c = m.read8(cursor);
            if (c == '=' && !equals) { equals = cursor; continue; }
            if (c == ',' || !c) {
                if (c) { m.write8(cursor, 0); ++cursor; }
                break;
            }
        }
        m.write64(s.x[0], cursor);
        if (equals) { m.write8(equals, 0); m.write64(s.x[2], equals + 1); }
        const auto name = text(m, option);
        for (std::uint64_t index = 0;; ++index) {
            const auto token = m.read64(s.x[1] + index * 8);
            if (!token) break;
            if (text(m, token) == name) return index;
        }
        m.write64(s.x[2], option);   // the whole unknown suboption, as BSD reports it
        return UINT64_MAX;
    });
    // strtoq / strtouq are strtoll / strtoull.
    for (const auto& [alias, target] : std::array<std::pair<const char*, const char*>, 2>{{{"_strtoq", "_strtoll"}, {"_strtouq", "_strtoull"}}})
        if (const auto found = context.resolve(system_library, target)) context.install_export(std::string(system_library), alias, *found);
    // at_quick_exit(func) / quick_exit(status): the functions in reverse order, then _Exit.
    install(state, "_at_quick_exit", [state](auto& s, auto&) -> std::uint64_t {
        std::lock_guard lock(state->mutex);
        if (state->quick_exit_functions.size() >= 32) return 1;
        state->quick_exit_functions.push_back(s.x[0]);
        return 0;
    });
    install(state, "_quick_exit", [state](auto& s, auto&) -> std::uint64_t {
        std::vector<std::uint64_t> functions;
        { std::lock_guard lock(state->mutex); functions = state->quick_exit_functions; }
        for (auto function = functions.rbegin(); function != functions.rend(); ++function) static_cast<void>(state->invoke(*function, {}));
        return state->call("__Exit", {s.x[0]});
    });
    // long a64l(const char*) / char* l64a(long): radix-64 with "./0-9A-Za-z", up to six digits.
    static constexpr std::string_view digits = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    install(state, "_a64l", [](auto& s, auto& m) {
        std::uint64_t value = 0;
        for (unsigned index = 0; index < 6; ++index) {
            const auto c = static_cast<char>(m.read8(add(s.x[0], index)));
            const auto digit = digits.find(c);
            if (!c || digit == std::string_view::npos) break;
            value |= static_cast<std::uint64_t>(digit) << (6 * index);
        }
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(value))));
    });
    const auto l64a_buffer = context.allocate_guest(8);
    install(state, "_l64a", [l64a_buffer](auto& s, auto& m) {
        auto value = static_cast<std::uint32_t>(s.x[0]);
        std::string result;
        while (value) { result.push_back(digits[value & 63]); value >>= 6; }
        put(m, l64a_buffer, result);
        return l64a_buffer;
    }, 0);
    // heapsort / mergesort (stable) / their _b forms, and bsearch_b; radixsort / sradixsort sort
    // strings by bytes mapped through `table` (NULL: the bytes themselves) up to `endbyte`.
    const auto element_sort = [state](bool stable, bool block) {
        return [state, stable, block](auto& s, auto& m) -> std::uint64_t {
            const auto base = s.x[0], count = s.x[1], width = s.x[2], comparator = s.x[3];
            if (!width) throw LibcError{einval};
            if (count < 2) return 0;
            if (count > (1ULL << 26) / std::max<std::uint64_t>(width, 1) && count * width > (1ULL << 30)) throw LibcError{enomem};
            if (!stable) { static_cast<void>(state->call(block ? "_qsort_b" : "_qsort", {base, count, width, comparator})); return 0; }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count * width));
            m.read_bytes(base, bytes);
            // Merge sort over element indices, comparing the elements in place in guest memory.
            std::vector<std::uint64_t> order(static_cast<std::size_t>(count));
            for (std::uint64_t index = 0; index < count; ++index) order[static_cast<std::size_t>(index)] = index;
            const auto function = block ? m.read64(comparator + 16) : comparator;
            const auto less = [&](std::uint64_t left, std::uint64_t right) {
                const auto a = base + left * width, b = base + right * width;
                return compared(block ? state->invoke(function, {comparator, a, b}) : state->invoke(function, {a, b})) < 0;
            };
            std::stable_sort(order.begin(), order.end(), less);
            std::vector<std::uint8_t> sorted(bytes.size());
            for (std::size_t index = 0; index < order.size(); ++index)
                std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(order[index] * width), width, sorted.begin() + static_cast<std::ptrdiff_t>(index * width));
            m.write_bytes(base, sorted);
            return 0;
        };
    };
    install(state, "_heapsort", element_sort(false, false));
    install(state, "_heapsort_b", element_sort(false, true));
    install(state, "_mergesort", element_sort(true, false));
    install(state, "_mergesort_b", element_sort(true, true));
    install(state, "_bsearch_b", [state](auto& s, auto& m) -> std::uint64_t {
        const auto key = s.x[0], base = s.x[1], count = s.x[2], width = s.x[3], block = s.x[4];
        const auto function = m.read64(block + 16);
        std::uint64_t low = 0, high = count;
        while (low < high) {
            const auto middle = low + (high - low) / 2, address = base + middle * width;
            const auto order = compared(state->invoke(function, {block, key, address}));
            if (!order) return address;
            if (order < 0) high = middle; else low = middle + 1;
        }
        return 0;
    }, 0);
    for (const bool stable : {false, true})
        install(state, stable ? "_sradixsort" : "_radixsort", [](auto& s, auto& m) -> std::uint64_t {
            const auto base = s.x[0], table = s.x[2];
            const auto count = static_cast<std::uint32_t>(s.x[1]);
            const auto end = static_cast<std::uint8_t>(s.x[3]);
            std::array<std::uint8_t, 256> map{};
            for (unsigned c = 0; c < 256; ++c) map[c] = table ? m.read8(table + c) : static_cast<std::uint8_t>(c);
            std::vector<std::pair<std::string, std::uint64_t>> keys;
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto pointer = m.read64(base + index * 8ULL);
                std::string mapped;
                for (std::uint64_t at = 0; at < string_limit; ++at) {
                    const auto c = m.read8(add(pointer, at));
                    if (c == end) break;
                    mapped.push_back(static_cast<char>(map[c]));
                }
                keys.emplace_back(std::move(mapped), pointer);
            }
            std::stable_sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) {
                return std::lexicographical_compare(a.first.begin(), a.first.end(), b.first.begin(), b.first.end(),
                    [](char x, char y) { return static_cast<std::uint8_t>(x) < static_cast<std::uint8_t>(y); });
            });
            for (std::uint32_t index = 0; index < count; ++index) m.write64(base + index * 8ULL, keys[index].second);
            return 0;
        });
    // mkostemp(template, oflags) / mkostemps(template, suffixlen, oflags): mkstemp(s), then the
    // extra open flags (O_APPEND 0x8, O_SHLOCK/O_EXLOCK, O_CLOEXEC 0x1000000) on the descriptor.
    const auto with_flags = [state](std::uint64_t descriptor, std::uint64_t flags) -> std::uint64_t {
        if (static_cast<std::int32_t>(descriptor) < 0) throw LibcError{state->guest_errno()};
        if (flags & 0x1000000) static_cast<void>(state->call("_fcntl", {descriptor, 2, 1}));          // F_SETFD FD_CLOEXEC
        if (flags & 0x8) static_cast<void>(state->call("_fcntl", {descriptor, 4, 0x8}));              // F_SETFL O_APPEND
        return descriptor;
    };
    install(state, "_mkostemp", [state, with_flags](auto& s, auto&) {
        if (s.x[1] & ~(0x8ULL | 0x10ULL | 0x20ULL | 0x1000000ULL)) throw LibcError{einval};
        return with_flags(state->call("_mkstemp", {s.x[0]}), s.x[1]);
    });
    install(state, "_mkostemps", [state, with_flags](auto& s, auto&) {
        if (s.x[2] & ~(0x8ULL | 0x10ULL | 0x20ULL | 0x1000000ULL)) throw LibcError{einval};
        return with_flags(state->call("_mkstemps", {s.x[0], s.x[1]}), s.x[2]);
    });
    // char* mkdtemps(char* template, int suffixlen): the X's before the suffix replaced until
    // mkdir(0700) succeeds.
    install(state, "_mkdtemps", [state](auto& s, auto& m) -> std::uint64_t {
        const auto pattern = text(m, s.x[0], 1024);
        const auto suffix = static_cast<std::size_t>(static_cast<std::int32_t>(s.x[1]));
        if (suffix > pattern.size()) throw LibcError{einval};
        auto end = pattern.size() - suffix, start = end;
        while (start > 0 && pattern[start - 1] == 'X') --start;
        if (start == end) throw LibcError{einval};
        std::random_device source;
        static constexpr std::string_view alphabet = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            auto candidate = pattern;
            for (auto at = start; at < end; ++at) candidate[at] = alphabet[source() % alphabet.size()];
            put(m, s.x[0], candidate);
            if (static_cast<std::int32_t>(state->call("_mkdir", {s.x[0], 0700})) == 0) return s.x[0];
            if (state->guest_errno() != eexist) throw LibcError{state->guest_errno()};
        }
        throw LibcError{eexist};
    }, 0);
}

void install_time_and_ctype(const std::shared_ptr<State>& state) {
    auto& context = state->context;
    // timelocal is mktime; posix2time / time2posix are the identity (no leap-second table).
    if (const auto found = context.resolve(system_library, "_mktime")) context.install_export(std::string(system_library), "_timelocal", *found);
    for (const auto name : {"_posix2time", "_time2posix"}) install(state, name, [](auto& s, auto&) { return s.x[0]; });
    // int timespec_get(struct timespec*, int base): TIME_UTC (1) only.
    install(state, "_timespec_get", [state](auto& s, auto&) -> std::uint64_t {
        if (static_cast<std::int32_t>(s.x[1]) != 1) return 0;
        if (static_cast<std::int32_t>(state->call("_clock_gettime", {0, s.x[0]})) != 0) return 0;
        return 1;
    }, 0);
    // Setting the clock needs root: EPERM, as for an unprivileged process on Darwin.
    for (const auto name : {"_settimeofday", "_clock_settime", "_adjtime"})
        install(state, name, [](auto&, auto&) -> std::uint64_t { throw LibcError{1}; });
    // <ctype.h> BSD extensions in the C locale.
    install(state, "_digittoint", [](auto& s, auto&) -> std::uint64_t {
        const auto c = static_cast<std::uint32_t>(s.x[0]);
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    });
    install(state, "_ishexnumber", [](auto& s, auto&) -> std::uint64_t { const auto c = static_cast<std::uint32_t>(s.x[0]); return c < 128 && std::isxdigit(static_cast<int>(c)) ? 1 : 0; });
    install(state, "_isnumber", [](auto& s, auto&) -> std::uint64_t { const auto c = static_cast<std::uint32_t>(s.x[0]); return c >= '0' && c <= '9' ? 1 : 0; });
    for (const auto name : {"_isideogram", "_isphonogram", "_isspecial"}) install(state, name, [](auto&, auto&) -> std::uint64_t { return 0; });
    install(state, "_isrune", [](auto& s, auto&) -> std::uint64_t { return static_cast<std::uint32_t>(s.x[0]) < 128 ? 1 : 0; });
}

void install_signals(const std::shared_ptr<State>& state) {
    auto& context = state->context;
    // sys_siglist / sys_signame: char* [32] (NSIG), the descriptions and the lower-case names.
    static constexpr std::array<std::string_view, 32> names{"Signal 0", "hup", "int", "quit", "ill", "trap", "abrt", "emt", "fpe", "kill", "bus", "segv",
        "sys", "pipe", "alrm", "term", "urg", "stop", "tstp", "cont", "chld", "ttin", "ttou", "io", "xcpu", "xfsz", "vtalrm", "prof", "winch",
        "info", "usr1", "usr2"};
    static constexpr std::array<std::string_view, 32> descriptions{"Signal 0", "Hangup", "Interrupt", "Quit", "Illegal instruction", "Trace/BPT trap",
        "Abort trap", "EMT trap", "Floating point exception", "Killed", "Bus error", "Segmentation fault", "Bad system call", "Broken pipe",
        "Alarm clock", "Terminated", "Urgent I/O condition", "Suspended (signal)", "Suspended", "Continued", "Child exited", "Stopped (tty input)",
        "Stopped (tty output)", "I/O possible", "Cputime limit exceeded", "Filesize limit exceeded", "Virtual timer expired",
        "Profiling timer expired", "Window size changes", "Information request", "User defined signal 1", "User defined signal 2"};
    auto& memory = context.calling_engine();
    for (const auto& [symbol, table] : std::array<std::pair<const char*, const std::array<std::string_view, 32>*>, 2>{{{"_sys_signame", &names}, {"_sys_siglist", &descriptions}}}) {
        if (context.resolve(system_library, symbol)) continue;
        const auto array = context.allocate_guest(32 * 8);
        for (std::size_t index = 0; index < 32; ++index) {
            const auto entry = context.allocate_guest((*table)[index].size() + 1);
            put(memory, entry, (*table)[index]);
            memory.write64(array + index * 8, entry);
        }
        context.install_export(std::string(system_library), symbol, {array, ExportKind::data});
    }
    // void psignal(unsigned sig, const char* s): "<s>: <description>\n" on standard error.
    const auto siglist = context.resolve(system_library, "_sys_siglist");
    install(state, "_psignal", [state, siglist](auto& s, auto& m) {
        const auto signal = static_cast<std::uint32_t>(s.x[0]);
        std::string line = s.x[1] ? text(m, s.x[1]) : std::string();
        if (!line.empty()) line += ": ";
        line += signal < 32 && siglist ? text(m, m.read64(siglist->address + signal * 8)) : "Unknown signal";
        line += '\n';
        const auto buffer = state->malloc(line.size());
        put(m, buffer, line, false);
        static_cast<void>(state->call("_write", {2, buffer, line.size()}));
        static_cast<void>(state->call("_free", {buffer}));
        return std::uint64_t{0};
    });
    // The BSD and System V calls over sigaction / sigprocmask. struct sigaction (user):
    // {handler @0, mask @8 (sigset_t, 4 bytes), flags @12}; SA_RESTART 0x2.
    const auto mask_change = [state](int how, std::uint32_t mask) -> std::uint64_t {
        const auto sets = state->malloc(8);
        auto& m = state->context.calling_engine();
        m.write32(sets, mask);
        const auto result = static_cast<std::int32_t>(state->call("_sigprocmask", {static_cast<std::uint64_t>(how), sets, sets + 4}));
        const auto previous = m.read32(sets + 4);
        static_cast<void>(state->call("_free", {sets}));
        if (result != 0) throw LibcError{state->guest_errno()};
        return previous;
    };
    const auto bit = [](std::uint64_t signal) -> std::uint32_t {
        if (!signal || signal >= 32) throw LibcError{einval};
        return 1U << (signal - 1);
    };
    // sigblock(mask) / sigsetmask(mask): the previous mask.
    install(state, "_sigblock", [mask_change](auto& s, auto&) { return mask_change(1, static_cast<std::uint32_t>(s.x[0])); });   // SIG_BLOCK
    install(state, "_sigsetmask", [mask_change](auto& s, auto&) { return mask_change(3, static_cast<std::uint32_t>(s.x[0])); });   // SIG_SETMASK
    install(state, "_sighold", [mask_change, bit](auto& s, auto&) { static_cast<void>(mask_change(1, bit(s.x[0]))); return std::uint64_t{0}; });
    install(state, "_sigrelse", [mask_change, bit](auto& s, auto&) { static_cast<void>(mask_change(2, bit(s.x[0]))); return std::uint64_t{0}; });   // SIG_UNBLOCK
    // signal-like setters: handler with SA_RESTART (bsd_signal), sigignore (SIG_IGN), sigset
    // (SIG_HOLD 5 blocks the signal; otherwise sets the handler and unblocks it).
    const auto set_handler = [state](std::uint64_t signal, std::uint64_t handler, std::uint32_t flags) -> std::uint64_t {
        const auto actions = state->malloc(32);
        auto& m = state->context.calling_engine();
        m.write64(actions, handler); m.write32(actions + 8, 0); m.write32(actions + 12, flags);
        const auto result = static_cast<std::int32_t>(state->call("_sigaction", {signal, actions, actions + 16}));
        const auto previous = m.read64(actions + 16);
        static_cast<void>(state->call("_free", {actions}));
        if (result != 0) throw LibcError{state->guest_errno()};
        return previous;
    };
    install(state, "_bsd_signal", [set_handler](auto& s, auto&) { return set_handler(s.x[0], s.x[1], 0x2); });
    install(state, "_sigignore", [set_handler](auto& s, auto&) { static_cast<void>(set_handler(s.x[0], 1, 0)); return std::uint64_t{0}; });
    install(state, "_sigset", [set_handler, mask_change, bit](auto& s, auto&) -> std::uint64_t {
        const auto mask = bit(s.x[0]);
        if (s.x[1] == 5) {   // SIG_HOLD
            const auto previous = mask_change(1, mask);
            return (previous & mask) ? 5 : 0;
        }
        const auto previous = set_handler(s.x[0], s.x[1], 0);
        const auto held = mask_change(2, mask) & mask;
        return held ? 5 : previous;
    });
    // int siginterrupt(sig, flag): flag clears SA_RESTART (interrupted calls fail with EINTR).
    install(state, "_siginterrupt", [state](auto& s, auto&) {
        const auto actions = state->malloc(32);
        auto& m = state->context.calling_engine();
        if (static_cast<std::int32_t>(state->call("_sigaction", {s.x[0], 0, actions})) != 0) {
            const auto error = state->guest_errno();
            static_cast<void>(state->call("_free", {actions}));
            throw LibcError{error};
        }
        auto flags = m.read32(actions + 12);
        flags = static_cast<std::uint32_t>(s.x[1]) ? flags & ~0x2U : flags | 0x2U;
        m.write32(actions + 12, flags);
        const auto result = static_cast<std::int32_t>(state->call("_sigaction", {s.x[0], actions, 0}));
        static_cast<void>(state->call("_free", {actions}));
        if (result != 0) throw LibcError{state->guest_errno()};
        return std::uint64_t{0};
    });
}

void install_io_and_paths(const std::shared_ptr<State>& state) {
    auto& context = state->context;
    // dprintf(fd, format, ...) / vdprintf(fd, format, va_list): vsnprintf, then write.
    const auto print = [state](std::uint64_t fd, std::uint64_t format, std::uint64_t arguments) -> std::uint64_t {
        const auto length = state->call("_vsnprintf", {0, 0, format, arguments});
        if (static_cast<std::int32_t>(length) < 0) throw LibcError{state->guest_errno()};
        const auto buffer = state->malloc(length + 1);
        static_cast<void>(state->call("_vsnprintf", {buffer, length + 1, format, arguments}));
        std::uint64_t written = 0;
        while (written < length) {
            const auto count = static_cast<std::int64_t>(state->call("_write", {fd, buffer + written, length - written}));
            if (count <= 0) { const auto error = state->guest_errno(); static_cast<void>(state->call("_free", {buffer})); throw LibcError{error}; }
            written += static_cast<std::uint64_t>(count);
        }
        static_cast<void>(state->call("_free", {buffer}));
        return length;
    };
    install(state, "_dprintf", [print](auto& s, auto&) { return print(s.x[0], s.x[1], s.sp); });   // variadic arguments: the stack
    install(state, "_vdprintf", [print](auto& s, auto&) { return print(s.x[0], s.x[1], s.x[2]); });
    // The _unlocked stdio forms behave as the locked ones here (each stream call is atomic).
    for (const auto& [alias, target] : std::array<std::pair<const char*, const char*>, 9>{{
             {"_getc_unlocked", "_getc"}, {"_putc_unlocked", "_putc"}, {"_getchar_unlocked", "_getchar"}, {"_putchar_unlocked", "_putchar"},
             {"_feof_unlocked", "_feof"}, {"_ferror_unlocked", "_ferror"}, {"_clearerr_unlocked", "_clearerr"},
             {"_fileno_unlocked", "_fileno"}, {"_fputs_unlocked", "_fputs"}}})
        if (!context.resolve(system_library, alias))
            if (const auto found = context.resolve(system_library, target)) context.install_export(std::string(system_library), alias, *found);
    // fdatasync is fsync (file data and metadata are flushed together).
    if (const auto found = context.resolve(system_library, "_fsync")) context.install_export(std::string(system_library), "_fdatasync", *found);
    // dirname_r / basename_r(path, buffer of MAXPATHLEN): the POSIX results in the caller's buffer.
    const auto split = [](std::string path, bool directory) -> std::string {
        if (path.empty()) return ".";
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        const auto slash = path.rfind('/');
        if (!directory) return slash == std::string::npos || path == "/" ? path : path.substr(slash + 1);
        if (slash == std::string::npos) return ".";
        auto head = path.substr(0, slash);
        while (head.size() > 1 && head.back() == '/') head.pop_back();
        return head.empty() ? "/" : head;
    };
    for (const bool directory : {false, true})
        install(state, directory ? "_dirname_r" : "_basename_r", [split, directory](auto& s, auto& m) -> std::uint64_t {
            const auto result = split(s.x[0] ? text(m, s.x[0], 1024) : std::string(), directory);
            if (result.size() >= 1024) throw LibcError{63};   // ENAMETOOLONG
            if (!s.x[1]) throw LibcError{einval};
            put(m, s.x[1], result);
            return s.x[1];
        }, 0);
    // int mkpath_np(const char* path, mode_t mode): mkdir -p; EEXIST when the path already is a directory.
    install(state, "_mkpath_np", [state](auto& s, auto& m) -> std::uint64_t {
        const auto path = text(m, s.x[0], 1024);
        const auto buffer = state->malloc(path.size() + 1);
        // The path and each parent, deepest first: climb while the parent is missing, then make
        // the levels back down. EEXIST only when the path itself exists.
        std::vector<std::size_t> ends{path.size()};
        for (auto at = path.size(); at > 1;) {
            at = path.rfind('/', at - 1);
            if (at == std::string::npos || at == 0) break;
            if (at != ends.back()) ends.push_back(at);
        }
        const auto make = [&](std::size_t end) -> std::uint32_t {
            put(m, buffer, path.substr(0, end));
            return static_cast<std::int32_t>(state->call("_mkdir", {buffer, s.x[1]})) == 0 ? 0 : state->guest_errno();
        };
        std::uint64_t result = 0;
        std::size_t level = 0;
        for (;; ++level) {
            const auto error = make(ends[level]);
            if (error == enoent && level + 1 < ends.size()) continue;
            if (error && !(error == eexist && level)) result = error;
            break;
        }
        while (!result && level-- > 0)
            if (const auto error = make(ends[level])) result = error;
        static_cast<void>(state->call("_free", {buffer}));
        return result;
    });
    // ether_ntoa / ether_aton: "x:x:x:x:x:x" (lower-case hex, no leading zeros) and back.
    const auto ether_buffer = context.allocate_guest(24), ether_address = context.allocate_guest(8);
    install(state, "_ether_ntoa", [ether_buffer](auto& s, auto& m) {
        std::string value;
        for (unsigned index = 0; index < 6; ++index) {
            static constexpr std::string_view hex = "0123456789abcdef";
            const auto byte = m.read8(s.x[0] + index);
            if (index) value.push_back(':');
            if (byte >> 4) value.push_back(hex[byte >> 4]);
            value.push_back(hex[byte & 15]);
        }
        put(m, ether_buffer, value);
        return ether_buffer;
    }, 0);
    install(state, "_ether_aton", [ether_address](auto& s, auto& m) -> std::uint64_t {
        const auto value = text(m, s.x[0], 64);
        std::array<std::uint8_t, 6> bytes{};
        std::size_t at = 0;
        for (unsigned index = 0; index < 6; ++index) {
            unsigned part = 0, digits = 0;
            while (at < value.size() && std::isxdigit(static_cast<unsigned char>(value[at])) && digits < 2) {
                part = part * 16 + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(value[at])) ? value[at] - '0' : std::tolower(value[at]) - 'a' + 10);
                ++at; ++digits;
            }
            if (!digits) return 0;
            bytes[index] = static_cast<std::uint8_t>(part);
            if (index < 5) { if (at >= value.size() || value[at] != ':') return 0; ++at; }
        }
        if (at != value.size()) return 0;
        m.write_bytes(ether_address, bytes);
        return ether_address;
    }, 0);
    // ftw(path, fn(path, struct stat*, flag), depth) / nftw(path, fn(path, stat*, flag, struct FTW*),
    // depth, flags): a walk over opendir/readdir/lstat (stat with FTW_PHYS unset follows links),
    // preorder unless FTW_DEPTH (nftw); FTW_F 0, FTW_D 1, FTW_DNR 2, FTW_NS 3, FTW_SL 4, FTW_DP 5,
    // FTW_SLN 6; a callback's nonzero result stops the walk and is returned.
    for (const bool extended : {false, true})
        install(state, extended ? "_nftw" : "_ftw", [state, extended](auto& s, auto& m) -> std::uint64_t {
            const auto flags = extended ? static_cast<std::uint32_t>(s.x[3]) : 0U;
            const bool physical = extended && (flags & 1), depth_first = extended && (flags & 8);   // FTW_PHYS, FTW_DEPTH (ftw follows links)
            const auto info = state->malloc(144), path_buffer = state->malloc(1024), level = state->malloc(8);
            const auto release = [&] { for (const auto block : {info, path_buffer, level}) static_cast<void>(state->call("_free", {block})); };
            std::function<std::uint64_t(const std::string&, std::uint32_t)> visit = [&](const std::string& path, std::uint32_t depth) -> std::uint64_t {
                if (path.size() >= 1024) throw LibcError{63};
                put(m, path_buffer, path);
                const bool stat_ok = static_cast<std::int32_t>(state->call(physical ? "_lstat" : "_stat", {path_buffer, info})) == 0;
                const auto mode = stat_ok ? (static_cast<std::uint32_t>(m.read8(info + 4)) | static_cast<std::uint32_t>(m.read8(info + 5)) << 8) & 0170000 : 0;
                const auto base = path.rfind('/');
                m.write32(level, static_cast<std::uint32_t>(base == std::string::npos ? 0 : base + 1)); m.write32(level + 4, depth);
                const auto call_back = [&](std::uint32_t type) {
                    put(m, path_buffer, path);
                    return extended ? state->invoke(s.x[1], {path_buffer, info, type, level}) : state->invoke(s.x[1], {path_buffer, info, type});
                };
                if (!stat_ok) return call_back(3) & 0xffffffff;
                if (mode == 0120000) return call_back(4) & 0xffffffff;
                if (mode != 0040000) return call_back(0) & 0xffffffff;
                if (!depth_first) if (const auto result = call_back(1) & 0xffffffff) return result;
                put(m, path_buffer, path);
                const auto directory = state->call("_opendir", {path_buffer});
                if (!directory) {
                    if (depth_first) return call_back(2) & 0xffffffff;
                    return 0;
                }
                std::vector<std::string> names;
                for (;;) {
                    const auto entry = state->call("_readdir", {directory});
                    if (!entry) break;
                    const auto name = text(m, entry + 21, 1024);   // struct dirent (64-bit inodes): d_name at 21
                    if (name != "." && name != "..") names.push_back(name);
                }
                static_cast<void>(state->call("_closedir", {directory}));
                for (const auto& name : names)
                    if (const auto result = visit(path + (path.ends_with('/') ? "" : "/") + name, depth + 1)) return result;
                if (depth_first) {
                    put(m, path_buffer, path);
                    static_cast<void>(state->call(physical ? "_lstat" : "_stat", {path_buffer, info}));
                    return call_back(5) & 0xffffffff;
                }
                return 0;
            };
            try {
                const auto result = visit(text(m, s.x[0], 1024), 0);
                release();
                return result;
            } catch (...) { release(); throw; }
        });
}

} // namespace

void install_darwin_libc_extra(ProcessContext& context) {
    const auto state = std::make_shared<State>(context);
    install_strings(state);
    install_wide(state);
    install_search(state);
    install_stdlib(state);
    install_time_and_ctype(state);
    install_signals(state);
    install_io_and_paths(state);
}

} // namespace alchemy
