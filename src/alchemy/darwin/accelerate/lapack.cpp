#include "alchemy/darwin/accelerate/lapack.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <functional>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// LAPACK decomposition drivers of Accelerate's LP64 interface (`$NEWLAPACK` names, column-major,
// every argument by reference, INFO as LAPACK documents), computed in double precision on the
// host for all four types:
// - ?gesdd: the singular value decomposition, by one-sided Jacobi rotations (JOBZ A, S, O, N);
// - ?syevd / ?heevd: symmetric / Hermitian eigenvalues (ascending) and eigenvectors, by cyclic
//   Jacobi rotations;
// - ?geev: general eigenvalues and left/right eigenvectors, by Householder reduction to
//   Hessenberg form and shifted QR iteration to complex Schur form; real matrices return their
//   complex pairs as LAPACK does (consecutive, positive imaginary part first, the vector's real
//   and imaginary parts in two columns); vectors have unit norm and their largest component real;
// - ?geqrf / ?orgqr / ?ungqr: Householder QR with LAPACK's reflector representation (R above the
//   diagonal, the vectors below with an implicit unit, TAU) and the explicit Q.
// Workspace queries (LWORK, LRWORK or LIWORK of -1) answer sizes in WORK(1), RWORK(1), IWORK(1).
// The internal work arrays are not used. Matrices are limited to 4096 rows and columns.
namespace alchemy::accelerate {
namespace {

using transmute::CpuState;
using transmute::GuestMemory;
using C = std::complex<double>;
constexpr double eps = std::numeric_limits<double>::epsilon();
constexpr std::int32_t dimension_limit = 4096;

template <class T> constexpr bool complex_v = false;
template <class T> constexpr bool complex_v<std::complex<T>> = true;
template <class T> struct Real { using type = T; };
template <class T> struct Real<std::complex<T>> { using type = T; };

// The i-th argument: x0-x7, then 8-byte stack slots (every LAPACK argument is a pointer).
std::uint64_t arg(const CpuState& s, const GuestMemory& m, unsigned i) { return i < 8 ? s.x[i] : m.read64(s.sp + 8ULL * (i - 8)); }
std::int32_t integer(const GuestMemory& m, std::uint64_t at) { return at ? static_cast<std::int32_t>(m.read32(at)) : 0; }
char letter(const GuestMemory& m, std::uint64_t at) {
    if (!at) return 'N';
    std::uint8_t c{};
    m.read_bytes(at, std::span(&c, 1));
    return static_cast<char>(std::toupper(c));
}
void set_info(GuestMemory& m, std::uint64_t at, std::int32_t value) { if (at) m.write32(at, static_cast<std::uint32_t>(value)); }

template <class T> T stored(C value) {
    if constexpr (complex_v<T>) return T(static_cast<typename Real<T>::type>(value.real()), static_cast<typename Real<T>::type>(value.imag()));
    else return static_cast<T>(value.real());
}
template <class T> C loaded(T value) {
    if constexpr (complex_v<T>) return C(value.real(), value.imag());
    else return C(value, 0);
}

struct Matrix {
    int rows{}, cols{};
    std::vector<C> a;
    Matrix() = default;
    Matrix(int r, int c) : rows(r), cols(c), a(static_cast<std::size_t>(r) * c) {}
    C& operator()(int r, int c) { return a[static_cast<std::size_t>(c) * rows + r]; }
    const C& operator()(int r, int c) const { return a[static_cast<std::size_t>(c) * rows + r]; }
    static Matrix identity(int r, int c) {
        Matrix out(r, c);
        for (int i = 0; i < std::min(r, c); ++i) out(i, i) = 1;
        return out;
    }
};

template <class T> Matrix read_matrix(const GuestMemory& m, std::uint64_t at, int rows, int cols, int ld) {
    Matrix out(rows, cols);
    std::vector<T> column(static_cast<std::size_t>(rows));
    for (int c = 0; c < cols; ++c) {
        m.read_bytes(at + static_cast<std::uint64_t>(c) * ld * sizeof(T), std::span(reinterpret_cast<std::uint8_t*>(column.data()), column.size() * sizeof(T)));
        for (int r = 0; r < rows; ++r) out(r, c) = loaded<T>(column[static_cast<std::size_t>(r)]);
    }
    return out;
}
template <class T> void write_matrix(GuestMemory& m, std::uint64_t at, const Matrix& value, int ld, int rows = -1, int cols = -1) {
    rows = rows < 0 ? value.rows : rows;
    cols = cols < 0 ? value.cols : cols;
    std::vector<T> column(static_cast<std::size_t>(rows));
    for (int c = 0; c < cols; ++c) {
        for (int r = 0; r < rows; ++r) column[static_cast<std::size_t>(r)] = stored<T>(value(r, c));
        m.write_bytes(at + static_cast<std::uint64_t>(c) * ld * sizeof(T), std::span(reinterpret_cast<const std::uint8_t*>(column.data()), column.size() * sizeof(T)));
    }
}
template <class T> void write_scalar(GuestMemory& m, std::uint64_t at, double value) {
    if (!at) return;
    const T v = stored<T>(C(value, 0));
    m.write_bytes(at, std::span(reinterpret_cast<const std::uint8_t*>(&v), sizeof(T)));
}
template <class R> void write_reals(GuestMemory& m, std::uint64_t at, const std::vector<double>& values) {
    std::vector<R> out;
    out.reserve(values.size());
    for (const auto value : values) out.push_back(static_cast<R>(value));
    if (!out.empty()) m.write_bytes(at, std::span(reinterpret_cast<const std::uint8_t*>(out.data()), out.size() * sizeof(R)));
}

double norm2(const Matrix& a, int col, int from = 0) {
    double sum = 0;
    for (int r = from; r < a.rows; ++r) sum += std::norm(a(r, col));
    return std::sqrt(sum);
}

// Fills the columns from `have` on with an orthonormal complement of the earlier ones.
void complete_basis(Matrix& q, int have) {
    int next_unit = 0;
    for (int c = have; c < q.cols; ++c) {
        for (;;) {
            if (next_unit >= q.rows) throw std::runtime_error("Alchemy LAPACK: no orthonormal complement");
            std::vector<C> v(static_cast<std::size_t>(q.rows));
            v[static_cast<std::size_t>(next_unit++)] = 1;
            for (int pass = 0; pass < 2; ++pass)
                for (int k = 0; k < c; ++k) {
                    C dot = 0;
                    for (int r = 0; r < q.rows; ++r) dot += std::conj(q(r, k)) * v[static_cast<std::size_t>(r)];
                    for (int r = 0; r < q.rows; ++r) v[static_cast<std::size_t>(r)] -= dot * q(r, k);
                }
            double n = 0;
            for (const auto& x : v) n += std::norm(x);
            n = std::sqrt(n);
            if (n < 1e-8) continue;
            for (int r = 0; r < q.rows; ++r) q(r, c) = v[static_cast<std::size_t>(r)] / n;
            break;
        }
    }
}

// ---- SVD (one-sided Jacobi) ----
struct Svd { Matrix u, v; std::vector<double> s; bool converged{true}; };

// A (m x n, m >= n) = U (m x n) diag(s) V^H (V n x n), s descending.
Svd jacobi_svd(Matrix a) {
    const int n = a.cols;
    Svd out;
    out.v = Matrix::identity(n, n);
    bool rotated = true;
    int sweep = 0;
    for (; rotated && sweep < 80; ++sweep) {
        rotated = false;
        for (int p = 0; p < n - 1; ++p)
            for (int q = p + 1; q < n; ++q) {
                double alpha = 0, beta = 0;
                C gamma = 0;
                for (int r = 0; r < a.rows; ++r) {
                    alpha += std::norm(a(r, p));
                    beta += std::norm(a(r, q));
                    gamma += std::conj(a(r, p)) * a(r, q);
                }
                const double g = std::abs(gamma);
                if (g == 0 || g <= eps * std::sqrt(alpha * beta)) continue;
                rotated = true;
                const C phase = std::conj(gamma / g);   // makes <a_p, phase a_q> real and positive
                const double zeta = (beta - alpha) / (2 * g);
                const double t = (zeta >= 0 ? 1.0 : -1.0) / (std::abs(zeta) + std::sqrt(1 + zeta * zeta));
                const double c = 1 / std::sqrt(1 + t * t), s = c * t;
                for (int r = 0; r < a.rows; ++r) {
                    const C ap = a(r, p), aq = a(r, q) * phase;
                    a(r, p) = c * ap - s * aq;
                    a(r, q) = s * ap + c * aq;
                }
                for (int r = 0; r < n; ++r) {
                    const C vp = out.v(r, p), vq = out.v(r, q) * phase;
                    out.v(r, p) = c * vp - s * vq;
                    out.v(r, q) = s * vp + c * vq;
                }
            }
    }
    out.converged = !rotated;
    std::vector<int> order(static_cast<std::size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    std::vector<double> sigma(static_cast<std::size_t>(n));
    for (int j = 0; j < n; ++j) sigma[static_cast<std::size_t>(j)] = norm2(a, j);
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return sigma[static_cast<std::size_t>(x)] > sigma[static_cast<std::size_t>(y)]; });
    out.u = Matrix(a.rows, n);
    Matrix v(n, n);
    int nonzero = 0;
    const double scale = sigma.empty() ? 0 : *std::max_element(sigma.begin(), sigma.end());
    for (int j = 0; j < n; ++j) {
        const int from = order[static_cast<std::size_t>(j)];
        const double s = sigma[static_cast<std::size_t>(from)];
        out.s.push_back(s);
        for (int r = 0; r < n; ++r) v(r, j) = out.v(r, from);
        if (s > scale * eps * a.rows && s > 0) {
            for (int r = 0; r < a.rows; ++r) out.u(r, j) = a(r, from) / s;
            nonzero = j + 1;
        }
    }
    out.v = std::move(v);
    complete_basis(out.u, nonzero);   // columns of zero singular values
    return out;
}

Matrix adjoint(const Matrix& a) {
    Matrix out(a.cols, a.rows);
    for (int r = 0; r < a.rows; ++r)
        for (int c = 0; c < a.cols; ++c) out(c, r) = std::conj(a(r, c));
    return out;
}

// ---- Hermitian eigen (cyclic Jacobi) ----
bool jacobi_eigen(Matrix& a, Matrix& v, std::vector<double>& w) {
    const int n = a.rows;
    v = Matrix::identity(n, n);
    bool converged = false;
    for (int sweep = 0; sweep < 80 && !converged; ++sweep) {
        double off = 0, total = 0;
        for (int c = 0; c < n; ++c)
            for (int r = 0; r < n; ++r) {
                total += std::norm(a(r, c));
                if (r != c) off += std::norm(a(r, c));
            }
        if (off <= eps * eps * total || off == 0) { converged = true; break; }
        for (int p = 0; p < n - 1; ++p)
            for (int q = p + 1; q < n; ++q) {
                const double g = std::abs(a(p, q));
                if (g == 0) continue;
                const C phase = a(p, q) / g;
                // Make a(p, q) real: column q times conj(phase), row q times phase.
                for (int k = 0; k < n; ++k) a(k, q) *= std::conj(phase);
                for (int k = 0; k < n; ++k) a(q, k) *= phase;
                for (int k = 0; k < n; ++k) v(k, q) *= std::conj(phase);
                const double app = a(p, p).real(), aqq = a(q, q).real(), apq = a(p, q).real();
                const double zeta = (aqq - app) / (2 * apq);
                const double t = (zeta >= 0 ? 1.0 : -1.0) / (std::abs(zeta) + std::sqrt(1 + zeta * zeta));
                const double c = 1 / std::sqrt(1 + t * t), s = c * t;
                for (int k = 0; k < n; ++k) {
                    const C kp = a(k, p), kq = a(k, q);
                    a(k, p) = c * kp - s * kq;
                    a(k, q) = s * kp + c * kq;
                }
                for (int k = 0; k < n; ++k) {
                    const C pk = a(p, k), qk = a(q, k);
                    a(p, k) = c * pk - s * qk;
                    a(q, k) = s * pk + c * qk;
                }
                for (int k = 0; k < n; ++k) {
                    const C vp = v(k, p), vq = v(k, q);
                    v(k, p) = c * vp - s * vq;
                    v(k, q) = s * vp + c * vq;
                }
                a(p, q) = a(q, p) = 0;
            }
    }
    std::vector<int> order(static_cast<std::size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return a(x, x).real() < a(y, y).real(); });
    Matrix sorted(n, n);
    w.clear();
    for (int j = 0; j < n; ++j) {
        const int from = order[static_cast<std::size_t>(j)];
        w.push_back(a(from, from).real());
        for (int r = 0; r < n; ++r) sorted(r, j) = v(r, from);
    }
    v = std::move(sorted);
    return converged;
}

