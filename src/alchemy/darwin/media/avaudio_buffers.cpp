#include "alchemy/darwin/media/avaudio.hpp"

#include "alchemy/darwin/security/bridge.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

// AVFAudio's value and data classes:
// - AVAudioTime: a host time (mach_absolute_time, nanoseconds here) and/or a sample time at a
//   rate; -extrapolateTimeFromAnchor: fills in the missing one from an anchor with both.
// - AVAudioPCMBuffer over guest memory in its format's layout (any linear PCM AVAudioFormat):
//   the channel data arrays, the AudioBufferList (its byte sizes follow frameLength) and the
//   frame length within the capacity.
// - AVAudioFile for reading the files AudioFile reads (linear PCM WAVE, AIFF/AIFC, CAF:
//   audio_file_core.hpp), converted to its processing format (by default float32
//   deinterleaved at the file's rate and channels) as it is read into buffers.
// - AVAudioFile for writing (WAVE, AIFF/AIFC or CAF by the URL's extension; linear PCM settings)
//   through ExtAudioFile (audio_file.cpp): buffers of the processing format are converted to the
//   file's format as they are written.
// - Unavailable, as documented: compressed files and settings (no decoder or encoder) answer nil
//   with an error.
namespace alchemy::gui::completion_kit::media::avaudio {

namespace af = audio_file;

namespace {

constexpr std::uint32_t timestamp_sample_valid = 1, timestamp_host_valid = 2;
constexpr std::int32_t param_error = -50;
constexpr std::uint64_t max_decoded_samples = 1ULL << 28;   // 1 GiB of float samples

std::shared_ptr<security::Bridge> bridge() { return security::bridge(); }

void append_floats(std::vector<float>& out, const std::vector<double>& values) {
    out.reserve(out.size() + values.size());
    for (const auto value : values) out.push_back(static_cast<float>(value));
}

Address autorelease(Support& support, Address object) { return object ? support.send_id(object, "autorelease") : 0; }
void release(Support& support, Address object) { if (object) static_cast<void>(support.send_id(object, "release")); }

void store_status(Address out, std::int32_t status, std::string_view text) {
    if (!out) return;
    const auto b = bridge();
    b->store_error(out, b->os_status_error(status, text), true);
}

void update_list(Support& support, PcmBuffer& b) {
    auto& m = support.memory();
    m.write32(b.list, b.planes());
    m.write32(b.list + 4, 0);
    for (std::uint32_t i = 0; i < b.planes(); ++i) {
        const auto at = b.list + 8 + 16ULL * i;
        m.write32(at, b.pcm.non_interleaved ? 1 : b.pcm.channels);
        m.write32(at + 4, static_cast<std::uint32_t>(static_cast<std::uint64_t>(b.length) * b.pcm.frame_bytes()));
        m.write64(at + 8, b.data + i * b.plane_bytes());
    }
}

// Sets up a buffer's planes (zeroed), pointers and list.
void allocate(Support& support, PcmBuffer& b) {
    auto& m = support.memory();
    const auto total = std::max<std::uint64_t>(b.plane_bytes() * b.planes(), 16);
    b.data = support.process.allocate_guest(total, 16);
    m.fill_bytes(b.data, 0, total);
    b.pointers = support.process.allocate_guest(8ULL * b.planes(), 8);
    for (std::uint32_t i = 0; i < b.planes(); ++i) m.write64(b.pointers + 8ULL * i, b.data + i * b.plane_bytes());
    b.list = support.process.allocate_guest(8 + 16ULL * b.planes(), 8);
    update_list(support, b);
}

void free_buffer(Support& support, PcmBuffer& b) {
    for (auto* at : {&b.data, &b.pointers, &b.list})
        if (*at) support.process.release_guest(std::exchange(*at, 0));
    release(support, std::exchange(b.format, 0));
}

std::shared_ptr<TimeState> time_state(Support& support, Address time) { return time ? support.objc.find_host_state<TimeState>(time) : nullptr; }

constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";

void close_file(Support& support, AudioFileState& f) {
    if (f.writer) static_cast<void>(support.call(audio_toolbox, "_ExtAudioFileDispose", {std::exchange(f.writer, 0)}));
    if (!f.open) return;
    close_guest_audio(support, f.audio);
    f.open = false;
}

// The PCM format a common format names (interleaved or not), at `rate` and `channels`.
af::Pcm common_pcm(std::uint64_t common, double rate, std::uint32_t channels, bool interleaved) {
    af::Pcm pcm;
    pcm.rate = rate;
    pcm.channels = channels;
    pcm.non_interleaved = !interleaved;
    pcm.is_signed = true;
    switch (common) {
    case 1: pcm.is_float = true; pcm.bits = 32; pcm.container = 4; break;
    case 2: pcm.is_float = true; pcm.bits = 64; pcm.container = 8; break;
    case 3: pcm.bits = 16; pcm.container = 2; break;
    default: pcm.bits = 32; pcm.container = 4; break;
    }
    return pcm;
}

} // namespace

Address declare(Support& support, std::string_view name, std::string_view superclass) {
    const auto cls = support.declare_class(name, superclass, avf_audio);
    support.objc.publish_class(name, cls, av_foundation);
    return cls;
}

std::vector<std::uint8_t> format_asbd(Support& support, Address format) {
    if (!format || !support.objc.responds(format, support.objc.selector("streamDescription"))) return {};
    const auto at = support.send_id(format, "streamDescription");
    if (!at) return {};
    std::vector<std::uint8_t> asbd(40);
    support.memory().read_bytes(at, asbd);
    return asbd;
}

std::optional<af::Pcm> format_pcm(Support& support, Address format) {
    const auto asbd = format_asbd(support, format);
    if (asbd.empty()) return std::nullopt;
    std::string why;
    return af::pcm_from_asbd(asbd, why);
}

Address make_format(Support& support, const af::Pcm& pcm, const std::vector<std::uint8_t>& layout) {
    auto& m = support.memory();
    const auto asbd = af::asbd_from_pcm(pcm);
    const auto at = support.process.allocate_guest(40, 8);
    m.write_bytes(at, asbd);
    Address channel_layout{};
    if (pcm.channels > 2) {
        const auto layout_class = support.objc.find_class("AVAudioChannelLayout");
        if (!layout.empty()) {
            const auto bytes = support.process.allocate_guest(layout.size(), 8);
            m.write_bytes(bytes, layout);
            channel_layout = support.send_id(support.send_id(layout_class, "alloc"), "initWithLayout:", {bytes});
            support.process.release_guest(bytes);
        } else {
            CallRegs tag{};
            tag.x[2] = (147U << 16) | pcm.channels;   // kAudioChannelLayoutTag_DiscreteInOrder
            channel_layout = support.send(support.send_id(layout_class, "alloc"), "initWithLayoutTag:", tag).x0;
        }
    }
    const auto format_class = support.objc.find_class("AVAudioFormat");
    const auto format = channel_layout
        ? support.send_id(support.send_id(format_class, "alloc"), "initWithStreamDescription:channelLayout:", {at, channel_layout})
        : support.send_id(support.send_id(format_class, "alloc"), "initWithStreamDescription:", {at});
    release(support, channel_layout);
    support.process.release_guest(at);
    return format;
}

Address standard_format(Support& support, double rate, std::uint32_t channels) {
    af::Pcm pcm;
    pcm.rate = rate;
    pcm.channels = channels;
    pcm.is_float = true;
    pcm.bits = 32;
    pcm.container = 4;
    pcm.non_interleaved = true;
    return make_format(support, pcm);
}

Address make_buffer(Support& support, Address format, std::uint32_t capacity) {
    CallRegs r{};
    r.x[2] = format;
    r.x[3] = capacity;
    return support.send(support.send_id(support.objc.find_class("AVAudioPCMBuffer"), "alloc"), "initWithPCMFormat:frameCapacity:", r).x0;
}

std::vector<float> buffer_samples(Support& support, const PcmBuffer& b) {
    auto& m = support.memory();
    const auto bytes = static_cast<std::size_t>(b.length) * b.pcm.frame_bytes();
    std::vector<std::vector<std::uint8_t>> storage;
    std::vector<std::span<const std::uint8_t>> spans;
    for (std::uint32_t i = 0; i < b.planes(); ++i) {
        storage.emplace_back(bytes);
        m.read_bytes(b.data + i * b.plane_bytes(), storage.back());
        spans.emplace_back(storage.back());
    }
    std::vector<float> samples;
    append_floats(samples, af::decode(b.pcm, spans, b.length));
    return samples;
}

void fill_buffer(Support& support, PcmBuffer& b, std::span<const double> interleaved, std::uint32_t channels, std::uint32_t frames) {
    frames = std::min(frames, b.capacity);
    const auto bytes = static_cast<std::size_t>(frames) * b.pcm.frame_bytes();
    std::vector<std::vector<std::uint8_t>> storage(b.planes(), std::vector<std::uint8_t>(bytes));
    std::vector<std::span<std::uint8_t>> spans;
    for (auto& plane : storage) spans.emplace_back(plane);
    af::encode(b.pcm, interleaved, channels, frames, spans);
    for (std::uint32_t i = 0; i < b.planes(); ++i) support.memory().write_bytes(b.data + i * b.plane_bytes(), storage[i]);
    b.length = frames;
    update_list(support, b);
}

std::int32_t decode_file(Support& support, const GuestAudio& audio, Decoded& out, std::uint64_t start, std::uint64_t count) {
    const auto& format = audio.container.format;
    const auto total = audio.frames();
    start = std::min(start, total);
    count = std::min(count, total - start);
    if (count * format.channels > max_decoded_samples) return af::error::unspecified;
    out.channels = format.channels;
    out.rate = format.rate;
    out.samples.clear();
    out.samples.reserve(static_cast<std::size_t>(count * format.channels));
    constexpr std::uint64_t chunk = 65536;
    for (std::uint64_t done = 0; done < count;) {
        const auto frames = std::min(chunk, count - done);
        std::vector<double> samples;
        const auto got = read_audio_frames(support, audio, start + done, static_cast<std::size_t>(frames), samples);
        if (!got) break;
        append_floats(out.samples, samples);
        done += got;
    }
    return 0;
}

std::int32_t decode_bytes(std::span<const std::uint8_t> bytes, Decoded& out) {
    af::Container container;
    std::string why;
    const auto read = [bytes](std::uint64_t offset, std::span<std::uint8_t> to) -> std::size_t {
        if (offset >= bytes.size()) return 0;
        const auto n = std::min<std::size_t>(to.size(), bytes.size() - static_cast<std::size_t>(offset));
        std::memcpy(to.data(), bytes.data() + offset, n);
        return n;
    };
    if (const auto status = af::parse(read, bytes.size(), container, why)) {
        if (status != af::error::unsupported_file_type && status != af::error::unsupported_data_format) return status;
        // Compressed data: decoded through its stream.
        GuestAudio audio;
        audio.size = bytes.size();
        if (const auto result = open_audio_stream(audio, read, status, why)) return result;
        if (audio.frames() * audio.container.format.channels > max_decoded_samples) return af::error::unspecified;
        std::vector<double> samples;
        read_audio_frames(audio, read, 0, static_cast<std::size_t>(audio.frames()), samples);
        out.channels = audio.container.format.channels;
        out.rate = audio.container.format.rate;
        out.samples.clear();
        append_floats(out.samples, samples);
        return 0;
    }
    const auto& format = container.format;
    const auto available = container.data_offset < bytes.size() ? bytes.size() - container.data_offset : 0;
    const auto frames = std::min<std::uint64_t>(container.data_bytes, available) / format.frame_bytes();
    if (frames * format.channels > max_decoded_samples) return af::error::unspecified;
    const auto samples = af::decode(format, {bytes.subspan(static_cast<std::size_t>(container.data_offset), static_cast<std::size_t>(frames * format.frame_bytes()))},
                                    static_cast<std::size_t>(frames));
    out.channels = format.channels;
    out.rate = format.rate;
    out.samples.clear();
    append_floats(out.samples, samples);
    return 0;
}

Address make_time(Support& support, std::optional<std::uint64_t> host_time, std::optional<double> sample_time, double rate) {
    const auto time = support.send_id(support.send_id(support.objc.find_class("AVAudioTime"), "alloc"), "init");
    auto t = support.objc.host_state<TimeState>(time);
    t->host_valid = host_time.has_value();
    t->host = host_time.value_or(0);
    t->sample_valid = sample_time.has_value();
    t->sample = sample_time.value_or(0);
    t->rate = rate;
    return time;
}

void install_avaudio_buffers(Support& support) {
    auto* sp = &support;
    const auto ns_object = "NSObject";

    // ---- AVAudioTime -------------------------------------------------------------------------
    const auto time = declare(support, "AVAudioTime", ns_object);
    const auto set_time = [sp](Address self, std::optional<std::uint64_t> host, std::optional<double> sample, double rate) {
        auto t = sp->objc.host_state<TimeState>(self);
        t->host_valid = host.has_value();
        t->host = host.value_or(0);
        t->sample_valid = sample.has_value();
        t->sample = sample.value_or(0);
        t->rate = rate;
    };
    const auto real = [](const CpuState& s, unsigned i) { return std::bit_cast<double>(s.v[i][0]); };
    // -initWithAudioTimeStamp:sampleRate: (const AudioTimeStamp * x2, rate d0)
    support.method(time, "initWithAudioTimeStamp:sampleRate:", [set_time, real](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) { set_time(s.x[0], std::nullopt, std::nullopt, real(s, 0)); return; }
        const auto flags = m.read32(s.x[2] + 56);
        const auto sample = std::bit_cast<double>(m.read64(s.x[2]));
        const auto host = m.read64(s.x[2] + 8);
        set_time(s.x[0], (flags & timestamp_host_valid) ? std::optional(host) : std::nullopt,
                 (flags & timestamp_sample_valid) ? std::optional(sample) : std::nullopt, real(s, 0));
    }, "@@:^{AudioTimeStamp=dQdQ{SMPTETime=ssIIIssss}II}d");
    support.method(time, "initWithHostTime:", [set_time](CpuState& s, GuestMemory&) { set_time(s.x[0], s.x[2], std::nullopt, 0); }, "@@:Q");
    support.method(time, "initWithSampleTime:atRate:", [set_time, real](CpuState& s, GuestMemory&) {
        set_time(s.x[0], std::nullopt, static_cast<double>(static_cast<std::int64_t>(s.x[2])), real(s, 0));
    }, "@@:qd");
    support.method(time, "initWithHostTime:sampleTime:atRate:", [set_time, real](CpuState& s, GuestMemory&) {
        set_time(s.x[0], s.x[2], static_cast<double>(static_cast<std::int64_t>(s.x[3])), real(s, 0));
    }, "@@:Qqd");
    for (const auto& [factory, init, types] : std::initializer_list<std::tuple<const char*, const char*, const char*>>{
             {"timeWithAudioTimeStamp:sampleRate:", "initWithAudioTimeStamp:sampleRate:", "@@:^{AudioTimeStamp=dQdQ{SMPTETime=ssIIIssss}II}d"},
             {"timeWithHostTime:", "initWithHostTime:", "@@:Q"},
             {"timeWithSampleTime:atRate:", "initWithSampleTime:atRate:", "@@:qd"},
             {"timeWithHostTime:sampleTime:atRate:", "initWithHostTime:sampleTime:atRate:", "@@:Qqd"}}) {
        const std::string selector = init;
        support.method(time, factory, [sp, selector](CpuState& s, GuestMemory&) {
            CallRegs r{};
            r.x = {0, 0, s.x[2], s.x[3], 0, 0, 0, 0};
            r.v[0] = {s.v[0][0], s.v[0][1]};
            s.x[0] = autorelease(*sp, sp->send(sp->send_id(s.x[0], "alloc"), selector, r).x0);
        }, types, true);
    }
    support.method(time, "hostTimeForSeconds:", [real](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint64_t>(std::max(0.0, real(s, 0)) * 1e9);
    }, "Q@:d", true);
    support.method(time, "secondsForHostTime:", [](CpuState& s, GuestMemory&) {
        s.v[0] = {std::bit_cast<std::uint64_t>(static_cast<double>(s.x[2]) / 1e9), 0};
    }, "d@:Q", true);
    support.method(time, "hostTime", [sp](CpuState& s, GuestMemory&) { const auto t = time_state(*sp, s.x[0]); s.x[0] = t ? t->host : 0; }, "Q@:");
    support.method(time, "isHostTimeValid", [sp](CpuState& s, GuestMemory&) { const auto t = time_state(*sp, s.x[0]); s.x[0] = t && t->host_valid; }, "B@:");
    support.method(time, "sampleTime", [sp](CpuState& s, GuestMemory&) {
        const auto t = time_state(*sp, s.x[0]);
        s.x[0] = static_cast<std::uint64_t>(t ? static_cast<std::int64_t>(std::floor(t->sample)) : 0);
    }, "q@:");
    support.method(time, "isSampleTimeValid", [sp](CpuState& s, GuestMemory&) { const auto t = time_state(*sp, s.x[0]); s.x[0] = t && t->sample_valid; }, "B@:");
    support.method(time, "sampleRate", [sp](CpuState& s, GuestMemory&) {
        const auto t = time_state(*sp, s.x[0]);
        s.v[0] = {std::bit_cast<std::uint64_t>(t ? t->rate : 0.0), 0};
    }, "d@:");
    // -audioTimeStamp: a 64-byte struct, returned through x8.
    support.method(time, "audioTimeStamp", [sp](CpuState& s, GuestMemory& m) {
        const auto t = time_state(*sp, s.x[0]);
        const auto out = s.x[8];
        if (!out) return;
        m.fill_bytes(out, 0, 64);
        if (!t) return;
        m.write64(out, std::bit_cast<std::uint64_t>(t->sample));
        m.write64(out + 8, t->host);
        m.write64(out + 16, std::bit_cast<std::uint64_t>(1.0));   // mRateScalar
        m.write32(out + 56, (t->sample_valid ? timestamp_sample_valid : 0) | (t->host_valid ? timestamp_host_valid : 0) |
                                (t->host_valid ? 4U : 0U));   // kAudioTimeStampRateScalarValid with a host time
    }, "{AudioTimeStamp=dQdQ{SMPTETime=ssIIIssss}II}@:");
    // -extrapolateTimeFromAnchor: the missing time from an anchor that has both.
    support.method(time, "extrapolateTimeFromAnchor:", [sp](CpuState& s, GuestMemory&) {
        const auto t = time_state(*sp, s.x[0]);
        const auto anchor = time_state(*sp, s.x[2]);
        if (!t) { s.x[0] = 0; return; }
        auto host = t->host_valid ? std::optional(t->host) : std::nullopt;
        auto sample = t->sample_valid ? std::optional(t->sample) : std::nullopt;
        const double rate = t->rate > 0 ? t->rate : anchor ? anchor->rate : 0;
        if (anchor && anchor->host_valid && anchor->sample_valid && rate > 0) {
            if (host && !sample)
                sample = anchor->sample + (static_cast<double>(*host) - static_cast<double>(anchor->host)) / 1e9 * rate;
            else if (sample && !host)
                host = static_cast<std::uint64_t>(std::max(0.0, static_cast<double>(anchor->host) + (*sample - anchor->sample) / rate * 1e9));
        }
        if (!host || !sample) { s.x[0] = 0; return; }
        s.x[0] = autorelease(*sp, make_time(*sp, host, sample, rate));
    }, "@@:@");
    support.method(time, "copyWithZone:", [sp](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? sp->send_id(s.x[0], "retain") : 0; }, "@@:^v");

