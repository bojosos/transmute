// Accelerate (src/alchemy/darwin/accelerate/): the vDSP kernels and packed real FFT
// (vdsp.cpp), the wider vDSP API (vdsp_vectors.cpp) and vForce (vforce.cpp), and BNNS, where no
// filter can be created (bnns.cpp).
#include "alchemy/gui/tests/framework_case.hpp"

#include <bit>
#include <cmath>

namespace {
using namespace fill_test;

constexpr std::string_view vdsp = "/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate";

Address floats(Fixture& f, std::initializer_list<float> values) {
    const auto address = f.block(values.size() * 4);
    std::size_t i = 0;
    for (const auto value : values) f.engine.write32(address + 4 * i++, std::bit_cast<std::uint32_t>(value));
    return address;
}
float read_float(Fixture& f, Address address, std::size_t index = 0) { return std::bit_cast<float>(f.engine.read32(address + index * 4)); }


Address doubles(Fixture& f, std::initializer_list<double> values) {
    const auto address = f.block(values.size() * 8);
    std::size_t i = 0;
    for (const auto value : values) f.engine.write64(address + 8 * i++, std::bit_cast<std::uint64_t>(value));
    return address;
}
double read_double(Fixture& f, Address address, std::size_t index = 0) { return std::bit_cast<double>(f.engine.read64(address + index * 8)); }
Address count(Fixture& f, std::int32_t n) {
    const auto address = f.block(4);
    f.engine.write32(address, static_cast<std::uint32_t>(n));
    return address;
}
bool near(double a, double b, double tolerance = 1e-5) { return std::fabs(a - b) <= tolerance; }

// The wider vDSP API (vdsp_vectors.cpp) and vForce (vforce.cpp).
void vectors(Fixture& f) {
    const auto a = doubles(f, {1, 2, 3, 4}), b = doubles(f, {10, 20, 30, 40}), c = f.block(32), scalar = f.block(8);
    static_cast<void>(f.call(vdsp, "_vDSP_vaddD", {a, 1, b, 1, c, 1, 4}));
    require(read_double(f, c, 3) == 44, "vDSP_vaddD");
    static_cast<void>(f.call(vdsp, "_vDSP_vdivD", {a, 1, b, 1, c, 1, 4}));
    require(read_double(f, c, 1) == 10, "vDSP_vdivD: B first, C = A / B");
    static_cast<void>(f.call(vdsp, "_vDSP_meanvD", {a, 1, scalar, 4}));
    require(read_double(f, scalar) == 2.5, "vDSP_meanvD");
    static_cast<void>(f.call(vdsp, "_vDSP_dotprD", {a, 1, b, 1, scalar, 4}));
    require(read_double(f, scalar) == 300, "vDSP_dotprD");
    const auto values = floats(f, {3, -9, 7, 1}), best = f.block(4), index = f.block(8);
    static_cast<void>(f.call(vdsp, "_vDSP_maxvi", {values, 1, best, index, 4}));
    require(read_float(f, best) == 7 && f.engine.read64(index) == 2, "vDSP_maxvi");
    static_cast<void>(f.call(vdsp, "_vDSP_maxmgvi", {values, 2, best, index, 2}));
    require(read_float(f, best) == 7 && f.engine.read64(index) == 2, "vDSP_maxmgvi: index times the stride");
    // int16 <-> float, rounding and saturating.
    const auto shorts = f.block(8), back = f.block(8), half = floats(f, {1.5f, -2.5f, 40000.0f, -3.7f}), converted = f.block(16);
    static_cast<void>(f.call(vdsp, "_vDSP_vfixr16", {half, 1, shorts, 1, 4}));
    static_cast<void>(f.call(vdsp, "_vDSP_vflt16", {shorts, 1, converted, 1, 4}));
    require(read_float(f, converted, 0) == 2 && read_float(f, converted, 1) == -2 && read_float(f, converted, 2) == 32767 && read_float(f, converted, 3) == -4,
            "vfixr16 / vflt16");
    static_cast<void>(f.call(vdsp, "_vDSP_vfix16", {half, 1, back, 1, 4}));
    static_cast<void>(f.call(vdsp, "_vDSP_vflt16", {back, 1, converted, 1, 4}));
    require(read_float(f, converted, 0) == 1 && read_float(f, converted, 3) == -3, "vfix16 truncates");
    // Windows, matrices.
    const auto window = f.block(16);
    static_cast<void>(f.call(vdsp, "_vDSP_hann_window", {window, 4, 0}));
    require(read_float(f, window, 0) == 0 && near(read_float(f, window, 2), 1), "Hann");
    const auto m1 = floats(f, {1, 2, 3, 4, 5, 6}), m2 = floats(f, {1, 0, 0, 1, 1, 1}), product = f.block(16);
    alchemy::CallRegs mmul{};
    mmul.x = {m1, 1, m2, 1, product, 1, 2, 2};
    mmul.push_stack(std::uint64_t{3});
    static_cast<void>(f.regs(vdsp, "_vDSP_mmul", mmul));
    require(read_float(f, product, 0) == 4 && read_float(f, product, 1) == 5 && read_float(f, product, 3) == 11, "vDSP_mmul (2x3 by 3x2)");
    static_cast<void>(f.call(vdsp, "_vDSP_mtrans", {m1, 1, product, 1, 3, 2}));
    require(read_float(f, product, 1) == 4 && read_float(f, product, 2) == 2, "vDSP_mtrans: [1 4; 2 5; 3 6]");
    // Complex FFT round trip (double, N = 8), and a DFT of 12 points against the plain sum.
    const auto re = doubles(f, {1, 2, 3, 4, 0, 0, 0, 0}), im = doubles(f, {0, 0, 0, 0, 0, 0, 0, 0}), split = f.block(16);
    f.engine.write64(split, re);
    f.engine.write64(split + 8, im);
    const auto setup = f.call(vdsp, "_vDSP_create_fftsetupD", {3, 0});
    require(setup != 0, "fft setup D");
    static_cast<void>(f.call(vdsp, "_vDSP_fft_zipD", {setup, split, 1, 3, 1}));
    require(read_double(f, re, 0) == 10 && near(read_double(f, im, 2), 2), "forward FFT: X[0] = 10, X[2] = -2 + 2i");
    static_cast<void>(f.call(vdsp, "_vDSP_fft_zipD", {setup, split, 1, 3, static_cast<Address>(-1)}));
    require(near(read_double(f, re, 1), 16) && near(read_double(f, im, 1), 0), "inverse: 8x the input");
    static_cast<void>(f.call(vdsp, "_vDSP_destroy_fftsetupD", {setup}));
    const auto dft = f.call(vdsp, "_vDSP_DFT_zop_CreateSetup", {0, 12, 1});
    require(dft != 0 && f.call(vdsp, "_vDSP_DFT_zop_CreateSetup", {0, 7, 1}) == 0, "DFT setups: 12 = 3 x 4, not 7");
    const auto ir = f.block(48), ii = f.block(48), or_ = f.block(48), oi = f.block(48);
    f.engine.write32(ir + 4, std::bit_cast<std::uint32_t>(1.0f));   // a unit impulse at 1
    static_cast<void>(f.call(vdsp, "_vDSP_DFT_Execute", {dft, ir, ii, or_, oi}));
    require(near(read_float(f, or_, 3), 0) && near(read_float(f, oi, 3), -1), "X[3] = e^{-2 pi i 3/12} = -i");
    static_cast<void>(f.call(vdsp, "_vDSP_DFT_DestroySetup", {dft}));
    // A biquad: two cascaded sections of y = x + 0.5 y[n-1] on an impulse.
    const auto coefficients = doubles(f, {1, 0, 0, -0.5, 0, 1, 0, 0, -0.5, 0});
    const auto biquad = f.call(vdsp, "_vDSP_biquad_CreateSetup", {coefficients, 2});
    const auto delay = f.block(24), impulse = floats(f, {1, 0, 0}), out = f.block(12);
    static_cast<void>(f.call(vdsp, "_vDSP_biquad", {biquad, delay, impulse, 1, out, 1, 3}));
    require(read_float(f, out, 0) == 1 && read_float(f, out, 1) == 1 && read_float(f, out, 2) == 0.75f, "biquad cascade: 1, 1, 0.75");
    static_cast<void>(f.call(vdsp, "_vDSP_biquad_DestroySetup", {biquad}));
    // vForce.
    const auto x = floats(f, {4, 9}), y = floats(f, {0.5f, 2}), z = f.block(8), n = count(f, 2);
    static_cast<void>(f.call(vdsp, "_vvsqrtf", {z, x, n}));
    require(read_float(f, z, 0) == 2 && read_float(f, z, 1) == 3, "vvsqrtf");
    static_cast<void>(f.call(vdsp, "_vvpowf", {z, y, x, n}));
    require(read_float(f, z, 0) == 2 && read_float(f, z, 1) == 81, "vvpowf: z = x^y");
    const auto xd = doubles(f, {0, 1.5707963267948966}), sines = f.block(16), cosines = f.block(16);
    static_cast<void>(f.call(vdsp, "_vvsincos", {sines, cosines, xd, n}));
    require(read_double(f, sines, 0) == 0 && near(read_double(f, sines, 1), 1) && read_double(f, cosines, 0) == 1, "vvsincos");
}

const Case accelerate_case("accelerate", [](Fixture& f) {
    const auto a = floats(f, {1, 2, 3, 4}), b = floats(f, {10, 20, 30, 40}), c = f.block(16);
    static_cast<void>(f.call(vdsp, "_vDSP_vadd", {a, 1, b, 1, c, 1, 4}));
    require(read_float(f, c, 3) == 44, "vDSP_vadd");
    static_cast<void>(f.call(vdsp, "_vDSP_vsub", {a, 1, b, 1, c, 1, 4}));
    require(read_float(f, c, 0) == 9, "vDSP_vsub computes A - B with B first");
    const auto scalar = floats(f, {0.5f});
    static_cast<void>(f.call(vdsp, "_vDSP_vsmul", {b, 1, scalar, c, 1, 4}));
    require(read_float(f, c, 1) == 10, "vDSP_vsmul");
    const auto low = floats(f, {2}), high = floats(f, {3});
    static_cast<void>(f.call(vdsp, "_vDSP_vclip", {a, 1, low, high, c, 1, 4}));
    require(read_float(f, c, 0) == 2 && read_float(f, c, 3) == 3, "vDSP_vclip");
    const auto signed_values = floats(f, {1, -7, 3}), result = f.block(4);
    static_cast<void>(f.call(vdsp, "_vDSP_maxmgv", {signed_values, 1, result, 3}));
    require(read_float(f, result) == 7, "vDSP_maxmgv");
    static_cast<void>(f.call(vdsp, "_vDSP_svesq", {a, 1, result, 4}));
    require(read_float(f, result) == 30, "vDSP_svesq");
    // Correlation of [1 2 3 4] with [1 1]: [3 5 7].
    const auto filter = floats(f, {1, 1}), conv = f.block(12);
    static_cast<void>(f.call(vdsp, "_vDSP_conv", {a, 1, filter, 1, conv, 1, 3, 2}));
    require(read_float(f, conv, 0) == 3 && read_float(f, conv, 2) == 7, "vDSP_conv");
    // Packed real FFT of [1 0 0 0 0 0 0 0] (N = 8): forward = 2 * DFT = 2 everywhere.
    const auto real = f.block(16), imag = f.block(16), split = f.block(16);
    f.engine.write64(split, real);
    f.engine.write64(split + 8, imag);
    const auto signal = floats(f, {1, 0, 0, 0, 0, 0, 0, 0});
    static_cast<void>(f.call(vdsp, "_vDSP_ctoz", {signal, 2, split, 1, 4}));
    const auto setup = f.call(vdsp, "_vDSP_create_fftsetup", {3, 0});
    require(setup != 0, "fft setup");
    static_cast<void>(f.call(vdsp, "_vDSP_fft_zrip", {setup, split, 1, 3, 1}));
    require(read_float(f, real, 0) == 2 && read_float(f, imag, 0) == 2 && std::fabs(read_float(f, real, 1) - 2) < 1e-6, "forward packed FFT");
    static_cast<void>(f.call(vdsp, "_vDSP_fft_zrip", {setup, split, 1, 3, static_cast<Address>(-1)}));
    const auto back = f.block(32);
    static_cast<void>(f.call(vdsp, "_vDSP_ztoc", {split, 1, back, 2, 4}));
    require(std::fabs(read_float(f, back, 0) - 16) < 1e-5 && std::fabs(read_float(f, back, 1)) < 1e-5, "inverse scales by 2N");
    static_cast<void>(f.call(vdsp, "_vDSP_destroy_fftsetup", {setup}));
    require(f.call(vdsp, "_vDSP_create_fftsetup", {3, 2}) == 0, "radix 5 unsupported");
    // zvmul with conjugate.
    const auto ar = floats(f, {1}), ai = floats(f, {2}), br = floats(f, {3}), bi = floats(f, {4}), cr = f.block(4), ci = f.block(4);
    const auto za = f.block(16), zb = f.block(16), zc = f.block(16);
    f.engine.write64(za, ar); f.engine.write64(za + 8, ai);
    f.engine.write64(zb, br); f.engine.write64(zb + 8, bi);
    f.engine.write64(zc, cr); f.engine.write64(zc + 8, ci);
    static_cast<void>(f.call(vdsp, "_vDSP_zvmul", {za, 1, zb, 1, zc, 1, 1, static_cast<Address>(static_cast<std::uint32_t>(-1))}));
    require(read_float(f, cr) == 11 && read_float(f, ci) == -2, "conj(1+2i)(3+4i) = 11-2i");
    // BNNS: no filter can be created (NULL, the documented failure), applying
    // one fails and only NULL may be destroyed.
    const auto descriptors = f.block(1024);
    for (const auto* create : {"_BNNSFilterCreateLayerConvolution", "_BNNSFilterCreateLayerPooling",
                               "_BNNSFilterCreateLayerFullyConnected", "_BNNSFilterCreateVectorActivation"})
        require(f.call(vdsp, create, {descriptors, descriptors + 256, descriptors + 512, 0}) == 0, create);
    require(f.call(vdsp, "_BNNSFilterApply", {0, descriptors, descriptors + 512}) == UINT64_MAX, "BNNSFilterApply fails");
    static_cast<void>(f.call(vdsp, "_BNNSFilterDestroy", {0}));
    vectors(f);
});

} // namespace
