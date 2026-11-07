#include "alchemy/darwin/media/media.hpp"
#include "alchemy/darwin/media/pixel_convert.hpp"
#include "alchemy/darwin/media/vt_session.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// VTPixelTransferSession (<VideoToolbox/VTPixelTransferSession.h>) between CVPixelBuffers:
// conversion between the RGB, gray and Y'CbCr formats of pixel_convert.hpp (BGRA, ARGB, RGBA,
// ABGR, 24RGB, 24BGR, L008, 2vuy, yuvs, 420v/420f, x420/xf20, y420/f420) and scaling
// (kVTPixelTransferPropertyKey_ScalingMode: Normal and CropSourceToCleanAperture stretch, Letterbox
// fits inside black bars, Trim crops to fill). The source's kCVImageBufferYCbCrMatrixKey
// attachment (else BT.601 up to 576 lines, BT.709 above) reads Y'CbCr; the destination is
// written with kVTPixelTransferPropertyKey_DestinationYCbCrMatrix, else the destination's own
// attachment, else the source's matrix, and gets that attachment. Other formats answer
// kVTPixelTransferNotSupportedErr. VTSessionSetProperty / CopyProperty / SetProperties /
// CopySupportedPropertyDictionary work on transfer, rotation and decompression sessions
// (vt_session.hpp, each with its own keys); any other object is not a session
// (kVTInvalidSessionErr), as no compression session exists.
//
// VTPixelRotationSession rotates (kVTPixelRotationPropertyKey_Rotation: kVTRotation_0, _CW90,
// _180, _CCW90) and then flips (FlipHorizontalOrientation, FlipVerticalOrientation) a buffer into
// one of the rotated size, in any of the same formats. VTCreateCGImageFromCVPixelBuffer makes an
// 8-bit device RGB CGImage of a buffer in those formats (premultiplied when it has alpha).
namespace alchemy::gui::fill {
namespace {

namespace px = completion_kit::media::pixels;

constexpr std::int32_t vt_property_not_supported = -12900, vt_parameter = -12902, vt_invalid_session = -12903,
    vt_pixel_transfer_not_supported = -12905;

using TransferSession = VTSessionState;

constexpr std::array<const char*, 9> property_keys{"ScalingMode", "DestinationCleanAperture", "DestinationPixelAspectRatio", "DownsamplingMode",
                                                   "DestinationColorPrimaries", "DestinationTransferFunction", "DestinationICCProfile",
                                                   "DestinationYCbCrMatrix", "RealTime"};
// Pixel rotation sessions: the kVTPixelRotationPropertyKey_* strings.
constexpr std::array<const char*, 3> rotation_keys{"Rotation", "FlipHorizontalOrientation", "FlipVerticalOrientation"};
// Compression sessions (vt_compression.cpp): the kVTCompressionPropertyKey_* strings it honours (the
// others answer kVTPropertyNotSupportedErr).
constexpr std::array<const char*, 10> compression_keys{"AverageBitRate", "DataRateLimits", "ExpectedFrameRate", "MaxKeyFrameInterval",
                                                       "MaxKeyFrameIntervalDuration", "AllowFrameReordering", "AllowTemporalCompression",
                                                       "ProfileLevel", "RealTime", "UsingHardwareAcceleratedVideoEncoder"};
// Decompression sessions (vt_decompression.cpp): the kVTDecompressionPropertyKey_* strings.
constexpr std::array<const char*, 10> decompression_keys{"RealTime", "ThreadCount", "MaximizePowerEfficiency", "OutputPoolRequestedMinimumBufferCount",
                                                         "PixelTransferProperties", "ReducedResolutionDecode", "FieldMode", "DeinterlaceMode",
                                                         "PropagatePerFrameHDRDisplayMetadata", "UsingHardwareAcceleratedVideoDecoder"};

class PixelTransfer final : public Provider {
public:
    using Provider::Provider;
    void install();

private:
    std::shared_ptr<TransferSession> session(Address object) const { return object ? objc.find_host_state<TransferSession>(object) : nullptr; }
    std::int32_t transfer(TransferSession& session, Address source, Address destination);
    std::int32_t rotate(TransferSession& session, Address source, Address destination);
    std::int32_t create_image(Address buffer, Address& out);
    void add_dealloc(Address cls);
    bool pixel_buffer(Address object) {
        return object && call(core_foundation, "_CFGetTypeID", {object}) == call(core_video, "_CVPixelBufferGetTypeID", {});
    }
    static bool has_alpha(std::uint32_t format) {
        return format == 0x20 || format == fourcc("BGRA") || format == fourcc("RGBA") || format == fourcc("ABGR");
    }
    // A Y'CbCr destination gets the matrix it was written with as its kCVImageBufferYCbCrMatrixKey.
    void tag_matrix(Address destination, const px::Image& to) {
        if (has_alpha(to.format) || to.format == 0x18 || to.format == fourcc("24BG") || to.format == fourcc("L008")) return;
        const auto key = cf.string("CVImageBufferYCbCrMatrix");
        const auto name = cf.string(matrix_name(to.matrix));
        static_cast<void>(call(core_video, "_CVBufferSetAttachment", {destination, key, name, 1}));
        release(name);
        release(key);
    }
    // The image of a locked pixel buffer, with its planes copied out of guest memory into `storage`.
    std::optional<px::Image> image(Address buffer, std::vector<std::vector<std::uint8_t>>& storage, std::vector<Address>& bases);
    px::Matrix matrix_of(Address buffer, const px::Image& image) {
        const auto key = cf.string("CVImageBufferYCbCrMatrix");
        const auto value = call(core_video, "_CVBufferGetAttachment", {buffer, key, 0});
        release(key);
        return value ? matrix_named(cf.utf8(value)).value_or(px::default_matrix(image.width, image.height))
                     : px::default_matrix(image.width, image.height);
    }
    static std::optional<px::Matrix> matrix_named(const std::string& name) {
        if (name == "ITU_R_601_4" || name == "SMPTE_240M_1995") return px::Matrix::bt601;
        if (name == "ITU_R_709_2") return px::Matrix::bt709;
        if (name == "ITU_R_2020") return px::Matrix::bt2020;
        return std::nullopt;
    }
    static const char* matrix_name(px::Matrix m) {
        return m == px::Matrix::bt601 ? "ITU_R_601_4" : m == px::Matrix::bt2020 ? "ITU_R_2020" : "ITU_R_709_2";
    }

