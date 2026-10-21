#include "alchemy/darwin_numeric.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace alchemy {
transmute::GuestCallHandler numeric_floating_handler(transmute::GuestCallHandler handler);
namespace {
constexpr std::string_view accelerate = "/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate";
constexpr std::uint64_t byte_limit = 256ULL * 1024 * 1024, operation_limit = 64ULL * 1024 * 1024;
constexpr std::int32_t dimension_limit = 4096;
[[noreturn]] void fail(std::string_view message) { throw std::runtime_error("Alchemy numeric: " + std::string(message)); }
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    if (b > UINT64_MAX - a) fail("address arithmetic overflow");
    return a + b;
}
std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    if (a && b > UINT64_MAX / a) fail("shape arithmetic overflow");
    return a * b;
}
void validate(const transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size, bool writable) {
    if (size > byte_limit) fail("buffer exceeds the 256 MiB numeric profile");
    const auto end = add(address, size);
    while (address < end) {
        const auto region = memory.query_region(address);
        if (!region || !region->readable || (writable && !region->writable) ||
            address < region->base || address - region->base >= region->size) fail("inaccessible guest numeric buffer");
        address = std::min(end, add(region->base, region->size));
    }
}
void work(std::int32_t m, std::int32_t n, std::int32_t k = 1) {
    if (m < 0 || n < 0 || k < 0) fail("negative BLAS dimension");
    if (m > dimension_limit || n > dimension_limit || k > dimension_limit ||
        multiply(multiply(static_cast<std::uint64_t>(m), static_cast<std::uint64_t>(n)), static_cast<std::uint64_t>(k)) > operation_limit)
        fail("operation exceeds the bounded numeric profile");
}
// Apple fixed arguments pack spilled int32 values into four bytes, while
// pointers retain eight-byte alignment. Integer and FP register banks advance
// separately. No host ABI or guest pointer is passed to a host BLAS routine.
struct Arguments {
    transmute::CpuState& state;
    transmute::GuestMemory& memory;
    unsigned integer{}, floating{};
    std::uint64_t stack{};
    std::uint64_t spill(unsigned bytes) {
        stack = add(stack, bytes - 1) & ~static_cast<std::uint64_t>(bytes - 1);
        const auto address = add(state.sp, stack); stack = add(stack, bytes); return address;
    }
    std::int32_t i32() {
        if (integer < 8) return static_cast<std::int32_t>(state.x[integer++]);
        return static_cast<std::int32_t>(memory.read32(spill(4)));
    }
    std::uint64_t pointer() {
        if (integer < 8) return state.x[integer++];
        return memory.read64(spill(8));
    }
    template<class Real> Real real() {
        if constexpr (sizeof(Real) == 4) {
            const auto bits = floating < 8 ? static_cast<std::uint32_t>(state.v[floating++][0]) : memory.read32(spill(4));
            return std::bit_cast<float>(bits);
        } else {
            const auto bits = floating < 8 ? state.v[floating++][0] : memory.read64(spill(8));
            return std::bit_cast<double>(bits);
        }
    }
};
template<class T> struct Scalar { using Real = T; static constexpr bool complex = false; };
template<class R> struct Scalar<std::complex<R>> { using Real = R; static constexpr bool complex = true; };
template<class T> constexpr unsigned width = static_cast<unsigned>(sizeof(typename Scalar<T>::Real)) * (Scalar<T>::complex ? 2U : 1U);
template<class R> R read_real(const transmute::GuestMemory& memory, std::uint64_t address) {
    if constexpr (sizeof(R) == 4) return std::bit_cast<float>(memory.read32(address));
    else return std::bit_cast<double>(memory.read64(address));
}
template<class R> void write_real(transmute::GuestMemory& memory, std::uint64_t address, R value) {
    if constexpr (sizeof(R) == 4) memory.write32(address, std::bit_cast<std::uint32_t>(value));
    else memory.write64(address, std::bit_cast<std::uint64_t>(value));
}
template<class T> T read(const transmute::GuestMemory& memory, std::uint64_t address) {
    using R = typename Scalar<T>::Real;
    if constexpr (Scalar<T>::complex) return {read_real<R>(memory, address), read_real<R>(memory, add(address, sizeof(R)))};
    else return read_real<R>(memory, address);
}
template<class T> void write(transmute::GuestMemory& memory, std::uint64_t address, T value) {
    if constexpr (Scalar<T>::complex) {
        using R = typename Scalar<T>::Real;
        write_real(memory, address, value.real()); write_real(memory, add(address, sizeof(R)), value.imag());
    } else write_real(memory, address, value);
}
template<class T> T conjugate(T value) {
    if constexpr (Scalar<T>::complex) return std::conj(value);
    else return value;
}
template<class T> T coefficient(Arguments& args) {
    if constexpr (Scalar<T>::complex) return read<T>(args.memory, args.pointer());
    else return args.real<T>();
}
template<class R> void result(transmute::CpuState& state, R value, unsigned reg = 0) {
    if constexpr (sizeof(R) == 4) state.v[reg][0] = std::bit_cast<std::uint32_t>(value);
    else state.v[reg][0] = std::bit_cast<std::uint64_t>(value);
    state.v[reg][1] = 0;
}
template<class T> struct Vector {
    transmute::GuestMemory& memory;
    std::uint64_t base;
    std::int32_t count, increment;
    Vector(transmute::GuestMemory& m, std::uint64_t b, std::int32_t n, std::int32_t inc, bool writable)
        : memory(m), base(b), count(n), increment(inc) {
        if (!inc) fail("zero BLAS vector increment");
        if (n > 0) validate(m, b, multiply(add(multiply(static_cast<std::uint64_t>(n - 1),
            static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(inc)))), 1), width<T>), writable);
    }
    std::uint64_t address(std::int32_t index) const {
        const auto at = increment < 0 ? count - 1 - index : index;
        return add(base, multiply(multiply(static_cast<std::uint64_t>(at),
            static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(increment)))), width<T>));
    }
    T get(std::int32_t index) const { return read<T>(memory, address(index)); }
    void put(std::int32_t index, T value) const { write(memory, address(index), value); }
};
template<class T> struct Matrix {
    transmute::GuestMemory& memory;
    std::uint64_t base;
    std::int32_t rows, columns, leading;
    bool row_major;
    Matrix(transmute::GuestMemory& m, std::uint64_t b, std::int32_t r, std::int32_t c,
           std::int32_t ld, bool row, bool writable) : memory(m), base(b), rows(r), columns(c), leading(ld), row_major(row) {
        if (ld < std::max(1, row ? c : r)) fail("invalid BLAS leading dimension");
        if (r && c) validate(m, b, multiply(add(multiply(static_cast<std::uint64_t>((row ? r : c) - 1),
            static_cast<std::uint64_t>(ld)), static_cast<std::uint64_t>(row ? c : r)), width<T>), writable);
    }
    std::uint64_t address(std::int32_t r, std::int32_t c) const {
        return add(base, multiply(add(multiply(static_cast<std::uint64_t>(row_major ? r : c),
            static_cast<std::uint64_t>(leading)), static_cast<std::uint64_t>(row_major ? c : r)), width<T>));
    }
    T get(std::int32_t r, std::int32_t c) const { return read<T>(memory, address(r, c)); }
    void put(std::int32_t r, std::int32_t c, T value) const { write(memory, address(r, c), value); }
};
bool order(std::int32_t value) {
    if (value != 101 && value != 102) fail("invalid CBLAS order"); return value == 101;
}
void transpose(std::int32_t value) { if (value < 111 || value > 113) fail("invalid CBLAS transpose"); }
template<class T> T element(const Matrix<T>& matrix, std::int32_t trans, std::int32_t r, std::int32_t c) {
    const auto value = trans == 111 ? matrix.get(r, c) : matrix.get(c, r);
    return trans == 113 ? conjugate(value) : value;
}
template<class T> void axpy(transmute::CpuState& state, transmute::GuestMemory& memory) {
    Arguments args{state, memory}; const auto n = args.i32(); const auto alpha = coefficient<T>(args);
    const auto xp = args.pointer(); const auto ix = args.i32(); const auto yp = args.pointer(); const auto iy = args.i32();
    if (n <= 0 || alpha == T{}) return;
    work(n, 1); Vector<T> x(memory, xp, n, ix, false), y(memory, yp, n, iy, true);
    std::vector<T> values(static_cast<std::size_t>(n));
    for (std::int32_t i = 0; i < n; ++i) values[i] = alpha * x.get(i) + y.get(i);
    for (std::int32_t i = 0; i < n; ++i) y.put(i, values[i]);
}
template<class T> void dot(transmute::CpuState& state, transmute::GuestMemory& memory, bool conj) {
    Arguments args{state, memory}; const auto n = args.i32(); const auto xp = args.pointer(); const auto ix = args.i32();
    const auto yp = args.pointer(); const auto iy = args.i32(); std::uint64_t output{};
    if constexpr (Scalar<T>::complex) { output = args.pointer(); validate(memory, output, width<T>, true); }
    T total{};
    if (n > 0) {
        work(n, 1); Vector<T> x(memory, xp, n, ix, false), y(memory, yp, n, iy, false);
        for (std::int32_t i = 0; i < n; ++i) total += (conj ? conjugate(x.get(i)) : x.get(i)) * y.get(i);
    }
    if constexpr (Scalar<T>::complex) write(memory, output, total);
    else result(state, total);
}
template<class T> void gemm(transmute::CpuState& state, transmute::GuestMemory& memory) {
    Arguments args{state, memory}; const auto row = order(args.i32()); const auto ta = args.i32(), tb = args.i32();
    transpose(ta); transpose(tb); const auto m = args.i32(), n = args.i32(), k = args.i32();
    const auto alpha = coefficient<T>(args); const auto ap = args.pointer(); const auto lda = args.i32();
    const auto bp = args.pointer(); const auto ldb = args.i32(); const auto beta = coefficient<T>(args);
    const auto cp = args.pointer(); const auto ldc = args.i32(); work(m, n, k);
    if (!m || !n) return;
    Matrix<T> c(memory, cp, m, n, ldc, row, true);
    std::vector<T> values(static_cast<std::size_t>(m) * n);
    if (alpha != T{} && k) {
        Matrix<T> a(memory, ap, ta == 111 ? m : k, ta == 111 ? k : m, lda, row, false);
        Matrix<T> b(memory, bp, tb == 111 ? k : n, tb == 111 ? n : k, ldb, row, false);
        for (std::int32_t i = 0; i < m; ++i) for (std::int32_t j = 0; j < n; ++j) {
            T sum{}; for (std::int32_t l = 0; l < k; ++l) sum += element(a, ta, i, l) * element(b, tb, l, j);
            values[static_cast<std::size_t>(i) * n + j] = alpha * sum;
        }
    }
    if (beta != T{}) for (std::int32_t i = 0; i < m; ++i) for (std::int32_t j = 0; j < n; ++j)
        values[static_cast<std::size_t>(i) * n + j] += beta * c.get(i, j);
    for (std::int32_t i = 0; i < m; ++i) for (std::int32_t j = 0; j < n; ++j) c.put(i, j, values[static_cast<std::size_t>(i) * n + j]);
}
template<class T> void gemv(transmute::CpuState& state, transmute::GuestMemory& memory) {
    Arguments args{state, memory}; const auto row = order(args.i32()); const auto trans = args.i32(); transpose(trans);
    const auto m = args.i32(), n = args.i32(); const auto alpha = coefficient<T>(args);
    const auto ap = args.pointer(); const auto lda = args.i32(); const auto xp = args.pointer(); const auto ix = args.i32();
    const auto beta = coefficient<T>(args); const auto yp = args.pointer(); const auto iy = args.i32(); work(m, n);
    if (!m || !n) return;
    const auto rows = trans == 111 ? m : n, columns = trans == 111 ? n : m;
    Vector<T> y(memory, yp, rows, iy, true); std::vector<T> values(static_cast<std::size_t>(rows));
    if (alpha != T{}) {
        Matrix<T> a(memory, ap, m, n, lda, row, false); Vector<T> x(memory, xp, columns, ix, false);
        for (std::int32_t i = 0; i < rows; ++i) {
            T sum{}; for (std::int32_t j = 0; j < columns; ++j) sum += element(a, trans, i, j) * x.get(j);
            values[i] = alpha * sum;
        }
    }
    for (std::int32_t i = 0; i < rows; ++i) { if (beta != T{}) values[i] += beta * y.get(i); }
    for (std::int32_t i = 0; i < rows; ++i) y.put(i, values[i]);
}
template<class T> void syrk(transmute::CpuState& state, transmute::GuestMemory& memory) {
    Arguments args{state, memory}; const auto row = order(args.i32()); const auto uplo = args.i32(), trans = args.i32();
    transpose(trans); if (uplo != 121 && uplo != 122) fail("invalid CBLAS triangle");
    if constexpr (Scalar<T>::complex) { if (trans == 113) fail("complex SYRK does not accept conjugate transpose"); }
    const auto n = args.i32(), k = args.i32(); const auto alpha = coefficient<T>(args);
    const auto ap = args.pointer(); const auto lda = args.i32(); const auto beta = coefficient<T>(args);
    const auto cp = args.pointer(); const auto ldc = args.i32(); work(n, n, k); if (!n) return;
    Matrix<T> c(memory, cp, n, n, ldc, row, true); std::vector<T> values(static_cast<std::size_t>(n) * n);
    if (alpha != T{} && k) {
        Matrix<T> a(memory, ap, trans == 111 ? n : k, trans == 111 ? k : n, lda, row, false);
        for (std::int32_t i = 0; i < n; ++i) for (std::int32_t j = 0; j < n; ++j) if ((uplo == 121) == (i <= j) || i == j) {
            T sum{}; for (std::int32_t l = 0; l < k; ++l) sum += element(a, trans, i, l) * element(a, trans, j, l);
            values[static_cast<std::size_t>(i) * n + j] = alpha * sum;
        }
    }
    for (std::int32_t i = 0; i < n; ++i) for (std::int32_t j = 0; j < n; ++j) if ((uplo == 121) == (i <= j) || i == j) {
        auto& value = values[static_cast<std::size_t>(i) * n + j]; if (beta != T{}) value += beta * c.get(i, j);
    }
    for (std::int32_t i = 0; i < n; ++i) for (std::int32_t j = 0; j < n; ++j)
        if ((uplo == 121) == (i <= j) || i == j) c.put(i, j, values[static_cast<std::size_t>(i) * n + j]);
}
std::int32_t integer(const transmute::GuestMemory& memory, std::uint64_t address) { return static_cast<std::int32_t>(memory.read32(address)); }
void info(transmute::GuestMemory& memory, std::uint64_t address, std::int32_t value) { memory.write32(address, static_cast<std::uint32_t>(value)); }
template<class T> auto pivot_size(T value) {
    if constexpr (Scalar<T>::complex) return std::abs(value.real()) + std::abs(value.imag());
    else return std::abs(value);
}
template<class T> std::int32_t factor(std::vector<T>& a, std::int32_t m, std::int32_t n, std::vector<std::int32_t>& pivots) {
    std::int32_t singular{};
    const auto cell = [&](std::int32_t r, std::int32_t c) -> T& { return a[static_cast<std::size_t>(c) * m + r]; };
    for (std::int32_t k = 0; k < std::min(m, n); ++k) {
        auto pivot = k; for (std::int32_t r = k + 1; r < m; ++r) if (pivot_size(cell(r, k)) > pivot_size(cell(pivot, k))) pivot = r;
        pivots[k] = pivot + 1;
        if (pivot != k) for (std::int32_t c = 0; c < n; ++c) std::swap(cell(k, c), cell(pivot, c));
        if (cell(k, k) == T{}) { if (!singular) singular = k + 1; continue; }
        for (std::int32_t r = k + 1; r < m; ++r) {
            cell(r, k) /= cell(k, k);
            for (std::int32_t c = k + 1; c < n; ++c) cell(r, c) -= cell(r, k) * cell(k, c);
        }
    }
    return singular;
}
template<class T> std::vector<T> snapshot(const Matrix<T>& a) {
    std::vector<T> values(static_cast<std::size_t>(a.rows) * a.columns);
    for (std::int32_t c = 0; c < a.columns; ++c) for (std::int32_t r = 0; r < a.rows; ++r) values[static_cast<std::size_t>(c) * a.rows + r] = a.get(r, c);
    return values;
}
template<class T> void store(const Matrix<T>& a, const std::vector<T>& values) {
    for (std::int32_t c = 0; c < a.columns; ++c) for (std::int32_t r = 0; r < a.rows; ++r) a.put(r, c, values[static_cast<std::size_t>(c) * a.rows + r]);
}
template<class T> void getrf(transmute::CpuState& s, transmute::GuestMemory& memory) {
    const auto m = integer(memory, s.x[0]), n = integer(memory, s.x[1]), lda = integer(memory, s.x[3]);
    const auto out = s.x[5]; validate(memory, out, 4, true);
    if (m < 0) { info(memory, out, -1); return; } if (n < 0) { info(memory, out, -2); return; }
    if (lda < std::max(1, m)) { info(memory, out, -4); return; } work(m, n, std::min(m, n));
    Matrix<T> matrix(memory, s.x[2], m, n, lda, false, true);
    validate(memory, s.x[4], static_cast<std::uint64_t>(std::min(m, n)) * 4, true);
    auto a = snapshot(matrix); std::vector<std::int32_t> pivots(static_cast<std::size_t>(std::min(m, n)));
    const auto status = factor(a, m, n, pivots); store(matrix, a);
    for (std::size_t i = 0; i < pivots.size(); ++i) memory.write32(add(s.x[4], i * 4), static_cast<std::uint32_t>(pivots[i]));
    info(memory, out, status); s.x[0] = 0;
}
template<class T> void gesv(transmute::CpuState& s, transmute::GuestMemory& memory) {
    const auto n = integer(memory, s.x[0]), nrhs = integer(memory, s.x[1]);
    const auto lda = integer(memory, s.x[3]), ldb = integer(memory, s.x[6]); const auto out = s.x[7]; validate(memory, out, 4, true);
    if (n < 0) { info(memory, out, -1); return; } if (nrhs < 0) { info(memory, out, -2); return; }
    if (lda < std::max(1, n)) { info(memory, out, -4); return; } if (ldb < std::max(1, n)) { info(memory, out, -7); return; }
    work(n, n, std::max(n, nrhs)); Matrix<T> matrix(memory, s.x[2], n, n, lda, false, true), right(memory, s.x[5], n, nrhs, ldb, false, true);
    validate(memory, s.x[4], static_cast<std::uint64_t>(n) * 4, true);
    auto a = snapshot(matrix), b = snapshot(right); std::vector<std::int32_t> pivots(static_cast<std::size_t>(n));
    const auto status = factor(a, n, n, pivots);
    if (!status) for (std::int32_t c = 0; c < nrhs; ++c) {
        const auto cell = [&](std::int32_t r) -> T& { return b[static_cast<std::size_t>(c) * n + r]; };
        for (std::int32_t k = 0; k < n; ++k) if (pivots[k] - 1 != k) std::swap(cell(k), cell(pivots[k] - 1));
        for (std::int32_t r = 0; r < n; ++r) for (std::int32_t k = 0; k < r; ++k) cell(r) -= a[static_cast<std::size_t>(k) * n + r] * cell(k);
        for (std::int32_t r = n; r-- > 0;) {
            for (std::int32_t k = r + 1; k < n; ++k) cell(r) -= a[static_cast<std::size_t>(k) * n + r] * cell(k);
            cell(r) /= a[static_cast<std::size_t>(r) * n + r];
        }
    }
    store(matrix, a); if (!status) store(right, b);
    for (std::size_t i = 0; i < pivots.size(); ++i) memory.write32(add(s.x[4], i * 4), static_cast<std::uint32_t>(pivots[i]));
    info(memory, out, status); s.x[0] = 0;
}
template<class T> void potrf(transmute::CpuState& s, transmute::GuestMemory& memory) {
    const auto triangle = static_cast<char>(memory.read8(s.x[0])); const auto n = integer(memory, s.x[1]), lda = integer(memory, s.x[3]);
    const auto out = s.x[4]; validate(memory, out, 4, true);
    const bool lower = triangle == 'L' || triangle == 'l', upper = triangle == 'U' || triangle == 'u';
    if (!lower && !upper) { info(memory, out, -1); return; } if (n < 0) { info(memory, out, -2); return; }
    if (lda < std::max(1, n)) { info(memory, out, -4); return; } work(n, n, n);
    Matrix<T> matrix(memory, s.x[2], n, n, lda, false, true); std::vector<T> l(static_cast<std::size_t>(n) * n);
    const auto cell = [&](std::int32_t r, std::int32_t c) -> T& { return l[static_cast<std::size_t>(c) * n + r]; };
    std::int32_t status{};
    for (std::int32_t c = 0; c < n; ++c) {
        auto diagonal = std::real(matrix.get(c, c));
        for (std::int32_t k = 0; k < c; ++k) diagonal -= std::real(cell(c, k) * conjugate(cell(c, k)));
        if (!(diagonal > 0)) { status = c + 1; cell(c, c) = T{diagonal}; break; }
        cell(c, c) = T{std::sqrt(diagonal)};
        for (std::int32_t r = c + 1; r < n; ++r) {
            T value = lower ? matrix.get(r, c) : conjugate(matrix.get(c, r));
            for (std::int32_t k = 0; k < c; ++k) value -= cell(r, k) * conjugate(cell(c, k));
            cell(r, c) = value / cell(c, c);
        }
    }
    const auto done = status ? status : n;
    for (std::int32_t c = 0; c < done; ++c) for (std::int32_t r = c; r < n; ++r) {
        if (status && c == status - 1 && r != c) continue;
        if (lower) matrix.put(r, c, cell(r, c)); else matrix.put(c, r, conjugate(cell(r, c)));
    }
    info(memory, out, status); s.x[0] = 0;
}
template<class T> void copy(transmute::CpuState& s, transmute::GuestMemory& memory) {
    const auto n = integer(memory, s.x[0]), ix = integer(memory, s.x[2]), iy = integer(memory, s.x[4]);
    if (n <= 0) return; work(n, 1); Vector<T> x(memory, s.x[1], n, ix, false), y(memory, s.x[3], n, iy, true);
    std::vector<T> values(static_cast<std::size_t>(n)); for (std::int32_t i = 0; i < n; ++i) values[i] = x.get(i);
    for (std::int32_t i = 0; i < n; ++i) y.put(i, values[i]); s.x[0] = 0;
}
using Complex = std::complex<double>;
template<class T> Complex working(T value) { return {static_cast<double>(std::real(value)), static_cast<double>(std::imag(value))}; }
template<class T> T stored(Complex value) {
    using R = typename Scalar<T>::Real;
    if constexpr (Scalar<T>::complex) return {static_cast<R>(value.real()), static_cast<R>(value.imag())};
    else return static_cast<T>(value.real());
}
struct SingularSystem {
    std::vector<Complex> left, right;
    std::vector<double> scaled_values;
    double scale{};
    bool converged{};
};
// An authored one-sided Jacobi SVD orthogonalizes columns directly, rather
// than squaring the condition number through normal equations. For wide A,
// factor A^H and exchange its left/right singular vectors. This bounded
// algorithm implements the GELSD contract without importing LAPACK code.
SingularSystem singular_system(const std::vector<Complex>& original, std::int32_t m, std::int32_t n) {
    const bool wide = m < n; const auto rows = std::max(m, n), columns = std::min(m, n);
    SingularSystem result; result.scaled_values.resize(static_cast<std::size_t>(columns));
    std::vector<Complex> a(static_cast<std::size_t>(rows) * columns), v(static_cast<std::size_t>(columns) * columns);
    for (const auto value : original) {
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) return result;
        result.scale = std::max({result.scale, std::abs(value.real()), std::abs(value.imag())});
    }
    for (std::int32_t c = 0; c < columns; ++c) {
        v[static_cast<std::size_t>(c) * columns + c] = 1;
        for (std::int32_t r = 0; r < rows; ++r) a[static_cast<std::size_t>(c) * rows + r] =
            result.scale ? (wide ? std::conj(original[static_cast<std::size_t>(r) * m + c]) : original[static_cast<std::size_t>(c) * m + r]) / result.scale : Complex{};
    }
    std::uint64_t operations{};
    for (unsigned sweep = 0; sweep < 80; ++sweep) {
        bool changed{};
        for (std::int32_t p = 0; p < columns; ++p) for (std::int32_t q = p + 1; q < columns; ++q) {
            double aa{}, bb{}; Complex cross{};
            for (std::int32_t r = 0; r < rows; ++r) {
                const auto x = a[static_cast<std::size_t>(p) * rows + r], y = a[static_cast<std::size_t>(q) * rows + r];
                aa += std::norm(x); bb += std::norm(y); cross += std::conj(x) * y;
            }
            const auto magnitude = std::abs(cross);
            if (!magnitude || magnitude <= 8 * std::numeric_limits<double>::epsilon() * std::sqrt(aa) * std::sqrt(bb)) continue;
            const auto tau = (bb - aa) / (2 * magnitude);
            const auto tangent = std::copysign(1.0, tau) / (std::abs(tau) + std::hypot(1.0, tau));
            if (!tangent) continue;
            const auto cosine = 1 / std::hypot(1.0, tangent), sine = cosine * tangent; const auto phase = cross / magnitude;
            for (std::int32_t r = 0; r < rows; ++r) {
                auto& x = a[static_cast<std::size_t>(p) * rows + r]; auto& y = a[static_cast<std::size_t>(q) * rows + r];
                const auto old = x; x = cosine * old - sine * std::conj(phase) * y; y = sine * phase * old + cosine * y;
            }
            for (std::int32_t r = 0; r < columns; ++r) {
                auto& x = v[static_cast<std::size_t>(p) * columns + r]; auto& y = v[static_cast<std::size_t>(q) * columns + r];
                const auto old = x; x = cosine * old - sine * std::conj(phase) * y; y = sine * phase * old + cosine * y;
            }
            operations = add(operations, static_cast<std::uint64_t>(rows + columns) * 4);
            if (operations > operation_limit) fail("least-squares SVD exceeds the numeric operation budget");
            changed = true;
        }
        if (!changed) { result.converged = true; break; }
    }
    if (!result.converged) return result;
    std::vector<double> norms(static_cast<std::size_t>(columns)); std::vector<std::int32_t> ordering(static_cast<std::size_t>(columns));
    std::iota(ordering.begin(), ordering.end(), 0);
    for (std::int32_t c = 0; c < columns; ++c) for (std::int32_t r = 0; r < rows; ++r)
        norms[c] = std::hypot(norms[c], std::abs(a[static_cast<std::size_t>(c) * rows + r]));
    std::stable_sort(ordering.begin(), ordering.end(), [&](auto left, auto right) { return norms[left] > norms[right]; });
    result.left.resize(static_cast<std::size_t>(m) * columns); result.right.resize(static_cast<std::size_t>(n) * columns);
    for (std::int32_t c = 0; c < columns; ++c) {
        const auto source = ordering[c]; result.scaled_values[c] = norms[source];
        for (std::int32_t r = 0; r < rows; ++r) {
            const auto value = norms[source] ? a[static_cast<std::size_t>(source) * rows + r] / norms[source] : Complex{};
            (wide ? result.right : result.left)[static_cast<std::size_t>(c) * rows + r] = value;
        }
        for (std::int32_t r = 0; r < columns; ++r)
            (wide ? result.left : result.right)[static_cast<std::size_t>(c) * columns + r] = v[static_cast<std::size_t>(source) * columns + r];
    }
    return result;
}
template<class T> void gelsd(transmute::CpuState& state, transmute::GuestMemory& memory) {
    using R = typename Scalar<T>::Real;
    Arguments args{state, memory}; std::uint64_t p[15]{};
    constexpr unsigned count = Scalar<T>::complex ? 15 : 14;
    for (unsigned i = 0; i < count; ++i) p[i] = args.pointer();
    const auto m = integer(memory, p[0]), n = integer(memory, p[1]), nrhs = integer(memory, p[2]);
    const auto lda = integer(memory, p[4]), ldb = integer(memory, p[6]), lwork = integer(memory, p[11]);
    const auto output = p[count - 1]; validate(memory, output, 4, true);
    if (m < 0) { info(memory, output, -1); return; } if (n < 0) { info(memory, output, -2); return; }
    if (nrhs < 0) { info(memory, output, -3); return; }
    if (lda < std::max(1, m)) { info(memory, output, -5); return; }
    if (ldb < std::max({1, m, n})) { info(memory, output, -7); return; }
    const auto k = std::min(m, n), extent = std::max(m, n);
    if (extent > 512 || k > 128 || nrhs > 256) fail("GELSD profile supports at most 512 rows/columns, 128 singular values and 256 right-hand sides");
    const auto needed = std::max(1, m * n + (m + n) * k + nrhs * extent + k * k);
    std::int32_t levels{}; for (auto size = k / 26; size > 0; size /= 2) ++levels;
    const auto integer_needed = std::max(1, 3 * k * levels + 11 * k), real_needed = std::max(1, needed);
    const auto iwork = p[Scalar<T>::complex ? 13 : 12];
    validate(memory, p[10], width<T>, true); validate(memory, iwork, 4, true);
    if constexpr (Scalar<T>::complex) validate(memory, p[12], sizeof(R), true);
    if (lwork == -1) {
        write(memory, p[10], T{static_cast<R>(needed)}); info(memory, iwork, integer_needed);
        if constexpr (Scalar<T>::complex) write_real(memory, p[12], static_cast<R>(real_needed));
        info(memory, output, 0); state.x[0] = 0; return;
    }
    if (lwork < needed) { info(memory, output, -12); return; }
    validate(memory, p[10], multiply(static_cast<std::uint64_t>(lwork), width<T>), true);
    validate(memory, iwork, static_cast<std::uint64_t>(integer_needed) * 4, true);
    if constexpr (Scalar<T>::complex) validate(memory, p[12], static_cast<std::uint64_t>(real_needed) * sizeof(R), true);
    Matrix<T> matrix(memory, p[3], m, n, lda, false, true), rhs(memory, p[5], extent, nrhs, ldb, false, true);
    validate(memory, p[7], static_cast<std::uint64_t>(k) * sizeof(R), true); validate(memory, p[9], 4, true);
    const auto rcond = read_real<R>(memory, p[8]);
    if (std::isnan(rcond)) { info(memory, output, -9); return; }
    const auto a = snapshot(matrix), b = snapshot(rhs); std::vector<Complex> values(a.size());
    std::transform(a.begin(), a.end(), values.begin(), [](T value) { return working(value); });
    const auto svd = singular_system(values, m, n);
    if (!svd.converged) { info(memory, output, 1); return; }
    for (std::int32_t c = 0; c < nrhs; ++c) for (std::int32_t r = 0; r < m; ++r) {
        const auto value = working(b[static_cast<std::size_t>(c) * extent + r]);
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) { info(memory, output, 1); return; }
    }
    const auto cutoff = (rcond < 0 ? static_cast<double>(std::numeric_limits<R>::epsilon()) : static_cast<double>(rcond)) * (k ? svd.scaled_values[0] : 0);
    std::int32_t rank{}; for (const auto sigma : svd.scaled_values) if (sigma > cutoff && sigma > 0) ++rank;
    std::vector<T> solved(b.size());
    for (std::int32_t c = 0; c < nrhs; ++c) {
        std::vector<Complex> solution(static_cast<std::size_t>(n));
        double bscale{};
        for (std::int32_t r = 0; r < m; ++r) {
            const auto value = working(b[static_cast<std::size_t>(c) * extent + r]);
            bscale = std::max({bscale, std::abs(value.real()), std::abs(value.imag())});
        }
        for (std::int32_t i = 0; i < rank; ++i) {
            Complex projection{}; for (std::int32_t r = 0; r < m; ++r)
                if (bscale) projection += std::conj(svd.left[static_cast<std::size_t>(i) * m + r]) *
                    (working(b[static_cast<std::size_t>(c) * extent + r]) / bscale);
            int be{}, ae{}; const auto bf = std::frexp(bscale, &be), af = std::frexp(svd.scale, &ae);
            const auto coefficient = projection * (bf / af) / svd.scaled_values[i];
            const Complex amplitude{std::scalbn(coefficient.real(), be - ae), std::scalbn(coefficient.imag(), be - ae)};
            for (std::int32_t r = 0; r < n; ++r) solution[r] += svd.right[static_cast<std::size_t>(i) * n + r] * amplitude;
        }
        for (std::int32_t r = 0; r < n; ++r) {
            if (!std::isfinite(solution[r].real()) || !std::isfinite(solution[r].imag())) fail("GELSD solution exceeds representable arithmetic");
            solved[static_cast<std::size_t>(c) * extent + r] = stored<T>(solution[r]);
        }
        if (m > n) {
            double residual{};
            for (std::int32_t r = 0; r < m; ++r) {
                auto error = working(b[static_cast<std::size_t>(c) * extent + r]);
                for (std::int32_t j = 0; j < n; ++j) error -= working(a[static_cast<std::size_t>(j) * m + r]) * solution[j];
                residual = std::hypot(residual, std::abs(error));
            }
            // GELSD specifies the squared norm of this transformed tail, not
            // a particular orthogonal basis for its individual components.
            solved[static_cast<std::size_t>(c) * extent + n] = T{static_cast<R>(residual)};
        }
    }
    store(rhs, solved);
    for (std::int32_t i = 0; i < k; ++i) write_real(memory, add(p[7], static_cast<std::uint64_t>(i) * sizeof(R)), static_cast<R>(svd.scaled_values[i] * svd.scale));
    info(memory, p[9], rank); write(memory, p[10], T{static_cast<R>(needed)}); info(memory, iwork, integer_needed);
    if constexpr (Scalar<T>::complex) write_real(memory, p[12], static_cast<R>(real_needed));
    info(memory, output, 0); state.x[0] = 0;
}
template<class T> void install_type(ProcessContext& context, char precision) {
    const auto install = [&](std::string operation, transmute::GuestCallHandler handler) {
        static_cast<void>(context.install_function("_" + operation + "$NEWLAPACK", numeric_floating_handler(std::move(handler)), std::string(accelerate)));
    };
    const auto prefix = std::string("cblas_") + precision;
    install(prefix + "axpy", axpy<T>); install(prefix + "gemm", gemm<T>); install(prefix + "gemv", gemv<T>); install(prefix + "syrk", syrk<T>);
    if constexpr (Scalar<T>::complex) {
        install(prefix + "dotu_sub", [](auto& s, auto& m) { dot<T>(s, m, false); });
        install(prefix + "dotc_sub", [](auto& s, auto& m) { dot<T>(s, m, true); });
    } else install(prefix + "dot", [](auto& s, auto& m) { dot<T>(s, m, false); });
    install(std::string(1, precision) + "copy", copy<T>); install(std::string(1, precision) + "getrf", getrf<T>);
    install(std::string(1, precision) + "gesv", gesv<T>); install(std::string(1, precision) + "potrf", potrf<T>);
    install(std::string(1, precision) + "gelsd", gelsd<T>);
}
constexpr std::string_view deferred[] = {"_cgesdd$NEWLAPACK", "_cheevd$NEWLAPACK",
    "_dgeev$NEWLAPACK", "_dgeqrf$NEWLAPACK", "_dgesdd$NEWLAPACK", "_dorgqr$NEWLAPACK", "_dsyevd$NEWLAPACK",
    "_sgeev$NEWLAPACK", "_sgesdd$NEWLAPACK", "_ssyevd$NEWLAPACK",
    "_zgeev$NEWLAPACK", "_zgeqrf$NEWLAPACK", "_zgesdd$NEWLAPACK", "_zheevd$NEWLAPACK", "_zungqr$NEWLAPACK"};
} // namespace

std::string_view deferred_numeric_api(std::string_view symbol) {
    for (const auto name : deferred) if (name == symbol)
        return "LAPACK decomposition driver is outside Alchemy's BLAS/LU/Cholesky/least-squares numeric profile";
    if (symbol.find("$ILP64") != std::string_view::npos) return "Accelerate ILP64 integer ABI is unsupported; numeric replacements implement LP64";
    return {};
}

void install_darwin_numeric_math(ProcessContext& context);
void install_darwin_numeric(ProcessContext& context) {
    install_type<float>(context, 's'); install_type<double>(context, 'd');
    install_type<std::complex<float>>(context, 'c'); install_type<std::complex<double>>(context, 'z');
    for (const auto symbol : deferred) static_cast<void>(context.install_function(std::string(symbol),
        [name = std::string(symbol)](auto&, auto&) { fail(name + ": " + std::string(deferred_numeric_api(name))); }, std::string(accelerate)));
    install_darwin_numeric_math(context);
}

} // namespace alchemy
