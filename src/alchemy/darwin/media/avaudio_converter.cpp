#include "alchemy/darwin/media/avaudio.hpp"
#include "alchemy/darwin/media/media_codecs.hpp"
#include "alchemy/darwin/media/resampler.hpp"

#include "alchemy/darwin/security/bridge.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// AVAudioConverter and AVAudioCompressedBuffer:
// - Linear PCM to linear PCM between any sample formats and interleavings, channel counts and
//   sample rates (resampler.hpp), through -convertToBuffer:error:withInputFromBlock:, which asks
//   its block for input as the output buffer needs it and flushes the resampler at the end of the
//   stream. -convertToBuffer:fromBuffer:error: converts everything but the rate, as documented.
// - Channels: by default output channel i takes input channel i, a mono input feeds every output
//   channel, and output channels beyond the input's are silent; a channelMap names an input
//   channel (or -1, silence) for each output channel; with downmix and no map, more input channels
//   than output channels are averaged into them in turn (channel i into i modulo the output's).
// - Compressed input with a decoder here (MPEG audio layers I-III, Opus, FLAC: media_codecs.hpp)
//   from AVAudioCompressedBuffers, packet by packet (their packet descriptions, or the format's
//   constant packet size), the format's magic cookie configuring the decoder.
// - AVAudioCompressedBuffer: packets and their descriptions in guest memory, behind one
//   AudioBufferList.
// - Unavailable, as documented: compressed output (no encoder here) and compressed input without
//   a decoder: -initFromFormat:toFormat: answers nil.
namespace alchemy::gui::completion_kit::media::avaudio {
namespace {

namespace af = audio_file;

constexpr std::int32_t param_error = -50;
constexpr std::int32_t format_not_supported = static_cast<std::int32_t>(0x666D743F);   // 'fmt?'
// AVAudioConverterInputStatus and AVAudioConverterOutputStatus
constexpr std::uint64_t input_have_data = 0, input_no_data_now = 1, input_end_of_stream = 2;
constexpr std::int64_t output_have_data = 0, output_input_ran_dry = 1, output_end_of_stream = 2, output_error = 3;

struct CompressedBuffer final : ObjectPayload {
    Address format{};                  // retained AVAudioFormat
    std::uint32_t packet_capacity{}, packet_count{};
    std::uint64_t maximum_packet_size{};
    std::uint32_t byte_capacity{}, byte_length{};
    std::uint32_t channels{};
    Address data{}, descriptions{}, list{};   // guest memory (descriptions: 0 for constant packet sizes)
};

struct Converter final : ObjectPayload {
    Address input_format{}, output_format{};   // retained
    af::Pcm out;
    bool compressed{};                 // input decoded by `decoder`
    std::uint32_t bytes_per_packet{};  // compressed input with constant packet sizes
    std::unique_ptr<codecs::AudioDecoder> decoder;
    double in_rate{};
    std::uint32_t in_channels{};
    std::vector<int> map;              // per output channel: an input channel, or -1
    bool custom_map{}, downmix{}, dither{};
    std::int64_t quality{}, bit_rate{};
    Address algorithm{};               // retained NSString
    std::unique_ptr<Resampler> resampler;
    std::vector<double> pending;       // input frames in the output's channels, interleaved
    std::size_t pending_offset{};      // frames already used
    bool ended{};
    std::uint64_t total_in{}, total_out{};
    [[nodiscard]] std::size_t pending_frames() const { return out.channels ? pending.size() / out.channels - pending_offset : 0; }
};

std::shared_ptr<security::Bridge> bridge() { return security::bridge(); }

void store_status(Address out, std::int32_t status, std::string_view text) {
    if (!out) return;
    const auto b = bridge();
    b->store_error(out, b->os_status_error(status, text), true);
}

std::vector<int> default_map(std::uint32_t inputs, std::uint32_t outputs) {
    std::vector<int> map(outputs, -1);
    for (std::uint32_t c = 0; c < outputs; ++c) map[c] = c < inputs ? static_cast<int>(c) : inputs == 1 ? 0 : -1;
    return map;
}

// Appends `frames` interleaved frames of `channels` to the converter's pending input, mapped to
// its output channels.
void map_into(Converter& c, const float* in, std::uint32_t channels, std::size_t frames) {
    const auto outputs = c.out.channels;
    const bool averaging = c.downmix && !c.custom_map && channels > outputs;
    std::vector<double> counts(outputs, 0.0);
    if (averaging)
        for (std::uint32_t i = 0; i < channels; ++i) counts[i % outputs] += 1;
    c.pending.reserve(c.pending.size() + frames * outputs);
    for (std::size_t f = 0; f < frames; ++f) {
        const float* frame = in + f * channels;
        for (std::uint32_t o = 0; o < outputs; ++o) {
            double value = 0;
            if (averaging) {
                for (std::uint32_t i = o; i < channels; i += outputs) value += frame[i];
                value /= counts[o];
            } else if (const int source = o < c.map.size() ? c.map[o] : -1; source >= 0 && static_cast<std::uint32_t>(source) < channels) {
                value = frame[source];
            }
            c.pending.push_back(value);
        }
    }
    c.total_in += frames;
}

// The interleaved float samples of a compressed buffer's packets, decoded: false, with `why`,
// when a packet does not decode.
bool decode_packets(Support& support, Converter& c, const CompressedBuffer& b, std::vector<float>& out, std::string& why) {
    auto& m = support.memory();
    for (std::uint32_t i = 0; i < b.packet_count; ++i) {
        std::uint64_t offset{}, size{};
        if (b.descriptions) {
            const auto at = b.descriptions + 16ULL * i;
            offset = m.read64(at);
            size = m.read32(at + 12);
        } else {
            offset = static_cast<std::uint64_t>(i) * c.bytes_per_packet;
            size = c.bytes_per_packet;
        }
        if (offset + size > b.byte_length) { why = "a packet beyond the buffer's byte length"; return false; }
        std::vector<std::uint8_t> packet(static_cast<std::size_t>(size));
        m.read_bytes(b.data + offset, packet);
        if (!c.decoder->decode(packet, out, why)) return false;
    }
    return true;
}

class Converters : public std::enable_shared_from_this<Converters> {
public:
    explicit Converters(Support& support) : support_(support) {}
    void install();

private:
    std::shared_ptr<Converter> converter(Address self) {
        const auto c = support_.objc.find_host_state<Converter>(self);
        if (!c) unsupported("an AVAudioConverter that was not initialized with -initFromFormat:toFormat:");
        return c;
    }
    std::shared_ptr<CompressedBuffer> compressed(Address self) {
        const auto b = support_.objc.find_host_state<CompressedBuffer>(self);
        if (!b) unsupported("an AVAudioCompressedBuffer that was not initialized with -initWithFormat:packetCapacity:...");
        return b;
    }
    void install_compressed_buffer();
    void install_converter();
    // Takes one buffer the input block returned into the pending input: false, with `why`, when
    // it is not usable.
    bool take(Converter& c, Address buffer, std::string& why);
    // Writes `frames` output frames from the pending input (the resampler pads with silence once
    // the input has ended) into `out`, interleaved in the output's channels.
    void produce(Converter& c, std::vector<double>& out, std::size_t frames);

