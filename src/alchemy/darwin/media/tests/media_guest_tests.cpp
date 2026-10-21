// QLab media group (G5) exports through the strict-mode provider fixture: the
// guest calling convention of the CoreMedia CMTime/sample-buffer additions,
// CoreVideo, the Accelerate kernels, CoreMIDI over a fake device backend,
// the CoreAudio HAL over a fake device backend, AudioToolbox (MusicSequence,
// AudioFile/ExtAudioFile, AU listeners) and the AVFoundation classes.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/midi_core.hpp"
#include "alchemy/darwin/media/hal_core.hpp"

#include "alchemy/path.hpp"
#include "alchemy/darwin/security/bridge.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <map>
#include <mutex>

namespace {
using namespace fill_test;

#define FRAMEWORK(name) "/System/Library/Frameworks/" name ".framework/Versions/A/" name
constexpr std::string_view cm = FRAMEWORK("CoreMedia"), cv = FRAMEWORK("CoreVideo"), midi_lib = FRAMEWORK("CoreMIDI"),
    acc = FRAMEWORK("Accelerate"), vt = FRAMEWORK("VideoToolbox"), av = FRAMEWORK("AVFoundation"),
    ca = FRAMEWORK("CoreAudio"), at = FRAMEWORK("AudioToolbox"), au = FRAMEWORK("AudioUnit");
#undef FRAMEWORK

namespace midi = alchemy::gui::completion_kit::media::midi;

struct FakeHal final : alchemy::gui::completion_kit::media::hal::Backend {
    alchemy::gui::completion_kit::media::hal::Snapshot snapshot() override {
        alchemy::gui::completion_kit::media::hal::Snapshot result;
        result.devices.push_back({"{render}", "Line Out", "", false, 2, 48000, 3, 2});
        result.default_output = "{render}";
        return result;
    }
};

struct FakeMidi final : midi::Backend {
    std::vector<midi::PortInfo> inputs() override { return {{"Port A", "Maker", "drv", false}}; }
    std::vector<midi::PortInfo> outputs() override { return {{"Port A", "Maker", "drv", false}}; }
    bool open_input(std::size_t, Input) override { return true; }
    void close_input(std::size_t) override {}
    bool send(std::size_t, std::span<const std::uint8_t> message) override {
        std::lock_guard lock(mutex());
        sent().emplace_back(message.begin(), message.end());
        return true;
    }
    static std::mutex& mutex() { static std::mutex value; return value; }
    static std::vector<std::vector<std::uint8_t>>& sent() { static std::vector<std::vector<std::uint8_t>> value; return value; }
};

void write_time(Fixture& f, Address at, std::int64_t value, std::int32_t timescale, std::uint32_t flags = 1) {
    f.engine.write64(at, static_cast<std::uint64_t>(value));
    f.engine.write32(at + 8, static_cast<std::uint32_t>(timescale));
    f.engine.write32(at + 12, flags);
    f.engine.write64(at + 16, 0);
}

void core_media(Fixture& f) {
    const auto a = f.block(24), b = f.block(24), out = f.block(48);
    write_time(f, a, 1, 3);
    write_time(f, b, 1, 2);
    alchemy::CallRegs r{};
    r.x[0] = a; r.x[1] = b; r.indirect_result = out;
    static_cast<void>(f.regs(cm, "_CMTimeAdd", r));
    require(f.engine.read64(out) == 5 && f.engine.read32(out + 8) == 6 && f.engine.read32(out + 12) == 1, "CMTimeAdd");
    require(static_cast<std::int32_t>(f.regs(cm, "_CMTimeCompare", r).x0) == -1, "CMTimeCompare");
    alchemy::CallRegs seconds{};
    seconds.set_d(0, 2.5);
    seconds.x[0] = 600;
    seconds.indirect_result = out;
    static_cast<void>(f.regs(cm, "_CMTimeMakeWithSeconds", seconds));
    require(f.engine.read64(out) == 1500 && f.engine.read32(out + 8) == 600, "CMTimeMakeWithSeconds");
    alchemy::CallRegs scaled{};
    scaled.x[0] = a;
    scaled.set_d(0, 3.0);
    scaled.indirect_result = out;
    static_cast<void>(f.regs(cm, "_CMTimeMultiplyByFloat64", scaled));
    require(f.engine.read64(out) == 3 && f.engine.read32(out + 8) == 3, "CMTimeMultiplyByFloat64");
    const auto zero = f.symbol(cm, "_kCMTimeZero"), infinity = f.symbol(cm, "_kCMTimePositiveInfinity");
    require(f.engine.read32(zero + 8) == 1 && f.engine.read32(zero + 12) == 1 && f.engine.read32(infinity + 12) == 5, "time constants");
    const auto description = f.call(cm, "_CMTimeCopyDescription", {0, b});
    require(f.cf.utf8(description) == "{1/2 = 0.500}", "CMTimeCopyDescription");
    alchemy::CallRegs range{};
    range.x[0] = a; range.x[1] = b; range.indirect_result = out;
    static_cast<void>(f.regs(cm, "_CMTimeRangeMake", range));
    const auto end = f.block(24);
    alchemy::CallRegs get_end{};
    get_end.x[0] = out; get_end.indirect_result = end;
    static_cast<void>(f.regs(cm, "_CMTimeRangeGetEnd", get_end));
    require(f.engine.read64(end) == 5 && f.engine.read32(end + 8) == 6, "CMTimeRangeGetEnd");

    // An interleaved stereo float audio description and a 4-frame sample buffer.
    const auto asbd = f.block(40);
    f.engine.write64(asbd, std::bit_cast<std::uint64_t>(48000.0));
    f.engine.write32(asbd + 8, 0x6c70636d);   // 'lpcm'
    f.engine.write32(asbd + 12, 9);           // float | packed
    f.engine.write32(asbd + 16, 8); f.engine.write32(asbd + 20, 1); f.engine.write32(asbd + 24, 8);
    f.engine.write32(asbd + 28, 2); f.engine.write32(asbd + 32, 32);
    const auto format_out = f.block(8);
    require(f.status(cm, "_CMAudioFormatDescriptionCreate", {0, asbd, 0, 0, 0, 0, 0, format_out}) == 0, "CMAudioFormatDescriptionCreate");
    const auto format = f.engine.read64(format_out);
    const auto stream = f.call(cm, "_CMAudioFormatDescriptionGetStreamBasicDescription", {format});
    require(stream && f.engine.read32(stream + 28) == 2, "stream description");
    const auto block_out = f.block(8);
    alchemy::CallRegs block{};
    block.x[2] = 32; block.x[6] = 32; block.x[7] = 1;
    block.push_stack(block_out);
    require(f.regs(cm, "_CMBlockBufferCreateWithMemoryBlock", block).x0 == 0, "block buffer");
    const auto timing = f.block(72);
    write_time(f, timing, 1, 48000);
    write_time(f, timing + 24, 0, 48000);
    write_time(f, timing + 48, 0, 0, 0);
    const auto sample_out = f.block(8);
    alchemy::CallRegs create{};
    create.x[1] = f.engine.read64(block_out); create.x[2] = format; create.x[3] = 4; create.x[4] = 1; create.x[5] = timing;
    create.push_stack(sample_out);
    require(f.regs(cm, "_CMSampleBufferCreateReady", create).x0 == 0, "sample buffer");
    const auto sample = f.engine.read64(sample_out);
    require(f.call(cm, "_CMSampleBufferGetNumSamples", {sample}) == 4 && f.call(cm, "_CMSampleBufferIsValid", {sample}) == 1, "num samples");
    alchemy::CallRegs duration{};
    duration.x[0] = sample; duration.indirect_result = out;
    static_cast<void>(f.regs(cm, "_CMSampleBufferGetDuration", duration));
    require(f.engine.read64(out) == 4 && f.engine.read32(out + 8) == 48000, "sample buffer duration");
    const auto needed = f.block(8), list = f.block(64), retained = f.block(8);
    require(f.status(cm, "_CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer", {sample, needed, list, 64, 0, 0, 0, retained}) == 0, "audio buffer list");
    require(f.engine.read64(needed) == 24 && f.engine.read32(list) == 1 && f.engine.read32(list + 8) == 2 && f.engine.read32(list + 12) == 32 &&
            f.engine.read64(retained) == f.engine.read64(block_out), "interleaved list");
}

void core_video(Fixture& f) {
    require(f.call(cv, "_CVGetCurrentHostTime") > 0, "host time");
    alchemy::CallRegs none{};
    require(f.regs(cv, "_CVGetHostClockFrequency", none).d(0) == 1e9, "host clock frequency");
    const auto key = f.engine.read64(f.symbol(cv, "_kCVImageBufferPixelAspectRatioKey"));
    require(f.cf.utf8(key) == "CVPixelAspectRatio", "pixel aspect ratio key");
    require(f.call(cv, "_CVBufferRetain", {0}) == 0, "retain NULL");
    const auto out = f.block(8);
    require(f.status(cv, "_CVMetalTextureCacheCreate", {0, 0, 0, 0, out}) == -6661, "texture cache needs a device");
    alchemy::CallRegs bytes{};
    bytes.x[1] = 16; bytes.x[2] = 16; bytes.x[3] = 0x42475241;
    bytes.push_stack(Address{0});
    bytes.push_stack(out);
    require(static_cast<std::int32_t>(f.regs(cv, "_CVPixelBufferCreateWithBytes", bytes).x0) == -6663 && f.engine.read64(out) == 0, "no pixel buffers");
    require(static_cast<std::int32_t>(f.call(vt, "_VTDecompressionSessionDecodeFrameWithOutputHandler", {0, 0, 0, 0, 0})) == -12902, "no session");
    require(f.string_constant(av, "_AVMetadataCommonKeyTitle") == "title" && f.string_constant(av, "_AVSampleRateKey") == "AVSampleRateKey", "AV keys");
}

std::vector<float> floats(Fixture& f, Address at, std::size_t count) {
    std::vector<float> values(count);
    for (std::size_t i = 0; i < count; ++i) values[i] = std::bit_cast<float>(f.engine.read32(at + i * 4));
    return values;
}
Address float_block(Fixture& f, std::initializer_list<float> values) {
    const auto at = f.block(values.size() * 4 + 4);
    std::size_t i = 0;
    for (const auto value : values) f.engine.write32(at + 4 * i++, std::bit_cast<std::uint32_t>(value));
    return at;
}

// vDSP kernels against reference loops; BLAS/LAPACK on small systems.
void accelerate(Fixture& f) {
    const auto a = float_block(f, {1, -4, 3, 2}), b = float_block(f, {-2, 1, 5, 0.5f}), out = f.block(64);
    static_cast<void>(f.call(acc, "_vDSP_maxv", {a, 1, out, 4}));
    require(floats(f, out, 1)[0] == 3, "maxv");
    static_cast<void>(f.call(acc, "_vDSP_rmsqv", {a, 1, out, 4}));
    require(std::fabs(floats(f, out, 1)[0] - std::sqrt(30.0f / 4)) < 1e-6, "rmsqv");
    static_cast<void>(f.call(acc, "_vDSP_vmaxmg", {a, 1, b, 1, out, 1, 4}));
    require(floats(f, out, 4) == std::vector<float>({2, 4, 5, 2}), "vmaxmg");
    const auto start = float_block(f, {1}), step = float_block(f, {0.5f});
    static_cast<void>(f.call(acc, "_vDSP_vramp", {start, step, out, 1, 4}));
    require(floats(f, out, 4) == std::vector<float>({1, 1.5f, 2, 2.5f}), "vramp");
    static_cast<void>(f.call(acc, "_vDSP_vclr", {out, 2, 2}));
    require(floats(f, out, 4) == std::vector<float>({0, 1.5f, 0, 2.5f}), "vclr with stride");
    const auto table = float_block(f, {0, 10, 20, 30}), index = float_block(f, {0.5f, 2.25f, 1});
    static_cast<void>(f.call(acc, "_vDSP_vlint", {table, index, 1, out, 1, 3, 4}));
    require(floats(f, out, 3) == std::vector<float>({5, 22.5f, 10}), "vlint");
    const auto c = float_block(f, {1, 1, 1, 1}), d = float_block(f, {2, 2, 2, 2});
    alchemy::CallRegs mmsb{};
    mmsb.x = {a, 1, b, 1, c, 1, d, 1};
    mmsb.push_stack(out);
    mmsb.push_stack(std::int64_t{1});
    mmsb.push_stack(std::uint64_t{4});
    static_cast<void>(f.regs(acc, "_vDSP_vmmsb", mmsb));
    require(floats(f, out, 4) == std::vector<float>({-4, -6, 13, -1}), "vmmsb = A*B - C*D");

    // cblas_dgemm, row-major: C(2x2) = 1 * A(2x3) * B(3x2) + 0 * C.
    const auto doubles = [&](std::initializer_list<double> values) {
        const auto at = f.block(values.size() * 8);
        std::size_t i = 0;
        for (const auto value : values) f.engine.write64(at + 8 * i++, std::bit_cast<std::uint64_t>(value));
        return at;
    };
    const auto read = [&](Address at, std::size_t i) { return std::bit_cast<double>(f.engine.read64(at + 8 * i)); };
    const auto ma = doubles({1, 2, 3, 4, 5, 6}), mb = doubles({7, 8, 9, 10, 11, 12}), mc = doubles({0, 0, 0, 0});
    alchemy::CallRegs gemm{};
    gemm.x = {101, 111, 111, 2, 2, 3, ma, 3};
    gemm.set_d(0, 1.0);
    gemm.set_d(1, 0.0);
    gemm.push_stack(mb);
    gemm.push_stack(std::int32_t{2});
    gemm.push_stack(mc);
    gemm.push_stack(std::int32_t{2});
    static_cast<void>(f.regs(acc, "_cblas_dgemm", gemm));
    require(read(mc, 0) == 58 && read(mc, 1) == 64 && read(mc, 2) == 139 && read(mc, 3) == 154, "dgemm");
    // dgesv_: [[2, 1], [1, 3]] x = [3, 5] -> x = [0.8, 1.4] (column-major).
    const auto ints = [&](std::initializer_list<std::int32_t> values) {
        const auto at = f.block(values.size() * 4);
        std::size_t i = 0;
        for (const auto value : values) f.engine.write32(at + 4 * i++, static_cast<std::uint32_t>(value));
        return at;
    };
    const auto n = ints({2}), nrhs = ints({1}), lda = ints({2}), ipiv = ints({0, 0}), ldb = ints({2}), info = ints({-99});
    const auto matrix = doubles({2, 1, 1, 3}), rhs = doubles({3, 5});
    static_cast<void>(f.call(acc, "_dgesv_", {n, nrhs, matrix, lda, ipiv, rhs, ldb, info}));
    require(f.engine.read32(info) == 0 && std::fabs(read(rhs, 0) - 0.8) < 1e-12 && std::fabs(read(rhs, 1) - 1.4) < 1e-12, "dgesv");
    const auto singular = doubles({1, 2, 2, 4}), rhs2 = doubles({1, 1});
    static_cast<void>(f.call(acc, "_dgesv_", {n, nrhs, singular, lda, ipiv, rhs2, ldb, info}));
    require(f.engine.read32(info) == 2, "dgesv reports the zero pivot");
}

void core_midi(Fixture& f) {
    const auto client_out = f.block(4), port_out = f.block(4), name = f.cf.string("QLab");
    require(f.status(midi_lib, "_MIDIClientCreate", {name, 0, 0, client_out}) == 0, "MIDIClientCreate");
    const auto client = f.engine.read32(client_out);
    require(f.call(midi_lib, "_MIDIGetNumberOfDevices") == 1 && f.call(midi_lib, "_MIDIGetNumberOfSources") == 1 &&
            f.call(midi_lib, "_MIDIGetNumberOfDestinations") == 1 && f.call(midi_lib, "_MIDIGetNumberOfExternalDevices") == 0, "enumeration");
    const auto device = f.call(midi_lib, "_MIDIGetDevice", {0});
    const auto entity = f.call(midi_lib, "_MIDIDeviceGetEntity", {device, 0});
    require(f.call(midi_lib, "_MIDIDeviceGetNumberOfEntities", {device}) == 1 && f.call(midi_lib, "_MIDIEntityGetNumberOfSources", {entity}) == 1, "entity");
    const auto destination = f.call(midi_lib, "_MIDIEntityGetDestination", {entity, 0});
    require(destination == f.call(midi_lib, "_MIDIGetDestination", {0}), "entity destination is listed");
    const auto string_out = f.block(8);
    const auto name_key = f.engine.read64(f.symbol(midi_lib, "_kMIDIPropertyName"));
    require(f.status(midi_lib, "_MIDIObjectGetStringProperty", {destination, name_key, string_out}) == 0 &&
            f.cf.utf8(f.engine.read64(string_out)) == "Port A", "endpoint name");
    const auto manufacturer = f.engine.read64(f.symbol(midi_lib, "_kMIDIPropertyManufacturer"));
    require(f.status(midi_lib, "_MIDIObjectGetStringProperty", {destination, manufacturer, string_out}) == 0 &&
            f.cf.utf8(f.engine.read64(string_out)) == "Maker", "inherited manufacturer");
    const auto offline = f.engine.read64(f.symbol(midi_lib, "_kMIDIPropertyOffline")), number_out = f.block(4, 0xff);
    require(f.status(midi_lib, "_MIDIObjectGetIntegerProperty", {destination, offline, number_out}) == 0 && f.engine.read32(number_out) == 0, "online");
    const auto private_key = f.engine.read64(f.symbol(midi_lib, "_kMIDIPropertyPrivate"));
    require(f.status(midi_lib, "_MIDIObjectGetIntegerProperty", {destination, private_key, number_out}) == -10835, "unset property");
    const auto entity_out = f.block(4);
    require(f.status(midi_lib, "_MIDIEndpointGetEntity", {destination, entity_out}) == 0 && f.engine.read32(entity_out) == entity, "endpoint entity");
    const auto properties = f.block(8);
    require(f.status(midi_lib, "_MIDIObjectGetProperties", {device, properties, 1}) == 0, "MIDIObjectGetProperties");
    const auto entities_key = f.cf.string("entities");
    require(f.cf.dictionary_value(f.engine.read64(properties), entities_key) != 0, "deep properties list the entities");

    // Output: a note to the hardware destination reaches the backend.
    require(f.status(midi_lib, "_MIDIOutputPortCreate", {client, name, port_out}) == 0, "output port");
    const auto list = f.block(256);
    const auto packet = f.call(midi_lib, "_MIDIPacketListInit", {list});
    const auto bytes = f.block(3);
    f.memory().write8(bytes, 0x90); f.memory().write8(bytes + 1, 64); f.memory().write8(bytes + 2, 100);
    require(f.call(midi_lib, "_MIDIPacketListAdd", {list, 256, packet, 0, 3, bytes}) != 0, "packet added");
    require(f.status(midi_lib, "_MIDISend", {f.engine.read32(port_out), destination, list}) == 0, "MIDISend");
    {
        std::lock_guard lock(FakeMidi::mutex());
        require(FakeMidi::sent().size() == 1 && FakeMidi::sent()[0] == std::vector<std::uint8_t>({0x90, 64, 100}), "backend got the note");
    }
    // A virtual source with a chosen unique ID; SysEx requests complete synchronously.
    const auto source_out = f.block(4);
    require(f.status(midi_lib, "_MIDISourceCreate", {client, name, source_out}) == 0 && f.call(midi_lib, "_MIDIGetNumberOfSources") == 2, "virtual source");
    const auto unique_key = f.engine.read64(f.symbol(midi_lib, "_kMIDIPropertyUniqueID"));
    require(f.status(midi_lib, "_MIDIObjectSetIntegerProperty", {f.engine.read32(source_out), unique_key, 31337}) == 0, "set unique ID");
    const auto found = f.block(4), type = f.block(4);
    require(f.status(midi_lib, "_MIDIObjectFindByUniqueID", {31337, found, type}) == 0 && f.engine.read32(found) == f.engine.read32(source_out) &&
            f.engine.read32(type) == 2, "find by unique ID");
    require(f.status(midi_lib, "_MIDIReceived", {f.engine.read32(source_out), list}) == 0, "MIDIReceived with no listeners");
    const auto request = f.block(40), sysex = f.block(4);
    f.memory().write8(sysex, 0xf0); f.memory().write8(sysex + 1, 0x7e); f.memory().write8(sysex + 2, 0x01); f.memory().write8(sysex + 3, 0xf7);
    f.engine.write32(request, static_cast<std::uint32_t>(destination));
    f.engine.write64(request + 8, sysex);
    f.engine.write32(request + 16, 4);
    require(f.status(midi_lib, "_MIDISendSysex", {request}) == 0 && f.memory().read8(request + 20) == 1 && f.engine.read32(request + 16) == 0, "SysEx request completes");
    {
        std::lock_guard lock(FakeMidi::mutex());
        require(FakeMidi::sent().size() == 2 && FakeMidi::sent()[1].size() == 4, "SysEx reached the backend");
    }
    require(f.status(midi_lib, "_MIDIFlushOutput", {0}) == 0, "flush all");
    require(f.status(midi_lib, "_MIDIEndpointDispose", {f.engine.read32(source_out)}) == 0 && f.call(midi_lib, "_MIDIGetNumberOfSources") == 1, "dispose");
    require(f.status(midi_lib, "_MIDIPortDispose", {f.engine.read32(port_out)}) == 0 && f.status(midi_lib, "_MIDIClientDispose", {client}) == 0, "dispose port and client");
}

void core_audio(Fixture& f) {
    namespace hal = alchemy::gui::completion_kit::media::hal;
    const auto address = f.block(12), size = f.block(4), data = f.block(64);
    const auto set = [&](std::uint32_t selector, std::uint32_t scope) {
        f.engine.write32(address, selector);
        f.engine.write32(address + 4, scope);
        f.engine.write32(address + 8, 0);
    };
    set(hal::four("dev#"), hal::scope_global);
    require(f.status(ca, "_AudioObjectGetPropertyDataSize", {1, address, 0, 0, size}) == 0 && f.engine.read32(size) == 4, "one device");
    f.engine.write32(size, 64);
    require(f.status(ca, "_AudioObjectGetPropertyData", {1, address, 0, 0, size, data}) == 0 && f.engine.read32(size) == 4, "device list");
    const auto device = f.engine.read32(data);
    set(hal::four("dOut"), hal::scope_global);
    f.engine.write32(size, 4);
    require(f.status(ca, "_AudioObjectGetPropertyData", {1, address, 0, 0, size, data}) == 0 && f.engine.read32(data) == device, "default output");
    set(hal::four("lnam"), hal::scope_global);
    f.engine.write32(size, 8);
    require(f.status(ca, "_AudioObjectGetPropertyData", {device, address, 0, 0, size, data}) == 0 && f.cf.utf8(f.engine.read64(data)) == "Line Out", "device name");
    // kAudioHardwarePropertyTranslateUIDToDevice with a CFString qualifier.
    set(hal::four("uidd"), hal::scope_global);
    const auto uid = f.block(8);
    f.engine.write64(uid, f.cf.string("{render}"));
    f.engine.write32(size, 4);
    require(f.status(ca, "_AudioObjectGetPropertyData", {1, address, 8, uid, size, data}) == 0 && f.engine.read32(data) == device, "UID to device");
    set(hal::four("nsrt"), hal::scope_global);
    f.engine.write32(size, 8);
    require(f.status(ca, "_AudioObjectGetPropertyData", {device, address, 0, 0, size, data}) == 0 &&
            std::bit_cast<double>(f.engine.read64(data)) == 48000.0, "nominal sample rate");
    set(hal::four("dev#"), hal::scope_global);
    f.engine.write32(size, 2);
    require(f.status(ca, "_AudioObjectGetPropertyData", {1, address, 0, 0, size, data}) == 0 && f.engine.read32(size) == 0, "array truncates to whole elements");
    set(hal::four("nsrt"), hal::scope_global);
    f.engine.write32(size, 4);
    require(f.status(ca, "_AudioObjectGetPropertyData", {device, address, 0, 0, size, data}) == hal::error::bad_size, "short scalar");
    const auto proc_out = f.block(8);
    require(f.status(ca, "_AudioDeviceCreateIOProcID", {device, 0x1000, 0, proc_out}) == hal::error::unsupported, "no device I/O");
    require(f.status(ca, "_AudioDeviceCreateIOProcID", {77, 0x1000, 0, proc_out}) == hal::error::bad_device, "not a device");
    require(f.status(ca, "_AudioDeviceGetCurrentTime", {device, data}) == hal::error::not_running, "not running");
    set(hal::four("dOut"), hal::scope_global);
    require(f.status(ca, "_AudioObjectAddPropertyListener", {1, address, 0x1000, 0}) == 0, "listener");
    require(f.call(ca, "_AudioConvertHostTimeToNanos", {12345}) == 12345, "host time is nanoseconds");
}

std::vector<std::uint8_t> fixture_bytes(const char* name) {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
    std::ifstream in(path, std::ios::binary);
    require(in.good(), "fixture missing: " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void music_sequence(Fixture& f) {
    const auto out = f.block(8);
    require(f.status(at, "_NewMusicSequence", {out}) == 0, "NewMusicSequence");
    const auto sequence = f.engine.read64(out);
    const auto bytes = fixture_bytes("two_tracks.mid");
    const auto data = alchemy::gui::security::bridge()->data(bytes);
    require(f.status(at, "_MusicSequenceFileLoadData", {sequence, data, 0, 0}) == 0, "MusicSequenceFileLoadData");
    const auto count = f.block(4);
    require(f.status(at, "_MusicSequenceGetTrackCount", {sequence, count}) == 0 && f.engine.read32(count) == 1, "track count");
    require(f.status(at, "_MusicSequenceGetIndTrack", {sequence, 0, out}) == 0, "GetIndTrack");
    const auto track = f.engine.read64(out);
    require(f.status(at, "_MusicSequenceGetIndTrack", {sequence, 1, out}) == -10859, "track index error");
    require(f.status(at, "_MusicSequenceGetTrackIndex", {sequence, track, count}) == 0 && f.engine.read32(count) == 0, "GetTrackIndex");
    require(f.status(at, "_MusicTrackGetSequence", {track, out}) == 0 && f.engine.read64(out) == sequence, "MusicTrackGetSequence");
    // Iterate: the six events with their types.
    require(f.status(at, "_NewMusicEventIterator", {track, out}) == 0, "iterator");
    const auto iterator = f.engine.read64(out);
    const auto has = f.block(1), time = f.block(8), type = f.block(4), payload = f.block(8), size = f.block(4);
    std::vector<std::uint32_t> types;
    for (;;) {
        require(f.status(at, "_MusicEventIteratorHasNextEvent", {iterator, has}) == 0, "HasNextEvent");
        if (!f.memory().read8(has)) break;
        require(f.status(at, "_MusicEventIteratorGetEventInfo", {iterator, time, type, payload, size}) == 0, "GetEventInfo");
        types.push_back(f.engine.read32(type));
        if (f.engine.read32(type) == 6 && types.size() == 2)
            require(f.memory().read8(f.engine.read64(payload) + 1) == 60 && f.engine.read32(size) == 8, "note payload");
        require(f.status(at, "_MusicEventIteratorNextEvent", {iterator}) == 0, "NextEvent");
    }
    require((types == std::vector<std::uint32_t>{5, 6, 6, 7, 7, 8}), "event types");
    require(f.status(at, "_MusicEventIteratorNextEvent", {iterator}) == -10857, "end of track");
    require(f.status(at, "_DisposeMusicEventIterator", {iterator}) == 0, "dispose iterator");
    // Tempo map and bar/beat conversion through the guest ABI.
    alchemy::CallRegs seconds{};
    seconds.x[0] = sequence; seconds.x[1] = time; seconds.set_d(0, 5.0);
    require(f.regs(at, "_MusicSequenceGetSecondsForBeats", seconds).x0 == 0 && std::bit_cast<double>(f.engine.read64(time)) == 3.0, "seconds for beats");
    const auto bar = f.block(12);
    alchemy::CallRegs to_bar{};
    to_bar.x[0] = sequence; to_bar.x[1] = 4; to_bar.x[2] = bar; to_bar.set_d(0, 4.5);
    require(f.regs(at, "_MusicSequenceBeatsToBarBeatTime", to_bar).x0 == 0 && f.engine.read32(bar) == 2 && f.memory().read8(bar + 4) == 2 &&
            f.memory().read8(bar + 6) == 2, "bar/beat");
    require(f.status(at, "_MusicSequenceBarBeatTimeToBeats", {sequence, bar, time}) == 0 && std::bit_cast<double>(f.engine.read64(time)) == 4.5, "bar/beat back");
    // Track properties: length, then mute.
    const auto length = f.block(4);
    f.engine.write32(length, 8);
    require(f.status(at, "_MusicTrackGetProperty", {track, 5, time, length}) == 0 && std::bit_cast<double>(f.engine.read64(time)) == 5.0, "track length");
    // Events added through the API keep time order; tempo events only go to the tempo track.
    const auto note = f.block(8);
    f.memory().write8(note, 0); f.memory().write8(note + 1, 72); f.memory().write8(note + 2, 80); f.memory().write8(note + 3, 0);
    f.engine.write32(note + 4, std::bit_cast<std::uint32_t>(0.5f));
    alchemy::CallRegs add{};
    add.x[0] = track; add.x[1] = note; add.set_d(0, 0.25);
    require(f.regs(at, "_MusicTrackNewMIDINoteEvent", add).x0 == 0, "add note");
    alchemy::CallRegs tempo{};
    tempo.x[0] = track; tempo.set_d(0, 1.0); tempo.set_d(1, 90.0);
    require(static_cast<std::int32_t>(f.regs(at, "_MusicTrackNewExtendedTempoEvent", tempo).x0) == -10853, "tempo events need the tempo track");
    // Writing a file.
    require(f.status(at, "_MusicSequenceFileCreateData", {sequence, 0x6d696469, 0, 480, out}) == 0 && f.engine.read64(out) != 0, "FileCreateData");
    // The player plays the sequence to a MIDI destination.
    const auto player_out = f.block(8);
    require(f.status(at, "_NewMusicPlayer", {player_out}) == 0, "NewMusicPlayer");
    const auto player = f.engine.read64(player_out);
    require(f.status(at, "_MusicPlayerStart", {player}) == -10854, "no sequence");
    require(f.status(at, "_MusicPlayerSetSequence", {player, sequence}) == 0, "SetSequence");
    require(f.status(at, "_MusicPlayerStart", {player}) == -66720, "no MIDI destination and no AUGraph");
    const auto destination = f.call(midi_lib, "_MIDIGetDestination", {0});
    require(f.status(at, "_MusicSequenceSetMIDIEndpoint", {sequence, destination}) == 0, "SetMIDIEndpoint");
    require(f.status(at, "_MusicSequenceSetMIDIEndpoint", {sequence, 0x7fff0000}) == -10855, "not a destination");
    std::size_t before{};
    { std::lock_guard lock(FakeMidi::mutex()); before = FakeMidi::sent().size(); }
    require(f.status(at, "_MusicPlayerStart", {player}) == 0, "MusicPlayerStart");
    const auto host = f.block(8);
    alchemy::CallRegs host_for{};
    host_for.x[0] = player; host_for.x[1] = host; host_for.set_d(0, 2.0);
    require(f.regs(at, "_MusicPlayerGetHostTimeForBeats", host_for).x0 == 0 && f.engine.read64(host) > 0, "host time for beats");
    bool got_note = false;
    for (int i = 0; i < 100 && !got_note; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard lock(FakeMidi::mutex());
        for (std::size_t k = before; k < FakeMidi::sent().size(); ++k)
            got_note = got_note || FakeMidi::sent()[k] == std::vector<std::uint8_t>({0x90, 60, 100});
    }
    require(got_note, "the first note reached the MIDI destination");
    require(f.status(at, "_MusicPlayerGetTime", {player, time}) == 0 && std::bit_cast<double>(f.engine.read64(time)) > 0, "time advances");
    require(f.status(at, "_MusicPlayerStop", {player}) == 0, "stop");
    require(f.status(at, "_MusicPlayerGetHostTimeForBeats", {player, host}) == -10852, "host time needs a playing player");
    require(f.status(at, "_DisposeMusicPlayer", {player}) == 0 && f.status(at, "_DisposeMusicSequence", {sequence}) == 0, "dispose");
}

// ExtAudioFile reads of a generated WAVE file (1000 frames of 16-bit stereo ramps).
void audio_files(Fixture& f) {
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_tests";
    std::filesystem::create_directories(directory);
    const auto path = directory / "ramp.wav";
    {
        std::vector<std::uint8_t> data;
        for (int i = 0; i < 1000; ++i)
            for (const int v : {i * 32, -i * 32}) { data.push_back(static_cast<std::uint8_t>(v)); data.push_back(static_cast<std::uint8_t>(v >> 8)); }
        std::vector<std::uint8_t> file;
        const auto le = [&](std::uint32_t value, int bytes) { for (int i = 0; i < bytes; ++i) file.push_back(static_cast<std::uint8_t>(value >> (8 * i))); };
        const auto tag = [&](const char* text) { file.insert(file.end(), text, text + 4); };
        tag("RIFF"); le(36 + static_cast<std::uint32_t>(data.size()), 4); tag("WAVE");
        tag("fmt "); le(16, 4); le(1, 2); le(2, 2); le(44100, 4); le(44100 * 4, 4); le(4, 2); le(16, 2);
        tag("data"); le(static_cast<std::uint32_t>(data.size()), 4);
        file.insert(file.end(), data.begin(), data.end());
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
    const auto out = f.block(8);
    require(f.status(at, "_AudioFileOpenURL", {url, 1, 0, out}) == 0, "AudioFileOpenURL");
    const auto file = f.engine.read64(out);
    const auto size = f.block(4), value = f.block(64);
    f.engine.write32(size, 8);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x70636e74, size, value}) == 0 && f.engine.read64(value) == 1000, "packet count");   // 'pcnt'
    f.engine.write32(size, 40);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x64666d74, size, value}) == 0 && f.engine.read32(value + 28) == 2 &&
            f.engine.read32(value + 8) == 0x6c70636d, "data format");   // 'dfmt'
    f.engine.write32(size, 4);
    require(f.status(at, "_AudioFileGetProperty", {file, 0x64666d74, size, value}) == static_cast<std::int32_t>(0x2173697a), "'!siz'");
    require(f.status(at, "_AudioFileOpenURL", {url, 3, 0, out}) == static_cast<std::int32_t>(0x6F703F3F), "writing is not provided");
    require(f.status(at, "_ExtAudioFileWrapAudioFileID", {file, 0, out}) == 0, "ExtAudioFileWrapAudioFileID");
    const auto ext = f.engine.read64(out);
    f.engine.write32(size, 8);
    require(f.status(at, "_ExtAudioFileGetProperty", {ext, 0x2366726d, size, value}) == 0 && f.engine.read64(value) == 1000, "file length frames");   // '#frm'
    // Client format: float32, non-interleaved, same rate.
    const auto client = f.block(40);
    f.engine.write64(client, std::bit_cast<std::uint64_t>(44100.0));
    f.engine.write32(client + 8, 0x6c70636d);
    f.engine.write32(client + 12, 1 | 8 | 32);   // float, packed, non-interleaved
    f.engine.write32(client + 16, 4); f.engine.write32(client + 20, 1); f.engine.write32(client + 24, 4);
    f.engine.write32(client + 28, 2); f.engine.write32(client + 32, 32);
    require(f.status(at, "_ExtAudioFileSetProperty", {ext, 0x63666d74, 40, client}) == 0, "client format");   // 'cfmt'
    f.engine.write64(client, std::bit_cast<std::uint64_t>(48000.0));
    require(f.status(at, "_ExtAudioFileSetProperty", {ext, 0x63666d74, 40, client}) == static_cast<std::int32_t>(0x666d743f), "no sample-rate conversion");
    // Read 600 frames into two 512-frame buffers: 512 come back, then the rest.
    const auto left = f.block(512 * 4), right = f.block(512 * 4), list = f.block(8 + 32), frames = f.block(4);
    const auto setup = [&] {
        f.engine.write32(list, 2);
        f.engine.write32(list + 8, 1); f.engine.write32(list + 12, 512 * 4); f.engine.write64(list + 16, left);
        f.engine.write32(list + 24, 1); f.engine.write32(list + 28, 512 * 4); f.engine.write64(list + 32, right);
    };
    setup();
    f.engine.write32(frames, 600);
    require(f.status(at, "_ExtAudioFileRead", {ext, frames, list}) == 0 && f.engine.read32(frames) == 512 && f.engine.read32(list + 12) == 2048, "read limited by the buffers");
    const auto sample = [&](Address buffer, int index) { return std::bit_cast<float>(f.engine.read32(buffer + 4 * index)); };
    require(sample(left, 100) == 3200.0f / 32768 && sample(right, 100) == -3200.0f / 32768, "converted samples");
    require(f.status(at, "_ExtAudioFileSeek", {ext, 990}) == 0, "seek");
    setup();
    f.engine.write32(frames, 512);
    require(f.status(at, "_ExtAudioFileRead", {ext, frames, list}) == 0 && f.engine.read32(frames) == 10 && sample(left, 0) == 990.0f * 32 / 32768, "read to the end");
    setup();
    f.engine.write32(frames, 512);
    require(f.status(at, "_ExtAudioFileRead", {ext, frames, list}) == 0 && f.engine.read32(frames) == 0, "end of file: 0 frames");
    require(f.status(at, "_ExtAudioFileDispose", {ext}) == 0 && f.status(at, "_AudioFileClose", {file}) == 0, "dispose and close");
    require(f.status(at, "_ExtAudioFileOpenURL", {f.cf.file_url(alchemy::guest_path_string(directory / "missing.wav"), false), out}) == -43, "missing file");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// AU listeners keep registrations; with no audio unit instance, setting and notifying answer
