// Accelerate (src/alchemy/darwin/accelerate/): the vDSP kernels and packed real FFT
// (vdsp.cpp), the wider vDSP API (vdsp_vectors.cpp), vForce (vforce.cpp), vImage (vimage.cpp), BLAS
// (blas.cpp), and BNNS, where no filter can be created (bnns.cpp).
#include "alchemy/gui/tests/framework_case.hpp"

#include <array>
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


// A vImage_Buffer over `bytes` guest bytes per row.
Address image_buffer(Fixture& f, Address data, std::uint64_t height, std::uint64_t width, std::uint64_t row_bytes) {
    const auto b = f.block(32);
    f.engine.write64(b, data);
    f.engine.write64(b + 8, height);
    f.engine.write64(b + 16, width);
    f.engine.write64(b + 24, row_bytes);
    return b;
}
std::int64_t verror(Address value) { return static_cast<std::int64_t>(value); }

// vImage (vimage.cpp): buffers, conversions, geometry, convolution, histograms.
void vimage(Fixture& f) {
    const auto made = f.block(32);
    require(verror(f.call(vdsp, "_vImageBuffer_Init", {made, 2, 3, 32, 0})) == 0 && f.engine.read64(made) != 0 && f.engine.read64(made + 24) == 16,
            "vImageBuffer_Init: rows of 16 bytes");
    const auto color = f.block(4);
    f.engine.write32(color, 0x1e140aff);   // A 255, R 10, G 20, B 30
    require(verror(f.call(vdsp, "_vImageBufferFill_ARGB8888", {made, color, 0})) == 0 && f.engine.read32(f.engine.read64(made) + 16 + 8) == 0x1e140aff,
            "vImageBufferFill_ARGB8888");
    const auto reversed = f.block(32), map = f.block(4);
    f.engine.write32(map, 0x00010203);   // {3, 2, 1, 0}
    const auto out = image_buffer(f, reversed, 2, 3, 16);
    require(verror(f.call(vdsp, "_vImagePermuteChannels_ARGB8888", {made, out, map, 0})) == 0 && f.engine.read32(reversed) == 0xff0a141e,
            "vImagePermuteChannels_ARGB8888");
    // Premultiplying (A 128, R 200, G 100, B 0).
    const auto pixel = f.block(4), premultiplied = f.block(4);
    f.engine.write32(pixel, 0x0064c880);
    require(verror(f.call(vdsp, "_vImagePremultiplyData_ARGB8888", {image_buffer(f, pixel, 1, 1, 4), image_buffer(f, premultiplied, 1, 1, 4), 0})) == 0 &&
                f.engine.read32(premultiplied) == 0x00326480, "premultiplied: 100, 50, 0");
    // Planar8: a 3x3 image, 90 in the middle.
    const auto planar = f.block(9), blurred = f.block(9), histogram = f.block(256 * 8);
    f.memory().write8(planar + 4, 90);
    const auto src = image_buffer(f, planar, 3, 3, 3), dst = image_buffer(f, blurred, 3, 3, 3);
    alchemy::CallRegs box{};
    box.x = {src, dst, 0, 0, 0, 3, 3, 0};
    box.push_stack(std::uint32_t{8});   // kvImageEdgeExtend
    require(verror(f.regs(vdsp, "_vImageBoxConvolve_Planar8", box).x0) == 0 && f.memory().read8(blurred + 4) == 10 && f.memory().read8(blurred) == 10,
            "box blur with extended edges");
    alchemy::CallRegs no_edge = box;
    no_edge.stack.clear();
    no_edge.push_stack(std::uint32_t{0});
    require(verror(f.regs(vdsp, "_vImageBoxConvolve_Planar8", no_edge).x0) == -21768, "an edge style is required");
    require(verror(f.call(vdsp, "_vImageHistogramCalculation_Planar8", {src, histogram, 0})) == 0 && f.engine.read64(histogram) == 8 &&
                f.engine.read64(histogram + 90 * 8) == 1, "histogram");
    // Geometry: a 3x2 Planar8 image rotated a quarter turn clockwise, and reflected.
    const auto wide = f.block(6), tall = f.block(6), mirror = f.block(6);
    for (int i = 0; i < 6; ++i) f.memory().write8(wide + i, static_cast<std::uint8_t>(i + 1));   // rows 1 2 3 / 4 5 6
    require(verror(f.call(vdsp, "_vImageRotate90_Planar8", {image_buffer(f, wide, 2, 3, 3), image_buffer(f, tall, 3, 2, 2), 3, 0, 0})) == 0 &&
                f.memory().read8(tall) == 4 && f.memory().read8(tall + 1) == 1 && f.memory().read8(tall + 5) == 3, "clockwise: rows 4 1 / 5 2 / 6 3");
    require(verror(f.call(vdsp, "_vImageHorizontalReflect_Planar8", {image_buffer(f, wide, 2, 3, 3), image_buffer(f, mirror, 2, 3, 3), 0})) == 0 &&
                f.memory().read8(mirror) == 3 && f.memory().read8(mirror + 5) == 4, "horizontal reflection");
    // Scaling a flat 4x4 image to 2x2 keeps its value.
    const auto flat = f.block(16, 100), small = f.block(4);
    require(verror(f.call(vdsp, "_vImageScale_Planar8", {image_buffer(f, flat, 4, 4, 4), image_buffer(f, small, 2, 2, 2), 0, 0})) == 0 &&
                f.memory().read8(small) == 100 && f.memory().read8(small + 3) == 100, "Lanczos scaling");
    // Planar8 <-> PlanarF over [0, 1].
    const auto floats_out = f.block(16), bytes_back = f.block(4), ramp = f.block(4);
    f.engine.write32(ramp, 0xff7f0000);   // 0, 0, 127, 255
    alchemy::CallRegs range{};
    range.x[0] = image_buffer(f, ramp, 1, 4, 4);
    range.x[1] = image_buffer(f, floats_out, 1, 4, 16);
    range.set_s(0, 1.0f);
    range.set_s(1, 0.0f);
    require(verror(f.regs(vdsp, "_vImageConvert_Planar8toPlanarF", range).x0) == 0 && read_float(f, floats_out, 3) == 1.0f, "Planar8 to PlanarF");
    range.x[0] = range.x[1];
    range.x[1] = image_buffer(f, bytes_back, 1, 4, 4);
    require(verror(f.regs(vdsp, "_vImageConvert_PlanarFtoPlanar8", range).x0) == 0 && f.engine.read32(bytes_back) == 0xff7f0000, "and back");
}

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