    Address session_class_{}, rotation_class_{};
};

std::optional<px::Image> PixelTransfer::image(Address buffer, std::vector<std::vector<std::uint8_t>>& storage, std::vector<Address>& bases) {
    auto& m = memory();
    px::Image out;
    out.format = static_cast<std::uint32_t>(call(core_video, "_CVPixelBufferGetPixelFormatType", {buffer}));
    out.width = static_cast<std::uint32_t>(call(core_video, "_CVPixelBufferGetWidth", {buffer}));
    out.height = static_cast<std::uint32_t>(call(core_video, "_CVPixelBufferGetHeight", {buffer}));
    const auto planes = px::plane_count(out.format);
    if (!planes || !out.width || !out.height) return std::nullopt;
    const bool planar = call(core_video, "_CVPixelBufferIsPlanar", {buffer}) != 0;
    if (planar != (planes > 1)) return std::nullopt;
    storage.assign(planes, {});
    bases.assign(planes, 0);
    for (unsigned i = 0; i < planes; ++i) {
        const auto base = planar ? call(core_video, "_CVPixelBufferGetBaseAddressOfPlane", {buffer, i}) : call(core_video, "_CVPixelBufferGetBaseAddress", {buffer});
        const auto row = planar ? call(core_video, "_CVPixelBufferGetBytesPerRowOfPlane", {buffer, i}) : call(core_video, "_CVPixelBufferGetBytesPerRow", {buffer});
        const auto rows = planar ? call(core_video, "_CVPixelBufferGetHeightOfPlane", {buffer, i}) : out.height;
        if (!base || !row || !rows) return std::nullopt;
        storage[i].resize(static_cast<std::size_t>(row * rows));
        m.read_bytes(base, storage[i]);
        bases[i] = base;
        out.planes.push_back({storage[i].data(), row});
    }
    return out;
}

std::int32_t PixelTransfer::transfer(TransferSession& s, Address source, Address destination) {
    const auto buffer_type = call(core_video, "_CVPixelBufferGetTypeID", {});
    if (!source || !destination || call(core_foundation, "_CFGetTypeID", {source}) != buffer_type ||
        call(core_foundation, "_CFGetTypeID", {destination}) != buffer_type)
        return vt_parameter;
    std::string scaling = "Normal";
    std::optional<px::Matrix> destination_matrix;
    {
        std::lock_guard lock(s.mutex);
        if (const auto found = s.properties.find("ScalingMode"); found != s.properties.end() && found->second) scaling = cf.utf8(found->second);
        if (const auto found = s.properties.find("DestinationYCbCrMatrix"); found != s.properties.end() && found->second)
            destination_matrix = matrix_named(cf.utf8(found->second));
    }
    static_cast<void>(call(core_video, "_CVPixelBufferLockBaseAddress", {source, 1}));
    static_cast<void>(call(core_video, "_CVPixelBufferLockBaseAddress", {destination, 0}));
    std::int32_t result = 0;
    std::vector<std::vector<std::uint8_t>> source_storage, destination_storage;
    std::vector<Address> source_bases, destination_bases;
    auto from = image(source, source_storage, source_bases);
    auto to = image(destination, destination_storage, destination_bases);
    if (!from || !to) {
        result = vt_pixel_transfer_not_supported;
    } else {
        from->matrix = matrix_of(source, *from);
        const auto key = cf.string("CVImageBufferYCbCrMatrix");
        const auto own = call(core_video, "_CVBufferGetAttachment", {destination, key, 0});
        to->matrix = destination_matrix.value_or(own ? matrix_named(cf.utf8(own)).value_or(from->matrix) : from->matrix);
        auto rgba = px::to_rgba(*from);
        auto source_width = from->width, source_height = from->height;
        if (scaling == "Trim") {
            // Crop the source to the destination's aspect ratio, centered.
            const double want = static_cast<double>(to->width) / to->height, have = static_cast<double>(source_width) / source_height;
            std::uint32_t cw = source_width, ch = source_height;
            if (have > want) cw = std::max(1U, static_cast<std::uint32_t>(source_height * want));
            else ch = std::max(1U, static_cast<std::uint32_t>(source_width / want));
            const auto x0 = (source_width - cw) / 2, y0 = (source_height - ch) / 2;
            std::vector<float> cropped(static_cast<std::size_t>(cw) * ch * 4);
            for (std::uint32_t y = 0; y < ch; ++y)
                std::copy_n(rgba.data() + ((static_cast<std::size_t>(y0 + y) * source_width + x0) * 4), cw * 4, cropped.data() + static_cast<std::size_t>(y) * cw * 4);
            rgba = std::move(cropped);
            source_width = cw;
            source_height = ch;
        }
        std::vector<float> out;
        if (scaling == "Letterbox") {
            // Fit inside the destination, centered, on black.
            const double fit = std::min(static_cast<double>(to->width) / source_width, static_cast<double>(to->height) / source_height);
            const auto fw = std::max(1U, static_cast<std::uint32_t>(source_width * fit)), fh = std::max(1U, static_cast<std::uint32_t>(source_height * fit));
            const auto fitted = px::scale(rgba, source_width, source_height, fw, fh);
            out.assign(static_cast<std::size_t>(to->width) * to->height * 4, 0.0f);
            for (std::size_t i = 3; i < out.size(); i += 4) out[i] = 1;
            const auto x0 = (to->width - fw) / 2, y0 = (to->height - fh) / 2;
            for (std::uint32_t y = 0; y < fh; ++y)
                std::copy_n(fitted.data() + static_cast<std::size_t>(y) * fw * 4, fw * 4, out.data() + ((static_cast<std::size_t>(y0 + y) * to->width + x0) * 4));
        } else {
            out = px::scale(rgba, source_width, source_height, to->width, to->height);
        }
        px::from_rgba(out, *to);
        for (std::size_t i = 0; i < destination_bases.size(); ++i) memory().write_bytes(destination_bases[i], destination_storage[i]);
        tag_matrix(destination, *to);
        release(key);
    }
    static_cast<void>(call(core_video, "_CVPixelBufferUnlockBaseAddress", {destination, 0}));
    static_cast<void>(call(core_video, "_CVPixelBufferUnlockBaseAddress", {source, 1}));
    return result;
}

std::int32_t PixelTransfer::rotate(TransferSession& s, Address source, Address destination) {
    if (!pixel_buffer(source) || !pixel_buffer(destination)) return vt_parameter;
    std::string rotation = "0";
    bool flip_horizontal = false, flip_vertical = false;
    {
        std::lock_guard lock(s.mutex);
        const auto flag = [&](const char* name) {
            const auto found = s.properties.find(name);
            return found != s.properties.end() && found->second && call(core_foundation, "_CFBooleanGetValue", {found->second}) != 0;
        };
        if (const auto found = s.properties.find("Rotation"); found != s.properties.end() && found->second) rotation = cf.utf8(found->second);
        flip_horizontal = flag("FlipHorizontalOrientation");
        flip_vertical = flag("FlipVerticalOrientation");
    }
    if (rotation != "0" && rotation != "CW90" && rotation != "180" && rotation != "CCW90") return vt_parameter;
    const bool quarter = rotation == "CW90" || rotation == "CCW90";
    static_cast<void>(call(core_video, "_CVPixelBufferLockBaseAddress", {source, 1}));
    static_cast<void>(call(core_video, "_CVPixelBufferLockBaseAddress", {destination, 0}));
    std::int32_t result = 0;
    std::vector<std::vector<std::uint8_t>> source_storage, destination_storage;
    std::vector<Address> source_bases, destination_bases;
    auto from = image(source, source_storage, source_bases);
    auto to = image(destination, destination_storage, destination_bases);
    if (!from || !to) {
        result = vt_pixel_transfer_not_supported;
    } else if ((quarter ? from->height : from->width) != to->width || (quarter ? from->width : from->height) != to->height) {
        result = vt_parameter;   // the destination must have the rotated size
    } else {
        from->matrix = matrix_of(source, *from);
        const auto key = cf.string("CVImageBufferYCbCrMatrix");
        const auto own = call(core_video, "_CVBufferGetAttachment", {destination, key, 0});
        to->matrix = own ? matrix_named(cf.utf8(own)).value_or(from->matrix) : from->matrix;
        release(key);
        const auto rgba = px::to_rgba(*from);
        std::vector<float> out(rgba.size());
        const auto w = from->width, h = from->height, ow = to->width, oh = to->height;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint32_t dx = x, dy = y;
                if (rotation == "CW90") { dx = h - 1 - y; dy = x; }
                else if (rotation == "CCW90") { dx = y; dy = w - 1 - x; }
                else if (rotation == "180") { dx = w - 1 - x; dy = h - 1 - y; }
                if (flip_horizontal) dx = ow - 1 - dx;
                if (flip_vertical) dy = oh - 1 - dy;
                std::copy_n(rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4, 4, out.data() + (static_cast<std::size_t>(dy) * ow + dx) * 4);
            }
        px::from_rgba(out, *to);
        for (std::size_t i = 0; i < destination_bases.size(); ++i) memory().write_bytes(destination_bases[i], destination_storage[i]);
        tag_matrix(destination, *to);
    }
    static_cast<void>(call(core_video, "_CVPixelBufferUnlockBaseAddress", {destination, 0}));
    static_cast<void>(call(core_video, "_CVPixelBufferUnlockBaseAddress", {source, 1}));
    return result;
}

std::int32_t PixelTransfer::create_image(Address buffer, Address& out) {
    out = 0;
    if (!pixel_buffer(buffer)) return vt_parameter;
    static_cast<void>(call(core_video, "_CVPixelBufferLockBaseAddress", {buffer, 1}));
    std::vector<std::vector<std::uint8_t>> storage;
    std::vector<Address> bases;
    auto from = image(buffer, storage, bases);
    std::int32_t result = 0;
    if (!from) {
        result = vt_pixel_transfer_not_supported;
    } else {
        from->matrix = matrix_of(buffer, *from);
        auto rgba = px::to_rgba(*from);
        const bool alpha = has_alpha(from->format);
        if (alpha)
            for (std::size_t i = 0; i < rgba.size(); i += 4)
                for (std::size_t c = 0; c < 3; ++c) rgba[i + c] *= rgba[i + 3];
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(from->width) * 4 * from->height);
        px::Image bgra;
        bgra.format = fourcc("BGRA");
        bgra.width = from->width;
        bgra.height = from->height;
        bgra.planes = {{pixels.data(), static_cast<std::uint64_t>(from->width) * 4}};
        px::from_rgba(rgba, bgra);
        const auto data = bridge->data(pixels);
        const auto provider = call(core_graphics, "_CGDataProviderCreateWithCFData", {data});
        release(data);
        const auto space = call(core_graphics, "_CGColorSpaceCreateDeviceRGB", {});
        // CGImageCreate(width, height, bitsPerComponent, bitsPerPixel, bytesPerRow, space, bitmapInfo, provider,
        //     [stack] decode, shouldInterpolate, intent)
        CallRegs create{};
        create.x[0] = from->width;
        create.x[1] = from->height;
        create.x[2] = 8;
        create.x[3] = 32;
        create.x[4] = static_cast<std::uint64_t>(from->width) * 4;
        create.x[5] = space;
        create.x[6] = (2U << 12) | (alpha ? 2U : 6U);   // kCGBitmapByteOrder32Little | PremultipliedFirst or NoneSkipFirst
        create.x[7] = provider;
        create.push_stack(std::uint64_t{0});
        create.push_stack(std::uint8_t{0});
        create.push_stack(std::int32_t{0});
        out = call_regs(core_graphics, "_CGImageCreate", create).x0;
        static_cast<void>(call(core_graphics, "_CGColorSpaceRelease", {space}));
        static_cast<void>(call(core_graphics, "_CGDataProviderRelease", {provider}));
        if (!out) result = vt_parameter;
    }
    static_cast<void>(call(core_video, "_CVPixelBufferUnlockBaseAddress", {buffer, 1}));
    return result;
}

