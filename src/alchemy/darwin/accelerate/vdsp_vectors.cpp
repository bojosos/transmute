#include "alchemy/darwin/accelerate/install.hpp"
#include "alchemy/gui/fill_provider.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <type_traits>
#include <vector>

// vDSP beyond vdsp.cpp's kernels, in single precision and (the "D" names) double, computed in the
// element type on the host per the documented semantics:
// - Vector-vector: vadd/vsub/vmul (D), vdiv (B first: C = A / B), vmax, vmin, vmaxmg (D), vminmg;
//   vma, vam, vmsa; unary vabs, vneg, vnabs, vsq, vssq; vfill, vclr (D), vramp (D), vgen.
// - Vector-scalar: vsadd/vsmul (D), vsdiv, vsma (D), vsmsa; vclip (D), vlim, vthr, vthres, vthrsc.
// - Reductions: sve, svemg, svesq (D), meanv, meanmgv, measqv, rmsqv (D), maxv (D), minv,
//   maxmgv (D), minmgv, dotpr; maxvi, minvi, maxmgvi, minmgvi (the index is the element's offset,
//   n * stride, as vDSP returns it).
// - Conversions: vflt8/16/32, vfltu8/16/32, vfix8/16/32 (toward zero), vfixr8/16/32 (to nearest),
//   vfixu8/16/32 and vfixru8/16/32 (saturating), vspdp, vdpsp; vdbcon (power or amplitude).
// - Windows: hann (normalized or not, half or full), hamm, blkman.
// - Matrices: mmul (C[M x N] = A[M x P] B[P x N]), mtrans (C[M x N] = transpose of A[N x M]).
// - Split complex: zvadd, zvsub, zvmul (D; conjugating A when asked), zvmags, zvabs, zvphas, zvconj,
//   zvmov, ctoz/ztoc (D).
// - FFTs: create/destroy_fftsetup (D, radix 2), fft_zip/zop (complex, in or out of place; forward
//   unnormalized, inverse unnormalized), fft_zrip/zrop (D; packed real, results 2x the DFT as vDSP
//   documents); the DFT interface (DFT_zop/zrop_CreateSetup(D), DFT_Execute(D), DFT_DestroySetup(D))
//   for lengths f * 2^n, f in {1, 3, 5, 15} (computed as plain DFTs where not radix 2).
// - Biquads: biquad_CreateSetup(D), biquad(D) (sections in cascade, direct form II transposed,
//   the delay array 2 * sections + 2 long as documented), biquad_DestroySetup(D); desamp (D).
// Lengths over 2^26 stop explicitly.
namespace alchemy::gui::fill {
namespace {

constexpr std::uint64_t maximum_length = 1ULL << 26;

std::int64_t stride_of(std::uint64_t value) { return static_cast<std::int64_t>(value); }

template <class T> T load_one(GuestMemory& m, Address at) {
    if constexpr (std::is_same_v<T, float>) return std::bit_cast<float>(m.read32(at));
    else return std::bit_cast<double>(m.read64(at));
}
template <class T> void store_one(GuestMemory& m, Address at, T value) {
    if constexpr (std::is_same_v<T, float>) m.write32(at, std::bit_cast<std::uint32_t>(value));
    else m.write64(at, std::bit_cast<std::uint64_t>(value));
}

void check_length(std::uint64_t count) {
    if (count > maximum_length) throw std::runtime_error("unsupported: Alchemy vDSP: vector longer than 2^26 elements");
}

// `count` elements of `stride` (in elements) from `base`.
template <class T> std::vector<T> load(GuestMemory& m, Address base, std::int64_t stride, std::uint64_t count) {
    check_length(count);
    std::vector<T> values(static_cast<std::size_t>(count));
    if (!count) return values;
    if (stride == 1) {
        m.read_bytes(base, std::span(reinterpret_cast<std::uint8_t*>(values.data()), values.size() * sizeof(T)));
        return values;
    }
    for (std::uint64_t i = 0; i < count; ++i) values[i] = load_one<T>(m, base + static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * stride * static_cast<std::int64_t>(sizeof(T))));
    return values;
}
template <class T> void store(GuestMemory& m, Address base, std::int64_t stride, const std::vector<T>& values) {
    if (values.empty()) return;
    if (stride == 1) {
        m.write_bytes(base, std::span(reinterpret_cast<const std::uint8_t*>(values.data()), values.size() * sizeof(T)));
        return;
    }
    for (std::size_t i = 0; i < values.size(); ++i)
        store_one<T>(m, base + static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * stride * static_cast<std::int64_t>(sizeof(T))), values[i]);
}

template <class T> std::string named(const char* name) { return std::string(name) + (std::is_same_v<T, double> ? "D" : ""); }

// Radix-2 complex FFT in place (forward: e^{-i...}; neither direction normalized).
template <class T> void fft(std::vector<std::complex<T>>& a, bool inverse) {
    const auto n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = 2 * std::numbers::pi / static_cast<double>(length) * (inverse ? 1 : -1);
        for (std::size_t i = 0; i < n; i += length)
            for (std::size_t j = 0; j < length / 2; ++j) {
                const std::complex<T> w(static_cast<T>(std::cos(angle * static_cast<double>(j))), static_cast<T>(std::sin(angle * static_cast<double>(j))));
                const auto u = a[i + j], v = a[i + j + length / 2] * w;
                a[i + j] = u + v;
                a[i + j + length / 2] = u - v;
            }
    }
}
// Any length: radix 2 when a power of two, else a plain DFT (in double).
template <class T> void dft(std::vector<std::complex<T>>& a, bool inverse) {
    const auto n = a.size();
    if (n && !(n & (n - 1))) return fft(a, inverse);
    std::vector<std::complex<T>> out(n);
    for (std::size_t k = 0; k < n; ++k) {
        std::complex<double> sum;
        for (std::size_t j = 0; j < n; ++j) {
            const double angle = 2 * std::numbers::pi * static_cast<double>((k * j) % n) / static_cast<double>(n) * (inverse ? 1 : -1);
            sum += std::complex<double>(a[j].real(), a[j].imag()) * std::complex<double>(std::cos(angle), std::sin(angle));
        }
        out[k] = std::complex<T>(static_cast<T>(sum.real()), static_cast<T>(sum.imag()));
    }
    a = std::move(out);
}

