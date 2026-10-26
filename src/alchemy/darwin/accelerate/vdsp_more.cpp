#include "alchemy/darwin/accelerate/install.hpp"
#include "alchemy/gui/fill_provider.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <functional>
#include <numeric>
#include <string>
#include <type_traits>
#include <vector>

// More of vDSP, in single precision and (the "D" names) double, beyond vdsp.cpp and
// vdsp_vectors.cpp, computed in double on the host per the documented formulas:
// - Compound arithmetic: vasm, vsbm, vsbsm, vaam, vasbm, vmma, vmmsb, vmsb, vsmsb, vintb, vaddsub,
//   vpythg, vdist, distancesq, dotpr2.
// - Ramps applied: vrampmul, vrampmuladd (and the two-channel forms), updating *Start.
// - Clipping and envelopes: vclipc (with the counts), viclip, venvlp.
// - Sums and integrals: vrsum, vswsum, vtrapz, vsimps, sve_svesq, svs, mvessq, meamgv; normalize;
//   nzcros.
// - Rearranging: vswap, vsort, vsorti, vtmerg, vgathr (1-based indices), vgathra, vindex, mmov,
//   vtabi, vpoly, vlint (D), polar, rect.
// - Filters: deq22 (float), conv (D), f3x3, f5x5, imgfir (outer rows and columns zero).
// - Split complex: zvdiv (B first, as vDSP takes it), zvneg, zvfill, zvzsml, zvsma, zvcma (conjugated
//   A), zrvadd/zrvsub/zrvmul/zrvdiv (a real B), zdotpr, zidotpr (conjugated A), zaspec, zcspec
//   (accumulating), zmmul.
// - 2-D FFTs: fft2d_zip and fft2d_zop (rows of 2^log2N0 elements, 2^log2N1 rows; the row stride
//   defaults to the row length); fft_zipt/zript take their temporary buffer and run as fft_zip/zrip.
// Lengths over 2^26 stop explicitly.
namespace alchemy::gui::fill {
namespace {

constexpr std::uint64_t maximum_length = 1ULL << 26;

void check_length(std::uint64_t count) {
    if (count > maximum_length) throw std::runtime_error("unsupported: Alchemy vDSP: vector longer than 2^26 elements");
}
// The i-th argument: x0-x7, then 8-byte stack slots (vDSP's arguments are pointers and longs).
std::uint64_t arg(const CpuState& s, GuestMemory& m, unsigned i) { return i < 8 ? s.x[i] : m.read64(s.sp + 8ULL * (i - 8)); }
std::int64_t stride(std::uint64_t value) { return static_cast<std::int64_t>(value); }

template <class T> double get(GuestMemory& m, Address at) {
    if constexpr (std::is_same_v<T, float>) return std::bit_cast<float>(m.read32(at));
    else return std::bit_cast<double>(m.read64(at));
}
template <class T> void put(GuestMemory& m, Address at, double value) {
    if constexpr (std::is_same_v<T, float>) m.write32(at, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
    else m.write64(at, std::bit_cast<std::uint64_t>(value));
}
template <class T> Address element(Address base, std::int64_t step, std::uint64_t index) {
    return base + static_cast<Address>(static_cast<std::int64_t>(index) * step * static_cast<std::int64_t>(sizeof(T)));
}
template <class T> std::vector<double> load(GuestMemory& m, Address base, std::int64_t step, std::uint64_t n) {
    check_length(n);
    std::vector<double> out(static_cast<std::size_t>(n));
    for (std::uint64_t i = 0; i < n; ++i) out[static_cast<std::size_t>(i)] = get<T>(m, element<T>(base, step, i));
    return out;
}
template <class T> void store(GuestMemory& m, Address base, std::int64_t step, const std::vector<double>& values) {
    for (std::size_t i = 0; i < values.size(); ++i) put<T>(m, element<T>(base, step, i), values[i]);
}
// A DSPSplitComplex (realp, imagp) as complex values.
template <class T> std::vector<std::complex<double>> load_split(GuestMemory& m, Address split, std::int64_t step, std::uint64_t n) {
    const auto re = load<T>(m, m.read64(split), step, n), im = load<T>(m, m.read64(split + 8), step, n);
    std::vector<std::complex<double>> out(re.size());
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = {re[i], im[i]};
    return out;
}
template <class T> void store_split(GuestMemory& m, Address split, std::int64_t step, const std::vector<std::complex<double>>& values) {
    const auto re = m.read64(split), im = m.read64(split + 8);
    for (std::size_t i = 0; i < values.size(); ++i) {
        put<T>(m, element<T>(re, step, i), values[i].real());
        put<T>(m, element<T>(im, step, i), values[i].imag());
    }
}
template <class T> std::complex<double> load_split_scalar(GuestMemory& m, Address split) {
    return {get<T>(m, m.read64(split)), get<T>(m, m.read64(split + 8))};
}

// Radix-2 complex FFT in place (forward: e^{-i...}; unnormalized both ways).
void fft(std::vector<std::complex<double>>& a, bool inverse) {
    const auto n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = 2 * std::acos(-1.0) / static_cast<double>(length) * (inverse ? 1 : -1);
        for (std::size_t i = 0; i < n; i += length)
            for (std::size_t j = 0; j < length / 2; ++j) {
                const std::complex<double> w(std::cos(angle * static_cast<double>(j)), std::sin(angle * static_cast<double>(j)));
                const auto u = a[i + j], v = a[i + j + length / 2] * w;
                a[i + j] = u + v;
                a[i + j + length / 2] = u - v;
            }
    }
}

class More final : public Provider {
public:
    using Provider::Provider;
    void install() {
        install_type<float>();
        install_type<double>();
    }

private:
    void kernel(const std::string& name, transmute::GuestCallHandler handler, const std::source_location& where = std::source_location::current()) {
        if (has(accelerate, name)) return;
        raw(accelerate, name, std::move(handler), where);
    }
    template <class T> static std::string named(const char* stem) { return std::string("_vDSP_") + stem + (std::is_same_v<T, double> ? "D" : ""); }
    template <class T> void install_type();
    template <class T> void install_complex();
    template <class T> void install_filters();
};

// Element-wise forms over vectors (strided) and scalars (by reference): `inputs` lists, per input,
// whether it is a vector ('v', two arguments) or a scalar ('s', one); then the output vector and N.
template <class T, class F> transmute::GuestCallHandler elementwise(std::string inputs, F f) {
    return [inputs = std::move(inputs), f](CpuState& s, GuestMemory& m) {
        unsigned i = 0;
        struct In { Address base; std::int64_t step; bool vector; };
        std::vector<In> in;
        for (const char kind : inputs) {
            if (kind == 'v') { in.push_back({arg(s, m, i), stride(arg(s, m, i + 1)), true}); i += 2; }
            else { in.push_back({arg(s, m, i), 0, false}); i += 1; }
        }
        const Address out = arg(s, m, i), out_step = arg(s, m, i + 1);
        const auto n = arg(s, m, i + 2);
        check_length(n);
        std::array<double, 4> scalars{};
        for (std::size_t k = 0; k < in.size(); ++k)
            if (!in[k].vector) scalars[k] = get<T>(m, in[k].base);
        for (std::uint64_t e = 0; e < n; ++e) {
            std::array<double, 4> v{};
            for (std::size_t k = 0; k < in.size(); ++k) v[k] = in[k].vector ? get<T>(m, element<T>(in[k].base, in[k].step, e)) : scalars[k];
            put<T>(m, element<T>(out, stride(out_step), e), f(v));
        }
    };
}

template <class T>
void More::install_type() {
    using V = const std::array<double, 4>&;
    kernel(named<T>("vasm"), elementwise<T>("vvs", [](V v) { return (v[0] + v[1]) * v[2]; }));
    kernel(named<T>("vsbm"), elementwise<T>("vvv", [](V v) { return (v[0] - v[1]) * v[2]; }));
    kernel(named<T>("vsbsm"), elementwise<T>("vvs", [](V v) { return (v[0] - v[1]) * v[2]; }));
    kernel(named<T>("vaam"), elementwise<T>("vvvv", [](V v) { return (v[0] + v[1]) * (v[2] + v[3]); }));
    kernel(named<T>("vasbm"), elementwise<T>("vvvv", [](V v) { return (v[0] + v[1]) * (v[2] - v[3]); }));
    kernel(named<T>("vmma"), elementwise<T>("vvvv", [](V v) { return v[0] * v[1] + v[2] * v[3]; }));
    kernel(named<T>("vmmsb"), elementwise<T>("vvvv", [](V v) { return v[0] * v[1] - v[2] * v[3]; }));
    kernel(named<T>("vmsb"), elementwise<T>("vvv", [](V v) { return v[0] * v[1] - v[2]; }));
    kernel(named<T>("vsmsb"), elementwise<T>("vsv", [](V v) { return v[0] * v[1] - v[2]; }));
    kernel(named<T>("vintb"), elementwise<T>("vvs", [](V v) { return v[0] + v[2] * (v[1] - v[0]); }));
    kernel(named<T>("vpythg"), elementwise<T>("vvvv", [](V v) { return std::hypot(v[0] - v[2], v[1] - v[3]); }));
    kernel(named<T>("vdist"), elementwise<T>("vv", [](V v) { return std::hypot(v[0], v[1]); }));
    kernel(named<T>("venvlp"), elementwise<T>("vvv", [](V v) { return v[2] > v[0] || v[2] < v[1] ? v[2] : 0.0; }));
    // vaddsub(I0, I0S, I1, I1S, O0, O0S, O1, O1S, N): O0 = I1 + I0, O1 = I1 - I0.
    kernel(named<T>("vaddsub"), [](CpuState& s, GuestMemory& m) {
        const auto n = arg(s, m, 8);
        const auto a = load<T>(m, s.x[0], stride(s.x[1]), n), b = load<T>(m, s.x[2], stride(s.x[3]), n);
        std::vector<double> sum(a.size()), difference(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) { sum[i] = b[i] + a[i]; difference[i] = b[i] - a[i]; }
        store<T>(m, s.x[4], stride(s.x[5]), sum);
        store<T>(m, s.x[6], stride(s.x[7]), difference);
    });
    // distancesq(A, IA, B, IB, C*, N) and dotpr2(A0, IA0, A1, IA1, B, IB, C0*, C1*, N)
    kernel(named<T>("distancesq"), [](CpuState& s, GuestMemory& m) {
        const auto a = load<T>(m, s.x[0], stride(s.x[1]), s.x[5]), b = load<T>(m, s.x[2], stride(s.x[3]), s.x[5]);
        double sum = 0;
        for (std::size_t i = 0; i < a.size(); ++i) sum += (a[i] - b[i]) * (a[i] - b[i]);
        put<T>(m, s.x[4], sum);
    });
    kernel(named<T>("dotpr2"), [](CpuState& s, GuestMemory& m) {
        const auto n = arg(s, m, 8);
        const auto a0 = load<T>(m, s.x[0], stride(s.x[1]), n), a1 = load<T>(m, s.x[2], stride(s.x[3]), n), b = load<T>(m, s.x[4], stride(s.x[5]), n);
        double c0 = 0, c1 = 0;
        for (std::size_t i = 0; i < b.size(); ++i) { c0 += a0[i] * b[i]; c1 += a1[i] * b[i]; }
        put<T>(m, s.x[6], c0);
        put<T>(m, s.x[7], c1);
    });

    // ---- ramps ---------------------------------------------------------------------------------
    // vrampmul(I, IS, Start*, Step*, O, OS, N) and vrampmuladd: O = (O +) I * ramp; *Start advances.
    for (const bool adding : {false, true}) {
        kernel(named<T>(adding ? "vrampmuladd" : "vrampmul"), [adding](CpuState& s, GuestMemory& m) {
            const auto n = s.x[6];
            const auto in = load<T>(m, s.x[0], stride(s.x[1]), n);
            double start = get<T>(m, s.x[2]);
            const double step = get<T>(m, s.x[3]);
            for (std::uint64_t i = 0; i < n; ++i) {
                const auto at = element<T>(s.x[4], stride(s.x[5]), i);
                put<T>(m, at, (adding ? get<T>(m, at) : 0.0) + in[static_cast<std::size_t>(i)] * start);
                start += step;
            }
            put<T>(m, s.x[2], start);
        });
        // The two-channel forms: (I0, I1, IS, Start*, Step*, O0, O1, OS, N)
        kernel(named<T>(adding ? "vrampmuladd2" : "vrampmul2"), [adding](CpuState& s, GuestMemory& m) {
            const auto n = arg(s, m, 8);
            const auto in0 = load<T>(m, s.x[0], stride(s.x[2]), n), in1 = load<T>(m, s.x[1], stride(s.x[2]), n);
            double start = get<T>(m, s.x[3]);
            const double step = get<T>(m, s.x[4]);
            for (std::uint64_t i = 0; i < n; ++i) {
                const auto at0 = element<T>(s.x[5], stride(s.x[7]), i), at1 = element<T>(s.x[6], stride(s.x[7]), i);
                put<T>(m, at0, (adding ? get<T>(m, at0) : 0.0) + in0[static_cast<std::size_t>(i)] * start);
                put<T>(m, at1, (adding ? get<T>(m, at1) : 0.0) + in1[static_cast<std::size_t>(i)] * start);
                start += step;
            }
            put<T>(m, s.x[3], start);
        });
    }

    // ---- clipping ------------------------------------------------------------------------------
    // vclipc(A, IA, Low*, High*, D, ID, N, NLow*, NHigh*): the counts of values clipped each way.
    kernel(named<T>("vclipc"), [](CpuState& s, GuestMemory& m) {
        const auto n = s.x[6];
        const double low = get<T>(m, s.x[2]), high = get<T>(m, s.x[3]);
        auto a = load<T>(m, s.x[0], stride(s.x[1]), n);
        std::uint64_t below = 0, above = 0;
        for (auto& value : a) {
            if (value < low) { value = low; ++below; }
            else if (value > high) { value = high; ++above; }
        }
        store<T>(m, s.x[4], stride(s.x[5]), a);
        m.write64(s.x[7], below);
        m.write64(arg(s, m, 8), above);
    });
    // viclip(A, IA, Low*, High*, D, ID, N): values strictly inside (Low, High) go to the nearer bound
    // by sign (Low when negative, High otherwise).
    kernel(named<T>("viclip"), [](CpuState& s, GuestMemory& m) {
        const double low = get<T>(m, s.x[2]), high = get<T>(m, s.x[3]);
        auto a = load<T>(m, s.x[0], stride(s.x[1]), s.x[6]);
        for (auto& value : a)
            if (value > low && value < high) value = value < 0 ? low : high;
        store<T>(m, s.x[4], stride(s.x[5]), a);
    });

    // ---- sums and integrals --------------------------------------------------------------------
    // vrsum(A, IA, S*, C, IC, N): C[0] = 0, C[n] = C[n-1] + S * A[n].
    kernel(named<T>("vrsum"), [](CpuState& s, GuestMemory& m) {
        const auto a = load<T>(m, s.x[0], stride(s.x[1]), s.x[5]);
        const double scale = get<T>(m, s.x[2]);
        std::vector<double> c(a.size());
        for (std::size_t i = 1; i < a.size(); ++i) c[i] = c[i - 1] + scale * a[i];
        store<T>(m, s.x[3], stride(s.x[4]), c);
    });
    // vswsum(A, IA, C, IC, N, P): C[n] = A[n] + ... + A[n + P - 1].
    kernel(named<T>("vswsum"), [](CpuState& s, GuestMemory& m) {
        const auto n = s.x[4], p = s.x[5];
        check_length(n + p);
        const auto a = load<T>(m, s.x[0], stride(s.x[1]), n + (p ? p - 1 : 0));
        std::vector<double> c(static_cast<std::size_t>(n));
        for (std::size_t i = 0; i < c.size(); ++i)
            for (std::uint64_t k = 0; k < p; ++k) c[i] += a[i + k];
        store<T>(m, s.x[2], stride(s.x[3]), c);
    });
    // vtrapz and vsimps(A, IA, B*, C, IC, N): running integrals with step B.
    kernel(named<T>("vtrapz"), [](CpuState& s, GuestMemory& m) {
        const auto a = load<T>(m, s.x[0], stride(s.x[1]), s.x[5]);
        const double h = get<T>(m, s.x[2]);
        std::vector<double> c(a.size());
        for (std::size_t i = 1; i < a.size(); ++i) c[i] = c[i - 1] + h * (a[i - 1] + a[i]) / 2;
        store<T>(m, s.x[3], stride(s.x[4]), c);
    });
    kernel(named<T>("vsimps"), [](CpuState& s, GuestMemory& m) {
        const auto a = load<T>(m, s.x[0], stride(s.x[1]), s.x[5]);
        const double h = get<T>(m, s.x[2]);
        std::vector<double> c(a.size());
        if (a.size() > 1) c[1] = h * (a[0] + a[1]) / 2;
        for (std::size_t i = 2; i < a.size(); ++i) c[i] = c[i - 2] + h * (a[i - 2] + 4 * a[i - 1] + a[i]) / 3;
        store<T>(m, s.x[3], stride(s.x[4]), c);
    });
    // sve_svesq(A, IA, Sum*, SumOfSquares*, N); svs (signed squares), mvessq (their mean), meamgv.
    kernel(named<T>("sve_svesq"), [](CpuState& s, GuestMemory& m) {
        const auto a = load<T>(m, s.x[0], stride(s.x[1]), s.x[4]);
        double sum = 0, squares = 0;
        for (const auto v : a) { sum += v; squares += v * v; }
        put<T>(m, s.x[2], sum);
        put<T>(m, s.x[3], squares);
    });
    const auto reduction = [this](const char* stem, double (*f)(const std::vector<double>&)) {
        kernel(named<T>(stem), [f](CpuState& s, GuestMemory& m) { put<T>(m, s.x[2], f(load<T>(m, s.x[0], stride(s.x[1]), s.x[3]))); });
    };
    reduction("svs", [](const std::vector<double>& a) { double sum = 0; for (const auto v : a) sum += v * std::abs(v); return sum; });
    reduction("mvessq", [](const std::vector<double>& a) {
        double sum = 0;
        for (const auto v : a) sum += v * std::abs(v);
        return a.empty() ? std::nan("") : sum / static_cast<double>(a.size());
    });
    reduction("meamgv", [](const std::vector<double>& a) {
        double sum = 0;
        for (const auto v : a) sum += std::abs(v);
        return a.empty() ? std::nan("") : sum / static_cast<double>(a.size());
    });
    // normalize(A, IA, C, IC, Mean*, StandardDeviation*, N): C = (A - mean) / deviation (C may be null).
    kernel(named<T>("normalize"), [](CpuState& s, GuestMemory& m) {
        const auto a = load<T>(m, s.x[0], stride(s.x[1]), s.x[6]);
        double mean = 0, squares = 0;
        for (const auto v : a) mean += v;
        mean /= static_cast<double>(std::max<std::size_t>(a.size(), 1));
        for (const auto v : a) squares += (v - mean) * (v - mean);
        const double deviation = std::sqrt(squares / static_cast<double>(std::max<std::size_t>(a.size(), 1)));
        put<T>(m, s.x[4], mean);
        put<T>(m, s.x[5], deviation);
        if (!s.x[2]) return;
        std::vector<double> c(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) c[i] = (a[i] - mean) / deviation;
        store<T>(m, s.x[2], stride(s.x[3]), c);
    });
    // nzcros(A, IA, B, C*, D*, N): the index (n * IA) of the B-th zero crossing (or of the last found)
    // in C, and how many were found (at most B) in D.
    kernel(named<T>("nzcros"), [](CpuState& s, GuestMemory& m) {
        const auto ia = stride(s.x[1]);
        const auto a = load<T>(m, s.x[0], ia, s.x[5]);
        std::uint64_t found = 0, last = 0;
        for (std::size_t i = 1; i < a.size() && found < s.x[2]; ++i)
            if (std::signbit(a[i]) != std::signbit(a[i - 1])) { ++found; last = static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * ia); }
        m.write64(s.x[3], last);
        m.write64(s.x[4], found);
    });

