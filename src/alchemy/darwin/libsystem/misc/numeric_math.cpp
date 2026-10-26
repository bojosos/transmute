#include "alchemy/darwin_numeric.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <bit>
#include <cerrno>
#include <cfenv>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>
#include <string>

namespace alchemy {
namespace {
class FloatingEnvironment {
public:
    explicit FloatingEnvironment(std::uint32_t fpcr) : old_errno_(errno) {
        if (std::feholdexcept(&saved_)) throw std::runtime_error("Alchemy numeric: host floating-point environment is unavailable");
        const int modes[]{FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
        if (std::fesetround(modes[(fpcr >> 22) & 3])) {
            static_cast<void>(std::fesetenv(&saved_)); errno = old_errno_;
            throw std::runtime_error("Alchemy numeric: host floating-point rounding is unavailable");
        }
        errno = 0;
    }
    ~FloatingEnvironment() { static_cast<void>(std::fesetenv(&saved_)); errno = old_errno_; }
    FloatingEnvironment(const FloatingEnvironment&) = delete;
    FloatingEnvironment& operator=(const FloatingEnvironment&) = delete;
    std::uint32_t status() const {
        const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
        return ((flags & FE_INVALID) ? 1U : 0U) | ((flags & FE_DIVBYZERO) ? 2U : 0U) |
            ((flags & FE_OVERFLOW) ? 4U : 0U) | ((flags & FE_UNDERFLOW) ? 8U : 0U) | ((flags & FE_INEXACT) ? 16U : 0U);
    }
private:
    std::fenv_t saved_{};
    int old_errno_{};
};
void merge_status(transmute::CpuState& state, std::uint32_t status) {
    if ((status << 8) & state.fpcr) throw std::runtime_error("Alchemy numeric: enabled floating-point exception trap requires guest signal delivery");
    state.fpsr |= status;
}
template<class R> R argument(const transmute::CpuState& state, unsigned index) {
    if constexpr (sizeof(R) == 4) return std::bit_cast<float>(static_cast<std::uint32_t>(state.v[index][0]));
    else return std::bit_cast<double>(state.v[index][0]);
}
template<class R> void result(transmute::CpuState& state, R value, unsigned index = 0) {
    if constexpr (sizeof(R) == 4) state.v[index][0] = std::bit_cast<std::uint32_t>(value);
    else state.v[index][0] = std::bit_cast<std::uint64_t>(value);
    state.v[index][1] = 0;
}
// AAPCS64 maps a C complex value to an HFA of two real components. Apple's
// long double is binary64, so the double-complex adapter also serves *l names.
template<class R> std::complex<R> complex_argument(const transmute::CpuState& state, unsigned first = 0) {
    return {argument<R>(state, first), argument<R>(state, first + 1)};
}
template<class R> void complex_result(transmute::CpuState& state, std::complex<R> value) {
    result(state, value.real()); result(state, value.imag(), 1);
}
template<class R> R rounded(R input, unsigned mode) {
    if (!std::isfinite(input) || input == R{}) return input;
    R value{};
    if (mode == 1) value = std::ceil(input);
    else if (mode == 2) value = std::floor(input);
    else if (mode == 3) value = std::trunc(input);
    else {
        const auto lower = std::floor(input), delta = input - lower;
        value = delta < R{0.5} ? lower : delta > R{0.5} ? lower + R{1} : std::fmod(lower, R{2}) == R{} ? lower : lower + R{1};
    }
    return value == R{} ? std::copysign(value, input) : value;
}
template<class R> void install_precision(ProcessContext& context, std::string suffix) {
    const auto install = [&](std::string name, transmute::GuestCallHandler handler) {
        static_cast<void>(context.install_function("_" + name + suffix,
            [&context, operation = name, handler = std::move(handler)](auto& s, auto& m) {
                const auto first = argument<R>(s, 0), second = argument<R>(s, 1);
                std::uint32_t status{}; int error{};
                { FloatingEnvironment environment(s.fpcr); handler(s, m); status = environment.status(); error = errno; }
                if (error == EDOM) status |= 1;
                if (error == ERANGE) {
                    const bool pole = ((operation == "log" || operation == "log2" || operation == "log10") && first == R{}) ||
                        (operation == "log1p" && first == R{-1}) || (operation == "atanh" && std::abs(first) == R{1}) ||
                        (operation == "pow" && first == R{} && second < R{});
                    if (pole) status |= 2;
                    else status |= std::isinf(argument<R>(s, 0)) ? 20U : 24U;
                }
                merge_status(s, status);
                if (error == EDOM) context.set_errno(33); else if (error == ERANGE) context.set_errno(34);
            }));
    };
    const std::pair<const char*, R(*)(R)> unary[]{
        {"sqrt",std::sqrt},{"sin",std::sin},{"cos",std::cos},{"tan",std::tan},{"asin",std::asin},{"acos",std::acos},
        {"atan",std::atan},{"exp",std::exp},{"log",std::log},{"log10",std::log10},{"floor",std::floor},{"ceil",std::ceil},
        {"fabs",std::fabs},{"exp2",std::exp2},{"acosh",std::acosh},{"asinh",std::asinh},{"atanh",std::atanh},{"cbrt",std::cbrt},
        {"cosh",std::cosh},{"erf",std::erf},{"erfc",std::erfc},{"expm1",std::expm1},{"log1p",std::log1p},{"log2",std::log2},
        {"sinh",std::sinh},{"tanh",std::tanh},{"trunc",std::trunc},{"logb",std::logb},{"round",std::round},
        {"lgamma",[](R value) { return std::lgamma(value); }},
        {"__exp10",[](R value) { return std::pow(R{10}, value); }}}; // ___exp10 and ___exp10f
    for (const auto& [name, function] : unary) install(name, [function](auto& s, auto&) { result(s, function(argument<R>(s, 0))); });
    const std::pair<const char*, R(*)(R,R)> binary[]{
        {"pow",std::pow},{"atan2",std::atan2},{"fmod",std::fmod},{"hypot",std::hypot},{"copysign",std::copysign},{"nextafter",std::nextafter}};
    for (const auto& [name, function] : binary) install(name, [function](auto& s, auto&) { result(s, function(argument<R>(s, 0), argument<R>(s, 1))); });
    install("frexp", [](auto& s, auto& m) { int exponent{}; const auto value = std::frexp(argument<R>(s, 0), &exponent); m.write32(s.x[0], static_cast<std::uint32_t>(exponent)); result(s, value); });
    install("ldexp", [](auto& s, auto&) { result(s, std::ldexp(argument<R>(s, 0), static_cast<std::int32_t>(s.x[0]))); });
    install("scalbn", [](auto& s, auto&) { result(s, std::scalbn(argument<R>(s, 0), static_cast<std::int32_t>(s.x[0]))); });
    install("modf", [](auto& s, auto& m) {
        R whole{}; const auto value = std::modf(argument<R>(s, 0), &whole);
        if constexpr (sizeof(R) == 4) m.write32(s.x[0], std::bit_cast<std::uint32_t>(whole)); else m.write64(s.x[0], std::bit_cast<std::uint64_t>(whole));
        result(s, value);
    });
    install("rint", [](auto& s, auto&) {
        const auto input = argument<R>(s, 0), value = rounded(input, (s.fpcr >> 22) & 3);
        if (std::isfinite(input) && input != value) s.fpsr |= 16;
        result(s, value);
    });
    install("cabs", [](auto& s, auto&) { result(s, std::abs(complex_argument<R>(s))); });
    install("carg", [](auto& s, auto&) { result(s, std::arg(complex_argument<R>(s))); });
    install("cacos", [](auto& s, auto&) { complex_result(s, std::acos(complex_argument<R>(s))); });
    install("cacosh", [](auto& s, auto&) { complex_result(s, std::acosh(complex_argument<R>(s))); });
    install("casin", [](auto& s, auto&) { complex_result(s, std::asin(complex_argument<R>(s))); });
    install("casinh", [](auto& s, auto&) { complex_result(s, std::asinh(complex_argument<R>(s))); });
    install("catan", [](auto& s, auto&) { complex_result(s, std::atan(complex_argument<R>(s))); });
    install("catanh", [](auto& s, auto&) { complex_result(s, std::atanh(complex_argument<R>(s))); });
    install("ccos", [](auto& s, auto&) { complex_result(s, std::cos(complex_argument<R>(s))); });
    install("ccosh", [](auto& s, auto&) { complex_result(s, std::cosh(complex_argument<R>(s))); });
    install("cexp", [](auto& s, auto&) { complex_result(s, std::exp(complex_argument<R>(s))); });
    install("clog", [](auto& s, auto&) { complex_result(s, std::log(complex_argument<R>(s))); });
    install("cpow", [](auto& s, auto&) { complex_result(s, std::pow(complex_argument<R>(s), complex_argument<R>(s, 2))); });
    install("csin", [](auto& s, auto&) { complex_result(s, std::sin(complex_argument<R>(s))); });
    install("csinh", [](auto& s, auto&) { complex_result(s, std::sinh(complex_argument<R>(s))); });
    install("csqrt", [](auto& s, auto&) { complex_result(s, std::sqrt(complex_argument<R>(s))); });
    install("ctan", [](auto& s, auto&) { complex_result(s, std::tan(complex_argument<R>(s))); });
    install("ctanh", [](auto& s, auto&) { complex_result(s, std::tanh(complex_argument<R>(s))); });
}
struct Wide { std::uint64_t low{}, high{}; };
bool greater_equal(Wide left, Wide right) { return left.high > right.high || (left.high == right.high && left.low >= right.low); }
Wide subtract(Wide left, Wide right) { return {left.low - right.low, left.high - right.high - (left.low < right.low ? 1ULL : 0ULL)}; }
std::uint32_t exception_status(std::uint32_t flags) { return (flags & 31U) | ((flags & 32U) ? 128U : 0U); }
std::uint32_t exception_flags(std::uint32_t status) { return (status & 31U) | ((status & 128U) ? 32U : 0U); }
} // namespace

transmute::GuestCallHandler numeric_floating_handler(transmute::GuestCallHandler handler);
namespace {

// The rest of C99/POSIX libm and Darwin's extensions: rounding to integers (lround, llround,
// lrint, llrint, nearbyint), fmax/fmin/fdim/fma, tgamma, lgamma_r, remquo, remainder, ilogb,
// scalbln, nan, nexttoward (long double is binary64 on Apple arm64, so *l is double), the
// Bessel functions, BSD's gamma/significand/drem/finite, __sinpi/__cospi/__tanpi and
// __sincospi_stret, fegetexceptflag/fesetexceptflag, and the complex conj/cproj/creal/cimag.
// Every name is installed only where no provider exports it yet (some float forms already exist).
template<class R> void install_precision_extra(ProcessContext& context, const std::string& suffix) {
    // Float and double forms other providers own (leftovers.cpp, machsys.cpp, libsystem_misc.cpp, support.cpp).
    static const std::array<std::string_view, 7> owned_elsewhere{"_nearbyintf", "_fmaf", "_remainder", "_remainderf", "_ilogbf", "_nanf", "_nan"};
    const auto missing = [&](const std::string& name) {
        const auto symbol = "_" + name + suffix;
        return std::find(owned_elsewhere.begin(), owned_elsewhere.end(), symbol) == owned_elsewhere.end() &&
            !context.resolve("/usr/lib/libSystem.B.dylib", symbol);
    };
    const auto install = [&](const std::string& name, transmute::GuestCallHandler handler) {
        if (missing(name)) static_cast<void>(context.install_function("_" + name + suffix, numeric_floating_handler(std::move(handler))));
    };
    const auto integer = [](std::int64_t value) { return static_cast<std::uint64_t>(value); };
    // Rounding to long / long long: the current mode (lrint, llrint) or half away from zero (lround, llround).
    install("lround", [integer](auto& s, auto&) { s.x[0] = integer(std::llround(argument<R>(s, 0))); });
    install("llround", [integer](auto& s, auto&) { s.x[0] = integer(std::llround(argument<R>(s, 0))); });
    install("lrint", [integer](auto& s, auto&) { s.x[0] = integer(std::llrint(argument<R>(s, 0))); });
    install("llrint", [integer](auto& s, auto&) { s.x[0] = integer(std::llrint(argument<R>(s, 0))); });
    install("nearbyint", [](auto& s, auto&) { result(s, std::nearbyint(argument<R>(s, 0))); });
    install("fmax", [](auto& s, auto&) { result(s, std::fmax(argument<R>(s, 0), argument<R>(s, 1))); });
    install("fmin", [](auto& s, auto&) { result(s, std::fmin(argument<R>(s, 0), argument<R>(s, 1))); });
    install("fdim", [](auto& s, auto&) { result(s, std::fdim(argument<R>(s, 0), argument<R>(s, 1))); });
    install("fma", [](auto& s, auto&) { result(s, std::fma(argument<R>(s, 0), argument<R>(s, 1), argument<R>(s, 2))); });
    install("tgamma", [](auto& s, auto&) { result(s, std::tgamma(argument<R>(s, 0))); });
    install("remainder", [](auto& s, auto&) { result(s, std::remainder(argument<R>(s, 0), argument<R>(s, 1))); });
    // remquo(x, y, int* quo): the low bits of the quotient with its sign.
    install("remquo", [](auto& s, auto& m) {
        int quotient{};
        const auto value = std::remquo(argument<R>(s, 0), argument<R>(s, 1), &quotient);
        if (s.x[0]) m.write32(s.x[0], static_cast<std::uint32_t>(quotient));
        result(s, value);
    });
    install("ilogb", [](auto& s, auto&) { s.x[0] = static_cast<std::uint32_t>(std::ilogb(argument<R>(s, 0))); });
    install("scalbln", [](auto& s, auto&) { result(s, std::scalbln(argument<R>(s, 0), static_cast<long>(std::clamp<std::int64_t>(static_cast<std::int64_t>(s.x[0]), std::numeric_limits<long>::min(), std::numeric_limits<long>::max())))); });
    install("nexttoward", [](auto& s, auto&) {
        if constexpr (sizeof(R) == 4) result(s, static_cast<R>(std::nexttoward(argument<R>(s, 0), static_cast<long double>(argument<double>(s, 1)))));
        else result(s, std::nextafter(argument<R>(s, 0), argument<R>(s, 1)));
    });
    // lgamma_r(x, int* signgamp): ln|Γ(x)| and Γ's sign (negative for x in (-2k-1, -2k)).
    if (const auto name = "_lgamma" + suffix + "_r"; !context.resolve("/usr/lib/libSystem.B.dylib", name))
        static_cast<void>(context.install_function(name, numeric_floating_handler([](auto& s, auto& m) {
            const auto x = argument<R>(s, 0);
            std::int32_t sign = 1;
            if (x < R{} && std::floor(x) != x && static_cast<std::int64_t>(std::floor(x)) % 2 != 0) sign = -1;
            if (s.x[0]) m.write32(s.x[0], static_cast<std::uint32_t>(sign));
            result(s, std::lgamma(x));
        })));
    // nan(const char* tag): a quiet NaN (the tag's payload is not encoded).
    install("nan", [](auto& s, auto&) { result(s, std::numeric_limits<R>::quiet_NaN()); });
}

template<class R> R sin_pi(R x) {
    if (!std::isfinite(x)) return std::numeric_limits<R>::quiet_NaN();
    const auto r = std::remainder(x, R{2});   // in [-1, 1]
    if (r == R{} || std::fabs(r) == R{1}) return std::copysign(R{}, x);
    if (std::fabs(r) == R{0.5}) return std::copysign(R{1}, r);
    return static_cast<R>(std::sin(static_cast<long double>(r) * 3.14159265358979323846264338327950288L));
}
template<class R> R cos_pi(R x) {
    if (!std::isfinite(x)) return std::numeric_limits<R>::quiet_NaN();
    const auto r = std::fabs(std::remainder(x, R{2}));
    if (r == R{0.5}) return R{};
    if (r == R{}) return R{1};
    if (r == R{1}) return R{-1};
    return static_cast<R>(std::cos(static_cast<long double>(r) * 3.14159265358979323846264338327950288L));
}

void install_numeric_extra(ProcessContext& context) {
    install_precision_extra<float>(context, "f"); install_precision_extra<double>(context, ""); install_precision_extra<double>(context, "l");
    const auto missing = [&](std::string_view name) { return !context.resolve("/usr/lib/libSystem.B.dylib", name); };
    const auto install = [&](std::string name, transmute::GuestCallHandler handler) {
        if (missing(name)) static_cast<void>(context.install_function(std::move(name), numeric_floating_handler(std::move(handler))));
    };
    // Bessel functions of the first (j) and second (y) kind; y of x < 0 is NaN (invalid), of 0 is -inf.
    const auto bessel_j = [](unsigned order, double x) {
        const auto value = std::cyl_bessel_j(static_cast<double>(order), std::fabs(x));
        return (x < 0 && (order & 1)) ? -value : value;
    };
    const auto bessel_y = [](unsigned order, double x) {
        if (std::isnan(x) || x < 0) return std::numeric_limits<double>::quiet_NaN();
        if (x == 0) return -std::numeric_limits<double>::infinity();
        return std::cyl_neumann(static_cast<double>(order), x);
    };
    install("_j0", [bessel_j](auto& s, auto&) { result(s, bessel_j(0, argument<double>(s, 0))); });
    install("_j1", [bessel_j](auto& s, auto&) { result(s, bessel_j(1, argument<double>(s, 0))); });
    install("_jn", [bessel_j](auto& s, auto&) {
        const auto order = static_cast<std::int32_t>(s.x[0]);
        const auto value = bessel_j(static_cast<unsigned>(std::abs(order)), argument<double>(s, 0));
        result(s, (order < 0 && (std::abs(order) & 1)) ? -value : value);
    });
    install("_y0", [bessel_y](auto& s, auto&) { result(s, bessel_y(0, argument<double>(s, 0))); });
    install("_y1", [bessel_y](auto& s, auto&) { result(s, bessel_y(1, argument<double>(s, 0))); });
    install("_yn", [bessel_y](auto& s, auto&) {
        const auto order = static_cast<std::int32_t>(s.x[0]);
        const auto value = bessel_y(static_cast<unsigned>(std::abs(order)), argument<double>(s, 0));
        result(s, (order < 0 && (std::abs(order) & 1)) ? -value : value);
    });
    // BSD: gamma is lgamma, drem is remainder, significand(x) is x scaled into [1, 2), finite is isfinite.
    install("_gamma", [](auto& s, auto&) { result(s, std::lgamma(argument<double>(s, 0))); });
    install("_drem", [](auto& s, auto&) { result(s, std::remainder(argument<double>(s, 0), argument<double>(s, 1))); });
    install("_significand", [](auto& s, auto&) {
        const auto x = argument<double>(s, 0);
        result(s, (x == 0 || !std::isfinite(x)) ? x : std::scalbn(x, -std::ilogb(x)));
    });
    install("_finite", [](auto& s, auto&) { s.x[0] = std::isfinite(argument<double>(s, 0)) ? 1 : 0; });
    // sin(πx), cos(πx), tan(πx), exact at integers and half-integers.
    install("___sinpi", [](auto& s, auto&) { result(s, sin_pi(argument<double>(s, 0))); });
    install("___sinpif", [](auto& s, auto&) { result(s, sin_pi(argument<float>(s, 0))); });
    install("___cospi", [](auto& s, auto&) { result(s, cos_pi(argument<double>(s, 0))); });
    install("___cospif", [](auto& s, auto&) { result(s, cos_pi(argument<float>(s, 0))); });
    install("___tanpi", [](auto& s, auto&) { const auto x = argument<double>(s, 0); result(s, sin_pi(x) / cos_pi(x)); });
    install("___tanpif", [](auto& s, auto&) { const auto x = argument<float>(s, 0); result(s, sin_pi(x) / cos_pi(x)); });
    install("___sincospi_stret", [](auto& s, auto&) { const auto x = argument<double>(s, 0); result(s, sin_pi(x)); result(s, cos_pi(x), 1); });
    install("___sincospif_stret", [](auto& s, auto&) { const auto x = argument<float>(s, 0); result(s, sin_pi(x)); result(s, cos_pi(x), 1); });
    if (missing("___fpclassifyl"))
        if (const auto found = context.resolve("/usr/lib/libSystem.B.dylib", "___fpclassifyd"))
            context.install_export("/usr/lib/libSystem.B.dylib", "___fpclassifyl", *found);
    // fegetexceptflag(fexcept_t* flagp, int excepts) / fesetexceptflag(const fexcept_t*, int):
    // fexcept_t (16 bits) holds Darwin's exception bits; setting them raises no trap.
    if (missing("_fegetexceptflag"))
        static_cast<void>(context.install_function("_fegetexceptflag", [](auto& s, auto& m) {
            const auto flags = exception_flags(s.fpsr) & static_cast<std::uint32_t>(s.x[1]);
            m.write8(s.x[0], static_cast<std::uint8_t>(flags)); m.write8(s.x[0] + 1, static_cast<std::uint8_t>(flags >> 8));
            s.x[0] = 0;
        }));
    if (missing("_fesetexceptflag"))
        static_cast<void>(context.install_function("_fesetexceptflag", [](auto& s, auto& m) {
            const auto excepts = static_cast<std::uint32_t>(s.x[1]);
            const auto flags = (static_cast<std::uint32_t>(m.read8(s.x[0])) | static_cast<std::uint32_t>(m.read8(s.x[0] + 1)) << 8) & excepts;
            s.fpsr = (s.fpsr & ~exception_status(excepts)) | exception_status(flags);
            s.x[0] = 0;
        }));
    // conj, cproj (an infinite part projects to (+inf, ±0)), creal, cimag.
    install("_conj", [](auto& s, auto&) { complex_result(s, std::conj(complex_argument<double>(s))); });
    install("_conjf", [](auto& s, auto&) { complex_result(s, std::conj(complex_argument<float>(s))); });
    install("_cproj", [](auto& s, auto&) { complex_result(s, std::proj(complex_argument<double>(s))); });
    install("_cprojf", [](auto& s, auto&) { complex_result(s, std::proj(complex_argument<float>(s))); });
    install("_creal", [](auto& s, auto&) { result(s, complex_argument<double>(s).real()); });
    install("_crealf", [](auto& s, auto&) { result(s, complex_argument<float>(s).real()); });
    install("_cimag", [](auto& s, auto&) { result(s, complex_argument<double>(s).imag()); });
    install("_cimagf", [](auto& s, auto&) { result(s, complex_argument<float>(s).imag()); });
}

} // namespace

transmute::GuestCallHandler numeric_floating_handler(transmute::GuestCallHandler handler) {
    return [handler = std::move(handler)](auto& state, auto& memory) {
        std::uint32_t status{};
        { FloatingEnvironment environment(state.fpcr); handler(state, memory); status = environment.status(); }
        merge_status(state, status);
    };
}

void install_darwin_numeric_math(ProcessContext& context) {
    install_precision<float>(context, "f"); install_precision<double>(context, ""); install_precision<double>(context, "l");
    static_cast<void>(context.install_function("___sincosf_stret", numeric_floating_handler([](auto& s, auto&) {
        const auto input = argument<float>(s, 0); result(s, std::sin(input)); result(s, std::cos(input), 1);
    })));
    static_cast<void>(context.install_function("___sincos_stret", numeric_floating_handler([](auto& s, auto&) {
        const auto input = argument<double>(s, 0); result(s, std::sin(input)); result(s, std::cos(input), 1);
    })));
    // Darwin's five C exception bits have the same positions as AArch64 FPSR;
    // its FE_FLUSHTOZERO bit 5 maps to the architecture's input-denormal bit 7.
    // Operate on the calling guest CPU, not on the host thread's FP environment.
    static_cast<void>(context.install_function("_feclearexcept", [](auto& s, auto&) { s.fpsr &= ~exception_status(static_cast<std::uint32_t>(s.x[0])); s.x[0] = 0; }));
    static_cast<void>(context.install_function("_feraiseexcept", [](auto& s, auto&) {
        const auto status = exception_status(static_cast<std::uint32_t>(s.x[0]));
        if ((status << 8) & s.fpcr) throw std::runtime_error("Alchemy numeric: enabled floating-point exception trap requires guest signal delivery");
        s.fpsr |= status; s.x[0] = 0;
    }));
    static_cast<void>(context.install_function("_fetestexcept", [](auto& s, auto&) { s.x[0] &= exception_flags(s.fpsr); }));
    // Darwin arm64 fenv_t is {uint64 __fpsr; uint64 __fpcr}; FE_DFL_ENV is the zeroed _FE_DFL_ENV.
    const auto default_environment = context.allocate_guest(16);
    context.calling_engine().write64(default_environment, 0); context.calling_engine().write64(default_environment + 8, 0);
    context.install_export("/usr/lib/libSystem.B.dylib", "__FE_DFL_ENV", {default_environment, ExportKind::data});
    static_cast<void>(context.install_function("_fegetenv", [](auto& s, auto& m) {
        m.write64(s.x[0], s.fpsr); m.write64(s.x[0] + 8, s.fpcr); s.x[0] = 0;
    }));
    static_cast<void>(context.install_function("_fesetenv", [](auto& s, auto& m) {
        s.fpsr = static_cast<std::uint32_t>(m.read64(s.x[0])); s.fpcr = static_cast<std::uint32_t>(m.read64(s.x[0] + 8)); s.x[0] = 0;
    }));
    static_cast<void>(context.install_function("_feholdexcept", [](auto& s, auto& m) {
        // Save, clear the flags and disable every trap enable (FPCR bits 8-12 and 15).
        m.write64(s.x[0], s.fpsr); m.write64(s.x[0] + 8, s.fpcr);
        s.fpsr &= ~0x9fU; s.fpcr &= ~0x9f00U; s.x[0] = 0;
    }));
    static_cast<void>(context.install_function("_feupdateenv", [](auto& s, auto& m) {
        const auto raised = s.fpsr & 0x9fU;
        s.fpsr = static_cast<std::uint32_t>(m.read64(s.x[0])); s.fpcr = static_cast<std::uint32_t>(m.read64(s.x[0] + 8));
        if ((raised << 8) & s.fpcr) throw std::runtime_error("Alchemy numeric: enabled floating-point exception trap requires guest signal delivery");
        s.fpsr |= raised; s.x[0] = 0;
    }));
    static_cast<void>(context.install_function("_fegetround", [](auto& s, auto&) { s.x[0] = s.fpcr & 0x00c00000U; }));
    static_cast<void>(context.install_function("_fesetround", [](auto& s, auto&) {
        if (s.x[0] & ~0x00c00000ULL) { s.x[0] = 1; return; }
        s.fpcr = (s.fpcr & ~0x00c00000U) | static_cast<std::uint32_t>(s.x[0]); s.x[0] = 0;
    }));
    // nexttoward's long double direction is binary64 on Apple arm64.
    static_cast<void>(context.install_function("_nexttowardf", numeric_floating_handler([](auto& s, auto&) {
        result(s, static_cast<float>(std::nexttoward(argument<float>(s, 0), static_cast<long double>(argument<double>(s, 1)))));
    })));
    static_cast<void>(context.install_function("_nexttoward", numeric_floating_handler([](auto& s, auto&) {
        result(s, std::nextafter(argument<double>(s, 0), argument<double>(s, 1)));
    })));
    // Classification helpers behind <math.h> macros: FP_NAN 1, FP_INFINITE 2, FP_ZERO 3, FP_NORMAL 4, FP_SUBNORMAL 5.
    const auto classify = [](auto value) -> std::uint64_t {
        switch (std::fpclassify(value)) { case FP_NAN: return 1; case FP_INFINITE: return 2; case FP_ZERO: return 3; case FP_SUBNORMAL: return 5; default: return 4; }
    };
    for (const auto suffix : {"d", "f"}) {
        const bool single = std::string_view(suffix) == "f";
        const auto value = [single](const transmute::CpuState& s) { return single ? static_cast<double>(argument<float>(s, 0)) : argument<double>(s, 0); };
        static_cast<void>(context.install_function(std::string("___fpclassify") + suffix, [value, classify, single](auto& s, auto&) {
            s.x[0] = single ? classify(argument<float>(s, 0)) : classify(value(s));
        }));
        static_cast<void>(context.install_function(std::string("___isfinite") + suffix, [value](auto& s, auto&) { s.x[0] = std::isfinite(value(s)) ? 1 : 0; }));
        static_cast<void>(context.install_function(std::string("___isinf") + suffix, [value](auto& s, auto&) { s.x[0] = std::isinf(value(s)) ? 1 : 0; }));
        static_cast<void>(context.install_function(std::string("___isnan") + suffix, [value](auto& s, auto&) { s.x[0] = std::isnan(value(s)) ? 1 : 0; }));
        static_cast<void>(context.install_function(std::string("___isnormal") + suffix, [single](auto& s, auto&) {
            s.x[0] = (single ? std::isnormal(argument<float>(s, 0)) : std::isnormal(argument<double>(s, 0))) ? 1 : 0;
        }));
        static_cast<void>(context.install_function(std::string("___signbit") + suffix, [value](auto& s, auto&) { s.x[0] = std::signbit(value(s)) ? 1 : 0; }));
    }
    static_cast<void>(context.install_function("___udivti3", [](auto& s, auto&) {
        const Wide numerator{s.x[0], s.x[1]}, denominator{s.x[2], s.x[3]};
        if (!(denominator.low | denominator.high)) throw std::runtime_error("Alchemy numeric: unsigned 128-bit division by zero");
        Wide remainder{}, quotient{};
        for (unsigned bit = 128; bit-- > 0;) {
            const auto carry = remainder.high >> 63;
            remainder.high = (remainder.high << 1) | (remainder.low >> 63);
            remainder.low = (remainder.low << 1) | ((bit < 64 ? numerator.low >> bit : numerator.high >> (bit - 64)) & 1);
            if (carry || greater_equal(remainder, denominator)) {
                remainder = subtract(remainder, denominator);
                if (bit < 64) quotient.low |= 1ULL << bit; else quotient.high |= 1ULL << (bit - 64);
            }
        }
        s.x[0] = quotient.low; s.x[1] = quotient.high;
    }));
    install_numeric_extra(context);
}

} // namespace alchemy
