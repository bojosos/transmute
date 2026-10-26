#include "alchemy/darwin/accelerate/install.hpp"
#include "alchemy/gui/fill_provider.hpp"

#include <bit>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <type_traits>
#include <vector>

// vForce (vForce.h): element-wise math over arrays, `const int *n` elements, single precision with
// an "f" suffix and double without, computed with the host's C library in the element type.
// - One input (y, x, n): sqrt, rsqrt, rec, exp, exp2, expm1, log, log2, log10, log1p, logb, fabs,
//   floor, ceil, int (toward zero), nint (to nearest, ties to even), sin, cos, tan, asin, acos,
//   atan, sinh, cosh, tanh, asinh, acosh, atanh, sinpi, cospi, tanpi, cbrt.
// - Two inputs (z, y, x, n): pow (z = x^y), atan2 (z = atan2(y, x)), div (z = y / x), fmod
//   (z = fmod(y, x)), remainder, copysign (z = copysign(y, x)), nextafter (from y toward x).
// - powsf/pows (z, const T *y, x, n): z = x^y for one exponent; sincos (s, c, x, n); cosisin
//   (complex z = cos x + i sin x, interleaved).
namespace alchemy::gui::fill {
namespace {

constexpr std::int64_t maximum_length = 1LL << 26;

template <class T> std::vector<T> load(GuestMemory& m, Address base, std::int64_t n) {
    std::vector<T> values(static_cast<std::size_t>(n));
    if (n > 0) m.read_bytes(base, std::span(reinterpret_cast<std::uint8_t*>(values.data()), values.size() * sizeof(T)));
    return values;
}
template <class T> void store(GuestMemory& m, Address base, const std::vector<T>& values) {
    if (!values.empty()) m.write_bytes(base, std::span(reinterpret_cast<const std::uint8_t*>(values.data()), values.size() * sizeof(T)));
}
std::int64_t count_at(GuestMemory& m, Address n) {
    const auto value = n ? static_cast<std::int32_t>(m.read32(n)) : 0;
    if (value > maximum_length) throw std::runtime_error("unsupported: Alchemy vForce: more than 2^26 elements");
    return value < 0 ? 0 : value;
}

template <class T> T pi_times(T x) { return static_cast<T>(std::numbers::pi * static_cast<double>(x)); }

class VForce final : public Provider {
public:
    using Provider::Provider;
    void install() {
        install_type<float>();
        install_type<double>();
    }

private:
    template <class T> std::string named(const char* stem) { return std::string("_vv") + stem + (std::is_same_v<T, float> ? "f" : ""); }
    void kernel(const std::string& name, transmute::GuestCallHandler handler, const std::source_location& where = std::source_location::current()) {
        if (has(accelerate, name)) return;
        raw(accelerate, name, std::move(handler), where);
    }
    template <class T> void install_type();
};

template <class T>
void VForce::install_type() {
    const auto unary = [this](const char* stem, auto f) {
        kernel(named<T>(stem), [f](CpuState& s, GuestMemory& m) {   // (T *y, const T *x, const int *n)
            auto x = load<T>(m, s.x[1], count_at(m, s.x[2]));
            for (auto& v : x) v = static_cast<T>(f(v));
            store<T>(m, s.x[0], x);
        });
    };
    unary("sqrt", [](T x) { return std::sqrt(x); });
    unary("rsqrt", [](T x) { return T(1) / std::sqrt(x); });
    unary("rec", [](T x) { return T(1) / x; });
    unary("exp", [](T x) { return std::exp(x); });
    unary("exp2", [](T x) { return std::exp2(x); });
    unary("expm1", [](T x) { return std::expm1(x); });
    unary("log", [](T x) { return std::log(x); });
    unary("log2", [](T x) { return std::log2(x); });
    unary("log10", [](T x) { return std::log10(x); });
    unary("log1p", [](T x) { return std::log1p(x); });
    unary("logb", [](T x) { return std::logb(x); });
    unary("fabs", [](T x) { return std::fabs(x); });
    unary("floor", [](T x) { return std::floor(x); });
    unary("ceil", [](T x) { return std::ceil(x); });
    unary("int", [](T x) { return std::trunc(x); });
    unary("nint", [](T x) { return std::nearbyint(x); });
    unary("sin", [](T x) { return std::sin(x); });
    unary("cos", [](T x) { return std::cos(x); });
    unary("tan", [](T x) { return std::tan(x); });
    unary("asin", [](T x) { return std::asin(x); });
    unary("acos", [](T x) { return std::acos(x); });
    unary("atan", [](T x) { return std::atan(x); });
    unary("sinh", [](T x) { return std::sinh(x); });
    unary("cosh", [](T x) { return std::cosh(x); });
    unary("tanh", [](T x) { return std::tanh(x); });
    unary("asinh", [](T x) { return std::asinh(x); });
    unary("acosh", [](T x) { return std::acosh(x); });
    unary("atanh", [](T x) { return std::atanh(x); });
    unary("sinpi", [](T x) { return std::sin(pi_times(x)); });
    unary("cospi", [](T x) { return std::cos(pi_times(x)); });
    unary("tanpi", [](T x) { return std::tan(pi_times(x)); });
    unary("cbrt", [](T x) { return std::cbrt(x); });
    const auto binary = [this](const char* stem, auto f) {
        kernel(named<T>(stem), [f](CpuState& s, GuestMemory& m) {   // (T *z, const T *y, const T *x, const int *n)
            const auto n = count_at(m, s.x[3]);
            auto y = load<T>(m, s.x[1], n);
            const auto x = load<T>(m, s.x[2], n);
            for (std::size_t i = 0; i < y.size(); ++i) y[i] = static_cast<T>(f(y[i], x[i]));
            store<T>(m, s.x[0], y);
        });
    };
    binary("pow", [](T y, T x) { return std::pow(x, y); });
    binary("atan2", [](T y, T x) { return std::atan2(y, x); });
    binary("div", [](T y, T x) { return y / x; });
    binary("fmod", [](T y, T x) { return std::fmod(y, x); });
    binary("remainder", [](T y, T x) { return std::remainder(y, x); });
    binary("copysign", [](T y, T x) { return std::copysign(y, x); });
    binary("nextafter", [](T y, T x) { return std::nextafter(y, x); });
    // pows(f) (T *z, const T *y, const T *x, const int *n): z = x^y for the one exponent *y.
    kernel(named<T>("pows"), [](CpuState& s, GuestMemory& m) {
        auto x = load<T>(m, s.x[2], count_at(m, s.x[3]));
        T y{};
        if constexpr (std::is_same_v<T, float>) y = std::bit_cast<float>(m.read32(s.x[1]));
        else y = std::bit_cast<double>(m.read64(s.x[1]));
        for (auto& v : x) v = static_cast<T>(std::pow(v, y));
        store<T>(m, s.x[0], x);
    });
    // sincos(f) (T *s, T *c, const T *x, const int *n)
    kernel(named<T>("sincos"), [](CpuState& s, GuestMemory& m) {
        const auto x = load<T>(m, s.x[2], count_at(m, s.x[3]));
        std::vector<T> sines(x.size()), cosines(x.size());
        for (std::size_t i = 0; i < x.size(); ++i) { sines[i] = std::sin(x[i]); cosines[i] = std::cos(x[i]); }
        store<T>(m, s.x[0], sines);
        store<T>(m, s.x[1], cosines);
    });
    // cosisin(f) (complex T *z, const T *x, const int *n): z = cos x + i sin x.
    kernel(named<T>("cosisin"), [](CpuState& s, GuestMemory& m) {
        const auto x = load<T>(m, s.x[1], count_at(m, s.x[2]));
        std::vector<T> z(2 * x.size());
        for (std::size_t i = 0; i < x.size(); ++i) { z[2 * i] = std::cos(x[i]); z[2 * i + 1] = std::sin(x[i]); }
        store<T>(m, s.x[0], z);
    });
}

} // namespace

void install_vforce(InstallContext& context) { std::make_shared<VForce>(context)->install(); }

} // namespace alchemy::gui::fill
