#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/audio_file_core.hpp"
#include "alchemy/darwin/media/media_codecs.hpp"
#include "alchemy/darwin/media/media_container.hpp"
#include "alchemy/darwin/media/resampler.hpp"

#include "alchemy/engine_adapter.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

// AudioConverter (<AudioToolbox/AudioConverter.h>) between linear PCM formats: any integer
// (8-32 bits, either endianness and signedness, high- or low-aligned) or float (32/64-bit)
// layout, interleaved or not, any channel counts (kAudioConverterChannelMap, else the same
// channels, mono to every channel, every channel to mono averaged, or the first channels in
// order) and any sample rates (resampler.hpp). AudioConverterFillComplexBuffer pulls its input
// through the input procedure until the output is full or the input ends (0 packets, or an
// error, whose status it returns once nothing more comes out). AudioConverterConvertBuffer and
// AudioConverterConvertComplexBuffer convert between equal sample rates
// (kAudioConverterErr_InvalidInputSize otherwise, as the API documents for rate changes).
// Decoding: MPEG audio ('.mp1', '.mp2', '.mp3'), Opus and FLAC input converts to any of those PCM
// outputs (media_codecs.hpp). The input procedure hands over packets, with their descriptions
// (without them, one packet, or MPEG frames found by their headers); the magic cookie
// (kAudioConverterDecompressionMagicCookie) configures Opus and FLAC, and a FLAC stream without
// one is described by the input format. Other compressed formats (AAC, ALAC, ...) and every
// encoder answer kAudioConverterErr_FormatNotSupported from AudioConverterNew.
namespace alchemy::gui::completion_kit::media {
namespace {

namespace af = audio_file;
namespace mc = container;

constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::uint32_t four(const char (&code)[5]) { return af::four(code); }

namespace error {
inline constexpr std::int32_t format_not_supported = static_cast<std::int32_t>(four("fmt?")),
    property_not_supported = static_cast<std::int32_t>(four("prop")), invalid_input_size = static_cast<std::int32_t>(four("insz")),
    invalid_output_size = static_cast<std::int32_t>(four("otsz")), bad_property_size = static_cast<std::int32_t>(four("!siz")),
    unspecified = static_cast<std::int32_t>(four("what")), param = -50;
}

struct Converter {
    af::Pcm in, out;
    std::vector<std::uint8_t> in_asbd, out_asbd;
    std::vector<std::int32_t> channel_map;    // per output channel: the input channel, or -1
    std::unique_ptr<Resampler> resampler;
    std::deque<double> pending;               // input frames in the output's channels, not yet resampled
    std::uint64_t input_frames{};             // since the last reset
    std::uint64_t output_frames{};
    bool ended{};
    std::int32_t end_status{};
    std::uint32_t quality{0x60}, complexity{four("norm")}, prime_method{2};
    std::uint64_t scratch{};                  // guest memory for the input procedure
    std::size_t scratch_size{};
    // Decoding: the input's format ID (0 for PCM input; `in` is then the decoded float format),
    // its description, the magic cookie, and the decoder (made at the first conversion).
    std::uint32_t codec{};
    double codec_rate{};
    std::uint32_t codec_channels{}, codec_flags{}, codec_frames_per_packet{};
    std::vector<std::uint8_t> cookie;
    std::unique_ptr<codecs::AudioDecoder> decoder;
    std::mutex mutex;
};

bool decodable(std::uint32_t format_id) {
    return format_id == four(".mp1") || format_id == four(".mp2") || format_id == four(".mp3") || format_id == four("opus") ||
           format_id == four("flac");
}

// The decoder's configuration: the cookie, or for FLAC without one a STREAMINFO from the format.
std::vector<std::uint8_t> decoder_config(const Converter& c) {
    if (!c.cookie.empty() || c.codec != four("flac")) return c.cookie;
    const std::uint32_t bits = c.codec_flags == 2 ? 20 : c.codec_flags == 3 ? 24 : c.codec_flags == 4 ? 32 : 16;
    const std::uint32_t block = c.codec_frames_per_packet ? std::min<std::uint32_t>(c.codec_frames_per_packet, 65535) : 4096;
    const auto rate = static_cast<std::uint32_t>(c.codec_rate);
    std::vector<std::uint8_t> info{0, 0, 0, 34, 0, 16, static_cast<std::uint8_t>(block >> 8), static_cast<std::uint8_t>(block), 0, 0, 0, 0, 0, 0};
    info.push_back(static_cast<std::uint8_t>(rate >> 12));
    info.push_back(static_cast<std::uint8_t>(rate >> 4));
    info.push_back(static_cast<std::uint8_t>(((rate & 15) << 4) | (((c.codec_channels - 1) & 7) << 1) | (((bits - 1) >> 4) & 1)));
    info.push_back(static_cast<std::uint8_t>(((bits - 1) & 15) << 4));
    info.resize(4 + 34, 0);
    return info;
}

// Makes the decoder if there is none: its rate and channels become the input's.
std::int32_t ensure_decoder(Converter& c) {
    if (!c.codec || c.decoder) return 0;
    std::string why;
    const auto config = decoder_config(c);
    c.decoder = codecs::make_audio_decoder(c.codec == four(".mp1") || c.codec == four(".mp2") ? four(".mp3") : c.codec, c.codec_rate,
                                           c.codec_channels, config, why);
    if (!c.decoder) return error::format_not_supported;
    c.in.rate = c.decoder->rate;
    c.in.channels = c.decoder->channels;
    return 0;
}

// The input frame `frame` (interleaved doubles in `in` channels) mapped to the output channels.
void map_channels(const Converter& c, const double* frame, double* out) {
    const auto in = c.in.channels, out_channels = c.out.channels;
    if (!c.channel_map.empty()) {
        for (std::uint32_t o = 0; o < out_channels; ++o) {
            const auto source = o < c.channel_map.size() ? c.channel_map[o] : -1;
            out[o] = source >= 0 && static_cast<std::uint32_t>(source) < in ? frame[source] : 0.0;
        }
        return;
    }
    if (in == out_channels) {
        std::copy_n(frame, in, out);
    } else if (in == 1) {
        std::fill_n(out, out_channels, frame[0]);
    } else if (out_channels == 1) {
        double sum = 0;
        for (std::uint32_t i = 0; i < in; ++i) sum += frame[i];
        out[0] = sum / in;
    } else {
        for (std::uint32_t o = 0; o < out_channels; ++o) out[o] = o < in ? frame[o] : 0.0;
    }
}

class Converters : public std::enable_shared_from_this<Converters> {
public:
    explicit Converters(Support& support) : support_(support) {}
    void install();

private:
    std::shared_ptr<Converter> find(std::uint64_t handle) {
        std::lock_guard lock(mutex_);
        const auto found = converters_.find(handle);
        return found == converters_.end() ? nullptr : found->second;
    }
    std::uint64_t call(std::uint64_t function, std::initializer_list<std::uint64_t> arguments) {
        return EngineAdapter(support_.process.calling_engine()).call(function, std::span(arguments.begin(), arguments.size()));
    }
    // Decodes `frames` frames of the buffers of `list` (an AudioBufferList in guest memory) in `format`.
    std::vector<double> decode_list(GuestMemory& m, const af::Pcm& format, std::uint64_t list, std::size_t frames, std::int32_t& status) {
        const auto count = m.read32(list);
        const auto wanted = format.non_interleaved ? format.channels : 1U;
        if (count < wanted) {
            status = error::param;
            return {};
        }
        const auto bytes = frames * format.frame_bytes();
        std::vector<std::vector<std::uint8_t>> storage(wanted);
        std::vector<std::span<const std::uint8_t>> spans;
        for (std::uint32_t b = 0; b < wanted; ++b) {
            const auto entry = list + 8 + 16ULL * b;
            const auto data = m.read64(entry + 8);
            const auto size = m.read32(entry + 4);
            if (size < bytes || (!data && bytes)) {
                status = error::invalid_input_size;
                return {};
            }
            storage[b].resize(bytes);
            if (bytes) m.read_bytes(data, storage[b]);
            spans.emplace_back(storage[b]);
        }
        return af::decode(format, spans, frames);
    }
    // Encodes interleaved output-channel samples into the buffers of `list`; returns the frames written.
    std::size_t encode_list(GuestMemory& m, const af::Pcm& format, std::uint64_t list, const std::vector<double>& samples, std::size_t frames) {
        const auto wanted = format.non_interleaved ? format.channels : 1U;
        const auto bytes = frames * format.frame_bytes();
        std::vector<std::vector<std::uint8_t>> storage(wanted, std::vector<std::uint8_t>(bytes));
        std::vector<std::span<std::uint8_t>> spans;
        for (auto& b : storage) spans.emplace_back(b);
        af::encode(format, samples, format.channels, frames, spans);
        for (std::uint32_t b = 0; b < wanted; ++b) {
            const auto entry = list + 8 + 16ULL * b;
            const auto data = m.read64(entry + 8);
            if (data && bytes) m.write_bytes(data, storage[b]);
            m.write32(entry + 4, static_cast<std::uint32_t>(bytes));
        }
        return frames;
    }
    // Pulls up to `frames` input frames through the input procedure into c.pending.
    void pull_input(Converter& c, std::uint64_t handle, std::uint64_t proc, std::uint64_t user_data, std::size_t frames);
    // The same for compressed input: packets, decoded.
    void pull_packets(Converter& c, std::uint64_t handle, std::uint64_t proc, std::uint64_t user_data, std::size_t frames);
    std::int32_t fill(Converter& c, std::uint64_t handle, std::uint64_t proc, std::uint64_t user_data, std::uint64_t io_packets, std::uint64_t list);
    std::int32_t get_property(Converter& c, std::uint32_t id, std::vector<std::uint8_t>& out, bool& writable);
    std::int32_t set_property(Converter& c, std::uint32_t id, std::span<const std::uint8_t> data);