// ---- Householder QR (LAPACK's representation) ----
template <class T>
void householder_qr(Matrix& a, std::vector<C>& tau) {
    const int m = a.rows, n = a.cols, k = std::min(m, n);
    tau.assign(static_cast<std::size_t>(k), 0);
    for (int i = 0; i < k; ++i) {
        const C alpha = a(i, i);
        const double xnorm = norm2(a, i, i + 1);
        if (xnorm == 0 && alpha.imag() == 0) continue;   // H = I
        const double beta = -std::copysign(std::sqrt(std::norm(alpha) + xnorm * xnorm), alpha.real());
        const C t = (beta - alpha) / beta;
        const C scale = 1.0 / (alpha - beta);
        for (int r = i + 1; r < m; ++r) a(r, i) *= scale;
        a(i, i) = beta;
        tau[static_cast<std::size_t>(i)] = t;
        // A(i:m, i+1:n) = H^H A with H = I - tau v v^H, v = (1, a(i+1:m, i)).
        for (int c = i + 1; c < n; ++c) {
            C dot = a(i, c);
            for (int r = i + 1; r < m; ++r) dot += std::conj(a(r, i)) * a(r, c);
            const C f = std::conj(t) * dot;
            a(i, c) -= f;
            for (int r = i + 1; r < m; ++r) a(r, c) -= f * a(r, i);
        }
    }
}

