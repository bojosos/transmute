// Linux host backend, area "gpu": image files for NSImage, NSBitmapImageRep, the pasteboard and
// ImageIO (include/alchemy/host/gpu.hpp), the counterpart of WIC on win32. PNG through libpng's
// simplified API, JPEG through TurboJPEG, TIFF through libtiff (pages are frames), GIF through
// giflib (frames as stored, not composited, like WIC's), BMP and ICO read here. Frame facts
// follow WIC's: resolution (pHYs, JFIF, TIFF tags), alpha, bits per component, GIF delays,
// EXIF orientation (the pixels stay as stored).
#include "alchemy/host/gpu.hpp"

#include <gif_lib.h>
#include <png.h>
#include <tiffio.h>
#include <turbojpeg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace alchemy::host::gpu {
namespace {

using Bytes = std::span<const std::uint8_t>;
constexpr std::uint32_t max_side = 32768;

enum class Kind { unknown, png, jpeg, tiff, gif, bmp, ico };

Kind sniff(Bytes b) {
    if (b.size() >= 8 && std::memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) == 0) return Kind::png;
    if (b.size() >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) return Kind::jpeg;
    if (b.size() >= 4 && (std::memcmp(b.data(), "II*\0", 4) == 0 || std::memcmp(b.data(), "MM\0*", 4) == 0)) return Kind::tiff;
    if (b.size() >= 6 && (std::memcmp(b.data(), "GIF87a", 6) == 0 || std::memcmp(b.data(), "GIF89a", 6) == 0)) return Kind::gif;
    if (b.size() >= 18 && b[0] == 'B' && b[1] == 'M') return Kind::bmp;
    if (b.size() >= 6 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0 && (b[4] | b[5] << 8) > 0) return Kind::ico;
    return Kind::unknown;
}

std::uint32_t be32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }
std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t le32(const std::uint8_t* p) { return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24; }
std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

// A frame whose file names no resolution reads as 96 dpi, as WIC reports it.
ImageFrame unresolved_frame() {
    ImageFrame frame;
    frame.dpi_x = frame.dpi_y = 96;
    return frame;
}

bool sane(std::uint32_t width, std::uint32_t height) { return width > 0 && height > 0 && width <= max_side && height <= max_side; }

// ---- PNG ----

Result<ImageFrame> png_frame(Bytes bytes, bool pixels) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) return Error::unsupported;
    ImageFrame frame = unresolved_frame();
    frame.width = image.width;
    frame.height = image.height;
    frame.has_alpha = (image.format & PNG_FORMAT_FLAG_ALPHA) != 0;
    if (bytes.size() >= 33) frame.bits_per_component = bytes[24];   // IHDR bit depth
    // pHYs: pixels per metre.
    for (std::size_t at = 8; at + 12 <= bytes.size();) {
        const auto length = be32(&bytes[at]);
        if (at + 12 + length > bytes.size()) break;
        if (std::memcmp(&bytes[at + 4], "pHYs", 4) == 0 && length == 9 && bytes[at + 16] == 1) {
            frame.dpi_x = be32(&bytes[at + 8]) * 0.0254;
            frame.dpi_y = be32(&bytes[at + 12]) * 0.0254;
        }
        if (std::memcmp(&bytes[at + 4], "IDAT", 4) == 0) break;
        at += 12 + length;
    }
    if (!sane(frame.width, frame.height)) {
        png_image_free(&image);
        return Error::out_of_range;
    }
    if (!pixels) {
        png_image_free(&image);
        return frame;
    }
    image.format = PNG_FORMAT_RGBA;
    frame.rgba.resize(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, frame.rgba.data(), 0, nullptr)) {
        png_image_free(&image);
        return Error::io;
    }
    return frame;
}

Result<std::vector<std::uint8_t>> png_encode(const Pixels& p) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = p.width;
    image.height = p.height;
    image.format = PNG_FORMAT_RGBA;
    png_alloc_size_t size = 0;
    if (!png_image_write_to_memory(&image, nullptr, &size, 0, p.rgba.data(), 0, nullptr)) return Error::io;
    std::vector<std::uint8_t> out(size);
    if (!png_image_write_to_memory(&image, out.data(), &size, 0, p.rgba.data(), 0, nullptr)) return Error::io;
    out.resize(size);
    return out;
}

// ---- JPEG ----