// the AudioUnit layer's badComponentInstance.
void au_listeners(Fixture& f) {
    constexpr auto bad_instance = static_cast<std::int32_t>(0x80008001U);
    const auto out = f.block(8), parameter = f.block(24), event = f.block(32);
    alchemy::CallRegs create{};
    create.x[0] = 0x1000; create.x[1] = 7; create.x[4] = out;
    create.set_s(0, 0.1f);
    require(f.regs(at, "_AUListenerCreate", create).x0 == 0, "AUListenerCreate");
    const auto listener = f.engine.read64(out);
    f.engine.write64(parameter, 0x5000);   // an AudioUnit that does not exist
    f.engine.write32(parameter + 8, 3);
    require(f.status(at, "_AUListenerAddParameter", {listener, 0, parameter}) == 0, "AUListenerAddParameter");
    alchemy::CallRegs set{};
    set.x[0] = listener; set.x[2] = parameter;
    set.set_s(0, 0.5f);
    require(static_cast<std::int32_t>(f.regs(at, "_AUParameterSet", set).x0) == bad_instance, "AUParameterSet: no unit");
    require(f.status(at, "_AUParameterListenerNotify", {listener, 0, parameter}) == bad_instance, "notify: no unit");
    require(f.status(at, "_AUListenerRemoveParameter", {listener, 0, parameter}) == 0, "remove");
    alchemy::CallRegs events{};
    events.x[0] = 0x1000; events.x[4] = out;
    require(f.regs(at, "_AUEventListenerCreate", events).x0 == 0, "AUEventListenerCreate");
    const auto event_listener = f.engine.read64(out);
    f.engine.write32(event, 0);
    f.engine.write64(event + 8, 0x5000);
    require(f.status(at, "_AUEventListenerAddEventType", {event_listener, 0, event}) == 0, "parameter event");
    require(f.status(at, "_AUEventListenerRemoveEventType", {event_listener, 0, event}) == 0, "remove event");
    f.engine.write32(event, 3);
    require(f.status(at, "_AUEventListenerAddEventType", {event_listener, 0, event}) == bad_instance, "property events need a unit");
    require(f.status(at, "_AUListenerDispose", {listener}) == 0 && f.status(at, "_AUListenerDispose", {event_listener}) == 0, "dispose");
    require(f.status(at, "_AUListenerDispose", {listener}) == -50, "dispose twice");
    require(f.status(au, "_AudioComponentCopyName", {0, out}) == -50 && f.status(au, "_AudioComponentGetDescription", {0, out}) == -50, "no components");
    require(f.status(au, "_AudioUnitAddRenderNotify", {0x5000, 0x1000, 0}) == bad_instance, "no units");
}