// FFT setups: the largest log2 length; DFT setups: the length, direction and kind.
struct FftSetup { std::uint64_t log2n{}; };
struct DftSetup { std::uint64_t length{}; bool inverse{}; bool real{}; };
struct Biquad { std::vector<double> coefficients; std::uint64_t sections{}; };   // b0 b1 b2 a1 a2 per section

class Vectors final : public Provider {
public:
    using Provider::Provider;
    void install();

private:
    template <class T> void install_type();
    template <class T> void install_fft();
    void install_conversions();
    // Registers `name` unless another provider already exports it (vdsp.cpp's float kernels).
    void kernel(const std::string& name, transmute::GuestCallHandler handler, const std::source_location& where = std::source_location::current()) {
        if (has(accelerate, name)) return;
        raw(accelerate, name, std::move(handler), where);
    }
    template <class Object> Address make_handle(std::shared_ptr<Object> object) {
        std::lock_guard lock(mutex_);
        const auto handle = process.allocate_guest(16, 16);
        handles_[handle] = std::static_pointer_cast<void>(object);
        return handle;
    }
    template <class Object> std::shared_ptr<Object> handle(Address address) {
        std::lock_guard lock(mutex_);
        const auto found = handles_.find(address);
        return found == handles_.end() ? nullptr : std::static_pointer_cast<Object>(found->second);
    }
    void drop(Address address) {
        std::lock_guard lock(mutex_);
        if (handles_.erase(address)) process.release_guest(address);
    }

    std::mutex mutex_;
    std::map<Address, std::shared_ptr<void>> handles_;
};