// JFIF density and EXIF orientation from the APPn segments.
void jpeg_facts(Bytes b, ImageFrame& frame) {
    for (std::size_t at = 2; at + 4 <= b.size() && b[at] == 0xff;) {
        const auto marker = b[at + 1];
        if (marker == 0xda || marker == 0xd9) break;   // start of scan, end of image
        const std::size_t length = be16(&b[at + 2]);
        if (length < 2 || at + 2 + length > b.size()) break;
        const auto* s = &b[at + 4];
        const std::size_t n = length - 2;
        if (marker == 0xe0 && n >= 12 && std::memcmp(s, "JFIF\0", 5) == 0) {
            const auto units = s[7];
            const double x = be16(s + 8), y = be16(s + 10);
            if (units == 1 && x > 0 && y > 0) { frame.dpi_x = x; frame.dpi_y = y; }
            if (units == 2 && x > 0 && y > 0) { frame.dpi_x = x * 2.54; frame.dpi_y = y * 2.54; }
            if ((units == 1 || units == 2) && (x == 0 || y == 0)) frame.dpi_x = frame.dpi_y = 72;   // as WIC reads a zero density
        } else if (marker == 0xe1 && n >= 14 && std::memcmp(s, "Exif\0\0", 6) == 0) {
            const auto* tiff = s + 6;
            const std::size_t size = n - 6;
            const bool little = tiff[0] == 'I';
            const auto u16 = [&](std::size_t o) { return little ? le16(tiff + o) : be16(tiff + o); };
            const auto u32 = [&](std::size_t o) { return little ? le32(tiff + o) : be32(tiff + o); };
            const std::size_t ifd = u32(4);
            if (ifd + 2 <= size) {
                const std::size_t count = u16(ifd);
                for (std::size_t i = 0; i < count && ifd + 2 + (i + 1) * 12 <= size; ++i) {
                    const auto entry = ifd + 2 + i * 12;
                    if (u16(entry) == 0x112) frame.orientation = u16(entry + 8);
                }
            }
        }
        at += 2 + length;
    }
}

struct TurboHandle {
    tjhandle handle{};
    explicit TurboHandle(tjhandle h) : handle(h) {}
    ~TurboHandle() { if (handle) tjDestroy(handle); }
};

Result<ImageFrame> jpeg_frame(Bytes bytes, bool pixels) {
    TurboHandle tj(tjInitDecompress());
    if (!tj.handle) return Error::unsupported;
    int width{}, height{}, subsampling{}, colorspace{};
    if (tjDecompressHeader3(tj.handle, bytes.data(), static_cast<unsigned long>(bytes.size()), &width, &height, &subsampling, &colorspace) != 0)
        return Error::unsupported;
    ImageFrame frame = unresolved_frame();
    frame.width = static_cast<std::uint32_t>(width);
    frame.height = static_cast<std::uint32_t>(height);
    jpeg_facts(bytes, frame);
    if (!sane(frame.width, frame.height)) return Error::out_of_range;
    if (!pixels) return frame;
    frame.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    const bool cmyk = colorspace == TJCS_CMYK || colorspace == TJCS_YCCK;
    if (!cmyk) {
        if (tjDecompress2(tj.handle, bytes.data(), static_cast<unsigned long>(bytes.size()), frame.rgba.data(), width, width * 4, height, TJPF_RGBA, 0) != 0 &&
            tjGetErrorCode(tj.handle) == TJERR_FATAL)
            return Error::io;
    } else {
        // CMYK as Adobe writes it (inverted): R = C * K / 255 (no colour management).
        std::vector<std::uint8_t> cmyk_pixels(static_cast<std::size_t>(width) * height * 4);
        if (tjDecompress2(tj.handle, bytes.data(), static_cast<unsigned long>(bytes.size()), cmyk_pixels.data(), width, width * 4, height, TJPF_CMYK, 0) != 0 &&
            tjGetErrorCode(tj.handle) == TJERR_FATAL)
            return Error::io;
        for (std::size_t i = 0; i < cmyk_pixels.size(); i += 4) {
            const int k = cmyk_pixels[i + 3];
            for (int c = 0; c < 3; ++c) frame.rgba[i + c] = static_cast<std::uint8_t>(cmyk_pixels[i + c] * k / 255);
        }
    }
    for (std::size_t at = 3; at < frame.rgba.size(); at += 4) frame.rgba[at] = 0xff;
    return frame;
}

Result<std::vector<std::uint8_t>> jpeg_encode(const Pixels& p, double quality) {
    TurboHandle tj(tjInitCompress());
    if (!tj.handle) return Error::unsupported;
    unsigned char* out{};
    unsigned long size = 0;
    const int q = std::clamp(static_cast<int>(std::lround(quality * 100)), 1, 100);
    if (tjCompress2(tj.handle, p.rgba.data(), static_cast<int>(p.width), static_cast<int>(p.width * 4), static_cast<int>(p.height), TJPF_RGBA, &out, &size,
                    TJSAMP_420, q, 0) != 0) {
        if (out) tjFree(out);
        return Error::io;
    }
    std::vector<std::uint8_t> bytes(out, out + size);
    tjFree(out);
    return bytes;
}