// Q (m x n) = H(1) ... H(k) from the reflectors in the first k columns of `a` and `tau`.
Matrix explicit_q(const Matrix& a, const std::vector<C>& tau, int n, int k) {
    const int m = a.rows;
    Matrix q = Matrix::identity(m, n);
    for (int i = k - 1; i >= 0; --i) {
        const C t = tau[static_cast<std::size_t>(i)];
        if (t == C(0)) continue;
        for (int c = i; c < n; ++c) {
            C dot = q(i, c);
            for (int r = i + 1; r < m; ++r) dot += std::conj(a(r, i)) * q(r, c);
            const C f = t * dot;
            q(i, c) -= f;
            for (int r = i + 1; r < m; ++r) q(r, c) -= f * a(r, i);
        }
    }
    return q;
}

// ---- general eigen (Hessenberg + complex shifted QR) ----
struct Eigen { std::vector<C> values; Matrix right, left; bool converged{true}; };

void normalize_vector(Matrix& x, int col) {
    const double n = norm2(x, col);
    if (n == 0) return;
    int big = 0;
    for (int r = 0; r < x.rows; ++r)
        if (std::abs(x(r, col)) > std::abs(x(big, col))) big = r;
    const C phase = std::abs(x(big, col)) > 0 ? std::conj(x(big, col)) / std::abs(x(big, col)) : C(1);
    for (int r = 0; r < x.rows; ++r) x(r, col) = x(r, col) * phase / n;
    x(big, col) = C(x(big, col).real(), 0);
}

