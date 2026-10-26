#include "alchemy/darwin_numeric.hpp"
#include "alchemy/darwin_locale.hpp"
#include "transmute/jit.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cfenv>
#include <complex>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::string_view library = "/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate";
void require(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
struct Returned { std::uint64_t x0{}, x1{}, d0{}, d1{}, fpsr{}; };
struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext context{engine, [](unsigned, auto bytes) { return alchemy::OutputResult{bytes.size(), 0}; }};
    explicit Fixture(bool jit) {
        alchemy::install_darwin_numeric(context); engine.set_jit_enabled(jit); engine.set_jit_profiling(jit);
    }
    std::uint64_t allocation(std::uint64_t bytes = 64) { return context.allocate_guest(bytes); }
    std::uint64_t integer(std::int32_t value) { const auto p = allocation(4); engine.write32(p, static_cast<std::uint32_t>(value)); return p; }
    std::uint64_t character(char value) { const auto p = allocation(1); static_cast<transmute::GuestMemory&>(engine).write8(p, static_cast<std::uint8_t>(value)); return p; }
    template<class R> void real(std::uint64_t p, R value) {
        if constexpr (sizeof(R) == 4) engine.write32(p, std::bit_cast<std::uint32_t>(value));
        else engine.write64(p, std::bit_cast<std::uint64_t>(value));
    }
    template<class R> R get_real(std::uint64_t p) {
        if constexpr (sizeof(R) == 4) return std::bit_cast<float>(engine.read32(p)); else return std::bit_cast<double>(engine.read64(p));
    }
    template<class R> std::uint64_t array(std::initializer_list<R> values) {
        const auto p = allocation(values.size() * sizeof(R)); std::uint64_t i{};
        for (const auto value : values) { real(p + i, value); i += sizeof(R); } return p;
    }
    template<class R> std::uint64_t complex(std::initializer_list<std::complex<R>> values) {
        const auto p = allocation(values.size() * sizeof(R) * 2); std::uint64_t i{};
        for (const auto value : values) { real(p + i, value.real()); real(p + i + sizeof(R), value.imag()); i += sizeof(R) * 2; } return p;
    }
    // Project-authored ARM64 bridge loads all argument banks and spilled bytes,
    // calls the provider, then records guest results before callback restoration.
    Returned call(std::string_view name, std::initializer_list<std::uint64_t> gp = {},
                  std::initializer_list<std::uint64_t> fp = {},
                  std::initializer_list<std::pair<unsigned, std::uint64_t>> stack = {},
                  std::uint32_t fpcr = 0, std::uint32_t fpsr = 0) {
        const auto provider = context.resolve(name.find("$NEWLAPACK") != std::string_view::npos ? library : "/usr/lib/libSystem.B.dylib", name);
        require(provider.has_value(), "missing numeric export");
        const auto packet = allocation(224); engine.fill_bytes(packet, 0, 224);
        unsigned index{}; for (const auto value : gp) engine.write64(packet + index++ * 8, value);
        index = 0; for (const auto value : fp) engine.write64(packet + 64 + index++ * 8, value);
        for (const auto& [offset, value] : stack) engine.write64(packet + 96 + offset, value);
        engine.write64(packet + 200, fpcr); engine.write64(packet + 208, fpsr);
        std::vector<std::uint32_t> code{0xa9bf7bf3, 0xd10103ff, 0xaa0003f3};
        for (unsigned at = 0; at < 8; ++at) {
            code.push_back(0xf9400009U | ((12 + at) << 10) | (19 << 5));
            code.push_back(0xf9000009U | (at << 10) | (31 << 5));
        }
        code.push_back(0xf9406669); code.push_back(0xd51b4409);
        code.push_back(0xf9406a69); code.push_back(0xd51b4429);
        for (unsigned at = 0; at < 4; ++at) code.push_back(0xfd400000U | ((8 + at) << 10) | (19 << 5) | at);
        for (unsigned at = 0; at < 8; ++at) code.push_back(0xf9400000U | (at << 10) | (19 << 5) | at);
        for (unsigned at = 0; at < 4; ++at) code.push_back((at ? 0xf2800010U : 0xd2800010U) |
            (at << 21) | (static_cast<std::uint32_t>((provider->address >> (at * 16)) & 0xffff) << 5));
        code.push_back(0xd63f0200);
        code.push_back(0xfd005260); code.push_back(0xfd005661);
        code.push_back(0xf9005a60); code.push_back(0xf9005e61);
        code.push_back(0xd53b4429); code.push_back(0xf9006269);
        code.push_back(0x910103ff); code.push_back(0xa8c17bf3); code.push_back(0xd65f03c0);
        const auto start = allocation(code.size() * 4);
        for (std::size_t at = 0; at < code.size(); ++at) engine.write32(start + at * 4, code[at]);
        engine.set_memory_access(start, true, false, true);
        const std::array args{packet}; static_cast<void>(engine.invoke_callback(start, args));
        return {engine.read64(packet + 176), engine.read64(packet + 184), engine.read64(packet + 160), engine.read64(packet + 168), engine.read64(packet + 192)};
    }
};
std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }
std::uint64_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }
void close(double actual, double expected, double tolerance = 1e-10) {
    if (!(std::abs(actual - expected) <= tolerance * std::max(1.0, std::abs(expected))))
        throw std::runtime_error("numeric result differs: actual " + std::to_string(actual) + ", expected " + std::to_string(expected));
}
void blas(bool jit) {
    Fixture f(jit);
    const auto a = f.array<double>({1,2,3,4,5,6}), b = f.array<double>({7,8,9,10,11,12});
    const auto c = f.array<double>({99,99,99,99});
    static_cast<void>(f.call("_cblas_dgemm$NEWLAPACK", {101,111,111,2,2,3,a,3}, {bits(1.0),bits(0.0)}, {{0,b},{8,2},{16,c},{24,2}}));
    close(f.get_real<double>(c), 58); close(f.get_real<double>(c + 8), 64); close(f.get_real<double>(c + 16), 139); close(f.get_real<double>(c + 24), 154);
    const auto x = f.array<double>({2,3,4}), y = f.array<double>({1,1});
    static_cast<void>(f.call("_cblas_dgemv$NEWLAPACK", {101,111,2,3,a,3,x,1}, {bits(2.0),bits(3.0)}, {{0,y},{8,1}}));
    close(f.get_real<double>(y), 43); close(f.get_real<double>(y + 8), 97);
    const auto reverse = f.call("_cblas_ddot$NEWLAPACK", {3,x,static_cast<std::uint64_t>(-1LL),x,1});
    close(std::bit_cast<double>(reverse.d0), 25);
    const auto sx = f.array<float>({1,2,3}), sy = f.array<float>({4,5,6});
    close(std::bit_cast<float>(static_cast<std::uint32_t>(f.call("_cblas_sdot$NEWLAPACK", {3,sx,1,sy,1}).d0)), 32, 1e-5);
    static_cast<void>(f.call("_cblas_saxpy$NEWLAPACK", {3,sx,1,sy,1}, {bits(2.0f)})); close(f.get_real<float>(sy + 8), 12, 1e-5);
    const auto large = f.array<double>({std::numeric_limits<double>::max()}), twice = f.array<double>({2});
    const auto overflowing = f.call("_cblas_ddot$NEWLAPACK", {1,large,1,twice,1});
    require(std::isinf(std::bit_cast<double>(overflowing.d0)) && (overflowing.fpsr & 4), "BLAS omitted guest overflow");
    const auto units = f.array<double>({1,1}), rounding = f.array<double>({1,0x1p-53});
    const auto down = f.call("_cblas_ddot$NEWLAPACK", {2,units,1,rounding,1}, {}, {}, 2U << 22);
    const auto up = f.call("_cblas_ddot$NEWLAPACK", {2,units,1,rounding,1}, {}, {}, 1U << 22);
    require(down.d0 == bits(1.0) && up.d0 == bits(1.0) + 1, "BLAS ignored guest rounding");
    const auto triangle = f.array<double>({-9,-9,-9,-9});
    static_cast<void>(f.call("_cblas_dsyrk$NEWLAPACK", {101,121,111,2,3,a,3,triangle}, {bits(1.0),bits(0.0)}, {{0,2}}));
    close(f.get_real<double>(triangle), 14); close(f.get_real<double>(triangle + 8), 32); close(f.get_real<double>(triangle + 16), -9); close(f.get_real<double>(triangle + 24), 77);
    const auto z = f.complex<double>({{1,2},{3,-1}}), w = f.complex<double>({{2,-1},{4,3}}), out = f.allocation(16);
    static_cast<void>(f.call("_cblas_zdotc_sub$NEWLAPACK", {2,z,1,w,1,out})); close(f.get_real<double>(out), 9); close(f.get_real<double>(out + 8), 8);
    const auto one = f.complex<double>({{1,0}}), zero = f.complex<double>({{0,0}});
    static_cast<void>(f.call("_cblas_zgemm$NEWLAPACK", {101,111,113,1,1,2,one,z}, {}, {{0,2},{8,w},{16,2},{24,zero},{32,out},{40,1}}));
    close(f.get_real<double>(out), 9); close(f.get_real<double>(out + 8), -8);
    bool rejected{};
    try { static_cast<void>(f.call("_cblas_dgemm$NEWLAPACK", {101,111,111,2,2,3,a,3}, {bits(1.0),bits(0.0)}, {{0,0xfffffffffffffff8ULL},{8,2},{16,c},{24,2}})); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected && f.get_real<double>(c) == 58, "bad BLAS input changed output");
    f.engine.set_memory_access(c, true, false, false); rejected = false;
    try { static_cast<void>(f.call("_cblas_dgemm$NEWLAPACK", {101,111,111,2,2,3,a,3}, {bits(1.0),bits(0.0)}, {{0,b},{8,2},{16,c},{24,2}})); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected && f.get_real<double>(c) == 58, "read-only BLAS output changed");
    if (jit) require(f.engine.jit_stats().native_guest_instructions > 0, "numeric JIT bridge did not execute translated code");
}
void lapack(bool jit) {
    Fixture f(jit); const auto n = f.integer(2), nrhs = f.integer(1), lda = f.integer(2), info = f.integer(-999), pivots = f.allocation(8);
    const auto a = f.array<double>({0,2,1,3}), b = f.array<double>({1,5});
    static_cast<void>(f.call("_dgesv$NEWLAPACK", {n,nrhs,a,lda,pivots,b,lda,info}));
    require(f.engine.read32(info) == 0 && f.engine.read32(pivots) == 2, "LU pivot or status"); close(f.get_real<double>(b), 1); close(f.get_real<double>(b + 8), 1);
    const auto singular = f.array<float>({1,2,2,4}), sb = f.array<float>({3,6});
    static_cast<void>(f.call("_sgesv$NEWLAPACK", {n,nrhs,singular,lda,pivots,sb,lda,info}));
    require(f.engine.read32(info) == 2 && f.get_real<float>(sb) == 3, "singular solve must report INFO and preserve RHS");
    const auto za = f.complex<double>({{2,0},{0,-1},{0,1},{3,0}}), zb = f.complex<double>({{2,1},{3,-1}});
    static_cast<void>(f.call("_zgesv$NEWLAPACK", {n,nrhs,za,lda,pivots,zb,lda,info}));
    require(f.engine.read32(info) == 0, "complex solve INFO"); close(f.get_real<double>(zb), 1); close(f.get_real<double>(zb + 8), 0); close(f.get_real<double>(zb + 16), 1); close(f.get_real<double>(zb + 24), 0);
    const auto spd = f.array<double>({4,2,123,5});
    static_cast<void>(f.call("_dpotrf$NEWLAPACK", {f.character('L'),n,spd,lda,info}));
    require(f.engine.read32(info) == 0, "Cholesky INFO"); close(f.get_real<double>(spd), 2); close(f.get_real<double>(spd + 8), 1); close(f.get_real<double>(spd + 16), 123); close(f.get_real<double>(spd + 24), 2);
    const auto hermitian = f.complex<float>({{4,0},{0,-2},{99,99},{5,0}});
    static_cast<void>(f.call("_cpotrf$NEWLAPACK", {f.character('L'),n,hermitian,lda,info}));
    require(f.engine.read32(info) == 0, "Hermitian Cholesky INFO"); close(f.get_real<float>(hermitian), 2, 1e-5); close(f.get_real<float>(hermitian + 12), -1, 1e-5); close(f.get_real<float>(hermitian + 24), 2, 1e-5);
    const auto nonpositive = f.array<double>({1,2,2,1});
    static_cast<void>(f.call("_dpotrf$NEWLAPACK", {f.character('U'),n,nonpositive,lda,info})); require(f.engine.read32(info) == 2, "indefinite Cholesky accepted");
    static_cast<void>(f.call("_dgetrf$NEWLAPACK", {f.integer(-1),n,0,lda,pivots,info})); require(static_cast<std::int32_t>(f.engine.read32(info)) == -1, "LAPACK invalid argument status");
    // The decomposition drivers (darwin/accelerate/lapack.cpp).
    const auto two = f.integer(2), one = f.integer(1), work = f.allocation(512), iwork = f.allocation(64), lwork = f.integer(64);
    const auto sv = f.allocation(16), u = f.allocation(32), vt = f.allocation(32);
    const auto diagonal = f.array<double>({3, 0, 0, 4});
    static_cast<void>(f.call("_dgesdd$NEWLAPACK", {f.character('A'), two, two, diagonal, two, sv, u, two}, {},
                             {{0, vt}, {8, two}, {16, work}, {24, lwork}, {32, iwork}, {40, info}}));
    require(f.engine.read32(info) == 0, "dgesdd INFO"); close(f.get_real<double>(sv), 4); close(f.get_real<double>(sv + 8), 3);
    const auto symmetric = f.array<double>({2, 1, 1, 2}), w = f.allocation(16);
    static_cast<void>(f.call("_dsyevd$NEWLAPACK", {f.character('V'), f.character('U'), two, symmetric, two, w, work, lwork}, {},
                             {{0, iwork}, {8, f.integer(64)}, {16, info}}));
    require(f.engine.read32(info) == 0, "dsyevd INFO"); close(f.get_real<double>(w), 1); close(f.get_real<double>(w + 8), 3);
    close(std::abs(f.get_real<double>(symmetric)), std::sqrt(0.5));
    const auto rotation = f.array<double>({0, 1, -1, 0}), wr = f.allocation(16), wi = f.allocation(16), vr = f.allocation(32);
    static_cast<void>(f.call("_dgeev$NEWLAPACK", {f.character('N'), f.character('V'), two, rotation, two, wr, wi, 0}, {},
                             {{0, two}, {8, vr}, {16, two}, {24, work}, {32, lwork}, {40, info}}));
    require(f.engine.read32(info) == 0, "dgeev INFO"); close(f.get_real<double>(wr), 0); close(f.get_real<double>(wi), 1); close(f.get_real<double>(wi + 8), -1);
    const auto column = f.array<double>({3, 4}), tau = f.allocation(8);
    static_cast<void>(f.call("_dgeqrf$NEWLAPACK", {two, one, column, two, tau, work, lwork, info}));
    require(f.engine.read32(info) == 0, "dgeqrf INFO"); close(f.get_real<double>(column), -5);
    static_cast<void>(f.call("_dorgqr$NEWLAPACK", {two, one, one, column, two, tau, work, lwork}, {}, {{0, info}}));
    require(f.engine.read32(info) == 0, "dorgqr INFO"); close(f.get_real<double>(column), -0.6); close(f.get_real<double>(column + 8), -0.8);
    require(alchemy::deferred_numeric_api("_dgesdd$NEWLAPACK").empty() && !alchemy::deferred_numeric_api("_dgesdd$NEWLAPACK$ILP64").empty(),
            "only the ILP64 interface is deferred");
}
void helpers(bool jit) {
    Fixture f(jit);
    const auto absolute = f.call("_cabs", {}, {bits(3.0),bits(4.0)}); close(std::bit_cast<double>(absolute.d0), 5);
    const auto root = f.call("_csqrt", {}, {bits(-4.0),bits(-0.0)}); close(std::bit_cast<double>(root.d0), 0); close(std::bit_cast<double>(root.d1), -2);
    const auto exponential = f.call("_cexpf", {}, {bits(0.0f),bits(0.0f)}); close(std::bit_cast<float>(static_cast<std::uint32_t>(exponential.d0)), 1, 1e-5);
    const auto overflow = f.call("_exp", {}, {bits(1000.0)});
    require(std::isinf(std::bit_cast<double>(overflow.d0)) && (overflow.fpsr & 4), "scalar exp omitted guest overflow");
    const auto underflow = f.call("_expf", {}, {bits(-1000.0f)});
    require(std::bit_cast<float>(static_cast<std::uint32_t>(underflow.d0)) == 0 && (underflow.fpsr & 8), "scalar expf omitted guest underflow");
    const auto invalid = f.call("_sqrt", {}, {bits(-1.0)});
    require(std::isnan(std::bit_cast<double>(invalid.d0)) && (invalid.fpsr & 1), "scalar sqrt omitted guest invalid");
    const auto sincos = f.call("___sincosf_stret", {}, {bits(std::numeric_limits<float>::infinity())});
    require(std::isnan(std::bit_cast<float>(static_cast<std::uint32_t>(sincos.d0))) && (sincos.fpsr & 1),
        "sincosf omitted guest invalid");
    const auto host_rounding = std::fegetround();
    const auto down = f.call("_sqrt", {}, {bits(2.0)}, {}, 2U << 22);
    const auto up = f.call("_sqrt", {}, {bits(2.0)}, {}, 1U << 22);
    require(down.d0 < up.d0 && std::fegetround() == host_rounding, "scalar math ignored guest rounding or changed host environment");
    const auto nearest = f.call("_rint", {}, {bits(2.5)}); close(std::bit_cast<double>(nearest.d0), 2); require(nearest.fpsr & 16, "rint omitted inexact");
    close(std::bit_cast<double>(f.call("_rintl", {}, {bits(-2.25)}, {}, 1U << 22).d0), -2);
    require(std::signbit(std::bit_cast<double>(f.call("_rint", {}, {bits(-0.25)}).d0)), "rint lost negative zero");
    require(f.call("_feraiseexcept", {7}).fpsr == 7, "raise guest FP flags");
    require(f.call("_feclearexcept", {2}, {}, {}, 0, 7).fpsr == 5, "clear selected guest FP flags");
    require(f.call("_fetestexcept", {6}, {}, {}, 0, 7).x0 == 6, "test selected guest FP flags");
    require(f.call("_feraiseexcept", {32}).fpsr == 128 && f.call("_fetestexcept", {32}, {}, {}, 0, 128).x0 == 32,
        "Darwin flush-to-zero exception mapping");
    const auto division = f.call("___udivti3", {5,1,3,0}); require(division.x0 == 0x5555555555555557ULL && division.x1 == 0, "128-bit divide with wide numerator");
    require(f.call("___udivti3", {0,0x8000000000000000ULL,0,1}).x0 == 0x8000000000000000ULL, "128-bit divide with wide denominator");
    require(f.call("___udivti3", {UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX}).x0 == 1, "128-bit divide subtraction carry");
    bool rejected{}; try { static_cast<void>(f.call("___udivti3", {1,0,0,0})); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "128-bit divide zero accepted");
    close(std::bit_cast<double>(f.call("___exp10", {}, {bits(2.0)}).d0), 100);
    close(std::bit_cast<float>(static_cast<std::uint32_t>(f.call("___exp10f", {}, {bits(3.0f)}).d0)), 1000, 1e-3);
    close(std::bit_cast<double>(f.call("_round", {}, {bits(-2.5)}).d0), -3);
    close(std::bit_cast<double>(f.call("_logb", {}, {bits(8.0)}).d0), 3);
    close(std::bit_cast<double>(f.call("_scalbn", {5}, {bits(1.5)}).d0), 48);
    require(std::bit_cast<float>(static_cast<std::uint32_t>(f.call("_nexttowardf", {}, {bits(1.0f), bits(2.0)}).d0)) > 1.0f, "nexttowardf direction");
    require(f.call("___fpclassifyd", {}, {bits(std::numeric_limits<double>::infinity())}).x0 == 2 &&
        f.call("___fpclassifyd", {}, {bits(0.0)}).x0 == 3 && f.call("___fpclassifyd", {}, {bits(1e-310)}).x0 == 5, "__fpclassifyd");
    require(f.call("___isfinited", {}, {bits(1.0)}).x0 == 1 && f.call("___isfinited", {}, {bits(std::nan(""))}).x0 == 0, "__isfinited");
    const auto environment = f.allocation(16);
    const auto saved = f.call("_fegetenv", {environment}, {}, {}, 2U << 22, 5);
    require(saved.x0 == 0 && f.engine.read64(environment) == 5 && f.engine.read64(environment + 8) == (2U << 22), "fegetenv");
    require(f.call("_fesetenv", {environment}).fpsr == 5, "fesetenv restores flags");
    require(f.call("_fegetround", {}, {}, {}, 3U << 22).x0 == 0x00c00000U, "fegetround");
}
void least_squares(bool jit) {
    Fixture f(jit);
    for (const auto precision : {'s','d','c','z'}) {
        const bool single = precision == 's' || precision == 'c', complex = precision == 'c' || precision == 'z';
        const unsigned bytes = single ? 4U : 8U, element = bytes * (complex ? 2U : 1U);
        const auto put = [&](std::uint64_t p, double value) { if (single) f.real(p, static_cast<float>(value)); else f.real(p, value); };
        const auto get = [&](std::uint64_t p) { return single ? static_cast<double>(f.get_real<float>(p)) : f.get_real<double>(p); };
        const auto m = f.integer(3), n = f.integer(2), nrhs = f.integer(1), lda = f.integer(3), ldb = f.integer(3);
        const auto a = f.allocation(6 * element), b = f.allocation(3 * element), sigma = f.allocation(2 * bytes);
        const auto cutoff = f.allocation(bytes), rank = f.integer(-1), status = f.integer(-1), lwork = f.integer(-1);
        auto workspace = f.allocation(element), iwork = f.integer(0), rwork = f.allocation(bytes);
        put(cutoff, -1);
        const auto name = "_" + std::string(1, precision) + "gelsd$NEWLAPACK";
        const auto call = [&] {
            if (complex) return f.call(name, {m,n,nrhs,a,lda,b,ldb,sigma}, {}, {{0,cutoff},{8,rank},{16,workspace},{24,lwork},{32,rwork},{40,iwork},{48,status}});
            return f.call(name, {m,n,nrhs,a,lda,b,ldb,sigma}, {}, {{0,cutoff},{8,rank},{16,workspace},{24,lwork},{32,iwork},{40,status}});
        };
        static_cast<void>(call()); require(f.engine.read32(status) == 0 && get(workspace) >= 1, "GELSD workspace query");
        const auto count = static_cast<std::uint32_t>(get(workspace));
        const auto integer_count = f.engine.read32(iwork), real_count = complex ? static_cast<std::uint32_t>(get(rwork)) : 0;
        workspace = f.allocation(count * element); iwork = f.allocation(integer_count * 4);
        if (complex) rwork = f.allocation(real_count * bytes);
        f.engine.write32(lwork, count);
        f.engine.fill_bytes(a, 0, 6 * element); f.engine.fill_bytes(b, 0, 3 * element);
        // Columns [1,0,1], [0,1,1] or [0,1,i] are independent and nonorthogonal.
        put(a, 1); put(a + 2 * element, 1); put(a + 4 * element, 1);
        put(a + 5 * element + (complex ? bytes : 0), 1);
        put(b, 1); put(b + element, complex ? 3 : 2); put(b + 2 * element, complex ? 2 : 4);
        if (complex) { put(b + bytes, 2); put(b + element + bytes, -1); put(b + 2 * element + bytes, 5); }
        static_cast<void>(call()); require(f.engine.read32(status) == 0 && f.engine.read32(rank) == 2, "GELSD full rank");
        const auto tolerance = single ? 2e-5 : 1e-10;
        close(get(sigma), std::sqrt(3.0), tolerance); close(get(sigma + bytes), 1, tolerance);
        close(get(b), complex ? 1 : 4.0 / 3, tolerance); close(get(b + element), complex ? 3 : 7.0 / 3, tolerance);
        if (complex) { close(get(b + bytes), 2, tolerance); close(get(b + element + bytes), -1, tolerance); }
        else close(get(b + 2 * element) * get(b + 2 * element), 1.0 / 3, tolerance);
        // Rank-deficient tall matrices require the minimum-norm solution.
        f.engine.fill_bytes(a, 0, 6 * element); f.engine.fill_bytes(b, 0, 3 * element);
        for (unsigned r = 0; r < 3; ++r) { put(a + r * element, r + 1); put(a + (r + 3) * element, 2 * (r + 1)); put(b + r * element, r + 1); }
        static_cast<void>(call()); require(f.engine.read32(status) == 0 && f.engine.read32(rank) == 1, "GELSD deficient rank");
        close(get(b), .2, tolerance); close(get(b + element), .4, tolerance); close(get(sigma), std::sqrt(70.0), tolerance);
        // Wide A=[1,1] must return X=[1,1], including the second output row.
        f.engine.write32(m, 1); f.engine.write32(lda, 1); f.engine.write32(ldb, 2);
        f.engine.fill_bytes(a, 0, 6 * element); f.engine.fill_bytes(b, 0, 3 * element);
        put(a, 1); put(a + element, 1); put(b, 2);
        static_cast<void>(call()); require(f.engine.read32(status) == 0 && f.engine.read32(rank) == 1, "GELSD wide rank");
        close(get(b), 1, tolerance); close(get(b + element), 1, tolerance); close(get(sigma), std::sqrt(2.0), tolerance);
        f.engine.write32(lwork, 0); put(b, 19); static_cast<void>(call());
        require(static_cast<std::int32_t>(f.engine.read32(status)) == -12 && get(b) == 19, "GELSD invalid workspace changed RHS");
    }
}
void locale_parse(bool jit) {
    Fixture f(jit); static_cast<void>(alchemy::install_darwin_locale(f.context));
    const auto name = f.allocation(2), input = f.allocation(5), end = f.allocation(8);
    static_cast<transmute::GuestMemory&>(f.engine).write_bytes(name, std::array<std::uint8_t,2>{'C',0});
    static_cast<transmute::GuestMemory&>(f.engine).write_bytes(input, std::array<std::uint8_t,5>{'-','1','1','3',0});
    f.context.set_errno(0);
    const auto locale = f.call("_newlocale", {63,name,0}).x0;
    require(locale && f.engine.read32(f.context.thread(f.engine)->errno_address) == 0, "Darwin LC_ALL_MASK rejected");
    close(std::bit_cast<double>(f.call("_strtold_l", {input,end,locale}).d0), -113);
    require(f.engine.read64(end) == input + 4 && f.engine.read32(f.context.thread(f.engine)->errno_address) == 0,
        "Darwin longdouble parse end or errno");
    require(f.call("_newlocale", {64,name,0}).x0 == 0 && f.engine.read32(f.context.thread(f.engine)->errno_address) == 22,
        "newlocale accepted a mask outside Darwin categories");
    static_cast<void>(f.call("_freelocale", {locale}));
    f.context.set_errno(0); static_cast<void>(f.call("_strtold_l", {input,end,locale}));
    require(f.engine.read32(f.context.thread(f.engine)->errno_address) == 22, "freed locale identity was accepted");
}
} // namespace
int main() {
    try {
        for (const auto jit : {false, true}) { blas(jit); lapack(jit); helpers(jit); locale_parse(jit); least_squares(jit); }
        std::cout << "Alchemy numeric tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