// ---- TIFF ----

struct Memory {
    std::vector<std::uint8_t> data;
    std::size_t position{};
};

tmsize_t memory_read(thandle_t h, void* buffer, tmsize_t size) {
    auto& m = *static_cast<Memory*>(h);
    const auto n = std::min<std::size_t>(static_cast<std::size_t>(size), m.data.size() - std::min(m.position, m.data.size()));
    std::memcpy(buffer, m.data.data() + m.position, n);
    m.position += n;
    return static_cast<tmsize_t>(n);
}
tmsize_t memory_write(thandle_t h, void* buffer, tmsize_t size) {
    auto& m = *static_cast<Memory*>(h);
    const auto n = static_cast<std::size_t>(size);
    if (m.position + n > m.data.size()) m.data.resize(m.position + n);
    std::memcpy(m.data.data() + m.position, buffer, n);
    m.position += n;
    return size;
}
toff_t memory_seek(thandle_t h, toff_t offset, int whence) {
    auto& m = *static_cast<Memory*>(h);
    const auto base = whence == SEEK_CUR ? m.position : whence == SEEK_END ? m.data.size() : 0;
    m.position = static_cast<std::size_t>(base + offset);
    return m.position;
}
int memory_close(thandle_t) { return 0; }
toff_t memory_size(thandle_t h) { return static_cast<Memory*>(h)->data.size(); }
int no_map(thandle_t, void**, toff_t*) { return 0; }
void no_unmap(thandle_t, void*, toff_t) {}

struct Tiff {
    Memory memory;
    TIFF* tif{};
    ~Tiff() { if (tif) TIFFClose(tif); }
};

// libtiff's warnings and errors stay quiet: a failure is reported by the result.
void quiet_tiff() {
    static const bool done = [] {
        TIFFSetErrorHandler(nullptr);
        TIFFSetWarningHandler(nullptr);
        return true;
    }();
    static_cast<void>(done);
}

bool open_tiff(Tiff& t, Bytes bytes) {
    quiet_tiff();
    t.memory.data.assign(bytes.begin(), bytes.end());
    t.tif = TIFFClientOpen("memory", "rm", &t.memory, memory_read, memory_write, memory_seek, memory_close, memory_size, no_map, no_unmap);
    return t.tif != nullptr;
}

Result<std::size_t> tiff_count(Bytes bytes) {
    Tiff t;
    if (!open_tiff(t, bytes)) return Error::unsupported;
    return static_cast<std::size_t>(TIFFNumberOfDirectories(t.tif));
}