Eigen general_eigen(Matrix h, bool want_right, bool want_left) {
    const int n = h.rows;
    Matrix z = Matrix::identity(n, n);
    // Hessenberg reduction: Householder reflectors on columns 0..n-3.
    for (int k = 0; k + 2 < n; ++k) {
        const C alpha = h(k + 1, k);
        double xnorm = 0;
        for (int r = k + 2; r < n; ++r) xnorm += std::norm(h(r, k));
        xnorm = std::sqrt(xnorm);
        if (xnorm == 0) continue;
        const double beta = -std::copysign(std::sqrt(std::norm(alpha) + xnorm * xnorm), alpha.real() == 0 ? 1.0 : alpha.real());
        std::vector<C> v(static_cast<std::size_t>(n));
        v[static_cast<std::size_t>(k + 1)] = alpha - beta;
        for (int r = k + 2; r < n; ++r) v[static_cast<std::size_t>(r)] = h(r, k);
        double vn = 0;
        for (const auto& x : v) vn += std::norm(x);
        if (vn == 0) continue;
        // P = I - 2 v v^H / (v^H v): H = P H P, Z = Z P.
        for (int c = 0; c < n; ++c) {
            C dot = 0;
            for (int r = k + 1; r < n; ++r) dot += std::conj(v[static_cast<std::size_t>(r)]) * h(r, c);
            const C f = 2.0 * dot / vn;
            for (int r = k + 1; r < n; ++r) h(r, c) -= f * v[static_cast<std::size_t>(r)];
        }
        for (int r = 0; r < n; ++r) {
            C dot = 0;
            for (int c = k + 1; c < n; ++c) dot += h(r, c) * v[static_cast<std::size_t>(c)];
            const C f = 2.0 * dot / vn;
            for (int c = k + 1; c < n; ++c) h(r, c) -= f * std::conj(v[static_cast<std::size_t>(c)]);
        }
        for (int r = 0; r < n; ++r) {
            C dot = 0;
            for (int c = k + 1; c < n; ++c) dot += z(r, c) * v[static_cast<std::size_t>(c)];
            const C f = 2.0 * dot / vn;
            for (int c = k + 1; c < n; ++c) z(r, c) -= f * std::conj(v[static_cast<std::size_t>(c)]);
        }
        for (int r = k + 2; r < n; ++r) h(r, k) = 0;
    }
    Eigen out;
    // Shifted QR on the active window [lo, hi] with Givens rotations.
    double scale = 0;
    for (const auto& x : h.a) scale = std::max(scale, std::abs(x));
    int hi = n - 1, iterations = 0;
    while (hi > 0) {
        int lo = hi;
        while (lo > 0 && std::abs(h(lo, lo - 1)) > eps * (std::abs(h(lo, lo)) + std::abs(h(lo - 1, lo - 1)) + (scale == 0 ? 1 : 0)))
            --lo;
        if (lo > 0) h(lo, lo - 1) = 0;
        if (lo == hi) { --hi; iterations = 0; continue; }
        if (++iterations > 60 * n) { out.converged = false; break; }
        // Wilkinson shift from the trailing 2x2 block (exceptional shift every 11 steps).
        C mu;
        if (iterations % 11 == 0) {
            mu = h(hi, hi) + C(std::abs(h(hi, hi - 1)) * 0.75, 0);
        } else {
            const C a = h(hi - 1, hi - 1), b = h(hi - 1, hi), c = h(hi, hi - 1), d = h(hi, hi);
            const C tr = a + d, det = a * d - b * c;
            const C disc = std::sqrt(tr * tr / 4.0 - det);
            const C l1 = tr / 2.0 + disc, l2 = tr / 2.0 - disc;
            mu = std::abs(l1 - d) < std::abs(l2 - d) ? l1 : l2;
        }
        for (int i = lo; i <= hi; ++i) h(i, i) -= mu;
        std::vector<std::pair<C, C>> rotations;   // (c, s) per step: [c s; -conj(s) c] with c real
        for (int k = lo; k < hi; ++k) {
            const C x = h(k, k), y = h(k + 1, k);
            const double r = std::sqrt(std::norm(x) + std::norm(y));
            C c = 1, s = 0;
            if (r > 0) { c = x / r; s = y / r; }
            // G^H applied to rows k, k+1: [conj(c) conj(s); -s c].
            for (int j = k; j < n; ++j) {
                const C a1 = h(k, j), a2 = h(k + 1, j);
                h(k, j) = std::conj(c) * a1 + std::conj(s) * a2;
                h(k + 1, j) = -s * a1 + c * a2;
            }
            rotations.emplace_back(c, s);
        }
        for (int k = lo; k < hi; ++k) {
            const auto [c, s] = rotations[static_cast<std::size_t>(k - lo)];
            // Columns k, k+1 times G = [c -conj(s); s conj(c)].
            for (int i = 0; i <= std::min(hi, k + 2); ++i) {
                const C a1 = h(i, k), a2 = h(i, k + 1);
                h(i, k) = a1 * c + a2 * s;
                h(i, k + 1) = -a1 * std::conj(s) + a2 * std::conj(c);
            }
            for (int i = 0; i < n; ++i) {
                const C a1 = z(i, k), a2 = z(i, k + 1);
                z(i, k) = a1 * c + a2 * s;
                z(i, k + 1) = -a1 * std::conj(s) + a2 * std::conj(c);
            }
        }
        for (int i = lo; i <= hi; ++i) h(i, i) += mu;
    }
    for (int i = 0; i < n; ++i) out.values.push_back(h(i, i));
    const double small = std::max(scale, 1.0) * eps;
    if (want_right) {
        Matrix y(n, n);
        for (int i = 0; i < n; ++i) {
            const C lambda = h(i, i);
            y(i, i) = 1;
            for (int j = i - 1; j >= 0; --j) {
                C sum = 0;
                for (int k = j + 1; k <= i; ++k) sum += h(j, k) * y(k, i);
                C d = h(j, j) - lambda;
                if (std::abs(d) < small) d = small;
                y(j, i) = -sum / d;
            }
        }
        out.right = Matrix(n, n);
        for (int c = 0; c < n; ++c) {
            for (int r = 0; r < n; ++r) {
                C sum = 0;
                for (int k = 0; k <= c; ++k) sum += z(r, k) * y(k, c);
                out.right(r, c) = sum;
            }
            normalize_vector(out.right, c);
        }
    }
    if (want_left) {
        Matrix w(n, n);
        for (int i = 0; i < n; ++i) {
            const C lambda = h(i, i);
            w(i, i) = 1;
            for (int j = i + 1; j < n; ++j) {
                C sum = 0;
                for (int k = i; k < j; ++k) sum += std::conj(h(k, j)) * w(k, i);
                C d = std::conj(h(j, j)) - std::conj(lambda);
                if (std::abs(d) < small) d = small;
                w(j, i) = -sum / d;
            }
        }
        out.left = Matrix(n, n);
        for (int c = 0; c < n; ++c) {
            for (int r = 0; r < n; ++r) {
                C sum = 0;
                for (int k = c; k < n; ++k) sum += z(r, k) * w(k, c);
                out.left(r, c) = sum;
            }
            normalize_vector(out.left, c);
        }
    }
    return out;
}

