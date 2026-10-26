#include "alchemy/darwin/accelerate/install.hpp"
#include "alchemy/gui/fill_provider.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <limits>
#include <optional>
#include <string>
#include <numbers>
#include <vector>

// Accelerate vDSP kernels the Qt WebEngine images bind (Chromium's audio
// code). All are real implementations of the documented semantics, computed
// in double precision on the host:
//   vadd, vsub (C = A - B, B first), vmul, vsadd, vsmul, vsma, vclip, maxmgv,
//   svesq, conv (correlation; a negative filter stride gives convolution),
//   deq22D (2-pole 2-zero filter, double), ctoz/ztoc (IC counts floats and is
//   even for contiguous complex values), zvmul (Conjugate -1 conjugates A),
//   create/destroy_fftsetup (radix 2 only) and fft_zrip (packed real FFT:
//   forward results are 2x the DFT, DC in realp[0] and Nyquist in imagp[0];
//   the inverse is the unnormalized inverse DFT, so a round trip scales by 2N).
// QLab's (gap group G5): maxv, rmsqv, vclr, vlint, vmaxmg, vmmsb, vramp, and
// the LP64 BLAS/LAPACK entries cblas_dgemm (row- or column-major, either
// operand transposed) and dgesv_ (LU with partial pivoting, column-major,
// 1-based pivots, INFO as LAPACK documents), and libndi's vImage 8-bit
// Y'CbCr <-> ARGB conversions (BT.601-4 / BT.709-2 matrices, pixel ranges,
// permute maps; see install_vimage).
namespace alchemy::gui::fill {
namespace {

constexpr std::uint64_t maximum_length = 1ULL << 26;

std::int64_t stride_of(std::uint64_t value) { return static_cast<std::int64_t>(value); }

std::vector<double> read_floats(GuestMemory& m, Address base, std::int64_t stride, std::uint64_t count) {
    if (count > maximum_length) throw std::runtime_error("Alchemy vDSP: vector too long");
    std::vector<double> values(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i)
        values[static_cast<std::size_t>(i)] = std::bit_cast<float>(m.read32(base + static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * stride * 4)));
    return values;
}
void write_floats(GuestMemory& m, Address base, std::int64_t stride, const std::vector<double>& values) {
    for (std::size_t i = 0; i < values.size(); ++i)
        m.write32(base + static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * stride * 4), std::bit_cast<std::uint32_t>(static_cast<float>(values[i])));
}
double read_float(GuestMemory& m, Address address) { return std::bit_cast<float>(m.read32(address)); }

void fft(std::vector<std::complex<double>>& a, bool inverse) {
    const auto n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = 2 * std::numbers::pi / static_cast<double>(length) * (inverse ? 1 : -1);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += length) {
            std::complex<double> w(1);
            for (std::size_t j = 0; j < length / 2; ++j) {
                const auto u = a[i + j], v = a[i + j + length / 2] * w;
                a[i + j] = u + v;
                a[i + j + length / 2] = u - v;
                w *= step;
            }
        }
    }
}

class Accelerate final : public Provider {
public:
    using Provider::Provider;

    template <class Op>
    void binary(const char* name, bool b_first, Op op) {
        // (A, IA, B, IB, C, IC, N); vDSP_vsub passes B first.
        raw(accelerate, name, [b_first, op](CpuState& s, GuestMemory& m) {
            const auto n = s.x[6];
            auto a = read_floats(m, b_first ? s.x[2] : s.x[0], stride_of(b_first ? s.x[3] : s.x[1]), n);
            const auto b = read_floats(m, b_first ? s.x[0] : s.x[2], stride_of(b_first ? s.x[1] : s.x[3]), n);
            for (std::size_t i = 0; i < a.size(); ++i) a[i] = op(a[i], b[i]);
            write_floats(m, s.x[4], stride_of(s.x[5]), a);
        });
    }
    template <class Op>
    void scalar(const char* name, Op op) {
        // (A, IA, const float *B, C, IC, N)
        raw(accelerate, name, [op](CpuState& s, GuestMemory& m) {
            auto a = read_floats(m, s.x[0], stride_of(s.x[1]), s.x[5]);
            const auto b = read_float(m, s.x[2]);
            for (auto& value : a) value = op(value, b);
            write_floats(m, s.x[3], stride_of(s.x[4]), a);
        });
    }
    void install();
    void install_vimage();
};