Result<ImageFrame> tiff_frame(Bytes bytes, std::size_t index, bool pixels) {
    Tiff t;
    if (!open_tiff(t, bytes)) return Error::unsupported;
    if (index >= static_cast<std::size_t>(TIFFNumberOfDirectories(t.tif))) return Error::out_of_range;
    if (!TIFFSetDirectory(t.tif, static_cast<tdir_t>(index))) return Error::io;
    std::uint32_t width{}, height{};
    std::uint16_t bits = 8, samples = 1, extra_count = 0, unit = RESUNIT_INCH, orientation = ORIENTATION_TOPLEFT;
    std::uint16_t* extra{};
    float x_resolution{}, y_resolution{};
    TIFFGetField(t.tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(t.tif, TIFFTAG_IMAGELENGTH, &height);
    TIFFGetFieldDefaulted(t.tif, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(t.tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetFieldDefaulted(t.tif, TIFFTAG_EXTRASAMPLES, &extra_count, &extra);
    TIFFGetFieldDefaulted(t.tif, TIFFTAG_RESOLUTIONUNIT, &unit);
    TIFFGetFieldDefaulted(t.tif, TIFFTAG_ORIENTATION, &orientation);
    ImageFrame frame = unresolved_frame();
    frame.width = width;
    frame.height = height;
    frame.bits_per_component = bits;
    frame.has_alpha = extra_count > 0;
    frame.orientation = orientation >= 1 && orientation <= 8 ? orientation : 1;
    if (TIFFGetField(t.tif, TIFFTAG_XRESOLUTION, &x_resolution) && TIFFGetField(t.tif, TIFFTAG_YRESOLUTION, &y_resolution) && x_resolution > 0 &&
        y_resolution > 0 && unit != RESUNIT_NONE) {
        const double scale = unit == RESUNIT_CENTIMETER ? 2.54 : 1;
        frame.dpi_x = x_resolution * scale;
        frame.dpi_y = y_resolution * scale;
    }
    if (!sane(width, height)) return Error::out_of_range;
    if (!pixels) return frame;
    // 8-bit RGB and RGBA scanlines are read as stored (colours under zero
    // alpha kept, as WIC keeps them); every other layout through libtiff's
    // RGBA interface.
    std::uint16_t photometric{}, planar = PLANARCONFIG_CONTIG;
    TIFFGetField(t.tif, TIFFTAG_PHOTOMETRIC, &photometric);
    TIFFGetFieldDefaulted(t.tif, TIFFTAG_PLANARCONFIG, &planar);
    const bool associated = extra_count > 0 && extra[0] == EXTRASAMPLE_ASSOCALPHA;
    if (photometric == PHOTOMETRIC_RGB && bits == 8 && planar == PLANARCONFIG_CONTIG && (samples == 3 || samples == 4) && !TIFFIsTiled(t.tif)) {
        std::vector<std::uint8_t> line(static_cast<std::size_t>(TIFFScanlineSize(t.tif)));
        if (line.size() < static_cast<std::size_t>(width) * samples) return Error::io;
        frame.rgba.resize(static_cast<std::size_t>(width) * height * 4);
        for (std::uint32_t row = 0; row < height; ++row) {
            if (TIFFReadScanline(t.tif, line.data(), row, 0) < 0) return Error::io;
            auto* out = &frame.rgba[static_cast<std::size_t>(row) * width * 4];
            for (std::uint32_t x = 0; x < width; ++x, out += 4) {
                const auto* in = &line[static_cast<std::size_t>(x) * samples];
                const int a = samples == 4 ? in[3] : 255;
                for (int c = 0; c < 3; ++c)
                    out[c] = associated && a > 0 && a < 255 ? static_cast<std::uint8_t>(std::min(255, (in[c] * 255 + a / 2) / a)) : in[c];
                out[3] = frame.has_alpha ? static_cast<std::uint8_t>(a) : 0xff;
            }
        }
        return frame;
    }
    std::vector<std::uint32_t> raster(static_cast<std::size_t>(width) * height);
    // The pixels as stored, top row first (the orientation is reported, not applied).
    if (!TIFFReadRGBAImageOriented(t.tif, width, height, raster.data(), ORIENTATION_TOPLEFT, 0)) return Error::io;
    frame.rgba.resize(raster.size() * 4);
    for (std::size_t i = 0; i < raster.size(); ++i) {
        const auto v = raster[i];
        auto r = static_cast<std::uint8_t>(TIFFGetR(v)), g = static_cast<std::uint8_t>(TIFFGetG(v)), b = static_cast<std::uint8_t>(TIFFGetB(v));
        const auto a = static_cast<std::uint8_t>(TIFFGetA(v));
        // libtiff's RGBA raster is premultiplied; frames are straight.
        if (frame.has_alpha && a > 0 && a < 255) {
            r = static_cast<std::uint8_t>(std::min(255, (r * 255 + a / 2) / a));
            g = static_cast<std::uint8_t>(std::min(255, (g * 255 + a / 2) / a));
            b = static_cast<std::uint8_t>(std::min(255, (b * 255 + a / 2) / a));
        }
        frame.rgba[i * 4] = r;
        frame.rgba[i * 4 + 1] = g;
        frame.rgba[i * 4 + 2] = b;
        frame.rgba[i * 4 + 3] = frame.has_alpha ? a : 0xff;
    }
    return frame;
}

Result<std::vector<std::uint8_t>> tiff_encode(std::span<const Pixels> frames) {
    quiet_tiff();
    Memory memory;
    TIFF* tif = TIFFClientOpen("memory", "w", &memory, memory_read, memory_write, memory_seek, memory_close, memory_size, no_map, no_unmap);
    if (!tif) return Error::io;
    for (const auto& p : frames) {
        const std::uint16_t extra[] = {EXTRASAMPLE_UNASSALPHA};
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, p.width);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, p.height);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, extra);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
        TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
        TIFFSetField(tif, TIFFTAG_XRESOLUTION, 72.0f);
        TIFFSetField(tif, TIFFTAG_YRESOLUTION, 72.0f);
        TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
        for (std::uint32_t row = 0; row < p.height; ++row)
            if (TIFFWriteScanline(tif, const_cast<std::uint8_t*>(p.rgba.data()) + static_cast<std::size_t>(row) * p.width * 4, row, 0) < 0) {
                TIFFClose(tif);
                return Error::io;
            }
        if (!TIFFWriteDirectory(tif)) {
            TIFFClose(tif);
            return Error::io;
        }
    }
    TIFFClose(tif);
    return std::move(memory.data);
}

// ---- GIF ----