bool bad_dimension(std::int32_t value) { return value < 0 || value > dimension_limit; }

// ---- drivers ----
template <class T>
void gesdd(CpuState& s, GuestMemory& m) {
    using R = typename Real<T>::type;
    constexpr bool cx = complex_v<T>;
    const auto info = arg(s, m, cx ? 14 : 13);
    const char jobz = letter(m, arg(s, m, 0));
    const auto rows = integer(m, arg(s, m, 1)), cols = integer(m, arg(s, m, 2));
    const auto lda = integer(m, arg(s, m, 4)), ldu = integer(m, arg(s, m, 7)), ldvt = integer(m, arg(s, m, 9));
    const auto lwork = integer(m, arg(s, m, 11));
    const auto a_at = arg(s, m, 3), s_at = arg(s, m, 5), u_at = arg(s, m, 6), vt_at = arg(s, m, 8), work = arg(s, m, 10);
    const int k = std::min(rows, cols);
    if (jobz != 'A' && jobz != 'S' && jobz != 'O' && jobz != 'N') return set_info(m, info, -1);
    if (bad_dimension(rows)) return set_info(m, info, -2);
    if (bad_dimension(cols)) return set_info(m, info, -3);
    if (lda < std::max(1, rows)) return set_info(m, info, -5);
    if (lwork == -1) {
        write_scalar<T>(m, work, std::max(1.0, 3.0 * k * k + 4.0 * std::max(rows, cols)));
        return set_info(m, info, 0);
    }
    if (k == 0) return set_info(m, info, 0);
    auto a = read_matrix<T>(m, a_at, rows, cols, lda);
    const bool wide = rows < cols;
    Svd svd = jacobi_svd(wide ? adjoint(a) : a);
    // A = U S V^H; for a wide matrix the decomposition of A^H swaps U and V.
    Matrix u = wide ? svd.v : svd.u, v = wide ? svd.u : svd.v;
    write_reals<R>(m, s_at, svd.s);
    if (jobz != 'N') {
        const int ucols = jobz == 'A' || (jobz == 'O' && rows >= cols) ? rows : k;
        const int vrows = jobz == 'A' || (jobz == 'O' && rows < cols) ? cols : k;
        if (u.cols < ucols) {
            Matrix full(rows, ucols);
            for (int c = 0; c < u.cols; ++c)
                for (int r = 0; r < rows; ++r) full(r, c) = u(r, c);
            complete_basis(full, u.cols);
            u = std::move(full);
        }
        if (v.cols < vrows) {
            Matrix full(cols, vrows);
            for (int c = 0; c < v.cols; ++c)
                for (int r = 0; r < cols; ++r) full(r, c) = v(r, c);
            complete_basis(full, v.cols);
            v = std::move(full);
        }
        const auto vt = adjoint(v);   // vrows x cols
        if (jobz == 'O') {
            if (rows >= cols) {
                write_matrix<T>(m, a_at, u, lda, rows, k);
                write_matrix<T>(m, vt_at, vt, ldvt, cols, cols);
            } else {
                write_matrix<T>(m, u_at, u, ldu, rows, rows);
                write_matrix<T>(m, a_at, vt, lda, k, cols);
            }
        } else {
            write_matrix<T>(m, u_at, u, ldu, rows, ucols);
            write_matrix<T>(m, vt_at, vt, ldvt, vrows, cols);
        }
    }
    set_info(m, info, svd.converged ? 0 : 1);
}

