#include "alchemy/darwin/media/media.hpp"
#include "alchemy/darwin/media/media_codecs.hpp"
#include "alchemy/darwin/media/video_frames.hpp"
#include "alchemy/darwin/media/vt_session.hpp"
#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// VTDecompressionSession (<VideoToolbox/VTDecompressionSession.h>) for AV1 ('av01'), VP9 ('vp09')
// and VP8 ('vp08') video, over media_codecs.hpp's software decoders (dav1d, libvpx).
// - Create reads the format description's codec type, dimensions and its sample-description
//   extension atoms (av1C configuration OBUs). Other codecs (H.264, HEVC, ProRes, ...) answer
//   kVTCouldNotFindVideoDecoderErr, as does a destination format list with no format
//   video_frames.hpp writes.
// - Frames decode synchronously in DecodeFrame / DecodeFrameWithOutputHandler: each picture goes
//   to the output callback (or handler) in presentation order as a new CVPixelBuffer in the first
//   writable kCVPixelBufferPixelFormatTypeKey of the destination attributes (else 420v/420f, x420
//   for more than 8 bits, 422/444 bi-planar for those layouts), made with those attributes
//   (IOSurface, Metal compatibility) at the picture's size, with its Y'CbCr matrix, primaries and
//   transfer function attached. A sample that shows no picture (a VP9 hidden frame) is reported
//   with kVTDecodeInfo_FrameDropped; one the decoder rejects with kVTVideoDecoderBadDataErr.
//   kVTDecodeInfo_Asynchronous is never set, so WaitForAsynchronousFrames has nothing to wait
//   for; FinishDelayedFrames delivers what the decoder still holds.
// - Properties are vt_session.hpp's (VTSessionSetProperty and the rest, pixel_transfer.cpp).
namespace alchemy::gui::fill {
namespace {

namespace codecs = completion_kit::media::codecs;
namespace frames = completion_kit::media::video_frames;
namespace px = completion_kit::media::pixels;

constexpr std::int32_t vt_parameter = -12902, vt_invalid_session = -12903, vt_could_not_find_decoder = -12906, vt_bad_data = -12909;
constexpr std::uint32_t info_frame_dropped = 2;   // kVTDecodeInfo_FrameDropped
constexpr std::int32_t cv_success = 0;

struct InFlight {
    std::int64_t serial{};
    std::array<std::uint8_t, 24> pts{}, duration{};   // CMTime
    Address source_refcon{};
};

struct Decoding {
    std::mutex mutex;
    std::unique_ptr<codecs::VideoDecoder> decoder;
    std::uint32_t codec{};
    std::int32_t width{}, height{};
    std::vector<std::uint8_t> config;
    Address format{};                   // retained CMVideoFormatDescription
    Address attributes{};               // retained destination image buffer attributes, or 0
    std::vector<std::uint32_t> formats; // the destination formats requested, in order
    Address callback{}, refcon{};       // VTDecompressionOutputCallbackRecord
    std::int64_t serial{};
    std::deque<InFlight> in_flight;
};

class Decompression final : public Provider {
public:
    using Provider::Provider;
    void install();

private:
    std::shared_ptr<Decoding> decoding(Address session) {
        std::lock_guard lock(mutex_);
        const auto found = sessions_.find(session);
        return found == sessions_.end() ? nullptr : found->second;
    }
    // The session's state, or the status to answer.
    std::int32_t check(Address session, std::shared_ptr<Decoding>& out) {
        if (!session) return vt_parameter;
        const auto state = objc.find_host_state<VTSessionState>(session);
        out = decoding(session);
        if (!state || state->kind != VTSessionState::Kind::decompression || !out) return vt_invalid_session;
        return state->invalid ? vt_invalid_session : 0;
    }
    std::int32_t create(GuestMemory& m, Address format, Address attributes, Address record, Address out);
    // Decodes a sample buffer and delivers its pictures through `deliver(status, flags, image, pts, duration, source_refcon)`.
    using Deliver = std::function<void(std::int32_t, std::uint32_t, Address, Address, Address, Address)>;
    std::int32_t decode(Decoding& d, Address sample, Address source_refcon, const Deliver& deliver);
    void deliver_ready(Decoding& d, const Deliver& deliver, std::optional<std::int64_t> sent);
    Address pixel_buffer(Decoding& d, const codecs::VideoFrame& frame);
    void attach(Address buffer, const codecs::VideoFrame& frame);
    // Calls `deliver` with the CMTimes copied into guest memory.
    void call_deliver(const Deliver& deliver, std::int32_t status, std::uint32_t flags, Address image, const InFlight& entry);

