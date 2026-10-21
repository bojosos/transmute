#define _CRT_SECURE_NO_WARNINGS
#include "alchemy/darwin_libsystem_extra.hpp"
#include "alchemy/host/system.hpp"
#include "alchemy/host/time.hpp"
#include "alchemy/darwin/libsystem/mach/task_shell.hpp"
#include "alchemy/darwin/libsystem/misc/process_usage.hpp"
#include "alchemy/darwin_version.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace alchemy {
namespace {
constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";
constexpr std::string_view objc_library = "/usr/lib/libobjc.A.dylib";
constexpr std::uint32_t enoent = 2, esrch = 3, eio = 5, echild = 10, enomem = 12, efault = 14,
    einval = 22, erange = 34, enosys = 78;
constexpr std::uint64_t string_limit = 1024 * 1024;
constexpr std::uint64_t page_size = 16384;
struct ExtraError { std::uint32_t value; };

std::uint64_t add(std::uint64_t address, std::uint64_t amount) {
    if (amount > UINT64_MAX - address) throw ExtraError{efault};
    return address + amount;
}
bool accessible(const transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size, bool writable) {
    if (!address || size > UINT64_MAX - address) return false;
    while (size) {
        const auto region = memory.query_region(address);
        if (!region || address < region->base || address - region->base >= region->size ||
            !region->readable || (writable && !region->writable)) return false;
        const auto count = std::min(size, region->size - (address - region->base));
        address += count; size -= count;
    }
    return true;
}
void validate(const transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size, bool writable) {
    if (!accessible(memory, address, size, writable)) throw ExtraError{efault};
}
std::string text(const transmute::GuestMemory& memory, std::uint64_t address) {
    if (!address) throw ExtraError{efault};
    bool terminated{};
    const auto bytes = memory.read_string(address, 1, string_limit, &terminated);
    if (!terminated) throw std::runtime_error("Darwin libSystem string exceeds 1 MiB");
    return {bytes.begin(), bytes.end()};
}
void put_text(transmute::GuestMemory& memory, std::uint64_t address, std::string_view value) {
    memory.write_bytes(address, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    memory.write8(add(address, value.size()), 0);
}
std::uint16_t read16(const transmute::GuestMemory& memory, std::uint64_t address) {
    return static_cast<std::uint16_t>(memory.read8(address) | (memory.read8(address + 1) << 8));
}

std::uint64_t uptime_nanoseconds() { return host::time::monotonic_ns(); }
std::uint64_t wall_nanoseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// ---- POSIX fnmatch, shared by glob -------------------------------------------
constexpr int fnm_noescape = 1, fnm_pathname = 2, fnm_period = 4, fnm_leading_dir = 8, fnm_casefold = 16;
char fold(char value) { return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value; }
bool character_class(std::string_view name, unsigned char value) {
    if (name == "alpha") return std::isalpha(value) != 0;
    if (name == "digit") return std::isdigit(value) != 0;
    if (name == "alnum") return std::isalnum(value) != 0;
    if (name == "upper") return std::isupper(value) != 0;
    if (name == "lower") return std::islower(value) != 0;
    if (name == "space") return std::isspace(value) != 0;
    if (name == "blank") return value == ' ' || value == '\t';
    if (name == "punct") return std::ispunct(value) != 0;
    if (name == "print") return std::isprint(value) != 0;
    if (name == "graph") return std::isgraph(value) != 0;
    if (name == "cntrl") return std::iscntrl(value) != 0;
    if (name == "xdigit") return std::isxdigit(value) != 0;
    return false;
}
// Bracket expression at pattern[at] == '['. Returns the index after ']' and
// whether `value` matched, or nothing when the bracket is unterminated.
std::optional<std::pair<std::size_t, bool>> bracket(std::string_view pattern, std::size_t at, char value, int flags) {
    std::size_t index = at + 1;
    bool negate{}, matched{};
    if (index < pattern.size() && (pattern[index] == '!' || pattern[index] == '^')) { negate = true; ++index; }
    const auto folded = (flags & fnm_casefold) ? fold(value) : value;
    bool first = true;
    while (index < pattern.size() && (first || pattern[index] != ']')) {
        first = false;
        if (pattern[index] == '[' && index + 1 < pattern.size() && pattern[index + 1] == ':') {
            const auto end = pattern.find(":]", index + 2);
            if (end == std::string_view::npos) return std::nullopt;
            if (character_class(pattern.substr(index + 2, end - index - 2), static_cast<unsigned char>(value))) matched = true;
            index = end + 2; continue;
        }
        char low = pattern[index++];
        if (low == '\\' && !(flags & fnm_noescape) && index < pattern.size()) low = pattern[index++];
        char high = low;
        if (index + 1 < pattern.size() && pattern[index] == '-' && pattern[index + 1] != ']') {
            high = pattern[index + 1]; index += 2;
            if (high == '\\' && !(flags & fnm_noescape) && index < pattern.size()) high = pattern[index++];
        }
        if (flags & fnm_casefold) { low = fold(low); high = fold(high); }
        if (static_cast<unsigned char>(folded) >= static_cast<unsigned char>(low) &&
            static_cast<unsigned char>(folded) <= static_cast<unsigned char>(high)) matched = true;
    }
    if (index >= pattern.size()) return std::nullopt;
    return std::pair{index + 1, matched != negate};
}
bool fnmatch_at(std::string_view pattern, std::size_t p, std::string_view value, std::size_t s, int flags, unsigned depth) {
    if (depth > 4096) throw std::runtime_error("Darwin fnmatch pattern recursion limit");
    const auto leading = [&](std::size_t at) { return at == 0 || ((flags & fnm_pathname) && value[at - 1] == '/'); };
    while (p < pattern.size()) {
        const auto c = pattern[p];
        if (c == '?') {
            if (s == value.size() || ((flags & fnm_pathname) && value[s] == '/') ||
                ((flags & fnm_period) && value[s] == '.' && leading(s))) return false;
            ++p; ++s; continue;
        }
        if (c == '*') {
            while (p < pattern.size() && pattern[p] == '*') ++p;
            if ((flags & fnm_period) && s < value.size() && value[s] == '.' && leading(s)) return false;
            if (p == pattern.size()) {
                if (!(flags & fnm_pathname) || (flags & fnm_leading_dir)) return true;
                return value.find('/', s) == std::string_view::npos;
            }
            for (auto at = s; at <= value.size(); ++at) {
                if (fnmatch_at(pattern, p, value, at, flags, depth + 1)) return true;
                if (at < value.size() && (flags & fnm_pathname) && value[at] == '/') break;
            }
            return false;
        }
        if (c == '[') {
            if (s == value.size() || ((flags & fnm_pathname) && value[s] == '/') ||
                ((flags & fnm_period) && value[s] == '.' && leading(s))) return false;
            if (const auto result = bracket(pattern, p, value[s], flags)) {
                if (!result->second) return false;
                p = result->first; ++s; continue;
            }
        }
        auto literal = c;
        if (c == '\\' && !(flags & fnm_noescape) && p + 1 < pattern.size()) literal = pattern[++p];
        if (s == value.size()) return false;
        if ((flags & fnm_casefold) ? fold(literal) != fold(value[s]) : literal != value[s]) return false;
        ++p; ++s;
    }
    return s == value.size() || ((flags & fnm_leading_dir) && value[s] == '/');
}
bool fnmatch(std::string_view pattern, std::string_view value, int flags) { return fnmatch_at(pattern, 0, value, 0, flags, 0); }
bool has_magic(std::string_view pattern, bool noescape) {
    for (std::size_t at = 0; at < pattern.size(); ++at) {
        if (pattern[at] == '\\' && !noescape) { ++at; continue; }
        if (pattern[at] == '*' || pattern[at] == '?' || pattern[at] == '[') return true;
    }
    return false;
}
std::string unescape(std::string_view pattern, bool noescape) {
    std::string value;
    for (std::size_t at = 0; at < pattern.size(); ++at) {
        if (pattern[at] == '\\' && !noescape && at + 1 < pattern.size()) ++at;
        value += pattern[at];
    }
    return value;
}
// GLOB_BRACE: {a,b} alternatives, nested, escaped braces stay literal.
void expand_braces(const std::string& pattern, std::vector<std::string>& output, bool noescape, unsigned depth = 0) {
    if (depth > 64 || output.size() > 65536) throw ExtraError{enomem};
    std::size_t open = std::string::npos;
    for (std::size_t at = 0; at < pattern.size(); ++at) {
        if (pattern[at] == '\\' && !noescape) { ++at; continue; }
        if (pattern[at] == '{') { open = at; break; }
    }
    if (open == std::string::npos) { output.push_back(pattern); return; }
    std::vector<std::string> alternatives;
    std::size_t nesting = 0, start = open + 1, close = std::string::npos;
    for (std::size_t at = open + 1; at < pattern.size(); ++at) {
        if (pattern[at] == '\\' && !noescape) { ++at; continue; }
        if (pattern[at] == '{') ++nesting;
        else if (pattern[at] == '}') { if (!nesting) { close = at; break; } --nesting; }
        else if (pattern[at] == ',' && !nesting) { alternatives.push_back(pattern.substr(start, at - start)); start = at + 1; }
    }
    if (close == std::string::npos) { output.push_back(pattern); return; }
    alternatives.push_back(pattern.substr(start, close - start));
    if (alternatives.size() == 1 && alternatives[0].empty()) { output.push_back(pattern); return; } // "{}" is literal
    for (const auto& alternative : alternatives)
        expand_braces(pattern.substr(0, open) + alternative + pattern.substr(close + 1), output, noescape, depth + 1);
}

// ---- POSIX regex over std::regex's POSIX grammars ----------------------------
constexpr std::uint32_t reg_extended = 1, reg_icase = 2, reg_nosub = 4, reg_newline = 8, reg_nospec = 16, reg_pend = 32;
constexpr std::uint32_t reg_notbol = 1, reg_noteol = 2, reg_startend = 4;
constexpr std::uint32_t reg_nomatch = 1, reg_badpat = 2, reg_ecollate = 3, reg_ectype = 4, reg_eescape = 5,
    reg_esubreg = 6, reg_ebrack = 7, reg_eparen = 8, reg_ebrace = 9, reg_badbr = 10, reg_erange = 11,
    reg_espace = 12, reg_badrpt = 13, reg_invarg = 16;
constexpr std::uint32_t regex_magic = 0xf265;
std::uint32_t posix_regex_code(std::regex_constants::error_type code) {
    using namespace std::regex_constants;
    switch (code) {
    case error_collate: return reg_ecollate;
    case error_ctype: return reg_ectype;
    case error_escape: return reg_eescape;
    case error_backref: return reg_esubreg;
    case error_brack: return reg_ebrack;
    case error_paren: return reg_eparen;
    case error_brace: return reg_ebrace;
    case error_badbrace: return reg_badbr;
    case error_range: return reg_erange;
    case error_space: case error_complexity: case error_stack: return reg_espace;
    case error_badrepeat: return reg_badrpt;
    default: return reg_badpat;
    }
}
const char* regex_message(std::uint32_t code) {
    static constexpr std::array<const char*, 18> messages{
        "success", "regexec() failed to match", "invalid regular expression", "invalid collating element",
        "invalid character class", "trailing backslash (\\)", "invalid backreference number",
        "brackets ([ ]) not balanced", "parentheses not balanced", "braces not balanced",
        "invalid repetition count(s)", "invalid character range", "out of memory",
        "repetition-operator operand invalid", "empty (sub)expression", "cannot happen - you found a bug",
        "invalid argument to regex routine", "illegal byte sequence"};
    return code < messages.size() ? messages[code] : "unknown regexp error";
}
const char* regex_name(std::uint32_t code) {
    static constexpr std::array<const char*, 18> names{"REG_0x0", "REG_NOMATCH", "REG_BADPAT", "REG_ECOLLATE",
        "REG_ECTYPE", "REG_EESCAPE", "REG_ESUBREG", "REG_EBRACK", "REG_EPAREN", "REG_EBRACE", "REG_BADBR",
        "REG_ERANGE", "REG_ESPACE", "REG_BADRPT", "REG_EMPTY", "REG_ASSERT", "REG_INVARG", "REG_ILLSEQ"};
    return code < names.size() ? names[code] : "REG_UNKNOWN";
}

// ---- State --------------------------------------------------------------------
struct State : std::enable_shared_from_this<State> {
    State(ProcessContext& value, SignalMaskAccess access) : context(value), masks(std::move(access)) {}
    ProcessContext& context;
    SignalMaskAccess masks;
    std::mutex mutex;        // general state
    std::mutex block_mutex;  // Blocks reference counts (guest flag words)
    std::uint64_t malloc_block_isa{};
    std::uint64_t context_entry{}, context_return{};
    // Mach semaphores.
    struct Semaphore {
        std::mutex gate;
        std::condition_variable changed;
        std::uint64_t count{}, tickets{};
        unsigned waiters{};
        bool destroyed{};
    };
    std::map<std::uint32_t, std::shared_ptr<Semaphore>> semaphores;
    std::uint32_t next_semaphore{0x5e03};
    // POSIX regex programs, keyed by the regex_t address.
    struct Program { std::regex expression; std::uint32_t flags{}; std::size_t groups{}; };
    std::map<std::uint64_t, std::shared_ptr<Program>> programs;
    std::uint64_t next_program{1};
    // glob results: gl_pathv -> owned string allocations.
    std::map<std::uint64_t, std::vector<std::uint64_t>> globs;
    // syslog.
    std::string log_identity;
    std::uint32_t log_options{}, log_mask{0xff};
    // rand.
    std::uint64_t rand_state{1};
    // ctime/asctime static result, passwd static records.
    std::uint64_t time_buffer{}, password_record{};

    std::optional<std::uint64_t> find(std::string_view library, std::string_view name) const {
        if (const auto value = context.resolve(library, name)) return value->address;
        return std::nullopt;
    }
    std::uint64_t symbol(std::string_view name) const {
        const auto value = find(system_library, name);
        if (!value) throw std::runtime_error("Alchemy libSystem requires " + std::string(name));
        return *value;
    }
    std::uint64_t invoke(std::uint64_t address, std::initializer_list<std::uint64_t> arguments) {
        return EngineAdapter(context.calling_engine()).call(address, std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> arguments) {
        return EngineAdapter(context.calling_engine()).call(symbol(name), std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t guest_malloc(std::uint64_t size) {
        const auto value = call("_malloc", {std::max<std::uint64_t>(size, 1)});
        if (!value) throw ExtraError{enomem};
        return value;
    }
    void guest_free(std::uint64_t address) { if (address) static_cast<void>(call("_free", {address})); }
    std::uint64_t guest_string(std::string_view value) {
        const auto address = context.allocate_guest(value.size() + 1);
        put_text(context.calling_engine(), address, value);
        return address;
    }
    std::uint32_t errno_value() { return context.calling_engine().read32(context.current_thread()->errno_address); }
    std::optional<std::string> environment(std::string_view key) {
        std::lock_guard lock(mutex);
        for (const auto& entry : context.environment)
            if (entry.size() > key.size() && entry.compare(0, key.size(), key) == 0 && entry[key.size()] == '=')
                return entry.substr(key.size() + 1);
        return std::nullopt;
    }

    // ---- Blocks runtime (Clang Blocks ABI) -----------------------------------
    static constexpr std::uint32_t refcount_mask = 0xfffe, deallocating = 1, needs_free = 1U << 24,
        has_copy_dispose = 1U << 25, is_global = 1U << 28, layout_extended = 1U << 28;
    static std::uint32_t increment(std::uint32_t flags) {
        return (flags & refcount_mask) == refcount_mask ? flags : flags + 2;
    }
    void retain_object(std::uint64_t object) {
        // libobjc installs objc_retain/objc_release as the Blocks object hooks.
        if (!object) return;
        if (const auto retain = find(objc_library, "_objc_retain")) static_cast<void>(invoke(*retain, {object}));
    }
    void release_object(std::uint64_t object) {
        if (!object) return;
        if (const auto release = find(objc_library, "_objc_release")) static_cast<void>(invoke(*release, {object}));
    }
    std::uint64_t block_copy(transmute::GuestMemory& m, std::uint64_t block) {
        if (!block) return 0;
        if (!accessible(m, block, 32, false)) throw std::runtime_error("_Block_copy received an invalid block");
        std::uint32_t flags;
        {
            std::lock_guard lock(block_mutex);
            flags = m.read32(block + 8);
            if (flags & needs_free) { m.write32(block + 8, increment(flags)); return block; }
            if (flags & is_global) return block;
        }
        const auto descriptor = m.read64(block + 24);
        if (!accessible(m, descriptor, (flags & has_copy_dispose) ? 32 : 16, false))
            throw std::runtime_error("_Block_copy received a block with an invalid descriptor");
        const auto size = m.read64(descriptor + 8);
        if (size < 32 || size > string_limit) throw std::runtime_error("_Block_copy received an invalid block size");
        const auto copy = context.allocate_guest(size, 16);
        m.copy_bytes(copy, block, size);
        m.write32(copy + 8, (flags & ~(refcount_mask | deallocating)) | needs_free | 2);
        m.write64(copy, malloc_block_isa);
        if (flags & has_copy_dispose) {
            try { static_cast<void>(invoke(m.read64(descriptor + 16), {copy, block})); }
            catch (...) { context.release_guest(copy); throw; }
        }
        return copy;
    }
    void block_release(transmute::GuestMemory& m, std::uint64_t block) {
        if (!block) return;
        if (!accessible(m, block, 32, false)) throw std::runtime_error("_Block_release received an invalid block");
        std::uint32_t flags;
        {
            std::lock_guard lock(block_mutex);
            flags = m.read32(block + 8);
            if ((flags & is_global) || !(flags & needs_free)) return;
            if ((flags & refcount_mask) == refcount_mask) return;
            if (!(flags & refcount_mask)) throw std::runtime_error("Darwin block over-released");
            flags -= 2;
            if (flags & refcount_mask) { m.write32(block + 8, flags); return; }
            m.write32(block + 8, flags | deallocating);
        }
        if (flags & has_copy_dispose) static_cast<void>(invoke(m.read64(m.read64(block + 24) + 24), {block}));
        context.release_guest(block);
    }
    // __block variables: {isa, forwarding, flags, size, [keep, destroy], [layout], payload}.
    std::uint64_t byref_copy(transmute::GuestMemory& m, std::uint64_t source) {
        if (!accessible(m, source, 24, true)) throw std::runtime_error("_Block_object_assign received an invalid __block variable");
        std::uint64_t copy, keep{};
        std::uint32_t flags;
        {
            std::lock_guard lock(block_mutex);
            const auto forwarding = m.read64(source + 8);
            const auto current = m.read32(forwarding + 16);
            if (current & refcount_mask) {
                if (current & needs_free) m.write32(forwarding + 16, increment(current));
                return forwarding;
            }
            flags = m.read32(source + 16);
            const auto size = m.read32(source + 20);
            const auto header = (flags & has_copy_dispose) ? ((flags & layout_extended) ? 48U : 40U) : 24U;
            if (size < header || size > string_limit) throw std::runtime_error("invalid Darwin __block variable size");
            copy = context.allocate_guest(size, 16);
            m.write64(copy, 0);
            m.write64(copy + 8, copy);
            m.write32(copy + 16, flags | needs_free | 4); // one reference for the caller, one for the stack
            m.write32(copy + 20, size);
            m.write64(source + 8, copy);
            if (flags & has_copy_dispose) {
                keep = m.read64(source + 24);
                m.write64(copy + 24, keep);
                m.write64(copy + 32, m.read64(source + 32));
                if (flags & layout_extended) m.write64(copy + 40, m.read64(source + 40));
            } else m.copy_bytes(copy + 24, source + 24, size - 24);
        }
        if (keep) static_cast<void>(invoke(keep, {copy, source}));
        return copy;
    }
    void byref_release(transmute::GuestMemory& m, std::uint64_t object) {
        if (!accessible(m, object, 24, false)) throw std::runtime_error("_Block_object_dispose received an invalid __block variable");
        std::uint64_t byref;
        std::uint32_t flags;
        {
            std::lock_guard lock(block_mutex);
            byref = m.read64(object + 8);
            flags = m.read32(byref + 16);
            if (!(flags & needs_free) || (flags & refcount_mask) == refcount_mask) return;
            if (!(flags & refcount_mask)) throw std::runtime_error("Darwin __block variable over-released");
            flags -= 2;
            m.write32(byref + 16, flags);
            if (flags & refcount_mask) return;
        }
        if (flags & has_copy_dispose) static_cast<void>(invoke(m.read64(byref + 32), {byref}));
        context.release_guest(byref);
    }

    // ---- setjmp/longjmp and ucontext -------------------------------------
    // Darwin arm64 jmp_buf is int[48]; Alchemy's private layout inside it:
    // x19..x28 @0, fp @80, lr @88, sp @96, cookie @104, d8..d15 @112,
    // signal mask @176, mask-saved flag @180.
    static constexpr std::uint64_t jmp_cookie = 0x414c4a4d50425546ULL;
    void save_jump(transmute::CpuState& s, transmute::GuestMemory& m, std::uint64_t buffer, bool mask) {
        validate(m, buffer, 184, true);
        m.write32(buffer + 176, mask ? masks.get() : 0);
        m.write32(buffer + 180, mask ? 1 : 0);
        for (unsigned index = 0; index < 10; ++index) m.write64(buffer + index * 8, s.x[19 + index]);
        m.write64(buffer + 80, s.x[29]); m.write64(buffer + 88, s.x[30]); m.write64(buffer + 96, s.sp);
        m.write64(buffer + 104, jmp_cookie ^ s.sp ^ s.x[30]);
        for (unsigned index = 0; index < 8; ++index) m.write64(buffer + 112 + index * 8, s.v[8 + index][0]);
        s.x[0] = 0;
    }
    void long_jump(transmute::CpuState& s, transmute::GuestMemory& m, std::uint64_t buffer, std::uint64_t value, bool mask) {
        if (!accessible(m, buffer, 184, false)) throw std::runtime_error("Darwin longjmp received an invalid jmp_buf");
        const auto sp = m.read64(buffer + 96), lr = m.read64(buffer + 88);
        if (m.read64(buffer + 104) != (jmp_cookie ^ sp ^ lr))
            throw std::runtime_error("Darwin longjmp to a jmp_buf that setjmp did not initialize");
        const auto [low, high] = context.calling_engine().stack_bounds();
        if (s.sp >= low && s.sp < high && sp >= low && sp < high && sp < s.sp)
            throw std::runtime_error("Darwin longjmp into a stack frame that has already returned");
        if (mask && m.read32(buffer + 180)) masks.set(m.read32(buffer + 176));
        for (unsigned index = 0; index < 10; ++index) s.x[19 + index] = m.read64(buffer + index * 8);
        s.x[29] = m.read64(buffer + 80); s.x[30] = lr; s.sp = sp;
        for (unsigned index = 0; index < 8; ++index) s.v[8 + index] = {m.read64(buffer + 112 + index * 8), 0};
        const auto result = static_cast<std::uint32_t>(value);
        s.x[0] = result ? result : 1; // the synthetic call returns to the setjmp call site
    }
    // ucontext_t: uc_onstack @0, uc_sigmask @4, uc_stack @8 {ss_sp, ss_size, ss_flags},
    // uc_link @32, uc_mcsize @40, uc_mcontext @48, __mcontext_data @64 (816 bytes).
    // mcontext64: es @0, ss @16 {x0..x28, fp @248, lr @256, sp @264, pc @272, cpsr @280},
    // ns @288 {v0..v31, fpsr @800, fpcr @804}.
    static constexpr std::uint64_t ucontext_size = 880, mcontext_size = 816;
    void save_context(transmute::CpuState& s, transmute::GuestMemory& m, std::uint64_t context_address) {
        validate(m, context_address, ucontext_size, true);
        const auto machine = context_address + 64;
        m.write32(context_address + 4, masks.get());
        m.write32(context_address, 0);
        m.write64(context_address + 8, 0); m.write64(context_address + 16, 0); m.write32(context_address + 24, 4); // SS_DISABLE
        m.write64(context_address + 40, mcontext_size); m.write64(context_address + 48, machine);
        m.fill_bytes(machine, 0, mcontext_size);
        for (unsigned index = 1; index < 29; ++index) m.write64(machine + 16 + index * 8, s.x[index]);
        m.write64(machine + 16, 0); // the resumed getcontext/swapcontext returns 0
        m.write64(machine + 248, s.x[29]); m.write64(machine + 256, s.x[30]);
        m.write64(machine + 264, s.sp); m.write64(machine + 272, s.x[30]); m.write32(machine + 280, s.nzcv);
        for (unsigned index = 0; index < 32; ++index) {
            m.write64(machine + 288 + index * 16, s.v[index][0]); m.write64(machine + 296 + index * 16, s.v[index][1]);
        }
        m.write32(machine + 800, s.fpsr); m.write32(machine + 804, s.fpcr);
        s.x[0] = 0;
    }
    void restore_context(transmute::CpuState& s, transmute::GuestMemory& m, std::uint64_t context_address) {
        if (!accessible(m, context_address, 64, false)) throw std::runtime_error("Darwin setcontext received an invalid ucontext");
        const auto machine = m.read64(context_address + 48);
        if (!accessible(m, machine, 808, false)) throw std::runtime_error("Darwin setcontext received an invalid mcontext");
        masks.set(m.read32(context_address + 4));
        for (unsigned index = 0; index < 29; ++index) s.x[index] = m.read64(machine + 16 + index * 8);
        s.x[29] = m.read64(machine + 248);
        s.sp = m.read64(machine + 264);
        s.nzcv = m.read32(machine + 280);
        for (unsigned index = 0; index < 32; ++index) s.v[index] = {m.read64(machine + 288 + index * 16), m.read64(machine + 296 + index * 16)};
        s.fpsr = m.read32(machine + 800); s.fpcr = m.read32(machine + 804);
        // The synthetic call returns to the private entry, which installs lr and branches to pc.
        s.x[16] = m.read64(machine + 272); s.x[17] = m.read64(machine + 256);
        s.x[30] = context_entry;
    }

    // ---- passwd ---------------------------------------------------------------
    struct Password { std::string name, directory; };
    Password password() {
        Password value;
        value.name = "alchemy"; // the virtual login name getlogin() reports
        if (const auto home = environment("HOME")) value.directory = *home;
        else if (const auto profile = environment("USERPROFILE")) value.directory = guest_path_string(utf8_path(*profile));
        else value.directory = "/";
        return value;
    }
    // Writes struct passwd at `record` with strings in [storage, storage + capacity).
    bool write_password(transmute::GuestMemory& m, std::uint64_t record, std::uint64_t storage, std::uint64_t capacity) {
        const auto value = password();
        const std::array<std::string_view, 6> strings{value.name, "*", "", value.name, value.directory, "/bin/sh"};
        std::uint64_t needed{};
        for (const auto item : strings) needed += item.size() + 1;
        if (needed > capacity) return false;
        validate(m, record, 72, true); validate(m, storage, needed, true);
        std::array<std::uint64_t, 6> addresses{};
        auto at = storage;
        for (std::size_t index = 0; index < strings.size(); ++index) { addresses[index] = at; put_text(m, at, strings[index]); at += strings[index].size() + 1; }
        m.fill_bytes(record, 0, 72);
        m.write64(record, addresses[0]); m.write64(record + 8, addresses[1]);
        m.write32(record + 16, 1000); m.write32(record + 20, 1000);
        m.write64(record + 32, addresses[2]); m.write64(record + 40, addresses[3]);
        m.write64(record + 48, addresses[4]); m.write64(record + 56, addresses[5]);
        return true;
    }
};

using Operation = std::function<std::uint64_t(transmute::CpuState&, transmute::GuestMemory&)>;
// errno-style results: ExtraError sets errno and returns `failure`.
void install(const std::shared_ptr<State>& state, std::string name, Operation operation, std::uint64_t failure = UINT64_MAX) {
    static_cast<void>(state->context.install_function(std::move(name), [state, operation = std::move(operation), failure](auto& s, auto& m) {
        try { s.x[0] = operation(s, m); }
        catch (const ExtraError& error) { state->context.set_errno(error.value); s.x[0] = failure; }
        catch (const std::bad_alloc&) { state->context.set_errno(enomem); s.x[0] = failure; }
    }));
}
// Mach and pthread-style results: ExtraError becomes the returned code.
void install_code(const std::shared_ptr<State>& state, std::string name, Operation operation) {
    static_cast<void>(state->context.install_function(std::move(name), [operation = std::move(operation)](auto& s, auto& m) {
        try { s.x[0] = operation(s, m); }
        catch (const ExtraError& error) { s.x[0] = error.value; }
    }));
}
// Register-level handlers; argument faults become diagnostics, never a silent return.
void install_raw(const std::shared_ptr<State>& state, std::string name, transmute::GuestCallHandler handler) {
    static_cast<void>(state->context.install_function(name, [name, handler = std::move(handler)](auto& s, auto& m) {
        try { handler(s, m); }
        catch (const ExtraError& error) {
            throw std::runtime_error("Darwin " + name.substr(1) + " failed with errno " + std::to_string(error.value));
        }
    }));
}
std::uint64_t private_code(ProcessContext& context, std::span<const std::uint32_t> instructions, std::span<const std::uint64_t> literals = {}) {
    const auto size = instructions.size() * 4 + literals.size() * 8;
    const auto address = context.allocate_guest(size, 16);
    auto& engine = context.calling_engine();
    for (std::size_t index = 0; index < instructions.size(); ++index) engine.write32(address + index * 4, instructions[index]);
    for (std::size_t index = 0; index < literals.size(); ++index) engine.write64(address + instructions.size() * 4 + index * 8, literals[index]);
    engine.set_memory_access(address, true, false, true);
    return address;
}

// ---- Mach -------------------------------------------------------------------------
constexpr std::uint64_t kern_success = 0, kern_invalid_argument = 4, kern_aborted = 14, kern_invalid_name = 15,
    kern_terminated = 37, kern_not_supported = 46, kern_operation_timed_out = 49;
constexpr std::uint32_t task_port = libsystem::headless_task_port, host_port = 0xa1c3, system_clock_port = 0xa1d1, calendar_clock_port = 0xa1d3;

void install_blocks(const std::shared_ptr<State>& state) {
    auto& context = state->context;
    const std::array<const char*, 6> classes{"__NSConcreteGlobalBlock", "__NSConcreteStackBlock", "__NSConcreteMallocBlock",
        "__NSConcreteAutoBlock", "__NSConcreteFinalizingBlock", "__NSConcreteWeakBlockVariable"};
    for (const auto name : classes) {
        // Class-sized zeroed storage. The Objective-C runtime registers its
        // Blocks classes (__NSStackBlock__, __NSMallocBlock__, ...) in this
        // storage, so block literals are real objects; it allocates and
        // exports the storage itself when it is installed first.
        std::uint64_t address{};
        if (const auto existing = context.resolve(std::string(system_library), name)) address = existing->address;
        else {
            address = context.allocate_guest(256);
            context.calling_engine().fill_bytes(address, 0, 256);
            context.install_export(std::string(system_library), name, {address, ExportKind::data});
        }
        if (std::string_view(name) == "__NSConcreteMallocBlock") state->malloc_block_isa = address;
    }
    install(state, "__Block_copy", [state](auto& s, auto& m) { return state->block_copy(m, s.x[0]); }, 0);
    install(state, "__Block_release", [state](auto& s, auto& m) { state->block_release(m, s.x[0]); return std::uint64_t{0}; });
    install(state, "__Block_object_assign", [state](auto& s, auto& m) {
        const auto destination = s.x[0], object = s.x[1];
        if (!accessible(m, destination, 8, true)) throw std::runtime_error("_Block_object_assign received an invalid destination");
        switch (static_cast<std::uint32_t>(s.x[2]) & 0x9f) {
        case 3: state->retain_object(object); m.write64(destination, object); break;          // object
        case 7: m.write64(destination, state->block_copy(m, object)); break;                  // block
        case 8: case 24: m.write64(destination, state->byref_copy(m, object)); break;         // __block (weak)
        case 131: case 135: case 147: case 151: m.write64(destination, object); break;        // byref caller
        default: throw std::runtime_error("unsupported Darwin _Block_object_assign flags");
        }
        return std::uint64_t{0};
    });
    install(state, "__Block_object_dispose", [state](auto& s, auto& m) {
        switch (static_cast<std::uint32_t>(s.x[1]) & 0x9f) {
        case 3: state->release_object(s.x[0]); break;
        case 7: state->block_release(m, s.x[0]); break;
        case 8: case 24: state->byref_release(m, s.x[0]); break;
        case 131: case 135: case 147: case 151: break;
        default: throw std::runtime_error("unsupported Darwin _Block_object_dispose flags");
        }
        return std::uint64_t{0};
    });
    install(state, "__Block_isDeallocating", [](auto& s, auto& m) {
        if (!accessible(m, s.x[0], 32, false)) throw std::runtime_error("_Block_isDeallocating received an invalid block");
        return static_cast<std::uint64_t>((m.read32(s.x[0] + 8) & State::deallocating) != 0);
    });
}

void install_jumps(const std::shared_ptr<State>& state) {
    for (const auto& [name, mask] : std::array<std::pair<const char*, bool>, 2>{{{"_setjmp", true}, {"__setjmp", false}}})
        install_raw(state, name, [state, mask](auto& s, auto& m) { state->save_jump(s, m, s.x[0], mask); });
    install_raw(state, "_sigsetjmp", [state](auto& s, auto& m) { state->save_jump(s, m, s.x[0], static_cast<std::uint32_t>(s.x[1]) != 0); });
    for (const auto& [name, mask] : std::array<std::pair<const char*, bool>, 3>{{{"_longjmp", true}, {"__longjmp", false}, {"_siglongjmp", true}}})
        install_raw(state, name, [state, mask](auto& s, auto& m) { state->long_jump(s, m, s.x[0], s.x[1], mask); });
    install_raw(state, "_longjmperror", [](auto&, auto&) { throw std::runtime_error("Darwin longjmp botch"); });

    auto& context = state->context;
    // mov x30, x17 ; br x16
    const std::array<std::uint32_t, 3> entry{0xaa1103feU, 0xd61f0200U, 0xd4200000U};
    state->context_entry = private_code(context, entry);
    const auto finish = context.install_function("__alchemy_context_finish", [state](auto& s, auto& m) {
        // A makecontext function returned: resume uc_link, or end the thread.
        const auto link = accessible(m, s.x[0], 40, false) ? m.read64(s.x[0] + 32) : 0;
        if (link) { state->restore_context(s, m, link); return; }
        s.x[0] = 0; s.x[30] = state->symbol("_pthread_exit");
    }, "alchemy.internal");
    // mov x0, x19 ; ldr x16, literal ; br x16 ; brk ; .quad finish
    const std::array<std::uint32_t, 4> exit_path{0xaa1303e0U, 0x58000070U, 0xd61f0200U, 0xd4200000U};
    const std::array<std::uint64_t, 1> literal{finish};
    state->context_return = private_code(context, exit_path, literal);
    install(state, "_getcontext", [state](auto& s, auto& m) { state->save_context(s, m, s.x[0]); return std::uint64_t{0}; });
    install_raw(state, "_setcontext", [state](auto& s, auto& m) {
        if (!accessible(m, s.x[0], 64, false)) { state->context.set_errno(efault); s.x[0] = UINT64_MAX; return; }
        state->restore_context(s, m, s.x[0]);
    });
    install_raw(state, "_swapcontext", [state](auto& s, auto& m) {
        if (!accessible(m, s.x[0], State::ucontext_size, true) || !accessible(m, s.x[1], 64, false)) {
            state->context.set_errno(efault); s.x[0] = UINT64_MAX; return;
        }
        const auto target = s.x[1];
        state->save_context(s, m, s.x[0]);
        state->restore_context(s, m, target);
    });
    install_raw(state, "_makecontext", [state](auto& s, auto& m) {
        const auto ucp = s.x[0], function = s.x[1];
        const auto count = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[2]));
        if (count < 0 || count > 8) throw std::runtime_error("Darwin makecontext supports zero to eight integer arguments");
        if (!accessible(m, ucp, State::ucontext_size, true) || !function) throw std::runtime_error("Darwin makecontext received an invalid context");
        auto machine = m.read64(ucp + 48);
        if (!machine) { machine = ucp + 64; m.write64(ucp + 48, machine); m.write64(ucp + 40, State::mcontext_size); }
        if (!accessible(m, machine, 808, true)) throw std::runtime_error("Darwin makecontext received an invalid mcontext");
        const auto stack = m.read64(ucp + 8), size = m.read64(ucp + 16);
        if (!stack || size < 4096 || !accessible(m, stack, size, true)) throw std::runtime_error("Darwin makecontext requires a valid uc_stack");
        // Darwin arm64 passes variadic arguments in 8-byte stack slots.
        for (std::int32_t index = 0; index < count; ++index)
            m.write64(machine + 16 + static_cast<std::uint64_t>(index) * 8, m.read64(s.sp + static_cast<std::uint64_t>(index) * 8));
        m.write64(machine + 16 + 19 * 8, ucp);
        m.write64(machine + 248, 0); m.write64(machine + 256, state->context_return);
        m.write64(machine + 264, (stack + size) & ~15ULL); m.write64(machine + 272, function);
    });
}

void install_mach(const std::shared_ptr<State>& state) {
    install(state, "_mach_host_self", [](auto&, auto&) { return std::uint64_t{host_port}; });
    install(state, "_mach_thread_self", [state](auto&, auto&) {
        // Matches pthread_mach_thread_np: the thread's identity is its port name.
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(state->context.current_thread()->identity));
    });
    // mach_port_deallocate, task_info, thread_policy_set and thread_policy_get
    // (alchemy.exe only; mach_exports.cpp owns them in alchemy-fx).
    libsystem::install_mach_task_shell(state->context);
    install_code(state, "_semaphore_create", [state](auto& s, auto& m) {
        const auto value = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[3]));
        if (static_cast<std::uint32_t>(s.x[0]) != task_port || value < 0 || !accessible(m, s.x[1], 4, true)) return kern_invalid_argument;
        auto semaphore = std::make_shared<State::Semaphore>();
        semaphore->count = static_cast<std::uint64_t>(value);
        std::lock_guard lock(state->mutex);
        const auto name = state->next_semaphore; state->next_semaphore += 4;
        state->semaphores.emplace(name, std::move(semaphore));
        m.write32(s.x[1], name);
        return kern_success;
    });
    const auto lookup = [state](std::uint64_t name) -> std::shared_ptr<State::Semaphore> {
        std::lock_guard lock(state->mutex);
        const auto found = state->semaphores.find(static_cast<std::uint32_t>(name));
        if (found == state->semaphores.end()) throw ExtraError{static_cast<std::uint32_t>(kern_invalid_argument)};
        return found->second;
    };
    install_code(state, "_semaphore_destroy", [state](auto& s, auto&) {
        if (static_cast<std::uint32_t>(s.x[0]) != task_port) return kern_invalid_argument;
        std::shared_ptr<State::Semaphore> value;
        { std::lock_guard lock(state->mutex); const auto found = state->semaphores.find(static_cast<std::uint32_t>(s.x[1]));
          if (found == state->semaphores.end()) return kern_invalid_argument; value = found->second; state->semaphores.erase(found); }
        std::lock_guard lock(value->gate); value->destroyed = true; value->changed.notify_all();
        return kern_success;
    });
    for (const auto& [name, all] : std::array<std::pair<const char*, bool>, 2>{{{"_semaphore_signal", false}, {"_semaphore_signal_all", true}}})
        install_code(state, name, [lookup, all](auto& s, auto&) {
            const auto value = lookup(s.x[0]);
            std::lock_guard lock(value->gate);
            if (all) { value->tickets = value->waiters; value->changed.notify_all(); }
            else if (value->tickets < value->waiters) { ++value->tickets; value->changed.notify_one(); }
            else ++value->count;
            return kern_success;
        });
    const auto wait = [state, lookup](std::uint64_t name, std::optional<std::chrono::steady_clock::time_point> deadline) {
        const auto value = lookup(name);
        std::unique_lock lock(value->gate);
        if (value->count) { --value->count; return kern_success; }
        ++value->waiters;
        std::uint64_t result = kern_success;
        while (!value->tickets) {
            if (value->destroyed) { result = kern_terminated; break; }
            if (deadline && std::chrono::steady_clock::now() >= *deadline) { result = kern_operation_timed_out; break; }
            if (state->context.requested_exit()) {
                --value->waiters; lock.unlock();
                if (guest_worker_must_stop(state->context)) throw GuestThreadStop{};
                return kern_aborted;
            }
            value->changed.wait_for(lock, std::chrono::milliseconds(10));
            if (value->count && !value->tickets) { --value->count; --value->waiters; return kern_success; }
        }
        if (result == kern_success) --value->tickets;
        --value->waiters;
        return result;
    };
    install_code(state, "_semaphore_wait", [wait](auto& s, auto&) { return wait(s.x[0], std::nullopt); });
    install_code(state, "_semaphore_timedwait", [wait](auto& s, auto&) {
        // mach_timespec_t {unsigned tv_sec; int tv_nsec} arrives by value in x1.
        const auto seconds = static_cast<std::uint32_t>(s.x[1]);
        const auto nanos = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[1] >> 32));
        if (nanos < 0 || nanos >= 1'000'000'000) return kern_invalid_argument;
        return wait(s.x[0], std::chrono::steady_clock::now() + std::chrono::seconds(seconds) + std::chrono::nanoseconds(nanos));
    });
    install_code(state, "_host_get_clock_service", [](auto& s, auto& m) {
        const auto id = static_cast<std::uint32_t>(s.x[1]);
        if (static_cast<std::uint32_t>(s.x[0]) != host_port || id > 1 || !accessible(m, s.x[2], 4, true)) return kern_invalid_argument;
        m.write32(s.x[2], id ? calendar_clock_port : system_clock_port);
        return kern_success;
    });
    install_code(state, "_clock_get_time", [](auto& s, auto& m) {
        const auto port = static_cast<std::uint32_t>(s.x[0]);
        if ((port != system_clock_port && port != calendar_clock_port) || !accessible(m, s.x[1], 8, true)) return kern_invalid_argument;
        const auto value = port == calendar_clock_port ? wall_nanoseconds() : uptime_nanoseconds();
        m.write32(s.x[1], static_cast<std::uint32_t>(value / 1'000'000'000)); m.write32(s.x[1] + 4, static_cast<std::uint32_t>(value % 1'000'000'000));
        return kern_success;
    });
    install_code(state, "_task_get_exception_ports", [](auto& s, auto& m) {
        // No exception ports are ever registered with an Alchemy task.
        if (static_cast<std::uint32_t>(s.x[0]) != task_port || !accessible(m, s.x[3], 4, true)) return kern_invalid_argument;
        m.write32(s.x[3], 0);
        return kern_success;
    });
    install_code(state, "_task_set_exception_ports", [](auto& s, auto&) {
        if (static_cast<std::uint32_t>(s.x[0]) != task_port) return kern_invalid_argument;
        // Clearing to MACH_PORT_NULL matches the existing state; Mach exception delivery is unsupported.
        return static_cast<std::uint32_t>(s.x[2]) == 0 ? kern_success : kern_not_supported;
    });
    install(state, "_proc_pid_rusage", [state](auto& s, auto& m) {
        const auto pid = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[0]));
        if (pid != static_cast<std::int32_t>(state->call("_getpid", {}))) throw ExtraError{esrch};
        static constexpr std::array<std::uint64_t, 6> sizes{96, 144, 160, 232, 296, 304};
        const auto flavor = static_cast<std::uint32_t>(s.x[1]);
        if (flavor >= sizes.size()) throw ExtraError{einval};
        validate(m, s.x[2], sizes[flavor], true);
        m.fill_bytes(s.x[2], 0, sizes[flavor]);
        const auto usage = libsystem::process_usage();
        m.write64(s.x[2] + 16, usage.user); m.write64(s.x[2] + 24, usage.system);
        m.write64(s.x[2] + 64, usage.resident); m.write64(s.x[2] + 72, usage.footprint);
        if (flavor >= 4) { m.write64(s.x[2] + 240, usage.resident_peak); m.write64(s.x[2] + 280, usage.resident_peak); }
        return std::uint64_t{0};
    });
}