struct GifReader {
    Bytes bytes;
    std::size_t position{};
};
int gif_read(GifFileType* gif, GifByteType* buffer, int size) {
    auto& r = *static_cast<GifReader*>(gif->UserData);
    const auto n = std::min<std::size_t>(static_cast<std::size_t>(size), r.bytes.size() - r.position);
    std::memcpy(buffer, r.bytes.data() + r.position, n);
    r.position += n;
    return static_cast<int>(n);
}

struct Gif {
    GifReader reader;
    GifFileType* gif{};
    ~Gif() {
        int error{};
        if (gif) DGifCloseFile(gif, &error);
    }
};

bool open_gif(Gif& g, Bytes bytes) {
    g.reader.bytes = bytes;
    int error{};
    g.gif = DGifOpen(&g.reader, gif_read, &error);
    if (!g.gif) return false;
    // A truncated last frame still leaves the frames read before it.
    return DGifSlurp(g.gif) == GIF_OK || g.gif->ImageCount > 0;
}

Result<ImageFrame> gif_frame(Bytes bytes, std::size_t index, bool pixels) {
    Gif g;
    if (!open_gif(g, bytes)) return Error::unsupported;
    if (index >= static_cast<std::size_t>(g.gif->ImageCount)) return Error::out_of_range;
    const auto& image = g.gif->SavedImages[index];
    const auto& desc = image.ImageDesc;
    ImageFrame frame = unresolved_frame();
    frame.width = static_cast<std::uint32_t>(desc.Width);
    frame.height = static_cast<std::uint32_t>(desc.Height);
    GraphicsControlBlock control{};
    control.TransparentColor = NO_TRANSPARENT_COLOR;
    const bool has_control = DGifSavedExtensionToGCB(g.gif, static_cast<int>(index), &control) == GIF_OK;
    if (has_control) frame.delay = control.DelayTime / 100.0;
    // Alpha where the frame has a transparent index (WIC reports none and
    // paints those pixels in their palette colour; macOS keeps them clear).
    frame.has_alpha = has_control && control.TransparentColor != NO_TRANSPARENT_COLOR;
    if (!sane(frame.width, frame.height)) return Error::out_of_range;
    if (!pixels) return frame;
    const ColorMapObject* map = desc.ColorMap ? desc.ColorMap : g.gif->SColorMap;
    if (!map || !image.RasterBits) return Error::io;
    frame.rgba.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(frame.width) * frame.height; ++i) {
        const int value = image.RasterBits[i];
        if (has_control && value == control.TransparentColor) continue;   // transparent black
        if (value >= map->ColorCount) continue;
        const auto& c = map->Colors[value];
        frame.rgba[i * 4] = c.Red;
        frame.rgba[i * 4 + 1] = c.Green;
        frame.rgba[i * 4 + 2] = c.Blue;
        frame.rgba[i * 4 + 3] = 0xff;
    }
    return frame;
}

int gif_write(GifFileType* gif, const GifByteType* buffer, int size) {
    auto& out = *static_cast<std::vector<std::uint8_t>*>(gif->UserData);
    out.insert(out.end(), buffer, buffer + size);
    return size;
}

Result<std::vector<std::uint8_t>> gif_encode(std::span<const Pixels> frames) {
    std::vector<std::uint8_t> out;
    int error{};
    GifFileType* gif = EGifOpen(&out, gif_write, &error);
    if (!gif) return Error::io;
    const auto fail = [&] {
        EGifCloseFile(gif, &error);
        return Error::io;
    };
    std::uint32_t screen_width = 0, screen_height = 0;
    for (const auto& p : frames) {
        screen_width = std::max(screen_width, p.width);
        screen_height = std::max(screen_height, p.height);
    }
    EGifSetGifVersion(gif, true);
    if (EGifPutScreenDesc(gif, static_cast<int>(screen_width), static_cast<int>(screen_height), 8, 0, nullptr) != GIF_OK) return fail();
    for (const auto& p : frames) {
        const std::size_t count = static_cast<std::size_t>(p.width) * p.height;
        bool transparent = false;
        std::vector<GifByteType> red(count), green(count), blue(count), indices(count);
        for (std::size_t i = 0; i < count; ++i) {
            red[i] = p.rgba[i * 4];
            green[i] = p.rgba[i * 4 + 1];
            blue[i] = p.rgba[i * 4 + 2];
            transparent |= p.rgba[i * 4 + 3] < 128;
        }
        // A median-cut palette (255 colours when one index is kept for transparency).
        int colours = transparent ? 255 : 256;
        GifColorType palette[256]{};
        if (GifQuantizeBuffer(p.width, p.height, &colours, red.data(), green.data(), blue.data(), indices.data(), palette) != GIF_OK)
            return fail();
        int transparent_index = NO_TRANSPARENT_COLOR;
        if (transparent) {
            transparent_index = 255;
            for (std::size_t i = 0; i < count; ++i)
                if (p.rgba[i * 4 + 3] < 128) indices[i] = 255;
        }
        GraphicsControlBlock control{};
        control.DisposalMode = DISPOSAL_UNSPECIFIED;
        control.TransparentColor = transparent_index;
        GifByteType extension[4];
        const auto length = EGifGCBToExtension(&control, extension);
        if (EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, static_cast<int>(length), extension) != GIF_OK) return fail();
        ColorMapObject* map = GifMakeMapObject(256, palette);
        if (!map) return fail();
        const bool placed = EGifPutImageDesc(gif, 0, 0, static_cast<int>(p.width), static_cast<int>(p.height), false, map) == GIF_OK;
        GifFreeMapObject(map);
        if (!placed) return fail();
        for (std::uint32_t row = 0; row < p.height; ++row)
            if (EGifPutLine(gif, indices.data() + static_cast<std::size_t>(row) * p.width, static_cast<int>(p.width)) != GIF_OK)
                return fail();
    }
    if (EGifCloseFile(gif, &error) != GIF_OK) return Error::io;
    return out;
}