// Y'CbCr 4:2:0 tri-planar round trip, chunky/planar, and CVPixelBuffer copies (420v and BGRA).
void vimage_video(Fixture& f) {
    constexpr std::string_view cv = "/System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo";
    // BT.601 video range.
    const auto range = f.block(32);
    const std::array<std::uint32_t, 8> video{16, 128, 235, 240, 235, 16, 240, 16};
    for (unsigned i = 0; i < 8; ++i) f.engine.write32(range + 4 * i, video[i]);
    const auto to_ycc = f.block(128), to_rgb = f.block(128);
    require(verror(f.call(vdsp, "_vImageConvert_ARGBToYpCbCr_GenerateConversion",
                          {f.engine.read64(f.symbol(vdsp, "_kvImage_ARGBToYpCbCrMatrix_ITU_R_601_4")), range, to_ycc, 0, 0, 0})) == 0 &&
                verror(f.call(vdsp, "_vImageConvert_YpCbCrToARGB_GenerateConversion",
                              {f.engine.read64(f.symbol(vdsp, "_kvImage_YpCbCrToARGBMatrix_ITU_R_601_4")), range, to_rgb, 0, 0, 0})) == 0,
            "Y'CbCr conversions");
    // A 2x2 ARGB block of (255, 200, 100, 50) through 420Yp8_Cb8_Cr8 and back.
    const auto argb = f.block(16), back = f.block(16), y = f.block(4), cb = f.block(1), cr = f.block(1);
    for (unsigned i = 0; i < 4; ++i) f.engine.write32(argb + 4 * i, 0x3264c8ff);
    require(verror(f.call(vdsp, "_vImageConvert_ARGB8888To420Yp8_Cb8_Cr8",
                          {image_buffer(f, argb, 2, 2, 8), image_buffer(f, y, 2, 2, 2), image_buffer(f, cb, 1, 1, 1), image_buffer(f, cr, 1, 1, 1), to_ycc, 0, 0})) == 0,
            "ARGB8888To420Yp8_Cb8_Cr8");
    alchemy::CallRegs decode{};
    decode.x = {image_buffer(f, y, 2, 2, 2), image_buffer(f, cb, 1, 1, 1), image_buffer(f, cr, 1, 1, 1), image_buffer(f, back, 2, 2, 8), to_rgb, 0, 255, 0};
    require(verror(f.regs(vdsp, "_vImageConvert_420Yp8_Cb8_Cr8ToARGB8888", decode).x0) == 0, "420Yp8_Cb8_Cr8ToARGB8888");
    const auto near_byte = [&](Address at, unsigned want) { return std::abs(static_cast<int>(f.memory().read8(at)) - static_cast<int>(want)) <= 2; };
    require(near_byte(back + 1, 200) && near_byte(back + 2, 100) && near_byte(back + 3, 50) && f.memory().read8(back) == 255,
            "the round trip keeps the colour: " + std::to_string(f.memory().read8(back + 1)) + " " + std::to_string(f.memory().read8(back + 2)) + " " +
                std::to_string(f.memory().read8(back + 3)));
    // PlanarToChunky8: two 2x1 planes interleaved with a stride of 2.
    const auto planes = f.block(64), p0 = f.block(2), p1 = f.block(2), chunky = f.block(4), pointers = f.block(16);
    f.memory().write8(p0, 1); f.memory().write8(p0 + 1, 2); f.memory().write8(p1, 3); f.memory().write8(p1 + 1, 4);
    for (unsigned i = 0; i < 2; ++i) {
        const auto b = image_buffer(f, i ? p1 : p0, 1, 2, 2);
        for (unsigned w = 0; w < 4; ++w) f.engine.write64(planes + 32 * i + 8 * w, f.engine.read64(b + 8 * w));
    }
    f.engine.write64(pointers, chunky);
    f.engine.write64(pointers + 8, chunky + 1);
    require(verror(f.call(vdsp, "_vImageConvert_PlanarToChunky8", {planes, pointers, 2, 2, 2, 1, 4, 0})) == 0 && f.engine.read32(chunky) == 0x04020301,
            "PlanarToChunky8");
    // A 2x2 red RGBA buffer into a 420v CVPixelBuffer and back.
    const auto red = f.block(16), returned = f.block(32), rgba = f.block(40);
    for (unsigned i = 0; i < 4; ++i) f.engine.write32(red + 4 * i, 0xff0000ff);
    f.engine.write32(rgba, 8);
    f.engine.write32(rgba + 4, 32);
    f.engine.write32(rgba + 16, 3);   // alpha last
    const auto out = f.block(8);
    require(f.status(cv, "_CVPixelBufferCreate", {0, 2, 2, 0x34323076, 0, out}) == 0, "a 420v pixel buffer");
    const auto pixel_buffer = f.engine.read64(out);
    require(verror(f.call(vdsp, "_vImageBuffer_CopyToCVPixelBuffer", {image_buffer(f, red, 2, 2, 8), rgba, pixel_buffer, 0, 0, 0})) == 0,
            "vImageBuffer_CopyToCVPixelBuffer to 420v");
    require(verror(f.call(vdsp, "_vImageBuffer_InitWithCVPixelBuffer", {returned, rgba, pixel_buffer, 0, 0, 0})) == 0, "vImageBuffer_InitWithCVPixelBuffer from 420v");
    const auto data = f.engine.read64(returned);
    require(near_byte(data, 255) && near_byte(data + 1, 0) && near_byte(data + 2, 0) && f.memory().read8(data + 3) == 255,
            "red survives 420v: " + std::to_string(f.memory().read8(data)) + " " + std::to_string(f.memory().read8(data + 1)) + " " + std::to_string(f.memory().read8(data + 2)));
    const auto format = f.call(vdsp, "_vImageCVImageFormat_CreateWithCVPixelBuffer", {pixel_buffer});
    require(format && f.call(vdsp, "_vImageCVImageFormat_GetFormatCode", {format}) == 0x34323076, "vImageCVImageFormat");
    static_cast<void>(f.call(vdsp, "_vImageCVImageFormat_Release", {format}));
}