template <class T>
void Vectors::install_type() {
    using V = std::vector<T>;
    // ---- vector-vector (A, IA, B, IB, C, IC, N) ----
    const auto binary = [this](const char* name, bool b_first, auto op) {
        kernel(named<T>(name), [b_first, op](CpuState& s, GuestMemory& m) {
            auto a = load<T>(m, b_first ? s.x[2] : s.x[0], stride_of(b_first ? s.x[3] : s.x[1]), s.x[6]);
            const auto b = load<T>(m, b_first ? s.x[0] : s.x[2], stride_of(b_first ? s.x[1] : s.x[3]), s.x[6]);
            for (std::size_t i = 0; i < a.size(); ++i) a[i] = op(a[i], b[i]);
            store<T>(m, s.x[4], stride_of(s.x[5]), a);
        });
    };
    binary("_vDSP_vadd", false, [](T a, T b) { return a + b; });
    binary("_vDSP_vsub", true, [](T a, T b) { return a - b; });
    binary("_vDSP_vmul", false, [](T a, T b) { return a * b; });
    binary("_vDSP_vdiv", true, [](T a, T b) { return a / b; });
    binary("_vDSP_vmax", false, [](T a, T b) { return std::max(a, b); });
    binary("_vDSP_vmin", false, [](T a, T b) { return std::min(a, b); });
    binary("_vDSP_vmaxmg", false, [](T a, T b) { return std::max(std::abs(a), std::abs(b)); });
    binary("_vDSP_vminmg", false, [](T a, T b) { return std::min(std::abs(a), std::abs(b)); });
    // vma / vam (A, IA, B, IB, C, IC, D, ID, N): D = A * B + C, D = (A + B) * C.
    for (const bool multiply_first : {true, false})
        kernel(named<T>(multiply_first ? "_vDSP_vma" : "_vDSP_vam"), [multiply_first](CpuState& s, GuestMemory& m) {
            const auto n = m.read64(s.sp);   // the ninth argument
            auto a = load<T>(m, s.x[0], stride_of(s.x[1]), n);
            const auto b = load<T>(m, s.x[2], stride_of(s.x[3]), n), c = load<T>(m, s.x[4], stride_of(s.x[5]), n);
            for (std::size_t i = 0; i < a.size(); ++i) a[i] = multiply_first ? a[i] * b[i] + c[i] : (a[i] + b[i]) * c[i];
            store<T>(m, s.x[6], stride_of(s.x[7]), a);
        });
    // vmsa (A, IA, B, IB, const T *C, D, ID, N): D = A * B + C.
    kernel(named<T>("_vDSP_vmsa"), [](CpuState& s, GuestMemory& m) {
        auto a = load<T>(m, s.x[0], stride_of(s.x[1]), s.x[7]);
        const auto b = load<T>(m, s.x[2], stride_of(s.x[3]), s.x[7]);
        const auto c = load_one<T>(m, s.x[4]);
        for (std::size_t i = 0; i < a.size(); ++i) a[i] = a[i] * b[i] + c;
        store<T>(m, s.x[5], stride_of(s.x[6]), a);
    });
    // ---- unary (A, IA, C, IC, N) ----
    const auto unary = [this](const char* name, auto op) {
        kernel(named<T>(name), [op](CpuState& s, GuestMemory& m) {
            auto a = load<T>(m, s.x[0], stride_of(s.x[1]), s.x[4]);
            for (auto& value : a) value = op(value);
            store<T>(m, s.x[2], stride_of(s.x[3]), a);
        });
    };
    unary("_vDSP_vabs", [](T a) { return std::abs(a); });
    unary("_vDSP_vneg", [](T a) { return -a; });
    unary("_vDSP_vnabs", [](T a) { return -std::abs(a); });
    unary("_vDSP_vsq", [](T a) { return a * a; });
    unary("_vDSP_vssq", [](T a) { return a * std::abs(a); });
    unary("_vDSP_vfrac", [](T a) { return a - std::trunc(a); });
    // vrvrs (C, IC, N): reversed in place.
    kernel(named<T>("_vDSP_vrvrs"), [](CpuState& s, GuestMemory& m) {
        auto c = load<T>(m, s.x[0], stride_of(s.x[1]), s.x[2]);
        std::reverse(c.begin(), c.end());
        store<T>(m, s.x[0], stride_of(s.x[1]), c);
    });
    // vfill (const T *A, C, IC, N), vclr (C, IC, N)
    kernel(named<T>("_vDSP_vfill"), [](CpuState& s, GuestMemory& m) {
        check_length(s.x[3]);
        store<T>(m, s.x[1], stride_of(s.x[2]), V(static_cast<std::size_t>(s.x[3]), load_one<T>(m, s.x[0])));
    });
    kernel(named<T>("_vDSP_vclr"), [](CpuState& s, GuestMemory& m) {
        check_length(s.x[2]);
        store<T>(m, s.x[0], stride_of(s.x[1]), V(static_cast<std::size_t>(s.x[2]), T{}));
    });
    // vramp (const T *A, const T *B, C, IC, N): C[n] = A + n B. vgen (const T *A, const T *B, C, IC, N):
    // from A to B in N steps.
    for (const bool generate : {false, true})
        kernel(named<T>(generate ? "_vDSP_vgen" : "_vDSP_vramp"), [generate](CpuState& s, GuestMemory& m) {
            const auto n = s.x[4];
            check_length(n);
            const auto a = load_one<T>(m, s.x[0]), b = load_one<T>(m, s.x[1]);
            V c(static_cast<std::size_t>(n));
            for (std::size_t i = 0; i < c.size(); ++i)
                c[i] = generate ? (n > 1 ? a + (b - a) * static_cast<T>(i) / static_cast<T>(n - 1) : a) : a + static_cast<T>(i) * b;
            store<T>(m, s.x[2], stride_of(s.x[3]), c);
        });
    // ---- vector-scalar (A, IA, const T *B, C, IC, N) ----
    const auto scalar = [this](const char* name, auto op) {
        kernel(named<T>(name), [op](CpuState& s, GuestMemory& m) {
            auto a = load<T>(m, s.x[0], stride_of(s.x[1]), s.x[5]);
            const auto b = load_one<T>(m, s.x[2]);
            for (auto& value : a) value = op(value, b);
            store<T>(m, s.x[3], stride_of(s.x[4]), a);
        });
    };
    scalar("_vDSP_vsadd", [](T a, T b) { return a + b; });
    scalar("_vDSP_vsmul", [](T a, T b) { return a * b; });
    scalar("_vDSP_vsdiv", [](T a, T b) { return a / b; });
    scalar("_vDSP_vthr", [](T a, T b) { return std::max(a, b); });
    scalar("_vDSP_vthres", [](T a, T b) { return a >= b ? a : T{}; });
    // svdiv (const T *A, B, IB, C, IC, N): C = A / B.
    kernel(named<T>("_vDSP_svdiv"), [](CpuState& s, GuestMemory& m) {
        const auto a = load_one<T>(m, s.x[0]);
        auto b = load<T>(m, s.x[1], stride_of(s.x[2]), s.x[5]);
        for (auto& value : b) value = a / value;
        store<T>(m, s.x[3], stride_of(s.x[4]), b);
    });
    // vsma (A, IA, const T *B, C, IC, D, ID, N): D = A B + C. vsmsa (A, IA, const T *B, const T *C, D, ID, N).
    kernel(named<T>("_vDSP_vsma"), [](CpuState& s, GuestMemory& m) {
        auto a = load<T>(m, s.x[0], stride_of(s.x[1]), s.x[7]);
        const auto b = load_one<T>(m, s.x[2]);
        const auto c = load<T>(m, s.x[3], stride_of(s.x[4]), s.x[7]);
        for (std::size_t i = 0; i < a.size(); ++i) a[i] = a[i] * b + c[i];
        store<T>(m, s.x[5], stride_of(s.x[6]), a);
    });
    kernel(named<T>("_vDSP_vsmsa"), [](CpuState& s, GuestMemory& m) {
        auto a = load<T>(m, s.x[0], stride_of(s.x[1]), s.x[6]);
        const auto b = load_one<T>(m, s.x[2]), c = load_one<T>(m, s.x[3]);
        for (auto& value : a) value = value * b + c;
        store<T>(m, s.x[4], stride_of(s.x[5]), a);
    });
    // vclip (A, IA, const T *low, const T *high, D, ID, N); vlim / vthrsc (A, IA, const T *B, const T *C, D, ID, N):
    // vlim D = A >= B ? C : -C; vthrsc D = A >= B ? C : -C as well, with B the threshold.
    kernel(named<T>("_vDSP_vclip"), [](CpuState& s, GuestMemory& m) {
        auto a = load<T>(m, s.x[0], stride_of(s.x[1]), s.x[6]);
        const auto low = load_one<T>(m, s.x[2]), high = load_one<T>(m, s.x[3]);
        for (auto& value : a) value = value < low ? low : value > high ? high : value;
        store<T>(m, s.x[4], stride_of(s.x[5]), a);
    });
    for (const char* name : {"_vDSP_vlim", "_vDSP_vthrsc"})
        kernel(named<T>(name), [](CpuState& s, GuestMemory& m) {
            auto a = load<T>(m, s.x[0], stride_of(s.x[1]), s.x[6]);
            const auto b = load_one<T>(m, s.x[2]), c = load_one<T>(m, s.x[3]);
            for (auto& value : a) value = value >= b ? c : -c;
            store<T>(m, s.x[4], stride_of(s.x[5]), a);
        });
    // ---- reductions (A, IA, T *C, N) ----
    const auto reduce = [this](const char* name, auto op) {
        kernel(named<T>(name), [op](CpuState& s, GuestMemory& m) {
            const auto a = load<T>(m, s.x[0], stride_of(s.x[1]), s.x[3]);
            store_one<T>(m, s.x[2], op(a));
        });
    };
    const auto sum = [](const V& a, auto f) { double total = 0; for (const auto value : a) total += f(value); return total; };
    reduce("_vDSP_sve", [sum](const V& a) { return static_cast<T>(sum(a, [](T v) { return double(v); })); });
    reduce("_vDSP_svemg", [sum](const V& a) { return static_cast<T>(sum(a, [](T v) { return std::abs(double(v)); })); });
    reduce("_vDSP_svesq", [sum](const V& a) { return static_cast<T>(sum(a, [](T v) { return double(v) * v; })); });
    reduce("_vDSP_meanv", [sum](const V& a) { return a.empty() ? T{} : static_cast<T>(sum(a, [](T v) { return double(v); }) / double(a.size())); });
    reduce("_vDSP_meanmgv", [sum](const V& a) { return a.empty() ? T{} : static_cast<T>(sum(a, [](T v) { return std::abs(double(v)); }) / double(a.size())); });
    reduce("_vDSP_measqv", [sum](const V& a) { return a.empty() ? T{} : static_cast<T>(sum(a, [](T v) { return double(v) * v; }) / double(a.size())); });
    reduce("_vDSP_rmsqv", [sum](const V& a) { return a.empty() ? T{} : static_cast<T>(std::sqrt(sum(a, [](T v) { return double(v) * v; }) / double(a.size()))); });
    // Empty vectors: maxv answers -infinity and minv +infinity, as vDSP documents.
    reduce("_vDSP_maxv", [](const V& a) { return a.empty() ? -std::numeric_limits<T>::infinity() : *std::max_element(a.begin(), a.end()); });
    reduce("_vDSP_minv", [](const V& a) { return a.empty() ? std::numeric_limits<T>::infinity() : *std::min_element(a.begin(), a.end()); });
    reduce("_vDSP_maxmgv", [](const V& a) { T r{}; for (const auto v : a) r = std::max(r, std::abs(v)); return r; });
    reduce("_vDSP_minmgv", [](const V& a) { T r = std::numeric_limits<T>::infinity(); for (const auto v : a) r = std::min(r, std::abs(v)); return r; });
    // dotpr (A, IA, B, IB, T *C, N)
    kernel(named<T>("_vDSP_dotpr"), [](CpuState& s, GuestMemory& m) {
        const auto a = load<T>(m, s.x[0], stride_of(s.x[1]), s.x[5]), b = load<T>(m, s.x[2], stride_of(s.x[3]), s.x[5]);
        double total = 0;
        for (std::size_t i = 0; i < a.size(); ++i) total += double(a[i]) * b[i];
        store_one<T>(m, s.x[4], static_cast<T>(total));
    });
    // Index reductions (A, IA, T *C, vDSP_Length *I, N): the first extreme; I = its index times IA.
    const auto reduce_index = [this](const char* name, bool largest, bool magnitude) {
        kernel(named<T>(name), [largest, magnitude](CpuState& s, GuestMemory& m) {
            const auto a = load<T>(m, s.x[0], stride_of(s.x[1]), s.x[4]);
            T best = largest ? -std::numeric_limits<T>::infinity() : std::numeric_limits<T>::infinity();
            std::size_t at = 0;
            for (std::size_t i = 0; i < a.size(); ++i) {
                const auto v = magnitude ? std::abs(a[i]) : a[i];
                if (largest ? v > best : v < best) { best = v; at = i; }
            }
            store_one<T>(m, s.x[2], best);
            m.write64(s.x[3], static_cast<std::uint64_t>(static_cast<std::int64_t>(at) * stride_of(s.x[1])));
        });
    };
    reduce_index("_vDSP_maxvi", true, false);
    reduce_index("_vDSP_minvi", false, false);
    reduce_index("_vDSP_maxmgvi", true, true);
    reduce_index("_vDSP_minmgvi", false, true);
    // vdbcon (A, IA, const T *B, C, IC, N, unsigned int F): F 0, power (10 log10 A/B); 1, amplitude (20 log10).
    kernel(named<T>("_vDSP_vdbcon"), [](CpuState& s, GuestMemory& m) {
        auto a = load<T>(m, s.x[0], stride_of(s.x[1]), s.x[5]);
        const auto b = load_one<T>(m, s.x[2]);
        const double factor = (s.x[6] & 0xffffffff) ? 20 : 10;
        for (auto& value : a) value = static_cast<T>(factor * std::log10(double(value) / b));
        store<T>(m, s.x[3], stride_of(s.x[4]), a);
    });
    // ---- windows (T *C, N, int Flag): Flag 2 normalizes the Hann window, 1 makes half a window ----
    const auto window = [this](const char* name, auto f) {
        kernel(named<T>(name), [f](CpuState& s, GuestMemory& m) {
            const auto n = s.x[1];
            check_length(n);
            const auto flag = static_cast<std::int32_t>(s.x[2]);
            const auto count = (flag & 1) ? (n + 1) / 2 : n;
            V c(static_cast<std::size_t>(count));
            for (std::size_t i = 0; i < c.size(); ++i) c[i] = static_cast<T>(f(double(i), double(n), flag));
            store<T>(m, s.x[0], 1, c);
        });
    };
    // Hann: 0.5 (1 - cos(2 pi i / N)), or with vDSP_HANN_NORM 0.8165 (1 - cos(2 pi i / N)) (sqrt(2/3)).
    window("_vDSP_hann_window", [](double i, double n, std::int32_t flag) {
        return ((flag & 2) ? 0.8165 : 0.5) * (1 - std::cos(2 * std::numbers::pi * i / n));
    });
    window("_vDSP_hamm_window", [](double i, double n, std::int32_t) { return 0.54 - 0.46 * std::cos(2 * std::numbers::pi * i / n); });
    window("_vDSP_blkman_window", [](double i, double n, std::int32_t) {
        return 0.42 - 0.5 * std::cos(2 * std::numbers::pi * i / n) + 0.08 * std::cos(4 * std::numbers::pi * i / n);
    });
    // ---- matrices ----
    // mmul (A, IA, B, IB, C, IC, M, N, P (on the stack)): C[M x N] = A[M x P] B[P x N], row-major.
    kernel(named<T>("_vDSP_mmul"), [](CpuState& s, GuestMemory& m) {
        const auto rows = s.x[6], columns = s.x[7], inner = m.read64(s.sp);
        check_length(rows * inner); check_length(inner * columns); check_length(rows * columns);
        const auto a = load<T>(m, s.x[0], stride_of(s.x[1]), rows * inner), b = load<T>(m, s.x[2], stride_of(s.x[3]), inner * columns);
        V c(static_cast<std::size_t>(rows * columns));
        for (std::uint64_t r = 0; r < rows; ++r)
            for (std::uint64_t k = 0; k < columns; ++k) {
                double total = 0;
                for (std::uint64_t p = 0; p < inner; ++p) total += double(a[r * inner + p]) * b[p * columns + k];
                c[r * columns + k] = static_cast<T>(total);
            }
        store<T>(m, s.x[4], stride_of(s.x[5]), c);
    });
    // mtrans (A, IA, C, IC, M, N): C[M x N] = transpose of A[N x M].
    kernel(named<T>("_vDSP_mtrans"), [](CpuState& s, GuestMemory& m) {
        const auto rows = s.x[4], columns = s.x[5];
        check_length(rows * columns);
        const auto a = load<T>(m, s.x[0], stride_of(s.x[1]), rows * columns);
        V c(a.size());
        for (std::uint64_t r = 0; r < rows; ++r)
            for (std::uint64_t k = 0; k < columns; ++k) c[r * columns + k] = a[k * rows + r];
        store<T>(m, s.x[2], stride_of(s.x[3]), c);
    });
    // ---- split complex: DSPSplitComplex {T *realp; T *imagp} ----
    struct Split { std::vector<T> re, im; };
    const auto load_split = [](GuestMemory& m, Address split, std::int64_t stride, std::uint64_t n) {
        return Split{load<T>(m, m.read64(split), stride, n), load<T>(m, m.read64(split + 8), stride, n)};
    };
    const auto store_split = [](GuestMemory& m, Address split, std::int64_t stride, const Split& v) {
        store<T>(m, m.read64(split), stride, v.re);
        store<T>(m, m.read64(split + 8), stride, v.im);
    };
    // zvadd / zvsub (A, IA, B, IB, C, IC, N): C = A + B, C = A - B.
    for (const bool add : {true, false})
        kernel(named<T>(add ? "_vDSP_zvadd" : "_vDSP_zvsub"), [add, load_split, store_split](CpuState& s, GuestMemory& m) {
            auto a = load_split(m, s.x[0], stride_of(s.x[1]), s.x[6]);
            const auto b = load_split(m, s.x[2], stride_of(s.x[3]), s.x[6]);
            for (std::size_t i = 0; i < a.re.size(); ++i) {
                a.re[i] = add ? a.re[i] + b.re[i] : a.re[i] - b.re[i];
                a.im[i] = add ? a.im[i] + b.im[i] : a.im[i] - b.im[i];
            }
            store_split(m, s.x[4], stride_of(s.x[5]), a);
        });
    // zvmul (A, IA, B, IB, C, IC, N, int Conjugate): C = A B, or conj(A) B when Conjugate is -1.
    kernel(named<T>("_vDSP_zvmul"), [load_split, store_split](CpuState& s, GuestMemory& m) {
        auto a = load_split(m, s.x[0], stride_of(s.x[1]), s.x[6]);
        const auto b = load_split(m, s.x[2], stride_of(s.x[3]), s.x[6]);
        const bool conjugate = static_cast<std::int32_t>(s.x[7]) == -1;
        for (std::size_t i = 0; i < a.re.size(); ++i) {
            const std::complex<T> x(a.re[i], conjugate ? -a.im[i] : a.im[i]), y(b.re[i], b.im[i]);
            const auto z = x * y;
            a.re[i] = z.real();
            a.im[i] = z.imag();
        }
        store_split(m, s.x[4], stride_of(s.x[5]), a);
    });
    // zvmags / zvabs / zvphas (A, IA, C, IC, N): |A|^2, |A|, arg A.
    const auto complex_to_real = [this, load_split](const char* name, auto f) {
        kernel(named<T>(name), [load_split, f](CpuState& s, GuestMemory& m) {
            const auto a = load_split(m, s.x[0], stride_of(s.x[1]), s.x[4]);
            V c(a.re.size());
            for (std::size_t i = 0; i < c.size(); ++i) c[i] = f(a.re[i], a.im[i]);
            store<T>(m, s.x[2], stride_of(s.x[3]), c);
        });
    };
    complex_to_real("_vDSP_zvmags", [](T re, T im) { return re * re + im * im; });
    complex_to_real("_vDSP_zvabs", [](T re, T im) { return std::hypot(re, im); });
    complex_to_real("_vDSP_zvphas", [](T re, T im) { return std::atan2(im, re); });
    // zvconj / zvmov (A, IA, C, IC, N)
    for (const bool conjugate : {true, false})
        kernel(named<T>(conjugate ? "_vDSP_zvconj" : "_vDSP_zvmov"), [conjugate, load_split, store_split](CpuState& s, GuestMemory& m) {
            auto a = load_split(m, s.x[0], stride_of(s.x[1]), s.x[4]);
            if (conjugate) for (auto& v : a.im) v = -v;
            store_split(m, s.x[2], stride_of(s.x[3]), a);
        });
    // ctoz (const DSPComplex *C, IC (in T units, 2 for contiguous), Z, IZ, N); ztoc (Z, IZ, DSPComplex *C, IC, N).
    kernel(named<T>("_vDSP_ctoz"), [store_split](CpuState& s, GuestMemory& m) {
        const auto n = s.x[4];
        check_length(n);
        Split z{V(static_cast<std::size_t>(n)), V(static_cast<std::size_t>(n))};
        for (std::uint64_t i = 0; i < n; ++i) {
            const auto at = s.x[0] + static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * stride_of(s.x[1]) * static_cast<std::int64_t>(sizeof(T)));
            z.re[i] = load_one<T>(m, at);
            z.im[i] = load_one<T>(m, at + sizeof(T));
        }
        store_split(m, s.x[2], stride_of(s.x[3]), z);
    });
    kernel(named<T>("_vDSP_ztoc"), [load_split](CpuState& s, GuestMemory& m) {
        const auto z = load_split(m, s.x[0], stride_of(s.x[1]), s.x[4]);
        for (std::size_t i = 0; i < z.re.size(); ++i) {
            const auto at = s.x[2] + static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * stride_of(s.x[3]) * static_cast<std::int64_t>(sizeof(T)));
            store_one<T>(m, at, z.re[i]);
            store_one<T>(m, at + sizeof(T), z.im[i]);
        }
    });
    // desamp (A, DF, F, C, N, P): C[n] = sum_p A[n DF + p] F[p].
    kernel(named<T>("_vDSP_desamp"), [](CpuState& s, GuestMemory& m) {
        const std::uint64_t factor = s.x[1], n = s.x[4], taps = s.x[5];
        check_length(n);
        const auto filter = load<T>(m, s.x[2], 1, taps);
        const auto a = load<T>(m, s.x[0], 1, n ? (n - 1) * factor + taps : 0);
        V c(static_cast<std::size_t>(n));
        for (std::uint64_t i = 0; i < n; ++i) {
            double total = 0;
            for (std::uint64_t p = 0; p < taps; ++p) total += double(a[i * factor + p]) * filter[p];
            c[i] = static_cast<T>(total);
        }
        store<T>(m, s.x[3], 1, c);
    });
    install_fft<T>();
}