void Accelerate::install() {
    install_vimage();
    binary("_vDSP_vadd", false, [](double a, double b) { return a + b; });
    binary("_vDSP_vsub", true, [](double a, double b) { return a - b; });
    binary("_vDSP_vmul", false, [](double a, double b) { return a * b; });
    scalar("_vDSP_vsadd", [](double a, double b) { return a + b; });
    scalar("_vDSP_vsmul", [](double a, double b) { return a * b; });
    // vDSP_vsma(A, IA, const float *B, C, IC, D, ID, N): D = A * B + C
    raw(accelerate, "_vDSP_vsma", [](CpuState& s, GuestMemory& m) {
        auto a = read_floats(m, s.x[0], stride_of(s.x[1]), s.x[7]);
        const auto b = read_float(m, s.x[2]);
        const auto c = read_floats(m, s.x[3], stride_of(s.x[4]), s.x[7]);
        for (std::size_t i = 0; i < a.size(); ++i) a[i] = a[i] * b + c[i];
        write_floats(m, s.x[5], stride_of(s.x[6]), a);
    });
    // vDSP_vclip(A, IA, const float *low, const float *high, D, ID, N)
    raw(accelerate, "_vDSP_vclip", [](CpuState& s, GuestMemory& m) {
        auto a = read_floats(m, s.x[0], stride_of(s.x[1]), s.x[6]);
        const auto low = read_float(m, s.x[2]), high = read_float(m, s.x[3]);
        for (auto& value : a) value = value < low ? low : value > high ? high : value;
        write_floats(m, s.x[4], stride_of(s.x[5]), a);
    });
    // vDSP_maxmgv(A, IA, float *C, N): the largest magnitude (0 for N = 0).
    raw(accelerate, "_vDSP_maxmgv", [](CpuState& s, GuestMemory& m) {
        const auto a = read_floats(m, s.x[0], stride_of(s.x[1]), s.x[3]);
        double result = 0;
        for (const auto value : a) result = std::max(result, std::fabs(value));
        m.write32(s.x[2], std::bit_cast<std::uint32_t>(static_cast<float>(result)));
    });
    // vDSP_svesq(A, IA, float *C, N): the sum of squares.
    raw(accelerate, "_vDSP_svesq", [](CpuState& s, GuestMemory& m) {
        const auto a = read_floats(m, s.x[0], stride_of(s.x[1]), s.x[3]);
        double sum = 0;
        for (const auto value : a) sum += value * value;
        m.write32(s.x[2], std::bit_cast<std::uint32_t>(static_cast<float>(sum)));
    });
    // vDSP_conv(A, IA, F, IF, C, IC, N, P): C[n] = sum_p A[n + p] * F[p].
    raw(accelerate, "_vDSP_conv", [](CpuState& s, GuestMemory& m) {
        const auto n = s.x[6], p = s.x[7];
        if (n > maximum_length || p > maximum_length) throw std::runtime_error("Alchemy vDSP: vector too long");
        const auto a = read_floats(m, s.x[0], stride_of(s.x[1]), n + p - (p ? 1 : 0));
        const auto f = read_floats(m, s.x[2], stride_of(s.x[3]), p);
        std::vector<double> c(static_cast<std::size_t>(n), 0);
        for (std::size_t i = 0; i < c.size(); ++i)
            for (std::size_t k = 0; k < f.size(); ++k) c[i] += a[i + k] * f[k];
        write_floats(m, s.x[4], stride_of(s.x[5]), c);
    });
    // vDSP_deq22D(const double *A, IA, const double *B, double *C, IC, N): A and C
    // hold N + 2 values; C[0..1] are the initial outputs.
    raw(accelerate, "_vDSP_deq22D", [](CpuState& s, GuestMemory& m) {
        const auto ia = stride_of(s.x[1]), ic = stride_of(s.x[4]);
        const auto n = s.x[5];
        if (n > maximum_length) throw std::runtime_error("Alchemy vDSP: vector too long");
        const auto at = [](Address base, std::int64_t stride, std::uint64_t index) {
            return base + static_cast<std::uint64_t>(static_cast<std::int64_t>(index) * stride * 8);
        };
        std::array<double, 5> b{};
        for (unsigned i = 0; i < 5; ++i) b[i] = std::bit_cast<double>(m.read64(s.x[2] + i * 8ULL));
        double c1 = std::bit_cast<double>(m.read64(at(s.x[3], ic, 1))), c2 = std::bit_cast<double>(m.read64(at(s.x[3], ic, 0)));
        double a1 = std::bit_cast<double>(m.read64(at(s.x[0], ia, 1))), a2 = std::bit_cast<double>(m.read64(at(s.x[0], ia, 0)));
        for (std::uint64_t i = 2; i < n + 2; ++i) {
            const double a0 = std::bit_cast<double>(m.read64(at(s.x[0], ia, i)));
            const double c0 = a0 * b[0] + a1 * b[1] + a2 * b[2] - c1 * b[3] - c2 * b[4];
            m.write64(at(s.x[3], ic, i), std::bit_cast<std::uint64_t>(c0));
            a2 = a1; a1 = a0; c2 = c1; c1 = c0;
        }
    });
    // vDSP_ctoz(const DSPComplex *C, IC, const DSPSplitComplex *Z, IZ, N)
    raw(accelerate, "_vDSP_ctoz", [](CpuState& s, GuestMemory& m) {
        const auto ic = stride_of(s.x[1]), iz = stride_of(s.x[3]);
        const auto n = s.x[4];
        if (n > maximum_length) throw std::runtime_error("Alchemy vDSP: vector too long");
        const auto real = m.read64(s.x[2]), imag = m.read64(s.x[2] + 8);
        for (std::uint64_t i = 0; i < n; ++i) {
            const auto from = s.x[0] + static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * ic * 4);
            const auto to = static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * iz * 4);
            m.write32(real + to, m.read32(from));
            m.write32(imag + to, m.read32(from + 4));
        }
    });
    // vDSP_ztoc(const DSPSplitComplex *Z, IZ, DSPComplex *C, IC, N)
    raw(accelerate, "_vDSP_ztoc", [](CpuState& s, GuestMemory& m) {
        const auto iz = stride_of(s.x[1]), ic = stride_of(s.x[3]);
        const auto n = s.x[4];
        if (n > maximum_length) throw std::runtime_error("Alchemy vDSP: vector too long");
        const auto real = m.read64(s.x[0]), imag = m.read64(s.x[0] + 8);
        for (std::uint64_t i = 0; i < n; ++i) {
            const auto from = static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * iz * 4);
            const auto to = s.x[2] + static_cast<std::uint64_t>(static_cast<std::int64_t>(i) * ic * 4);
            m.write32(to, m.read32(real + from));
            m.write32(to + 4, m.read32(imag + from));
        }
    });
    // vDSP_zvmul(A, IA, B, IB, C, IC, N, Conjugate): C = (Conjugate == -1 ? conj(A) : A) * B
    raw(accelerate, "_vDSP_zvmul", [](CpuState& s, GuestMemory& m) {
        const auto n = s.x[6];
        const bool conjugate = static_cast<std::int32_t>(s.x[7]) == -1;
        const auto ar = read_floats(m, m.read64(s.x[0]), stride_of(s.x[1]), n), ai = read_floats(m, m.read64(s.x[0] + 8), stride_of(s.x[1]), n);
        const auto br = read_floats(m, m.read64(s.x[2]), stride_of(s.x[3]), n), bi = read_floats(m, m.read64(s.x[2] + 8), stride_of(s.x[3]), n);
        std::vector<double> cr(ar.size()), ci(ar.size());
        for (std::size_t i = 0; i < ar.size(); ++i) {
            const std::complex<double> a(ar[i], conjugate ? -ai[i] : ai[i]), b(br[i], bi[i]);
            const auto c = a * b;
            cr[i] = c.real();
            ci[i] = c.imag();
        }
        write_floats(m, m.read64(s.x[4]), stride_of(s.x[5]), cr);
        write_floats(m, m.read64(s.x[4] + 8), stride_of(s.x[5]), ci);
    });
    // FFTSetup vDSP_create_fftsetup(log2n, radix): radix 2 only (kFFTRadix2 = 0).
    raw(accelerate, "_vDSP_create_fftsetup", [this](CpuState& s, GuestMemory& m) {
        if (s.x[1] != 0 || s.x[0] > 24) { s.x[0] = 0; return; }
        const auto setup = malloc(16);
        m.write64(setup, fourcc("AFFT"));
        m.write64(setup + 8, s.x[0]);
        s.x[0] = setup;
    });
    raw(accelerate, "_vDSP_destroy_fftsetup", [this](CpuState& s, GuestMemory&) { free(s.x[0]); });
    // vDSP_fft_zrip(setup, const DSPSplitComplex *C, IC, log2n, direction)
    raw(accelerate, "_vDSP_fft_zrip", [](CpuState& s, GuestMemory& m) {
        const auto setup = s.x[0];
        const auto log2n = s.x[3];
        if (!setup || m.read64(setup) != fourcc("AFFT") || log2n > m.read64(setup + 8))
            throw std::runtime_error("Alchemy vDSP: vDSP_fft_zrip with an invalid or too small FFT setup");
        if (log2n == 0) return;
        const auto n = std::size_t{1} << log2n, half = n / 2;
        const auto stride = stride_of(s.x[2]);
        const auto real_base = m.read64(s.x[1]), imag_base = m.read64(s.x[1] + 8);
        auto re = read_floats(m, real_base, stride, half), im = read_floats(m, imag_base, stride, half);
        const bool forward = static_cast<std::int32_t>(s.x[4]) == 1;
        std::vector<std::complex<double>> a(n);
        if (forward) {
            for (std::size_t k = 0; k < half; ++k) { a[2 * k] = re[k]; a[2 * k + 1] = im[k]; }
            fft(a, false);
            re[0] = 2 * a[0].real();
            im[0] = 2 * a[half].real();
            for (std::size_t k = 1; k < half; ++k) { re[k] = 2 * a[k].real(); im[k] = 2 * a[k].imag(); }
        } else {
            a[0] = re[0];
            a[half] = im[0];
            for (std::size_t k = 1; k < half; ++k) {
                a[k] = {re[k], im[k]};
                a[n - k] = std::conj(a[k]);
            }
            fft(a, true);
            for (std::size_t k = 0; k < half; ++k) { re[k] = a[2 * k].real(); im[k] = a[2 * k + 1].real(); }
        }
        write_floats(m, real_base, stride, re);
        write_floats(m, imag_base, stride, im);
    });

    // vDSP_maxv(A, IA, float *C, N): the largest element (-INFINITY for N = 0).
    raw(accelerate, "_vDSP_maxv", [](CpuState& s, GuestMemory& m) {
        const auto a = read_floats(m, s.x[0], stride_of(s.x[1]), s.x[3]);
        double result = -std::numeric_limits<double>::infinity();
        for (const auto value : a) result = std::max(result, value);
        m.write32(s.x[2], std::bit_cast<std::uint32_t>(static_cast<float>(result)));
    });
    // vDSP_rmsqv(A, IA, float *C, N): sqrt(sum(A[n]^2) / N); 0 for N = 0 (no samples, no energy).
    raw(accelerate, "_vDSP_rmsqv", [](CpuState& s, GuestMemory& m) {
        const auto a = read_floats(m, s.x[0], stride_of(s.x[1]), s.x[3]);
        double sum = 0;
        for (const auto value : a) sum += value * value;
        const double result = a.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(a.size()));
        m.write32(s.x[2], std::bit_cast<std::uint32_t>(static_cast<float>(result)));
    });
    // vDSP_vclr(C, IC, N): C = 0.
    raw(accelerate, "_vDSP_vclr", [](CpuState& s, GuestMemory& m) {
        if (s.x[2] > maximum_length) throw std::runtime_error("Alchemy vDSP: vector too long");
        write_floats(m, s.x[0], stride_of(s.x[1]), std::vector<double>(static_cast<std::size_t>(s.x[2]), 0.0));
    });
    // vDSP_vlint(A, B, IB, C, IC, N, M): C[n] = A[i] + f * (A[i + 1] - A[i]) with i = trunc(B[n]),
    // f = B[n] - i, over the M-element table A. Indices outside the table are an error.
    raw(accelerate, "_vDSP_vlint", [](CpuState& s, GuestMemory& m) {
        const auto n = s.x[5], table_length = s.x[6];
        const auto table = read_floats(m, s.x[0], 1, table_length);
        const auto b = read_floats(m, s.x[1], stride_of(s.x[2]), n);
        std::vector<double> c(b.size());
        for (std::size_t k = 0; k < b.size(); ++k) {
            const double whole = std::trunc(b[k]);
            if (!(whole >= 0) || whole + 1 >= static_cast<double>(table_length)) {
                if (whole + 1 == static_cast<double>(table_length) && b[k] == whole) { c[k] = table.back(); continue; }
                throw std::runtime_error("Alchemy vDSP: vDSP_vlint index " + std::to_string(b[k]) + " is outside the table");
            }
            const auto i = static_cast<std::size_t>(whole);
            c[k] = table[i] + (b[k] - whole) * (table[i + 1] - table[i]);
        }
        write_floats(m, s.x[3], stride_of(s.x[4]), c);
    });
    // vDSP_vmaxmg(A, IA, B, IB, C, IC, N): C = max(|A|, |B|).
    raw(accelerate, "_vDSP_vmaxmg", [](CpuState& s, GuestMemory& m) {
        auto a = read_floats(m, s.x[0], stride_of(s.x[1]), s.x[6]);
        const auto b = read_floats(m, s.x[2], stride_of(s.x[3]), s.x[6]);
        for (std::size_t i = 0; i < a.size(); ++i) a[i] = std::max(std::fabs(a[i]), std::fabs(b[i]));
        write_floats(m, s.x[4], stride_of(s.x[5]), a);
    });
    // vDSP_vmmsb(A, IA, B, IB, C, IC, D, ID, [stack] E, IE, N): E = A * B - C * D.
    raw(accelerate, "_vDSP_vmmsb", [](CpuState& s, GuestMemory& m) {
        const auto e = m.read64(s.sp), ie = m.read64(s.sp + 8), n = m.read64(s.sp + 16);
        auto a = read_floats(m, s.x[0], stride_of(s.x[1]), n);
        const auto b = read_floats(m, s.x[2], stride_of(s.x[3]), n);
        const auto c = read_floats(m, s.x[4], stride_of(s.x[5]), n);
        const auto d = read_floats(m, s.x[6], stride_of(s.x[7]), n);
        for (std::size_t i = 0; i < a.size(); ++i) a[i] = a[i] * b[i] - c[i] * d[i];
        write_floats(m, e, stride_of(ie), a);
    });
    // vDSP_vramp(const float *A, const float *B, C, IC, N): C[n] = A + n * B.
    raw(accelerate, "_vDSP_vramp", [](CpuState& s, GuestMemory& m) {
        if (s.x[4] > maximum_length) throw std::runtime_error("Alchemy vDSP: vector too long");
        const auto start = read_float(m, s.x[0]), step = read_float(m, s.x[1]);
        std::vector<double> c(static_cast<std::size_t>(s.x[4]));
        for (std::size_t i = 0; i < c.size(); ++i) c[i] = start + static_cast<double>(i) * step;
        write_floats(m, s.x[2], stride_of(s.x[3]), c);
    });
    // cblas_dgemm(Order, TransA, TransB, int M, int N, int K, double alpha, const double *A, int lda,
    //     [stack] const double *B, int ldb, double beta (d1), double *C, int ldc):
    //     C = alpha * op(A) * op(B) + beta * C. Darwin packs stack arguments at their natural alignment.
    raw(accelerate, "_cblas_dgemm", [](CpuState& s, GuestMemory& m) {
        const auto order = static_cast<std::int32_t>(s.x[0]), trans_a = static_cast<std::int32_t>(s.x[1]), trans_b = static_cast<std::int32_t>(s.x[2]);
        const auto rows = static_cast<std::int32_t>(s.x[3]), columns = static_cast<std::int32_t>(s.x[4]), inner = static_cast<std::int32_t>(s.x[5]);
        const double alpha = real(s, 0), beta = real(s, 1);
        const auto a = s.x[6];
        const auto lda = static_cast<std::int32_t>(s.x[7]);
        const auto b = m.read64(s.sp);
        const auto ldb = static_cast<std::int32_t>(m.read32(s.sp + 8));
        const auto c = m.read64(s.sp + 16);
        const auto ldc = static_cast<std::int32_t>(m.read32(s.sp + 24));
        constexpr std::int32_t row_major = 101, col_major = 102, no_trans = 111, trans = 112, conj_trans = 113;
        const auto valid_trans = [](std::int32_t t) { return t == no_trans || t == trans || t == conj_trans; };
        if ((order != row_major && order != col_major) || !valid_trans(trans_a) || !valid_trans(trans_b) || rows < 0 || columns < 0 || inner < 0)
            throw std::runtime_error("Alchemy BLAS: cblas_dgemm called with an invalid argument");
        if (static_cast<std::uint64_t>(rows) * static_cast<std::uint64_t>(columns) > maximum_length ||
            static_cast<std::uint64_t>(rows) * static_cast<std::uint64_t>(inner) > maximum_length ||
            static_cast<std::uint64_t>(inner) * static_cast<std::uint64_t>(columns) > maximum_length)
            throw std::runtime_error("Alchemy BLAS: cblas_dgemm matrices too large");
        // Element (i, j) of a matrix stored with leading dimension ld in `order`.
        const auto at = [order](Address base, std::int32_t ld, std::int64_t i, std::int64_t j) {
            const auto index = order == row_major ? i * ld + j : j * ld + i;
            return base + static_cast<std::uint64_t>(index) * 8;
        };
        const auto load = [&m](Address address) { return std::bit_cast<double>(m.read64(address)); };
        std::vector<double> result(static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns));
        for (std::int64_t i = 0; i < rows; ++i)
            for (std::int64_t j = 0; j < columns; ++j) {
                double sum = 0;
                if (alpha != 0)
                    for (std::int64_t k = 0; k < inner; ++k) {
                        const double x = trans_a == no_trans ? load(at(a, lda, i, k)) : load(at(a, lda, k, i));
                        const double y = trans_b == no_trans ? load(at(b, ldb, k, j)) : load(at(b, ldb, j, k));
                        sum += x * y;
                    }
                // BLAS: when beta is zero, C need not be set on input (NaNs there are not propagated).
                const double previous = beta == 0 ? 0.0 : beta * load(at(c, ldc, i, j));
                result[static_cast<std::size_t>(i * columns + j)] = alpha * sum + previous;
            }
        for (std::int64_t i = 0; i < rows; ++i)
            for (std::int64_t j = 0; j < columns; ++j)
                m.write64(at(c, ldc, i, j), std::bit_cast<std::uint64_t>(result[static_cast<std::size_t>(i * columns + j)]));
    });
    // dgesv_(int *N, int *NRHS, double *A, int *LDA, int *IPIV, double *B, int *LDB, int *INFO):
    // solves A * X = B by LU factorization with partial pivoting (column-major). On exit A holds
    // L and U, IPIV the 1-based row interchanges and B the solution; INFO = -i for an illegal
    // i-th argument, i > 0 when U(i,i) is exactly zero (B is then not solved).
    raw(accelerate, "_dgesv_", [](CpuState& s, GuestMemory& m) {
        const auto info = s.x[7];
        const auto n = static_cast<std::int32_t>(m.read32(s.x[0])), nrhs = static_cast<std::int32_t>(m.read32(s.x[1]));
        const auto lda = static_cast<std::int32_t>(m.read32(s.x[3])), ldb = static_cast<std::int32_t>(m.read32(s.x[6]));
        std::int32_t error = 0;
        if (n < 0) error = -1;
        else if (nrhs < 0) error = -2;
        else if (lda < std::max(1, n)) error = -4;
        else if (ldb < std::max(1, n)) error = -7;
        if (error) { m.write32(info, static_cast<std::uint32_t>(error)); return; }
        if (static_cast<std::uint64_t>(n) * static_cast<std::uint64_t>(n) > maximum_length) throw std::runtime_error("Alchemy LAPACK: dgesv_ matrix too large");
        const auto a_at = [&](std::int64_t i, std::int64_t j) { return s.x[2] + static_cast<std::uint64_t>(j * lda + i) * 8; };
        const auto b_at = [&](std::int64_t i, std::int64_t j) { return s.x[5] + static_cast<std::uint64_t>(j * ldb + i) * 8; };
        std::vector<double> a(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
        for (std::int64_t j = 0; j < n; ++j)
            for (std::int64_t i = 0; i < n; ++i) a[static_cast<std::size_t>(j * n + i)] = std::bit_cast<double>(m.read64(a_at(i, j)));
        const auto A = [&](std::int64_t i, std::int64_t j) -> double& { return a[static_cast<std::size_t>(j * n + i)]; };
        std::vector<std::int32_t> pivots(static_cast<std::size_t>(n));
        std::int32_t singular = 0;
        for (std::int64_t k = 0; k < n; ++k) {
            std::int64_t pivot = k;
            for (std::int64_t i = k + 1; i < n; ++i) if (std::fabs(A(i, k)) > std::fabs(A(pivot, k))) pivot = i;
            pivots[static_cast<std::size_t>(k)] = static_cast<std::int32_t>(pivot + 1);
            if (A(pivot, k) == 0) { if (!singular) singular = static_cast<std::int32_t>(k + 1); continue; }
            if (pivot != k) for (std::int64_t j = 0; j < n; ++j) std::swap(A(k, j), A(pivot, j));
            for (std::int64_t i = k + 1; i < n; ++i) A(i, k) /= A(k, k);
            for (std::int64_t j = k + 1; j < n; ++j)
                for (std::int64_t i = k + 1; i < n; ++i) A(i, j) -= A(i, k) * A(k, j);
        }
        for (std::int64_t j = 0; j < n; ++j)
            for (std::int64_t i = 0; i < n; ++i) m.write64(a_at(i, j), std::bit_cast<std::uint64_t>(A(i, j)));
        for (std::int64_t k = 0; k < n; ++k) m.write32(s.x[4] + static_cast<std::uint64_t>(k) * 4, static_cast<std::uint32_t>(pivots[static_cast<std::size_t>(k)]));
        if (singular) { m.write32(info, static_cast<std::uint32_t>(singular)); return; }
        for (std::int64_t r = 0; r < nrhs; ++r) {
            std::vector<double> x(static_cast<std::size_t>(n));
            for (std::int64_t i = 0; i < n; ++i) x[static_cast<std::size_t>(i)] = std::bit_cast<double>(m.read64(b_at(i, r)));
            for (std::int64_t k = 0; k < n; ++k) std::swap(x[static_cast<std::size_t>(k)], x[static_cast<std::size_t>(pivots[static_cast<std::size_t>(k)] - 1)]);
            for (std::int64_t i = 0; i < n; ++i)
                for (std::int64_t k = 0; k < i; ++k) x[static_cast<std::size_t>(i)] -= A(i, k) * x[static_cast<std::size_t>(k)];
            for (std::int64_t i = n - 1; i >= 0; --i) {
                for (std::int64_t k = i + 1; k < n; ++k) x[static_cast<std::size_t>(i)] -= A(i, k) * x[static_cast<std::size_t>(k)];
                x[static_cast<std::size_t>(i)] /= A(i, i);
            }
            for (std::int64_t i = 0; i < n; ++i) m.write64(b_at(i, r), std::bit_cast<std::uint64_t>(x[static_cast<std::size_t>(i)]));
        }
        m.write32(info, 0);
    });
}