void install_identity(const std::shared_ptr<State>& state) {
    install(state, "_gethostuuid", [](auto& s, auto& m) {
        validate(m, s.x[0], 16, true);
        const auto uuid = host::system::machine_uuid();
        if (!uuid) throw ExtraError{uuid.error() == host::Error::unsupported ? enosys : eio};
        m.write_bytes(s.x[0], *uuid);
        return std::uint64_t{0};
    });
    for (const auto& [name, upper] : std::array<std::pair<const char*, bool>, 3>{{{"_uuid_unparse", true}, {"_uuid_unparse_upper", true}, {"_uuid_unparse_lower", false}}})
        install(state, name, [upper](auto& s, auto& m) {
            // Darwin's uuid_unparse produces upper-case digits.
            validate(m, s.x[0], 16, false); validate(m, s.x[1], 37, true);
            std::string value;
            const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
            for (unsigned index = 0; index < 16; ++index) {
                if (index == 4 || index == 6 || index == 8 || index == 10) value += '-';
                const auto byte = m.read8(s.x[0] + index);
                value += digits[byte >> 4]; value += digits[byte & 15];
            }
            put_text(m, s.x[1], value);
            return std::uint64_t{0};
        });
    install(state, "_uuid_parse", [](auto& s, auto& m) -> std::uint64_t {
        const auto value = text(m, s.x[0]);
        if (value.size() != 36) return UINT32_MAX;
        std::array<std::uint8_t, 16> bytes{};
        unsigned index = 0;
        for (std::size_t at = 0; at < value.size(); ++at) {
            if (at == 8 || at == 13 || at == 18 || at == 23) { if (value[at] != '-') return UINT32_MAX; continue; }
            const auto c = value[at];
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) return UINT32_MAX;
            bytes[index / 2] = static_cast<std::uint8_t>(bytes[index / 2] | (digit << ((index & 1) ? 0 : 4))); ++index;
        }
        validate(m, s.x[1], 16, true); m.write_bytes(s.x[1], bytes);
        return 0;
    });
    for (const auto name : {"_uuid_generate", "_uuid_generate_random"})
        install(state, name, [](auto& s, auto& m) {
            validate(m, s.x[0], 16, true);
            std::random_device source;
            std::array<std::uint8_t, 16> bytes{};
            for (auto& byte : bytes) byte = static_cast<std::uint8_t>(source());
            bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40); bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
            m.write_bytes(s.x[0], bytes);
            return std::uint64_t{0};
        });
    install(state, "_uuid_clear", [](auto& s, auto& m) { validate(m, s.x[0], 16, true); m.fill_bytes(s.x[0], 0, 16); return std::uint64_t{0}; });
    install(state, "_uuid_copy", [](auto& s, auto& m) { validate(m, s.x[0], 16, true); validate(m, s.x[1], 16, false); m.copy_bytes(s.x[0], s.x[1], 16); return std::uint64_t{0}; });
    install(state, "_uuid_is_null", [](auto& s, auto& m) {
        validate(m, s.x[0], 16, false);
        for (unsigned index = 0; index < 16; ++index) if (m.read8(s.x[0] + index)) return std::uint64_t{0};
        return std::uint64_t{1};
    });
    install(state, "_uuid_compare", [](auto& s, auto& m) {
        validate(m, s.x[0], 16, false); validate(m, s.x[1], 16, false);
        for (unsigned index = 0; index < 16; ++index) {
            const int a = m.read8(s.x[0] + index), b = m.read8(s.x[1] + index);
            if (a != b) return static_cast<std::uint64_t>(static_cast<std::uint32_t>(a < b ? -1 : 1));
        }
        return std::uint64_t{0};
    });
    install(state, "__availability_version_check", [](auto& s, auto& m) {
        // Alchemy answers for its macOS 14.0 profile (darwin_version.hpp).
        const auto count = static_cast<std::uint32_t>(s.x[0]);
        if (count > 64) return std::uint64_t{0};
        if (count) validate(m, s.x[1], count * 8ULL, false);
        for (std::uint32_t index = 0; index < count; ++index)
            if (m.read32(s.x[1] + index * 8ULL) == 1) return static_cast<std::uint64_t>(m.read32(s.x[1] + index * 8ULL + 4) <= macos_packed_version);
        return std::uint64_t{0};
    });
    // libmacho section lookup (getsectiondata(3)) over a loaded 64-bit image:
    // the Swift runtime scans its __swift5_* sections through these. The
    // result is slid by the image's load address relative to __TEXT.
    struct SegmentMatch { std::uint64_t segment{}, section{}, size{}; };
    const auto find_section = [](transmute::GuestMemory& m, std::uint64_t header, std::uint64_t segment_name,
                                 std::uint64_t section_name) -> std::optional<SegmentMatch> {
        validate(m, header, 32, false);
        if (m.read32(header) != 0xfeedfacfU) return std::nullopt;
        const auto count = m.read32(header + 16), commands_size = m.read32(header + 20);
        if (count > 4096 || commands_size > (1U << 24)) return std::nullopt;
        validate(m, header + 32, commands_size, false);
        const auto read_name = [&m](std::uint64_t at) {
            std::string value;
            for (unsigned i = 0; i < 16; ++i) { const auto c = m.read8(at + i); if (!c) break; value.push_back(static_cast<char>(c)); }
            return value;
        };
        const auto wanted_segment = text(m, segment_name);
        const auto wanted_section = section_name ? text(m, section_name) : std::string();
        std::optional<std::uint64_t> slide;
        std::optional<SegmentMatch> found;
        auto cursor = header + 32;
        const auto end = cursor + commands_size;
        for (std::uint32_t index = 0; index < count && cursor + 8 <= end; ++index) {
            const auto command = m.read32(cursor), size = m.read32(cursor + 4);
            if (size < 8 || cursor + size > end) return std::nullopt;
            if (command == 0x19 && size >= 72) {
                const auto name = read_name(cursor + 8);
                const auto vmaddr = m.read64(cursor + 24), vmsize = m.read64(cursor + 32), fileoff = m.read64(cursor + 40);
                if (fileoff == 0 && m.read64(cursor + 48) != 0 && !slide) slide = header - vmaddr;
                if (name == wanted_segment && !found) {
                    if (!section_name) found = SegmentMatch{vmaddr, 0, vmsize};
                    else {
                        const auto sections = m.read32(cursor + 64);
                        for (std::uint32_t s = 0; s < sections && 72 + (s + 1) * 80ULL <= size; ++s) {
                            const auto entry = cursor + 72 + s * 80ULL;
                            if (read_name(entry) == wanted_section && read_name(entry + 16) == wanted_segment) {
                                found = SegmentMatch{vmaddr, m.read64(entry + 32), m.read64(entry + 40)};
                                break;
                            }
                        }
                    }
                }
            }
            cursor += size;
        }
        if (!found || !slide) return std::nullopt;
        found->segment += *slide;
        if (section_name) found->section += *slide;
        return found;
    };
    install(state, "_getsectiondata", [find_section](auto& s, auto& m) {
        const auto match = find_section(m, s.x[0], s.x[1], s.x[2]);
        if (!match) return std::uint64_t{0};
        validate(m, s.x[3], 8, true);
        m.write64(s.x[3], match->size);
        return match->section;
    }, 0);
    install(state, "_getsegmentdata", [find_section](auto& s, auto& m) {
        const auto match = find_section(m, s.x[0], s.x[1], 0);
        if (!match) return std::uint64_t{0};
        validate(m, s.x[2], 8, true);
        m.write64(s.x[2], match->size);
        return match->segment;
    }, 0);
    install(state, "_sys_icache_invalidate", [state](auto& s, auto&) {
        // Engines without explicit invalidation track guest code writes themselves.
        static_cast<void>(invalidate_engine_code(state->context.calling_engine(), s.x[0], s.x[1]));
        return std::uint64_t{0};
    });
    install(state, "_sys_dcache_flush", [](auto&, auto&) { return std::uint64_t{0}; });
    install(state, "_mincore", [](auto& s, auto& m) {
        if (s.x[0] % page_size) throw ExtraError{einval};
        const auto pages = (s.x[1] + page_size - 1) / page_size;
        validate(m, s.x[2], pages, true);
        for (std::uint64_t index = 0; index < pages; ++index) {
            const auto region = m.query_region(add(s.x[0], index * page_size));
            if (!region) throw ExtraError{enomem};
            // Committed guest pages are resident from the guest's point of view.
            m.write8(s.x[2] + index, region->readable ? 1 : 0);
        }
        return std::uint64_t{0};
    });
    install_code(state, "_posix_madvise", [](auto& s, auto&) {
        // Advisory only; POSIX_MADV_DONTNEED keeps contents, so ignoring advice is exact.
        return s.x[2] <= 4 ? std::uint64_t{0} : std::uint64_t{einval};
    });
    // madvise: DarwinServices (services.cpp), which lets the host drop MADV_FREE pages.
}