    std::mutex mutex_;
    std::map<Address, std::shared_ptr<Decoding>> sessions_;
    Address class_{};
};

std::int32_t Decompression::create(GuestMemory& m, Address format, Address attributes, Address record, Address out) {
    if (!format || !out) return vt_parameter;
    if (call(core_media, "_CMFormatDescriptionGetMediaType", {format}) != fourcc("vide")) return vt_parameter;
    const auto codec = static_cast<std::uint32_t>(call(core_media, "_CMFormatDescriptionGetMediaSubType", {format}));
    if (!codecs::video_decoder_available(codec)) return vt_could_not_find_decoder;   // H.264, HEVC, ProRes, ...
    auto d = std::make_shared<Decoding>();
    d->codec = codec;
    const auto dimensions = call(core_media, "_CMVideoFormatDescriptionGetDimensions", {format});   // CMVideoDimensions in x0
    d->width = static_cast<std::int32_t>(dimensions & 0xffffffff);
    d->height = static_cast<std::int32_t>(dimensions >> 32);
    // The codec configuration record from the sample-description extension atoms.
    const auto atoms_key = cf.string("SampleDescriptionExtensionAtoms");
    const auto atoms = call(core_media, "_CMFormatDescriptionGetExtension", {format, atoms_key});
    release(atoms_key);
    if (atoms && cf.kind(atoms) == ObjectKind::dictionary) {
        const auto name = cf.string(codec == fourcc("av01") ? "av1C" : "vpcC");
        const auto value = cf.dictionary_value(atoms, name);
        release(name);
        if (value && bridge->is_data(value)) {
            const auto bytes = bridge->bytes(value);
            d->config.assign(bytes.begin(), bytes.end());
        }
    }
    std::string why;
    d->decoder = codecs::make_video_decoder(codec, d->config, why);
    if (!d->decoder) return vt_could_not_find_decoder;
    // The destination formats asked for: a number or an array of numbers.
    if (attributes) {
        const auto key = cf.string("PixelFormatType");
        const auto value = cf.dictionary_value(attributes, key);
        release(key);
        std::vector<Address> numbers;
        if (value && cf.kind(value) == ObjectKind::array) numbers = cf.array_values(value);
        else if (value) numbers.push_back(value);
        for (const auto number : numbers)
            if (const auto v = cf.number_value(number)) d->formats.push_back(static_cast<std::uint32_t>(*v));
        if (!d->formats.empty() && std::none_of(d->formats.begin(), d->formats.end(), frames::writable)) return vt_could_not_find_decoder;
        d->attributes = retain(attributes);
    }
    d->format = retain(format);
    if (record) {
        d->callback = m.read64(record);
        d->refcon = m.read64(record + 8);
    }
    const auto session = objc.allocate(class_);
    objc.host_state<VTSessionState>(session)->kind = VTSessionState::Kind::decompression;
    {
        std::lock_guard lock(mutex_);
        sessions_[session] = std::move(d);
    }
    m.write64(out, session);
    return 0;
}

void Decompression::attach(Address buffer, const codecs::VideoFrame& f) {
    const auto set = [&](const char* key, const char* value) {
        if (!value) return;
        const auto k = cf.string(key), v = cf.string(value);
        static_cast<void>(call(core_video, "_CVBufferSetAttachment", {buffer, k, v, 1}));   // kCVAttachmentMode_ShouldPropagate
        release(k);
        release(v);
    };
    const char* matrix = nullptr;
    switch (frames::matrix_of(f)) {
    case px::Matrix::bt601: matrix = "ITU_R_601_4"; break;
    case px::Matrix::bt2020: matrix = "ITU_R_2020"; break;
    default: matrix = "ITU_R_709_2"; break;
    }
    if (f.matrix == 7) matrix = "SMPTE_240M_1995";
    set("CVImageBufferYCbCrMatrix", matrix);
    const char* primaries = f.primaries == 1 ? "ITU_R_709_2" : f.primaries == 5 ? "EBU_3213" : f.primaries == 6 || f.primaries == 7 ? "SMPTE_C"
        : f.primaries == 9 ? "ITU_R_2020" : nullptr;
    set("CVImageBufferColorPrimaries", primaries);
    const char* transfer = f.transfer == 1 || f.transfer == 6 || f.transfer == 14 || f.transfer == 15 ? "ITU_R_709_2" : f.transfer == 7 ? "SMPTE_240M_1995"
        : f.transfer == 8 ? "Linear" : f.transfer == 13 ? "IEC_sRGB" : f.transfer == 16 ? "SMPTE_ST_2084_PQ" : f.transfer == 18 ? "ITU_R_2100_HLG"
        : f.transfer == 17 ? "SMPTE_ST_428_1" : nullptr;
    set("CVImageBufferTransferFunction", transfer);
}

Address Decompression::pixel_buffer(Decoding& d, const codecs::VideoFrame& f) {
    auto format = frames::native_format(f);
    for (const auto requested : d.formats)
        if (frames::writable(requested)) { format = requested; break; }
    auto& m = process.calling_engine();
    const auto out = process.allocate_guest(8, 8);
    m.write64(out, 0);
    // CVPixelBufferCreate(allocator, width, height, format, attributes, CVPixelBufferRef *out)
    const auto result = static_cast<std::int32_t>(call(core_video, "_CVPixelBufferCreate", {0, f.width, f.height, format, d.attributes, out}));
    const auto buffer = m.read64(out);
    process.release_guest(out);
    if (result != cv_success || !buffer) return 0;
    static_cast<void>(call(core_video, "_CVPixelBufferLockBaseAddress", {buffer, 0}));
    const auto planes = call(core_video, "_CVPixelBufferGetPlaneCount", {buffer});
    std::vector<Address> bases;
    std::vector<std::vector<std::uint8_t>> storage;
    std::vector<px::Plane> views;
    for (Address p = 0; p < std::max<Address>(planes, 1); ++p) {
        const auto base = planes ? call(core_video, "_CVPixelBufferGetBaseAddressOfPlane", {buffer, p}) : call(core_video, "_CVPixelBufferGetBaseAddress", {buffer});
        const auto row = planes ? call(core_video, "_CVPixelBufferGetBytesPerRowOfPlane", {buffer, p}) : call(core_video, "_CVPixelBufferGetBytesPerRow", {buffer});
        const auto rows = planes ? call(core_video, "_CVPixelBufferGetHeightOfPlane", {buffer, p}) : call(core_video, "_CVPixelBufferGetHeight", {buffer});
        bases.push_back(base);
        storage.emplace_back(static_cast<std::size_t>(row * rows));
        if (base) m.read_bytes(base, storage.back());   // keep the padding as it is
        views.push_back({storage.back().data(), row});
    }
    const bool written = frames::write(f, format, views);
    for (std::size_t p = 0; p < bases.size(); ++p)
        if (written && bases[p]) m.write_bytes(bases[p], storage[p]);
    static_cast<void>(call(core_video, "_CVPixelBufferUnlockBaseAddress", {buffer, 0}));
    if (!written) {
        release(buffer);
        return 0;
    }
    attach(buffer, f);
    return buffer;
}

void Decompression::call_deliver(const Deliver& deliver, std::int32_t status_value, std::uint32_t flags, Address image, const InFlight& entry) {
    const auto times = process.allocate_guest(48, 8);
    process.calling_engine().write_bytes(times, entry.pts);
    process.calling_engine().write_bytes(times + 24, entry.duration);
    deliver(status_value, flags, image, times, times + 24, entry.source_refcon);
    process.release_guest(times);
}

// The pictures the decoder has ready, each to `deliver` with the timing of the sample that made
// it; then, when the sample `sent` made none, a dropped-frame report for it.
void Decompression::deliver_ready(Decoding& d, const Deliver& deliver, std::optional<std::int64_t> sent) {
    codecs::VideoFrame f;
    bool sent_shown = false;
    while (d.decoder->receive(f)) {
        const auto found = std::find_if(d.in_flight.begin(), d.in_flight.end(), [&](const InFlight& e) { return e.serial == f.pts; });
        if (found == d.in_flight.end()) continue;
        const auto entry = *found;
        d.in_flight.erase(found);
        if (sent && entry.serial == *sent) sent_shown = true;
        const auto image = pixel_buffer(d, f);
        call_deliver(deliver, image ? 0 : vt_bad_data, image ? 0 : info_frame_dropped, image, entry);
        release(image);
    }
    if (sent && !sent_shown) {
        const auto found = std::find_if(d.in_flight.begin(), d.in_flight.end(), [&](const InFlight& e) { return e.serial == *sent; });
        // A sample that shows nothing now (a hidden frame) and will not later (no reordering here).
        if (found != d.in_flight.end()) {
            const auto entry = *found;
            d.in_flight.erase(found);
            call_deliver(deliver, 0, info_frame_dropped, 0, entry);
        }
    }
}

std::int32_t Decompression::decode(Decoding& d, Address sample, Address source_refcon, const Deliver& deliver) {
    if (!sample) return vt_parameter;
    auto& m = process.calling_engine();
    InFlight entry;
    entry.serial = ++d.serial;
    entry.source_refcon = source_refcon;
    // The sample's bytes and times.
    const auto block = call(core_media, "_CMSampleBufferGetDataBuffer", {sample});
    const auto length = block ? call(core_media, "_CMBlockBufferGetDataLength", {block}) : 0;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (length) {
        const auto scratch = process.allocate_guest(length, 16);
        const auto copied = static_cast<std::int32_t>(call(core_media, "_CMBlockBufferCopyDataBytes", {block, 0, length, scratch}));
        if (!copied) m.read_bytes(scratch, bytes);
        process.release_guest(scratch);
        if (copied) bytes.clear();
    }
    const auto times = process.allocate_guest(24, 8);
    for (const auto* getter : {"_CMSampleBufferGetPresentationTimeStamp", "_CMSampleBufferGetDuration"}) {
        CallRegs r{};
        r.x[0] = sample;
        r.indirect_result = times;
        static_cast<void>(call_regs(core_media, getter, r));
        m.read_bytes(times, std::string_view(getter).ends_with("Duration") ? std::span(entry.duration) : std::span(entry.pts));
    }
    process.release_guest(times);
    if (bytes.empty()) {
        call_deliver(deliver, vt_bad_data, info_frame_dropped, 0, entry);
        return vt_bad_data;
    }
    d.in_flight.push_back(entry);
    std::string why;
    if (!d.decoder->send(bytes, entry.serial, why)) {
        d.in_flight.pop_back();
        call_deliver(deliver, vt_bad_data, info_frame_dropped, 0, entry);
        return vt_bad_data;
    }
    deliver_ready(d, deliver, entry.serial);
    return 0;
}

void Decompression::install() {
    class_ = objc.find_class("__AlchemyVTDecompressionSession");
    if (!class_) class_ = objc.register_native_class("__AlchemyVTDecompressionSession", objc.find_class("NSObject"), 8);
    register_type(class_, TypeId::vt_decompression_session);
    const auto cls = class_;
    static_cast<void>(objc.add_native_method(cls, "dealloc", [self = shared_from_this(), this, cls](CpuState& s, GuestMemory&) {
        if (const auto value = objc.find_host_state<VTSessionState>(s.x[0])) {
            std::map<std::string, Address> properties;
            {
                std::lock_guard lock(value->mutex);
                properties.swap(value->properties);
            }
            for (const auto& [name, v] : properties) release(v);
        }
        std::shared_ptr<Decoding> gone;
        {
            std::lock_guard lock(mutex_);
            if (const auto found = sessions_.find(s.x[0]); found != sessions_.end()) {
                gone = std::move(found->second);
                sessions_.erase(found);
            }
        }
        if (gone) {
            release(std::exchange(gone->format, 0));
            release(std::exchange(gone->attributes, 0));
        }
        static_cast<void>(objc.send_super(s.x[0], cls, objc.selector("dealloc")));
    }, false, "v@:"));

    constant(video_toolbox, "_VTDecompressionSessionGetTypeID", static_cast<Address>(TypeId::vt_decompression_session));
    // VTDecompressionSessionCreate(allocator, formatDescription, decoderSpecification, destinationImageBufferAttributes,
    //     const VTDecompressionOutputCallbackRecord *, VTDecompressionSessionRef *out)
    raw(video_toolbox, "_VTDecompressionSessionCreate", [this](CpuState& s, GuestMemory& m) {
        if (s.x[5]) m.write64(s.x[5], 0);
        s.x[0] = status(create(m, s.x[1], s.x[3], s.x[4], s.x[5]));
    });
    // VTDecompressionSessionDecodeFrame(session, sampleBuffer, decodeFlags, sourceFrameRefCon, VTDecodeInfoFlags *infoFlagsOut):
    // the output callback (decompressionOutputRefCon, sourceFrameRefCon, status, infoFlags, imageBuffer, CMTime pts, CMTime duration),
    // the CMTimes by reference.
    raw(video_toolbox, "_VTDecompressionSessionDecodeFrame", [this](CpuState& s, GuestMemory& m) {
        if (s.x[4]) m.write32(s.x[4], 0);
        std::shared_ptr<Decoding> d;
        if (const auto result = check(s.x[0], d)) { s.x[0] = status(result); return; }
        std::lock_guard lock(d->mutex);
        if (!d->callback) { s.x[0] = status(vt_parameter); return; }   // made for an output handler
        const auto callback = d->callback, refcon = d->refcon;
        const Deliver deliver = [this, callback, refcon](std::int32_t result, std::uint32_t flags, Address image, Address pts, Address duration, Address source) {
            static_cast<void>(EngineAdapter(process.calling_engine()).call(callback, std::array<Address, 7>{refcon, source,
                static_cast<Address>(static_cast<std::int64_t>(result)), flags, image, pts, duration}));
        };
        s.x[0] = status(decode(*d, s.x[1], s.x[3], deliver));
    });
    // VTDecompressionSessionDecodeFrameWithOutputHandler(session, sampleBuffer, decodeFlags, VTDecodeInfoFlags *infoFlagsOut,
    //     handler(OSStatus, VTDecodeInfoFlags, CVImageBufferRef, CMTime pts, CMTime duration))
    raw(video_toolbox, "_VTDecompressionSessionDecodeFrameWithOutputHandler", [this](CpuState& s, GuestMemory& m) {
        if (s.x[3]) m.write32(s.x[3], 0);
        std::shared_ptr<Decoding> d;
        if (const auto result = check(s.x[0], d)) { s.x[0] = status(result); return; }
        if (!s.x[4]) { s.x[0] = status(vt_parameter); return; }
        std::lock_guard lock(d->mutex);
        const auto handler = s.x[4];
        const Deliver deliver = [this, handler](std::int32_t result, std::uint32_t flags, Address image, Address pts, Address duration, Address) {
            static_cast<void>(bridge->invoke_block(handler, {static_cast<Address>(static_cast<std::int64_t>(result)), flags, image, pts, duration}));
        };
        s.x[0] = status(decode(*d, s.x[1], 0, deliver));
    });
    // VTDecompressionSessionFinishDelayedFrames(session): what the decoder holds, to the callback.
    raw(video_toolbox, "_VTDecompressionSessionFinishDelayedFrames", [this](CpuState& s, GuestMemory&) {
        std::shared_ptr<Decoding> d;
        if (const auto result = check(s.x[0], d)) { s.x[0] = status(result); return; }
        std::lock_guard lock(d->mutex);
        d->decoder->drain();
        if (d->callback) {
            const auto callback = d->callback, refcon = d->refcon;
            deliver_ready(*d, [this, callback, refcon](std::int32_t result, std::uint32_t flags, Address image, Address pts, Address duration, Address source) {
                static_cast<void>(EngineAdapter(process.calling_engine()).call(callback, std::array<Address, 7>{refcon, source,
                    static_cast<Address>(static_cast<std::int64_t>(result)), flags, image, pts, duration}));
            }, std::nullopt);
        }
        d->in_flight.clear();
        s.x[0] = 0;
    });
    // Decoding is synchronous: nothing is ever pending.
    raw(video_toolbox, "_VTDecompressionSessionWaitForAsynchronousFrames", [this](CpuState& s, GuestMemory&) {
        std::shared_ptr<Decoding> d;
        s.x[0] = status(check(s.x[0], d));
    });
    raw(video_toolbox, "_VTDecompressionSessionInvalidate", [this](CpuState& s, GuestMemory&) {
        std::shared_ptr<Decoding> d;
        if (check(s.x[0], d)) return;
        objc.host_state<VTSessionState>(s.x[0])->invalid = true;
        std::lock_guard lock(d->mutex);
        d->decoder.reset();
        d->in_flight.clear();
    });
    // VTDecompressionSessionCanAcceptFormatDescription(session, formatDescription): the same codec and size.
    raw(video_toolbox, "_VTDecompressionSessionCanAcceptFormatDescription", [this](CpuState& s, GuestMemory&) {
        std::shared_ptr<Decoding> d;
        if (check(s.x[0], d) || !s.x[1]) { s.x[0] = 0; return; }
        const auto codec = static_cast<std::uint32_t>(call(core_media, "_CMFormatDescriptionGetMediaSubType", {s.x[1]}));
        const auto dimensions = call(core_media, "_CMVideoFormatDescriptionGetDimensions", {s.x[1]});
        s.x[0] = codec == d->codec && static_cast<std::int32_t>(dimensions & 0xffffffff) == d->width &&
                 static_cast<std::int32_t>(dimensions >> 32) == d->height ? 1 : 0;
    });
    for (const auto& [symbol, value] : std::array<std::pair<const char*, const char*>, 10>{{
             {"_kVTDecompressionPropertyKey_RealTime", "RealTime"},
             {"_kVTDecompressionPropertyKey_ThreadCount", "ThreadCount"},
             {"_kVTDecompressionPropertyKey_MaximizePowerEfficiency", "MaximizePowerEfficiency"},
             {"_kVTDecompressionPropertyKey_OutputPoolRequestedMinimumBufferCount", "OutputPoolRequestedMinimumBufferCount"},
             {"_kVTDecompressionPropertyKey_PixelTransferProperties", "PixelTransferProperties"},
             {"_kVTDecompressionPropertyKey_ReducedResolutionDecode", "ReducedResolutionDecode"},
             {"_kVTDecompressionPropertyKey_FieldMode", "FieldMode"},
             {"_kVTDecompressionPropertyKey_DeinterlaceMode", "DeinterlaceMode"},
             {"_kVTDecompressionPropertyKey_PropagatePerFrameHDRDisplayMetadata", "PropagatePerFrameHDRDisplayMetadata"},
             {"_kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder", "UsingHardwareAcceleratedVideoDecoder"}}})
        string(video_toolbox, symbol, value);
}

} // namespace

void install_vt_decompression(InstallContext& context) { std::make_shared<Decompression>(context)->install(); }

} // namespace alchemy::gui::fill