    // ---- rearranging ---------------------------------------------------------------------------
    kernel(named<T>("vswap"), [](CpuState& s, GuestMemory& m) {
        const auto a = load<T>(m, s.x[0], stride(s.x[1]), s.x[4]), b = load<T>(m, s.x[2], stride(s.x[3]), s.x[4]);
        store<T>(m, s.x[0], stride(s.x[1]), b);
        store<T>(m, s.x[2], stride(s.x[3]), a);
    });
    // vsort(C, N, Order): in place, ascending (1) or descending (-1).
    kernel(named<T>("vsort"), [](CpuState& s, GuestMemory& m) {
        auto c = load<T>(m, s.x[0], 1, s.x[1]);
        if (static_cast<std::int32_t>(s.x[2]) < 0) std::stable_sort(c.begin(), c.end(), std::greater<>());
        else std::stable_sort(c.begin(), c.end());
        store<T>(m, s.x[0], 1, c);
    });
    // vsorti(C, I (vDSP_Length indices into C), Temporary, N, Order): sorts the indices by C's values.
    kernel(named<T>("vsorti"), [](CpuState& s, GuestMemory& m) {
        const auto n = s.x[3];
        check_length(n);
        std::vector<std::uint64_t> indices(static_cast<std::size_t>(n));
        for (std::uint64_t i = 0; i < n; ++i) indices[static_cast<std::size_t>(i)] = m.read64(s.x[1] + 8 * i);
        std::vector<std::pair<double, std::uint64_t>> keyed;
        for (const auto index : indices) keyed.emplace_back(get<T>(m, element<T>(s.x[0], 1, index)), index);
        const bool descending = static_cast<std::int32_t>(s.x[4]) < 0;
        std::stable_sort(keyed.begin(), keyed.end(), [descending](const auto& l, const auto& r) { return descending ? l.first > r.first : l.first < r.first; });
        for (std::uint64_t i = 0; i < n; ++i) m.write64(s.x[1] + 8 * i, keyed[static_cast<std::size_t>(i)].second);
    });
    // vtmerg(A, IA, B, IB, C, IC, N): C[n] = A[n] + (B[n] - A[n]) * n / (N - 1).
    kernel(named<T>("vtmerg"), [](CpuState& s, GuestMemory& m) {
        const auto n = s.x[6];
        const auto a = load<T>(m, s.x[0], stride(s.x[1]), n), b = load<T>(m, s.x[2], stride(s.x[3]), n);
        std::vector<double> c(a.size());
        for (std::size_t i = 0; i < c.size(); ++i) c[i] = a[i] + (b[i] - a[i]) * (n > 1 ? static_cast<double>(i) / static_cast<double>(n - 1) : 0.0);
        store<T>(m, s.x[4], stride(s.x[5]), c);
    });
    // vgathr(A, B (1-based vDSP_Length indices), IB, C, IC, N); vgathra(A (pointers), IA, C, IC, N);
    // vindex(A, B (float indices, truncated), IB, C, IC, N).
    kernel(named<T>("vgathr"), [](CpuState& s, GuestMemory& m) {
        const auto n = s.x[5];
        check_length(n);
        std::vector<double> c(static_cast<std::size_t>(n));
        for (std::uint64_t i = 0; i < n; ++i) c[static_cast<std::size_t>(i)] = get<T>(m, element<T>(s.x[0], 1, m.read64(element<std::uint64_t>(s.x[1], stride(s.x[2]), i)) - 1));
        store<T>(m, s.x[3], stride(s.x[4]), c);
    });
    kernel(named<T>("vgathra"), [](CpuState& s, GuestMemory& m) {
        const auto n = s.x[4];
        check_length(n);
        std::vector<double> c(static_cast<std::size_t>(n));
        for (std::uint64_t i = 0; i < n; ++i) c[static_cast<std::size_t>(i)] = get<T>(m, m.read64(element<std::uint64_t>(s.x[0], stride(s.x[1]), i)));
        store<T>(m, s.x[2], stride(s.x[3]), c);
    });
    kernel(named<T>("vindex"), [](CpuState& s, GuestMemory& m) {
        const auto b = load<T>(m, s.x[1], stride(s.x[2]), s.x[5]);
        std::vector<double> c(b.size());
        for (std::size_t i = 0; i < c.size(); ++i) c[i] = get<T>(m, element<T>(s.x[0], 1, static_cast<std::uint64_t>(std::trunc(b[i]))));
        store<T>(m, s.x[3], stride(s.x[4]), c);
    });
    // mmov(A, C, M (columns), N (rows), TA, TC): copies an N x M submatrix (row lengths TA and TC).
    kernel(named<T>("mmov"), [](CpuState& s, GuestMemory& m) {
        const auto columns = s.x[2], rows = s.x[3], ta = s.x[4], tc = s.x[5];
        check_length(columns * rows);
        for (std::uint64_t r = 0; r < rows; ++r) {
            const auto line = load<T>(m, element<T>(s.x[0], 1, r * ta), 1, columns);
            store<T>(m, element<T>(s.x[1], 1, r * tc), 1, line);
        }
    });
    // vtabi(A, IA, S1*, S2*, C (table), M, D, ID, N): D = the table at S1 * A + S2, interpolated,
    // clamped to its ends.
    kernel(named<T>("vtabi"), [](CpuState& s, GuestMemory& m) {
        const auto n = arg(s, m, 8), entries = s.x[5];
        const double scale = get<T>(m, s.x[2]), offset = get<T>(m, s.x[3]);
        const auto table = load<T>(m, s.x[4], 1, entries);
        const auto a = load<T>(m, s.x[0], stride(s.x[1]), n);
        std::vector<double> d(a.size());
        for (std::size_t i = 0; i < d.size() && !table.empty(); ++i) {
            const double p = scale * a[i] + offset;
            if (p <= 0) d[i] = table.front();
            else if (p >= static_cast<double>(table.size() - 1)) d[i] = table.back();
            else {
                const auto q = static_cast<std::size_t>(p);
                d[i] = table[q] + (p - static_cast<double>(q)) * (table[q + 1] - table[q]);
            }
        }
        store<T>(m, s.x[6], stride(s.x[7]), d);
    });
    // vpoly(A (coefficients, highest power first), IA, B, IB, C, IC, N, P): C = A0 B^P + ... + AP.
    kernel(named<T>("vpoly"), [](CpuState& s, GuestMemory& m) {
        const auto n = s.x[6], degree = s.x[7];
        const auto coefficients = load<T>(m, s.x[0], stride(s.x[1]), degree + 1);
        const auto b = load<T>(m, s.x[2], stride(s.x[3]), n);
        std::vector<double> c(b.size());
        for (std::size_t i = 0; i < c.size(); ++i)
            for (const auto coefficient : coefficients) c[i] = c[i] * b[i] + coefficient;
        store<T>(m, s.x[4], stride(s.x[5]), c);
    });
    // vlint(A, B (float indices), IB, C, IC, N, M): C = A[q] + f (A[q + 1] - A[q]) for B = q + f.
    kernel(named<T>("vlint"), [](CpuState& s, GuestMemory& m) {
        const auto b = load<T>(m, s.x[1], stride(s.x[2]), s.x[5]);
        std::vector<double> c(b.size());
        for (std::size_t i = 0; i < c.size(); ++i) {
            const auto q = static_cast<std::uint64_t>(std::trunc(b[i]));
            const double f = b[i] - static_cast<double>(q);
            const double low = get<T>(m, element<T>(s.x[0], 1, q));
            c[i] = f == 0 ? low : low + f * (get<T>(m, element<T>(s.x[0], 1, q + 1)) - low);
        }
        store<T>(m, s.x[3], stride(s.x[4]), c);
    });
    // polar and rect(A, IA, C, IC, N): pairs (re, im) <-> (magnitude, phase); the strides count elements.
    for (const bool to_polar : {true, false}) {
        kernel(named<T>(to_polar ? "polar" : "rect"), [to_polar](CpuState& s, GuestMemory& m) {
            const auto n = s.x[4];
            check_length(n);
            for (std::uint64_t i = 0; i < n; ++i) {
                const auto in = element<T>(s.x[0], stride(s.x[1]), i), out = element<T>(s.x[2], stride(s.x[3]), i);
                const double x = get<T>(m, in), y = get<T>(m, in + sizeof(T));
                put<T>(m, out, to_polar ? std::hypot(x, y) : x * std::cos(y));
                put<T>(m, out + sizeof(T), to_polar ? std::atan2(y, x) : x * std::sin(y));
            }
        });
    }
    install_complex<T>();
    install_filters<T>();
}

template <class T>
void More::install_complex() {
    using Z = std::vector<std::complex<double>>;
    // zvdiv(B, IB, A, IA, C, IC, N): C = A / B.
    kernel(named<T>("zvdiv"), [](CpuState& s, GuestMemory& m) {
        const auto b = load_split<T>(m, s.x[0], stride(s.x[1]), s.x[6]), a = load_split<T>(m, s.x[2], stride(s.x[3]), s.x[6]);
        Z c(a.size());
        for (std::size_t i = 0; i < c.size(); ++i) c[i] = a[i] / b[i];
        store_split<T>(m, s.x[4], stride(s.x[5]), c);
    });
    kernel(named<T>("zvneg"), [](CpuState& s, GuestMemory& m) {
        auto a = load_split<T>(m, s.x[0], stride(s.x[1]), s.x[4]);
        for (auto& v : a) v = -v;
        store_split<T>(m, s.x[2], stride(s.x[3]), a);
    });
    // zvfill(A* (split scalar), C, IC, N); zvzsml(A, IA, B*, C, IC, N): C = A * B.
    kernel(named<T>("zvfill"), [](CpuState& s, GuestMemory& m) {
        check_length(s.x[3]);
        store_split<T>(m, s.x[1], stride(s.x[2]), Z(static_cast<std::size_t>(s.x[3]), load_split_scalar<T>(m, s.x[0])));
    });
    kernel(named<T>("zvzsml"), [](CpuState& s, GuestMemory& m) {
        auto a = load_split<T>(m, s.x[0], stride(s.x[1]), s.x[5]);
        const auto b = load_split_scalar<T>(m, s.x[2]);
        for (auto& v : a) v *= b;
        store_split<T>(m, s.x[3], stride(s.x[4]), a);
    });
    // zvsma(A, IA, B*, C, IC, D, ID, N): D = A * B + C.
    kernel(named<T>("zvsma"), [](CpuState& s, GuestMemory& m) {
        const auto n = s.x[7];
        const auto a = load_split<T>(m, s.x[0], stride(s.x[1]), n), c = load_split<T>(m, s.x[3], stride(s.x[4]), n);
        const auto b = load_split_scalar<T>(m, s.x[2]);
        Z d(a.size());
        for (std::size_t i = 0; i < d.size(); ++i) d[i] = a[i] * b + c[i];
        store_split<T>(m, s.x[5], stride(s.x[6]), d);
    });
    // zvcma(A, IA, B, IB, C, IC, D, ID, N): D = conj(A) * B + C.
    kernel(named<T>("zvcma"), [](CpuState& s, GuestMemory& m) {
        const auto n = arg(s, m, 8);
        const auto a = load_split<T>(m, s.x[0], stride(s.x[1]), n), b = load_split<T>(m, s.x[2], stride(s.x[3]), n),
                   c = load_split<T>(m, s.x[4], stride(s.x[5]), n);
        Z d(a.size());
        for (std::size_t i = 0; i < d.size(); ++i) d[i] = std::conj(a[i]) * b[i] + c[i];
        store_split<T>(m, s.x[6], stride(s.x[7]), d);
    });
    // zrvadd/zrvsub/zrvmul/zrvdiv(A, IA, B (real), IB, C, IC, N)
    for (const char op : {'a', 's', 'm', 'd'}) {
        const char* stem = op == 'a' ? "zrvadd" : op == 's' ? "zrvsub" : op == 'm' ? "zrvmul" : "zrvdiv";
        kernel(named<T>(stem), [op](CpuState& s, GuestMemory& m) {
            auto a = load_split<T>(m, s.x[0], stride(s.x[1]), s.x[6]);
            const auto b = load<T>(m, s.x[2], stride(s.x[3]), s.x[6]);
            for (std::size_t i = 0; i < a.size(); ++i)
                a[i] = op == 'a' ? a[i] + b[i] : op == 's' ? a[i] - b[i] : op == 'm' ? a[i] * b[i] : a[i] / b[i];
            store_split<T>(m, s.x[4], stride(s.x[5]), a);
        });
    }
    // zdotpr and zidotpr(A, IA, B, IB, C* (split scalar), N): sum A * B (zidotpr: conj(A) * B).
    for (const bool conjugated : {false, true}) {
        kernel(named<T>(conjugated ? "zidotpr" : "zdotpr"), [conjugated](CpuState& s, GuestMemory& m) {
            const auto a = load_split<T>(m, s.x[0], stride(s.x[1]), s.x[5]), b = load_split<T>(m, s.x[2], stride(s.x[3]), s.x[5]);
            std::complex<double> sum;
            for (std::size_t i = 0; i < a.size(); ++i) sum += (conjugated ? std::conj(a[i]) : a[i]) * b[i];
            store_split<T>(m, s.x[4], 1, Z{sum});
        });
    }
    // zaspec(A, C (real), N): C += |A|^2; zcspec(A, B, C (split), N): C += conj(A) * B.
    kernel(named<T>("zaspec"), [](CpuState& s, GuestMemory& m) {
        const auto a = load_split<T>(m, s.x[0], 1, s.x[2]);
        auto c = load<T>(m, s.x[1], 1, s.x[2]);
        for (std::size_t i = 0; i < c.size(); ++i) c[i] += std::norm(a[i]);
        store<T>(m, s.x[1], 1, c);
    });
    kernel(named<T>("zcspec"), [](CpuState& s, GuestMemory& m) {
        const auto a = load_split<T>(m, s.x[0], 1, s.x[3]), b = load_split<T>(m, s.x[1], 1, s.x[3]);
        auto c = load_split<T>(m, s.x[2], 1, s.x[3]);
        for (std::size_t i = 0; i < c.size(); ++i) c[i] += std::conj(a[i]) * b[i];
        store_split<T>(m, s.x[2], 1, c);
    });
    // zmmul(A, IA, B, IB, C, IC, M, N, P): C[M x N] = A[M x P] B[P x N], row-major split complex.
    kernel(named<T>("zmmul"), [](CpuState& s, GuestMemory& m) {
        const auto rows = s.x[6], cols = s.x[7], inner = arg(s, m, 8);
        check_length(rows * inner + inner * cols + rows * cols);
        const auto a = load_split<T>(m, s.x[0], stride(s.x[1]), rows * inner), b = load_split<T>(m, s.x[2], stride(s.x[3]), inner * cols);
        Z c(static_cast<std::size_t>(rows * cols));
        for (std::uint64_t i = 0; i < rows; ++i)
            for (std::uint64_t k = 0; k < inner; ++k)
                for (std::uint64_t j = 0; j < cols; ++j) c[i * cols + j] += a[i * inner + k] * b[k * cols + j];
        store_split<T>(m, s.x[4], stride(s.x[5]), c);
    });
    // fft2d_zip(Setup, C, IC0, IC1, Log2N0, Log2N1, Direction) and fft2d_zop(Setup, A, IA0, IA1, C,
    // IC0, IC1, Log2N0, Log2N1, Direction): rows of 2^Log2N0 elements IC0 apart, 2^Log2N1 rows IC1
    // apart (0: IC0 * the row length).
    const auto fft2d = [](GuestMemory& m, Address in, std::int64_t in0, std::int64_t in1, Address out, std::int64_t out0, std::int64_t out1,
                          std::uint64_t log2n0, std::uint64_t log2n1, std::int32_t direction) {
        if (log2n0 > 24 || log2n1 > 24) throw std::runtime_error("unsupported: Alchemy vDSP: an FFT longer than 2^24");
        const std::uint64_t columns = 1ULL << log2n0, rows = 1ULL << log2n1;
        check_length(columns * rows);
        if (!in1) in1 = in0 * static_cast<std::int64_t>(columns);
        if (!out1) out1 = out0 * static_cast<std::int64_t>(columns);
        std::vector<Z> grid(static_cast<std::size_t>(rows));
        const auto re_in = m.read64(in), im_in = m.read64(in + 8);
        for (std::uint64_t r = 0; r < rows; ++r) {
            grid[r].resize(static_cast<std::size_t>(columns));
            for (std::uint64_t c = 0; c < columns; ++c) {
                const auto offset = static_cast<std::int64_t>(r) * in1 + static_cast<std::int64_t>(c) * in0;
                grid[r][c] = {get<T>(m, element<T>(re_in, 1, static_cast<std::uint64_t>(offset))), get<T>(m, element<T>(im_in, 1, static_cast<std::uint64_t>(offset)))};
            }
            fft(grid[r], direction < 0);
        }
        Z column(static_cast<std::size_t>(rows));
        for (std::uint64_t c = 0; c < columns; ++c) {
            for (std::uint64_t r = 0; r < rows; ++r) column[r] = grid[r][c];
            fft(column, direction < 0);
            for (std::uint64_t r = 0; r < rows; ++r) grid[r][c] = column[r];
        }
        const auto re_out = m.read64(out), im_out = m.read64(out + 8);
        for (std::uint64_t r = 0; r < rows; ++r)
            for (std::uint64_t c = 0; c < columns; ++c) {
                const auto offset = static_cast<std::uint64_t>(static_cast<std::int64_t>(r) * out1 + static_cast<std::int64_t>(c) * out0);
                put<T>(m, element<T>(re_out, 1, offset), grid[r][c].real());
                put<T>(m, element<T>(im_out, 1, offset), grid[r][c].imag());
            }
    };
    kernel(named<T>("fft2d_zip"), [fft2d](CpuState& s, GuestMemory& m) {
        fft2d(m, s.x[1], stride(s.x[2]), stride(s.x[3]), s.x[1], stride(s.x[2]), stride(s.x[3]), s.x[4], s.x[5], static_cast<std::int32_t>(s.x[6]));
    });
    kernel(named<T>("fft2d_zop"), [fft2d](CpuState& s, GuestMemory& m) {
        fft2d(m, s.x[1], stride(s.x[2]), stride(s.x[3]), s.x[4], stride(s.x[5]), stride(s.x[6]), s.x[7], arg(s, m, 8),
              static_cast<std::int32_t>(arg(s, m, 9)));
    });
    // fft_zipt(Setup, C, IC, Buffer, Log2N, Direction) and fft_zript: the temporary buffer is not needed.
    for (const auto* stem : {"fft_zip", "fft_zrip"}) {
        const auto target = named<T>(stem);
        kernel(named<T>(std::string(stem).append("t").c_str()), [this, target](CpuState& s, GuestMemory&) {
            static_cast<void>(call(accelerate, target, {s.x[0], s.x[1], s.x[2], s.x[4], s.x[5]}));
        });
    }
}

template <class T>
void More::install_filters() {
    // deq22(A, IA, B (5 coefficients), C, IC, N): A and C hold N + 2 values; C[0..1] are the
    // initial outputs (vdsp.cpp has deq22D).
    kernel(named<T>("deq22"), [](CpuState& s, GuestMemory& m) {
        const auto n = s.x[5];
        check_length(n + 2);
        const auto a = load<T>(m, s.x[0], stride(s.x[1]), n + 2), b = load<T>(m, s.x[2], 1, 5);
        auto c = load<T>(m, s.x[3], stride(s.x[4]), n + 2);
        for (std::size_t i = 2; i < c.size(); ++i) c[i] = a[i] * b[0] + a[i - 1] * b[1] + a[i - 2] * b[2] - c[i - 1] * b[3] - c[i - 2] * b[4];
        store<T>(m, s.x[3], stride(s.x[4]), c);
    });
    // conv(A, IA, F, IF, C, IC, N, P): C[n] = sum_p A[n + p] F[p] (IF negative: convolution).
    kernel(named<T>("conv"), [](CpuState& s, GuestMemory& m) {
        const auto n = s.x[6], p = s.x[7];
        check_length(n + p);
        const auto f_step = stride(s.x[3]);
        // A negative filter stride walks F backwards from the address given.
        std::vector<double> filter(static_cast<std::size_t>(p));
        for (std::uint64_t k = 0; k < p; ++k) filter[static_cast<std::size_t>(k)] = get<T>(m, element<T>(s.x[2], f_step, k));
        const auto a = load<T>(m, s.x[0], stride(s.x[1]), n + (p ? p - 1 : 0));
        std::vector<double> c(static_cast<std::size_t>(n));
        for (std::size_t i = 0; i < c.size(); ++i)
            for (std::size_t k = 0; k < filter.size(); ++k) c[i] += a[i + k] * filter[k];
        store<T>(m, s.x[4], stride(s.x[5]), c);
    });
    // imgfir(A, NR, NC, F, C, P, Q): a P x Q (odd) filter over an NR x NC image; the outer
    // (P - 1) / 2 rows and (Q - 1) / 2 columns of C are zero. f3x3 and f5x5(A, NR, NC, F, C).
    const auto image_filter = [](GuestMemory& m, Address in, std::uint64_t rows, std::uint64_t columns, Address filter, Address out,
                                 std::uint64_t p, std::uint64_t q) {
        if (!(p & 1) || !(q & 1) || p < 3 || q < 3) throw std::runtime_error("unsupported: Alchemy vDSP: an image filter needs odd dimensions of at least 3");
        check_length(rows * columns + p * q);
        const auto a = load<T>(m, in, 1, rows * columns), f = load<T>(m, filter, 1, p * q);
        std::vector<double> c(static_cast<std::size_t>(rows * columns));
        const auto hr = (p - 1) / 2, hc = (q - 1) / 2;
        for (std::uint64_t r = hr; r + hr < rows; ++r)
            for (std::uint64_t col = hc; col + hc < columns; ++col) {
                double sum = 0;
                for (std::uint64_t i = 0; i < p; ++i)
                    for (std::uint64_t j = 0; j < q; ++j) sum += a[(r - hr + i) * columns + (col - hc + j)] * f[i * q + j];
                c[r * columns + col] = sum;
            }
        store<T>(m, out, 1, c);
    };
    kernel(named<T>("imgfir"), [image_filter](CpuState& s, GuestMemory& m) { image_filter(m, s.x[0], s.x[1], s.x[2], s.x[3], s.x[4], s.x[5], s.x[6]); });
    kernel(named<T>("f3x3"), [image_filter](CpuState& s, GuestMemory& m) { image_filter(m, s.x[0], s.x[1], s.x[2], s.x[3], s.x[4], 3, 3); });
    kernel(named<T>("f5x5"), [image_filter](CpuState& s, GuestMemory& m) { image_filter(m, s.x[0], s.x[1], s.x[2], s.x[3], s.x[4], 5, 5); });
}

} // namespace

void install_vdsp_more(InstallContext& context) { std::make_shared<More>(context)->install(); }

} // namespace alchemy::gui::fill
