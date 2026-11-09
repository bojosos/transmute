#include "alchemy/darwin/coremediaio/install.hpp"
#include "alchemy/gui/value_provider.hpp"

#include <algorithm>
#include <array>

// CoreMediaIO: no DAL plug-in objects exist (kCMIOHardwareBadObjectError).
// Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
//
// The DAL plug-in and camera extension side (OBS's virtual camera) runs only inside the system's camera host,
// which does not exist here: a plug-in's objects, devices and streams answer CoreMediaIO's documented errors
// (kCMIOHardwareBadObjectError '!obj', BadDeviceError '!dev', BadStreamError '!str', UnsupportedOperationError
// 'unop'), and the CMIOExtension classes are shells nothing here instantiates. The properties the Swift overlay
// (libswiftCoreMediaIO) reaches are stored as given: the stream properties' activeFormatIndex, frameDuration,
// sinkBufferQueueSize and sinkBuffersRequiredForStartup, the device properties' transportType, and a stream
// format's description, frame durations (CMTimes) and valid frame durations. The CMIOExtensionProperty* strings
// are valued by their names: the public documentation gives no values (unverified).
namespace alchemy::coremediaio {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view core_media_io = "/System/Library/Frameworks/CoreMediaIO.framework/Versions/A/CoreMediaIO";
constexpr std::int32_t cmio_hardware_bad_object = static_cast<std::int32_t>(fill::fourcc("!obj")),
    cmio_hardware_bad_device = static_cast<std::int32_t>(fill::fourcc("!dev")), cmio_hardware_bad_stream = static_cast<std::int32_t>(fill::fourcc("!str")),
    cmio_hardware_unsupported_operation = static_cast<std::int32_t>(fill::fourcc("unop"));

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(core_media_io));
    failure(core_media_io, "_CMIOObjectGetPropertyData", cmio_hardware_bad_object);
    failure(core_media_io, "_CMIOObjectGetPropertyDataSize", cmio_hardware_bad_object);
    failure(core_media_io, "_CMIOObjectSetPropertyData", cmio_hardware_bad_object);
    constant(core_media_io, "_CMIOObjectHasProperty", 0);
    // A DAL plug-in's calls: no plug-in, device, stream or clock exists.
    // CMIOObjectCreate(owningPlugIn, owningObjectID, classID, CMIOObjectID *out)
    raw(core_media_io, "_CMIOObjectCreate", [](CpuState& s, GuestMemory& m) {
        if (s.x[3]) m.write32(s.x[3], 0);
        s.x[0] = fill::status(cmio_hardware_bad_object);
    });
    failure(core_media_io, "_CMIOObjectsPublishedAndDied", cmio_hardware_bad_object);
    failure(core_media_io, "_CMIODeviceStartStream", cmio_hardware_bad_device);
    failure(core_media_io, "_CMIODeviceStopStream", cmio_hardware_bad_device);
    // CMIOStreamCopyBufferQueue(streamID, queueAlteredProc, refCon, CMSimpleQueueRef *queue)
    raw(core_media_io, "_CMIOStreamCopyBufferQueue", [](CpuState& s, GuestMemory& m) {
        if (s.x[3]) m.write64(s.x[3], 0);
        s.x[0] = fill::status(cmio_hardware_bad_stream);
    });
    // CMIOStreamClockCreate(allocator, clockName, sourceIdentifier, CMTime getTimeCallMinimumInterval, numberOfEvents,
    //     numberOfAverages, CFTypeRef *clock): the clock of a plug-in's stream, and no stream exists.
    raw(core_media_io, "_CMIOStreamClockCreate", [](CpuState& s, GuestMemory& m) {
        if (s.x[6]) m.write64(s.x[6], 0);
        s.x[0] = fill::status(cmio_hardware_unsupported_operation);
    });
    failure(core_media_io, "_CMIOStreamClockPostTimingEvent", cmio_hardware_bad_object);   // no clock exists
    failure(core_media_io, "_CMIOStreamClockInvalidate", cmio_hardware_bad_object);
    // CMIOSampleBufferCreateForImageBuffer(allocator, imageBuffer, formatDescription, timing, sequenceNumber,
    //     discontinuityFlags, CMSampleBufferRef *out): a plug-in stream's buffer, and no stream exists to take it.
    raw(core_media_io, "_CMIOSampleBufferCreateForImageBuffer", [](CpuState& s, GuestMemory& m) {
        if (s.x[6]) m.write64(s.x[6], 0);
        s.x[0] = fill::status(cmio_hardware_unsupported_operation);
    });
    // (unverified) Valued by their names.
    for (const auto* name : {"CMIOExtensionPropertyProviderName", "CMIOExtensionPropertyProviderManufacturer", "CMIOExtensionPropertyDeviceModel",
                             "CMIOExtensionPropertyDeviceTransportType", "CMIOExtensionPropertyStreamActiveFormatIndex",
                             "CMIOExtensionPropertyStreamFrameDuration", "CMIOExtensionPropertyStreamSinkBufferQueueSize",
                             "CMIOExtensionPropertyStreamSinkBufferUnderrunCount", "CMIOExtensionPropertyStreamSinkBuffersRequiredForStartup",
                             "CMIOExtensionPropertyStreamSinkEndOfData"})
        string(core_media_io, std::string("_") + name, name);
    // The camera extension classes: a camera extension runs only in the system's camera host.
    for (const auto* name : {"CMIOExtensionProvider", "CMIOExtensionProviderProperties", "CMIOExtensionDevice", "CMIOExtensionStream",
                             "CMIOExtensionScheduledOutput"})
        static_cast<void>(cls(core_media_io, name, "NSObject"));
    const auto stream_properties = cls(core_media_io, "CMIOExtensionStreamProperties", "NSObject");
    object_properties(stream_properties, {"activeFormatIndex", "frameDuration", "sinkBufferQueueSize", "sinkBuffersRequiredForStartup"});
    const auto device_properties = cls(core_media_io, "CMIOExtensionDeviceProperties", "NSObject");
    object_properties(device_properties, {"transportType"});
    // CMIOExtensionStreamFormat: -initWithFormatDescription:maxFrameDuration:minFrameDuration:validFrameDurations: (the
    // CMTimes by reference, as 24-byte arguments travel) and its read-only properties; the CMTimes are kept as their bytes.
    const auto stream_format = cls(core_media_io, "CMIOExtensionStreamFormat", "NSObject");
    object_properties(stream_format, {"formatDescription", "validFrameDurations"});
    method(stream_format, "initWithFormatDescription:maxFrameDuration:minFrameDuration:validFrameDurations:", [this](CpuState& s, GuestMemory& m) {
        store(s.x[0], "formatDescription", s.x[2]);
        for (const auto& [key, at] : {std::pair{"maxFrameDuration", s.x[3]}, std::pair{"minFrameDuration", s.x[4]}}) {
            std::array<std::uint8_t, 24> time{};
            if (at) m.read_bytes(at, time);
            const auto data = bridge->data(time);
            store(s.x[0], key, data);
            release(data);
        }
        store(s.x[0], "validFrameDurations", s.x[5]);
    }, false, "@@:^{opaqueCMFormatDescription=}{?=qiIq}{?=qiIq}@");
    for (const auto* key : {"maxFrameDuration", "minFrameDuration"})
        method(stream_format, key, [this, key = std::string(key)](CpuState& s, GuestMemory& m) {
            std::array<std::uint8_t, 24> time{};   // kCMTimeInvalid when not given
            if (const auto data = stored(s.x[0], key); data && bridge->is_data(data)) {
                const auto bytes = bridge->bytes(data);
                std::copy_n(bytes.begin(), std::min<std::size_t>(bytes.size(), time.size()), time.begin());
            }
            if (s.x[8]) m.write_bytes(s.x[8], time);   // the CMTime by indirect result
        }, false, "{?=qiIq}@:");
}

} // namespace

void install_coremediaio(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::coremediaio