template <class T>
void eigen_symmetric(CpuState& s, GuestMemory& m) {
    using R = typename Real<T>::type;
    constexpr bool cx = complex_v<T>;
    // syevd: (jobz, uplo, n, a, lda, w, work, lwork, iwork, liwork, info)
    // heevd: (jobz, uplo, n, a, lda, w, work, lwork, rwork, lrwork, iwork, liwork, info)
    const auto info = arg(s, m, cx ? 12 : 10);
    const char jobz = letter(m, arg(s, m, 0)), uplo = letter(m, arg(s, m, 1));
    const auto n = integer(m, arg(s, m, 2)), lda = integer(m, arg(s, m, 4));
    const auto a_at = arg(s, m, 3), w_at = arg(s, m, 5), work = arg(s, m, 6);
    const auto lwork = integer(m, arg(s, m, 7));
    const auto rwork = cx ? arg(s, m, 8) : 0, iwork = arg(s, m, cx ? 10 : 8);
    const auto lrwork = cx ? integer(m, arg(s, m, 9)) : 0, liwork = integer(m, arg(s, m, cx ? 11 : 9));
    if (jobz != 'N' && jobz != 'V') return set_info(m, info, -1);
    if (uplo != 'U' && uplo != 'L') return set_info(m, info, -2);
    if (bad_dimension(n)) return set_info(m, info, -3);
    if (lda < std::max(1, n)) return set_info(m, info, -5);
    bool query = lwork == -1 || liwork == -1;
    if constexpr (cx) query = query || lrwork == -1;
    if (query) {
        write_scalar<T>(m, work, cx ? 2.0 * n + n * n : 1.0 + 6.0 * n + 2.0 * n * n);
        if constexpr (cx) {
            if (rwork) write_reals<R>(m, rwork, {1.0 + 5.0 * n + 2.0 * n * n});
        }
        if (iwork) m.write32(iwork, static_cast<std::uint32_t>(3 + 5 * n));
        return set_info(m, info, 0);
    }
    if (n == 0) return set_info(m, info, 0);
    auto a = read_matrix<T>(m, a_at, n, n, lda);
    // The other triangle mirrors the referenced one.
    for (int c = 0; c < n; ++c)
        for (int r = 0; r < n; ++r) {
            const bool referenced = uplo == 'U' ? r <= c : r >= c;
            if (!referenced) a(r, c) = std::conj(a(c, r));
        }
    for (int i = 0; i < n; ++i) a(i, i) = C(a(i, i).real(), 0);
    Matrix v;
    std::vector<double> w;
    const bool converged = jacobi_eigen(a, v, w);
    write_reals<R>(m, w_at, w);
    if (jobz == 'V') write_matrix<T>(m, a_at, v, lda);
    set_info(m, info, converged ? 0 : 1);
}