void PixelTransfer::add_dealloc(Address cls) {
    static_cast<void>(objc.add_native_method(cls, "dealloc", [self = shared_from_this(), this, cls](CpuState& s, GuestMemory&) {
        if (const auto value = session(s.x[0])) {
            std::map<std::string, Address> properties;
            {
                std::lock_guard lock(value->mutex);
                properties.swap(value->properties);
            }
            for (const auto& [name, v] : properties) release(v);
        }
        static_cast<void>(objc.send_super(s.x[0], cls, objc.selector("dealloc")));
    }, false, "v@:"));
}

void PixelTransfer::install() {
    session_class_ = objc.find_class("__AlchemyVTPixelTransferSession");
    if (!session_class_) session_class_ = objc.register_native_class("__AlchemyVTPixelTransferSession", objc.find_class("NSObject"), 8);
    register_type(session_class_, TypeId::vt_pixel_transfer_session);
    const auto cls = session_class_;
    add_dealloc(cls);
    rotation_class_ = objc.find_class("__AlchemyVTPixelRotationSession");
    if (!rotation_class_) rotation_class_ = objc.register_native_class("__AlchemyVTPixelRotationSession", objc.find_class("NSObject"), 8);
    register_type(rotation_class_, TypeId::vt_pixel_rotation_session);
    add_dealloc(rotation_class_);
    for (const auto& [symbol, value] : std::array<std::pair<const char*, const char*>, 11>{{
             {"_kVTPixelTransferPropertyKey_DestinationCleanAperture", "DestinationCleanAperture"},
             {"_kVTPixelTransferPropertyKey_DestinationPixelAspectRatio", "DestinationPixelAspectRatio"},
             {"_kVTPixelTransferPropertyKey_DownsamplingMode", "DownsamplingMode"},
             {"_kVTPixelTransferPropertyKey_DestinationColorPrimaries", "DestinationColorPrimaries"},
             {"_kVTPixelTransferPropertyKey_DestinationTransferFunction", "DestinationTransferFunction"},
             {"_kVTPixelTransferPropertyKey_DestinationICCProfile", "DestinationICCProfile"},
             {"_kVTPixelTransferPropertyKey_DestinationYCbCrMatrix", "DestinationYCbCrMatrix"},
             {"_kVTPixelTransferPropertyKey_RealTime", "RealTime"},
             {"_kVTScalingMode_Letterbox", "Letterbox"},
             {"_kVTScalingMode_Trim", "Trim"},
             {"_kVTDownsamplingMode_Decimate", "Decimate"}}})
        string(video_toolbox, symbol, value);
    string(video_toolbox, "_kVTDownsamplingMode_Average", "Average");
    constant(video_toolbox, "_VTPixelTransferSessionGetTypeID", static_cast<Address>(TypeId::vt_pixel_transfer_session));
    // VTPixelTransferSessionCreate(allocator, VTPixelTransferSessionRef *out)
    raw(video_toolbox, "_VTPixelTransferSessionCreate", [this, cls](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = status(vt_parameter); return; }
        const auto object = objc.allocate(cls);
        static_cast<void>(objc.host_state<TransferSession>(object));
        m.write64(s.x[1], object);
        s.x[0] = 0;
    });
    raw(video_toolbox, "_VTPixelTransferSessionTransferImage", [this](CpuState& s, GuestMemory&) {
        const auto value = session(s.x[0]);
        if (!value || value->invalid || value->kind != TransferSession::Kind::pixel_transfer) {
            s.x[0] = status(s.x[0] ? vt_invalid_session : vt_parameter);
            return;
        }
        s.x[0] = status(transfer(*value, s.x[1], s.x[2]));
    });
    raw(video_toolbox, "_VTPixelTransferSessionInvalidate", [this](CpuState& s, GuestMemory&) {
        if (const auto value = session(s.x[0]); value && value->kind == TransferSession::Kind::pixel_transfer) value->invalid = true;
    });

    // ---- VTPixelRotationSession ------------------------------------------------------------------
    for (const auto& [symbol, value] : std::array<std::pair<const char*, const char*>, 7>{{
             {"_kVTPixelRotationPropertyKey_Rotation", "Rotation"},   // (unverified: naming convention)
             {"_kVTPixelRotationPropertyKey_FlipHorizontalOrientation", "FlipHorizontalOrientation"},   // (unverified: naming convention)
             {"_kVTPixelRotationPropertyKey_FlipVerticalOrientation", "FlipVerticalOrientation"},   // (unverified: naming convention)
             {"_kVTRotation_0", "0"},   // (unverified: naming convention)
             {"_kVTRotation_CW90", "CW90"},   // (unverified: naming convention)
             {"_kVTRotation_180", "180"},   // (unverified: naming convention)
             {"_kVTRotation_CCW90", "CCW90"}}})   // (unverified: naming convention), as are the six above
        string(video_toolbox, symbol, value);
    constant(video_toolbox, "_VTPixelRotationSessionGetTypeID", static_cast<Address>(TypeId::vt_pixel_rotation_session));
    // VTPixelRotationSessionCreate(allocator, VTPixelRotationSessionRef *out)
    raw(video_toolbox, "_VTPixelRotationSessionCreate", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) { s.x[0] = status(vt_parameter); return; }
        const auto object = objc.allocate(rotation_class_);
        objc.host_state<TransferSession>(object)->kind = TransferSession::Kind::pixel_rotation;
        m.write64(s.x[1], object);
        s.x[0] = 0;
    });
    // VTPixelRotationSessionRotateImage(session, CVPixelBufferRef source, CVPixelBufferRef destination)
    raw(video_toolbox, "_VTPixelRotationSessionRotateImage", [this](CpuState& s, GuestMemory&) {
        const auto value = session(s.x[0]);
        if (!value || value->invalid || value->kind != TransferSession::Kind::pixel_rotation) {
            s.x[0] = status(s.x[0] ? vt_invalid_session : vt_parameter);
            return;
        }
        s.x[0] = status(rotate(*value, s.x[1], s.x[2]));
    });
    raw(video_toolbox, "_VTPixelRotationSessionInvalidate", [this](CpuState& s, GuestMemory&) {
        if (const auto value = session(s.x[0]); value && value->kind == TransferSession::Kind::pixel_rotation) value->invalid = true;
    });
    // VTCreateCGImageFromCVPixelBuffer(CVPixelBufferRef, CFDictionaryRef options, CGImageRef *out): +1.
    raw(video_toolbox, "_VTCreateCGImageFromCVPixelBuffer", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) { s.x[0] = status(vt_parameter); return; }
        Address image{};
        const auto result = create_image(s.x[0], image);
        m.write64(s.x[2], image);
        s.x[0] = status(result);
    });
    const auto known = [](const TransferSession& session, const std::string& name) {
        const auto keys = vt_session_keys(session.kind);
        return std::find(keys.begin(), keys.end(), name) != keys.end();
    };
    // VTSessionSetProperty(session, key, value): a decompression session's read-only
    // UsingHardwareAcceleratedVideoDecoder is not set.
    raw(video_toolbox, "_VTSessionSetProperty", [this, known](CpuState& s, GuestMemory&) {
        const auto value = session(s.x[0]);
        if (!value) { s.x[0] = status(s.x[0] ? vt_invalid_session : vt_parameter); return; }
        if (!s.x[1]) { s.x[0] = status(vt_parameter); return; }
        const auto name = cf.utf8(s.x[1]);
        if (!known(*value, name) || name == "UsingHardwareAcceleratedVideoDecoder") { s.x[0] = status(vt_property_not_supported); return; }
        if (value->check_set)
            if (const auto refused = value->check_set(name, s.x[2])) { s.x[0] = status(refused); return; }
        const auto retained = retain(s.x[2]);
        Address old{};
        {
            std::lock_guard lock(value->mutex);
            old = std::exchange(value->properties[name], retained);
        }
        release(old);
        s.x[0] = 0;
    });
    // VTSessionCopyProperty(session, key, allocator, void *valueOut): +1.
    raw(video_toolbox, "_VTSessionCopyProperty", [this, known](CpuState& s, GuestMemory& m) {
        const auto value = session(s.x[0]);
        if (!value) { s.x[0] = status(s.x[0] ? vt_invalid_session : vt_parameter); return; }
        if (!s.x[1] || !s.x[3]) { s.x[0] = status(vt_parameter); return; }
        const auto name = cf.utf8(s.x[1]);
        if (!known(*value, name)) { s.x[0] = status(vt_property_not_supported); return; }
        if (const auto made = value->computed ? value->computed(name) : 0) {
            m.write64(s.x[3], made);
            s.x[0] = 0;
            return;
        }
        Address found{};
        {
            std::lock_guard lock(value->mutex);
            if (const auto it = value->properties.find(name); it != value->properties.end()) found = it->second;
        }
        if (!found && name == "ScalingMode") found = cf.string("Normal");
        else if (!found && name == "Rotation") found = cf.string("0");
        else if (!found && name.starts_with("Flip")) found = retain(cf.boolean(false));
        else if (name == "UsingHardwareAcceleratedVideoDecoder") found = retain(cf.boolean(false));   // software decoders
        else found = retain(found);
        m.write64(s.x[3], found);
        s.x[0] = 0;
    });
    // VTSessionSetProperties(session, CFDictionaryRef)
    raw(video_toolbox, "_VTSessionSetProperties", [this](CpuState& s, GuestMemory&) {
        if (!session(s.x[0])) { s.x[0] = status(s.x[0] ? vt_invalid_session : vt_parameter); return; }
        std::int32_t result = 0;
        if (s.x[1])
            for (const auto& [k, v] : cf.dictionary_entries(s.x[1])) {
                const auto r = static_cast<std::int32_t>(call(video_toolbox, "_VTSessionSetProperty", {s.x[0], k, v}));
                if (r) result = r;
            }
        s.x[0] = status(result);
    });
    // VTSessionCopySupportedPropertyDictionary(session, CFDictionaryRef *out): each key with an empty description.
    raw(video_toolbox, "_VTSessionCopySupportedPropertyDictionary", [this](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write64(s.x[1], 0);
        const auto value = session(s.x[0]);
        if (!value) { s.x[0] = status(s.x[0] ? vt_invalid_session : vt_parameter); return; }
        if (!s.x[1]) { s.x[0] = status(vt_parameter); return; }
        std::vector<std::pair<Address, Address>> entries;
        for (const auto* key : vt_session_keys(value->kind)) entries.emplace_back(cf.string(key), cf.dictionary({}));
        m.write64(s.x[1], cf.dictionary(entries));
        for (const auto& [k, v] : entries) { release(k); release(v); }
        s.x[0] = 0;
    });
}

} // namespace

std::span<const char* const> vt_session_keys(VTSessionState::Kind kind) {
    if (kind == VTSessionState::Kind::decompression) return decompression_keys;
    if (kind == VTSessionState::Kind::pixel_rotation) return rotation_keys;
    if (kind == VTSessionState::Kind::compression) return compression_keys;
    return property_keys;
}

void install_pixel_transfer(InstallContext& context) { std::make_shared<PixelTransfer>(context)->install(); }

} // namespace alchemy::gui::fill