constexpr std::uint32_t glob_append = 1, glob_dooffs = 2, glob_err = 4, glob_mark = 8, glob_nocheck = 0x10, glob_nosort = 0x20,
    glob_altdirfunc = 0x40, glob_brace = 0x80, glob_magchar = 0x100, glob_nomagic = 0x200, glob_tilde = 0x800, glob_noescape = 0x2000;

void install_patterns(const std::shared_ptr<State>& state) {
    install_code(state, "_fnmatch", [](auto& s, auto& m) {
        return std::uint64_t{fnmatch(text(m, s.x[0]), text(m, s.x[1]), static_cast<int>(s.x[2])) ? 0U : 1U};
    });
    install_code(state, "_regcomp", [state](auto& s, auto& m) -> std::uint64_t {
        const auto flags = static_cast<std::uint32_t>(s.x[2]);
        if (!accessible(m, s.x[0], 32, true)) return reg_invarg;
        if (flags & ~(reg_extended | reg_icase | reg_nosub | reg_newline | reg_nospec | reg_pend)) return reg_invarg;
        std::string pattern;
        if (flags & reg_pend) {
            const auto end = m.read64(s.x[0] + 16);
            if (end < s.x[1] || end - s.x[1] > string_limit) return reg_invarg;
            pattern.resize(static_cast<std::size_t>(end - s.x[1]));
            m.read_bytes(s.x[1], {reinterpret_cast<std::uint8_t*>(pattern.data()), pattern.size()});
        } else pattern = text(m, s.x[1]);
        if (flags & reg_nospec) {
            if (flags & reg_extended) return reg_invarg;
            std::string literal;
            for (const auto c : pattern) { if (std::string_view(".[]\\*^$").find(c) != std::string_view::npos) literal += '\\'; literal += c; }
            pattern = std::move(literal);
        }
        auto program = std::make_shared<State::Program>();
        auto options = (flags & reg_extended) ? std::regex::extended : std::regex::basic;
        if (flags & reg_icase) options |= std::regex::icase;
        if (flags & reg_nosub) options |= std::regex::nosubs;
        try { program->expression = std::regex(pattern, options); }
        catch (const std::regex_error& error) { return posix_regex_code(error.code()); }
        catch (const std::bad_alloc&) { return reg_espace; }
        program->flags = flags;
        program->groups = (flags & reg_nosub) ? 0 : program->expression.mark_count();
        std::lock_guard lock(state->mutex);
        state->programs[s.x[0]] = std::move(program);
        m.write32(s.x[0], regex_magic);
        m.write64(s.x[0] + 8, (flags & reg_nosub) ? 0 : state->programs[s.x[0]]->expression.mark_count());
        m.write64(s.x[0] + 24, state->next_program++);
        return 0;
    });
    install_code(state, "_regexec", [state](auto& s, auto& m) -> std::uint64_t {
        std::shared_ptr<State::Program> program;
        { std::lock_guard lock(state->mutex); const auto found = state->programs.find(s.x[0]);
          if (found == state->programs.end() || m.read32(s.x[0]) != regex_magic) return reg_badpat; program = found->second; }
        const auto flags = static_cast<std::uint32_t>(s.x[4]);
        auto count = (program->flags & reg_nosub) ? 0 : s.x[2];
        const auto matches = s.x[3];
        if (count && !accessible(m, matches, count * 16, true)) return reg_invarg;
        std::string subject;
        std::uint64_t base = 0;
        if (flags & reg_startend) {
            if (!accessible(m, matches, 16, true)) return reg_invarg;
            const auto start = m.read64(matches), end = m.read64(matches + 8);
            if (end < start || end - start > string_limit) return reg_invarg;
            subject.resize(static_cast<std::size_t>(end - start));
            m.read_bytes(s.x[1] + start, {reinterpret_cast<std::uint8_t*>(subject.data()), subject.size()});
            base = start;
        } else subject = text(m, s.x[1]);
        // REG_NEWLINE: '.' and non-matching lists exclude newline, ^/$ match at line breaks.
        std::vector<std::pair<std::size_t, std::size_t>> lines;
        if (program->flags & reg_newline) {
            std::size_t start = 0;
            for (;;) { const auto end = subject.find('\n', start); lines.emplace_back(start, end == std::string::npos ? subject.size() : end);
                if (end == std::string::npos) break; start = end + 1; }
        } else lines.emplace_back(0, subject.size());
        std::cmatch result;
        for (std::size_t index = 0; index < lines.size(); ++index) {
            auto options = std::regex_constants::match_default;
            if (index == 0 && (flags & reg_notbol)) options |= std::regex_constants::match_not_bol;
            if (index + 1 == lines.size() && (flags & reg_noteol)) options |= std::regex_constants::match_not_eol;
            const char* begin = subject.data() + lines[index].first;
            const char* end = subject.data() + lines[index].second;
            bool found{};
            try { found = std::regex_search(begin, end, result, program->expression, options); }
            catch (const std::regex_error& error) { return posix_regex_code(error.code()); }
            if (!found) continue;
            for (std::uint64_t group = 0; group < count; ++group) {
                std::int64_t start = -1, stop = -1;
                if (group < result.size() && result[group].matched) {
                    start = static_cast<std::int64_t>(base + (result[group].first - subject.data()));
                    stop = static_cast<std::int64_t>(base + (result[group].second - subject.data()));
                }
                m.write64(matches + group * 16, std::bit_cast<std::uint64_t>(start)); m.write64(matches + group * 16 + 8, std::bit_cast<std::uint64_t>(stop));
            }
            return 0;
        }
        return reg_nomatch;
    });
    install(state, "_regerror", [state](auto& s, auto& m) {
        const auto code = static_cast<std::uint32_t>(s.x[0]);
        std::string message;
        if (code & 256) { // REG_ITOA
            message = regex_name(code & 255);
        } else if (code == 255) { // REG_ATOI: the buffer names an error code
            const auto name = text(m, m.read64(s.x[1] + 16));
            message = "0";
            for (std::uint32_t index = 0; index < 18; ++index) if (name == regex_name(index)) message = std::to_string(index);
        } else message = regex_message(code);
        if (s.x[3]) {
            const auto count = std::min<std::uint64_t>(message.size(), s.x[3] - 1);
            validate(m, s.x[2], count + 1, true);
            put_text(m, s.x[2], std::string_view(message).substr(0, static_cast<std::size_t>(count)));
        }
        static_cast<void>(state);
        return static_cast<std::uint64_t>(message.size() + 1);
    }, 0);
    install(state, "_regfree", [state](auto& s, auto& m) {
        std::lock_guard lock(state->mutex);
        if (state->programs.erase(s.x[0])) { m.write32(s.x[0], 0); m.write64(s.x[0] + 24, 0); }
        return std::uint64_t{0};
    });

    // glob_t: gl_pathc @0, gl_matchc @8, gl_offs @16, gl_flags @24, gl_pathv @32, gl_errfunc @40.
    install_code(state, "_glob", [state](auto& s, auto& m) -> std::uint64_t {
        constexpr std::uint64_t nospace = static_cast<std::uint32_t>(-1), aborted = static_cast<std::uint32_t>(-2),
            nomatch = static_cast<std::uint32_t>(-3), nosys = static_cast<std::uint32_t>(-4);
        const auto flags = static_cast<std::uint32_t>(s.x[1]);
        const auto output = s.x[3], callback = s.x[2];
        if (!accessible(m, output, 88, true)) return nospace;
        if (flags & glob_altdirfunc) return nosys;
        auto pattern = text(m, s.x[0]);
        if ((flags & glob_tilde) && !pattern.empty() && pattern[0] == '~' && (pattern.size() == 1 || pattern[1] == '/')) {
            const auto home = state->environment("HOME").value_or(state->password().directory);
            pattern = home + pattern.substr(1);
        }
        const bool literal_escape = (flags & glob_noescape) != 0;
        std::vector<std::string> patterns;
        if (flags & glob_brace) expand_braces(pattern, patterns, literal_escape); else patterns.push_back(pattern);
        std::vector<std::string> found;
        bool magic{}, abort{};
        const auto failed = [&](const std::string& directory, std::uint32_t error) {
            if (callback) {
                const auto path = state->guest_string(directory.empty() ? "." : directory);
                const auto stop = static_cast<std::uint32_t>(state->invoke(callback, {path, error}));
                state->context.release_guest(path);
                if (stop) abort = true;
            }
            if (flags & glob_err) abort = true;
        };
        std::function<void(const std::string&, const std::vector<std::string>&, std::size_t, std::vector<std::string>&)> walk;
        walk = [&](const std::string& prefix, const std::vector<std::string>& parts, std::size_t index, std::vector<std::string>& into) {
            if (abort) return;
            if (into.size() > 1'000'000) throw ExtraError{enomem};
            const bool last = index + 1 == parts.size();
            const auto join = [&](const std::string& name) { return prefix.empty() ? name : prefix == "/" ? "/" + name : prefix + "/" + name; };
            const auto host = [&](const std::string& guest) { return state->context.map_guest_path(guest.empty() ? "." : guest); };
            if (parts[index].empty()) { // repeated or trailing separator
                if (last) into.push_back(prefix.empty() ? "/" : prefix + "/");
                else walk(prefix, parts, index + 1, into);
                return;
            }
            if (!has_magic(parts[index], literal_escape)) {
                const auto candidate = join(unescape(parts[index], literal_escape));
                // Intermediate literal components may be virtual (/Volumes/C); the next
                // directory listing or the final existence check decides.
                if (!last) { walk(candidate, parts, index + 1, into); return; }
                std::error_code error;
                const auto status = std::filesystem::symlink_status(host(candidate), error);
                if (error || !std::filesystem::exists(status)) return;
                {
                    auto value = candidate;
                    if ((flags & glob_mark) && std::filesystem::is_directory(std::filesystem::status(host(candidate), error))) value += '/';
                    into.push_back(value);
                }
                return;
            }
            magic = true;
            std::error_code error;
            std::filesystem::directory_iterator iterator(host(prefix), error);
            if (error) {
                // Missing or non-directory paths are no match; other failures go to errfunc/GLOB_ERR.
                if (error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory) failed(prefix, 13);
                return;
            }
            for (std::filesystem::directory_iterator end; iterator != end; iterator.increment(error)) {
                if (error) { failed(prefix, eio); return; }
                const auto bytes = iterator->path().filename().u8string();
                const std::string name(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                if (!fnmatch(parts[index], name, fnm_period | (literal_escape ? fnm_noescape : 0))) continue;
                const auto candidate = join(name);
                std::error_code kind;
                const bool directory = iterator->is_directory(kind);
                if (last) into.push_back((flags & glob_mark) && directory ? candidate + "/" : candidate);
                else if (directory) walk(candidate, parts, index + 1, into);
                if (abort) return;
            }
        };
        for (const auto& item : patterns) {
            std::vector<std::string> parts;
            std::string prefix;
            std::size_t start = 0;
            if (!item.empty() && item[0] == '/') { prefix = "/"; start = 1; }
            for (;;) { const auto end = item.find('/', start); parts.push_back(item.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos) break; start = end + 1; }
            std::vector<std::string> matches;
            if (item == "/") matches.push_back("/");
            else walk(prefix, parts, 0, matches);
            if (abort) break;
            if (!(flags & glob_nosort)) std::sort(matches.begin(), matches.end());
            if (matches.empty() && ((flags & glob_nocheck) || ((flags & glob_nomagic) && !has_magic(item, literal_escape))))
                matches.push_back(literal_escape ? item : unescape(item, false));
            found.insert(found.end(), matches.begin(), matches.end());
        }
        if (abort) return aborted;
        // Rebuild gl_pathv: offsets, earlier results with GLOB_APPEND, new matches, NULL.
        const auto offsets = (flags & glob_dooffs) ? m.read64(output + 16) : 0;
        if (offsets > 65536) return nospace;
        std::vector<std::uint64_t> previous;
        std::uint64_t old_vector{};
        if (flags & glob_append) {
            old_vector = m.read64(output + 32);
            const auto count = m.read64(output);
            if (old_vector) { validate(m, old_vector, (offsets + count) * 8, false);
                for (std::uint64_t index = 0; index < count; ++index) previous.push_back(m.read64(old_vector + (offsets + index) * 8)); }
        }
        if (found.empty()) {
            if (!(flags & glob_append)) { m.write64(output, 0); m.write64(output + 32, 0); }
            m.write32(output + 8, 0);
            return nomatch;
        }
        const auto total = offsets + previous.size() + found.size() + 1;
        const auto vector = state->context.allocate_guest(total * 8);
        std::vector<std::uint64_t> owned;
        for (std::uint64_t index = 0; index < offsets; ++index) m.write64(vector + index * 8, 0);
        std::size_t at = static_cast<std::size_t>(offsets);
        for (const auto value : previous) m.write64(vector + at++ * 8ULL, value);
        for (const auto& value : found) { const auto address = state->guest_string(value); owned.push_back(address); m.write64(vector + at++ * 8ULL, address); }
        m.write64(vector + at * 8ULL, 0);
        {
            std::lock_guard lock(state->mutex);
            if (old_vector) {
                const auto prior = state->globs.find(old_vector);
                if (prior != state->globs.end()) { owned.insert(owned.end(), prior->second.begin(), prior->second.end()); state->globs.erase(prior); state->context.release_guest(old_vector); }
            }
            state->globs.emplace(vector, std::move(owned));
        }
        m.write64(output, previous.size() + found.size());
        m.write32(output + 8, static_cast<std::uint32_t>(found.size()));
        m.write32(output + 24, flags | (magic ? glob_magchar : 0));
        m.write64(output + 32, vector);
        m.write64(output + 40, callback);
        return 0;
    });
    install(state, "_globfree", [state](auto& s, auto& m) {
        validate(m, s.x[0], 40, true);
        const auto vector = m.read64(s.x[0] + 32);
        if (vector) {
            std::vector<std::uint64_t> owned;
            { std::lock_guard lock(state->mutex); const auto found = state->globs.find(vector);
              if (found == state->globs.end()) throw std::runtime_error("Darwin globfree received a gl_pathv that glob did not allocate");
              owned = std::move(found->second); state->globs.erase(found); }
            for (const auto address : owned) state->context.release_guest(address);
            state->context.release_guest(vector);
        }
        m.write64(s.x[0], 0); m.write64(s.x[0] + 32, 0);
        return std::uint64_t{0};
    });

    // scandir builds its results with the guest allocator: callers free() each entry and the array.
    install(state, "_scandir", [state](auto& s, auto& m) {
        validate(m, s.x[1], 8, true);
        const auto directory = state->call("_opendir", {s.x[0]});
        if (!directory) throw ExtraError{state->errno_value()};
        std::vector<std::uint64_t> entries;
        std::uint64_t array{};
        try {
            for (;;) {
                const auto entry = state->call("_readdir", {directory});
                if (!entry) break;
                if (s.x[2] && !static_cast<std::uint32_t>(state->invoke(s.x[2], {entry}))) continue;
                const std::uint64_t size = std::max<std::uint64_t>(read16(m, entry + 16), 22ULL + read16(m, entry + 18));
                const auto copy = state->guest_malloc(size);
                entries.push_back(copy);
                m.copy_bytes(copy, entry, size);
            }
            static_cast<void>(state->call("_closedir", {directory}));
            array = state->guest_malloc(std::max<std::size_t>(entries.size(), 1) * 8);
            for (std::size_t index = 0; index < entries.size(); ++index) m.write64(array + index * 8, entries[index]);
            if (s.x[3] && entries.size() > 1) static_cast<void>(state->call("_qsort", {array, entries.size(), 8, s.x[3]}));
        } catch (...) {
            for (const auto entry : entries) state->guest_free(entry);
            state->guest_free(array);
            throw;
        }
        m.write64(s.x[1], array);
        return static_cast<std::uint64_t>(entries.size());
    });
    install(state, "_alphasort", [](auto& s, auto& m) {
        const auto left = text(m, m.read64(s.x[0]) + 21), right = text(m, m.read64(s.x[1]) + 21);
        const auto order = left.compare(right);
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(order < 0 ? -1 : order > 0 ? 1 : 0));
    });
}