template <class T>
void Vectors::install_fft() {
    using C = std::complex<T>;
    const auto load_split = [](GuestMemory& m, Address split, std::int64_t stride, std::uint64_t n) {
        const auto re = load<T>(m, m.read64(split), stride, n), im = load<T>(m, m.read64(split + 8), stride, n);
        std::vector<C> out(re.size());
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = C(re[i], im[i]);
        return out;
    };
    const auto store_split = [](GuestMemory& m, Address split, std::int64_t stride, const std::vector<C>& v) {
        std::vector<T> re(v.size()), im(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) { re[i] = v[i].real(); im[i] = v[i].imag(); }
        store<T>(m, m.read64(split), stride, re);
        store<T>(m, m.read64(split + 8), stride, im);
    };
    // create_fftsetup(D) (vDSP_Length log2n, FFTRadix): radix 2 (0) only; destroy_fftsetup(D).
    kernel(std::is_same_v<T, double> ? "_vDSP_create_fftsetupD" : "_vDSP_create_fftsetup", [this](CpuState& s, GuestMemory&) {
        if (s.x[0] > 26 || static_cast<std::int32_t>(s.x[1]) != 0) { s.x[0] = 0; return; }
        s.x[0] = make_handle(std::make_shared<FftSetup>(FftSetup{s.x[0]}));
    });
    kernel(std::is_same_v<T, double> ? "_vDSP_destroy_fftsetupD" : "_vDSP_destroy_fftsetup", [this](CpuState& s, GuestMemory&) { drop(s.x[0]); });
    // A setup made here bounds the length; a single-precision one may come from vdsp.cpp's
    // create_fftsetup (radix 2 up to 2^26 as well).
    const auto check_setup = [this](Address setup, std::uint64_t log2n) {
        const auto found = handle<FftSetup>(setup);
        if (!setup || log2n > 26 || (found && log2n > found->log2n)) throw std::runtime_error("unsupported: Alchemy vDSP: an FFT longer than its setup");
    };
    // fft_zip(D) (setup, C, IC, log2n, direction): complex, in place. fft_zop(D) (setup, A, IA, C, IC, log2n, direction).
    kernel(named<T>("_vDSP_fft_zip"), [check_setup, load_split, store_split](CpuState& s, GuestMemory& m) {
        check_setup(s.x[0], s.x[3]);
        auto v = load_split(m, s.x[1], stride_of(s.x[2]), 1ULL << s.x[3]);
        fft(v, static_cast<std::int32_t>(s.x[4]) == -1);
        store_split(m, s.x[1], stride_of(s.x[2]), v);
    });
    kernel(named<T>("_vDSP_fft_zop"), [check_setup, load_split, store_split](CpuState& s, GuestMemory& m) {
        check_setup(s.x[0], s.x[5]);
        auto v = load_split(m, s.x[1], stride_of(s.x[2]), 1ULL << s.x[5]);
        fft(v, static_cast<std::int32_t>(s.x[6]) == -1);
        store_split(m, s.x[3], stride_of(s.x[4]), v);
    });
    // Packed real FFTs: N/2 complex values in, forward 2x the DFT (DC in realp[0], Nyquist in imagp[0]);
    // the inverse undoes the packing (the unnormalized inverse, so a round trip scales by 2N).
    const auto real_fft = [](std::vector<C> packed, bool inverse, std::size_t n) {
        std::vector<C> full(n);
        if (!inverse) {
            for (std::size_t i = 0; i < n / 2; ++i) { full[2 * i] = C(packed[i].real()); full[2 * i + 1] = C(packed[i].imag()); }
            fft(full, false);
            std::vector<C> out(n / 2);
            out[0] = C(2 * full[0].real(), 2 * full[n / 2].real());
            for (std::size_t k = 1; k < n / 2; ++k) out[k] = T(2) * full[k];
            return out;
        }
        full[0] = C(packed[0].real() / 2);
        full[n / 2] = C(packed[0].imag() / 2);
        for (std::size_t k = 1; k < n / 2; ++k) { full[k] = packed[k] / T(2); full[n - k] = std::conj(packed[k]) / T(2); }
        fft(full, true);
        std::vector<C> out(n / 2);
        for (std::size_t i = 0; i < n / 2; ++i) out[i] = C(T(2) * full[2 * i].real(), T(2) * full[2 * i + 1].real());
        return out;
    };
    if constexpr (std::is_same_v<T, double>) {
        kernel("_vDSP_fft_zripD", [check_setup, load_split, store_split, real_fft](CpuState& s, GuestMemory& m) {
            check_setup(s.x[0], s.x[3]);
            if (s.x[3] < 1) return;
            const auto n = std::size_t{1} << s.x[3];
            store_split(m, s.x[1], stride_of(s.x[2]), real_fft(load_split(m, s.x[1], stride_of(s.x[2]), n / 2), static_cast<std::int32_t>(s.x[4]) == -1, n));
        });
    }
    // fft_zrop(D) (setup, A, IA, C, IC, log2n, direction)
    kernel(named<T>("_vDSP_fft_zrop"), [check_setup, load_split, store_split, real_fft](CpuState& s, GuestMemory& m) {
        check_setup(s.x[0], s.x[5]);
        if (s.x[5] < 1) return;
        const auto n = std::size_t{1} << s.x[5];
        store_split(m, s.x[3], stride_of(s.x[4]), real_fft(load_split(m, s.x[1], stride_of(s.x[2]), n / 2), static_cast<std::int32_t>(s.x[6]) == -1, n));
    });
    // The DFT interface: DFT_zop_CreateSetup(D) / DFT_zrop_CreateSetup(D) (previous, vDSP_Length length,
    // vDSP_DFT_Direction (1 forward, -1 inverse)); DFT_Execute(D) (setup, Ir, Ii, Or, Oi); DFT_DestroySetup(D).
    const bool is_double = std::is_same_v<T, double>;
    for (const bool real : {false, true})
        kernel(std::string(real ? "_vDSP_DFT_zrop_CreateSetup" : "_vDSP_DFT_zop_CreateSetup") + (is_double ? "D" : ""), [this, real](CpuState& s, GuestMemory&) {
            auto length = s.x[1];
            while (length && !(length & 1)) length >>= 1;
            const bool valid = s.x[1] >= (real ? 2U : 1U) && (length == 1 || length == 3 || length == 5 || length == 15) && s.x[1] <= maximum_length &&
                               (!real || s.x[1] % 2 == 0);
            if (!valid) { s.x[0] = 0; return; }
            s.x[0] = make_handle(std::make_shared<DftSetup>(DftSetup{s.x[1], static_cast<std::int32_t>(s.x[2]) == -1, real}));
        });
    kernel(std::string("_vDSP_DFT_Execute") + (is_double ? "D" : ""), [this, real_fft](CpuState& s, GuestMemory& m) {
        const auto setup = handle<DftSetup>(s.x[0]);
        if (!setup) throw std::runtime_error("unsupported: Alchemy vDSP: DFT_Execute with an unknown setup");
        const auto n = static_cast<std::size_t>(setup->length);
        const auto count = setup->real ? n / 2 : n;
        const auto re = load<T>(m, s.x[1], 1, count), im = load<T>(m, s.x[2], 1, count);
        std::vector<C> v(count);
        for (std::size_t i = 0; i < count; ++i) v[i] = C(re[i], im[i]);
        if (!setup->real) {
            dft(v, setup->inverse);
        } else if (!(n & (n - 1))) {
            v = real_fft(v, setup->inverse, n);
        } else {
            // A real DFT of a length that is not a power of two: through the full complex DFT.
            std::vector<C> full(n);
            if (!setup->inverse) {
                for (std::size_t i = 0; i < count; ++i) { full[2 * i] = C(v[i].real()); full[2 * i + 1] = C(v[i].imag()); }
                dft(full, false);
                v[0] = C(2 * full[0].real(), 2 * full[n / 2].real());
                for (std::size_t k = 1; k < count; ++k) v[k] = T(2) * full[k];
            } else {
                full[0] = C(v[0].real() / 2);
                full[n / 2] = C(v[0].imag() / 2);
                for (std::size_t k = 1; k < count; ++k) { full[k] = v[k] / T(2); full[n - k] = std::conj(v[k]) / T(2); }
                dft(full, true);
                for (std::size_t i = 0; i < count; ++i) v[i] = C(T(2) * full[2 * i].real(), T(2) * full[2 * i + 1].real());
            }
        }
        std::vector<T> out_re(count), out_im(count);
        for (std::size_t i = 0; i < count; ++i) { out_re[i] = v[i].real(); out_im[i] = v[i].imag(); }
        store<T>(m, s.x[3], 1, out_re);
        store<T>(m, s.x[4], 1, out_im);
    });
    kernel(std::string("_vDSP_DFT_DestroySetup") + (is_double ? "D" : ""), [this](CpuState& s, GuestMemory&) { if (s.x[0]) drop(s.x[0]); });
    // Biquads: biquad_CreateSetup(D) (const double *coefficients, vDSP_Length sections): five per section,
    // b0 b1 b2 a1 a2; biquad(D) (setup, T *delay, X, IX, Y, IY, N); biquad_DestroySetup(D).
    kernel(std::string("_vDSP_biquad_CreateSetup") + (is_double ? "D" : ""), [this](CpuState& s, GuestMemory& m) {
        const auto sections = s.x[1];
        if (!s.x[0] || !sections || sections > 4096) { s.x[0] = 0; return; }
        auto setup = std::make_shared<Biquad>();
        setup->sections = sections;
        setup->coefficients = load<double>(m, s.x[0], 1, 5 * sections);
        s.x[0] = make_handle(setup);
    });
    kernel(std::string("_vDSP_biquad") + (is_double ? "D" : ""), [this](CpuState& s, GuestMemory& m) {
        const auto setup = handle<Biquad>(s.x[0]);
        if (!setup) throw std::runtime_error("unsupported: Alchemy vDSP: biquad with an unknown setup");
        auto x = load<T>(m, s.x[2], stride_of(s.x[3]), s.x[6]);
        // The delay array: x[n-1], x[n-2] of the input, then y[n-1], y[n-2] of each section's output.
        auto delay = load<T>(m, s.x[1], 1, 2 * setup->sections + 2);
        for (auto& sample : x) {
            double input = sample;
            for (std::uint64_t k = 0; k < setup->sections; ++k) {
                const auto* c = setup->coefficients.data() + 5 * k;
                const double x1 = delay[2 * k], x2 = delay[2 * k + 1], y1 = delay[2 * k + 2], y2 = delay[2 * k + 3];
                const double output = c[0] * input + c[1] * x1 + c[2] * x2 - c[3] * y1 - c[4] * y2;
                delay[2 * k] = static_cast<T>(input);
                delay[2 * k + 1] = static_cast<T>(x1);
                // The next section's inputs are this section's outputs (shared delay entries).
                if (k + 1 == setup->sections) {
                    delay[2 * k + 2] = static_cast<T>(output);
                    delay[2 * k + 3] = static_cast<T>(y1);
                }
                input = output;
            }
            sample = static_cast<T>(input);
        }
        store<T>(m, s.x[1], 1, delay);
        store<T>(m, s.x[4], stride_of(s.x[5]), x);
    });
    kernel(std::string("_vDSP_biquad_DestroySetup") + (is_double ? "D" : ""), [this](CpuState& s, GuestMemory&) { if (s.x[0]) drop(s.x[0]); });
}

