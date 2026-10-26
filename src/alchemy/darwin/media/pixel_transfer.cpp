#include "alchemy/darwin/media/media.hpp"
#include "alchemy/darwin/media/pixel_convert.hpp"

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
// CopySupportedPropertyDictionary work on transfer sessions; any other object is not a session
// (kVTInvalidSessionErr), as no decompression or compression session exists.
namespace alchemy::gui::fill {
namespace {

namespace px = completion_kit::media::pixels;

constexpr std::int32_t vt_property_not_supported = -12900, vt_parameter = -12902, vt_invalid_session = -12903,
    vt_pixel_transfer_not_supported = -12905;

struct TransferSession final : ObjectPayload {
    std::mutex mutex;
    std::map<std::string, Address> properties;   // retained values
    bool invalid{};
};

constexpr std::array<const char*, 9> property_keys{"ScalingMode", "DestinationCleanAperture", "DestinationPixelAspectRatio", "DownsamplingMode",
                                                   "DestinationColorPrimaries", "DestinationTransferFunction", "DestinationICCProfile",
                                                   "DestinationYCbCrMatrix", "RealTime"};

class PixelTransfer final : public Provider {
public:
    using Provider::Provider;
    void install();

private:
    std::shared_ptr<TransferSession> session(Address object) const { return object ? objc.find_host_state<TransferSession>(object) : nullptr; }
    std::int32_t transfer(TransferSession& session, Address source, Address destination);
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

    Address session_class_{};
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
        const bool ycbcr = to->format != 0x20 && to->format != 0x18 && to->format != fourcc("BGRA") && to->format != fourcc("RGBA") &&
            to->format != fourcc("ABGR") && to->format != fourcc("24BG") && to->format != fourcc("L008");
        if (ycbcr) {
            const auto name = cf.string(matrix_name(to->matrix));
            static_cast<void>(call(core_video, "_CVBufferSetAttachment", {destination, key, name, 1}));
            release(name);
        }
        release(key);
    }
    static_cast<void>(call(core_video, "_CVPixelBufferUnlockBaseAddress", {destination, 0}));
    static_cast<void>(call(core_video, "_CVPixelBufferUnlockBaseAddress", {source, 1}));
    return result;
}

void PixelTransfer::install() {
    session_class_ = objc.find_class("__AlchemyVTPixelTransferSession");
    if (!session_class_) session_class_ = objc.register_native_class("__AlchemyVTPixelTransferSession", objc.find_class("NSObject"), 8);
    register_type(session_class_, TypeId::vt_pixel_transfer_session);
    const auto cls = session_class_;
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
        if (!value || value->invalid) { s.x[0] = status(s.x[0] ? vt_invalid_session : vt_parameter); return; }
        s.x[0] = status(transfer(*value, s.x[1], s.x[2]));
    });
    raw(video_toolbox, "_VTPixelTransferSessionInvalidate", [this](CpuState& s, GuestMemory&) {
        if (const auto value = session(s.x[0])) value->invalid = true;
    });
    const auto known = [](const std::string& name) { return std::find(property_keys.begin(), property_keys.end(), name) != property_keys.end(); };
    // VTSessionSetProperty(session, key, value)
    raw(video_toolbox, "_VTSessionSetProperty", [this, known](CpuState& s, GuestMemory&) {
        const auto value = session(s.x[0]);
        if (!value) { s.x[0] = status(s.x[0] ? vt_invalid_session : vt_parameter); return; }
        if (!s.x[1]) { s.x[0] = status(vt_parameter); return; }
        const auto name = cf.utf8(s.x[1]);
        if (!known(name)) { s.x[0] = status(vt_property_not_supported); return; }
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
        if (!known(name)) { s.x[0] = status(vt_property_not_supported); return; }
        Address found{};
        {
            std::lock_guard lock(value->mutex);
            if (const auto it = value->properties.find(name); it != value->properties.end()) found = it->second;
        }
        if (!found && name == "ScalingMode") found = cf.string("Normal");
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
        if (!session(s.x[0])) { s.x[0] = status(s.x[0] ? vt_invalid_session : vt_parameter); return; }
        if (!s.x[1]) { s.x[0] = status(vt_parameter); return; }
        std::vector<std::pair<Address, Address>> entries;
        for (const auto* key : property_keys) entries.emplace_back(cf.string(key), cf.dictionary({}));
        m.write64(s.x[1], cf.dictionary(entries));
        for (const auto& [k, v] : entries) { release(k); release(v); }
        s.x[0] = 0;
    });
}

} // namespace

void install_pixel_transfer(InstallContext& context) { std::make_shared<PixelTransfer>(context)->install(); }

} // namespace alchemy::gui::fill