// ---- BMP and ICO (device-independent bitmaps) ----

// A DIB (BITMAPINFOHEADER or later) at `dib`; in an icon the height is
// doubled and an AND mask follows the colours.
Result<ImageFrame> dib_frame(Bytes file, std::size_t dib, std::optional<std::size_t> pixel_offset, bool icon, bool pixels) {
    if (dib + 16 > file.size()) return Error::unsupported;
    const auto* h = file.data() + dib;
    const std::uint32_t header = le32(h);
    if (header < 40 || dib + header > file.size()) return Error::unsupported;
    const auto width = static_cast<std::int32_t>(le32(h + 4));
    auto height = static_cast<std::int32_t>(le32(h + 8));
    const int bits = le16(h + 14);
    const std::uint32_t compression = le32(h + 16);
    if (icon) height /= 2;
    const bool top_down = height < 0;
    height = std::abs(height);
    if (width <= 0 || height == 0) return Error::unsupported;
    ImageFrame frame = unresolved_frame();
    frame.width = static_cast<std::uint32_t>(width);
    frame.height = static_cast<std::uint32_t>(height);
    frame.bits_per_component = bits >= 24 ? 8 : static_cast<std::uint32_t>(bits);
    const std::int32_t x_ppm = static_cast<std::int32_t>(le32(h + 24)), y_ppm = static_cast<std::int32_t>(le32(h + 28));
    if (x_ppm > 0 && y_ppm > 0) {
        frame.dpi_x = x_ppm * 0.0254;
        frame.dpi_y = y_ppm * 0.0254;
    }
    std::uint32_t masks[4] = {0x00ff0000, 0x0000ff00, 0x000000ff, 0};
    if (compression == 3 || compression == 6) {   // BI_BITFIELDS, BI_ALPHABITFIELDS
        const std::size_t at = header >= 52 ? dib + 40 : dib + header;
        if (at + 12 > file.size()) return Error::unsupported;
        for (int i = 0; i < 3; ++i) masks[i] = le32(&file[at + i * 4]);
        if ((header >= 56 || compression == 6) && at + 16 <= file.size()) masks[3] = le32(&file[at + 12]);
    } else if (compression != 0) {
        return Error::unsupported;   // RLE and embedded JPEG/PNG bitmaps are not read
    } else if (bits == 32 && icon) {
        masks[3] = 0xff000000;
    }
    if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) return Error::unsupported;
    frame.has_alpha = bits == 32 && (masks[3] != 0 || icon);
    if (bits == 16 && compression == 0) {
        masks[0] = 0x7c00;
        masks[1] = 0x03e0;
        masks[2] = 0x001f;
    }
    if (!sane(frame.width, frame.height)) return Error::out_of_range;
    if (!pixels) return frame;
    std::uint32_t colours_used = le32(h + 32);
    if (bits <= 8 && colours_used == 0) colours_used = 1U << bits;
    const std::size_t palette_at = dib + header + ((compression == 3 || compression == 6) && header < 52 ? (compression == 6 ? 16 : 12) : 0);
    const std::size_t stride = ((static_cast<std::size_t>(width) * bits + 31) / 32) * 4;
    const std::size_t data_at = pixel_offset ? *pixel_offset : palette_at + (bits <= 8 ? colours_used * 4 : 0);
    if (data_at + stride * height > file.size()) return Error::io;
    if (bits <= 8 && palette_at + colours_used * 4 > file.size()) return Error::io;
    const auto channel = [](std::uint32_t value, std::uint32_t mask) -> std::uint8_t {
        if (!mask) return 0;
        int shift = 0;
        while (!((mask >> shift) & 1)) ++shift;
        const std::uint32_t max = mask >> shift;
        return static_cast<std::uint8_t>(((value & mask) >> shift) * 255 / max);
    };
    frame.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    bool any_alpha = false;
    for (std::int32_t y = 0; y < height; ++y) {
        const auto* row = &file[data_at + stride * static_cast<std::size_t>(top_down ? y : height - 1 - y)];
        auto* out = &frame.rgba[static_cast<std::size_t>(y) * width * 4];
        for (std::int32_t x = 0; x < width; ++x, out += 4) {
            if (bits <= 8) {
                const int per_byte = 8 / bits;
                const int index = (row[x / per_byte] >> ((per_byte - 1 - x % per_byte) * bits)) & ((1 << bits) - 1);
                if (static_cast<std::uint32_t>(index) < colours_used) {
                    const auto* c = &file[palette_at + static_cast<std::size_t>(index) * 4];
                    out[0] = c[2];
                    out[1] = c[1];
                    out[2] = c[0];
                }
                out[3] = 0xff;
            } else if (bits == 24) {
                out[0] = row[x * 3 + 2];
                out[1] = row[x * 3 + 1];
                out[2] = row[x * 3];
                out[3] = 0xff;
            } else {
                const std::uint32_t v = bits == 16 ? le16(row + x * 2) : le32(row + x * 4);
                out[0] = channel(v, masks[0]);
                out[1] = channel(v, masks[1]);
                out[2] = channel(v, masks[2]);
                out[3] = masks[3] ? channel(v, masks[3]) : 0xff;
                any_alpha |= masks[3] && out[3] != 0;
            }
        }
    }
    // A 32-bit icon without alpha values, or any icon below 32 bits: the AND mask is its transparency.
    if (icon && (bits < 32 || !any_alpha)) {
        const std::size_t mask_stride = ((static_cast<std::size_t>(width) + 31) / 32) * 4;
        const std::size_t mask_at = data_at + stride * height;
        if (mask_at + mask_stride * height <= file.size()) {
            for (std::int32_t y = 0; y < height; ++y) {
                const auto* row = &file[mask_at + mask_stride * static_cast<std::size_t>(height - 1 - y)];
                for (std::int32_t x = 0; x < width; ++x)
                    frame.rgba[(static_cast<std::size_t>(y) * width + x) * 4 + 3] = (row[x / 8] >> (7 - x % 8)) & 1 ? 0 : 0xff;
            }
            frame.has_alpha = true;
        } else if (bits == 32) {
            for (std::size_t at = 3; at < frame.rgba.size(); at += 4) frame.rgba[at] = 0xff;
        }
    }
    if (!frame.has_alpha)
        for (std::size_t at = 3; at < frame.rgba.size(); at += 4) frame.rgba[at] = 0xff;
    return frame;
}