// vImage and CoreGraphics: a buffer to a CGImage and back in another layout, and a converter to gray.
void vimage_cg(Fixture& f) {
    constexpr std::string_view cg = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
    const auto rgb = f.call(cg, "_CGColorSpaceCreateDeviceRGB"), gray = f.call(cg, "_CGColorSpaceCreateDeviceGray");
    const auto format = [&](std::uint32_t bpc, std::uint32_t bpp, Address space, std::uint32_t info) {
        const auto at = f.block(40);
        f.engine.write32(at, bpc);
        f.engine.write32(at + 4, bpp);
        f.engine.write64(at + 8, space);
        f.engine.write32(at + 16, info);
        return at;
    };
    // RGBA, straight alpha last: opaque red, half-transparent green.
    const auto pixels = f.block(8);
    f.engine.write32(pixels, 0xff0000ff);
    f.engine.write32(pixels + 4, 0x8000ff00);
    const auto rgba = format(8, 32, rgb, 3);
    const auto error = f.block(8);
    const auto image = f.call(vdsp, "_vImageCreateCGImageFromBuffer", {image_buffer(f, pixels, 1, 2, 8), rgba, 0, 0, 0, error});
    require(image && f.engine.read64(error) == 0 && f.call(cg, "_CGImageGetWidth", {image}) == 2, "vImageCreateCGImageFromBuffer");
    // Back as BGRA premultiplied (alpha first, 32Little): green at half alpha is (0, 128, 0, 128).
    const auto bgra = format(8, 32, rgb, 2 | 0x2000);
    const auto made = f.block(32);
    require(verror(f.call(vdsp, "_vImageBuffer_InitWithCGImage", {made, bgra, 0, image, 0})) == 0 && f.engine.read64(made + 16) == 2, "vImageBuffer_InitWithCGImage");
    const auto data = f.engine.read64(made);
    require(f.engine.read32(data) == 0xffff0000 && f.engine.read32(data + 4) == 0x80008000,
            "the pixels in BGRA premultiplied: " + std::to_string(f.engine.read32(data)) + " " + std::to_string(f.engine.read32(data + 4)));
    // A converter to 8-bit gray with no alpha over a white background: red -> 54, green over white -> 219.
    const auto white = f.block(8);
    f.engine.write64(white, std::bit_cast<std::uint64_t>(1.0));
    const auto gray8 = format(8, 8, gray, 0);
    const auto converter = f.call(vdsp, "_vImageConverter_CreateWithCGImageFormat", {rgba, gray8, white, 0, error});
    const auto luma = f.block(2);
    require(converter && f.call(vdsp, "_vImageConverter_GetNumberOfSourceBuffers", {converter}) == 1 &&
                verror(f.call(vdsp, "_vImageConvert_AnyToAny", {converter, image_buffer(f, pixels, 1, 2, 8), image_buffer(f, luma, 1, 2, 2), 0, 0})) == 0 &&
                f.memory().read8(luma) == 54 && f.memory().read8(luma + 1) == 219,
            "vImageConvert_AnyToAny to gray: " + std::to_string(f.memory().read8(luma)) + " " + std::to_string(f.memory().read8(luma + 1)));
    static_cast<void>(f.call(vdsp, "_vImageConverter_Release", {converter}));
    require((f.call(vdsp, "_vImageCGImageFormat_IsEqual", {rgba, rgba}) & 0xff) == 1 && (f.call(vdsp, "_vImageCGImageFormat_IsEqual", {rgba, bgra}) & 0xff) == 0,
            "vImageCGImageFormat_IsEqual");
}