// ---- vImage Y'CbCr <-> ARGB (8-bit) ---------------------------------------------------------
// Matrices (vImage_Types.h): vImage_YpCbCrToARGBMatrix {Yp, Cr_R, Cr_G, Cb_G, Cb_B} and
// vImage_ARGBToYpCbCrMatrix {R_Yp, G_Yp, B_Yp, R_Cb, G_Cb, B_Cb_R_Cr, G_Cr, B_Cr}; the
// kvImage_* exports are pointers to the ITU-R BT.601-4 and BT.709-2 constants.
// vImage_YpCbCrPixelRange {Yp_bias, CbCr_bias, YpRangeMax, CbCrRangeMax, YpMax, YpMin, CbCrMax, CbCrMin}.
// The generated conversion (an opaque 128-byte vImage_YpCbCrToARGB / vImage_ARGBToYpCbCr)
// holds the matrix and the range. With y' = (Y' - Yp_bias) / (YpRangeMax - Yp_bias) and
// c = (C - CbCr_bias) / (2 (CbCrRangeMax - CbCr_bias)):
//   R = Yp y' + Cr_R cr, G = Yp y' + Cr_G cr + Cb_G cb, B = Yp y' + Cb_B cb   (x 255, rounded, clamped)
// and the inverse scales back, clamping Y' to [YpMin, YpMax] and Cb/Cr to [CbCrMin, CbCrMax].
// Permute maps: to ARGB, destination channel i is ARGB channel map[i]; from ARGB, ARGB
// channel i is source channel map[i] (NULL: the identity). 4:2:2 chroma is the mean of the
// pixel pair; 4:2:0 chroma is shared by each 2x2 block (nearest sample, no interpolation).
namespace vimage {
constexpr std::int64_t no_error = 0, roi_larger_than_input = -21766, null_pointer = -21772, invalid_parameter = -21773;
constexpr std::uint32_t to_argb_magic = 0x59434132, to_ycbcr_magic = 0x41594332;   // "YCA2", "AYC2"

struct Buffer { Address data{}; std::uint64_t height{}, width{}, row_bytes{}; };
Buffer read_buffer(GuestMemory& m, Address at) { return {m.read64(at), m.read64(at + 8), m.read64(at + 16), m.read64(at + 24)}; }

struct Range { std::int32_t yp_bias, cbcr_bias, yp_range_max, cbcr_range_max, yp_max, yp_min, cbcr_max, cbcr_min; };
Range read_range(GuestMemory& m, Address at) {
    std::array<std::int32_t, 8> v{};
    for (unsigned i = 0; i < 8; ++i) v[i] = static_cast<std::int32_t>(m.read32(at + 4 * i));
    return {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]};
}
bool valid(const Range& r) {
    return r.yp_range_max > r.yp_bias && r.cbcr_range_max > r.cbcr_bias && r.yp_max >= r.yp_min && r.cbcr_max >= r.cbcr_min;
}