void install_logging(const std::shared_ptr<State>& state) {
    // popen and pclose: install_process_services (process/process_model.cpp).
    install(state, "_openlog", [state](auto& s, auto& m) {
        std::lock_guard lock(state->mutex);
        state->log_identity = s.x[0] ? text(m, s.x[0]) : std::string{};
        state->log_options = static_cast<std::uint32_t>(s.x[1]);
        return std::uint64_t{0};
    });
    install(state, "_closelog", [state](auto&, auto&) {
        std::lock_guard lock(state->mutex); state->log_identity.clear(); state->log_options = 0; return std::uint64_t{0};
    });
    install(state, "_setlogmask", [state](auto& s, auto&) {
        std::lock_guard lock(state->mutex);
        const auto previous = state->log_mask;
        if (s.x[0]) state->log_mask = static_cast<std::uint32_t>(s.x[0]);
        return static_cast<std::uint64_t>(previous);
    });
    // There is no system log on the host. Messages reach stderr only when the
    // guest asked for LOG_PERROR, as Darwin does in addition to the system log.
    const auto log = [state](transmute::CpuState& s, transmute::GuestMemory& m, std::uint64_t arguments) {
        std::string identity; std::uint32_t options, mask;
        { std::lock_guard lock(state->mutex); identity = state->log_identity; options = state->log_options; mask = state->log_mask; }
        if (!(mask & (1U << (s.x[0] & 7))) || !(options & 0x20)) return std::uint64_t{0};
        auto format = text(m, s.x[1]);
        const auto error = state->errno_value();
        for (std::size_t at = 0; (at = format.find("%m", at)) != std::string::npos;) {
            if (at && format[at - 1] == '%') { at += 2; continue; }
            const auto message = state->guest_string(std::string(256, ' '));
            static_cast<void>(state->call("_strerror_r", {error, message, 256}));
            std::string replacement;
            for (const auto c : text(m, message)) { replacement += c; if (c == '%') replacement += '%'; }
            state->context.release_guest(message);
            format.replace(at, 2, replacement); at += replacement.size();
        }
        if (format.empty() || format.back() != '\n') format += '\n';
        if (!identity.empty()) {
            std::string prefix;
            for (const auto c : identity) { prefix += c; if (c == '%') prefix += '%'; }
            if (options & 1) prefix += "[" + std::to_string(static_cast<std::uint32_t>(state->call("_getpid", {}))) + "]"; // LOG_PID
            format = prefix + ": " + format;
        }
        const auto stream = m.read64(state->symbol("___stderrp"));
        const auto converted = state->guest_string(format);
        try { static_cast<void>(state->call("_vfprintf", {stream, converted, arguments})); }
        catch (...) { state->context.release_guest(converted); throw; }
        state->context.release_guest(converted);
        return std::uint64_t{0};
    };
    for (const auto name : {"_syslog", "_syslog$DARWIN_EXTSN"})
        install(state, name, [log](auto& s, auto& m) { return log(s, m, s.sp); }); // variadic arguments on the stack
    install(state, "_vsyslog", [log](auto& s, auto& m) { return log(s, m, s.x[2]); });
    install(state, "_backtrace_symbols", [state](auto& s, auto& m) {
        const auto count = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[1]));
        if (count <= 0 || count > 4096) throw ExtraError{einval};
        validate(m, s.x[0], static_cast<std::uint64_t>(count) * 8, false);
        const auto images = state->context.images();
        std::vector<std::string> lines;
        std::uint64_t bytes = static_cast<std::uint64_t>(count) * 8;
        for (std::int32_t index = 0; index < count; ++index) {
            const auto address = m.read64(s.x[0] + static_cast<std::uint64_t>(index) * 8);
            std::string module = "???";
            std::uint64_t offset = address;
            for (const auto& image : images)
                if (address >= image.begin && address < image.end) {
                    const auto name = utf8_path(image.name).filename().u8string();
                    module.assign(reinterpret_cast<const char*>(name.data()), name.size()); offset = address - image.begin; break;
                }
            char line[512];
            std::snprintf(line, sizeof(line), "%-4d%-35s 0x%016llx %s + %llu", index, module.c_str(),
                static_cast<unsigned long long>(address), module.c_str(), static_cast<unsigned long long>(offset));
            lines.emplace_back(line);
            bytes += lines.back().size() + 1;
        }
        // One allocation: pointer array followed by the strings, released with a single free().
        const auto block = state->guest_malloc(bytes);
        auto at = block + static_cast<std::uint64_t>(count) * 8;
        for (std::size_t index = 0; index < lines.size(); ++index) {
            m.write64(block + index * 8, at); put_text(m, at, lines[index]); at += lines[index].size() + 1;
        }
        return block;
    }, 0);
}

