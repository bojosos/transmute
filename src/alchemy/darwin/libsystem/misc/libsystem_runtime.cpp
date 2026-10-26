#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin_version.hpp"
#include "alchemy/darwin/security/md4.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <utility>
#include <vector>

// Pure runtime pieces of libSystem a renderer plug-in imports: OSAtomic (libkern), the
// compiler-rt 128-bit integer and powi builtins libSystem re-exports, BSD
// random(3), strndup, strptime, basename/dirname, __vsprintf_chk, the
// CommonCrypto MD4 digest. (CCRandomGenerateBytes is darwin/security's;
// wcspbrk and wcsstr are misc/crt_and_host_info.cpp's.)
namespace alchemy::export_fill {
namespace {

// ---- OSAtomic --------------------------------------------------------------
// alchemy-fx maps guest addresses one-to-one onto host addresses (FxEngine),
// so the host interlocked instruction operates on the guest's own word and is
// atomic with respect to translated guest LDXR/STXR/LSE sequences. The read
// first validates the address (a fault becomes the guest's fault).
template <class T>
T& guest_word(const GuestMemory& memory, std::uint64_t address) {
    if (address % sizeof(T)) throw std::runtime_error("Alchemy OSAtomic: misaligned address");
    if constexpr (sizeof(T) == 4) static_cast<void>(memory.read32(address));
    else static_cast<void>(memory.read64(address));
    return *reinterpret_cast<T*>(address);
}

// ---- 128-bit integers (x0 = low, x1 = high) --------------------------------
struct U128 { std::uint64_t low{}, high{}; };
bool negative(U128 value) { return static_cast<std::int64_t>(value.high) < 0; }
U128 negate(U128 value) {
    U128 result{~value.low + 1, ~value.high};
    if (result.low == 0) ++result.high;
    return result;
}
bool less(U128 a, U128 b) { return a.high != b.high ? a.high < b.high : a.low < b.low; }
U128 subtract(U128 a, U128 b) { return {a.low - b.low, a.high - b.high - (a.low < b.low ? 1 : 0)}; }
// Restoring division; the divisor must be nonzero (a zero divisor traps like compiler-rt).
std::pair<U128, U128> divide(U128 dividend, U128 divisor) {
    if (!divisor.low && !divisor.high) throw std::runtime_error("Alchemy: 128-bit integer division by zero");
    U128 quotient{}, remainder{};
    for (int bit = 127; bit >= 0; --bit) {
        const auto next = (bit >= 64 ? dividend.high >> (bit - 64) : dividend.low >> bit) & 1;
        remainder = {remainder.low << 1 | next, remainder.high << 1 | remainder.low >> 63};
        if (!less(remainder, divisor)) {
            remainder = subtract(remainder, divisor);
            if (bit >= 64) quotient.high |= std::uint64_t{1} << (bit - 64);
            else quotient.low |= std::uint64_t{1} << bit;
        }
    }
    return {quotient, remainder};
}
// Round-to-nearest-even conversion of an unsigned 128-bit magnitude.
double to_double(U128 value) {
    const int length = value.high ? 128 - std::countl_zero(value.high) : 64 - std::countl_zero(value.low);
    if (length <= 53) return static_cast<double>(value.low);
    const int shift = length - 53;
    const auto take = [&](int count) -> std::uint64_t {   // value >> count, low 64 bits
        if (count >= 64) return value.high >> (count - 64);
        return value.low >> count | (count ? value.high << (64 - count) : 0);
    };
    auto mantissa = take(shift) & ((std::uint64_t{1} << 53) - 1);
    // Bits below the kept 53: compare against one half.
    const bool half_bit = ((shift - 1 >= 64 ? value.high >> (shift - 1 - 64) : value.low >> (shift - 1)) & 1) != 0;
    bool sticky = false;
    for (int bit = 0; bit < shift - 1 && !sticky; ++bit)
        sticky = ((bit >= 64 ? value.high >> (bit - 64) : value.low >> bit) & 1) != 0;
    if (half_bit && (sticky || (mantissa & 1))) ++mantissa;
    return std::ldexp(static_cast<double>(mantissa), shift);
}
U128 operand(const CpuState& state, unsigned first) { return {state.x[first], state.x[first + 1]}; }
void result(CpuState& state, U128 value) { state.x[0] = value.low; state.x[1] = value.high; }
void return_double(CpuState& state, double value) { state.v[0] = {std::bit_cast<std::uint64_t>(value), 0}; }
void return_float(CpuState& state, float value) { state.v[0] = {std::bit_cast<std::uint32_t>(value), 0}; }

// ---- BSD random(3): additive feedback generators ---------------------------------
// One generator per state array: the process's own (degree 31, x^31 + x^3 + 1) and each
// array initstate set up, by its size: < 32 bytes the Park-Miller generator alone (type 0),
// < 64 degree 7, < 128 degree 15, < 256 degree 31, else degree 63. setstate switches
// between them; the arrays hold the type in their first word, the tables stay here.
struct Generator {
    std::uint32_t type{3};
    std::size_t separation{3};
    std::vector<std::int32_t> table = std::vector<std::int32_t>(31);
    std::size_t front{3}, rear{0};
    bool seeded{};
    static std::int32_t good_rand(std::int32_t x) {
        // Park-Miller "minimal standard" generator (Schrage's method).
        if (x == 0) x = 123459876;
        const std::int32_t high = x / 127773, low = x % 127773;
        x = 16807 * low - 2836 * high;
        if (x < 0) x += 0x7fffffff;
        return x;
    }
    static Generator of_size(std::uint64_t bytes) {
        Generator g;
        const auto shape = [&g](std::uint32_t type, std::size_t degree, std::size_t separation) {
            g.type = type; g.table.assign(degree, 0); g.separation = separation;
        };
        if (bytes < 32) shape(0, 1, 0);
        else if (bytes < 64) shape(1, 7, 3);
        else if (bytes < 128) shape(2, 15, 1);
        else if (bytes < 256) shape(3, 31, 3);
        else shape(4, 63, 1);
        return g;
    }
    std::int32_t next() {
        if (!seeded) seed(1);
        if (type == 0) return table[0] = good_rand(table[0]) & 0x7fffffff;
        table[front] = static_cast<std::int32_t>(static_cast<std::uint32_t>(table[front]) + static_cast<std::uint32_t>(table[rear]));
        const auto value = static_cast<std::int32_t>((static_cast<std::uint32_t>(table[front]) >> 1) & 0x7fffffff);
        front = (front + 1) % table.size();
        rear = (rear + 1) % table.size();
        return value;
    }
    void seed(std::uint32_t value) {
        table[0] = static_cast<std::int32_t>(value);
        seeded = true;
        if (type == 0) return;
        for (std::size_t index = 1; index < table.size(); ++index) table[index] = good_rand(table[index - 1]);
        front = separation; rear = 0;
        for (std::size_t index = 0; index < 10 * table.size(); ++index) static_cast<void>(next());
    }
};

struct Random {
    std::mutex mutex;
    std::map<std::uint64_t, Generator> generators;   // state array -> its generator
    std::uint64_t current{};                         // the state array in use
    std::int32_t next() { std::lock_guard lock(mutex); return generators[current].next(); }
    void seed(std::uint32_t value) { std::lock_guard lock(mutex); generators[current].seed(value); }
};

// ---- strptime ("C" locale) --------------------------------------------------
constexpr const char* day_names[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
constexpr const char* month_names[] = {"january", "february", "march", "april", "may", "june", "july", "august",
    "september", "october", "november", "december"};

struct Tm { std::int32_t sec{}, min{}, hour{}, mday{}, mon{}, year{}, wday{}, yday{}, isdst{}; std::int64_t gmtoff{}; };

class TimeParser {
public:
    TimeParser(std::string_view input, Tm& tm, std::uint32_t& set) : in_(input), tm_(tm), set_(set) {}
    std::size_t position() const noexcept { return at_; }
    bool parse(std::string_view format) {
        for (std::size_t index = 0; index < format.size(); ++index) {
            const char c = format[index];
            if (std::isspace(static_cast<unsigned char>(c))) { skip_space(); continue; }
            if (c != '%') { if (at_ >= in_.size() || in_[at_] != c) return false; ++at_; continue; }
            if (++index >= format.size()) return false;
            char conversion = format[index];
            if (conversion == 'E' || conversion == 'O') { if (++index >= format.size()) return false; conversion = format[index]; }
            if (!field(conversion)) return false;
        }
        return true;
    }

private:
    void skip_space() { while (at_ < in_.size() && std::isspace(static_cast<unsigned char>(in_[at_]))) ++at_; }
    bool number(int digits, int minimum, int maximum, std::int32_t& out) {
        skip_space();
        bool sign = false;
        if (at_ < in_.size() && (in_[at_] == '+' || in_[at_] == '-') && minimum < 0) sign = in_[at_++] == '-';
        int value = 0, count = 0;
        while (count < digits && at_ < in_.size() && std::isdigit(static_cast<unsigned char>(in_[at_]))) {
            value = value * 10 + (in_[at_++] - '0');
            ++count;
        }
        if (!count) return false;
        if (sign) value = -value;
        if (value < minimum || value > maximum) return false;
        out = value;
        return true;
    }
    bool name(const char* const* names, int count, std::int32_t& out) {
        skip_space();
        for (int index = 0; index < count; ++index) {
            const std::string_view full(names[index]);
            for (const auto length : {full.size(), std::size_t{3}}) {
                if (at_ + length > in_.size()) continue;
                bool match = true;
                for (std::size_t i = 0; i < length && match; ++i)
                    match = std::tolower(static_cast<unsigned char>(in_[at_ + i])) == full[i];
                if (match) { at_ += length; out = index; return true; }
            }
        }
        return false;
    }
    bool field(char conversion) {
        std::int32_t value{};
        switch (conversion) {
        case '%': if (at_ >= in_.size() || in_[at_] != '%') return false; ++at_; return true;
        case 'n': case 't': skip_space(); return true;
        case 'a': case 'A': if (!name(day_names, 7, tm_.wday)) return false; set_ |= 1; return true;
        case 'b': case 'B': case 'h': return name(month_names, 12, tm_.mon);
        case 'C': if (!number(2, 0, 99, value)) return false; century_ = value; apply_year(); return true;
        case 'd': case 'e': return number(2, 1, 31, tm_.mday);
        case 'D': return parse("%m/%d/%y");
        case 'F': return parse("%Y-%m-%d");
        case 'H': case 'k': return number(2, 0, 23, tm_.hour);
        case 'I': case 'l': if (!number(2, 1, 12, value)) return false; tm_.hour = value % 12; return true;
        case 'j': if (!number(3, 1, 366, value)) return false; tm_.yday = value - 1; return true;
        case 'm': if (!number(2, 1, 12, value)) return false; tm_.mon = value - 1; return true;
        case 'M': return number(2, 0, 59, tm_.min);
        case 'p': {
            skip_space();
            if (at_ + 2 > in_.size()) return false;
            const auto a = std::tolower(static_cast<unsigned char>(in_[at_])), b = std::tolower(static_cast<unsigned char>(in_[at_ + 1]));
            if (b != 'm' || (a != 'a' && a != 'p')) return false;
            at_ += 2;
            if (a == 'p' && tm_.hour < 12) tm_.hour += 12;
            if (a == 'a' && tm_.hour >= 12) tm_.hour -= 12;
            return true;
        }
        case 'r': return parse("%I:%M:%S %p");
        case 'R': return parse("%H:%M");
        case 'S': return number(2, 0, 60, tm_.sec);
        case 'T': case 'X': return parse("%H:%M:%S");
        case 'x': return parse("%m/%d/%y");
        case 'c': return parse("%a %b %e %H:%M:%S %Y");
        case 'u': if (!number(1, 1, 7, value)) return false; tm_.wday = value % 7; return true;
        case 'w': return number(1, 0, 6, tm_.wday);
        case 'U': case 'W': case 'V': return number(2, 0, 53, value);   // parsed, not applied (as Darwin)
        case 'y': if (!number(2, 0, 99, value)) return false; year2_ = value; apply_year(); return true;
        case 'Y': if (!number(4, -9999, 9999, value)) return false; tm_.year = value - 1900; return true;
        case 'Z': {
            skip_space();
            const auto start = at_;
            while (at_ < in_.size() && std::isalpha(static_cast<unsigned char>(in_[at_]))) ++at_;
            const auto zone = in_.substr(start, at_ - start);
            if (zone == "GMT" || zone == "UTC" || zone == "Z") { tm_.isdst = 0; tm_.gmtoff = 0; return true; }
            return !zone.empty();
        }
        case 'z': {
            skip_space();
            if (at_ < in_.size() && (in_[at_] == 'Z' || in_[at_] == 'z')) { ++at_; tm_.gmtoff = 0; return true; }
            if (at_ >= in_.size() || (in_[at_] != '+' && in_[at_] != '-')) return false;
            const bool minus = in_[at_++] == '-';
            int digits = 0, hours = 0, minutes = 0;
            while (digits < 4 && at_ < in_.size()) {
                if (in_[at_] == ':' && digits == 2) { ++at_; continue; }
                if (!std::isdigit(static_cast<unsigned char>(in_[at_]))) break;
                (digits < 2 ? hours : minutes) = (digits < 2 ? hours : minutes) * 10 + (in_[at_++] - '0');
                ++digits;
            }
            if (digits != 2 && digits != 4) return false;
            if (hours > 14 || minutes > 59) return false;
            tm_.gmtoff = (minus ? -1 : 1) * (hours * 3600 + minutes * 60);
            return true;
        }
        case 's': {
            skip_space();
            std::int64_t seconds = 0;
            bool any = false, minus = false;
            if (at_ < in_.size() && in_[at_] == '-') { minus = true; ++at_; }
            while (at_ < in_.size() && std::isdigit(static_cast<unsigned char>(in_[at_]))) { seconds = seconds * 10 + (in_[at_++] - '0'); any = true; }
            if (!any) return false;
            if (minus) seconds = -seconds;
            // Civil-from-days (UTC).
            auto days = seconds / 86400; auto rest = seconds % 86400;
            if (rest < 0) { rest += 86400; --days; }
            tm_.hour = static_cast<std::int32_t>(rest / 3600); tm_.min = static_cast<std::int32_t>(rest / 60 % 60); tm_.sec = static_cast<std::int32_t>(rest % 60);
            tm_.wday = static_cast<std::int32_t>(((days % 7) + 11) % 7);
            const auto z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
            const auto doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
            const auto d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
            const auto y = yoe + era * 400 + (m <= 2 ? 1 : 0);
            tm_.year = static_cast<std::int32_t>(y - 1900); tm_.mon = static_cast<std::int32_t>(m - 1); tm_.mday = static_cast<std::int32_t>(d);
            tm_.gmtoff = 0;
            return true;
        }
        default:
            throw std::runtime_error(std::string("Alchemy strptime: unsupported conversion %") + conversion);
        }
    }
    void apply_year() {
        if (century_ >= 0) tm_.year = century_ * 100 + (year2_ >= 0 ? year2_ : 0) - 1900;
        else if (year2_ >= 0) tm_.year = year2_ < 69 ? year2_ + 100 : year2_;
    }
    std::string_view in_;
    std::size_t at_{};
    Tm& tm_;
    std::uint32_t& set_;
    int century_{-1}, year2_{-1};
};

std::vector<std::uint32_t> wide_string(const GuestMemory& memory, std::uint64_t address) {
    if (!address) throw std::runtime_error("Alchemy: null wide string");
    std::vector<std::uint32_t> value;
    for (std::uint64_t offset = 0;; offset += 4) {
        if (value.size() > (1U << 20)) throw std::runtime_error("Alchemy: wide string exceeds 1 Mi characters");
        const auto character = memory.read32(address + offset);
        if (!character) return value;
        value.push_back(character);
    }
}

std::string base_name(std::string path) {
    if (path.empty()) return ".";
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (path == "/") return path;
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string directory_name(std::string path) {
    if (path.empty()) return ".";
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) return ".";
    auto end = slash;
    while (end > 0 && path[end - 1] == '/') --end;
    return end == 0 ? "/" : path.substr(0, end);
}

} // namespace

std::string test_base_name(std::string path) { return base_name(std::move(path)); }
std::string test_directory_name(std::string path) { return directory_name(std::move(path)); }
double test_floattidf(std::uint64_t low, std::uint64_t high) {
    U128 value{low, high};
    return negative(value) ? -to_double(negate(value)) : to_double(value);
}
std::pair<std::uint64_t, std::uint64_t> test_udivmod(std::uint64_t al, std::uint64_t ah, std::uint64_t bl, std::uint64_t bh, bool remainder) {
    const auto [q, r] = divide({al, ah}, {bl, bh});
    return remainder ? std::pair{r.low, r.high} : std::pair{q.low, q.high};
}

void install_libsystem_runtime(Provider& p) {
    auto& process = p.process();

    // libkern OSAtomic: sequentially consistent read-modify-writes on the guest
    // word (the Barrier variants are full fences, and so is every one here).
    const auto add32 = [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto amount = static_cast<std::int32_t>(s.x[0]);
        std::atomic_ref word(guest_word<std::int32_t>(m, s.x[1]));
        return static_cast<std::uint32_t>(word.fetch_add(amount, std::memory_order_seq_cst)) + static_cast<std::uint32_t>(amount);
    };
    const auto add64 = [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto amount = static_cast<std::int64_t>(s.x[0]);
        std::atomic_ref word(guest_word<std::int64_t>(m, s.x[1]));
        return static_cast<std::uint64_t>(word.fetch_add(amount, std::memory_order_seq_cst)) + static_cast<std::uint64_t>(amount);
    };
    const auto cas32 = [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::atomic_ref word(guest_word<std::int32_t>(m, s.x[2]));
        auto expected = static_cast<std::int32_t>(s.x[0]);
        return word.compare_exchange_strong(expected, static_cast<std::int32_t>(s.x[1]), std::memory_order_seq_cst) ? 1 : 0;
    };
    const auto cas64 = [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::atomic_ref word(guest_word<std::int64_t>(m, s.x[2]));
        auto expected = static_cast<std::int64_t>(s.x[0]);
        return word.compare_exchange_strong(expected, static_cast<std::int64_t>(s.x[1]), std::memory_order_seq_cst) ? 1 : 0;
    };
    // OSAtomic{Or,And,Xor}32[Orig][Barrier](mask, address): the plain forms
    // return the new value, the Orig forms the old one.
    const auto bits32 = [&p](std::string_view operation, auto apply) {
        for (const auto original : {false, true})
            for (const auto barrier : {false, true}) {
                const auto name = "_OSAtomic" + std::string(operation) + "32" + (original ? "Orig" : "") + (barrier ? "Barrier" : "");
                p.libsystem(name.c_str(), [apply, original](CpuState& s, GuestMemory& m) -> std::uint64_t {
                    const auto mask = static_cast<std::uint32_t>(s.x[0]);
                    std::atomic_ref word(guest_word<std::uint32_t>(m, s.x[1]));
                    auto old = word.load(std::memory_order_relaxed);
                    while (!word.compare_exchange_weak(old, apply(old, mask), std::memory_order_seq_cst)) {}
                    return original ? old : apply(old, mask);
                });
            }
    };
    bits32("Or", [](std::uint32_t value, std::uint32_t mask) { return value | mask; });
    bits32("And", [](std::uint32_t value, std::uint32_t mask) { return value & mask; });
    bits32("Xor", [](std::uint32_t value, std::uint32_t mask) { return value ^ mask; });
    for (const auto* name : {"_OSAtomicAdd32", "_OSAtomicAdd32Barrier"}) p.libsystem(name, add32);
    for (const auto* name : {"_OSAtomicAdd64", "_OSAtomicAdd64Barrier"}) p.libsystem(name, add64);
    for (const auto* name : {"_OSAtomicCompareAndSwap32", "_OSAtomicCompareAndSwap32Barrier",
                             "_OSAtomicCompareAndSwapInt", "_OSAtomicCompareAndSwapIntBarrier"}) p.libsystem(name, cas32);
    for (const auto* name : {"_OSAtomicCompareAndSwap64", "_OSAtomicCompareAndSwap64Barrier", "_OSAtomicCompareAndSwapLong",
                             "_OSAtomicCompareAndSwapLongBarrier", "_OSAtomicCompareAndSwapPtr", "_OSAtomicCompareAndSwapPtrBarrier"})
        p.libsystem(name, cas64);
    p.libsystem("_OSMemoryBarrier", [](CpuState&, GuestMemory&) -> std::uint64_t {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return 0;
    });

    // compiler-rt builtins re-exported by libSystem (x0:x1 and x2:x3 operands).
    const auto ti = [&p](const char* name, auto operation) {
        if (provided(p.process(), libsystem_library, name)) return;
        static_cast<void>(p.process().install_function(name, [operation](CpuState& s, GuestMemory&) { operation(s); },
                                                       std::string(libsystem_library)));
    };
    // ___udivti3: misc/numeric_math.cpp.
    ti("___umodti3", [](CpuState& s) { result(s, divide(operand(s, 0), operand(s, 2)).second); });
    ti("___divti3", [](CpuState& s) {
        const auto a = operand(s, 0), b = operand(s, 2);
        auto q = divide(negative(a) ? negate(a) : a, negative(b) ? negate(b) : b).first;
        result(s, negative(a) != negative(b) ? negate(q) : q);
    });
    ti("___modti3", [](CpuState& s) {
        const auto a = operand(s, 0), b = operand(s, 2);
        auto r = divide(negative(a) ? negate(a) : a, negative(b) ? negate(b) : b).second;
        result(s, negative(a) ? negate(r) : r);   // the remainder takes the dividend's sign
    });
    ti("___floattidf", [](CpuState& s) {
        const auto a = operand(s, 0);
        return_double(s, negative(a) ? -to_double(negate(a)) : to_double(a));
    });
    ti("___floatuntidf", [](CpuState& s) { return_double(s, to_double(operand(s, 0))); });
    // double __powidf2(double a, int b), float __powisf2(float a, int b): compiler-rt's
    // square-and-multiply in the argument's own precision.
    ti("___powidf2", [](CpuState& s) {
        auto a = std::bit_cast<double>(s.v[0][0]);
        auto b = static_cast<std::int32_t>(s.x[0]);
        const bool reciprocal = b < 0;
        double r = 1;
        for (;;) { if (b & 1) r *= a; b /= 2; if (b == 0) break; a *= a; }
        return_double(s, reciprocal ? 1 / r : r);
    });
    ti("___powisf2", [](CpuState& s) {
        auto a = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]));
        auto b = static_cast<std::int32_t>(s.x[0]);
        const bool reciprocal = b < 0;
        float r = 1;
        for (;;) { if (b & 1) r *= a; b /= 2; if (b == 0) break; a *= a; }
        return_float(s, reciprocal ? 1 / r : r);
    });

    // ---- more compiler-rt builtins (x0:x1 and x2:x3 are 128-bit operands, little end first) ----
    const auto ti_memory = [&p](const char* name, auto operation) {
        if (provided(p.process(), libsystem_library, name)) return;
        static_cast<void>(p.process().install_function(name, [operation](CpuState& s, GuestMemory& m) { operation(s, m); },
                                                       std::string(libsystem_library)));
    };
    const auto shift_left = [](U128 a, unsigned count) -> U128 {
        count &= 127;
        if (count == 0) return a;
        if (count >= 64) return {0, a.low << (count - 64)};
        return {a.low << count, a.high << count | a.low >> (64 - count)};
    };
    const auto shift_right = [](U128 a, unsigned count, bool arithmetic) -> U128 {
        count &= 127;
        if (count == 0) return a;
        const std::uint64_t fill = arithmetic && negative(a) ? ~0ULL : 0;
        if (count >= 64) return {count == 64 ? a.high : (arithmetic ? static_cast<std::uint64_t>(static_cast<std::int64_t>(a.high) >> (count - 64)) : a.high >> (count - 64)), fill};
        return {a.low >> count | a.high << (64 - count),
                arithmetic ? static_cast<std::uint64_t>(static_cast<std::int64_t>(a.high) >> count) : a.high >> count};
    };
    const auto add = [](U128 a, U128 b) -> U128 { const auto low = a.low + b.low; return {low, a.high + b.high + (low < a.low ? 1 : 0)}; };
    // The low 128 bits of the unsigned product a * b; *overflow: whether it needs more.
    const auto multiply = [](U128 a, U128 b, bool* overflow) -> U128 {
        const auto mul64 = [](std::uint64_t x, std::uint64_t y) -> U128 {
            const std::uint64_t x0 = x & 0xffffffff, x1 = x >> 32, y0 = y & 0xffffffff, y1 = y >> 32;
            const auto p00 = x0 * y0, p01 = x0 * y1, p10 = x1 * y0, p11 = x1 * y1;
            const auto middle = (p00 >> 32) + (p01 & 0xffffffff) + (p10 & 0xffffffff);
            return {(middle << 32) | (p00 & 0xffffffff), p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32)};
        };
        const auto product = mul64(a.low, b.low), cross1 = mul64(a.low, b.high), cross2 = mul64(a.high, b.low);
        auto high = product.high;
        bool carry = false;
        high += cross1.low; carry = carry || high < cross1.low;
        high += cross2.low; carry = carry || high < cross2.low;
        if (overflow) *overflow = carry || (a.high && b.high) || cross1.high || cross2.high;
        return {product.low, high};
    };
    ti("___multi3", [multiply](CpuState& s) { result(s, multiply(operand(s, 0), operand(s, 2), nullptr)); });
    ti("___ashlti3", [shift_left](CpuState& s) { result(s, shift_left(operand(s, 0), static_cast<unsigned>(s.x[2]))); });
    ti("___ashrti3", [shift_right](CpuState& s) { result(s, shift_right(operand(s, 0), static_cast<unsigned>(s.x[2]), true)); });
    ti("___lshrti3", [shift_right](CpuState& s) { result(s, shift_right(operand(s, 0), static_cast<unsigned>(s.x[2]), false)); });
    ti("___negti2", [](CpuState& s) { result(s, negate(operand(s, 0))); });
    // __udivmodti4(a, b, rem*): the quotient; *rem (x4) the remainder.
    ti_memory("___udivmodti4", [](CpuState& s, GuestMemory& m) {
        const auto [q, r] = divide(operand(s, 0), operand(s, 2));
        if (s.x[4]) { m.write64(s.x[4], r.low); m.write64(s.x[4] + 8, r.high); }
        result(s, q);
    });
    // Integer bit counts on 128-bit values.
    ti("___clzti2", [](CpuState& s) { const auto a = operand(s, 0); s.x[0] = a.high ? std::countl_zero(a.high) : 64 + std::countl_zero(a.low); });
    ti("___ctzti2", [](CpuState& s) { const auto a = operand(s, 0); s.x[0] = a.low ? std::countr_zero(a.low) : 64 + std::countr_zero(a.high); });
    ti("___popcountti2", [](CpuState& s) { const auto a = operand(s, 0); s.x[0] = std::popcount(a.low) + std::popcount(a.high); });
    ti("___ffsti2", [](CpuState& s) {
        const auto a = operand(s, 0);
        s.x[0] = a.low ? std::countr_zero(a.low) + 1 : a.high ? 64 + std::countr_zero(a.high) + 1 : 0;
    });
    // __cmpti2 / __ucmpti2: 0 less, 1 equal, 2 greater.
    ti("___cmpti2", [](CpuState& s) {
        const auto a = operand(s, 0), b = operand(s, 2);
        const auto ah = static_cast<std::int64_t>(a.high), bh = static_cast<std::int64_t>(b.high);
        s.x[0] = ah < bh ? 0 : ah > bh ? 2 : a.low < b.low ? 0 : a.low > b.low ? 2 : 1;
    });
    ti("___ucmpti2", [](CpuState& s) {
        const auto a = operand(s, 0), b = operand(s, 2);
        s.x[0] = less(a, b) ? 0 : less(b, a) ? 2 : 1;
    });
    // 64-bit division and byte/bit helpers.
    ti("___udivdi3", [](CpuState& s) {
        if (!s.x[1]) throw std::runtime_error("Alchemy: 64-bit integer division by zero");
        s.x[0] = s.x[0] / s.x[1];
    });
    ti("___divdi3", [](CpuState& s) {
        if (!s.x[1]) throw std::runtime_error("Alchemy: 64-bit integer division by zero");
        s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(s.x[0]) / static_cast<std::int64_t>(s.x[1]));
    });
    ti("___bswapsi2", [](CpuState& s) {
        const auto v = static_cast<std::uint32_t>(s.x[0]);
        s.x[0] = (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
    });
    ti("___bswapdi2", [](CpuState& s) {
        std::uint64_t v = s.x[0], swapped = 0;
        for (int index = 0; index < 8; ++index) { swapped = (swapped << 8) | (v & 0xff); v >>= 8; }
        s.x[0] = swapped;
    });
    ti("___paritysi2", [](CpuState& s) { s.x[0] = std::popcount(static_cast<std::uint32_t>(s.x[0])) & 1; });
    // Overflow-checked arithmetic: the *o forms report overflow; the *v forms trap (compiler-rt aborts).
    // __mulodi4(a, b, int* overflow) / __muloti4(a, b, int* overflow): the product; *overflow set when
    // the signed result does not fit.
    const auto signed_multiply = [multiply](U128 a, U128 b, bool& overflow) {
        const bool flip = negative(a) != negative(b);
        auto magnitude = multiply(negative(a) ? negate(a) : a, negative(b) ? negate(b) : b, &overflow);
        const bool minimum = magnitude.high == 0x8000000000000000ULL && magnitude.low == 0;
        if (negative(magnitude) && !(flip && minimum)) overflow = true;
        return flip ? negate(magnitude) : magnitude;
    };
    ti_memory("___muloti4", [signed_multiply](CpuState& s, GuestMemory& m) {
        bool overflow = false;
        result(s, signed_multiply(operand(s, 0), operand(s, 2), overflow));
        if (s.x[4]) m.write32(s.x[4], overflow ? 1 : 0);
    });
    ti_memory("___mulodi4", [signed_multiply](CpuState& s, GuestMemory& m) {
        const auto widen = [](std::uint64_t value) { return U128{value, static_cast<std::int64_t>(value) < 0 ? ~0ULL : 0}; };
        bool overflow = false;
        const auto product = signed_multiply(widen(s.x[0]), widen(s.x[1]), overflow);
        // The 64-bit result overflows when the 128-bit one is not its sign extension.
        overflow = overflow || product.high != (static_cast<std::int64_t>(product.low) < 0 ? ~0ULL : 0);
        s.x[0] = product.low;
        if (s.x[2]) m.write32(s.x[2], overflow ? 1 : 0);
    });
    const auto signed_overflow = [](const char* what) { throw std::runtime_error(std::string("Alchemy: 128-bit integer overflow in ") + what); };
    ti("___absvti2", [signed_overflow](CpuState& s) {
        const auto a = operand(s, 0);
        if (a.high == 0x8000000000000000ULL && a.low == 0) signed_overflow("__absvti2");
        result(s, negative(a) ? negate(a) : a);
    });
    ti("___negvti2", [signed_overflow](CpuState& s) {
        const auto a = operand(s, 0);
        if (a.high == 0x8000000000000000ULL && a.low == 0) signed_overflow("__negvti2");
        result(s, negate(a));
    });
    ti("___addvti3", [add, signed_overflow](CpuState& s) {
        const auto a = operand(s, 0), b = operand(s, 2), sum = add(a, b);
        if (negative(a) == negative(b) && negative(sum) != negative(a)) signed_overflow("__addvti3");
        result(s, sum);
    });
    ti("___subvti3", [add, signed_overflow](CpuState& s) {
        const auto a = operand(s, 0), b = operand(s, 2), difference = add(a, negate(b));
        if (negative(a) != negative(b) && negative(difference) != negative(a)) signed_overflow("__subvti3");
        result(s, difference);
    });
    ti("___mulvti3", [signed_multiply, signed_overflow](CpuState& s) {
        bool overflow = false;
        const auto product = signed_multiply(operand(s, 0), operand(s, 2), overflow);
        if (overflow) signed_overflow("__mulvti3");
        result(s, product);
    });
    // Float <-> 128-bit integer conversions (truncating toward zero; out of range saturates as compiler-rt does).
    const auto to_u128 = [](double value) -> U128 {
        if (!(value > 0)) return {};
        if (value >= 0x1p128) return {~0ULL, ~0ULL};
        if (value < 0x1p64) return {static_cast<std::uint64_t>(value), 0};
        const auto high = static_cast<std::uint64_t>(value / 0x1p64);
        return {static_cast<std::uint64_t>(value - static_cast<double>(high) * 0x1p64), high};
    };
    const auto to_i128 = [to_u128](double value) -> U128 {
        if (std::isnan(value)) return {};
        if (value >= 0x1p127) return {~0ULL, 0x7fffffffffffffffULL};
        if (value <= -0x1p127) return {0, 0x8000000000000000ULL};
        return value < 0 ? negate(to_u128(-value)) : to_u128(value);
    };
    ti("___fixdfti", [to_i128](CpuState& s) { result(s, to_i128(std::bit_cast<double>(s.v[0][0]))); });
    ti("___fixsfti", [to_i128](CpuState& s) { result(s, to_i128(std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])))); });
    ti("___fixunsdfti", [to_u128](CpuState& s) { result(s, to_u128(std::bit_cast<double>(s.v[0][0]))); });
    ti("___fixunssfti", [to_u128](CpuState& s) { result(s, to_u128(std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])))); });
    ti("___floattisf", [](CpuState& s) {
        const auto a = operand(s, 0);
        // Through double: 128-bit values round twice only when the double is exactly halfway, which a
        // 53-bit mantissa cannot be for a float's 24.
        return_float(s, static_cast<float>(negative(a) ? -to_double(negate(a)) : to_double(a)));
    });
    ti("___floatuntisf", [](CpuState& s) { return_float(s, static_cast<float>(to_double(operand(s, 0)))); });
    // IEEE half precision (binary16): _Float16 in h0 (the low 16 bits of v0), or a uint16 in w0 (__gnu_*).
    const auto half_to_float = [](std::uint16_t h) -> float {
        const std::uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
        if (exponent == 0x1f) return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13));
        if (exponent == 0) {
            if (!mantissa) return std::bit_cast<float>(sign);
            return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24);
        }
        return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
    };
    // double -> half, rounding to nearest even once.
    const auto double_to_half = [](double value) -> std::uint16_t {
        const auto bits = std::bit_cast<std::uint64_t>(value);
        const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 48) & 0x8000);
        const double magnitude = std::fabs(value);
        if (std::isnan(value)) return static_cast<std::uint16_t>(sign | 0x7e00);
        if (magnitude >= 65520.0) return static_cast<std::uint16_t>(sign | 0x7c00);   // rounds to infinity
        if (magnitude < 0x1p-14) {
            // Subnormal: units of 2^-24, rounded to nearest even.
            const double units = magnitude * 0x1p24;
            auto whole = static_cast<std::uint32_t>(units);
            const double rest = units - whole;
            if (rest > 0.5 || (rest == 0.5 && (whole & 1))) ++whole;
            return static_cast<std::uint16_t>(sign | whole);
        }
        int exponent{};
        const double fraction = std::frexp(magnitude, &exponent);   // [0.5, 1)
        const double units = (fraction * 2 - 1) * 1024;              // the 10 mantissa bits
        auto whole = static_cast<std::uint32_t>(units);
        const double rest = units - whole;
        if (rest > 0.5 || (rest == 0.5 && (whole & 1))) ++whole;
        std::uint32_t biased = static_cast<std::uint32_t>(exponent - 1 + 15);
        if (whole == 1024) { whole = 0; ++biased; }
        if (biased >= 31) return static_cast<std::uint16_t>(sign | 0x7c00);
        return static_cast<std::uint16_t>(sign | (biased << 10) | whole);
    };
    ti("___extendhfsf2", [half_to_float](CpuState& s) { return_float(s, half_to_float(static_cast<std::uint16_t>(s.v[0][0]))); });
    ti("___extendhfdf2", [half_to_float](CpuState& s) { return_double(s, half_to_float(static_cast<std::uint16_t>(s.v[0][0]))); });
    ti("___truncsfhf2", [double_to_half](CpuState& s) {
        s.v[0] = {double_to_half(std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]))), 0};
    });
    ti("___truncdfhf2", [double_to_half](CpuState& s) { s.v[0] = {double_to_half(std::bit_cast<double>(s.v[0][0])), 0}; });
    ti("___gnu_h2f_ieee", [half_to_float](CpuState& s) { return_float(s, half_to_float(static_cast<std::uint16_t>(s.x[0]))); });
    ti("___gnu_f2h_ieee", [double_to_half](CpuState& s) { s.x[0] = double_to_half(std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]))); });
    // Complex multiplication and division (C Annex G: infinities and NaNs recovered), double in
    // d0-d3 -> d0:d1, float in s0-s3 -> s0:s1.
    const auto complex_multiply = [](double a, double b, double c, double d) -> std::pair<double, double> {
        const double ac = a * c, bd = b * d, ad = a * d, bc = b * c;
        double x = ac - bd, y = ad + bc;
        if (std::isnan(x) && std::isnan(y)) {
            bool recalc = false;
            const auto box = [](double& value, bool infinite) { value = std::copysign(infinite ? 1.0 : 0.0, value); };
            if (std::isinf(a) || std::isinf(b)) {
                box(a, std::isinf(a)); box(b, std::isinf(b));
                if (std::isnan(c)) c = std::copysign(0.0, c);
                if (std::isnan(d)) d = std::copysign(0.0, d);
                recalc = true;
            }
            if (std::isinf(c) || std::isinf(d)) {
                box(c, std::isinf(c)); box(d, std::isinf(d));
                if (std::isnan(a)) a = std::copysign(0.0, a);
                if (std::isnan(b)) b = std::copysign(0.0, b);
                recalc = true;
            }
            if (!recalc && (std::isinf(ac) || std::isinf(bd) || std::isinf(ad) || std::isinf(bc))) {
                if (std::isnan(a)) a = std::copysign(0.0, a);
                if (std::isnan(b)) b = std::copysign(0.0, b);
                if (std::isnan(c)) c = std::copysign(0.0, c);
                if (std::isnan(d)) d = std::copysign(0.0, d);
                recalc = true;
            }
            if (recalc) {
                x = INFINITY * (a * c - b * d);
                y = INFINITY * (a * d + b * c);
            }
        }
        return {x, y};
    };
    const auto complex_divide = [](double a, double b, double c, double d) -> std::pair<double, double> {
        int scale = 0;
        const double logb = std::logb(std::fmax(std::fabs(c), std::fabs(d)));
        if (std::isfinite(logb)) { scale = -static_cast<int>(logb); c = std::scalbn(c, scale); d = std::scalbn(d, scale); }
        const double denominator = c * c + d * d;
        double x = std::scalbn((a * c + b * d) / denominator, scale), y = std::scalbn((b * c - a * d) / denominator, scale);
        if (std::isnan(x) && std::isnan(y)) {
            if (denominator == 0.0 && (!std::isnan(a) || !std::isnan(b))) {
                x = std::copysign(INFINITY, c) * a;
                y = std::copysign(INFINITY, c) * b;
            } else if ((std::isinf(a) || std::isinf(b)) && std::isfinite(c) && std::isfinite(d)) {
                a = std::copysign(std::isinf(a) ? 1.0 : 0.0, a);
                b = std::copysign(std::isinf(b) ? 1.0 : 0.0, b);
                x = INFINITY * (a * c + b * d);
                y = INFINITY * (b * c - a * d);
            } else if (std::isinf(logb) && logb > 0 && std::isfinite(a) && std::isfinite(b)) {
                c = std::copysign(std::isinf(c) ? 1.0 : 0.0, c);
                d = std::copysign(std::isinf(d) ? 1.0 : 0.0, d);
                x = 0.0 * (a * c + b * d);
                y = 0.0 * (b * c - a * d);
            }
        }
        return {x, y};
    };
    const auto d = [](const CpuState& s, unsigned index) { return std::bit_cast<double>(s.v[index][0]); };
    const auto f = [](const CpuState& s, unsigned index) { return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(s.v[index][0]))); };
    ti("___muldc3", [complex_multiply, d](CpuState& s) {
        const auto [x, y] = complex_multiply(d(s, 0), d(s, 1), d(s, 2), d(s, 3));
        s.v[0] = {std::bit_cast<std::uint64_t>(x), 0}; s.v[1] = {std::bit_cast<std::uint64_t>(y), 0};
    });
    ti("___divdc3", [complex_divide, d](CpuState& s) {
        const auto [x, y] = complex_divide(d(s, 0), d(s, 1), d(s, 2), d(s, 3));
        s.v[0] = {std::bit_cast<std::uint64_t>(x), 0}; s.v[1] = {std::bit_cast<std::uint64_t>(y), 0};
    });
    ti("___mulsc3", [complex_multiply, f](CpuState& s) {
        const auto [x, y] = complex_multiply(f(s, 0), f(s, 1), f(s, 2), f(s, 3));
        s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(x)), 0}; s.v[1] = {std::bit_cast<std::uint32_t>(static_cast<float>(y)), 0};
    });
    ti("___divsc3", [complex_divide, f](CpuState& s) {
        const auto [x, y] = complex_divide(f(s, 0), f(s, 1), f(s, 2), f(s, 3));
        s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(x)), 0}; s.v[1] = {std::bit_cast<std::uint32_t>(static_cast<float>(y)), 0};
    });
    // int32_t __isOSVersionAtLeast(major, minor, subminor): against the macOS version Alchemy reports.
    ti("___isOSVersionAtLeast", [](CpuState& s) {
        const std::array<std::int32_t, 3> wanted{static_cast<std::int32_t>(s.x[0]), static_cast<std::int32_t>(s.x[1]), static_cast<std::int32_t>(s.x[2])};
        std::array<std::int32_t, 3> have{};
        std::size_t index = 0;
        for (const auto c : macos_product_version) {
            if (c == '.') { if (++index == have.size()) break; continue; }
            have[index] = have[index] * 10 + (c - '0');
        }
        s.x[0] = have >= wanted ? 1 : 0;
    });
    // void __enable_execute_stack(void*): guest stacks need no change for trampolines here.
    ti("___enable_execute_stack", [](CpuState&) {});

    // BSD random(3).
    auto generator = std::make_shared<Random>();
    generator->current = process.allocate_guest(128);   // the process's own state array (degree 31)
    process.main_engine().fill_bytes(generator->current, 0, 128);
    generator->generators[generator->current] = Generator::of_size(128);
    p.libsystem("_random", [generator](CpuState&, GuestMemory&) -> std::uint64_t { return static_cast<std::uint64_t>(generator->next()); });
    // char* initstate(unsigned seed, char* state, size_t size): a generator for the array,
    // seeded and made current; the previous array, or NULL (EINVAL) under 8 bytes.
    p.libsystem("_initstate", [generator](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[1] || s.x[2] < 8) throw Errno{einval};
        auto fresh = Generator::of_size(s.x[2]);
        fresh.seed(static_cast<std::uint32_t>(s.x[0]));
        m.write32(s.x[1], fresh.type);
        std::lock_guard lock(generator->mutex);
        generator->generators[s.x[1]] = std::move(fresh);
        return std::exchange(generator->current, s.x[1]);
    }, 0);
    // char* setstate(const char* state): back to an array initstate set up (its generator
    // continues where it stopped); the previous array, or NULL (EINVAL) for another one.
    p.libsystem("_setstate", [generator](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::lock_guard lock(generator->mutex);
        if (!generator->generators.contains(s.x[0])) throw Errno{einval};
        return std::exchange(generator->current, s.x[0]);
    }, 0);
    p.libsystem("_srandom", [generator](CpuState& s, GuestMemory&) -> std::uint64_t { generator->seed(static_cast<std::uint32_t>(s.x[0])); return 0; });
    p.libsystem("_srandomdev", [generator](CpuState&, GuestMemory&) -> std::uint64_t {
        std::random_device device;
        generator->seed(device());
        return 0;
    });

    // char* strndup(const char* s, size_t n)
    p.libsystem("_strndup", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        if (!s.x[0]) throw Errno{efault};
        const auto maximum = static_cast<std::size_t>(std::min<std::uint64_t>(s.x[1], 1ULL << 30));
        const auto bytes = m.read_string(s.x[0], 1, maximum);
        const auto copy = p.malloc(bytes.size() + 1);
        m.write_bytes(copy, bytes);
        m.write8(copy + bytes.size(), 0);
        return copy;
    }, 0);

    // char* strptime(const char* buf, const char* format, struct tm* tm) ("C" locale).
    p.libsystem("_strptime", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto input = guest_string(m, s.x[0], 4096), format = guest_string(m, s.x[1], 1024);
        Tm tm{};
        std::array<std::int32_t, 9> fields{};
        for (std::size_t index = 0; index < fields.size(); ++index) fields[index] = static_cast<std::int32_t>(m.read32(s.x[2] + index * 4));
        tm.sec = fields[0]; tm.min = fields[1]; tm.hour = fields[2]; tm.mday = fields[3]; tm.mon = fields[4];
        tm.year = fields[5]; tm.wday = fields[6]; tm.yday = fields[7]; tm.isdst = fields[8];
        tm.gmtoff = static_cast<std::int64_t>(m.read64(s.x[2] + 40));
        std::uint32_t set = 0;
        TimeParser parser(input, tm, set);
        if (!parser.parse(format)) return 0;
        const std::array<std::int32_t, 9> out{tm.sec, tm.min, tm.hour, tm.mday, tm.mon, tm.year, tm.wday, tm.yday, tm.isdst};
        for (std::size_t index = 0; index < out.size(); ++index) m.write32(s.x[2] + index * 4, static_cast<std::uint32_t>(out[index]));
        m.write64(s.x[2] + 40, static_cast<std::uint64_t>(tm.gmtoff));
        return s.x[0] + parser.position();
    }, 0);

    // char* basename(char*), char* dirname(char*): results in a per-thread buffer
    // (MAXPATHLEN), valid until the thread's next call.
    auto buffers = std::make_shared<std::pair<std::mutex, std::map<std::uint64_t, std::uint64_t>>>();
    const auto buffer = [&process, buffers] {
        const auto thread = process.current_thread();
        const auto identity = thread ? thread->identity : 0;
        std::lock_guard lock(buffers->first);
        auto& address = buffers->second[identity];
        if (!address) address = process.allocate_guest(1024);
        return address;
    };
    p.libsystem("_basename", [buffer](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto value = base_name(s.x[0] ? guest_string(m, s.x[0], 4096) : std::string());
        if (value.size() >= 1024) throw Errno{enametoolong};
        const auto out = buffer();
        put_string(m, out, value);
        return out;
    }, 0);
    p.libsystem("_dirname", [buffer](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto value = directory_name(s.x[0] ? guest_string(m, s.x[0], 4096) : std::string());
        if (value.size() >= 1024) throw Errno{enametoolong};
        const auto out = buffer();
        put_string(m, out, value);
        return out;
    }, 0);

    // int __vsprintf_chk(char* s, int flag, size_t slen, const char* format, va_list ap)
    p.libsystem("___vsprintf_chk", [p](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto size = s.x[2];
        if (size == UINT64_MAX) return p.call(libsystem_library, "_vsprintf", {s.x[0], s.x[3], s.x[4]});
        const auto written = static_cast<std::int32_t>(p.call(libsystem_library, "_vsnprintf", {s.x[0], size, s.x[3], s.x[4]}));
        if (written >= 0 && static_cast<std::uint64_t>(written) >= size)
            throw std::runtime_error("Alchemy: __vsprintf_chk detected a buffer overflow (abort)");
        return static_cast<std::uint32_t>(written);
    });

    // CommonCrypto: MD4 (RFC 1320) over the 92-byte CC_MD4_CTX, and the system RNG.
    p.libsystem("_CC_MD4_Init", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        Md4 digest;
        digest.store(m, s.x[0]);
        return 1;
    });
    p.libsystem("_CC_MD4_Update", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        auto digest = Md4::load(m, s.x[0]);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(static_cast<std::uint32_t>(s.x[2])));
        if (!bytes.empty()) m.read_bytes(s.x[1], bytes);
        digest.update(bytes);
        digest.store(m, s.x[0]);
        return 1;
    });
    p.libsystem("_CC_MD4_Final", [](CpuState& s, GuestMemory& m) -> std::uint64_t {
        auto digest = Md4::load(m, s.x[1]);
        const auto value = digest.finish();
        m.write_bytes(s.x[0], value);
        m.fill_bytes(s.x[1], 0, Md4::context_size);
        return 1;
    });
}

} // namespace alchemy::export_fill