// vImage's added conversions, compositing, matrix multiply, morphology, rotation and scaling.
void vimage_more(Fixture& f) {
    // BGRA8888toRGB888: (B 1, G 2, R 3, A 4) -> (3, 2, 1).
    const auto bgra = f.block(4), rgb = f.block(4);
    f.engine.write32(bgra, 0x04030201);
    require(verror(f.call(vdsp, "_vImageConvert_BGRA8888toRGB888", {image_buffer(f, bgra, 1, 1, 4), image_buffer(f, rgb, 1, 1, 3), 0})) == 0 &&
                f.memory().read8(rgb) == 3 && f.memory().read8(rgb + 2) == 1, "vImageConvert_BGRA8888toRGB888");
    // Flattening a transparent ARGB pixel over white gives white.
    const auto clear = f.block(4), white = f.block(4), flat = f.block(4);
    f.engine.write32(clear, 0x33221100);   // A 0 (the first byte)
    f.engine.write32(white, 0xffffffff);
    require(verror(f.call(vdsp, "_vImageFlatten_ARGB8888ToRGB888", {image_buffer(f, clear, 1, 1, 4), image_buffer(f, flat, 1, 1, 3), white, 0, 0})) == 0 &&
                f.memory().read8(flat) == 255 && f.memory().read8(flat + 2) == 255, "vImageFlatten_ARGB8888ToRGB888");
    // MatrixMultiply: identity x 256 over a divisor of 256 keeps the pixel.
    const auto matrix = f.block(32), multiplied = f.block(4), pixel = f.block(4);
    for (unsigned i = 0; i < 4; ++i) { f.memory().write8(matrix + 2 * (i * 4 + i), 0); f.memory().write8(matrix + 2 * (i * 4 + i) + 1, 1); }
    f.engine.write32(pixel, 0x80402010);
    require(verror(f.call(vdsp, "_vImageMatrixMultiply_ARGB8888", {image_buffer(f, pixel, 1, 1, 4), image_buffer(f, multiplied, 1, 1, 4), matrix, 256, 0, 0, 0})) == 0 &&
                f.engine.read32(multiplied) == 0x80402010, "vImageMatrixMultiply_ARGB8888");
    // Max_Planar8 3x3 over a 3x3 image with 90 in the middle: everywhere 90.
    const auto planar = f.block(9), maximum = f.block(9);
    f.memory().write8(planar + 4, 90);
    require(verror(f.call(vdsp, "_vImageMax_Planar8", {image_buffer(f, planar, 3, 3, 3), image_buffer(f, maximum, 3, 3, 3), 0, 0, 0, 3, 3, 0})) == 0 &&
                f.memory().read8(maximum) == 90 && f.memory().read8(maximum + 8) == 90, "vImageMax_Planar8");
    // Rotate_ARGB8888 by pi swaps the two pixels of a 2x1 image.
    const auto pair = f.block(8), turned = f.block(8), background = f.block(4);
    f.engine.write32(pair, 0xff0000ff);
    f.engine.write32(pair + 4, 0xff00ff00);
    alchemy::CallRegs rotate{};
    rotate.x = {image_buffer(f, pair, 1, 2, 8), image_buffer(f, turned, 1, 2, 8), 0, background, 0};
    rotate.set_s(0, 3.14159265f);
    require(verror(f.regs(vdsp, "_vImageRotate_ARGB8888", rotate).x0) == 0 && f.engine.read32(turned) == 0xff00ff00 && f.engine.read32(turned + 4) == 0xff0000ff,
            "vImageRotate_ARGB8888 by pi");
    // Scale_Planar16U 1x1 -> 2x2 keeps the value; Planar16F 1.0 converts to 1.0f.
    const auto sixteen = f.block(2), scaled = f.block(8);
    f.memory().write8(sixteen, 0x34);
    f.memory().write8(sixteen + 1, 0x12);
    require(verror(f.call(vdsp, "_vImageScale_Planar16U", {image_buffer(f, sixteen, 1, 1, 2), image_buffer(f, scaled, 2, 2, 4), 0, 0})) == 0 &&
                (f.engine.read32(scaled + 4) & 0xffff) == 0x1234, "vImageScale_Planar16U");
    const auto half = f.block(2), single = f.block(4);
    f.memory().write8(half, 0x00);
    f.memory().write8(half + 1, 0x3c);
    require(verror(f.call(vdsp, "_vImageConvert_Planar16FtoPlanarF", {image_buffer(f, half, 1, 1, 2), image_buffer(f, single, 1, 1, 4), 0})) == 0 &&
                read_float(f, single) == 1.0f, "vImageConvert_Planar16FtoPlanarF");
}