// AVFoundation: an asset over a generated WAVE file, a reader vending float samples, value
// classes, and the classes whose instances cannot exist.
void av_foundation(Fixture& f) {
    const auto directory = std::filesystem::temp_directory_path() / "alchemy_qlab_media_av";
    std::filesystem::create_directories(directory);
    const auto path = directory / "tone.wav";
    {
        std::vector<std::uint8_t> file;
        const auto le = [&](std::uint32_t value, int bytes) { for (int i = 0; i < bytes; ++i) file.push_back(static_cast<std::uint8_t>(value >> (8 * i))); };
        const auto tag = [&](const char* text) { file.insert(file.end(), text, text + 4); };
        const std::uint32_t frames = 10000;
        tag("RIFF"); le(36 + frames * 2, 4); tag("WAVE");
        tag("fmt "); le(16, 4); le(1, 2); le(1, 2); le(8000, 4); le(16000, 4); le(2, 2); le(16, 2);
        tag("data"); le(frames * 2, 4);
        for (std::uint32_t i = 0; i < frames; ++i) le(static_cast<std::uint16_t>(static_cast<std::int16_t>(i % 2 ? 8192 : -8192)), 2);
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    const auto url = f.cf.file_url(alchemy::guest_path_string(path), false);
    const auto asset = f.send(f.cls("AVURLAsset"), "URLAssetWithURL:options:", {url, 0});
    require(asset != 0, "AVURLAsset");
    const auto tracks = f.send(asset, "tracksWithMediaType:", {f.cf.string("soun")});
    require(f.cf.array_values(tracks).size() == 1, "one audio track");
    require(f.cf.array_values(f.send(asset, "tracksWithMediaType:", {f.cf.string("vide")})).empty(), "no video track");
    const auto track = f.cf.array_values(tracks)[0];
    alchemy::CallRegs none{};
    const auto time = f.block(48);
    none.indirect_result = time;
    static_cast<void>(f.objc.send(asset, f.objc.selector("duration"), none));
    require(f.engine.read64(time) == 10000 && f.engine.read32(time + 8) == 8000, "duration 10000/8000");
    static_cast<void>(f.objc.send(track, f.objc.selector("timeRange"), none));
    require(f.engine.read64(time + 24) == 10000 && f.engine.read64(time) == 0, "track time range");
    require(f.cf.array_values(f.send(asset, "commonMetadata")).empty() && (f.send(asset, "hasProtectedContent") & 0xff) == 0, "no metadata, unprotected");
    const auto descriptions = f.cf.array_values(f.send(track, "formatDescriptions"));
    require(descriptions.size() == 1 && f.memory().read32(f.call(cm, "_CMAudioFormatDescriptionGetStreamBasicDescription", {descriptions[0]}) + 28) == 1, "track format");
    // A reader with float32 output settings.
    const auto error = f.block(8);
    const auto reader = f.send(f.cls("AVAssetReader"), "assetReaderWithAsset:error:", {asset, error});
    require(reader != 0, "AVAssetReader");
    std::vector<std::pair<Address, Address>> entries{
        {f.cf.string("AVFormatIDKey"), f.cf.integer(0x6c70636d, 3)}, {f.cf.string("AVLinearPCMBitDepthKey"), f.cf.integer(32, 3)},
        {f.cf.string("AVLinearPCMIsFloatKey"), f.cf.boolean(true)}, {f.cf.string("AVLinearPCMIsNonInterleaved"), f.cf.boolean(false)}};
    const auto settings = f.cf.dictionary(entries);
    const auto output = f.send(f.cls("AVAssetReaderTrackOutput"), "assetReaderTrackOutputWithTrack:outputSettings:", {track, settings});
    require((f.send(reader, "canAddOutput:", {output}) & 0xff) == 1, "canAddOutput:");
    static_cast<void>(f.send(reader, "addOutput:", {output}));
    // Read from 0.5 s for 0.5 s: 4000 frames.
    const auto range = f.block(48);
    f.engine.write64(range, 1); f.engine.write32(range + 8, 2); f.engine.write32(range + 12, 1); f.engine.write64(range + 16, 0);
    f.engine.write64(range + 24, 1); f.engine.write32(range + 32, 2); f.engine.write32(range + 36, 1); f.engine.write64(range + 40, 0);
    static_cast<void>(f.send(reader, "setTimeRange:", {range}));
    require((f.send(reader, "startReading") & 0xff) == 1 && f.send(reader, "status") == 1, "startReading");
    std::uint64_t frames = 0;
    bool first = true;
    while (const auto sample = f.send(output, "copyNextSampleBuffer")) {
        frames += f.call(cm, "_CMSampleBufferGetNumSamples", {sample});
        if (first) {
            const auto list = f.block(64), retained = f.block(8);
            require(f.status(cm, "_CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer", {sample, 0, list, 64, 0, 0, 0, retained}) == 0, "buffer list");
            const auto data = f.engine.read64(list + 16);
            require(std::bit_cast<float>(f.engine.read32(data)) == -0.25f && std::bit_cast<float>(f.engine.read32(data + 4)) == 0.25f, "float samples from frame 4000");
            alchemy::CallRegs pts{};
            pts.x[0] = sample;
            pts.indirect_result = time;
            static_cast<void>(f.regs(cm, "_CMSampleBufferGetPresentationTimeStamp", pts));
            require(f.engine.read64(time) == 4000 && f.engine.read32(time + 8) == 8000, "presentation time");
            first = false;
        }
        static_cast<void>(f.call("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", "_CFRelease", {sample}));
    }
    require(frames == 4000 && f.send(reader, "status") == 2, "4000 frames, then completed");
    // Media Alchemy cannot read: no tracks, failed status, no reader, no images.
    const auto video_url = f.cf.file_url(alchemy::guest_path_string(directory / "missing.mov"), false);
    const auto video = f.send(f.cls("AVURLAsset"), "URLAssetWithURL:options:", {video_url, 0});
    require(f.cf.array_values(f.send(video, "tracks")).empty() && f.send(video, "statusOfValueForKey:error:", {f.cf.string("tracks"), error}) == 3, "unreadable media");
    require(f.send(f.cls("AVAssetReader"), "assetReaderWithAsset:error:", {video, error}) == 0 && f.engine.read64(error) != 0, "no reader for unreadable media");
    const auto generator = f.send(f.cls("AVAssetImageGenerator"), "assetImageGeneratorWithAsset:", {asset});
    require(f.send(generator, "copyCGImageAtTime:actualTime:error:", {time, 0, error}) == 0, "no images");
    // Value classes.
    alchemy::CallRegs standard{};
    standard.set_d(0, 48000.0);
    standard.x[2] = 2;
    const auto format = f.objc.send(f.send(f.cls("AVAudioFormat"), "alloc"), f.objc.selector("initStandardFormatWithSampleRate:channels:"), standard).x0;
    require(format && f.send(format, "channelCount") == 2 && (f.send(format, "isStandard") & 0xff) == 1 && f.send(format, "commonFormat") == 1, "standard format");
    const auto asbd = f.send(format, "streamDescription");
    require(f.engine.read32(asbd + 12) == (1 | 8 | 32) && f.engine.read32(asbd + 24) == 4, "deinterleaved float32 description");
    standard.x[2] = 6;
    require(f.objc.send(f.send(f.cls("AVAudioFormat"), "alloc"), f.objc.selector("initStandardFormatWithSampleRate:channels:"), standard).x0 == 0, "six channels need a layout");
    const auto layout = f.send(f.cls("AVAudioChannelLayout"), "layoutWithLayoutTag:", {(121U << 16) | 6});   // 5.1 (A)
    require(layout && f.send(layout, "channelCount") == 6, "channel layout");
    alchemy::CallRegs with_layout{};
    with_layout.x[2] = 1; with_layout.x[3] = 0; with_layout.x[4] = layout;
    with_layout.set_d(0, 48000.0);
    const auto surround = f.objc.send(f.send(f.cls("AVAudioFormat"), "alloc"), f.objc.selector("initWithCommonFormat:sampleRate:interleaved:channelLayout:"), with_layout).x0;
    require(surround && f.send(surround, "channelCount") == 6 && f.send(surround, "channelLayout") == layout, "format with a layout");
    const auto bus = f.send(f.send(f.cls("AUAudioUnitBus"), "alloc"), "initWithFormat:error:", {format, error});
    require(bus && (f.send(bus, "setFormat:error:", {surround, error}) & 0xff) == 1 && f.send(bus, "format") == surround, "AUAudioUnitBus");
    // No audio component exists.
    const auto manager = f.send(f.cls("AVAudioUnitComponentManager"), "sharedAudioUnitComponentManager");
    const auto description = f.block(20);
    require(manager && f.cf.array_values(f.send(manager, "componentsMatchingDescription:", {description})).empty(), "no components");
    require(f.send(f.send(f.cls("AUAudioUnit"), "alloc"), "initWithComponentDescription:error:", {description, error}) == 0 && f.engine.read64(error) != 0, "no AUAudioUnit");
    // Capture: discovery finds nothing; a session runs with an output and no input.
    const auto discovery = f.send(f.cls("AVCaptureDeviceDiscoverySession"), "discoverySessionWithDeviceTypes:mediaType:position:", {0, 0, 0});
    require(discovery && f.cf.array_values(f.send(discovery, "devices")).empty(), "no capture devices");
    const auto session = f.send(f.send(f.cls("AVCaptureSession"), "alloc"), "init");
    const auto data_output = f.send(f.send(f.cls("AVCaptureVideoDataOutput"), "alloc"), "init");
    static_cast<void>(f.send(session, "beginConfiguration"));
    require((f.send(session, "canAddOutput:", {data_output}) & 0xff) == 1, "canAddOutput:");
    static_cast<void>(f.send(session, "addOutput:", {data_output}));
    static_cast<void>(f.send(session, "setSessionPreset:", {f.cf.string("AVCaptureSessionPresetHigh")}));
    static_cast<void>(f.send(session, "commitConfiguration"));
    static_cast<void>(f.send(session, "startRunning"));
    require((f.send(session, "isRunning") & 0xff) == 1 && f.cf.array_values(f.send(session, "outputs")).size() == 1, "session runs");
    static_cast<void>(f.send(session, "stopRunning"));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

// vImage Y'CbCr <-> ARGB against hand-computed BT.601 / BT.709 video-range values.
void vimage(Fixture& f) {
    const auto range = f.block(32);
    const std::array<std::uint32_t, 8> video{16, 128, 235, 240, 235, 16, 240, 16};
    for (std::size_t i = 0; i < 8; ++i) f.engine.write32(range + 4 * i, video[i]);
    const auto matrix = [&](const char* name) { return f.engine.read64(f.symbol(acc, name)); };
    require(std::bit_cast<float>(f.engine.read32(matrix("_kvImage_YpCbCrToARGBMatrix_ITU_R_601_4") + 4)) == 1.402f, "601 Cr_R");
    const auto to601 = f.block(128), to709 = f.block(128), from601 = f.block(128);
    require(f.call(acc, "_vImageConvert_YpCbCrToARGB_GenerateConversion", {matrix("_kvImage_YpCbCrToARGBMatrix_ITU_R_601_4"), range, to601, 0, 0, 0}) == 0, "601 conversion");
    require(f.call(acc, "_vImageConvert_YpCbCrToARGB_GenerateConversion", {matrix("_kvImage_YpCbCrToARGBMatrix_ITU_R_709_2"), range, to709, 0, 0, 0}) == 0, "709 conversion");
    require(f.call(acc, "_vImageConvert_ARGBToYpCbCr_GenerateConversion", {matrix("_kvImage_ARGBToYpCbCrMatrix_ITU_R_601_4"), range, from601, 0, 0, 0}) == 0, "601 inverse");
    const auto buffer = [&](Address data, std::uint64_t height, std::uint64_t width, std::uint64_t row_bytes) {
        const auto at = f.block(32);
        f.engine.write64(at, data); f.engine.write64(at + 8, height); f.engine.write64(at + 16, width); f.engine.write64(at + 24, row_bytes);
        return at;
    };
    const auto bytes = [&](std::initializer_list<std::uint8_t> values) {
        const auto at = f.block(values.size());
        f.engine.write_bytes(at, std::vector<std::uint8_t>(values));
        return at;
    };
    const auto read = [&](Address at, std::size_t count) {
        std::vector<std::uint8_t> out(count);
        f.engine.read_bytes(at, out);
        return out;
    };
    // 4:2:2 BT.601 red (Cb 90, Y' 81, Cr 240): R = 0.2968 + 1.402 * 0.5 -> 254; G, B round to 0.
    const auto red422 = bytes({90, 81, 240, 81});
    const auto out = f.block(16);
    require(f.call(acc, "_vImageConvert_422CbYpCrYp8ToARGB8888", {buffer(red422, 1, 2, 4), buffer(out, 1, 2, 8), to601, 0, 255, 0}) == 0, "422 to ARGB");
    require((read(out, 8) == std::vector<std::uint8_t>{255, 254, 0, 0, 255, 254, 0, 0}), "601 red");
    const auto bgra = bytes({3, 2, 1, 0});
    require(f.call(acc, "_vImageConvert_422CbYpCrYp8ToARGB8888", {buffer(red422, 1, 2, 4), buffer(out, 1, 2, 8), to601, bgra, 7, 0}) == 0 &&
            (read(out, 4) == std::vector<std::uint8_t>{0, 0, 254, 7}), "permute to BGRA with alpha 7");
    // BT.709 red (Cb 102, Y' 63, Cr 240): R clips to 255, G = 0.0023 -> 1, B -> 0.
    const auto red709 = bytes({102, 63, 240, 63});
    require(f.call(acc, "_vImageConvert_422CbYpCrYp8ToARGB8888", {buffer(red709, 1, 2, 4), buffer(out, 1, 2, 8), to709, 0, 255, 0}) == 0 &&
            (read(out, 4) == std::vector<std::uint8_t>{255, 255, 1, 0}), "709 red");
    // 4:2:0 biplanar, neutral chroma: Y' 235 white, 16 black, 81 grey 76.
    const auto luma = bytes({235, 16, 81, 81}), chroma = bytes({128, 128});
    require(f.call(acc, "_vImageConvert_420Yp8_CbCr8ToARGB8888", {buffer(luma, 2, 2, 2), buffer(chroma, 1, 1, 2), buffer(out, 2, 2, 8), to601, 0, 255, 0}) == 0, "420 to ARGB");
    require((read(out, 16) == std::vector<std::uint8_t>{255, 255, 255, 255, 255, 0, 0, 0, 255, 76, 76, 76, 255, 76, 76, 76}), "420 greys");
    require(static_cast<std::int64_t>(f.call(acc, "_vImageConvert_420Yp8_CbCr8ToARGB8888", {buffer(luma, 2, 2, 2), buffer(chroma, 1, 1, 2), buffer(out, 4, 4, 16), to601, 0, 255, 0})) == -21766,
            "destination larger than the source");
    // ARGB to 4:2:2: red then white. Y'0 = 0.299 * 219 + 16 -> 81, Y'1 235, Cb = 128 - 0.0844 * 224 -> 109, Cr = 128 + 0.25 * 224 = 184.
    const auto argb = bytes({255, 255, 0, 0, 255, 255, 255, 255});
    require(f.call(acc, "_vImageConvert_ARGB8888To422CbYpCrYp8", {buffer(argb, 1, 2, 8), buffer(out, 1, 2, 4), from601, 0, 0}) == 0 &&
            (read(out, 4) == std::vector<std::uint8_t>{109, 81, 184, 235}), "ARGB to 422");
    require(static_cast<std::int64_t>(f.call(acc, "_vImageConvert_422CbYpCrYp8ToARGB8888", {buffer(red422, 1, 2, 4), buffer(out, 1, 2, 8), from601, 0, 255, 0})) == -21773,
            "the wrong conversion kind");
    const std::array<std::uint32_t, 8> bad{16, 128, 16, 240, 235, 16, 240, 16};
    for (std::size_t i = 0; i < 8; ++i) f.engine.write32(range + 4 * i, bad[i]);
    require(static_cast<std::int64_t>(f.call(acc, "_vImageConvert_YpCbCrToARGB_GenerateConversion", {matrix("_kvImage_YpCbCrToARGBMatrix_ITU_R_601_4"), range, to601, 0, 0, 0})) == -21773, "invalid range");
    require(f.string_constant(cv, "_kCVImageBufferFieldDetailKey") == "CVFieldDetail" && f.call(vt, "_VTDecompressionSessionCanAcceptFormatDescription", {0, 0}) == 0,
            "field keys; no decompression session");
}

} // namespace

int main() {
    alchemy::gui::completion_kit::media::set_midi_backend_factory([] { return std::make_unique<FakeMidi>(); });
    alchemy::gui::completion_kit::media::set_hal_backend_factory([] { return std::make_unique<FakeHal>(); });
    return fill_test::run("qlab media", {
        {"core_media", core_media},
        {"core_video", core_video},
        {"accelerate", accelerate},
        {"vimage", vimage},
        {"core_midi", core_midi},
        {"core_audio", core_audio},
        {"music_sequence", music_sequence},
        {"audio_files", audio_files},
        {"au_listeners", au_listeners},
        {"av_foundation", av_foundation},
    });
}