struct ToArgb { std::array<float, 5> matrix{}; Range range{}; };
struct ToYcbcr { std::array<float, 8> matrix{}; Range range{}; };

// The opaque info layout: magic, then the matrix floats, then the range.
template <std::size_t N>
void write_info(GuestMemory& m, Address at, std::uint32_t magic, const std::array<float, N>& matrix, const Range& range) {
    m.fill_bytes(at, 0, 128);
    m.write32(at, magic);
    for (std::size_t i = 0; i < N; ++i) m.write32(at + 4 + 4 * i, std::bit_cast<std::uint32_t>(matrix[i]));
    const std::array<std::int32_t, 8> values{range.yp_bias, range.cbcr_bias, range.yp_range_max, range.cbcr_range_max,
                                             range.yp_max, range.yp_min, range.cbcr_max, range.cbcr_min};
    for (std::size_t i = 0; i < 8; ++i) m.write32(at + 4 + 4 * N + 4 * i, static_cast<std::uint32_t>(values[i]));
}
template <std::size_t N>
std::optional<std::pair<std::array<float, N>, Range>> read_info(GuestMemory& m, Address at, std::uint32_t magic) {
    if (!at || m.read32(at) != magic) return std::nullopt;
    std::array<float, N> matrix{};
    for (std::size_t i = 0; i < N; ++i) matrix[i] = std::bit_cast<float>(m.read32(at + 4 + 4 * i));
    return std::pair{matrix, read_range(m, at + 4 + 4 * N)};
}