    Support& support_;
    Address compressed_class_{}, converter_class_{};
};

bool Converters::take(Converter& c, Address buffer, std::string& why) {
    if (!buffer) { why = "the input block returned nil with AVAudioConverterInputStatus_HaveData"; return false; }
    if (const auto pcm = support_.objc.find_host_state<PcmBuffer>(buffer)) {
        if (c.compressed) { why = "the input block returned a PCM buffer for a compressed input format"; return false; }
        if (pcm->pcm.channels != c.in_channels || pcm->pcm.rate != c.in_rate) { why = "the input buffer's format is not the converter's input format"; return false; }
        const auto samples = buffer_samples(support_, *pcm);
        map_into(c, samples.data(), c.in_channels, pcm->length);
        return true;
    }
    if (const auto packets = support_.objc.find_host_state<CompressedBuffer>(buffer)) {
        if (!c.compressed) { why = "the input block returned a compressed buffer for a PCM input format"; return false; }
        std::vector<float> samples;
        if (!decode_packets(support_, c, *packets, samples, why)) return false;
        map_into(c, samples.data(), c.in_channels, samples.size() / std::max(1U, c.in_channels));
        return true;
    }
    why = "the input block returned an object that is not an AVAudioBuffer";
    return false;
}

void Converters::produce(Converter& c, std::vector<double>& out, std::size_t frames) {
    const auto channels = c.out.channels;
    out.assign(frames * channels, 0.0);
    const auto pull = [&c, channels](double* into, std::size_t count) {
        const auto have = std::min(count, c.pending_frames());
        std::copy_n(c.pending.begin() + static_cast<std::ptrdiff_t>(c.pending_offset * channels), have * channels, into);
        std::fill(into + have * channels, into + count * channels, 0.0);
        c.pending_offset += have;
    };
    if (c.resampler) c.resampler->render(out.data(), frames, pull);
    else pull(out.data(), frames);
    c.total_out += frames;
    // Drop what has been used.
    if (c.pending_offset > 4096 || c.pending_offset * channels == c.pending.size()) {
        c.pending.erase(c.pending.begin(), c.pending.begin() + static_cast<std::ptrdiff_t>(c.pending_offset * channels));
        c.pending_offset = 0;
    }
}

void Converters::install_compressed_buffer() {
    compressed_class_ = declare(support_, "AVAudioCompressedBuffer", "AVAudioBuffer");
    auto* sp = &support_;
    // -initWithFormat:packetCapacity:maximumPacketSize: (format x2, UInt32 x3, NSInteger x4) and
    // -initWithFormat:packetCapacity: (constant packet sizes: the format's bytes per packet).
    const auto init = [sp](CpuState& s, Address format, std::uint32_t capacity, std::uint64_t maximum) {
        const auto asbd = format_asbd(*sp, format);
        std::uint32_t bytes_per_packet{}, channels{};
        if (asbd.size() == 40) {
            std::memcpy(&bytes_per_packet, asbd.data() + 16, 4);
            std::memcpy(&channels, asbd.data() + 28, 4);
        }
        if (!maximum) maximum = bytes_per_packet;
        const auto bytes = static_cast<std::uint64_t>(capacity) * maximum;
        if (asbd.size() != 40 || !capacity || !maximum || bytes > (1ULL << 31)) {
            if (asbd.size() == 40 && capacity && !maximum)
                unsupported("-[AVAudioCompressedBuffer initWithFormat:packetCapacity:] for a format with variable packet sizes "
                            "(an exception on macOS; use -initWithFormat:packetCapacity:maximumPacketSize:)");
            sp->release(s.x[0]);
            s.x[0] = 0;
            return;
        }
        auto b = sp->objc.host_state<CompressedBuffer>(s.x[0]);
        b->format = sp->retain(format);
        b->packet_capacity = capacity;
        b->maximum_packet_size = maximum;
        b->byte_capacity = static_cast<std::uint32_t>(bytes);
        b->channels = channels;
        auto& m = sp->memory();
        b->data = sp->process.allocate_guest(std::max<std::uint64_t>(bytes, 16), 16);
        m.fill_bytes(b->data, 0, std::max<std::uint64_t>(bytes, 16));
        if (!bytes_per_packet) {
            b->descriptions = sp->process.allocate_guest(16ULL * capacity, 8);
            m.fill_bytes(b->descriptions, 0, 16ULL * capacity);
        }
        b->list = sp->process.allocate_guest(24, 8);
        m.write32(b->list, 1);
        m.write32(b->list + 8, channels);
        m.write32(b->list + 12, 0);
        m.write64(b->list + 16, b->data);
    };
    support_.method(compressed_class_, "initWithFormat:packetCapacity:maximumPacketSize:", [init](CpuState& s, GuestMemory&) {
        init(s, s.x[2], static_cast<std::uint32_t>(s.x[3]), s.x[4]);
    }, "@@:@Iq");
    support_.method(compressed_class_, "initWithFormat:packetCapacity:", [init](CpuState& s, GuestMemory&) {
        init(s, s.x[2], static_cast<std::uint32_t>(s.x[3]), 0);
    }, "@@:@I");
    support_.method(compressed_class_, "dealloc", [sp, cls = compressed_class_](CpuState& s, GuestMemory&) {
        if (const auto b = sp->objc.find_host_state<CompressedBuffer>(s.x[0])) {
            for (auto* at : {&b->data, &b->descriptions, &b->list})
                if (*at) sp->process.release_guest(std::exchange(*at, 0));
            sp->release(std::exchange(b->format, 0));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], cls, sp->objc.selector("dealloc")));
    }, "v@:");
    const auto self_state = [self = shared_from_this()](Address object) { return self->compressed(object); };
    support_.method(compressed_class_, "format", [self_state](CpuState& s, GuestMemory&) { s.x[0] = self_state(s.x[0])->format; }, "@@:");
    for (const auto* selector : {"audioBufferList", "mutableAudioBufferList"})
        support_.method(compressed_class_, selector, [self_state](CpuState& s, GuestMemory&) { s.x[0] = self_state(s.x[0])->list; },
                        "^{AudioBufferList=I[1{AudioBuffer=II^v}]}@:");
    support_.method(compressed_class_, "packetCapacity", [self_state](CpuState& s, GuestMemory&) { s.x[0] = self_state(s.x[0])->packet_capacity; }, "I@:");
    support_.method(compressed_class_, "packetCount", [self_state](CpuState& s, GuestMemory&) { s.x[0] = self_state(s.x[0])->packet_count; }, "I@:");
    support_.method(compressed_class_, "setPacketCount:", [self_state](CpuState& s, GuestMemory&) {
        const auto b = self_state(s.x[0]);
        const auto count = static_cast<std::uint32_t>(s.x[2]);
        if (count > b->packet_capacity) unsupported("-[AVAudioCompressedBuffer setPacketCount:] beyond the packet capacity (an exception on macOS)");
        b->packet_count = count;
    }, "v@:I");
    support_.method(compressed_class_, "maximumPacketSize", [self_state](CpuState& s, GuestMemory&) { s.x[0] = self_state(s.x[0])->maximum_packet_size; }, "q@:");
    support_.method(compressed_class_, "data", [self_state](CpuState& s, GuestMemory&) { s.x[0] = self_state(s.x[0])->data; }, "^v@:");
    support_.method(compressed_class_, "byteCapacity", [self_state](CpuState& s, GuestMemory&) { s.x[0] = self_state(s.x[0])->byte_capacity; }, "I@:");
    support_.method(compressed_class_, "byteLength", [self_state](CpuState& s, GuestMemory&) { s.x[0] = self_state(s.x[0])->byte_length; }, "I@:");
    support_.method(compressed_class_, "setByteLength:", [sp, self_state](CpuState& s, GuestMemory&) {
        const auto b = self_state(s.x[0]);
        const auto length = static_cast<std::uint32_t>(s.x[2]);
        if (length > b->byte_capacity) unsupported("-[AVAudioCompressedBuffer setByteLength:] beyond the byte capacity (an exception on macOS)");
        b->byte_length = length;
        sp->memory().write32(b->list + 12, length);
    }, "v@:I");
    support_.method(compressed_class_, "packetDescriptions", [self_state](CpuState& s, GuestMemory&) { s.x[0] = self_state(s.x[0])->descriptions; },
                    "^{AudioStreamPacketDescription=qII}@:");
}