    // ---- AVAudioBuffer / AVAudioPCMBuffer ----------------------------------------------------
    const auto audio_buffer = declare(support, "AVAudioBuffer", ns_object);
    const auto pcm_buffer = declare(support, "AVAudioPCMBuffer", "AVAudioBuffer");
    // -initWithPCMFormat:frameCapacity: nil for a format that is not linear PCM or no capacity.
    support.method(pcm_buffer, "initWithPCMFormat:frameCapacity:", [sp](CpuState& s, GuestMemory&) {
        const auto pcm = format_pcm(*sp, s.x[2]);
        const auto capacity = static_cast<std::uint32_t>(s.x[3]);
        if (!pcm || capacity == 0 || static_cast<std::uint64_t>(capacity) * pcm->frame_bytes() > (1ULL << 31)) {
            release(*sp, s.x[0]);
            s.x[0] = 0;
            return;
        }
        auto b = sp->objc.host_state<PcmBuffer>(s.x[0]);
        b->format = sp->send_id(s.x[2], "retain");
        b->pcm = *pcm;
        b->common = sp->send(s.x[2], "commonFormat").x0;
        b->capacity = capacity;
        allocate(*sp, *b);
    }, "@@:@I");
    const auto pcm_state = [sp](Address self) {
        const auto b = sp->objc.find_host_state<PcmBuffer>(self);
        if (!b) unsupported("an AVAudioPCMBuffer that was not initialized with -initWithPCMFormat:frameCapacity:");
        return b;
    };
    support.method(pcm_buffer, "dealloc", [sp, pcm_buffer](CpuState& s, GuestMemory&) {
        if (const auto b = sp->objc.find_host_state<PcmBuffer>(s.x[0])) free_buffer(*sp, *b);
        static_cast<void>(sp->objc.send_super(s.x[0], pcm_buffer, sp->objc.selector("dealloc")));
    }, "v@:");
    support.method(audio_buffer, "format", [sp](CpuState& s, GuestMemory&) {
        const auto b = sp->objc.find_host_state<PcmBuffer>(s.x[0]);
        s.x[0] = b ? b->format : 0;
    }, "@@:");
    support.method(audio_buffer, "audioBufferList", [sp](CpuState& s, GuestMemory&) {
        const auto b = sp->objc.find_host_state<PcmBuffer>(s.x[0]);
        s.x[0] = b ? b->list : 0;
    }, "^{AudioBufferList=I[1{AudioBuffer=II^v}]}@:");
    support.method(audio_buffer, "mutableAudioBufferList", [sp](CpuState& s, GuestMemory&) {
        const auto b = sp->objc.find_host_state<PcmBuffer>(s.x[0]);
        s.x[0] = b ? b->list : 0;
    }, "^{AudioBufferList=I[1{AudioBuffer=II^v}]}@:");
    support.method(pcm_buffer, "frameCapacity", [pcm_state](CpuState& s, GuestMemory&) { s.x[0] = pcm_state(s.x[0])->capacity; }, "I@:");
    support.method(pcm_buffer, "frameLength", [pcm_state](CpuState& s, GuestMemory&) { s.x[0] = pcm_state(s.x[0])->length; }, "I@:");
    support.method(pcm_buffer, "setFrameLength:", [sp, pcm_state](CpuState& s, GuestMemory&) {
        const auto b = pcm_state(s.x[0]);
        const auto length = static_cast<std::uint32_t>(s.x[2]);
        if (length > b->capacity) unsupported("-[AVAudioPCMBuffer setFrameLength:] beyond the frame capacity (an exception on macOS)");
        b->length = length;
        update_list(*sp, *b);
    }, "v@:I");
    support.method(pcm_buffer, "stride", [pcm_state](CpuState& s, GuestMemory&) {
        const auto b = pcm_state(s.x[0]);
        s.x[0] = b->pcm.non_interleaved ? 1 : b->pcm.channels;
    }, "Q@:");
    for (const auto& [selector, common, types] : std::initializer_list<std::tuple<const char*, std::uint64_t, const char*>>{
             {"floatChannelData", 1, "^^f@:"}, {"int16ChannelData", 3, "^^s@:"}, {"int32ChannelData", 4, "^^i@:"}}) {
        const auto wanted = common;
        support.method(pcm_buffer, selector, [pcm_state, wanted](CpuState& s, GuestMemory&) {
            const auto b = pcm_state(s.x[0]);
            s.x[0] = b->common == wanted ? b->pointers : 0;
        }, types);
    }

