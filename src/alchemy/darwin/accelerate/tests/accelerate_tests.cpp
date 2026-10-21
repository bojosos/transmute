// Accelerate (src/alchemy/darwin/accelerate/): the vDSP kernels and packed real FFT
// (vdsp.cpp), and BNNS, where no filter can be created (bnns.cpp).
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
});

} // namespace