// vdsp_more.cpp and the DCT: compound arithmetic, clipping counts, sorting, gathering, split
// complex division, the 2-D FFT, filters and integrals.
void vdsp_more(Fixture& f) {
    // vasm: (A + B) * c.
    const auto a = floats(f, {1, 2, 3}), b = floats(f, {3, 2, 1}), c = floats(f, {2}), out = floats(f, {0, 0, 0});
    static_cast<void>(f.call(vdsp, "_vDSP_vasm", {a, 1, b, 1, c, out, 1, 3}));
    require(read_float(f, out, 0) == 8 && read_float(f, out, 2) == 8, "vDSP_vasm");
    // vclipc into [1.5, 2.5]: one below, one above.
    const auto low = floats(f, {1.5f}), high = floats(f, {2.5f}), below = f.block(8), above = f.block(8);
    alchemy::CallRegs clip{};
    clip.x = {a, 1, low, high, out, 1, 3, below};
    clip.push_stack(above);
    static_cast<void>(f.regs(vdsp, "_vDSP_vclipc", clip));
    require(read_float(f, out, 0) == 1.5f && read_float(f, out, 2) == 2.5f && f.engine.read64(below) == 1 && f.engine.read64(above) == 1, "vDSP_vclipc");
    // vsort descending; vgathr with 1-based indices.
    const auto sortable = doubles(f, {2, 9, 4});
    static_cast<void>(f.call(vdsp, "_vDSP_vsortD", {sortable, 3, static_cast<std::uint64_t>(-1)}));
    require(read_double(f, sortable, 0) == 9 && read_double(f, sortable, 2) == 2, "vDSP_vsortD descending");
    const auto indices = f.block(16);
    f.engine.write64(indices, 3);
    f.engine.write64(indices + 8, 1);
    static_cast<void>(f.call(vdsp, "_vDSP_vgathr", {a, indices, 1, out, 1, 2}));
    require(read_float(f, out, 0) == 3 && read_float(f, out, 1) == 1, "vDSP_vgathr");
    // zvdiv (B first): (4 + 2i) / (1 + i) = 3 - i.
    const auto split = [&](float re, float im) {
        const auto s = f.block(16);
        f.engine.write64(s, floats(f, {re}));
        f.engine.write64(s + 8, floats(f, {im}));
        return s;
    };
    const auto zb = split(1, 1), za = split(4, 2), zc = split(0, 0);
    static_cast<void>(f.call(vdsp, "_vDSP_zvdiv", {zb, 1, za, 1, zc, 1, 1}));
    require(near(read_float(f, f.engine.read64(zc)), 3) && near(read_float(f, f.engine.read64(zc + 8)), -1), "vDSP_zvdiv");
    // fft2d_zip of a 2x2 grid of ones: 4 at DC, zeros elsewhere.
    const auto grid = f.block(16);
    f.engine.write64(grid, floats(f, {1, 1, 1, 1}));
    f.engine.write64(grid + 8, floats(f, {0, 0, 0, 0}));
    const auto setup = f.call(vdsp, "_vDSP_create_fftsetup", {2, 0});
    static_cast<void>(f.call(vdsp, "_vDSP_fft2d_zip", {setup, grid, 1, 0, 1, 1, 1}));
    const auto re = f.engine.read64(grid);
    require(read_float(f, re, 0) == 4 && read_float(f, re, 1) == 0 && read_float(f, re, 3) == 0, "vDSP_fft2d_zip");
    static_cast<void>(f.call(vdsp, "_vDSP_destroy_fftsetup", {setup}));
    // DCT-II of (1, 1, ..., 1) (16): 16 at k = 0, zero elsewhere.
    const auto dct = f.call(vdsp, "_vDSP_DCT_CreateSetup", {0, 16, 2});
    const auto ones = f.block(64), spectrum = f.block(64);
    for (int i = 0; i < 16; ++i) f.engine.write32(ones + 4ULL * i, std::bit_cast<std::uint32_t>(1.0f));
    static_cast<void>(f.call(vdsp, "_vDSP_DCT_Execute", {dct, ones, spectrum}));
    require(dct && near(read_float(f, spectrum, 0), 16) && near(read_float(f, spectrum, 5), 0, 1e-4), "vDSP_DCT_Execute type II");
    static_cast<void>(f.call(vdsp, "_vDSP_DFT_DestroySetup", {dct}));
    // f3x3 of a 3x3 image with a centre-only filter of 2: the centre doubles, the border is zero.
    const auto image = floats(f, {1, 2, 3, 4, 5, 6, 7, 8, 9}), kernel = floats(f, {0, 0, 0, 0, 2, 0, 0, 0, 0}), filtered = f.block(36);
    static_cast<void>(f.call(vdsp, "_vDSP_f3x3", {image, 3, 3, kernel, filtered}));
    require(read_float(f, filtered, 4) == 10 && read_float(f, filtered, 0) == 0, "vDSP_f3x3");
    // vtrapz of (0, 1, 2) with step 1: 0, 0.5, 2.
    const auto ramp = floats(f, {0, 1, 2}), step = floats(f, {1});
    static_cast<void>(f.call(vdsp, "_vDSP_vtrapz", {ramp, 1, step, out, 1, 3}));
    require(read_float(f, out, 1) == 0.5f && read_float(f, out, 2) == 2, "vDSP_vtrapz");
    // deq22 (float) as a pass-through: B = (1, 0, 0, 0, 0).
    const auto signal = floats(f, {0, 0, 5, 6}), coefficients = floats(f, {1, 0, 0, 0, 0}), filtered2 = floats(f, {0, 0, 0, 0});
    static_cast<void>(f.call(vdsp, "_vDSP_deq22", {signal, 1, coefficients, filtered2, 1, 2}));
    require(read_float(f, filtered2, 2) == 5 && read_float(f, filtered2, 3) == 6, "vDSP_deq22");
}