    // ---- AVAudioFile (reading) -----------------------------------------------------------------
    const auto file = declare(support, "AVAudioFile", ns_object);
    // Opens `url` for reading into `common` (interleaved or not) samples.
    const auto open_file = [sp](CpuState& s, Address url, std::uint64_t common, bool interleaved, Address error_out) {
        const auto self = s.x[0];
        const auto fail_with = [&](std::int32_t status, std::string_view text) {
            store_status(error_out, status, text);
            release(*sp, self);
            s.x[0] = 0;
        };
        const auto scheme = url ? sp->send_id(url, "scheme") : 0;
        if (!url || !scheme || sp->cf.utf8(scheme) != "file") return fail_with(af::error::file_not_found, "AVAudioFile reads file URLs only");
        if (common < 1 || common > 4) return fail_with(param_error, "AVAudioFile: not a common PCM format");
        GuestAudio audio;
        if (const auto status = open_guest_audio(*sp, sp->cf.url_path(url), audio))
            return fail_with(status, status == af::error::unsupported_data_format
                                         ? "The file's audio format has no decoder under Alchemy"
                                         : "The audio file could not be opened");
        auto f = sp->objc.host_state<AudioFileState>(self);
        f->url = sp->send_id(url, "retain");
        f->audio = audio;
        f->open = true;
        const auto& source = audio.container.format;
        auto processing = source;
        processing.non_interleaved = !interleaved;
        processing.big_endian = false;
        processing.is_signed = true;
        processing.aligned_high = false;
        switch (common) {
        case 1: processing.is_float = true; processing.bits = 32; processing.container = 4; break;
        case 2: processing.is_float = true; processing.bits = 64; processing.container = 8; break;
        case 3: processing.is_float = false; processing.bits = 16; processing.container = 2; break;
        default: processing.is_float = false; processing.bits = 32; processing.container = 4; break;
        }
        f->processing = processing;
        const auto layout = af::layout_for_mask(audio.container.channel_mask, source.channels);
        f->file_format = make_format(*sp, source, layout);
        f->processing_format = make_format(*sp, processing, layout);
        if (!f->file_format || !f->processing_format) {
            close_file(*sp, *f);
            return fail_with(af::error::unsupported_data_format, "AVAudioFile: the file's format has no AVAudioFormat");
        }
    };
    support.method(file, "initForReading:error:", [open_file](CpuState& s, GuestMemory&) { open_file(s, s.x[2], 1, false, s.x[3]); }, "@@:@^@");
    support.method(file, "initForReading:commonFormat:interleaved:error:", [open_file](CpuState& s, GuestMemory&) {
        open_file(s, s.x[2], s.x[3], (s.x[4] & 0xff) != 0, s.x[5]);
    }, "@@:@QB^@");
    // Writing: the file's format from the settings, its type from the URL's extension.
    const auto open_for_writing = [sp](CpuState& s, Address url, Address settings, std::uint64_t common, bool interleaved, Address error_out) {
        const auto self = s.x[0];
        const auto fail_with = [&](std::int32_t status, std::string_view text) {
            store_status(error_out, status, text);
            release(*sp, self);
            s.x[0] = 0;
        };
        const auto b = bridge();
        const auto setting = [&](std::string_view key) -> std::optional<double> {
            if (!settings) return std::nullopt;
            const auto name = sp->cf.string(key);
            const auto value = b->lookup(settings, name);
            sp->release(name);
            return value ? b->real(value) : std::nullopt;
        };
        const auto scheme = url ? sp->send_id(url, "scheme") : 0;
        if (!url || !scheme || sp->cf.utf8(scheme) != "file") return fail_with(af::error::file_not_found, "AVAudioFile writes file URLs only");
        if (common < 1 || common > 4) return fail_with(param_error, "AVAudioFile: not a common PCM format");
        auto extension = sp->cf.utf8(sp->send_id(url, "pathExtension"));
        for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::uint32_t type = extension == "wav" || extension == "wave" ? af::four("WAVE")
                                 : extension == "aif" || extension == "aiff" ? af::four("AIFF")
                                 : extension == "aifc" ? af::four("AIFC")
                                 : extension == "caf" ? af::four("caff") : 0;
        if (!type) return fail_with(af::error::unsupported_file_type, "AVAudioFile writes .wav, .aif, .aifc and .caf files");
        if (setting("AVFormatIDKey").value_or(af::four("lpcm")) != af::four("lpcm"))
            return fail_with(af::error::unsupported_data_format, "AVAudioFile writes linear PCM (no audio encoder exists under Alchemy)");
        const auto rate = setting("AVSampleRateKey"), channels = setting("AVNumberOfChannelsKey");
        if (!rate || !(*rate > 0) || !channels || *channels < 1 || *channels > 64) return fail_with(param_error, "AVAudioFile settings need a sample rate and channels");
        af::Pcm pcm;
        pcm.rate = *rate;
        pcm.channels = static_cast<std::uint32_t>(*channels);
        pcm.is_float = setting("AVLinearPCMIsFloatKey").value_or(0) != 0;
        pcm.bits = static_cast<std::uint32_t>(setting("AVLinearPCMBitDepthKey").value_or(pcm.is_float ? 32 : 16));
        pcm.container = (pcm.bits + 7) / 8;
        pcm.big_endian = setting("AVLinearPCMIsBigEndianKey").value_or(type == af::four("AIFF") || type == af::four("AIFC")) != 0;
        pcm.is_signed = pcm.is_float || !(type == af::four("WAVE") && pcm.bits == 8);   // 8-bit WAVE is unsigned
        pcm.aligned_high = !pcm.is_float && pcm.bits < pcm.container * 8 && type != af::four("caff");
        auto& m = sp->memory();
        const auto asbd = sp->process.allocate_guest(48, 8);
        const auto out = asbd + 40;
        m.write_bytes(asbd, af::asbd_from_pcm(pcm));
        m.write64(out, 0);
        // ExtAudioFileCreateWithURL(url, type, &format, NULL, kAudioFileFlags_EraseFile, &out)
        auto status = static_cast<std::int32_t>(sp->call(audio_toolbox, "_ExtAudioFileCreateWithURL", {url, type, asbd, 0, 1, out}));
        const auto writer = m.read64(out);
        const auto processing = common_pcm(common, pcm.rate, pcm.channels, interleaved);
        if (!status) {
            // The client format: the processing format (kExtAudioFileProperty_ClientDataFormat).
            m.write_bytes(asbd, af::asbd_from_pcm(processing));
            status = static_cast<std::int32_t>(sp->call(audio_toolbox, "_ExtAudioFileSetProperty", {writer, af::four("cfmt"), 40, asbd}));
            if (status) static_cast<void>(sp->call(audio_toolbox, "_ExtAudioFileDispose", {writer}));
        }
        sp->process.release_guest(asbd);
        if (status) return fail_with(status, "AVAudioFile could not create the file in that format");
        auto f = sp->objc.host_state<AudioFileState>(self);
        f->url = sp->send_id(url, "retain");
        f->writer = writer;
        f->processing = processing;
        f->file_format = make_format(*sp, pcm);
        f->processing_format = make_format(*sp, processing);
    };
    support.method(file, "initForWriting:settings:error:", [open_for_writing](CpuState& s, GuestMemory&) {
        open_for_writing(s, s.x[2], s.x[3], 1, false, s.x[4]);
    }, "@@:@@^@");
    support.method(file, "initForWriting:settings:commonFormat:interleaved:error:", [open_for_writing](CpuState& s, GuestMemory&) {
        open_for_writing(s, s.x[2], s.x[3], s.x[4], (s.x[5] & 0xff) != 0, s.x[6]);
    }, "@@:@@QB^@");
    const auto file_state = [sp](Address self) {
        const auto f = sp->objc.find_host_state<AudioFileState>(self);
        if (!f) unsupported("an AVAudioFile that was not opened for reading or writing");
        return f;
    };
    support.method(file, "dealloc", [sp, file](CpuState& s, GuestMemory&) {
        if (const auto f = sp->objc.find_host_state<AudioFileState>(s.x[0])) {
            close_file(*sp, *f);
            for (auto* object : {&f->url, &f->file_format, &f->processing_format}) release(*sp, std::exchange(*object, 0));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], file, sp->objc.selector("dealloc")));
    }, "v@:");
    support.method(file, "url", [file_state](CpuState& s, GuestMemory&) { s.x[0] = file_state(s.x[0])->url; }, "@@:");
    support.method(file, "fileFormat", [file_state](CpuState& s, GuestMemory&) { s.x[0] = file_state(s.x[0])->file_format; }, "@@:");
    support.method(file, "processingFormat", [file_state](CpuState& s, GuestMemory&) { s.x[0] = file_state(s.x[0])->processing_format; }, "@@:");
    support.method(file, "length", [file_state](CpuState& s, GuestMemory&) {
        const auto f = file_state(s.x[0]);
        s.x[0] = f->writer ? f->written : f->audio.frames();
    }, "q@:");
    support.method(file, "framePosition", [file_state](CpuState& s, GuestMemory&) {
        const auto f = file_state(s.x[0]);
        s.x[0] = f->writer ? f->written : f->position;
    }, "q@:");
    support.method(file, "setFramePosition:", [file_state](CpuState& s, GuestMemory&) {
        const auto f = file_state(s.x[0]);
        const auto wanted = static_cast<std::int64_t>(s.x[2]);
        f->position = std::min<std::uint64_t>(static_cast<std::uint64_t>(std::max<std::int64_t>(wanted, 0)), f->audio.frames());
    }, "v@:q");
    support.method(file, "isOpen", [file_state](CpuState& s, GuestMemory&) {
        const auto f = file_state(s.x[0]);
        s.x[0] = f->open || f->writer ? 1 : 0;
    }, "B@:");
    // -writeFromBuffer:error: (buffer x2, error x3): the buffer's frames at the end, in the file's format.
    support.method(file, "writeFromBuffer:error:", [sp, file_state](CpuState& s, GuestMemory&) {
        const auto f = file_state(s.x[0]);
        const auto b = s.x[2] ? sp->objc.find_host_state<PcmBuffer>(s.x[2]) : nullptr;
        if (!f->writer || !b || format_asbd(*sp, b->format) != af::asbd_from_pcm(f->processing)) {
            store_status(s.x[3], param_error, !f->writer ? "The AVAudioFile is not open for writing" : "The buffer's format is not the file's processing format");
            s.x[0] = 0;
            return;
        }
        const auto status = static_cast<std::int32_t>(sp->call(audio_toolbox, "_ExtAudioFileWrite", {f->writer, b->length, b->list}));
        if (status) {
            store_status(s.x[3], status, "The audio file could not be written");
            s.x[0] = 0;
            return;
        }
        f->written += b->length;
        s.x[0] = 1;
    }, "B@:@^@");
    support.method(file, "close", [sp, file_state](CpuState& s, GuestMemory&) { close_file(*sp, *file_state(s.x[0])); }, "v@:");
    // Reads up to `count` frames at the position into `buffer` (of the processing format).
    const auto read = [sp, file_state](CpuState& s, Address buffer, std::uint64_t count, Address error_out) {
        const auto f = file_state(s.x[0]);
        const auto b = buffer ? sp->objc.find_host_state<PcmBuffer>(buffer) : nullptr;
        if (!b || !f->open || format_asbd(*sp, b->format) != af::asbd_from_pcm(f->processing)) {
            store_status(error_out, param_error, !f->open ? "The AVAudioFile is closed" : "The buffer's format is not the file's processing format");
            s.x[0] = 0;
            return;
        }
        const auto& source = f->audio.container.format;
        const auto frames = std::min<std::uint64_t>({count, b->capacity, f->audio.frames() - std::min(f->position, f->audio.frames())});
        std::vector<double> samples;
        const auto got = frames ? read_audio_frames(*sp, f->audio, f->position, static_cast<std::size_t>(frames), samples) : 0;
        fill_buffer(*sp, *b, samples, source.channels, static_cast<std::uint32_t>(got));
        f->position += got;
        s.x[0] = 1;
    };
    support.method(file, "readIntoBuffer:error:", [read](CpuState& s, GuestMemory&) { read(s, s.x[2], std::numeric_limits<std::uint64_t>::max(), s.x[3]); }, "B@:@^@");
    support.method(file, "readIntoBuffer:frameCount:error:", [read](CpuState& s, GuestMemory&) { read(s, s.x[2], static_cast<std::uint32_t>(s.x[3]), s.x[4]); }, "B@:@I^@");
}

} // namespace alchemy::gui::completion_kit::media::avaudio
