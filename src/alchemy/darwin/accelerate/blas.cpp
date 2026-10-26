#include "alchemy/darwin/accelerate/install.hpp"
#include "alchemy/gui/fill_provider.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// BLAS (cblas.h and the Fortran interface), levels 1-3, in single, double, single complex and
// double complex precision, computed on the host in double precision (complex<double>), reading
// and writing the operands in place in guest memory:
// - CBLAS, `cblas_*` and `cblas_*$NEWLAPACK`: row- or column-major storage (CblasRowMajor,
//   CblasColMajor); negative increments walk vectors backwards; complex scalars by reference.
//   Level 1: axpy, scal (and csscal/zdscal), copy, swap, dot (sdsdot, dsdot; dotu_sub/dotc_sub),
//   nrm2, asum, i?amax, rot, rotg, rotm. Level 2: gemv, ger (geru/gerc), symv/hemv, trmv, trsv,
//   syr/her. Level 3: gemm, symm/hemm, syrk/herk, syr2k, trmm, trsm.
// - Fortran subroutines, every argument by reference and column-major: `?gemm_`, `?gemv_`,
//   `?axpy_`, `?scal_`, `?copy_`, `?swap_`, `?trsm_`, `?syrk_`, `?ger_` (real) and the LP64
//   `$NEWLAPACK` names of the same, with the `$NEWLAPACK` functions ?dot, ?nrm2, ?asum and i?amax.
//   (The original interface's REAL functions, `sdot_` and the like, are not offered.)
// - The legacy LAPACK names (`?gesv_`, `?getrf_`, `?gesdd_`, ...) are the `$NEWLAPACK` drivers
//   (darwin/accelerate/lapack.cpp, darwin/libsystem/misc/numeric.cpp) under their other name: the
//   two interfaces agree on LP64 for subroutines.
// The `$NEWLAPACK` names numeric.cpp registers itself keep their implementation there.
namespace alchemy::gui::fill {
namespace {

constexpr std::int64_t maximum_elements = 1LL << 26;
// CBLAS_ORDER, CBLAS_TRANSPOSE, CBLAS_UPLO, CBLAS_DIAG, CBLAS_SIDE
constexpr std::int64_t row_major = 101, no_trans = 111, conj_trans = 113, upper = 121, unit = 132, left = 141;

// Arguments by the arm64 Apple calling convention: integers and pointers in x0-x7, float and
// double in v0-v7, the rest on the stack at their natural alignment.
class Args {
public:
    Args(const CpuState& s, GuestMemory& m) : s_(s), m_(m) {}
    std::int64_t integer() { return gp_ < 8 ? static_cast<std::int32_t>(s_.x[gp_++]) : static_cast<std::int32_t>(m_.read32(stack(4))); }
    Address pointer() { return gp_ < 8 ? s_.x[gp_++] : m_.read64(stack(8)); }
    template <class R> R real() {
        if constexpr (std::is_same_v<R, float>)
            return fp_ < 8 ? std::bit_cast<float>(static_cast<std::uint32_t>(s_.v[fp_++][0])) : std::bit_cast<float>(m_.read32(stack(4)));
        else
            return fp_ < 8 ? std::bit_cast<double>(s_.v[fp_++][0]) : std::bit_cast<double>(m_.read64(stack(8)));
    }

private:
    Address stack(std::uint64_t size) {
        offset_ = (offset_ + size - 1) / size * size;
        const auto at = s_.sp + offset_;
        offset_ += size;
        return at;
    }
    const CpuState& s_;
    GuestMemory& m_;
    unsigned gp_{}, fp_{};
    std::uint64_t offset_{};
};

template <class T> struct Scalar { using Real = T; using Work = double; static constexpr bool complex = false; };
template <class R> struct Scalar<std::complex<R>> { using Real = R; using Work = std::complex<double>; static constexpr bool complex = true; };
template <class T> using Work = typename Scalar<T>::Work;
template <class T> using Real = typename Scalar<T>::Real;

template <class T> Work<T> load(GuestMemory& m, Address at) {
    if constexpr (std::is_same_v<T, float>) return std::bit_cast<float>(m.read32(at));
    else if constexpr (std::is_same_v<T, double>) return std::bit_cast<double>(m.read64(at));
    else return {static_cast<double>(load<Real<T>>(m, at)), static_cast<double>(load<Real<T>>(m, at + sizeof(Real<T>)))};
}
template <class T> void store(GuestMemory& m, Address at, Work<T> value) {
    if constexpr (std::is_same_v<T, float>) m.write32(at, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
    else if constexpr (std::is_same_v<T, double>) m.write64(at, std::bit_cast<std::uint64_t>(value));
    else {
        store<Real<T>>(m, at, value.real());
        store<Real<T>>(m, at + sizeof(Real<T>), value.imag());
    }
}
template <class W> W conj_if(W value, bool conjugate) {
    if constexpr (std::is_same_v<W, double>) return value;
    else return conjugate ? std::conj(value) : value;
}
template <class W> double magnitude1(W value) {   // |re| + |im| (i?amax, ?asum)
    if constexpr (std::is_same_v<W, double>) return std::abs(value);
    else return std::abs(value.real()) + std::abs(value.imag());
}
void check(std::int64_t elements) {
    if (elements > maximum_elements) throw std::runtime_error("unsupported: Alchemy BLAS: more than 2^26 elements in one operand");
}

// A strided vector of n elements (a negative increment starts at the far end).
template <class T> struct Vec {
    Address base{};
    std::int64_t n{}, inc{1};
    [[nodiscard]] Address at(std::int64_t i) const {
        const auto step = static_cast<std::int64_t>(sizeof(T));
        return base + static_cast<Address>((inc >= 0 ? i * inc : (n - 1 - i) * -inc) * step);
    }
    std::vector<Work<T>> read(GuestMemory& m) const {
        check(n);
        std::vector<Work<T>> out(static_cast<std::size_t>(std::max<std::int64_t>(n, 0)));
        for (std::int64_t i = 0; i < n; ++i) out[static_cast<std::size_t>(i)] = load<T>(m, at(i));
        return out;
    }
    void write(GuestMemory& m, const std::vector<Work<T>>& values) const {
        for (std::int64_t i = 0; i < n; ++i) store<T>(m, at(i), values[static_cast<std::size_t>(i)]);
    }
};

// A matrix in either storage order, as logical rows x cols.
template <class T> struct Mat {
    Address base{};
    std::int64_t ld{};
    bool by_rows{};
    [[nodiscard]] Address at(std::int64_t i, std::int64_t j) const {
        return base + static_cast<Address>((by_rows ? i * ld + j : j * ld + i) * static_cast<std::int64_t>(sizeof(T)));
    }
    std::vector<Work<T>> read(GuestMemory& m, std::int64_t rows, std::int64_t cols) const {
        check(rows * cols);
        std::vector<Work<T>> out(static_cast<std::size_t>(rows * cols));
        for (std::int64_t i = 0; i < rows; ++i)
            for (std::int64_t j = 0; j < cols; ++j) out[static_cast<std::size_t>(i * cols + j)] = load<T>(m, at(i, j));
        return out;
    }
    void write(GuestMemory& m, const std::vector<Work<T>>& values, std::int64_t rows, std::int64_t cols) const {
        for (std::int64_t i = 0; i < rows; ++i)
            for (std::int64_t j = 0; j < cols; ++j) store<T>(m, at(i, j), values[static_cast<std::size_t>(i * cols + j)]);
    }
};

// Dense row-major host matrices of Work values.
template <class W> struct Dense {
    std::int64_t rows{}, cols{};
    std::vector<W> v;
    W& operator()(std::int64_t i, std::int64_t j) { return v[static_cast<std::size_t>(i * cols + j)]; }
    W operator()(std::int64_t i, std::int64_t j) const { return v[static_cast<std::size_t>(i * cols + j)]; }
};
// op(A) of a stored rows x cols matrix: itself, its transpose or its conjugate transpose.
template <class W> Dense<W> op(const Dense<W>& a, std::int64_t trans) {
    if (trans == no_trans) return a;
    Dense<W> t{a.cols, a.rows, std::vector<W>(a.v.size())};
    for (std::int64_t i = 0; i < a.rows; ++i)
        for (std::int64_t j = 0; j < a.cols; ++j) t(j, i) = conj_if(a(i, j), trans == conj_trans);
    return t;
}
template <class T> Dense<Work<T>> dense(GuestMemory& m, const Mat<T>& a, std::int64_t rows, std::int64_t cols) {
    return {rows, cols, a.read(m, rows, cols)};
}
// The triangle `uplo` of a square matrix (the other zero; a unit diagonal when asked).
template <class W> Dense<W> triangle(Dense<W> a, std::int64_t uplo, std::int64_t diag) {
    for (std::int64_t i = 0; i < a.rows; ++i)
        for (std::int64_t j = 0; j < a.cols; ++j) {
            if (i == j && diag == unit) a(i, j) = W{1};
            else if (uplo == upper ? i > j : i < j) a(i, j) = W{};
        }
    return a;
}
// A symmetric (or Hermitian) matrix from its triangle `uplo`.
template <class W> Dense<W> mirrored(Dense<W> a, std::int64_t uplo, bool hermitian) {
    for (std::int64_t i = 0; i < a.rows; ++i)
        for (std::int64_t j = 0; j < a.cols; ++j) {
            if (uplo == upper ? i > j : i < j) a(i, j) = conj_if(a(j, i), hermitian);
            if (i == j && hermitian) a(i, j) = W{std::real(a(i, j))};
        }
    return a;
}
template <class W> Dense<W> product(const Dense<W>& a, const Dense<W>& b) {
    Dense<W> c{a.rows, b.cols, std::vector<W>(static_cast<std::size_t>(a.rows * b.cols))};
    for (std::int64_t i = 0; i < a.rows; ++i)
        for (std::int64_t k = 0; k < a.cols; ++k) {
            const W x = a(i, k);
            if (x == W{}) continue;
            for (std::int64_t j = 0; j < b.cols; ++j) c(i, j) += x * b(k, j);
        }
    return c;
}
// Solves T X = B in place for a triangular T (lower or upper, by its zeros' side `lower`).
template <class W> void solve(const Dense<W>& t, bool lower, Dense<W>& b) {
    const auto n = t.rows;
    for (std::int64_t c = 0; c < b.cols; ++c) {
        if (lower) {
            for (std::int64_t i = 0; i < n; ++i) {
                W sum = b(i, c);
                for (std::int64_t k = 0; k < i; ++k) sum -= t(i, k) * b(k, c);
                b(i, c) = sum / t(i, i);
            }
        } else {
            for (std::int64_t i = n; i-- > 0;) {
                W sum = b(i, c);
                for (std::int64_t k = i + 1; k < n; ++k) sum -= t(i, k) * b(k, c);
                b(i, c) = sum / t(i, i);
            }
        }
    }
}
// Whether op(triangle) is lower triangular.
bool lower_after(std::int64_t uplo, std::int64_t trans) { return (uplo == upper) == (trans != no_trans); }

// The result of C = alpha * P + beta * C over the rows x cols of C (beta == 0 ignores C's values),
// only within the triangle `uplo` when given.
template <class T> void update(GuestMemory& m, const Mat<T>& c, const Dense<Work<T>>& p, Work<T> alpha, Work<T> beta, std::int64_t uplo = 0,
                               bool real_diagonal = false) {
    for (std::int64_t i = 0; i < p.rows; ++i)
        for (std::int64_t j = 0; j < p.cols; ++j) {
            if (uplo && (uplo == upper ? i > j : i < j)) continue;
            const auto previous = beta == Work<T>{} ? Work<T>{} : beta * load<T>(m, c.at(i, j));
            auto value = alpha * p(i, j) + previous;
            if (real_diagonal && i == j) value = Work<T>{std::real(value)};
            store<T>(m, c.at(i, j), value);
        }
}

class Blas final : public Provider {
public:
    using Provider::Provider;
    void install() {
        install_type<float>('s');
        install_type<double>('d');
        install_type<std::complex<float>>('c');
        install_type<std::complex<double>>('z');
        install_mixed();
        install_lapack_names();
    }

private:
    // `_name` unless a provider has it already (numeric.cpp's `$NEWLAPACK` names, vdsp.cpp's).
    void entry(const std::string& name, transmute::GuestCallHandler handler, const std::source_location& where = std::source_location::current()) {
        if (has(accelerate, "_" + name)) return;
        raw(accelerate, "_" + name, std::move(handler), where);
    }
    void cblas(const std::string& name, const transmute::GuestCallHandler& handler) {
        entry("cblas_" + name, handler);
        entry("cblas_" + name + "$NEWLAPACK", handler);
    }
    void fortran(const std::string& name, const transmute::GuestCallHandler& handler) {
        entry(name + "_", handler);
        entry(name + "$NEWLAPACK", handler);
    }
    template <class T> void install_type(char p);
    void install_mixed();
    transmute::GuestCallHandler dot_;   // the real ?dot of the type being installed
    void install_lapack_names();
};

template <class T> void return_real(CpuState& s, double value) {
    if constexpr (std::is_same_v<Real<T>, float>) s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(value)), 0};
    else s.v[0] = {std::bit_cast<std::uint64_t>(value), 0};
}
// A scalar argument: by value for real types, by reference for complex ones.
template <class T> Work<T> scalar(Args& a, GuestMemory& m) {
    if constexpr (Scalar<T>::complex) return load<T>(m, a.pointer());
    else return a.template real<T>();
}

// ---- the operations, on decoded arguments -------------------------------------------------------

template <class T> void gemm(GuestMemory& m, bool by_rows, std::int64_t ta, std::int64_t tb, std::int64_t rows, std::int64_t cols, std::int64_t k,
                             Work<T> alpha, Address a, std::int64_t lda, Address b, std::int64_t ldb, Work<T> beta, Address c, std::int64_t ldc) {
    if (rows <= 0 || cols <= 0) return;
    const auto stored_a = dense(m, Mat<T>{a, lda, by_rows}, ta == no_trans ? rows : k, ta == no_trans ? k : rows);
    const auto stored_b = dense(m, Mat<T>{b, ldb, by_rows}, tb == no_trans ? k : cols, tb == no_trans ? cols : k);
    const auto p = k > 0 && alpha != Work<T>{} ? product(op(stored_a, ta), op(stored_b, tb))
                                                : Dense<Work<T>>{rows, cols, std::vector<Work<T>>(static_cast<std::size_t>(rows * cols))};
    update<T>(m, Mat<T>{c, ldc, by_rows}, p, alpha, beta);
}

template <class T> void gemv(GuestMemory& m, bool by_rows, std::int64_t trans, std::int64_t rows, std::int64_t cols, Work<T> alpha, Address a,
                             std::int64_t lda, Address x, std::int64_t incx, Work<T> beta, Address y, std::int64_t incy) {
    if (rows <= 0 || cols <= 0) return;
    const auto stored = op(dense(m, Mat<T>{a, lda, by_rows}, rows, cols), trans);
    const Vec<T> vx{x, stored.cols, incx}, vy{y, stored.rows, incy};
    const auto xs = vx.read(m);
    auto ys = beta == Work<T>{} ? std::vector<Work<T>>(static_cast<std::size_t>(stored.rows)) : vy.read(m);
    for (std::int64_t i = 0; i < stored.rows; ++i) {
        Work<T> sum{};
        for (std::int64_t j = 0; j < stored.cols; ++j) sum += stored(i, j) * xs[static_cast<std::size_t>(j)];
        ys[static_cast<std::size_t>(i)] = alpha * sum + beta * ys[static_cast<std::size_t>(i)];
    }
    vy.write(m, ys);
}

template <class T> void trsm(GuestMemory& m, bool by_rows, std::int64_t side, std::int64_t uplo, std::int64_t trans, std::int64_t diag, std::int64_t rows,
                             std::int64_t cols, Work<T> alpha, Address a, std::int64_t lda, Address b, std::int64_t ldb) {
    if (rows <= 0 || cols <= 0) return;
    const auto order = side == left ? rows : cols;
    const auto t = op(triangle(dense(m, Mat<T>{a, lda, by_rows}, order, order), uplo, diag), trans);
    const Mat<T> mb{b, ldb, by_rows};
    auto rhs = dense(m, mb, rows, cols);
    for (auto& value : rhs.v) value *= alpha;
    if (side == left) {
        solve(t, lower_after(uplo, trans), rhs);
    } else {   // X op(A) = B  <=>  op(A)^T X^T = B^T
        auto transposed = op(rhs, 112);
        solve(op(t, 112), !lower_after(uplo, trans), transposed);
        rhs = op(transposed, 112);
    }
    mb.write(m, rhs.v, rows, cols);
}

template <class T> void trmm(GuestMemory& m, bool by_rows, std::int64_t side, std::int64_t uplo, std::int64_t trans, std::int64_t diag, std::int64_t rows,
                             std::int64_t cols, Work<T> alpha, Address a, std::int64_t lda, Address b, std::int64_t ldb) {
    if (rows <= 0 || cols <= 0) return;
    const auto order = side == left ? rows : cols;
    const auto t = op(triangle(dense(m, Mat<T>{a, lda, by_rows}, order, order), uplo, diag), trans);
    const Mat<T> mb{b, ldb, by_rows};
    const auto stored = dense(m, mb, rows, cols);
    auto p = side == left ? product(t, stored) : product(stored, t);
    for (auto& value : p.v) value *= alpha;
    mb.write(m, p.v, rows, cols);
}

// C = alpha op(A) op(A)^T + beta C (syrk), or with the conjugate transpose (herk), in triangle uplo.
template <class T> void rank_k(GuestMemory& m, bool by_rows, std::int64_t uplo, std::int64_t trans, std::int64_t n, std::int64_t k, Work<T> alpha,
                               Address a, std::int64_t lda, Work<T> beta, Address c, std::int64_t ldc, bool hermitian) {
    if (n <= 0) return;
    const auto stored = dense(m, Mat<T>{a, lda, by_rows}, trans == no_trans ? n : k, trans == no_trans ? k : n);
    const auto left_factor = trans == no_trans ? stored : op(stored, hermitian ? conj_trans : 112);
    const auto p = k > 0 ? product(left_factor, op(left_factor, hermitian ? conj_trans : 112))
                         : Dense<Work<T>>{n, n, std::vector<Work<T>>(static_cast<std::size_t>(n * n))};
    update<T>(m, Mat<T>{c, ldc, by_rows}, p, alpha, beta, uplo, hermitian);
}

template <class T>
void Blas::install_type(char p) {
    using W = Work<T>;
    const std::string s(1, p);
    constexpr bool complex = Scalar<T>::complex;
    // ---- level 1 ------------------------------------------------------------------------------
    // axpy(N, alpha, X, incX, Y, incY)
    const auto axpy = [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const auto n = a.integer();
        const auto alpha = scalar<T>(a, m);
        const Vec<T> x{a.pointer(), n, a.integer()};
        const Vec<T> y{a.pointer(), n, a.integer()};
        if (n <= 0) return;
        const auto xs = x.read(m);
        auto ys = y.read(m);
        for (std::size_t i = 0; i < ys.size(); ++i) ys[i] += alpha * xs[i];
        y.write(m, ys);
    };
    cblas(s + "axpy", axpy);
    // scal(N, alpha, X, incX)
    const auto scal = [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const auto n = a.integer();
        const auto alpha = scalar<T>(a, m);
        const Vec<T> x{a.pointer(), n, a.integer()};
        if (n <= 0) return;
        auto xs = x.read(m);
        for (auto& v : xs) v *= alpha;
        x.write(m, xs);
    };
    cblas(s + "scal", scal);
    // copy and swap (N, X, incX, Y, incY)
    const auto copy = [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const auto n = a.integer();
        const Vec<T> x{a.pointer(), n, a.integer()};
        const Vec<T> y{a.pointer(), n, a.integer()};
        if (n > 0) y.write(m, x.read(m));
    };
    cblas(s + "copy", copy);
    const auto swap = [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const auto n = a.integer();
        const Vec<T> x{a.pointer(), n, a.integer()};
        const Vec<T> y{a.pointer(), n, a.integer()};
        if (n <= 0) return;
        const auto xs = x.read(m), ys = y.read(m);
        x.write(m, ys);
        y.write(m, xs);
    };
    cblas(s + "swap", swap);
    // nrm2 and asum (N, X, incX), real results (scnrm2, dznrm2, scasum, dzasum for complex).
    const auto nrm2 = [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const auto n = a.integer();
        const Vec<T> x{a.pointer(), n, a.integer()};
        double scale = 0, sum = 1;   // the reference algorithm's scaled sum of squares
        if (n > 0)
            for (const auto value : x.read(m))
                for (const double part : {std::real(value), std::imag(value)}) {
                    if (part == 0) continue;
                    const double absolute = std::abs(part);
                    if (scale < absolute) { sum = 1 + sum * (scale / absolute) * (scale / absolute); scale = absolute; }
                    else sum += (absolute / scale) * (absolute / scale);
                }
        return_real<T>(st, scale * std::sqrt(sum));
    };
    const auto asum = [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const auto n = a.integer();
        const Vec<T> x{a.pointer(), n, a.integer()};
        double sum = 0;
        if (n > 0)
            for (const auto value : x.read(m)) sum += magnitude1(value);
        return_real<T>(st, sum);
    };
    const std::string real_prefix = complex ? (p == 'c' ? "sc" : "dz") : s;
    cblas(real_prefix + "nrm2", nrm2);
    cblas(real_prefix + "asum", asum);
    // i?amax(N, X, incX): the 0-based index of the first largest |re| + |im|.
    const auto iamax = [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const auto n = a.integer();
        const Vec<T> x{a.pointer(), n, a.integer()};
        std::int64_t best = 0;
        if (n > 0) {
            const auto xs = x.read(m);
            for (std::size_t i = 1; i < xs.size(); ++i)
                if (magnitude1(xs[i]) > magnitude1(xs[static_cast<std::size_t>(best)])) best = static_cast<std::int64_t>(i);
        }
        st.x[0] = static_cast<std::uint64_t>(best);
    };
    cblas("i" + s + "amax", iamax);
    if constexpr (complex) {
        // dotu_sub/dotc_sub(N, X, incX, Y, incY, void *result)
        for (const bool conjugated : {false, true}) {
            cblas(s + (conjugated ? "dotc_sub" : "dotu_sub"), [conjugated](CpuState& st, GuestMemory& m) {
                Args a(st, m);
                const auto n = a.integer();
                const Vec<T> x{a.pointer(), n, a.integer()};
                const Vec<T> y{a.pointer(), n, a.integer()};
                const auto out = a.pointer();
                W sum{};
                if (n > 0) {
                    const auto xs = x.read(m), ys = y.read(m);
                    for (std::size_t i = 0; i < xs.size(); ++i) sum += conj_if(xs[i], conjugated) * ys[i];
                }
                store<T>(m, out, sum);
            });
        }
        // csscal/zdscal(N, real alpha, X, incX)
        cblas(std::string(1, p) + (p == 'c' ? "sscal" : "dscal"), [](CpuState& st, GuestMemory& m) {
            Args a(st, m);
            const auto n = a.integer();
            const double alpha = a.template real<Real<T>>();
            const Vec<T> x{a.pointer(), n, a.integer()};
            if (n <= 0) return;
            auto xs = x.read(m);
            for (auto& v : xs) v *= alpha;
            x.write(m, xs);
        });
    } else {
        // dot(N, X, incX, Y, incY)
        const auto dot = [](CpuState& st, GuestMemory& m) {
            Args a(st, m);
            const auto n = a.integer();
            const Vec<T> x{a.pointer(), n, a.integer()};
            const Vec<T> y{a.pointer(), n, a.integer()};
            double sum = 0;
            if (n > 0) {
                const auto xs = x.read(m), ys = y.read(m);
                for (std::size_t i = 0; i < xs.size(); ++i) sum += xs[i] * ys[i];
            }
            return_real<T>(st, sum);
        };
        cblas(s + "dot", dot);
        dot_ = dot;
        // rot(N, X, incX, Y, incY, c, s)
        cblas(s + "rot", [](CpuState& st, GuestMemory& m) {
            Args a(st, m);
            const auto n = a.integer();
            const Vec<T> x{a.pointer(), n, a.integer()};
            const Vec<T> y{a.pointer(), n, a.integer()};
            const double c = a.template real<T>(), sn = a.template real<T>();
            if (n <= 0) return;
            auto xs = x.read(m), ys = y.read(m);
            for (std::size_t i = 0; i < xs.size(); ++i) {
                const auto u = xs[i], v = ys[i];
                xs[i] = c * u + sn * v;
                ys[i] = c * v - sn * u;
            }
            x.write(m, xs);
            y.write(m, ys);
        });
        // rotg(a, b, c, s): the Givens rotation zeroing b; a becomes r, b the reconstruction value z.
        cblas(s + "rotg", [](CpuState& st, GuestMemory& m) {
            const auto pa = st.x[0], pb = st.x[1], pc = st.x[2], ps = st.x[3];
            const double a = load<T>(m, pa), b = load<T>(m, pb);
            const double roe = std::abs(a) > std::abs(b) ? a : b, scale = std::abs(a) + std::abs(b);
            double c = 1, sn = 0, r = 0, z = 0;
            if (scale != 0) {
                r = scale * std::sqrt((a / scale) * (a / scale) + (b / scale) * (b / scale));
                if (roe < 0) r = -r;
                c = a / r;
                sn = b / r;
                z = std::abs(a) > std::abs(b) ? sn : c != 0 ? 1 / c : 1;
            }
            store<T>(m, pa, r);
            store<T>(m, pb, z);
            store<T>(m, pc, c);
            store<T>(m, ps, sn);
        });
        // rotm(N, X, incX, Y, incY, const P[5]): the modified Givens transformation P.
        cblas(s + "rotm", [](CpuState& st, GuestMemory& m) {
            Args a(st, m);
            const auto n = a.integer();
            const Vec<T> x{a.pointer(), n, a.integer()};
            const Vec<T> y{a.pointer(), n, a.integer()};
            const auto param = a.pointer();
            const double flag = load<T>(m, param);
            if (n <= 0 || flag == -2) return;
            double h11 = load<T>(m, param + sizeof(T)), h21 = load<T>(m, param + 2 * sizeof(T));
            double h12 = load<T>(m, param + 3 * sizeof(T)), h22 = load<T>(m, param + 4 * sizeof(T));
            if (flag == 0) { h11 = 1; h22 = 1; }
            else if (flag == 1) { h12 = 1; h21 = -1; }
            auto xs = x.read(m), ys = y.read(m);
            for (std::size_t i = 0; i < xs.size(); ++i) {
                const auto u = xs[i], v = ys[i];
                xs[i] = h11 * u + h12 * v;
                ys[i] = h21 * u + h22 * v;
            }
            x.write(m, xs);
            y.write(m, ys);
        });
    }

    // ---- level 2 ------------------------------------------------------------------------------
    // gemv(Order, TransA, M, N, alpha, A, lda, X, incX, beta, Y, incY)
    cblas(s + "gemv", [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const bool by_rows = a.integer() == row_major;
        const auto trans = a.integer(), rows = a.integer(), cols = a.integer();
        const auto alpha = scalar<T>(a, m);
        const auto pa = a.pointer();
        const auto lda = a.integer();
        const auto x = a.pointer();
        const auto incx = a.integer();
        const auto beta = scalar<T>(a, m);
        const auto y = a.pointer();
        const auto incy = a.integer();
        gemv<T>(m, by_rows, trans, rows, cols, alpha, pa, lda, x, incx, beta, y, incy);
    });
    // ger / geru / gerc(Order, M, N, alpha, X, incX, Y, incY, A, lda): A += alpha x y^T (y^H)
    for (const bool conjugated : complex ? std::vector<bool>{false, true} : std::vector<bool>{false}) {
        cblas(s + (complex ? (conjugated ? "gerc" : "geru") : "ger"), [conjugated](CpuState& st, GuestMemory& m) {
            Args a(st, m);
            const bool by_rows = a.integer() == row_major;
            const auto rows = a.integer(), cols = a.integer();
            const auto alpha = scalar<T>(a, m);
            const Vec<T> x{a.pointer(), rows, a.integer()};
            const Vec<T> y{a.pointer(), cols, a.integer()};
            const Mat<T> ma{a.pointer(), a.integer(), by_rows};
            if (rows <= 0 || cols <= 0) return;
            const auto xs = x.read(m), ys = y.read(m);
            for (std::int64_t i = 0; i < rows; ++i)
                for (std::int64_t j = 0; j < cols; ++j)
                    store<T>(m, ma.at(i, j), load<T>(m, ma.at(i, j)) + alpha * xs[static_cast<std::size_t>(i)] * conj_if(ys[static_cast<std::size_t>(j)], conjugated));
        });
    }
    // symv / hemv(Order, Uplo, N, alpha, A, lda, X, incX, beta, Y, incY)
    cblas(s + (complex ? "hemv" : "symv"), [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const bool by_rows = a.integer() == row_major;
        const auto uplo = a.integer(), n = a.integer();
        const auto alpha = scalar<T>(a, m);
        const auto pa = a.pointer();
        const auto lda = a.integer();
        const Vec<T> x{a.pointer(), n, a.integer()};
        const auto beta = scalar<T>(a, m);
        const Vec<T> y{a.pointer(), n, a.integer()};
        if (n <= 0) return;
        const auto full = mirrored(dense(m, Mat<T>{pa, lda, by_rows}, n, n), uplo, complex);
        const auto xs = x.read(m);
        auto ys = beta == W{} ? std::vector<W>(static_cast<std::size_t>(n)) : y.read(m);
        for (std::int64_t i = 0; i < n; ++i) {
            W sum{};
            for (std::int64_t j = 0; j < n; ++j) sum += full(i, j) * xs[static_cast<std::size_t>(j)];
            ys[static_cast<std::size_t>(i)] = alpha * sum + beta * ys[static_cast<std::size_t>(i)];
        }
        y.write(m, ys);
    });
    // trmv / trsv(Order, Uplo, TransA, Diag, N, A, lda, X, incX)
    for (const bool solving : {false, true}) {
        cblas(s + (solving ? "trsv" : "trmv"), [solving](CpuState& st, GuestMemory& m) {
            Args a(st, m);
            const bool by_rows = a.integer() == row_major;
            const auto uplo = a.integer(), trans = a.integer(), diag = a.integer(), n = a.integer();
            const auto pa = a.pointer();
            const auto lda = a.integer();
            const Vec<T> x{a.pointer(), n, a.integer()};
            if (n <= 0) return;
            const auto t = op(triangle(dense(m, Mat<T>{pa, lda, by_rows}, n, n), uplo, diag), trans);
            Dense<W> column{n, 1, x.read(m)};
            if (solving) solve(t, lower_after(uplo, trans), column);
            else column = product(t, column);
            x.write(m, column.v);
        });
    }
    // syr(Order, Uplo, N, alpha, X, incX, A, lda) / her (real alpha): A += alpha x x^T (x^H)
    cblas(s + (complex ? "her" : "syr"), [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const bool by_rows = a.integer() == row_major;
        const auto uplo = a.integer(), n = a.integer();
        const double alpha = a.template real<Real<T>>();
        const Vec<T> x{a.pointer(), n, a.integer()};
        const Mat<T> ma{a.pointer(), a.integer(), by_rows};
        if (n <= 0) return;
        const auto xs = x.read(m);
        for (std::int64_t i = 0; i < n; ++i)
            for (std::int64_t j = 0; j < n; ++j) {
                if (uplo == upper ? i > j : i < j) continue;
                auto value = load<T>(m, ma.at(i, j)) + alpha * xs[static_cast<std::size_t>(i)] * conj_if(xs[static_cast<std::size_t>(j)], complex);
                if (complex && i == j) value = W{std::real(value)};
                store<T>(m, ma.at(i, j), value);
            }
    });

    // ---- level 3 ------------------------------------------------------------------------------
    // gemm(Order, TransA, TransB, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc)
    cblas(s + "gemm", [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const bool by_rows = a.integer() == row_major;
        const auto ta = a.integer(), tb = a.integer(), rows = a.integer(), cols = a.integer(), k = a.integer();
        const auto alpha = scalar<T>(a, m);
        const auto pa = a.pointer();
        const auto lda = a.integer();
        const auto pb = a.pointer();
        const auto ldb = a.integer();
        const auto beta = scalar<T>(a, m);
        const auto pc = a.pointer();
        const auto ldc = a.integer();
        gemm<T>(m, by_rows, ta, tb, rows, cols, k, alpha, pa, lda, pb, ldb, beta, pc, ldc);
    });
    // symm / hemm(Order, Side, Uplo, M, N, alpha, A, lda, B, ldb, beta, C, ldc)
    const auto symmetric_product = [](bool hermitian) {
        return [hermitian](CpuState& st, GuestMemory& m) {
            Args a(st, m);
            const bool by_rows = a.integer() == row_major;
            const auto side = a.integer(), uplo = a.integer(), rows = a.integer(), cols = a.integer();
            const auto alpha = scalar<T>(a, m);
            const auto pa = a.pointer();
            const auto lda = a.integer();
            const auto pb = a.pointer();
            const auto ldb = a.integer();
            const auto beta = scalar<T>(a, m);
            const auto pc = a.pointer();
            const auto ldc = a.integer();
            if (rows <= 0 || cols <= 0) return;
            const auto order = side == left ? rows : cols;
            const auto full = mirrored(dense(m, Mat<T>{pa, lda, by_rows}, order, order), uplo, hermitian);
            const auto stored_b = dense(m, Mat<T>{pb, ldb, by_rows}, rows, cols);
            update<T>(m, Mat<T>{pc, ldc, by_rows}, side == left ? product(full, stored_b) : product(stored_b, full), alpha, beta);
        };
    };
    cblas(s + "symm", symmetric_product(false));
    if constexpr (complex) cblas(s + "hemm", symmetric_product(true));
    // syrk(Order, Uplo, Trans, N, K, alpha, A, lda, beta, C, ldc); herk with real alpha and beta.
    cblas(s + "syrk", [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const bool by_rows = a.integer() == row_major;
        const auto uplo = a.integer(), trans = a.integer(), n = a.integer(), k = a.integer();
        const auto alpha = scalar<T>(a, m);
        const auto pa = a.pointer();
        const auto lda = a.integer();
        const auto beta = scalar<T>(a, m);
        const auto pc = a.pointer();
        const auto ldc = a.integer();
        rank_k<T>(m, by_rows, uplo, trans, n, k, alpha, pa, lda, beta, pc, ldc, false);
    });
    if constexpr (complex) {
        cblas(s + "herk", [](CpuState& st, GuestMemory& m) {
            Args a(st, m);
            const bool by_rows = a.integer() == row_major;
            const auto uplo = a.integer(), trans = a.integer(), n = a.integer(), k = a.integer();
            const W alpha{a.template real<Real<T>>()};
            const auto pa = a.pointer();
            const auto lda = a.integer();
            const W beta{a.template real<Real<T>>()};
            const auto pc = a.pointer();
            const auto ldc = a.integer();
            rank_k<T>(m, by_rows, uplo, trans, n, k, alpha, pa, lda, beta, pc, ldc, true);
        });
    }
    // syr2k(Order, Uplo, Trans, N, K, alpha, A, lda, B, ldb, beta, C, ldc)
    cblas(s + "syr2k", [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const bool by_rows = a.integer() == row_major;
        const auto uplo = a.integer(), trans = a.integer(), n = a.integer(), k = a.integer();
        const auto alpha = scalar<T>(a, m);
        const auto pa = a.pointer();
        const auto lda = a.integer();
        const auto pb = a.pointer();
        const auto ldb = a.integer();
        const auto beta = scalar<T>(a, m);
        const auto pc = a.pointer();
        const auto ldc = a.integer();
        if (n <= 0) return;
        const auto rows = trans == no_trans ? n : k, cols = trans == no_trans ? k : n;
        const auto fa = op(dense(m, Mat<T>{pa, lda, by_rows}, rows, cols), trans == no_trans ? no_trans : 112);
        const auto fb = op(dense(m, Mat<T>{pb, ldb, by_rows}, rows, cols), trans == no_trans ? no_trans : 112);
        auto p = product(fa, op(fb, 112));
        const auto q = product(fb, op(fa, 112));
        for (std::size_t i = 0; i < p.v.size(); ++i) p.v[i] += q.v[i];
        update<T>(m, Mat<T>{pc, ldc, by_rows}, p, alpha, beta, uplo);
    });
    // trmm / trsm(Order, Side, Uplo, TransA, Diag, M, N, alpha, A, lda, B, ldb)
    for (const bool solving : {false, true}) {
        cblas(s + (solving ? "trsm" : "trmm"), [solving](CpuState& st, GuestMemory& m) {
            Args a(st, m);
            const bool by_rows = a.integer() == row_major;
            const auto side = a.integer(), uplo = a.integer(), trans = a.integer(), diag = a.integer(), rows = a.integer(), cols = a.integer();
            const auto alpha = scalar<T>(a, m);
            const auto pa = a.pointer();
            const auto lda = a.integer();
            const auto pb = a.pointer();
            const auto ldb = a.integer();
            if (solving) trsm<T>(m, by_rows, side, uplo, trans, diag, rows, cols, alpha, pa, lda, pb, ldb);
            else trmm<T>(m, by_rows, side, uplo, trans, diag, rows, cols, alpha, pa, lda, pb, ldb);
        });
    }

    // ---- Fortran subroutines (every argument by reference; column-major) --------------------------
    const auto character = [](GuestMemory& m, Address at) { return static_cast<char>(std::toupper(m.read8(at))); };
    const auto transpose_of = [character](GuestMemory& m, Address at) {
        const auto c = character(m, at);
        return c == 'N' ? no_trans : c == 'T' ? std::int64_t{112} : conj_trans;
    };
    const auto integer_at = [](GuestMemory& m, Address at) { return static_cast<std::int64_t>(static_cast<std::int32_t>(m.read32(at))); };
    // ?gemm_(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc)
    fortran(s + "gemm", [transpose_of, integer_at](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const auto ta = transpose_of(m, a.pointer()), tb = transpose_of(m, a.pointer());
        const auto rows = integer_at(m, a.pointer()), cols = integer_at(m, a.pointer()), k = integer_at(m, a.pointer());
        const auto alpha = load<T>(m, a.pointer());
        const auto pa = a.pointer();
        const auto lda = integer_at(m, a.pointer());
        const auto pb = a.pointer();
        const auto ldb = integer_at(m, a.pointer());
        const auto beta = load<T>(m, a.pointer());
        const auto pc = a.pointer();
        gemm<T>(m, false, ta, tb, rows, cols, k, alpha, pa, lda, pb, ldb, beta, pc, integer_at(m, a.pointer()));
    });
    // ?gemv_(trans, m, n, alpha, a, lda, x, incx, beta, y, incy)
    fortran(s + "gemv", [transpose_of, integer_at](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const auto trans = transpose_of(m, a.pointer());
        const auto rows = integer_at(m, a.pointer()), cols = integer_at(m, a.pointer());
        const auto alpha = load<T>(m, a.pointer());
        const auto pa = a.pointer();
        const auto lda = integer_at(m, a.pointer());
        const auto x = a.pointer();
        const auto incx = integer_at(m, a.pointer());
        const auto beta = load<T>(m, a.pointer());
        const auto y = a.pointer();
        gemv<T>(m, false, trans, rows, cols, alpha, pa, lda, x, incx, beta, y, integer_at(m, a.pointer()));
    });
    // ?axpy_(n, alpha, x, incx, y, incy), ?scal_(n, alpha, x, incx), ?copy_ and ?swap_(n, x, incx, y, incy):
    // the CBLAS forms with the integers and scalars read through their pointers.
    const auto by_reference = [integer_at](transmute::GuestCallHandler cblas_form, std::vector<char> kinds) {
        return [cblas_form = std::move(cblas_form), kinds = std::move(kinds), integer_at](CpuState& st, GuestMemory& m) {
            Args a(st, m);
            CpuState copy = st;
            unsigned gp = 0, fp = 0;
            for (const auto kind : kinds) {
                const auto at = a.pointer();
                if (kind == 'i') copy.x[gp++] = static_cast<std::uint64_t>(integer_at(m, at));
                else if (kind == 'p') copy.x[gp++] = at;
                else if constexpr (complex) copy.x[gp++] = at;   // a complex scalar stays by reference
                else if constexpr (std::is_same_v<T, float>) copy.v[fp++] = {m.read32(at), 0};
                else copy.v[fp++] = {m.read64(at), 0};
            }
            cblas_form(copy, m);
            st.x[0] = copy.x[0];
            st.v[0] = copy.v[0];
        };
    };
    fortran(s + "axpy", by_reference(axpy, {'i', 's', 'p', 'i', 'p', 'i'}));
    fortran(s + "scal", by_reference(scal, {'i', 's', 'p', 'i'}));
    fortran(s + "copy", by_reference(copy, {'i', 'p', 'i', 'p', 'i'}));
    fortran(s + "swap", by_reference(swap, {'i', 'p', 'i', 'p', 'i'}));
    // The `$NEWLAPACK` functions: ?dot (real types), ?nrm2, ?asum, and i?amax with a 1-based index.
    if constexpr (!complex) entry(s + "dot$NEWLAPACK", by_reference(dot_, {'i', 'p', 'i', 'p', 'i'}));
    entry(real_prefix + "nrm2$NEWLAPACK", by_reference(nrm2, {'i', 'p', 'i'}));
    entry(real_prefix + "asum$NEWLAPACK", by_reference(asum, {'i', 'p', 'i'}));
    entry("i" + s + "amax$NEWLAPACK", [index = by_reference(iamax, {'i', 'p', 'i'})](CpuState& st, GuestMemory& m) {
        const auto n = static_cast<std::int32_t>(m.read32(st.x[0]));
        index(st, m);
        st.x[0] = n > 0 ? st.x[0] + 1 : 0;
    });
    // ?trsm_(side, uplo, transa, diag, m, n, alpha, a, lda, b, ldb)
    fortran(s + "trsm", [character, transpose_of, integer_at](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const auto side = character(m, a.pointer()) == 'L' ? left : std::int64_t{142};
        const auto uplo = character(m, a.pointer()) == 'U' ? upper : std::int64_t{122};
        const auto trans = transpose_of(m, a.pointer());
        const auto diag = character(m, a.pointer()) == 'U' ? unit : std::int64_t{131};
        const auto rows = integer_at(m, a.pointer()), cols = integer_at(m, a.pointer());
        const auto alpha = load<T>(m, a.pointer());
        const auto pa = a.pointer();
        const auto lda = integer_at(m, a.pointer());
        const auto pb = a.pointer();
        trsm<T>(m, false, side, uplo, trans, diag, rows, cols, alpha, pa, lda, pb, integer_at(m, a.pointer()));
    });
    // ?syrk_(uplo, trans, n, k, alpha, a, lda, beta, c, ldc)
    fortran(s + "syrk", [character, transpose_of, integer_at](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const auto uplo = character(m, a.pointer()) == 'U' ? upper : std::int64_t{122};
        const auto trans = transpose_of(m, a.pointer());
        const auto n = integer_at(m, a.pointer()), k = integer_at(m, a.pointer());
        const auto alpha = load<T>(m, a.pointer());
        const auto pa = a.pointer();
        const auto lda = integer_at(m, a.pointer());
        const auto beta = load<T>(m, a.pointer());
        const auto pc = a.pointer();
        rank_k<T>(m, false, uplo, trans, n, k, alpha, pa, lda, beta, pc, integer_at(m, a.pointer()), false);
    });
    if constexpr (!complex) {
        // ?ger_(m, n, alpha, x, incx, y, incy, a, lda)
        fortran(s + "ger", [integer_at](CpuState& st, GuestMemory& m) {
            Args a(st, m);
            const auto rows = integer_at(m, a.pointer()), cols = integer_at(m, a.pointer());
            const auto alpha = load<T>(m, a.pointer());
            const Vec<T> x{a.pointer(), rows, integer_at(m, a.pointer())};
            const Vec<T> y{a.pointer(), cols, integer_at(m, a.pointer())};
            const Mat<T> ma{a.pointer(), integer_at(m, a.pointer()), false};
            if (rows <= 0 || cols <= 0) return;
            const auto xs = x.read(m), ys = y.read(m);
            for (std::int64_t i = 0; i < rows; ++i)
                for (std::int64_t j = 0; j < cols; ++j)
                    store<T>(m, ma.at(i, j), load<T>(m, ma.at(i, j)) + alpha * xs[static_cast<std::size_t>(i)] * ys[static_cast<std::size_t>(j)]);
        });
    }
}

void Blas::install_mixed() {
    // sdsdot(N, alpha, X, incX, Y, incY): alpha + x . y accumulated in double, a float result.
    cblas("sdsdot", [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const auto n = a.integer();
        const double alpha = a.real<float>();
        const Vec<float> x{a.pointer(), n, a.integer()};
        const Vec<float> y{a.pointer(), n, a.integer()};
        double sum = alpha;
        if (n > 0) {
            const auto xs = x.read(m), ys = y.read(m);
            for (std::size_t i = 0; i < xs.size(); ++i) sum += xs[i] * ys[i];
        }
        return_real<float>(st, sum);
    });
    // dsdot(N, X, incX, Y, incY): float vectors, a double result.
    cblas("dsdot", [](CpuState& st, GuestMemory& m) {
        Args a(st, m);
        const auto n = a.integer();
        const Vec<float> x{a.pointer(), n, a.integer()};
        const Vec<float> y{a.pointer(), n, a.integer()};
        double sum = 0;
        if (n > 0) {
            const auto xs = x.read(m), ys = y.read(m);
            for (std::size_t i = 0; i < xs.size(); ++i) sum += xs[i] * ys[i];
        }
        return_real<double>(st, sum);
    });
}

// The legacy LAPACK names: the same subroutines as their `$NEWLAPACK` names.
void Blas::install_lapack_names() {
    for (const auto* stem : {"gesv", "getrf", "potrf", "gelsd", "gesdd", "syevd", "heevd", "geev", "geqrf", "orgqr", "ungqr"})
        for (const char p : {'s', 'd', 'c', 'z'}) {
            const auto name = std::string(1, p) + stem;
            const auto target = process.resolve(accelerate, "_" + name + "$NEWLAPACK");
            const auto alias = "_" + name + "_";
            if (!target || has(accelerate, alias)) continue;
            registration_audit::record_fill(accelerate, alias, false, std::source_location::current());
            process.install_export(std::string(accelerate), alias, {target->address, ExportKind::function});
        }
}

} // namespace

void install_blas(InstallContext& context) { std::make_shared<Blas>(context)->install(); }

} // namespace alchemy::gui::fill