Result<ImageFrame> bmp_frame(Bytes bytes, bool pixels) {
    if (bytes.size() < 26) return Error::unsupported;
    return dib_frame(bytes, 14, static_cast<std::size_t>(le32(&bytes[10])), false, pixels);
}

std::size_t ico_count(Bytes bytes) { return le16(&bytes[4]); }

Result<ImageFrame> ico_frame(Bytes bytes, std::size_t index, bool pixels) {
    const auto count = ico_count(bytes);
    if (index >= count) return Error::out_of_range;
    const std::size_t entry = 6 + index * 16;
    if (entry + 16 > bytes.size()) return Error::io;
    const std::size_t size = le32(&bytes[entry + 8]), offset = le32(&bytes[entry + 12]);
    if (offset >= bytes.size() || size > bytes.size() - offset) return Error::io;
    const auto image = bytes.subspan(offset, size);
    if (sniff(image) == Kind::png) return png_frame(image, pixels);
    return dib_frame(image, 0, std::nullopt, true, pixels);
}

// BMP with a BITMAPV4HEADER: 32-bit BGRA bit fields, so the alpha survives.
std::vector<std::uint8_t> bmp_encode(const Pixels& p) {
    const std::size_t header = 108, data = static_cast<std::size_t>(p.width) * p.height * 4;
    std::vector<std::uint8_t> out(14 + header + data);
    const auto put16 = [&](std::size_t at, std::uint32_t v) { out[at] = static_cast<std::uint8_t>(v); out[at + 1] = static_cast<std::uint8_t>(v >> 8); };
    const auto put32 = [&](std::size_t at, std::uint32_t v) { put16(at, v & 0xffff); put16(at + 2, v >> 16); };
    out[0] = 'B';
    out[1] = 'M';
    put32(2, static_cast<std::uint32_t>(out.size()));
    put32(10, static_cast<std::uint32_t>(14 + header));
    put32(14, static_cast<std::uint32_t>(header));
    put32(18, p.width);
    put32(22, p.height);   // bottom-up
    put16(26, 1);
    put16(28, 32);
    put32(30, 3);          // BI_BITFIELDS
    put32(34, static_cast<std::uint32_t>(data));
    put32(38, 2835);       // 72 dpi
    put32(42, 2835);
    put32(54, 0x00ff0000);
    put32(58, 0x0000ff00);
    put32(62, 0x000000ff);
    put32(66, 0xff000000);
    put32(70, 0x73524742);  // LCS_sRGB
    for (std::uint32_t y = 0; y < p.height; ++y) {
        const auto* source = &p.rgba[static_cast<std::size_t>(p.height - 1 - y) * p.width * 4];
        auto* target = &out[14 + header + static_cast<std::size_t>(y) * p.width * 4];
        for (std::uint32_t x = 0; x < p.width; ++x) {
            target[x * 4] = source[x * 4 + 2];
            target[x * 4 + 1] = source[x * 4 + 1];
            target[x * 4 + 2] = source[x * 4];
            target[x * 4 + 3] = source[x * 4 + 3];
        }
    }
    return out;
}