std::uint8_t to_byte(double unit) { return static_cast<std::uint8_t>(std::clamp(std::lround(unit * 255.0), 0L, 255L)); }

// One Y'CbCr sample to (A, R, G, B) with alpha.
std::array<std::uint8_t, 4> to_argb(const ToArgb& c, int y, int cb, int cr, std::uint8_t alpha) {
    const auto& r = c.range;
    y = std::clamp(y, r.yp_min, r.yp_max);
    cb = std::clamp(cb, r.cbcr_min, r.cbcr_max);
    cr = std::clamp(cr, r.cbcr_min, r.cbcr_max);
    const double yn = static_cast<double>(y - r.yp_bias) / (r.yp_range_max - r.yp_bias);
    const double chroma = 2.0 * (r.cbcr_range_max - r.cbcr_bias);
    const double cbn = (cb - r.cbcr_bias) / chroma, crn = (cr - r.cbcr_bias) / chroma;
    const auto& k = c.matrix;   // Yp, Cr_R, Cr_G, Cb_G, Cb_B
    return {alpha, to_byte(k[0] * yn + k[1] * crn), to_byte(k[0] * yn + k[2] * crn + k[3] * cbn), to_byte(k[0] * yn + k[4] * cbn)};
}

// Normalized (y', cb, cr) of one ARGB pixel.
std::array<double, 3> to_ycbcr(const ToYcbcr& c, std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    const double r = red / 255.0, g = green / 255.0, b = blue / 255.0;
    const auto& k = c.matrix;   // R_Yp, G_Yp, B_Yp, R_Cb, G_Cb, B_Cb_R_Cr, G_Cr, B_Cr
    return {k[0] * r + k[1] * g + k[2] * b, k[3] * r + k[4] * g + k[5] * b, k[5] * r + k[6] * g + k[7] * b};
}
std::uint8_t encode_luma(const Range& r, double y) {
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(y * (r.yp_range_max - r.yp_bias) + r.yp_bias)), std::max(r.yp_min, 0), std::min(r.yp_max, 255)));
}
std::uint8_t encode_chroma(const Range& r, double c) {
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(c * 2.0 * (r.cbcr_range_max - r.cbcr_bias) + r.cbcr_bias)), std::max(r.cbcr_min, 0), std::min(r.cbcr_max, 255)));
}