// BLAS: CBLAS in both storage orders and the four precisions (stack arguments packed as on Apple
// arm64), the Fortran and `$NEWLAPACK` names, and the legacy LAPACK aliases.
void blas(Fixture& f) {
    constexpr std::uint64_t row = 101, col = 102, no = 111, trans = 112, upper = 121, lower = 122, non_unit = 131, left = 141;
    // sgemm, row-major: [1 2 3; 4 5 6] x [1 2; 3 4; 5 6] = [22 28; 49 64], C = 2 A B + 1 C (C was ones).
    const auto a = floats(f, {1, 2, 3, 4, 5, 6}), b = floats(f, {1, 2, 3, 4, 5, 6}), c = floats(f, {1, 1, 1, 1});
    alchemy::CallRegs gemm{};
    gemm.x = {row, no, no, 2, 2, 3, a, 3};
    gemm.set_s(0, 2.0f);
    gemm.set_s(1, 1.0f);
    gemm.push_stack(b); gemm.push_stack(std::int32_t{2}); gemm.push_stack(c); gemm.push_stack(std::int32_t{2});
    static_cast<void>(f.regs(vdsp, "_cblas_sgemm", gemm));
    require(read_float(f, c, 0) == 45 && read_float(f, c, 1) == 57 && read_float(f, c, 2) == 99 && read_float(f, c, 3) == 129, "cblas_sgemm");
    // dgemv, column-major, transposed: A = [1 3; 2 4] stored by columns, y = A^T x for x = (1, 1).
    const auto ad = doubles(f, {1, 2, 3, 4}), x = doubles(f, {1, 1}), y = doubles(f, {0, 0});
    alchemy::CallRegs gemv{};
    gemv.x = {col, trans, 2, 2, ad, 2, x, 1};
    gemv.set_d(0, 1.0);
    gemv.set_d(1, 0.0);
    gemv.push_stack(y); gemv.push_stack(std::int32_t{1});
    static_cast<void>(f.regs(vdsp, "_cblas_dgemv", gemv));
    require(read_double(f, y, 0) == 3 && read_double(f, y, 1) == 7, "cblas_dgemv transposed");
    // zgemm 1x1: (1 + 2i)(3 - i) = 5 + 5i, alpha 1, beta 0.
    const auto za = doubles(f, {1, 2}), zb = doubles(f, {3, -1}), zc = doubles(f, {9, 9}), one = doubles(f, {1, 0}), zero = doubles(f, {0, 0});
    alchemy::CallRegs zgemm{};
    zgemm.x = {col, no, no, 1, 1, 1, one, za};
    zgemm.push_stack(std::int32_t{1}); zgemm.push_stack(zb); zgemm.push_stack(std::int32_t{1});
    zgemm.push_stack(zero); zgemm.push_stack(zc); zgemm.push_stack(std::int32_t{1});
    static_cast<void>(f.regs(vdsp, "_cblas_zgemm", zgemm));
    require(read_double(f, zc, 0) == 5 && read_double(f, zc, 1) == 5, "cblas_zgemm");
    // dtrsm: lower [2 0; 1 1] X = [2 4; 3 5] (column-major) gives X = [1 2; 2 3].
    const auto tri = doubles(f, {2, 1, 0, 1}), rhs = doubles(f, {2, 3, 4, 5});
    alchemy::CallRegs trsm{};
    trsm.x = {col, left, lower, no, non_unit, 2, 2, tri};
    trsm.set_d(0, 1.0);
    trsm.push_stack(std::int32_t{2}); trsm.push_stack(rhs); trsm.push_stack(std::int32_t{2});
    static_cast<void>(f.regs(vdsp, "_cblas_dtrsm", trsm));
    require(read_double(f, rhs, 0) == 1 && read_double(f, rhs, 1) == 2 && read_double(f, rhs, 2) == 2 && read_double(f, rhs, 3) == 3, "cblas_dtrsm");
    // dsyrk upper: C = A A^T for A = [1 2] (1x2) is 5 in the one-element triangle.
    const auto sa = doubles(f, {1, 2}), sc = doubles(f, {0});
    alchemy::CallRegs syrk{};
    syrk.x = {row, upper, no, 1, 2, sa, 2, sc};
    syrk.set_d(0, 1.0);
    syrk.set_d(1, 0.0);
    syrk.push_stack(std::int32_t{1});
    static_cast<void>(f.regs(vdsp, "_cblas_dsyrk", syrk));
    require(read_double(f, sc) == 5, "cblas_dsyrk");
    // Level 1: snrm2 (a float result), isamax (0-based), daxpy with a negative increment.
    const auto v = floats(f, {3, -4, 1});
    alchemy::CallRegs nrm2{};
    nrm2.x = {2, v, 1};
    require(near(f.regs(vdsp, "_cblas_snrm2", nrm2).s(0), 5.0), "cblas_snrm2");
    require(f.call(vdsp, "_cblas_isamax", {3, v, 1}) == 1, "cblas_isamax");
    const auto px = doubles(f, {1, 2}), py = doubles(f, {10, 20});
    alchemy::CallRegs axpy{};
    axpy.x = {2, px, static_cast<std::uint64_t>(-1), py, 1};
    axpy.set_d(0, 1.0);
    static_cast<void>(f.regs(vdsp, "_cblas_daxpy", axpy));
    require(read_double(f, py, 0) == 12 && read_double(f, py, 1) == 21, "cblas_daxpy backwards");
    // Fortran: sgemm_ by reference, column-major 1x1x1; ddot$NEWLAPACK; isamax$NEWLAPACK (1-based).
    const auto ta = f.block(1), fa = floats(f, {3}), fb = floats(f, {4}), fc = floats(f, {0}), falpha = floats(f, {1}), fbeta = floats(f, {0});
    f.memory().write8(ta, 'N');
    alchemy::CallRegs fortran{};
    fortran.x = {ta, ta, count(f, 1), count(f, 1), count(f, 1), falpha, fa, count(f, 1)};
    fortran.push_stack(fb); fortran.push_stack(count(f, 1)); fortran.push_stack(fbeta); fortran.push_stack(fc); fortran.push_stack(count(f, 1));
    static_cast<void>(f.regs(vdsp, "_sgemm_", fortran));
    require(read_float(f, fc) == 12, "sgemm_");
    alchemy::CallRegs ddot{};
    ddot.x = {count(f, 2), px, count(f, 1), px, count(f, 1)};
    require(f.regs(vdsp, "_ddot$NEWLAPACK", ddot).d(0) == 5, "ddot$NEWLAPACK");
    require(f.call(vdsp, "_isamax$NEWLAPACK", {count(f, 3), v, count(f, 1)}) == 2, "isamax$NEWLAPACK is 1-based");
    // The legacy LAPACK names are the `$NEWLAPACK` drivers.
    require(f.symbol(vdsp, "_sgetrf_") == f.symbol(vdsp, "_sgetrf$NEWLAPACK") && f.symbol(vdsp, "_zgesdd_") == f.symbol(vdsp, "_zgesdd$NEWLAPACK"),
            "legacy LAPACK aliases");
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
    vimage(f);
    blas(f);
    vdsp_more(f);
    vimage_more(f);
    vimage_cg(f);
    vimage_video(f);
});

} // namespace