bool valid(const Pixels& p) {
    return p.width && p.height && sane(p.width, p.height) && p.rgba.size() == static_cast<std::size_t>(p.width) * p.height * 4;
}

} // namespace

Result<std::size_t> image_frame_count(std::span<const std::uint8_t> bytes) {
    switch (sniff(bytes)) {
    case Kind::png:
    case Kind::jpeg:
    case Kind::bmp:
        return std::size_t{1};
    case Kind::tiff:
        return tiff_count(bytes);
    case Kind::gif: {
        Gif g;
        if (!open_gif(g, bytes)) return Error::unsupported;
        return static_cast<std::size_t>(g.gif->ImageCount);
    }
    case Kind::ico:
        return ico_count(bytes);
    case Kind::unknown:
        break;
    }
    return Error::unsupported;
}

Result<ImageFrame> image_frame(std::span<const std::uint8_t> bytes, std::size_t index, bool pixels) {
    const auto kind = sniff(bytes);
    if (kind == Kind::unknown) return Error::unsupported;
    if (index > 0 && (kind == Kind::png || kind == Kind::jpeg || kind == Kind::bmp)) {
        // The file must still be readable to have no second frame.
        auto first = kind == Kind::png ? png_frame(bytes, false) : kind == Kind::jpeg ? jpeg_frame(bytes, false) : bmp_frame(bytes, false);
        if (!first) return first.error();
        return Error::out_of_range;
    }
    switch (kind) {
    case Kind::png: return png_frame(bytes, pixels);
    case Kind::jpeg: return jpeg_frame(bytes, pixels);
    case Kind::tiff: return tiff_frame(bytes, index, pixels);
    case Kind::gif: return gif_frame(bytes, index, pixels);
    case Kind::bmp: return bmp_frame(bytes, pixels);
    case Kind::ico: return ico_frame(bytes, index, pixels);
    case Kind::unknown: break;
    }
    return Error::unsupported;
}

Result<Pixels> decode_image(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > (1ULL << 31)) return Error::invalid_argument;
    auto frame = image_frame(bytes, 0, true);
    if (!frame) return frame.error() == Error::io ? Error::unsupported : frame.error();
    Pixels pixels;
    pixels.width = frame->width;
    pixels.height = frame->height;
    pixels.rgba = std::move(frame->rgba);
    return pixels;
}

Result<std::vector<std::uint8_t>> encode_image_frames(ImageFormat format, std::span<const Pixels> frames, double quality) {
    if (frames.empty()) return Error::invalid_argument;
    for (const auto& frame : frames)
        if (!valid(frame)) return Error::invalid_argument;
    switch (format) {
    case ImageFormat::png: return png_encode(frames[0]);
    case ImageFormat::jpeg: return jpeg_encode(frames[0], quality);
    case ImageFormat::tiff: return tiff_encode(frames);
    case ImageFormat::bmp: return bmp_encode(frames[0]);
    case ImageFormat::gif: return gif_encode(frames);
    }
    return Error::invalid_argument;
}

Result<std::vector<std::uint8_t>> encode_image(const Pixels& pixels, ImageFormat format) {
    if (!valid(pixels)) return Error::invalid_argument;
    // WIC's default JPEG quality is 0.9.
    return encode_image_frames(format, std::span<const Pixels>(&pixels, 1), 0.9);
}

} // namespace alchemy::host::gpu
