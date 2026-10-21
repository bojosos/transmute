#include "alchemy/darwin_numeric.hpp"

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
}

} // namespace alchemy