    Support& support_;
    std::mutex mutex_;
    std::map<std::uint64_t, std::shared_ptr<Converter>> converters_;
};

void Converters::pull_packets(Converter& c, std::uint64_t handle, std::uint64_t proc, std::uint64_t user_data, std::size_t frames) {
    auto& m = support_.process.calling_engine();
    const auto size = 16 + 8 + 16ULL;
    if (c.scratch_size < size) {
        if (c.scratch) support_.process.release_guest(c.scratch);
        c.scratch = support_.process.allocate_guest(size, 16);
        c.scratch_size = size;
    }
    const std::size_t per_packet = c.codec_frames_per_packet ? c.codec_frames_per_packet : 1152;
    std::size_t got_total = 0;
    while (got_total < frames && !c.ended) {
        const auto packets = c.scratch, descriptions = c.scratch + 8, list = c.scratch + 16;
        const auto asked = static_cast<std::uint32_t>(std::max<std::size_t>(1, (frames - got_total + per_packet - 1) / per_packet));
        m.write32(packets, asked);
        m.write64(descriptions, 0);
        m.write32(list, 1);
        m.write32(list + 8, c.codec_channels);
        m.write32(list + 12, 0);
        m.write64(list + 16, 0);
        const auto status = static_cast<std::int32_t>(call(proc, {handle, packets, list, descriptions, user_data}));
        const auto got = std::min(m.read32(packets), asked);
        const auto data = m.read64(list + 16);
        const auto bytes = m.read32(list + 12);
        const auto table = m.read64(descriptions);
        std::vector<std::uint8_t> buffer(bytes);
        if (bytes && data) m.read_bytes(data, buffer);
        // The packets: by their descriptions, else the whole buffer for one, else MPEG frames.
        std::vector<std::span<const std::uint8_t>> pieces;
        if (got && table) {
            for (std::uint32_t i = 0; i < got; ++i) {
                const auto start = m.read64(table + 16ULL * i);
                const auto length = m.read32(table + 16ULL * i + 12);
                if (start + length > buffer.size()) break;
                pieces.emplace_back(buffer.data() + start, length);
            }
        } else if (got == 1) {
            pieces.emplace_back(buffer);
        } else if (got && (c.codec == four(".mp1") || c.codec == four(".mp2") || c.codec == four(".mp3"))) {
            for (std::size_t at = 0; at + 4 <= buffer.size() && pieces.size() < got;) {
                const auto frame = mc::mpeg_frame(buffer.data() + at);
                if (!frame.bytes || at + frame.bytes > buffer.size()) break;
                pieces.emplace_back(buffer.data() + at, frame.bytes);
                at += frame.bytes;
            }
        } else if (got) {
            c.ended = true;
            c.end_status = error::param;   // several packets of a variable format need descriptions
            break;
        }
        std::vector<float> decoded;
        std::vector<double> mapped(c.out.channels), frame(c.in.channels);
        for (const auto piece : pieces) {
            decoded.clear();
            std::string why;
            if (!c.decoder->decode(piece, decoded, why)) continue;   // a damaged packet is skipped
            const auto count = decoded.size() / c.in.channels;
            for (std::size_t f = 0; f < count; ++f) {
                for (std::uint32_t ch = 0; ch < c.in.channels; ++ch) frame[ch] = decoded[f * c.in.channels + ch];
                map_channels(c, frame.data(), mapped.data());
                c.pending.insert(c.pending.end(), mapped.begin(), mapped.end());
            }
            c.input_frames += count;
            got_total += count;
        }
        if (status || !got) {
            c.ended = true;
            c.end_status = status;
        }
    }
}

void Converters::pull_input(Converter& c, std::uint64_t handle, std::uint64_t proc, std::uint64_t user_data, std::size_t frames) {
    if (c.codec) return pull_packets(c, handle, proc, user_data, frames);
    auto& m = support_.process.calling_engine();
    const auto buffers = c.in.non_interleaved ? c.in.channels : 1U;
    // [UInt32 packets][pad][AudioStreamPacketDescription *][AudioBufferList]
    const auto size = 16 + 8 + 16ULL * buffers;
    if (c.scratch_size < size) {
        if (c.scratch) support_.process.release_guest(c.scratch);
        c.scratch = support_.process.allocate_guest(size, 16);
        c.scratch_size = size;
    }
    std::size_t got_total = 0;
    while (got_total < frames && !c.ended) {
        const auto packets = c.scratch, descriptions = c.scratch + 8, list = c.scratch + 16;
        m.write32(packets, static_cast<std::uint32_t>(frames - got_total));
        m.write64(descriptions, 0);
        m.write32(list, buffers);
        for (std::uint32_t b = 0; b < buffers; ++b) {
            const auto entry = list + 8 + 16ULL * b;
            m.write32(entry, c.in.non_interleaved ? 1 : c.in.channels);
            m.write32(entry + 4, 0);
            m.write64(entry + 8, 0);
        }
        const auto status = static_cast<std::int32_t>(call(proc, {handle, packets, list, descriptions, user_data}));
        const auto got = std::min<std::size_t>(m.read32(packets), frames - got_total);
        if (got) {
            std::int32_t decode_status = 0;
            const auto samples = decode_list(m, c.in, list, got, decode_status);
            if (decode_status) {
                c.ended = true;
                c.end_status = decode_status;
                break;
            }
            std::vector<double> mapped(c.out.channels);
            for (std::size_t frame = 0; frame < got; ++frame) {
                map_channels(c, samples.data() + frame * c.in.channels, mapped.data());
                c.pending.insert(c.pending.end(), mapped.begin(), mapped.end());
            }
            c.input_frames += got;
            got_total += got;
        }
        if (status || !got) {
            c.ended = true;
            c.end_status = status;
        }
    }
}

std::int32_t Converters::fill(Converter& c, std::uint64_t handle, std::uint64_t proc, std::uint64_t user_data, std::uint64_t io_packets,
                              std::uint64_t list) {
    auto& m = support_.process.calling_engine();
    const auto wanted = static_cast<std::size_t>(m.read32(io_packets));
    if (!wanted) return 0;
    // The output buffers' room limits the frames too.
    const auto buffers = c.out.non_interleaved ? c.out.channels : 1U;
    if (m.read32(list) < buffers) return error::param;
    auto room = wanted;
    for (std::uint32_t b = 0; b < buffers; ++b)
        room = std::min<std::size_t>(room, m.read32(list + 8 + 16ULL * b + 4) / c.out.frame_bytes());
    if (!room) return error::invalid_output_size;
    if (const auto result = ensure_decoder(c)) return result;
    if (!c.resampler) c.resampler = std::make_unique<Resampler>(c.in.rate, c.out.rate, c.out.channels);
    // Output frames the input received so far can make, once the input ended.
    const auto limit = [&] {
        if (!c.ended) return room;
        const auto possible = static_cast<std::uint64_t>(static_cast<double>(c.input_frames) / c.resampler->ratio());
        return possible > c.output_frames ? std::min<std::size_t>(room, possible - c.output_frames) : 0;
    };
    std::vector<double> samples(room * c.out.channels);
    std::size_t made = 0;
    while (made < room) {
        const auto step = std::min<std::size_t>(room - made, 4096);
        const auto needed = c.resampler->input_needed(step);
        if (c.pending.size() / c.out.channels < needed && !c.ended) pull_input(c, handle, proc, user_data, needed - c.pending.size() / c.out.channels);
        const auto now = std::min(step, limit());
        if (!now) break;
        c.resampler->render(samples.data() + made * c.out.channels, now, [&](double* out, std::size_t frames) {
            const auto values = frames * c.out.channels;
            const auto have = std::min(values, c.pending.size());
            std::copy_n(c.pending.begin(), have, out);
            std::fill(out + have, out + values, 0.0);
            c.pending.erase(c.pending.begin(), c.pending.begin() + static_cast<std::ptrdiff_t>(have));
        });
        made += now;
        c.output_frames += now;
        if (c.ended && limit() == 0) break;
    }
    samples.resize(made * c.out.channels);
    encode_list(m, c.out, list, samples, made);
    m.write32(io_packets, static_cast<std::uint32_t>(made));
    return made ? 0 : c.end_status;
}

std::int32_t Converters::get_property(Converter& c, std::uint32_t id, std::vector<std::uint8_t>& out, bool& writable) {
    const auto u32 = [&](std::uint32_t value) { out.resize(4); std::memcpy(out.data(), &value, 4); };
    writable = false;
    switch (id) {
    case four("acid"): out = c.in_asbd; return 0;
    case four("acod"): out = c.out_asbd; return 0;
    case four("dmgc"):   // kAudioConverterDecompressionMagicCookie
        if (!c.codec) return error::property_not_supported;
        out = c.cookie;
        writable = true;
        return 0;
    case four("xips"):
        if (c.codec) {
            // The largest packet: an MPEG frame (free format at 640 kb/s), six Opus frames, or a
            // FLAC block stored verbatim.
            const auto block = c.codec_frames_per_packet ? c.codec_frames_per_packet : 65535U;
            u32(c.codec == four("opus") ? 6 * 1275 + 7 : c.codec == four("flac") ? block * c.codec_channels * 4 + 64 : 2881);
            return 0;
        }
        u32(c.in.frame_bytes() * (c.in.non_interleaved ? c.in.channels : 1));
        return 0;
    case four("xops"): u32(c.out.frame_bytes() * (c.out.non_interleaved ? c.out.channels : 1)); return 0;
    case four("srcq"): u32(c.quality); writable = true; return 0;
    case four("srca"): u32(c.complexity); writable = true; return 0;
    case four("prmm"): u32(c.prime_method); writable = true; return 0;
    case four("prim"): out.assign(8, 0); writable = true; return 0;   // no leading or trailing frames
    case four("chmp"): {
        out.resize(4ULL * c.out.channels);
        for (std::uint32_t o = 0; o < c.out.channels; ++o) {
            const std::int32_t value = o < c.channel_map.size() ? c.channel_map[o]
                : static_cast<std::int32_t>(c.in.channels == 1 ? 0 : (o < c.in.channels ? o : -1));
            std::memcpy(out.data() + 4ULL * o, &value, 4);
        }
        writable = true;
        return 0;
    }
    default: return error::property_not_supported;
    }
}

std::int32_t Converters::set_property(Converter& c, std::uint32_t id, std::span<const std::uint8_t> data) {
    std::uint32_t value{};
    if (data.size() >= 4) std::memcpy(&value, data.data(), 4);
    switch (id) {
    case four("srcq"): if (data.size() < 4) return error::bad_property_size; c.quality = value; return 0;
    case four("srca"): if (data.size() < 4) return error::bad_property_size; c.complexity = value; return 0;
    case four("prmm"): if (data.size() < 4) return error::bad_property_size; c.prime_method = value; return 0;
    case four("prim"): return 0;
    case four("dmgc"):
        if (!c.codec) return error::property_not_supported;
        c.cookie.assign(data.begin(), data.end());
        c.decoder.reset();   // made again, from the cookie, at the next conversion
        c.resampler.reset();
        c.pending.clear();
        return 0;
    case four("chmp"):
        if (data.size() % 4) return error::bad_property_size;
        c.channel_map.resize(data.size() / 4);
        for (std::size_t i = 0; i < c.channel_map.size(); ++i) std::memcpy(&c.channel_map[i], data.data() + 4 * i, 4);
        return 0;
    case four("acid"): case four("acod"): {
        if (c.codec && id == four("acid")) return error::property_not_supported;   // a decoder keeps its input format
        std::string why;
        const auto format = af::pcm_from_asbd(data, why);
        if (!format) return error::format_not_supported;
        (id == four("acid") ? c.in : c.out) = *format;
        (id == four("acid") ? c.in_asbd : c.out_asbd).assign(data.begin(), data.begin() + 40);
        c.resampler.reset();
        c.pending.clear();
        return 0;
    }
    default: return error::property_not_supported;
    }
}

void Converters::install() {
    auto self = shared_from_this();
    const auto status = [](CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); };
    const auto create = [self](GuestMemory& m, std::uint64_t in_asbd, std::uint64_t out_asbd, std::uint64_t out) {
        if (out) m.write64(out, 0);
        if (!in_asbd || !out_asbd || !out) return error::param;
        std::vector<std::uint8_t> in(40), outf(40);
        m.read_bytes(in_asbd, in);
        m.read_bytes(out_asbd, outf);
        std::string why;
        std::uint32_t in_id{};
        std::memcpy(&in_id, in.data() + 8, 4);
        auto made = std::make_shared<Converter>();
        const auto to = af::pcm_from_asbd(outf, why);
        if (!to) return error::format_not_supported;
        if (in_id != four("lpcm")) {
            // Compressed input: decoded to float, then converted like PCM.
            if (!decodable(in_id)) return error::format_not_supported;
            double rate{};
            std::memcpy(&rate, in.data(), 8);
            std::memcpy(&made->codec_flags, in.data() + 12, 4);
            std::memcpy(&made->codec_frames_per_packet, in.data() + 20, 4);
            std::memcpy(&made->codec_channels, in.data() + 28, 4);
            if (!(rate > 0) || made->codec_channels == 0 || made->codec_channels > 8) return error::format_not_supported;
            made->codec = in_id;
            made->codec_rate = rate;
            made->in.rate = in_id == four("opus") ? 48000 : rate;
            made->in.channels = made->codec_channels;
            made->in.is_float = true;
            made->in.bits = 32;
            made->in.container = 4;
        } else {
            const auto from = af::pcm_from_asbd(in, why);
            if (!from) return error::format_not_supported;
            made->in = *from;
        }
        made->out = *to;
        made->in_asbd = in;
        made->out_asbd = outf;
        const auto handle = self->support_.process.allocate_guest(16, 16);
        m.fill_bytes(handle, 0, 16);
        {
            std::lock_guard lock(self->mutex_);
            self->converters_[handle] = std::move(made);
        }
        m.write64(out, handle);
        return 0;
    };
    support_.function(audio_toolbox, "_AudioConverterNew", [create, status](CpuState& s, GuestMemory& m) {
        status(s, create(m, s.x[0], s.x[1], s.x[2]));
    });
    // AudioConverterNewSpecific(in, out, numberClassDescriptions, classDescriptions, outConverter)
    support_.function(audio_toolbox, "_AudioConverterNewSpecific", [create, status](CpuState& s, GuestMemory& m) {
        status(s, create(m, s.x[0], s.x[1], s.x[4]));
    });
    support_.function(audio_toolbox, "_AudioConverterDispose", [self, status](CpuState& s, GuestMemory&) {
        std::shared_ptr<Converter> gone;
        {
            std::lock_guard lock(self->mutex_);
            const auto found = self->converters_.find(s.x[0]);
            if (found == self->converters_.end()) return status(s, error::param);
            gone = std::move(found->second);
            self->converters_.erase(found);
        }
        if (gone->scratch) self->support_.process.release_guest(gone->scratch);
        self->support_.process.release_guest(s.x[0]);
        status(s, 0);
    });
    support_.function(audio_toolbox, "_AudioConverterReset", [self, status](CpuState& s, GuestMemory&) {
        const auto c = self->find(s.x[0]);
        if (!c) return status(s, error::param);
        std::lock_guard lock(c->mutex);
        c->resampler.reset();
        c->pending.clear();
        if (c->decoder) c->decoder->reset();
        c->input_frames = c->output_frames = 0;
        c->ended = false;
        c->end_status = 0;
        status(s, 0);
    });
    // AudioConverterGetPropertyInfo(converter, id, UInt32 *outSize, Boolean *outWritable)
    support_.function(audio_toolbox, "_AudioConverterGetPropertyInfo", [self, status](CpuState& s, GuestMemory& m) {
        const auto c = self->find(s.x[0]);
        if (!c) return status(s, error::param);
        std::lock_guard lock(c->mutex);
        std::vector<std::uint8_t> value;
        bool writable{};
        if (const auto result = self->get_property(*c, static_cast<std::uint32_t>(s.x[1]), value, writable)) return status(s, result);
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(value.size()));
        if (s.x[3]) m.write8(s.x[3], writable ? 1 : 0);
        status(s, 0);
    });
    // AudioConverterGetProperty(converter, id, UInt32 *ioSize, void *outData)
    support_.function(audio_toolbox, "_AudioConverterGetProperty", [self, status](CpuState& s, GuestMemory& m) {
        const auto c = self->find(s.x[0]);
        if (!c) return status(s, error::param);
        if (!s.x[2] || !s.x[3]) return status(s, error::param);
        std::lock_guard lock(c->mutex);
        std::vector<std::uint8_t> value;
        bool writable{};
        if (const auto result = self->get_property(*c, static_cast<std::uint32_t>(s.x[1]), value, writable)) return status(s, result);
        const auto room = m.read32(s.x[2]);
        if (room < value.size() && static_cast<std::uint32_t>(s.x[1]) != four("chmp")) return status(s, error::bad_property_size);
        const auto size = std::min<std::size_t>(room, value.size());
        if (size) m.write_bytes(s.x[3], std::span(value.data(), size));
        m.write32(s.x[2], static_cast<std::uint32_t>(size));
        status(s, 0);
    });
    // AudioConverterSetProperty(converter, id, UInt32 size, const void *data)
    support_.function(audio_toolbox, "_AudioConverterSetProperty", [self, status](CpuState& s, GuestMemory& m) {
        const auto c = self->find(s.x[0]);
        if (!c) return status(s, error::param);
        std::vector<std::uint8_t> data(static_cast<std::uint32_t>(s.x[2]));
        if (!data.empty()) {
            if (!s.x[3]) return status(s, error::param);
            m.read_bytes(s.x[3], data);
        }
        std::lock_guard lock(c->mutex);
        status(s, self->set_property(*c, static_cast<std::uint32_t>(s.x[1]), data));
    });
    // AudioConverterFillComplexBuffer(converter, inputProc, userData, UInt32 *ioOutputDataPacketSize,
    // AudioBufferList *outOutputData, AudioStreamPacketDescription *outPacketDescription)
    support_.function(audio_toolbox, "_AudioConverterFillComplexBuffer", [self, status](CpuState& s, GuestMemory&) {
        const auto c = self->find(s.x[0]);
        if (!c) return status(s, error::param);
        if (!s.x[1] || !s.x[3] || !s.x[4]) return status(s, error::param);
        std::lock_guard lock(c->mutex);
        status(s, self->fill(*c, s.x[0], s.x[1], s.x[2], s.x[3], s.x[4]));
    });
    // AudioConverterConvertComplexBuffer(converter, frames, const AudioBufferList *in, AudioBufferList *out)
    support_.function(audio_toolbox, "_AudioConverterConvertComplexBuffer", [self, status](CpuState& s, GuestMemory& m) {
        const auto c = self->find(s.x[0]);
        if (!c) return status(s, error::param);
        if (!s.x[2] || !s.x[3]) return status(s, error::param);
        std::lock_guard lock(c->mutex);
        // Packets need AudioConverterFillComplexBuffer, as rate changes do.
        if (c->codec || c->in.rate != c->out.rate) return status(s, error::invalid_input_size);
        const auto frames = static_cast<std::uint32_t>(s.x[1]);
        std::int32_t result = 0;
        const auto samples = self->decode_list(m, c->in, s.x[2], frames, result);
        if (result) return status(s, result);
        std::vector<double> mapped(static_cast<std::size_t>(frames) * c->out.channels);
        for (std::size_t f = 0; f < frames; ++f) map_channels(*c, samples.data() + f * c->in.channels, mapped.data() + f * c->out.channels);
        self->encode_list(m, c->out, s.x[3], mapped, frames);
        status(s, 0);
    });
    // AudioConverterConvertBuffer(converter, inSize, const void *in, UInt32 *ioOutSize, void *out): interleaved.
    support_.function(audio_toolbox, "_AudioConverterConvertBuffer", [self, status](CpuState& s, GuestMemory& m) {
        const auto c = self->find(s.x[0]);
        if (!c) return status(s, error::param);
        if (!s.x[2] || !s.x[3] || !s.x[4]) return status(s, error::param);
        std::lock_guard lock(c->mutex);
        if (c->codec || c->in.rate != c->out.rate || c->in.non_interleaved || c->out.non_interleaved) return status(s, error::invalid_input_size);
        const auto in_bytes = static_cast<std::uint32_t>(s.x[1]);
        if (in_bytes % c->in.frame_bytes()) return status(s, error::invalid_input_size);
        const auto frames = in_bytes / c->in.frame_bytes();
        if (m.read32(s.x[3]) < frames * c->out.frame_bytes()) return status(s, error::invalid_output_size);
        std::vector<std::uint8_t> in(in_bytes);
        if (in_bytes) m.read_bytes(s.x[2], in);
        const auto samples = af::decode(c->in, {std::span<const std::uint8_t>(in)}, frames);
        std::vector<double> mapped(static_cast<std::size_t>(frames) * c->out.channels);
        for (std::size_t f = 0; f < frames; ++f) map_channels(*c, samples.data() + f * c->in.channels, mapped.data() + f * c->out.channels);
        std::vector<std::uint8_t> out(static_cast<std::size_t>(frames) * c->out.frame_bytes());
        af::encode(c->out, mapped, c->out.channels, frames, {std::span<std::uint8_t>(out)});
        if (!out.empty()) m.write_bytes(s.x[4], out);
        m.write32(s.x[3], static_cast<std::uint32_t>(out.size()));
        status(s, 0);
    });
}

} // namespace

void install_audio_converter(Support& support) { std::make_shared<Converters>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media