template <class T>
void geev(CpuState& s, GuestMemory& m) {
    using R = typename Real<T>::type;
    constexpr bool cx = complex_v<T>;
    // real:    (jobvl, jobvr, n, a, lda, wr, wi, vl, ldvl, vr, ldvr, work, lwork, info)
    // complex: (jobvl, jobvr, n, a, lda, w, vl, ldvl, vr, ldvr, work, lwork, rwork, info)
    const auto info = arg(s, m, 13);
    const char jobvl = letter(m, arg(s, m, 0)), jobvr = letter(m, arg(s, m, 1));
    const auto n = integer(m, arg(s, m, 2)), lda = integer(m, arg(s, m, 4));
    const auto a_at = arg(s, m, 3);
    const unsigned o = cx ? 0 : 1;   // the real form has wr and wi
    const auto vl_at = arg(s, m, 6 + o), vr_at = arg(s, m, 8 + o), work = arg(s, m, 10 + o);
    const auto ldvl = integer(m, arg(s, m, 7 + o)), ldvr = integer(m, arg(s, m, 9 + o)), lwork = integer(m, arg(s, m, 11 + o));
    if (jobvl != 'N' && jobvl != 'V') return set_info(m, info, -1);
    if (jobvr != 'N' && jobvr != 'V') return set_info(m, info, -2);
    if (bad_dimension(n)) return set_info(m, info, -3);
    if (lda < std::max(1, n)) return set_info(m, info, -5);
    if (lwork == -1) {
        write_scalar<T>(m, work, std::max(1.0, 4.0 * n));
        return set_info(m, info, 0);
    }
    if (n == 0) return set_info(m, info, 0);
    const auto a = read_matrix<T>(m, a_at, n, n, lda);
    const auto e = general_eigen(a, jobvr == 'V', jobvl == 'V');
    if constexpr (cx) {
        std::vector<T> w(e.values.size());
        for (std::size_t i = 0; i < w.size(); ++i) w[i] = stored<T>(e.values[i]);
        m.write_bytes(arg(s, m, 5), std::span(reinterpret_cast<const std::uint8_t*>(w.data()), w.size() * sizeof(T)));
        if (jobvl == 'V') write_matrix<T>(m, vl_at, e.left, ldvl);
        if (jobvr == 'V') write_matrix<T>(m, vr_at, e.right, ldvr);
    } else {
        // Pair conjugate eigenvalues (positive imaginary part first); vectors as real and imaginary parts.
        double scale = 0;
        for (const auto& x : a.a) scale = std::max(scale, std::abs(x));
        const double tolerance = std::max(scale, 1.0) * 1e-10;
        std::vector<double> wr, wi;
        Matrix vl(n, n), vr(n, n);
        std::vector<bool> used(static_cast<std::size_t>(n));
        int column = 0;
        for (int i = 0; i < n; ++i) {
            if (used[static_cast<std::size_t>(i)]) continue;
            used[static_cast<std::size_t>(i)] = true;
            const C lambda = e.values[static_cast<std::size_t>(i)];
            if (std::abs(lambda.imag()) <= tolerance) {
                wr.push_back(lambda.real());
                wi.push_back(0);
                for (int r = 0; r < n; ++r) {
                    if (jobvr == 'V') vr(r, column) = C(e.right(r, i).real(), 0);
                    if (jobvl == 'V') vl(r, column) = C(e.left(r, i).real(), 0);
                }
                ++column;
                continue;
            }
            // The partner: the unused eigenvalue nearest conj(lambda).
            int partner = -1;
            for (int j = 0; j < n; ++j)
                if (!used[static_cast<std::size_t>(j)] &&
                    (partner < 0 || std::abs(e.values[static_cast<std::size_t>(j)] - std::conj(lambda)) <
                                        std::abs(e.values[static_cast<std::size_t>(partner)] - std::conj(lambda))))
                    partner = j;
            const int positive = lambda.imag() > 0 || partner < 0 ? i : partner;
            if (partner >= 0) used[static_cast<std::size_t>(partner)] = true;
            const C value = e.values[static_cast<std::size_t>(positive)];
            const double re = value.real(), im = std::abs(value.imag());
            wr.insert(wr.end(), {re, re});
            wi.insert(wi.end(), {im, -im});
            for (int r = 0; r < n; ++r) {
                if (jobvr == 'V') { vr(r, column) = e.right(r, positive).real(); vr(r, column + 1) = e.right(r, positive).imag(); }
                if (jobvl == 'V') { vl(r, column) = e.left(r, positive).real(); vl(r, column + 1) = e.left(r, positive).imag(); }
            }
            column += 2;
            if (column > n) break;   // an unpaired complex value at the end: keep the count
        }
        wr.resize(static_cast<std::size_t>(n));
        wi.resize(static_cast<std::size_t>(n));
        write_reals<R>(m, arg(s, m, 5), wr);
        write_reals<R>(m, arg(s, m, 6), wi);
        if (jobvl == 'V') write_matrix<T>(m, vl_at, vl, ldvl);
        if (jobvr == 'V') write_matrix<T>(m, vr_at, vr, ldvr);
    }
    set_info(m, info, e.converged ? 0 : 1);
}