std::array<std::uint8_t, 4> permute_map(GuestMemory& m, Address at) {
    if (!at) return {0, 1, 2, 3};
    std::array<std::uint8_t, 4> map{};
    for (unsigned i = 0; i < 4; ++i) map[i] = m.read8(at + i);
    return map;
}
bool valid_map(const std::array<std::uint8_t, 4>& map) { return std::all_of(map.begin(), map.end(), [](std::uint8_t v) { return v < 4; }); }
} // namespace vimage

void Accelerate::install_vimage() {
    using namespace vimage;
    // The matrices, exported as `const vImage_*Matrix *` constants.
    const auto matrix_export = [this](const char* name, std::initializer_list<float> values) {
        std::vector<std::uint8_t> bytes;
        for (const auto v : values) for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(std::bit_cast<std::uint32_t>(v) >> (8 * i)));
        const auto storage = process.allocate_guest(bytes.size(), 16);
        process.main_engine().write_bytes(storage, bytes);
        value(accelerate, name, storage);
    };
    matrix_export("_kvImage_YpCbCrToARGBMatrix_ITU_R_601_4", {1.0f, 1.402f, -0.7141363f, -0.3441363f, 1.772f});
    matrix_export("_kvImage_YpCbCrToARGBMatrix_ITU_R_709_2", {1.0f, 1.5748f, -0.4681243f, -0.1873243f, 1.8556f});
    matrix_export("_kvImage_ARGBToYpCbCrMatrix_ITU_R_601_4", {0.299f, 0.587f, 0.114f, -0.1687359f, -0.3312641f, 0.5f, -0.4186876f, -0.0813124f});
    matrix_export("_kvImage_ARGBToYpCbCrMatrix_ITU_R_709_2", {0.2126f, 0.7152f, 0.0722f, -0.1145721f, -0.3854279f, 0.5f, -0.4541529f, -0.0458471f});

    // vImageConvert_YpCbCrToARGB_GenerateConversion(const matrix *, const pixelRange *, vImage_YpCbCrToARGB *out,
    //     vImageYpCbCrType, vImageARGBType, vImage_Flags)
    raw(accelerate, "_vImageConvert_YpCbCrToARGB_GenerateConversion", [](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1] || !s.x[2]) { s.x[0] = static_cast<std::uint64_t>(null_pointer); return; }
        const auto range = read_range(m, s.x[1]);
        if (!valid(range)) { s.x[0] = static_cast<std::uint64_t>(invalid_parameter); return; }
        std::array<float, 5> matrix{};
        for (unsigned i = 0; i < 5; ++i) matrix[i] = std::bit_cast<float>(m.read32(s.x[0] + 4 * i));
        write_info(m, s.x[2], to_argb_magic, matrix, range);
        s.x[0] = no_error;
    });
    // vImageConvert_ARGBToYpCbCr_GenerateConversion(const matrix *, const pixelRange *, vImage_ARGBToYpCbCr *out,
    //     vImageARGBType, vImageYpCbCrType, vImage_Flags)
    raw(accelerate, "_vImageConvert_ARGBToYpCbCr_GenerateConversion", [](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1] || !s.x[2]) { s.x[0] = static_cast<std::uint64_t>(null_pointer); return; }
        const auto range = read_range(m, s.x[1]);
        if (!valid(range)) { s.x[0] = static_cast<std::uint64_t>(invalid_parameter); return; }
        std::array<float, 8> matrix{};
        for (unsigned i = 0; i < 8; ++i) matrix[i] = std::bit_cast<float>(m.read32(s.x[0] + 4 * i));
        write_info(m, s.x[2], to_ycbcr_magic, matrix, range);
        s.x[0] = no_error;
    });
    // vImageConvert_420Yp8_CbCr8ToARGB8888(const vImage_Buffer *srcYp, const vImage_Buffer *srcCbCr, const vImage_Buffer *dest,
    //     const vImage_YpCbCrToARGB *info, const uint8_t permuteMap[4], uint8_t alpha, vImage_Flags flags)
    raw(accelerate, "_vImageConvert_420Yp8_CbCr8ToARGB8888", [](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1] || !s.x[2] || !s.x[3]) { s.x[0] = static_cast<std::uint64_t>(null_pointer); return; }
        const auto info = read_info<5>(m, s.x[3], to_argb_magic);
        const auto map = permute_map(m, s.x[4]);
        if (!info || !valid_map(map)) { s.x[0] = static_cast<std::uint64_t>(invalid_parameter); return; }
        const ToArgb c{info->first, info->second};
        const auto alpha = static_cast<std::uint8_t>(s.x[5]);
        const auto luma = read_buffer(m, s.x[0]), chroma = read_buffer(m, s.x[1]), dest = read_buffer(m, s.x[2]);
        if (luma.width < dest.width || luma.height < dest.height || chroma.width * 2 < dest.width || chroma.height * 2 < dest.height) {
            s.x[0] = static_cast<std::uint64_t>(roi_larger_than_input);
            return;
        }
        if (dest.width * dest.height > maximum_length) throw std::runtime_error("Alchemy vImage: image too large");
        for (std::uint64_t row = 0; row < dest.height; ++row) {
            std::vector<std::uint8_t> y(static_cast<std::size_t>(dest.width)), uv(static_cast<std::size_t>((dest.width + 1) / 2 * 2));
            m.read_bytes(luma.data + row * luma.row_bytes, y);
            m.read_bytes(chroma.data + (row / 2) * chroma.row_bytes, uv);
            std::vector<std::uint8_t> out(static_cast<std::size_t>(dest.width * 4));
            for (std::size_t x = 0; x < y.size(); ++x) {
                const auto argb = to_argb(c, y[x], uv[x / 2 * 2], uv[x / 2 * 2 + 1], alpha);
                for (unsigned i = 0; i < 4; ++i) out[x * 4 + i] = argb[map[i]];
            }
            m.write_bytes(dest.data + row * dest.row_bytes, out);
        }
        s.x[0] = no_error;
    });
    // vImageConvert_422CbYpCrYp8ToARGB8888(const vImage_Buffer *src, const vImage_Buffer *dest,
    //     const vImage_YpCbCrToARGB *info, const uint8_t permuteMap[4], uint8_t alpha, vImage_Flags flags)
    raw(accelerate, "_vImageConvert_422CbYpCrYp8ToARGB8888", [](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1] || !s.x[2]) { s.x[0] = static_cast<std::uint64_t>(null_pointer); return; }
        const auto info = read_info<5>(m, s.x[2], to_argb_magic);
        const auto map = permute_map(m, s.x[3]);
        if (!info || !valid_map(map)) { s.x[0] = static_cast<std::uint64_t>(invalid_parameter); return; }
        const ToArgb c{info->first, info->second};
        const auto alpha = static_cast<std::uint8_t>(s.x[4]);
        const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
        if (src.width < dest.width || src.height < dest.height) { s.x[0] = static_cast<std::uint64_t>(roi_larger_than_input); return; }
        if (dest.width * dest.height > maximum_length) throw std::runtime_error("Alchemy vImage: image too large");
        for (std::uint64_t row = 0; row < dest.height; ++row) {
            std::vector<std::uint8_t> in(static_cast<std::size_t>((dest.width + 1) / 2 * 4));
            m.read_bytes(src.data + row * src.row_bytes, in);
            std::vector<std::uint8_t> out(static_cast<std::size_t>(dest.width * 4));
            for (std::size_t x = 0; x < dest.width; ++x) {
                const auto* pair = in.data() + x / 2 * 4;   // Cb Y0 Cr Y1
                const auto argb = to_argb(c, pair[x % 2 ? 3 : 1], pair[0], pair[2], alpha);
                for (unsigned i = 0; i < 4; ++i) out[x * 4 + i] = argb[map[i]];
            }
            m.write_bytes(dest.data + row * dest.row_bytes, out);
        }
        s.x[0] = no_error;
    });
    // vImageConvert_ARGB8888To422CbYpCrYp8(const vImage_Buffer *src, const vImage_Buffer *dest,
    //     const vImage_ARGBToYpCbCr *info, const uint8_t permuteMap[4], vImage_Flags flags)
    raw(accelerate, "_vImageConvert_ARGB8888To422CbYpCrYp8", [](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1] || !s.x[2]) { s.x[0] = static_cast<std::uint64_t>(null_pointer); return; }
        const auto info = read_info<8>(m, s.x[2], to_ycbcr_magic);
        const auto map = permute_map(m, s.x[3]);
        if (!info || !valid_map(map)) { s.x[0] = static_cast<std::uint64_t>(invalid_parameter); return; }
        const ToYcbcr c{info->first, info->second};
        const auto src = read_buffer(m, s.x[0]), dest = read_buffer(m, s.x[1]);
        if (src.width < dest.width || src.height < dest.height) { s.x[0] = static_cast<std::uint64_t>(roi_larger_than_input); return; }
        if (dest.width * dest.height > maximum_length) throw std::runtime_error("Alchemy vImage: image too large");
        for (std::uint64_t row = 0; row < dest.height; ++row) {
            std::vector<std::uint8_t> in(static_cast<std::size_t>(dest.width * 4));
            m.read_bytes(src.data + row * src.row_bytes, in);
            std::vector<std::uint8_t> out(static_cast<std::size_t>((dest.width + 1) / 2 * 4));
            for (std::size_t x = 0; x < dest.width; x += 2) {
                // An odd last pixel pairs with itself.
                const auto* p0 = in.data() + x * 4;
                const auto* p1 = x + 1 < dest.width ? p0 + 4 : p0;
                const auto a = to_ycbcr(c, p0[map[1]], p0[map[2]], p0[map[3]]);
                const auto b = to_ycbcr(c, p1[map[1]], p1[map[2]], p1[map[3]]);
                auto* o = out.data() + x / 2 * 4;
                o[0] = encode_chroma(c.range, (a[1] + b[1]) / 2);
                o[1] = encode_luma(c.range, a[0]);
                o[2] = encode_chroma(c.range, (a[2] + b[2]) / 2);
                o[3] = encode_luma(c.range, b[0]);
            }
            m.write_bytes(dest.data + row * dest.row_bytes, out);
        }
        s.x[0] = no_error;
    });
}

} // namespace

void install_accelerate(InstallContext& context) {
    std::make_shared<Accelerate>(context)->install();
    install_vdsp_vectors(context);   // after the kernels above, which keep their names
    install_vforce(context);
    install_vimage(context);
    install_blas(context);
    install_vdsp_more(context);
}

} // namespace alchemy::gui::fill
