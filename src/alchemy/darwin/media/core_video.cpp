#include "alchemy/darwin/media/media.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// CoreVideo pixel buffers (<CoreVideo/CVPixelBuffer.h>, CVBuffer.h, CVImageBuffer.h,
// CVPixelBufferPool.h).
//
// - A pixel buffer is a CF-type object (CFRetain/CFRelease, CFGetTypeID ==
//   CVPixelBufferGetTypeID) over one block of guest memory: malloc'd (rows aligned to 64 bytes,
//   or kCVPixelBufferBytesPerRowAlignmentKey), an IOSurface's pages (CVPixelBufferCreate with
//   kCVPixelBufferIOSurfacePropertiesKey, kCVPixelBufferMetalCompatibilityKey or
//   kCVPixelBufferOpenGLCompatibilityKey, or CVPixelBufferCreateWithIOSurface), or the caller's
//   memory (CVPixelBufferCreateWithBytes / WithPlanarBytes, released through the caller's
//   callback when the buffer goes).
// - Formats: 32ARGB, 32BGRA, 32RGBA, 32ABGR, 24RGB, 24BGR, 16-bit 555/565, l10r, 2vuy/yuvs
//   (2 pixels per 4-byte element), the one- and two-component 8/16/32-bit and half/float
//   formats, 64RGBAHalf, 128RGBAFloat, and the planar Y'CbCr formats 420v/420f, x420/xf20,
//   y420/f420, 422v/422f, 444v/444f. Others answer kCVReturnInvalidPixelFormat.
// - Locking an IOSurface-backed buffer locks the surface (read-only with
//   kCVPixelBufferLock_ReadOnly); a memory-backed buffer needs no lock.
// - Attachments (CVBufferSetAttachment and the rest) are kept per buffer with their
//   propagation mode.
// - Pools make a new buffer for every request from their pixel-buffer attributes, counting the
//   ones alive for kCVPixelBufferPoolAllocationThresholdKey (no reuse).
namespace alchemy::gui::fill {
namespace {

constexpr std::int32_t cv_success = 0, cv_error = -6660, cv_invalid_argument = -6661, cv_allocation_failed = -6662,
    cv_invalid_pixel_format = -6680, cv_invalid_size = -6681, cv_invalid_attributes = -6682, cv_would_exceed = -6689;
constexpr std::string_view iosurface = "/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface";
constexpr std::uint64_t lock_read_only = 1;   // kCVPixelBufferLock_ReadOnly / kIOSurfaceLockReadOnly

// One plane of a format: its size divides the buffer's (rounded up), and each element of
// `bytes` bytes covers `pixels` pixels of a row.
struct PlaneSpec { std::uint32_t width_divisor{1}, height_divisor{1}, bytes{}, pixels{1}; };
struct FormatSpec {
    std::uint32_t type{};
    std::array<PlaneSpec, 3> planes{};
    std::uint32_t plane_count{};       // 0: chunky (planes[0] describes the pixels)
};

constexpr FormatSpec chunky(std::uint32_t type, std::uint32_t bytes, std::uint32_t pixels = 1) { return {type, {{{1, 1, bytes, pixels}}}, 0}; }
constexpr FormatSpec planar(std::uint32_t type, PlaneSpec a, PlaneSpec b, PlaneSpec c = {}) {
    return {type, {{a, b, c}}, c.bytes ? 3U : 2U};
}

// kCVPixelFormatType_* values (CVPixelBuffer.h).
constexpr std::array<FormatSpec, 33> formats{{
    chunky(0x00000020, 4), chunky(fourcc("BGRA"), 4), chunky(fourcc("RGBA"), 4), chunky(fourcc("ABGR"), 4),
    chunky(0x00000018, 3), chunky(fourcc("24BG"), 3), chunky(0x00000010, 2), chunky(fourcc("L555"), 2), chunky(fourcc("L565"), 2),
    chunky(fourcc("B565"), 2), chunky(fourcc("l10r"), 4), chunky(fourcc("2vuy"), 4, 2), chunky(fourcc("yuvs"), 4, 2),
    chunky(fourcc("L008"), 1), chunky(fourcc("L016"), 2), chunky(fourcc("L00h"), 2), chunky(fourcc("L00f"), 4),
    chunky(fourcc("2C08"), 2), chunky(fourcc("2C16"), 4), chunky(fourcc("2C0h"), 4), chunky(fourcc("2C0f"), 8),
    chunky(fourcc("b16g"), 2), chunky(fourcc("RGhA"), 8), chunky(fourcc("RGfA"), 16),
    planar(fourcc("420v"), {1, 1, 1}, {2, 2, 2}), planar(fourcc("420f"), {1, 1, 1}, {2, 2, 2}),
    planar(fourcc("x420"), {1, 1, 2}, {2, 2, 4}), planar(fourcc("xf20"), {1, 1, 2}, {2, 2, 4}),
    planar(fourcc("y420"), {1, 1, 1}, {2, 2, 1}, {2, 2, 1}), planar(fourcc("f420"), {1, 1, 1}, {2, 2, 1}, {2, 2, 1}),
    planar(fourcc("422v"), {1, 1, 1}, {2, 1, 2}), planar(fourcc("444v"), {1, 1, 1}, {1, 1, 2}),
    planar(fourcc("444f"), {1, 1, 1}, {1, 1, 2}),
}};

const FormatSpec* format_spec(std::uint32_t type) {
    for (const auto& f : formats)
        if (f.type == type) return &f;
    if (type == fourcc("422f")) return format_spec(fourcc("422v"));
    return nullptr;
}

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

struct Plane { Address base{}; std::uint32_t width{}, height{}; std::uint64_t bytes_per_row{}; };

struct Attachment { Address key{}, value{}; std::uint32_t mode{}; };

struct PixelBuffer final : ObjectPayload {
    std::uint32_t width{}, height{}, format{};
    std::vector<Plane> planes;         // one for a chunky buffer
    bool planar{};
    Address data{};                    // the block (planar: the first plane's start)
    std::uint64_t data_size{};
    Address surface{};                 // retained IOSurface, or 0
    Address owned{};                   // malloc'd block, or 0
    Address release_callback{}, release_refcon{}, plane_addresses{};   // caller's memory
    bool planar_callback{};
    std::uint32_t plane_count_for_callback{};
    std::mutex mutex;
    std::vector<Attachment> attachments;
    Address attachments_snapshot{};    // the dictionary CVBufferGetAttachments returned last (+1, ours)
    std::weak_ptr<struct PoolState> pool;
};

struct PoolState final : ObjectPayload {
    Address pool_attributes{}, buffer_attributes{};   // retained
    std::atomic<std::uint32_t> outstanding{};
};

class CoreVideo final : public Provider {
public:
    using Provider::Provider;
    void install();

private:
    std::shared_ptr<PixelBuffer> buffer(Address object) const { return object ? objc.find_host_state<PixelBuffer>(object) : nullptr; }
    std::optional<std::int64_t> number(Address dictionary, std::string_view key) {
        if (!dictionary) return std::nullopt;
        const auto name = cf.string(key);
        const auto value = cf.dictionary_value(dictionary, name);
        release(name);
        if (!value) return std::nullopt;
        if (const auto n = cf.number_value(value)) return static_cast<std::int64_t>(*n);
        return std::nullopt;
    }
    Address value(Address dictionary, std::string_view key) {
        if (!dictionary) return 0;
        const auto name = cf.string(key);
        const auto found = cf.dictionary_value(dictionary, name);
        release(name);
        return found;
    }
    Address make(std::shared_ptr<PixelBuffer> state) {
        const auto object = objc.allocate(buffer_class_);
        static_cast<void>(objc.host_state<PixelBuffer>(object, [&] { return state; }));
        return object;
    }
    // The planes' geometry for `spec`, rows aligned to `alignment`.
    std::vector<Plane> layout(const FormatSpec& spec, std::uint32_t width, std::uint32_t height, std::uint64_t alignment) const {
        std::vector<Plane> out;
        const auto count = spec.plane_count ? spec.plane_count : 1U;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& p = spec.planes[i];
            Plane plane;
            plane.width = (width + p.width_divisor - 1) / p.width_divisor;
            plane.height = (height + p.height_divisor - 1) / p.height_divisor;
            plane.bytes_per_row = align_up((static_cast<std::uint64_t>(plane.width) + p.pixels - 1) / p.pixels * p.bytes, alignment);
            out.push_back(plane);
        }
        return out;
    }
    std::int32_t create(std::uint32_t width, std::uint32_t height, std::uint32_t format, Address attributes, Address& out,
                        const std::shared_ptr<PoolState>& pool = nullptr);
    std::int32_t create_with_surface(Address surface, Address& out);
    void install_buffers();
    void install_attachments();
    void install_pools();