template <class T>
void geqrf(CpuState& s, GuestMemory& m) {
    // (m, n, a, lda, tau, work, lwork, info)
    const auto info = arg(s, m, 7);
    const auto rows = integer(m, s.x[0]), cols = integer(m, s.x[1]), lda = integer(m, s.x[3]), lwork = integer(m, s.x[6]);
    if (bad_dimension(rows)) return set_info(m, info, -1);
    if (bad_dimension(cols)) return set_info(m, info, -2);
    if (lda < std::max(1, rows)) return set_info(m, info, -4);
    if (lwork == -1) {
        write_scalar<T>(m, s.x[5], std::max(1, cols) * 32.0);
        return set_info(m, info, 0);
    }
    if (!rows || !cols) return set_info(m, info, 0);
    auto a = read_matrix<T>(m, s.x[2], rows, cols, lda);
    std::vector<C> tau;
    householder_qr<T>(a, tau);
    write_matrix<T>(m, s.x[2], a, lda);
    std::vector<T> t(tau.size());
    for (std::size_t i = 0; i < t.size(); ++i) t[i] = stored<T>(tau[i]);
    if (!t.empty()) m.write_bytes(s.x[4], std::span(reinterpret_cast<const std::uint8_t*>(t.data()), t.size() * sizeof(T)));
    set_info(m, info, 0);
}

template <class T>
void orgqr(CpuState& s, GuestMemory& m) {
    // (m, n, k, a, lda, tau, work, lwork, info)
    const auto info = arg(s, m, 8);
    const auto rows = integer(m, s.x[0]), cols = integer(m, s.x[1]), k = integer(m, s.x[2]), lda = integer(m, s.x[4]);
    const auto lwork = integer(m, s.x[7]);
    if (bad_dimension(rows)) return set_info(m, info, -1);
    if (cols < 0 || cols > rows) return set_info(m, info, -2);
    if (k < 0 || k > cols) return set_info(m, info, -3);
    if (lda < std::max(1, rows)) return set_info(m, info, -5);
    if (lwork == -1) {
        write_scalar<T>(m, s.x[6], std::max(1, cols) * 32.0);
        return set_info(m, info, 0);
    }
    if (!cols) return set_info(m, info, 0);
    const auto a = read_matrix<T>(m, s.x[3], rows, cols, lda);
    std::vector<T> raw(static_cast<std::size_t>(k));
    if (k) m.read_bytes(s.x[5], std::span(reinterpret_cast<std::uint8_t*>(raw.data()), raw.size() * sizeof(T)));
    std::vector<C> tau;
    for (const auto& t : raw) tau.push_back(loaded<T>(t));
    write_matrix<T>(m, s.x[3], explicit_q(a, tau, cols, k), lda);
    set_info(m, info, 0);
}

} // namespace

void install_accelerate_lapack(ProcessContext& context, const std::function<transmute::GuestCallHandler(transmute::GuestCallHandler)>& wrap) {
    constexpr std::string_view accelerate = "/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate";
    const auto install = [&](const std::string& name, transmute::GuestCallHandler handler) {
        static_cast<void>(context.install_function("_" + name + "$NEWLAPACK", wrap(std::move(handler)), std::string(accelerate)));
    };
    install("sgesdd", gesdd<float>);
    install("dgesdd", gesdd<double>);
    install("cgesdd", gesdd<std::complex<float>>);
    install("zgesdd", gesdd<std::complex<double>>);
    install("ssyevd", eigen_symmetric<float>);
    install("dsyevd", eigen_symmetric<double>);
    install("cheevd", eigen_symmetric<std::complex<float>>);
    install("zheevd", eigen_symmetric<std::complex<double>>);
    install("sgeev", geev<float>);
    install("dgeev", geev<double>);
    install("cgeev", geev<std::complex<float>>);
    install("zgeev", geev<std::complex<double>>);
    install("sgeqrf", geqrf<float>);
    install("dgeqrf", geqrf<double>);
    install("cgeqrf", geqrf<std::complex<float>>);
    install("zgeqrf", geqrf<std::complex<double>>);
    install("sorgqr", orgqr<float>);
    install("dorgqr", orgqr<double>);
    install("cungqr", orgqr<std::complex<float>>);
    install("zungqr", orgqr<std::complex<double>>);
}

} // namespace alchemy::accelerate
