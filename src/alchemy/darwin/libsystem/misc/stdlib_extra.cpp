#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>

// <stdlib.h> leftovers: the rand48 generators (drand48(3)); crypt(3) answers ENOSYS. One 48-bit linear congruential
// generator per process, X(n+1) = (a X(n) + c) mod 2^48, with a = 0x5DEECE66D and c = 0xB
// until lcong48 changes them; the *rand48 forms that take xsubi[3] keep their own X.
namespace alchemy::export_fill {
namespace {

constexpr std::uint64_t mask48 = (1ULL << 48) - 1;
constexpr std::uint64_t default_multiplier = 0x5DEECE66DULL, default_addend = 0xB;

struct Rand48 {
    std::mutex mutex;
    std::uint64_t x = 0x1234ABCD330EULL, a = default_multiplier, c = default_addend;
    std::uint64_t previous{};   // seed48's static unsigned short[3] (guest address)
};

std::uint16_t read16(const GuestMemory& m, std::uint64_t address) {
    return static_cast<std::uint16_t>(m.read8(address) | m.read8(address + 1) << 8);
}
std::uint64_t load(const GuestMemory& m, std::uint64_t words) {
    if (!words) throw Errno{efault};
    return static_cast<std::uint64_t>(read16(m, words)) | static_cast<std::uint64_t>(read16(m, words + 2)) << 16 |
           static_cast<std::uint64_t>(read16(m, words + 4)) << 32;
}
void store(GuestMemory& m, std::uint64_t words, std::uint64_t value) {
    for (unsigned index = 0; index < 3; ++index) write16(m, words + index * 2, static_cast<std::uint16_t>(value >> (16 * index)));
}
double fraction(std::uint64_t x) { return std::ldexp(static_cast<double>(x & mask48), -48); }
void double_result(CpuState& s, double value) { s.v[0][0] = std::bit_cast<std::uint64_t>(value); s.v[0][1] = 0; }

} // namespace

void install_libsystem_stdlib(Provider& p) {
    const auto state = std::make_shared<Rand48>();
    state->previous = p.process().allocate_guest(8);
    // Advances the process generator (or `words`' X) and returns the new X.
    const auto next = [state](GuestMemory& m, std::uint64_t words) {
        std::lock_guard lock(state->mutex);
        auto x = words ? load(m, words) : state->x;
        x = (state->a * x + state->c) & mask48;
        if (words) store(m, words, x); else state->x = x;
        return x;
    };

    p.libsystem("_drand48", [next](CpuState& s, GuestMemory& m) -> std::uint64_t { double_result(s, fraction(next(m, 0))); return 0; });
    p.libsystem("_erand48", [next](CpuState& s, GuestMemory& m) -> std::uint64_t { double_result(s, fraction(next(m, s.x[0]))); return 0; });
    // lrand48/nrand48: the high 31 bits, in [0, 2^31); mrand48/jrand48: the high 32 bits, signed.
    p.libsystem("_lrand48", [next](CpuState&, GuestMemory& m) -> std::uint64_t { return next(m, 0) >> 17; });
    p.libsystem("_nrand48", [next](CpuState& s, GuestMemory& m) -> std::uint64_t { return next(m, s.x[0]) >> 17; });
    const auto high = [](std::uint64_t x) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(x >> 16))));
    };
    p.libsystem("_mrand48", [next, high](CpuState&, GuestMemory& m) -> std::uint64_t { return high(next(m, 0)); });
    p.libsystem("_jrand48", [next, high](CpuState& s, GuestMemory& m) -> std::uint64_t { return high(next(m, s.x[0])); });

    // srand48(seed): X = seed's low 32 bits, then 0x330E; seed48(seed16v) sets X from three
    // words and returns the previous X; lcong48(param) sets X (0-2), a (3-5) and c (6).
    // srand48 and seed48 restore the default a and c.
    p.libsystem("_srand48", [state](CpuState& s, GuestMemory&) -> std::uint64_t {
        std::lock_guard lock(state->mutex);
        state->x = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.x[0])) << 16) | 0x330E;
        state->a = default_multiplier; state->c = default_addend;
        return 0;
    });
    p.libsystem("_seed48", [state](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto seed = load(m, s.x[0]);
        std::lock_guard lock(state->mutex);
        store(m, state->previous, state->x);
        state->x = seed; state->a = default_multiplier; state->c = default_addend;
        return state->previous;
    }, 0);
    p.libsystem("_lcong48", [state](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto x = load(m, s.x[0]), a = load(m, s.x[0] + 6);
        const std::uint64_t c = read16(m, s.x[0] + 12);
        std::lock_guard lock(state->mutex);
        state->x = x; state->a = a; state->c = c;
        return 0;
    });

    // crypt(3), encrypt and setkey: the DES password hash is not implemented yet, so crypt
    // fails with ENOSYS (NULL) and encrypt/setkey set ENOSYS, as POSIX allows for them.
    p.libsystem("_crypt", [](CpuState&, GuestMemory&) -> std::uint64_t { throw Errno{enosys}; }, 0);
    for (const auto* name : {"_encrypt", "_setkey"})
        p.libsystem(name, [](CpuState&, GuestMemory&) -> std::uint64_t { throw Errno{enosys}; }, 0);
}

} // namespace alchemy::export_fill