    Address buffer_class_{}, pool_class_{};
};

std::int32_t CoreVideo::create(std::uint32_t width, std::uint32_t height, std::uint32_t format, Address attributes, Address& out,
                               const std::shared_ptr<PoolState>& pool) {
    out = 0;
    const auto* spec = format_spec(format);
    if (!spec) return cv_invalid_pixel_format;
    if (!width || !height || width > 32768 || height > 32768) return cv_invalid_size;
    const auto alignment = static_cast<std::uint64_t>(std::clamp<std::int64_t>(number(attributes, "BytesPerRowAlignment").value_or(64), 1, 4096));
    auto state = std::make_shared<PixelBuffer>();
    state->width = width;
    state->height = height;
    state->format = format;
    state->planar = spec->plane_count != 0;
    state->planes = layout(*spec, width, height, std::max<std::uint64_t>(alignment, 16));
    state->pool = pool;
    // IOSurface-backed when asked for, and for Metal and OpenGL use (their textures alias the surface).
    const auto truthy = [&](std::string_view key) { const auto v = value(attributes, key); return v && cf.number_value(v).value_or(0) != 0; };
    if (value(attributes, "IOSurfaceProperties") || truthy("MetalCompatibility") || truthy("OpenGLCompatibility")) {
        // An IOSurface of the same geometry: IOSurfaceCreate with plane info for planar formats.
        std::vector<std::pair<Address, Address>> entries;
        const auto put = [&](std::string_view key, std::int64_t n) { entries.emplace_back(cf.string(key), cf.integer(n, 9)); };
        put("IOSurfaceWidth", width);
        put("IOSurfaceHeight", height);
        put("IOSurfacePixelFormat", format);
        if (state->planar) {
            std::vector<Address> plane_dicts;
            for (std::uint32_t i = 0; i < spec->plane_count; ++i) {
                const auto& p = state->planes[i];
                const std::array<std::pair<Address, Address>, 5> plane_entries{{
                    {cf.string("IOSurfacePlaneWidth"), cf.integer(p.width, 9)}, {cf.string("IOSurfacePlaneHeight"), cf.integer(p.height, 9)},
                    {cf.string("IOSurfacePlaneBytesPerElement"), cf.integer(spec->planes[i].bytes, 9)},
                    {cf.string("IOSurfacePlaneElementWidth"), cf.integer(spec->planes[i].pixels, 9)},
                    {cf.string("IOSurfacePlaneBytesPerRow"), cf.integer(static_cast<std::int64_t>(p.bytes_per_row), 9)}}};
                plane_dicts.push_back(cf.dictionary(plane_entries));
                for (const auto& [k, v] : plane_entries) { release(k); release(v); }
            }
            entries.emplace_back(cf.string("IOSurfacePlaneInfo"), cf.array(plane_dicts));
            for (const auto d : plane_dicts) release(d);
        } else {
            put("IOSurfaceBytesPerElement", spec->planes[0].bytes);
            put("IOSurfaceElementWidth", spec->planes[0].pixels);
            put("IOSurfaceBytesPerRow", static_cast<std::int64_t>(state->planes[0].bytes_per_row));
        }
        const auto properties = cf.dictionary(entries);
        for (const auto& [k, v] : entries) { release(k); release(v); }
        const auto surface = call(iosurface, "_IOSurfaceCreate", {properties});
        release(properties);
        if (!surface) return cv_allocation_failed;
        const auto made = create_with_surface(surface, out);
        release(surface);   // the buffer holds its own reference
        if (const auto b = buffer(out)) b->pool = pool;
        return made;
    }
    std::uint64_t total = 0;
    std::vector<std::uint64_t> offsets;
    for (const auto& p : state->planes) {
        total = align_up(total, 64);
        offsets.push_back(total);
        total += p.bytes_per_row * p.height;
    }
    state->owned = malloc(total);
    memory().fill_bytes(state->owned, 0, total);
    for (std::size_t i = 0; i < state->planes.size(); ++i) state->planes[i].base = state->owned + offsets[i];
    state->data = state->owned;
    state->data_size = total;
    out = make(std::move(state));
    return cv_success;
}

std::int32_t CoreVideo::create_with_surface(Address surface, Address& out) {
    out = 0;
    if (!surface) return cv_invalid_argument;
    auto state = std::make_shared<PixelBuffer>();
    state->width = static_cast<std::uint32_t>(call(iosurface, "_IOSurfaceGetWidth", {surface}));
    state->height = static_cast<std::uint32_t>(call(iosurface, "_IOSurfaceGetHeight", {surface}));
    state->format = static_cast<std::uint32_t>(call(iosurface, "_IOSurfaceGetPixelFormat", {surface}));
    const auto planes = call(iosurface, "_IOSurfaceGetPlaneCount", {surface});
    state->planar = planes > 0;
    for (Address i = 0; i < std::max<Address>(planes, 1); ++i) {
        Plane p;
        if (planes) {
            p.base = call(iosurface, "_IOSurfaceGetBaseAddressOfPlane", {surface, i});
            p.width = static_cast<std::uint32_t>(call(iosurface, "_IOSurfaceGetWidthOfPlane", {surface, i}));
            p.height = static_cast<std::uint32_t>(call(iosurface, "_IOSurfaceGetHeightOfPlane", {surface, i}));
            p.bytes_per_row = call(iosurface, "_IOSurfaceGetBytesPerRowOfPlane", {surface, i});
        } else {
            p.base = call(iosurface, "_IOSurfaceGetBaseAddress", {surface});
            p.width = state->width;
            p.height = state->height;
            p.bytes_per_row = call(iosurface, "_IOSurfaceGetBytesPerRow", {surface});
        }
        state->planes.push_back(p);
    }
    state->data = call(iosurface, "_IOSurfaceGetBaseAddress", {surface});
    state->data_size = call(iosurface, "_IOSurfaceGetAllocSize", {surface});
    state->surface = retain(surface);
    out = make(std::move(state));
    return cv_success;
}

void CoreVideo::install_buffers() {
    buffer_class_ = objc.find_class("__AlchemyCVPixelBuffer");
    if (!buffer_class_) buffer_class_ = objc.register_native_class("__AlchemyCVPixelBuffer", objc.find_class("NSObject"), 8);
    register_type(buffer_class_, TypeId::cv_pixel_buffer);
    const auto cls = buffer_class_;
    static_cast<void>(objc.add_native_method(cls, "dealloc", [self = shared_from_this(), this, cls](CpuState& s, GuestMemory& m) {
        if (const auto b = buffer(s.x[0])) {
            std::vector<Attachment> attachments;
            Address snapshot{};
            {
                std::lock_guard lock(b->mutex);
                attachments = std::move(b->attachments);
                snapshot = std::exchange(b->attachments_snapshot, 0);
            }
            for (const auto& a : attachments) { release(a.key); release(a.value); }
            release(snapshot);
            if (b->surface) release(b->surface);
            if (b->owned) free(b->owned);
            if (b->release_callback) {
                EngineAdapter adapter(process.calling_engine());
                if (b->planar_callback) {
                    const std::array<std::uint64_t, 5> arguments{b->release_refcon, b->data, b->data_size, b->plane_count_for_callback, b->plane_addresses};
                    static_cast<void>(adapter.call(b->release_callback, arguments));
                } else {
                    const std::array<std::uint64_t, 2> arguments{b->release_refcon, b->planes.empty() ? 0 : b->planes[0].base};
                    static_cast<void>(adapter.call(b->release_callback, arguments));
                }
            }
            if (b->plane_addresses) process.release_guest(b->plane_addresses);
            if (const auto pool = b->pool.lock()) --pool->outstanding;
        }
        static_cast<void>(m);
        static_cast<void>(objc.send_super(s.x[0], cls, objc.selector("dealloc")));
    }, false, "v@:"));

    constant(core_video, "_CVPixelBufferGetTypeID", static_cast<Address>(TypeId::cv_pixel_buffer));
    // The attribute keys (CVPixelBuffer.h); the rest are video.cpp's and media_gaps.cpp's.
    for (const auto& [symbol, text] : std::array<std::pair<const char*, const char*>, 18>{{
             {"_kCVPixelBufferMetalCompatibilityKey", "MetalCompatibility"},
             {"_kCVPixelBufferOpenGLCompatibilityKey", "OpenGLCompatibility"},
             {"_kCVPixelBufferIOSurfacePropertiesKey", "IOSurfaceProperties"},
             {"_kCVPixelBufferPixelFormatTypeKey", "PixelFormatType"},
             {"_kCVPixelBufferWidthKey", "Width"},
             {"_kCVPixelBufferHeightKey", "Height"},
             {"_kCVPixelBufferMemoryAllocatorKey", "MemoryAllocator"},
             {"_kCVPixelBufferExtendedPixelsLeftKey", "ExtendedPixelsLeft"},
             {"_kCVPixelBufferExtendedPixelsTopKey", "ExtendedPixelsTop"},
             {"_kCVPixelBufferExtendedPixelsRightKey", "ExtendedPixelsRight"},
             {"_kCVPixelBufferExtendedPixelsBottomKey", "ExtendedPixelsBottom"},
             {"_kCVPixelBufferPlaneAlignmentKey", "PlaneAlignment"},
             {"_kCVPixelBufferCGImageCompatibilityKey", "CGImageCompatibility"},
             {"_kCVPixelBufferOpenGLTextureCacheCompatibilityKey", "OpenGLTextureCacheCompatibility"},
             {"_kCVPixelBufferIOSurfaceOpenGLTextureCompatibilityKey", "IOSurfaceOpenGLTextureCompatibility"},
             {"_kCVPixelBufferIOSurfaceOpenGLFBOCompatibilityKey", "IOSurfaceOpenGLFBOCompatibility"},
             {"_kCVPixelBufferIOSurfaceCoreAnimationCompatibilityKey", "IOSurfaceCoreAnimationCompatibility"},
             {"_kCVPixelBufferPoolMaximumBufferAgeKey", "MaximumBufferAge"}}})
        string(core_video, symbol, text);
    // CVPixelBufferCreate(allocator, width, height, format, attributes, CVPixelBufferRef *out)
    raw(core_video, "_CVPixelBufferCreate", [this](CpuState& s, GuestMemory& m) {
        if (s.x[5]) m.write64(s.x[5], 0);
        if (!s.x[5]) { s.x[0] = status(cv_invalid_argument); return; }
        Address made{};
        const auto code = create(static_cast<std::uint32_t>(s.x[1]), static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3]), s.x[4], made);
        m.write64(s.x[5], made);
        s.x[0] = status(code);
    });
    // CVPixelBufferCreateWithIOSurface(allocator, surface, attributes, CVPixelBufferRef *out)
    raw(core_video, "_CVPixelBufferCreateWithIOSurface", [this](CpuState& s, GuestMemory& m) {
        if (s.x[3]) m.write64(s.x[3], 0);
        if (!s.x[3]) { s.x[0] = status(cv_invalid_argument); return; }
        Address made{};
        const auto code = create_with_surface(s.x[1], made);
        m.write64(s.x[3], made);
        s.x[0] = status(code);
    });
    // CVPixelBufferCreateWithBytes(allocator, width, height, format, base, bytesPerRow, releaseCallback,
    //     releaseRefCon, [stack] attributes, [stack] CVPixelBufferRef *out)
    raw(core_video, "_CVPixelBufferCreateWithBytes", [this](CpuState& s, GuestMemory& m) {
        const auto out = m.read64(s.sp + 8);
        if (out) m.write64(out, 0);
        const auto width = static_cast<std::uint32_t>(s.x[1]), height = static_cast<std::uint32_t>(s.x[2]), format = static_cast<std::uint32_t>(s.x[3]);
        const auto* spec = format_spec(format);
        if (!out || !s.x[4]) { s.x[0] = status(cv_invalid_argument); return; }
        if (!spec || spec->plane_count) { s.x[0] = status(cv_invalid_pixel_format); return; }
        if (!width || !height) { s.x[0] = status(cv_invalid_size); return; }
        auto state = std::make_shared<PixelBuffer>();
        state->width = width;
        state->height = height;
        state->format = format;
        state->planes = {{s.x[4], width, height, s.x[5]}};
        state->data = s.x[4];
        state->data_size = s.x[5] * height;
        state->release_callback = s.x[6];
        state->release_refcon = s.x[7];
        m.write64(out, make(std::move(state)));
        s.x[0] = status(cv_success);
    });
    // CVPixelBufferCreateWithPlanarBytes(allocator, width, height, format, dataPtr, dataSize,
    //     numberOfPlanes, planeBaseAddress[], [stack] planeWidth[], planeHeight[], planeBytesPerRow[],
    //     releaseCallback, releaseRefCon, attributes, CVPixelBufferRef *out)
    raw(core_video, "_CVPixelBufferCreateWithPlanarBytes", [this](CpuState& s, GuestMemory& m) {
        const auto stack = [&](unsigned index) { return m.read64(s.sp + 8ULL * index); };
        const auto out = stack(6);
        if (out) m.write64(out, 0);
        const auto width = static_cast<std::uint32_t>(s.x[1]), height = static_cast<std::uint32_t>(s.x[2]), format = static_cast<std::uint32_t>(s.x[3]);
        const auto count = static_cast<std::uint32_t>(s.x[6]);
        const auto bases = s.x[7], widths = stack(0), heights = stack(1), rows = stack(2);
        if (!out || !bases || !widths || !heights || !rows || count == 0 || count > 3) { s.x[0] = status(cv_invalid_argument); return; }
        const auto* spec = format_spec(format);
        if (!spec || spec->plane_count != count) { s.x[0] = status(cv_invalid_pixel_format); return; }
        auto state = std::make_shared<PixelBuffer>();
        state->width = width;
        state->height = height;
        state->format = format;
        state->planar = true;
        for (std::uint32_t i = 0; i < count; ++i)
            state->planes.push_back({m.read64(bases + 8ULL * i), static_cast<std::uint32_t>(m.read64(widths + 8ULL * i)),
                                     static_cast<std::uint32_t>(m.read64(heights + 8ULL * i)), m.read64(rows + 8ULL * i)});
        state->data = s.x[4] ? s.x[4] : state->planes[0].base;
        state->data_size = s.x[5];
        state->release_callback = stack(3);
        state->release_refcon = stack(4);
        state->planar_callback = true;
        state->plane_count_for_callback = count;
        state->plane_addresses = process.allocate_guest(8ULL * count, 8);
        for (std::uint32_t i = 0; i < count; ++i) m.write64(state->plane_addresses + 8ULL * i, state->planes[i].base);
        m.write64(out, make(std::move(state)));
        s.x[0] = status(cv_success);
    });
    for (const auto* name : {"_CVBufferRetain", "_CVPixelBufferRetain"})
        raw(core_video, name, [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    for (const auto* name : {"_CVBufferRelease", "_CVPixelBufferRelease"})
        raw(core_video, name, [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    // Accessors: the documented answers for NULL (0) or for anything that is not a pixel buffer.
    const auto getter = [this](const char* name, auto read) {
        raw(core_video, name, [this, read](CpuState& s, GuestMemory&) {
            const auto b = buffer(s.x[0]);
            s.x[0] = b ? read(*b) : 0;
        });
    };
    getter("_CVPixelBufferGetWidth", [](const PixelBuffer& b) -> Address { return b.width; });
    getter("_CVPixelBufferGetHeight", [](const PixelBuffer& b) -> Address { return b.height; });
    getter("_CVPixelBufferGetPixelFormatType", [](const PixelBuffer& b) -> Address { return b.format; });
    getter("_CVPixelBufferGetBaseAddress", [](const PixelBuffer& b) -> Address { return b.planar ? b.data : b.planes[0].base; });
    getter("_CVPixelBufferGetBytesPerRow", [](const PixelBuffer& b) -> Address { return b.planar ? 0 : b.planes[0].bytes_per_row; });
    getter("_CVPixelBufferGetDataSize", [](const PixelBuffer& b) -> Address { return b.data_size; });
    getter("_CVPixelBufferIsPlanar", [](const PixelBuffer& b) -> Address { return b.planar ? 1 : 0; });
    getter("_CVPixelBufferGetPlaneCount", [](const PixelBuffer& b) -> Address { return b.planar ? b.planes.size() : 0; });
    getter("_CVPixelBufferGetIOSurface", [](const PixelBuffer& b) -> Address { return b.surface; });
    const auto plane_getter = [this](const char* name, auto read) {
        raw(core_video, name, [this, read](CpuState& s, GuestMemory&) {
            const auto b = buffer(s.x[0]);
            const auto index = s.x[1];
            // A chunky buffer has no planes: 0.
            s.x[0] = b && b->planar && index < b->planes.size() ? read(b->planes[index]) : 0;
        });
    };
    plane_getter("_CVPixelBufferGetBaseAddressOfPlane", [](const Plane& p) -> Address { return p.base; });
    plane_getter("_CVPixelBufferGetBytesPerRowOfPlane", [](const Plane& p) -> Address { return p.bytes_per_row; });
    plane_getter("_CVPixelBufferGetWidthOfPlane", [](const Plane& p) -> Address { return p.width; });
    plane_getter("_CVPixelBufferGetHeightOfPlane", [](const Plane& p) -> Address { return p.height; });
    // CVPixelBufferGetExtendedPixels(buffer, left*, right*, top*, bottom*): none.
    raw(core_video, "_CVPixelBufferGetExtendedPixels", [](CpuState& s, GuestMemory& m) {
        for (unsigned i = 1; i <= 4; ++i)
            if (s.x[i]) m.write64(s.x[i], 0);
    });
    raw(core_video, "_CVPixelBufferFillExtendedPixels", [this](CpuState& s, GuestMemory&) {
        s.x[0] = status(buffer(s.x[0]) ? cv_success : cv_invalid_argument);
    });
    for (const auto* name : {"_CVPixelBufferLockBaseAddress", "_CVPixelBufferUnlockBaseAddress"}) {
        const bool lock = std::string_view(name) == "_CVPixelBufferLockBaseAddress";
        raw(core_video, name, [this, lock](CpuState& s, GuestMemory&) {
            const auto b = buffer(s.x[0]);
            if (!b) { s.x[0] = status(cv_invalid_argument); return; }
            if (b->surface)
                static_cast<void>(call(iosurface, lock ? "_IOSurfaceLock" : "_IOSurfaceUnlock", {b->surface, s.x[1] & lock_read_only, 0}));
            s.x[0] = status(cv_success);
        });
    }
    // CVPixelBufferCreateResolvedAttributesDictionary(allocator, CFArrayRef attributes, CFDictionaryRef *out):
    // the dictionaries merged, later ones winning.
    raw(core_video, "_CVPixelBufferCreateResolvedAttributesDictionary", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) { s.x[0] = status(cv_invalid_argument); return; }
        std::vector<std::pair<Address, Address>> merged;
        if (s.x[1])
            for (const auto dictionary : cf.array_values(s.x[1]))
                for (const auto& [k, v] : cf.dictionary_entries(dictionary)) {
                    const auto same = std::find_if(merged.begin(), merged.end(), [&](const auto& e) { return cf.equal(e.first, k); });
                    if (same != merged.end()) same->second = v;
                    else merged.emplace_back(k, v);
                }
        m.write64(s.x[2], cf.dictionary(merged));
        s.x[0] = status(cv_success);
    });
    // CVImageBuffer: the clean aperture, display and encoded sizes are the whole buffer.
    raw(core_video, "_CVImageBufferGetCleanRect", [this](CpuState& s, GuestMemory&) {
        const auto b = buffer(s.x[0]);
        return_real(s, 0, 0);
        return_real(s, 0, 1);
        return_real(s, b ? b->width : 0, 2);
        return_real(s, b ? b->height : 0, 3);
    });
    for (const auto* name : {"_CVImageBufferGetDisplaySize", "_CVImageBufferGetEncodedSize"})
        raw(core_video, name, [this](CpuState& s, GuestMemory&) {
            const auto b = buffer(s.x[0]);
            return_real(s, b ? b->width : 0, 0);
            return_real(s, b ? b->height : 0, 1);
        });
    constant(core_video, "_CVImageBufferIsFlipped", 0);
    constant(core_video, "_CVImageBufferGetColorSpace", 0);   // no CGColorSpace attachment is made
}

void CoreVideo::install_attachments() {
    // CVBufferSetAttachment(buffer, key, value, mode)
    raw(core_video, "_CVBufferSetAttachment", [this](CpuState& s, GuestMemory&) {
        const auto b = buffer(s.x[0]);
        if (!b || !s.x[1]) return;
        const auto key = retain(s.x[1]), value = retain(s.x[2]);
        Address old_key{}, old_value{};
        {
            std::lock_guard lock(b->mutex);
            const auto found = std::find_if(b->attachments.begin(), b->attachments.end(), [&](const Attachment& a) { return cf.equal(a.key, key); });
            if (found != b->attachments.end()) {
                old_key = found->key;
                old_value = found->value;
                *found = {key, value, static_cast<std::uint32_t>(s.x[3])};
            } else {
                b->attachments.push_back({key, value, static_cast<std::uint32_t>(s.x[3])});
            }
        }
        release(old_key);
        release(old_value);
    });
    // CVBufferGetAttachment(buffer, key, CVAttachmentMode *mode): +0.
    raw(core_video, "_CVBufferGetAttachment", [this](CpuState& s, GuestMemory& m) {
        const auto b = buffer(s.x[0]);
        Address found_value{};
        std::uint32_t mode{};
        if (b && s.x[1]) {
            std::lock_guard lock(b->mutex);
            for (const auto& a : b->attachments)
                if (cf.equal(a.key, s.x[1])) { found_value = a.value; mode = a.mode; break; }
        }
        if (found_value && s.x[2]) m.write32(s.x[2], mode);
        s.x[0] = found_value;
    });
    raw(core_video, "_CVBufferRemoveAttachment", [this](CpuState& s, GuestMemory&) {
        const auto b = buffer(s.x[0]);
        if (!b || !s.x[1]) return;
        std::vector<Attachment> gone;
        {
            std::lock_guard lock(b->mutex);
            const auto split = std::partition(b->attachments.begin(), b->attachments.end(), [&](const Attachment& a) { return !cf.equal(a.key, s.x[1]); });
            gone.assign(split, b->attachments.end());
            b->attachments.erase(split, b->attachments.end());
        }
        for (const auto& a : gone) { release(a.key); release(a.value); }
    });
    raw(core_video, "_CVBufferRemoveAllAttachments", [this](CpuState& s, GuestMemory&) {
        const auto b = buffer(s.x[0]);
        if (!b) return;
        std::vector<Attachment> gone;
        {
            std::lock_guard lock(b->mutex);
            gone = std::move(b->attachments);
            b->attachments.clear();
        }
        for (const auto& a : gone) { release(a.key); release(a.value); }
    });
    // The attachments of one mode as a dictionary: CVBufferCopyAttachments (+1), and
    // CVBufferGetAttachments (+0, valid until the next call or the buffer goes).
    const auto collect = [this](PixelBuffer& b, std::uint32_t mode) {
        std::vector<std::pair<Address, Address>> pairs;
        std::lock_guard lock(b.mutex);
        for (const auto& a : b.attachments)
            if (a.mode == mode) pairs.emplace_back(a.key, a.value);
        return cf.dictionary(pairs);
    };
    raw(core_video, "_CVBufferCopyAttachments", [this, collect](CpuState& s, GuestMemory&) {
        const auto b = buffer(s.x[0]);
        s.x[0] = b ? collect(*b, static_cast<std::uint32_t>(s.x[1])) : 0;
    });
    raw(core_video, "_CVBufferGetAttachments", [this, collect](CpuState& s, GuestMemory&) {
        const auto b = buffer(s.x[0]);
        if (!b) { s.x[0] = 0; return; }
        const auto made = collect(*b, static_cast<std::uint32_t>(s.x[1]));
        Address old{};
        {
            std::lock_guard lock(b->mutex);
            old = std::exchange(b->attachments_snapshot, made);
        }
        release(old);
        s.x[0] = made;
    });
    // CVBufferSetAttachments(buffer, CFDictionaryRef, mode)
    raw(core_video, "_CVBufferSetAttachments", [this](CpuState& s, GuestMemory&) {
        if (!buffer(s.x[0]) || !s.x[1]) return;
        for (const auto& [k, v] : cf.dictionary_entries(s.x[1]))
            static_cast<void>(call(core_video, "_CVBufferSetAttachment", {s.x[0], k, v, s.x[2]}));
    });
    // CVBufferPropagateAttachments(source, destination): the kCVAttachmentMode_ShouldPropagate ones.
    raw(core_video, "_CVBufferPropagateAttachments", [this](CpuState& s, GuestMemory&) {
        const auto source = buffer(s.x[0]);
        if (!source || !buffer(s.x[1])) return;
        std::vector<Attachment> copy;
        {
            std::lock_guard lock(source->mutex);
            for (const auto& a : source->attachments)
                if (a.mode == 1) copy.push_back(a);
        }
        for (const auto& a : copy) static_cast<void>(call(core_video, "_CVBufferSetAttachment", {s.x[1], a.key, a.value, a.mode}));
    });
}

void CoreVideo::install_pools() {
    pool_class_ = objc.find_class("__AlchemyCVPixelBufferPool");
    if (!pool_class_) pool_class_ = objc.register_native_class("__AlchemyCVPixelBufferPool", objc.find_class("NSObject"), 8);
    register_type(pool_class_, TypeId::cv_pixel_buffer_pool);
    const auto cls = pool_class_;
    static_cast<void>(objc.add_native_method(cls, "dealloc", [self = shared_from_this(), this, cls](CpuState& s, GuestMemory&) {
        if (const auto p = objc.find_host_state<PoolState>(s.x[0])) {
            release(p->pool_attributes);
            release(p->buffer_attributes);
        }
        static_cast<void>(objc.send_super(s.x[0], cls, objc.selector("dealloc")));
    }, false, "v@:"));
    constant(core_video, "_CVPixelBufferPoolGetTypeID", static_cast<Address>(TypeId::cv_pixel_buffer_pool));
    // CVPixelBufferPoolCreate(allocator, poolAttributes, pixelBufferAttributes, CVPixelBufferPoolRef *out)
    raw(core_video, "_CVPixelBufferPoolCreate", [this, cls](CpuState& s, GuestMemory& m) {
        if (s.x[3]) m.write64(s.x[3], 0);
        if (!s.x[3]) { s.x[0] = status(cv_invalid_argument); return; }
        const auto attributes = s.x[2];
        if (!number(attributes, "Width") || !number(attributes, "Height")) { s.x[0] = status(cv_invalid_attributes); return; }
        auto state = std::make_shared<PoolState>();
        state->pool_attributes = retain(s.x[1]);
        state->buffer_attributes = retain(attributes);
        const auto pool = objc.allocate(cls);
        static_cast<void>(objc.host_state<PoolState>(pool, [&] { return state; }));
        m.write64(s.x[3], pool);
        s.x[0] = status(cv_success);
    });
    // The buffer format: kCVPixelBufferPixelFormatTypeKey, a number or an array (its first).
    const auto make_buffer = [this](Address pool, Address aux, Address out_slot, GuestMemory& m) {
        if (out_slot) m.write64(out_slot, 0);
        const auto state = pool ? objc.find_host_state<PoolState>(pool) : nullptr;
        if (!state || !out_slot) return cv_invalid_argument;
        if (const auto threshold = number(aux, "AllocationThreshold"); threshold && state->outstanding >= static_cast<std::uint64_t>(*threshold))
            return cv_would_exceed;
        const auto attributes = state->buffer_attributes;
        auto format = number(attributes, "PixelFormatType");
        if (!format) {
            const auto list = value(attributes, "PixelFormatType");
            if (list && cf.length(list)) {
                if (const auto n = cf.number_value(cf.element(list, 0))) format = static_cast<std::int64_t>(*n);
            }
        }
        if (!format) return cv_invalid_attributes;
        Address made{};
        const auto code = create(static_cast<std::uint32_t>(number(attributes, "Width").value_or(0)),
                                 static_cast<std::uint32_t>(number(attributes, "Height").value_or(0)), static_cast<std::uint32_t>(*format),
                                 attributes, made, state);
        if (!code) ++state->outstanding;
        m.write64(out_slot, made);
        return code;
    };
    // CVPixelBufferPoolCreatePixelBuffer(allocator, pool, CVPixelBufferRef *out)
    raw(core_video, "_CVPixelBufferPoolCreatePixelBuffer", [make_buffer](CpuState& s, GuestMemory& m) {
        s.x[0] = status(make_buffer(s.x[1], 0, s.x[2], m));
    });
    // CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(allocator, pool, auxAttributes, CVPixelBufferRef *out)
    raw(core_video, "_CVPixelBufferPoolCreatePixelBufferWithAuxAttributes", [make_buffer](CpuState& s, GuestMemory& m) {
        s.x[0] = status(make_buffer(s.x[1], s.x[2], s.x[3], m));
    });
    raw(core_video, "_CVPixelBufferPoolGetAttributes", [this](CpuState& s, GuestMemory&) {
        const auto state = s.x[0] ? objc.find_host_state<PoolState>(s.x[0]) : nullptr;
        s.x[0] = state ? state->pool_attributes : 0;
    });
    raw(core_video, "_CVPixelBufferPoolGetPixelBufferAttributes", [this](CpuState& s, GuestMemory&) {
        const auto state = s.x[0] ? objc.find_host_state<PoolState>(s.x[0]) : nullptr;
        s.x[0] = state ? state->buffer_attributes : 0;
    });
    raw(core_video, "_CVPixelBufferPoolRetain", [this](CpuState& s, GuestMemory&) { s.x[0] = retain(s.x[0]); });
    raw(core_video, "_CVPixelBufferPoolRelease", [this](CpuState& s, GuestMemory&) { release(s.x[0]); });
    // Nothing is held back for reuse, so there is nothing to flush.
    raw(core_video, "_CVPixelBufferPoolFlush", [](CpuState&, GuestMemory&) {});
}

void CoreVideo::install() {
    install_buffers();
    install_attachments();
    install_pools();
}

} // namespace

void install_core_video_pixel_buffers(InstallContext& context) { std::make_shared<CoreVideo>(context)->install(); }

} // namespace alchemy::gui::fill