void Converters::install_converter() {
    converter_class_ = declare(support_, "AVAudioConverter", "NSObject");
    auto* sp = &support_;
    // -initFromFormat:(AVAudioFormat *)fromFormat toFormat:(AVAudioFormat *)toFormat
    support_.method(converter_class_, "initFromFormat:toFormat:", [sp](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], from = s.x[2], to = s.x[3];
        const auto out = format_pcm(*sp, to);
        const auto in = format_pcm(*sp, from);
        const auto from_asbd = format_asbd(*sp, from);
        auto c = std::make_shared<Converter>();
        bool usable = out.has_value() && from_asbd.size() == 40;
        if (usable && in) {
            c->in_rate = in->rate;
            c->in_channels = in->channels;
        } else if (usable) {
            std::uint32_t codec{}, channels{};
            double rate{};
            std::memcpy(&rate, from_asbd.data(), 8);
            std::memcpy(&codec, from_asbd.data() + 8, 4);
            std::memcpy(&c->bytes_per_packet, from_asbd.data() + 16, 4);
            std::memcpy(&channels, from_asbd.data() + 28, 4);
            std::vector<std::uint8_t> cookie;
            if (const auto data = sp->send_id(from, "magicCookie")) {
                cookie.resize(static_cast<std::size_t>(sp->send(data, "length").x0));
                if (!cookie.empty()) sp->memory().read_bytes(sp->send_id(data, "bytes"), cookie);
            }
            std::string why;
            c->decoder = codecs::make_audio_decoder(codec, rate, channels, cookie, why);
            usable = c->decoder != nullptr;
            if (usable) {
                c->compressed = true;
                c->in_rate = c->decoder->rate;
                c->in_channels = c->decoder->channels;
            }
        }
        if (!usable || !c->in_channels) {
            sp->release(self);
            s.x[0] = 0;
            return;
        }
        c->out = *out;
        c->map = default_map(c->in_channels, c->out.channels);
        if (c->in_rate != c->out.rate) c->resampler = std::make_unique<Resampler>(c->in_rate, c->out.rate, c->out.channels);
        c->input_format = sp->retain(from);
        c->output_format = sp->retain(to);
        static_cast<void>(sp->objc.host_state<Converter>(self, [&] { return c; }));
        s.x[0] = self;
    }, "@@:@@");
    support_.method(converter_class_, "dealloc", [sp, cls = converter_class_](CpuState& s, GuestMemory&) {
        if (const auto c = sp->objc.find_host_state<Converter>(s.x[0])) {
            sp->release(std::exchange(c->input_format, 0));
            sp->release(std::exchange(c->output_format, 0));
            sp->release(std::exchange(c->algorithm, 0));
        }
        static_cast<void>(sp->objc.send_super(s.x[0], cls, sp->objc.selector("dealloc")));
    }, "v@:");
    const auto state = [self = shared_from_this()](Address object) { return self->converter(object); };
    support_.method(converter_class_, "inputFormat", [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->input_format; }, "@@:");
    support_.method(converter_class_, "outputFormat", [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->output_format; }, "@@:");
    support_.method(converter_class_, "reset", [state](CpuState& s, GuestMemory&) {
        const auto c = state(s.x[0]);
        c->pending.clear();
        c->pending_offset = 0;
        c->ended = false;
        c->total_in = c->total_out = 0;
        if (c->resampler) c->resampler->reset();
        if (c->decoder) c->decoder->reset();
    }, "v@:");
    // channelMap: an NSArray of NSNumbers, one per output channel (-1: silence).
    support_.method(converter_class_, "channelMap", [sp, state](CpuState& s, GuestMemory&) {
        const auto c = state(s.x[0]);
        std::vector<Address> items;
        for (const auto source : c->map) items.push_back(sp->cf.integer(source));
        const auto array = sp->cf.array(items);
        for (const auto item : items) sp->release(item);
        s.x[0] = sp->send_id(array, "autorelease");
    }, "@@:");
    support_.method(converter_class_, "setChannelMap:", [sp, state](CpuState& s, GuestMemory&) {
        const auto c = state(s.x[0]);
        const auto values = s.x[2] ? sp->cf.array_values(s.x[2]) : std::vector<std::uint64_t>{};
        if (values.size() != c->out.channels) unsupported("-[AVAudioConverter setChannelMap:] with a count other than the output's channels (an exception on macOS)");
        std::vector<int> map;
        for (const auto value : values) {
            const auto number = sp->cf.number_value(value);
            const int source = number ? static_cast<int>(*number) : -1;
            if (source >= static_cast<int>(c->in_channels)) unsupported("-[AVAudioConverter setChannelMap:] naming a channel the input does not have");
            map.push_back(source);
        }
        c->map = std::move(map);
        c->custom_map = true;
    }, "v@:@");
    const auto flag = [this, state](const char* getter, const char* setter, bool Converter::*member) {
        support_.method(converter_class_, getter, [state, member](CpuState& s, GuestMemory&) { s.x[0] = (*state(s.x[0])).*member ? 1 : 0; }, "B@:");
        support_.method(converter_class_, setter, [state, member](CpuState& s, GuestMemory&) { (*state(s.x[0])).*member = (s.x[2] & 0xff) != 0; }, "v@:B");
    };
    flag("downmix", "setDownmix:", &Converter::downmix);
    flag("dither", "setDither:", &Converter::dither);
    const auto integer = [this, state](const char* getter, const char* setter, std::int64_t Converter::*member) {
        support_.method(converter_class_, getter, [state, member](CpuState& s, GuestMemory&) { s.x[0] = static_cast<std::uint64_t>((*state(s.x[0])).*member); }, "q@:");
        support_.method(converter_class_, setter, [state, member](CpuState& s, GuestMemory&) { (*state(s.x[0])).*member = static_cast<std::int64_t>(s.x[2]); }, "v@:q");
    };
    integer("sampleRateConverterQuality", "setSampleRateConverterQuality:", &Converter::quality);
    integer("bitRate", "setBitRate:", &Converter::bit_rate);
    support_.method(converter_class_, "sampleRateConverterAlgorithm", [state](CpuState& s, GuestMemory&) { s.x[0] = state(s.x[0])->algorithm; }, "@@:");
    support_.method(converter_class_, "setSampleRateConverterAlgorithm:", [sp, state](CpuState& s, GuestMemory&) {
        const auto c = state(s.x[0]);
        sp->release(std::exchange(c->algorithm, sp->retain(s.x[2])));
    }, "v@:@");
    // No encoder: there is no output cookie and no encode bit rate to offer.
    for (const auto* selector : {"magicCookie", "availableEncodeBitRates", "applicableEncodeBitRates", "availableEncodeSampleRates",
                                 "applicableEncodeSampleRates", "availableEncodeChannelLayoutTags"})
        support_.method(converter_class_, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");

    // -convertToBuffer:(AVAudioPCMBuffer *)outputBuffer fromBuffer:(const AVAudioPCMBuffer *)inputBuffer error:
    support_.method(converter_class_, "convertToBuffer:fromBuffer:error:", [sp, state](CpuState& s, GuestMemory&) {
        const auto c = state(s.x[0]);
        const auto out = s.x[2] ? sp->objc.find_host_state<PcmBuffer>(s.x[2]) : nullptr;
        const auto in = s.x[3] ? sp->objc.find_host_state<PcmBuffer>(s.x[3]) : nullptr;
        const auto fail = [&](std::int32_t status, std::string_view text) { store_status(s.x[4], status, text); s.x[0] = 0; };
        if (!out || !in || c->compressed) return fail(param_error, "AVAudioConverter converts one PCM buffer into another here");
        if (c->resampler) return fail(param_error, "AVAudioConverter: -convertToBuffer:fromBuffer:error: does not convert sample rates");
        if (in->pcm.channels != c->in_channels || out->pcm.channels != c->out.channels) return fail(param_error, "AVAudioConverter: a buffer is not in the converter's format");
        if (in->length > out->capacity) return fail(param_error, "AVAudioConverter: the output buffer is too small");
        const auto samples = buffer_samples(*sp, *in);
        Converter scratch;
        scratch.out = c->out;
        scratch.map = c->map;
        scratch.custom_map = c->custom_map;
        scratch.downmix = c->downmix;
        map_into(scratch, samples.data(), c->in_channels, in->length);
        fill_buffer(*sp, *out, scratch.pending, c->out.channels, in->length);
        s.x[0] = 1;
    }, "B@:@@^@");

    // -convertToBuffer:(AVAudioBuffer *)outputBuffer error:(NSError **)outError
    //     withInputFromBlock:(AVAudioBuffer * (^)(AVAudioPacketCount inNumberOfPackets, AVAudioConverterInputStatus *outStatus))
    support_.method(converter_class_, "convertToBuffer:error:withInputFromBlock:", [self = shared_from_this(), sp, state](CpuState& s, GuestMemory&) {
        const auto c = state(s.x[0]);
        const auto out = s.x[2] ? sp->objc.find_host_state<PcmBuffer>(s.x[2]) : nullptr;
        const auto error_out = s.x[3], block = s.x[4];
        if (!out || out->pcm.channels != c->out.channels) {
            store_status(error_out, format_not_supported, "AVAudioConverter fills PCM buffers of its output format here");
            s.x[0] = static_cast<std::uint64_t>(output_error);
            return;
        }
        const auto capacity = static_cast<std::size_t>(out->capacity);
        const auto b = bridge();
        const auto status_cell = sp->process.allocate_guest(8, 8);
        bool dry = false;
        std::string why;
        // Asks the block for input until `frames` input frames are pending, or it has no more.
        const auto gather = [&](std::size_t frames) {
            for (int asks = 0; c->pending_frames() < frames && !c->ended && !dry && why.empty() && asks < 4096; ++asks) {
                auto wanted = frames - c->pending_frames();
                if (c->compressed) wanted = (wanted + std::max(1U, c->decoder->max_frames) - 1) / std::max(1U, c->decoder->max_frames);
                sp->memory().write64(status_cell, input_have_data);
                const auto buffer = b->invoke_block(block, {static_cast<Address>(std::min<std::size_t>(wanted, 0xffffffffU)), status_cell});
                const auto status = sp->memory().read64(status_cell);
                if (status == input_no_data_now) dry = true;
                else if (status == input_end_of_stream) c->ended = true;
                else {
                    const auto before = c->pending_frames();
                    if (!self->take(*c, buffer, why)) break;
                    if (c->pending_frames() == before) dry = true;   // no frames: as no data now
                }
            }
        };
        std::size_t frames = 0;
        if (!c->resampler) {
            gather(capacity);
            frames = std::min(capacity, c->pending_frames());
        } else {
            gather(c->resampler->input_needed(capacity));
            if (c->pending_frames() >= c->resampler->input_needed(capacity)) {
                frames = capacity;
            } else if (c->ended) {
                // The stream's remaining output: its length at the output's rate, the resampler's
                // history flushed with silence.
                const auto total = static_cast<std::uint64_t>(std::llround(static_cast<double>(c->total_in) * c->out.rate / c->in_rate));
                frames = static_cast<std::size_t>(std::min<std::uint64_t>(capacity, total > c->total_out ? total - c->total_out : 0));
            } else {
                // As much as the pending input makes.
                std::size_t low = 0, high = capacity;
                while (low < high) {
                    const auto middle = (low + high + 1) / 2;
                    if (c->resampler->input_needed(middle) <= c->pending_frames()) low = middle;
                    else high = middle - 1;
                }
                frames = low;
            }
        }
        sp->process.release_guest(status_cell);
        if (!why.empty()) {
            store_status(error_out, param_error, "AVAudioConverter: " + why);
            s.x[0] = static_cast<std::uint64_t>(output_error);
            return;
        }
        std::vector<double> converted;
        self->produce(*c, converted, frames);
        fill_buffer(*sp, *out, converted, c->out.channels, static_cast<std::uint32_t>(frames));
        const bool finished = c->ended && c->pending_frames() == 0 &&
            (!c->resampler || c->total_out >= static_cast<std::uint64_t>(std::llround(static_cast<double>(c->total_in) * c->out.rate / c->in_rate)));
        std::int64_t result = output_have_data;
        if (frames == 0 && finished) result = output_end_of_stream;
        else if (dry) result = output_input_ran_dry;
        s.x[0] = static_cast<std::uint64_t>(result);
    }, "q@:@^@@?");
}

void Converters::install() {
    install_compressed_buffer();
    install_converter();
}

} // namespace

void install_avaudio_converter(Support& support) { std::make_shared<Converters>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media::avaudio