void Vectors::install_conversions() {
    // vflt (integer A, IA, float C, IC, N) and vfix / vfixr (float A, IA, integer C, IC, N), D for double.
    const auto convert_from = [this]<class I>(const char* name, I) {
        for (const bool is_double : {false, true}) {
            kernel(std::string(name) + (is_double ? "D" : ""), [is_double](CpuState& s, GuestMemory& m) {
                const auto n = s.x[4];
                check_length(n);
                for (std::uint64_t i = 0; i < n; ++i) {
                    I value{};
                    const auto at = s.x[0] + static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * stride_of(s.x[1]) * static_cast<std::int64_t>(sizeof(I)));
                    std::uint64_t raw = sizeof(I) == 1 ? m.read8(at) : sizeof(I) == 2 ? static_cast<std::uint64_t>(m.read8(at) | (m.read8(at + 1) << 8)) : m.read32(at);
                    std::memcpy(&value, &raw, sizeof(I));
                    const auto out = s.x[2] + static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * stride_of(s.x[3]) * (is_double ? 8 : 4));
                    if (is_double) store_one<double>(m, out, static_cast<double>(value));
                    else store_one<float>(m, out, static_cast<float>(value));
                }
            });
        }
    };
    convert_from("_vDSP_vflt8", std::int8_t{});
    convert_from("_vDSP_vflt16", std::int16_t{});
    convert_from("_vDSP_vflt32", std::int32_t{});
    convert_from("_vDSP_vfltu8", std::uint8_t{});
    convert_from("_vDSP_vfltu16", std::uint16_t{});
    convert_from("_vDSP_vfltu32", std::uint32_t{});
    const auto convert_to = [this]<class I>(const char* name, bool rounding, I) {
        for (const bool is_double : {false, true}) {
            kernel(std::string(name) + (is_double ? "D" : ""), [is_double, rounding](CpuState& s, GuestMemory& m) {
                const auto n = s.x[4];
                check_length(n);
                for (std::uint64_t i = 0; i < n; ++i) {
                    const auto at = s.x[0] + static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * stride_of(s.x[1]) * (is_double ? 8 : 4));
                    double value = is_double ? load_one<double>(m, at) : load_one<float>(m, at);
                    value = rounding ? std::nearbyint(value) : std::trunc(value);
                    // Unsigned forms saturate; the signed ones too, as the integer type cannot hold more.
                    value = std::clamp(value, double(std::numeric_limits<I>::min()), double(std::numeric_limits<I>::max()));
                    const auto integer = static_cast<I>(value);
                    std::uint64_t raw{};
                    std::memcpy(&raw, &integer, sizeof(I));
                    const auto out = s.x[2] + static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * stride_of(s.x[3]) * static_cast<std::int64_t>(sizeof(I)));
                    for (unsigned b = 0; b < sizeof(I); ++b) m.write8(out + b, static_cast<std::uint8_t>(raw >> (8 * b)));
                }
            });
        }
    };
    convert_to("_vDSP_vfix8", false, std::int8_t{});
    convert_to("_vDSP_vfix16", false, std::int16_t{});
    convert_to("_vDSP_vfix32", false, std::int32_t{});
    convert_to("_vDSP_vfixr8", true, std::int8_t{});
    convert_to("_vDSP_vfixr16", true, std::int16_t{});
    convert_to("_vDSP_vfixr32", true, std::int32_t{});
    convert_to("_vDSP_vfixu8", false, std::uint8_t{});
    convert_to("_vDSP_vfixu16", false, std::uint16_t{});
    convert_to("_vDSP_vfixu32", false, std::uint32_t{});
    convert_to("_vDSP_vfixru8", true, std::uint8_t{});
    convert_to("_vDSP_vfixru16", true, std::uint16_t{});
    convert_to("_vDSP_vfixru32", true, std::uint32_t{});
    // vspdp (float A, IA, double C, IC, N), vdpsp (double A, IA, float C, IC, N)
    kernel("_vDSP_vspdp", [](CpuState& s, GuestMemory& m) {
        const auto a = load<float>(m, s.x[0], stride_of(s.x[1]), s.x[4]);
        store<double>(m, s.x[2], stride_of(s.x[3]), std::vector<double>(a.begin(), a.end()));
    });
    kernel("_vDSP_vdpsp", [](CpuState& s, GuestMemory& m) {
        const auto a = load<double>(m, s.x[0], stride_of(s.x[1]), s.x[4]);
        std::vector<float> c(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) c[i] = static_cast<float>(a[i]);
        store<float>(m, s.x[2], stride_of(s.x[3]), c);
    });
}

void Vectors::install() {
    install_type<float>();
    install_type<double>();
    install_conversions();
}

} // namespace

void install_vdsp_vectors(InstallContext& context) { std::make_shared<Vectors>(context)->install(); }

} // namespace alchemy::gui::fill