void install_libc(const std::shared_ptr<State>& state) {
    auto& context = state->context;
    install(state, "_abs", [](auto& s, auto&) {
        const auto value = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[0]));
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(value < 0 ? 0U - static_cast<std::uint32_t>(value) : static_cast<std::uint32_t>(value)));
    });
    for (const auto name : {"_labs", "_llabs", "_imaxabs"}) install(state, name, [](auto& s, auto&) {
        const auto value = std::bit_cast<std::int64_t>(s.x[0]);
        return value < 0 ? 0ULL - s.x[0] : s.x[0];
    });
    install_raw(state, "_div", [](auto& s, auto&) {
        const auto numerator = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[0]));
        const auto denominator = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.x[1]));
        if (!denominator || (numerator == INT32_MIN && denominator == -1)) throw std::runtime_error("Darwin div overflow or division by zero");
        // div_t {int quot; int rem} returns packed in x0.
        s.x[0] = static_cast<std::uint32_t>(numerator / denominator) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(numerator % denominator)) << 32);
    });
    for (const auto name : {"_ldiv", "_lldiv", "_imaxdiv"}) install_raw(state, name, [](auto& s, auto&) {
        const auto numerator = std::bit_cast<std::int64_t>(s.x[0]), denominator = std::bit_cast<std::int64_t>(s.x[1]);
        if (!denominator || (numerator == INT64_MIN && denominator == -1)) throw std::runtime_error("Darwin ldiv overflow or division by zero");
        s.x[0] = std::bit_cast<std::uint64_t>(numerator / denominator); s.x[1] = std::bit_cast<std::uint64_t>(numerator % denominator);
    });
    // Darwin's rand(): the Park-Miller minimal standard generator, RAND_MAX 0x7fffffff.
    const auto next = [](std::uint64_t& seed) {
        std::int64_t value = static_cast<std::int64_t>(seed);
        if (!value) value = 123459876;
        const auto high = value / 127773, low = value % 127773;
        auto result = 16807 * low - 2836 * high;
        if (result < 0) result += 0x7fffffff;
        seed = static_cast<std::uint64_t>(result);
        return static_cast<std::uint64_t>(result % (static_cast<std::int64_t>(0x7fffffff) + 1));
    };
    install(state, "_rand", [state, next](auto&, auto&) { std::lock_guard lock(state->mutex); return next(state->rand_state); });
    install(state, "_srand", [state](auto& s, auto&) { std::lock_guard lock(state->mutex); state->rand_state = static_cast<std::uint32_t>(s.x[0]); return std::uint64_t{0}; });
    install(state, "_rand_r", [next](auto& s, auto& m) {
        validate(m, s.x[0], 4, true);
        std::uint64_t seed = m.read32(s.x[0]);
        const auto value = next(seed);
        m.write32(s.x[0], static_cast<std::uint32_t>(seed));
        return value;
    });
    for (const auto& [alias, target] : std::array<std::pair<const char*, const char*>, 2>{{{"_strtoumax", "_strtoull"}, {"_strtoimax", "_strtoll"}}})
        if (const auto value = context.resolve(system_library, target)) context.install_export(std::string(system_library), alias, *value);
    install_raw(state, "_atof", [](auto& s, auto& m) {
        // The C locale's decimal point; Darwin atof is strtod(nptr, NULL).
        const auto value = text(m, s.x[0]);
        s.v[0] = {std::bit_cast<std::uint64_t>(std::strtod(value.c_str(), nullptr)), 0};
    });
    const auto ascii = [](transmute::GuestMemory& m, std::uint64_t tm) -> std::optional<std::string> {
        validate(m, tm, 36, false);
        static constexpr std::array<const char*, 7> days{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static constexpr std::array<const char*, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        std::array<std::int32_t, 9> field{};
        for (unsigned index = 0; index < field.size(); ++index) field[index] = static_cast<std::int32_t>(m.read32(tm + index * 4));
        if (field[6] < 0 || field[6] > 6 || field[4] < 0 || field[4] > 11 || field[5] > 8099) return std::nullopt;
        char line[64];
        std::snprintf(line, sizeof(line), "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n", days[static_cast<std::size_t>(field[6])],
            months[static_cast<std::size_t>(field[4])], field[3], field[2], field[1], field[0], field[5] + 1900);
        return std::string(line);
    };
    const auto buffer = [state] {
        std::lock_guard lock(state->mutex);
        if (!state->time_buffer) state->time_buffer = state->context.allocate_guest(64);
        return state->time_buffer;
    };
    install(state, "_asctime_r", [ascii](auto& s, auto& m) {
        const auto value = ascii(m, s.x[0]);
        if (!value) throw ExtraError{84}; // EOVERFLOW
        validate(m, s.x[1], 26, true); put_text(m, s.x[1], *value); return s.x[1];
    }, 0);
    install(state, "_asctime", [ascii, buffer](auto& s, auto& m) {
        const auto value = ascii(m, s.x[0]);
        if (!value) throw ExtraError{84};
        const auto at = buffer(); put_text(m, at, *value); return at;
    }, 0);
    const auto local = [state, ascii](transmute::GuestMemory& m, std::uint64_t clock, std::uint64_t output) -> std::uint64_t {
        const auto tm = state->context.allocate_guest(64);
        struct Release { ProcessContext& context; std::uint64_t address; ~Release() { context.release_guest(address); } } release{state->context, tm};
        if (!state->call("_localtime_r", {clock, tm})) throw ExtraError{84};
        const auto value = ascii(m, tm);
        if (!value) throw ExtraError{84};
        put_text(m, output, *value);
        return output;
    };
    install(state, "_ctime_r", [local](auto& s, auto& m) { validate(m, s.x[1], 26, true); return local(m, s.x[0], s.x[1]); }, 0);
    install(state, "_ctime", [local, buffer](auto& s, auto& m) { return local(m, s.x[0], buffer()); }, 0);
    {
        const auto count = context.allocate_guest(8);
        context.calling_engine().write32(count, 107); // ELAST + 1
        context.install_export(std::string(system_library), "_sys_nerr", {count, ExportKind::data});
    }
    // A single virtual user (uid/gid 1000, matching getuid) backed by the host profile.
    const auto record = [state] {
        std::lock_guard lock(state->mutex);
        if (!state->password_record) state->password_record = state->context.allocate_guest(72 + 4096);
        return state->password_record;
    };
    install(state, "_getpwuid", [state, record](auto& s, auto& m) {
        if (static_cast<std::uint32_t>(s.x[0]) != 1000) return std::uint64_t{0};
        const auto at = record();
        if (!state->write_password(m, at, at + 72, 4096)) throw ExtraError{erange};
        return at;
    }, 0);
    install(state, "_getpwnam", [state, record](auto& s, auto& m) {
        if (text(m, s.x[0]) != state->password().name) return std::uint64_t{0};
        const auto at = record();
        if (!state->write_password(m, at, at + 72, 4096)) throw ExtraError{erange};
        return at;
    }, 0);
    for (const auto by_name : {false, true})
        install_code(state, by_name ? "_getpwnam_r" : "_getpwuid_r", [state, by_name](auto& s, auto& m) -> std::uint64_t {
            validate(m, s.x[4], 8, true);
            m.write64(s.x[4], 0);
            const bool selected = by_name ? text(m, s.x[0]) == state->password().name : static_cast<std::uint32_t>(s.x[0]) == 1000;
            if (!selected) return 0;
            if (!state->write_password(m, s.x[1], s.x[2], s.x[3])) return erange;
            m.write64(s.x[4], s.x[1]);
            return 0;
        });
}
} // namespace

void install_darwin_libsystem_extra(ProcessContext& context, SignalMaskAccess masks) {
    if (!masks.get || !masks.set) throw std::invalid_argument("Darwin libSystem extras require signal mask access");
    auto state = std::make_shared<State>(context, std::move(masks));
    install_blocks(state);
    install_jumps(state);
    install_mach(state);
    install_identity(state);
    install_patterns(state);
    install_logging(state);
    install_libc(state);
}
} // namespace alchemy
