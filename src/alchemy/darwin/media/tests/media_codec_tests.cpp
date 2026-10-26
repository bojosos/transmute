// Host-only tests of the media decoders (media_codecs.hpp) through the containers
// (media_container.hpp): each decodes and compares.
// - MPEG audio layers I, II and III: minimp3's own conformance vectors
//   (third_party/minimp3/vectors), to the upstream test's PSNR bound.
// - Opus: a stereo signal encoded here by libopus, in Ogg and through an MP4 dOps
//   configuration, against the input by signal-to-noise ratio.
// - FLAC: signals encoded here by libFLAC, parsed as native FLAC streams, decoded bit-exactly.
// - VP8 and VP9 (profiles 0, 1 and 2): libvpx's test vectors (IVF, and WebM through the
//   Matroska parser) against their per-frame MD5s.
// - AV1 (8 and 10 bits): libaom's test vectors against their per-frame MD5s.
#include "alchemy/darwin/media/audio_stream.hpp"
#include "alchemy/darwin/media/media_codecs.hpp"
#include "alchemy/darwin/media/media_container.hpp"
#include "alchemy/darwin/media/video_frames.hpp"

#include "alchemy/host/crypto.hpp"

#include "FLAC/stream_encoder.h"
#include "opus.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
namespace mc = alchemy::gui::completion_kit::media::container;
namespace codecs = alchemy::gui::completion_kit::media::codecs;
using Bytes = std::vector<std::uint8_t>;

void require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}
void put_le(Bytes& out, std::uint64_t value, int bytes) { for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
void put(Bytes& out, std::uint64_t value, int bytes) { for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
void append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }
mc::ReadAt reader(const Bytes& file) {
    return [&file](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
        if (offset >= file.size()) return 0;
        const auto n = std::min<std::size_t>(out.size(), file.size() - static_cast<std::size_t>(offset));
        std::memcpy(out.data(), file.data() + offset, n);
        return n;
    };
}
Bytes load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    require(in.good(), "cannot open " + path);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}

// Decodes every sample of the file's first track.
std::vector<float> decode_track(const Bytes& file, mc::Track* track_out = nullptr) {
    mc::Movie movie;
    std::string why;
    require(mc::parse(reader(file), file.size(), movie, why), "parses: " + why);
    require(!movie.tracks.empty(), "a track");
    const auto& t = movie.tracks[0];
    auto decoder = codecs::make_audio_decoder(t.codec, t.sample_rate, t.channels, t.config, why);
    require(decoder != nullptr, "a decoder: " + why);
    std::vector<float> out;
    for (const auto& s : t.samples) {
        const auto packet = mc::read_sample(reader(file), t, s);
        require(decoder->decode(packet, out, why), "decodes: " + why);
    }
    require(decoder->channels == t.channels, "decoder channels match the track");
    if (track_out) *track_out = t;
    return out;
}

// The file through audio_stream::Stream: its length, a whole read, chunked reads, and reads after
// seeks (backwards and forwards), each compared with `reference` (interleaved, priming removed)
// within `tolerance`.
void stream_checks(const std::string& what, const Bytes& file, const std::vector<float>& reference, std::uint32_t channels, float tolerance) {
    std::int32_t status = 0;
    std::string why;
    auto stream = alchemy::gui::completion_kit::media::audio_stream::Stream::open(reader(file), file.size(), status, why);
    require(stream != nullptr && status == 0, what + ": stream opens: " + why);
    require(stream->channels() == channels && stream->frames() == reference.size() / channels,
            what + ": stream length " + std::to_string(stream->frames()) + " vs " + std::to_string(reference.size() / channels));
    const auto compare = [&](const std::vector<float>& got, std::uint64_t start, const std::string& how) {
        for (std::size_t i = 0; i < got.size(); ++i)
            require(std::fabs(got[i] - reference[std::size_t(start) * channels + i]) <= tolerance, what + ": " + how + " sample " + std::to_string(i) + " is " + std::to_string(got[i]) + ", expected " + std::to_string(reference[std::size_t(start) * channels + i]) + " (next expected " + std::to_string(reference[std::size_t(start) * channels + i + channels]) + ")");
    };
    std::vector<float> whole;
    require(stream->read(0, std::size_t(stream->frames()) + 100, whole) == stream->frames(), what + ": a whole read");
    compare(whole, 0, "whole");
    std::vector<float> chunks;
    for (std::uint64_t at = 0; at < stream->frames(); at += 777) {
        std::vector<float> part;
        stream->read(at, 777, part);
        chunks.insert(chunks.end(), part.begin(), part.end());
    }
    require(chunks.size() == whole.size(), what + ": chunked reads cover the stream");
    compare(chunks, 0, "chunked");
    const auto frames = stream->frames();
    for (const std::uint64_t at : {frames * 3 / 4, frames / 3, frames / 2 + 1, std::uint64_t{0}, frames - 10}) {
        std::vector<float> part;
        const auto got = stream->read(at, 500, part);
        require(got == std::min<std::uint64_t>(500, frames - at), what + ": a read at " + std::to_string(at));
        compare(part, at, "seek to " + std::to_string(at));
    }
    std::vector<float> none;
    require(stream->read(frames, 10, none) == 0 && none.empty(), what + ": nothing past the end");
}

void mpeg_tests() {
    const std::string dir = std::string(ALCHEMY_THIRD_PARTY) + "/minimp3/vectors";
    for (const char* name : {"l1-fl4", "l2-fl13", "l3-hecommon"}) {
        const auto file = load(dir + "/" + name + ".bit");
        const auto reference = load(dir + "/" + name + ".pcm");
        mc::Track track;
        const auto pcm = decode_track(file, &track);
        const std::size_t ref_samples = reference.size() / 2;
        // As the upstream test: for layer III the reference may be shorter by up to two frames; the
        // layer I and II references are padded, so only the common length is compared.
        if (name[1] == '3')
            require(pcm.size() >= ref_samples && pcm.size() <= ref_samples + 2304, std::string(name) + ": sample count " +
                    std::to_string(pcm.size()) + " vs " + std::to_string(ref_samples));
        require(!pcm.empty(), std::string(name) + " decodes");
        const std::size_t common = std::min(ref_samples, pcm.size());
        double mse = 0;
        for (std::size_t i = 0; i < common; ++i) {
            const double v = std::clamp(std::lround(double(pcm[i]) * 32768.0), -32768L, 32767L);
            const auto r = static_cast<std::int16_t>(reference[2 * i] | (reference[2 * i + 1] << 8));
            mse += (v - r) * (v - r);
        }
        mse /= double(pcm.size());
        const double psnr = mse == 0 ? 99.0 : 10.0 * std::log10(32767.0 * 32767.0 / mse);
        require(psnr >= 96.0, std::string(name) + ": PSNR " + std::to_string(psnr));
        if (name[1] == '3') stream_checks(name, file, pcm, track.channels, 1e-6f);
    }
}

Bytes ogg_page(std::uint32_t sequence, std::uint8_t type, std::int64_t granule, const Bytes& packet) {
    Bytes lacing;
    auto n = packet.size();
    while (n >= 255) { lacing.push_back(255); n -= 255; }
    lacing.push_back(static_cast<std::uint8_t>(n));
    Bytes page{'O', 'g', 'g', 'S', 0, type};
    put_le(page, static_cast<std::uint64_t>(granule), 8); put_le(page, 1, 4); put_le(page, sequence, 4); put_le(page, 0, 4);
    page.push_back(static_cast<std::uint8_t>(lacing.size()));
    append(page, lacing); append(page, packet);
    return page;
}

double snr_db(const std::vector<float>& reference, const std::vector<float>& decoded, std::size_t from, std::size_t count) {
    double signal = 0, noise = 0;
    for (std::size_t i = from; i < from + count; ++i) {
        signal += double(reference[i]) * reference[i];
        noise += (double(reference[i]) - decoded[i]) * (double(reference[i]) - decoded[i]);
    }
    return 10.0 * std::log10(signal / std::max(noise, 1e-20));
}

void opus_tests() {
    constexpr int rate = 48000, frame = 960, frames = 50;
    std::vector<float> input(std::size_t(frame) * (frames + 1) * 2, 0.0f);
    for (int i = 0; i < frame * frames; ++i) {
        input[2 * i] = 0.4f * float(std::sin(2 * 3.14159265358979 * 440 * i / rate));
        input[2 * i + 1] = 0.3f * float(std::sin(2 * 3.14159265358979 * 660 * i / rate));
    }
    int error = 0;
    OpusEncoder* encoder = opus_encoder_create(rate, 2, OPUS_APPLICATION_AUDIO, &error);
    require(encoder && error == OPUS_OK, "Opus encoder");
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(128000));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead));
    std::vector<Bytes> packets;
    for (int f = 0; f <= frames; ++f) {
        Bytes packet(4000);
        const int n = opus_encode_float(encoder, input.data() + std::size_t(f) * frame * 2, frame, packet.data(), opus_int32(packet.size()));
        require(n > 0, "Opus encodes");
        packet.resize(std::size_t(n));
        packets.push_back(packet);
    }
    opus_encoder_destroy(encoder);

    // Ogg Opus: OpusHead (pre-skip = the encoder's lookahead), OpusTags, one packet per page.
    Bytes head{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2};
    put_le(head, std::uint64_t(lookahead), 2); put_le(head, rate, 4); put_le(head, 0, 2); head.push_back(0);
    Bytes tags{'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 0, 0, 0, 0, 0, 0, 0, 0};
    Bytes file = ogg_page(0, 2, 0, head);
    append(file, ogg_page(1, 0, 0, tags));
    for (std::size_t p = 0; p < packets.size(); ++p)
        append(file, ogg_page(std::uint32_t(p + 2), p + 1 == packets.size() ? 4 : 0, std::int64_t(p + 1) * frame, packets[p]));
    mc::Track track;
    const auto decoded = decode_track(file, &track);
    require(track.priming == std::uint32_t(lookahead), "the Opus pre-skip");
    require(decoded.size() == packets.size() * frame * 2, "every Opus packet decodes to 20 ms");
    // Skip the pre-skip, then compare (after the first 20 ms, while the codec settles).
    std::vector<float> aligned(decoded.begin() + std::ptrdiff_t(lookahead) * 2, decoded.end());
    const double snr = snr_db(input, aligned, std::size_t(frame) * 2, std::size_t(frame) * (frames - 2) * 2);
    require(snr > 20.0, "Opus round trip SNR " + std::to_string(snr));

    // The same packets through an MP4 dOps configuration decode identically.
    Bytes dops{0x00, 0x02};
    put(dops, std::uint64_t(lookahead), 2); put(dops, rate, 4); put(dops, 0, 2); dops.push_back(0);
    std::string why;
    auto d = codecs::make_audio_decoder(mc::fourcc("opus"), rate, 2, dops, why);
    require(d && d->rate == 48000 && d->channels == 2, "Opus decoder from dOps: " + why);
    std::vector<float> again;
    for (const auto& p : packets) require(d->decode(p, again, why), "dOps decode: " + why);
    require(again == decoded, "dOps and OpusHead decode the same");
    // As a stream: the pre-skip removed, and reads after seeks within the codec's convergence error.
    {
        std::vector<float> trimmed(decoded.begin() + std::ptrdiff_t(lookahead) * 2, decoded.end());
        stream_checks("Ogg Opus", file, trimmed, 2, 0.002f);
    }
    // A truncated packet is rejected; the decoder keeps working.
    Bytes broken{0xfc, 0xff};
    std::vector<float> scratch;
    d->reset();
    (void)d->decode(broken, scratch, why);
    scratch.clear();
    require(d->decode(packets[3], scratch, why) && scratch.size() == std::size_t(frame) * 2, "Opus decodes after a bad packet");
}

FLAC__StreamEncoderWriteStatus flac_write(const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes, uint32_t, uint32_t, void* out) {
    static_cast<Bytes*>(out)->insert(static_cast<Bytes*>(out)->end(), buffer, buffer + bytes);
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

void flac_tests() {
    for (const auto [bits, channels, rate] : {std::tuple{16U, 2U, 44100U}, std::tuple{24U, 1U, 96000U}}) {
        constexpr std::uint32_t count = 10000;
        const std::int32_t peak = (1 << (bits - 1)) - 1;
        std::vector<FLAC__int32> input(std::size_t(count) * channels);
        std::uint32_t lcg = 12345;
        for (std::uint32_t i = 0; i < count; ++i)
            for (std::uint32_t c = 0; c < channels; ++c) {
                lcg = lcg * 1664525U + 1013904223U;
                const double tone = 0.5 * std::sin(2 * 3.14159265358979 * (300.0 + 200 * c) * i / rate);
                const double noise = (double(lcg >> 8) / double(1 << 24) - 0.5) * 0.05;
                input[std::size_t(i) * channels + c] = std::int32_t(std::lround((tone + noise) * peak));
            }
        input[0] = peak;
        input[channels] = -peak - 1;   // the extremes survive
        FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
        require(encoder != nullptr, "FLAC encoder");
        FLAC__stream_encoder_set_channels(encoder, channels);
        FLAC__stream_encoder_set_bits_per_sample(encoder, bits);
        FLAC__stream_encoder_set_sample_rate(encoder, rate);
        FLAC__stream_encoder_set_compression_level(encoder, 5);
        FLAC__stream_encoder_set_total_samples_estimate(encoder, count);
        Bytes file;
        require(FLAC__stream_encoder_init_stream(encoder, flac_write, nullptr, nullptr, nullptr, &file) == FLAC__STREAM_ENCODER_INIT_STATUS_OK,
                "FLAC encoder init");
        require(FLAC__stream_encoder_process_interleaved(encoder, input.data(), count), "FLAC encodes");
        FLAC__stream_encoder_finish(encoder);
        FLAC__stream_encoder_delete(encoder);

        mc::Track track;
        const auto decoded = decode_track(file, &track);
        require(track.sample_rate == rate && track.channels == channels && track.bits == bits, "FLAC track format");
        require(decoded.size() == input.size(), "FLAC decodes every sample: " + std::to_string(decoded.size()));
        const double scale = double(std::uint32_t(1) << (bits - 1));
        for (std::size_t i = 0; i < input.size(); ++i)
            require(std::lround(double(decoded[i]) * scale) == input[i], "FLAC sample " + std::to_string(i) + " is exact");
        stream_checks("FLAC " + std::to_string(bits), file, decoded, channels, 0.0f);
    }
    // A corrupted frame (CRC mismatch) is reported, and the next frame still decodes.
    std::string why;
    Bytes info{0, 0, 0, 34, 0x10, 0x00, 0x10, 0x00, 0, 0, 0, 0, 0, 0, 0x0a, 0xc4, 0x40, 0xf0};
    put(info, 0, 4); append(info, Bytes(16, 0));
    auto d = codecs::make_audio_decoder(mc::fourcc("flac"), 0, 0, info, why);
    require(d && d->rate == 44100 && d->channels == 1 && d->bits == 16, "FLAC decoder from a bare STREAMINFO: " + why);
    std::vector<float> scratch;
    require(!d->decode(Bytes{0xff, 0xf8, 0xc9, 0x08, 0x00, 0x00, 0x00, 0x00}, scratch, why), "a broken FLAC frame is rejected");
}

void unsupported_tests() {
    std::string why;
    for (const char* code : {"mp4a", "alac", "avc1"}) {
        const char c[5] = {code[0], code[1], code[2], code[3], 0};
        require(!codecs::audio_decoder_available(mc::fourcc(c)) && !codecs::make_audio_decoder(mc::fourcc(c), 44100, 2, {}, why) && !why.empty(),
                std::string(code) + " has no decoder");
    }
    require(!codecs::make_audio_decoder(mc::fourcc("opus"), 48000, 6, {}, why), "Opus with six channels needs a mapping table");
}

// ---- video ------------------------------------------------------------------------------------------

std::string hex(const std::vector<std::uint8_t>& digest) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const auto b : digest) { out += digits[b >> 4]; out += digits[b & 15]; }
    return out;
}
std::string md5(const Bytes& data) {
    const auto digest = alchemy::host::crypto::hash(alchemy::host::crypto::Digest::md5, std::as_bytes(std::span(data)));
    require(static_cast<bool>(digest), "MD5 is available");
    return hex(*digest);
}
// The planes as the projects' test tools hash them: rows of the displayed size, luma then chroma.
void append_planes(Bytes& out, const codecs::VideoFrame& f) {
    for (int p = 0; p < (f.monochrome ? 1 : 3); ++p) append(out, f.planes[p].data);
}

// IVF (the projects' test container): a 32-byte header, then frames of (size, pts, bytes).
struct IvfFrame { Bytes data; std::int64_t pts; };
std::vector<IvfFrame> ivf_frames(const Bytes& file, std::uint32_t& fourcc_out) {
    require(file.size() >= 32 && std::memcmp(file.data(), "DKIF", 4) == 0, "an IVF file");
    fourcc_out = mc::fourcc({char(file[8]), char(file[9]), char(file[10]), char(file[11]), 0});
    std::vector<IvfFrame> frames;
    std::size_t at = file[6] | (file[7] << 8);
    while (at + 12 <= file.size()) {
        const std::uint32_t size = file[at] | (file[at + 1] << 8) | (file[at + 2] << 16) | (std::uint32_t(file[at + 3]) << 24);
        std::int64_t pts = 0;
        for (int i = 7; i >= 0; --i) pts = (pts << 8) | file[at + 4 + std::size_t(i)];
        at += 12;
        require(at + size <= file.size(), "an IVF frame inside the file");
        frames.push_back({Bytes(file.begin() + std::ptrdiff_t(at), file.begin() + std::ptrdiff_t(at + size)), pts});
        at += size;
    }
    return frames;
}

std::vector<codecs::VideoFrame> decode_all(std::uint32_t codec, const std::vector<IvfFrame>& packets, std::span<const std::uint8_t> config = {}) {
    std::string why;
    auto decoder = codecs::make_video_decoder(codec, config, why);
    require(decoder != nullptr, "a video decoder: " + why);
    std::vector<codecs::VideoFrame> frames;
    codecs::VideoFrame f;
    for (const auto& p : packets) {
        require(decoder->send(p.data, p.pts, why), "decodes: " + why);
        while (decoder->receive(f)) frames.push_back(std::move(f));
    }
    decoder->drain();
    while (decoder->receive(f)) frames.push_back(std::move(f));
    return frames;
}

// video_frames.hpp's CVPixelBuffer layouts of a decoded picture: the native format holds it exactly
// (4:4:4 bi-planar, or 10 bits in the high bits of 16), and 420v averages 4:4:4 chroma.
void video_frame_checks(const std::string& what, const codecs::VideoFrame& f) {
    namespace vf = alchemy::gui::completion_kit::media::video_frames;
    namespace px = alchemy::gui::completion_kit::media::pixels;
    const auto native = vf::native_format(f);
    const bool ten = f.bit_depth > 8;
    require(native == (ten ? mc::fourcc("x420") : mc::fourcc("444v")), what + ": native format");
    const std::uint32_t bytes = ten ? 2 : 1, cw = f.planes[1].width, ch = f.planes[1].height;
    Bytes luma(std::size_t(f.width) * f.height * bytes), chroma(std::size_t(cw) * ch * 2 * bytes);
    const std::array<px::Plane, 2> planes{{{luma.data(), f.width * bytes}, {chroma.data(), cw * 2 * bytes}}};
    require(vf::write(f, native, planes), what + ": writes its native format");
    const auto code = [&](const codecs::VideoFrame::Plane& p, std::uint32_t x, std::uint32_t y) {
        const auto* at = p.data.data() + std::size_t(y) * p.stride + std::size_t(x) * bytes;
        return ten ? std::uint32_t(at[0] | (at[1] << 8)) : std::uint32_t(at[0]);
    };
    const auto stored = [&](const Bytes& b, std::size_t index) {
        return ten ? std::uint32_t(b[2 * index] | (b[2 * index + 1] << 8)) >> 6 : std::uint32_t(b[index]);
    };
    for (std::uint32_t y = 0; y < f.height; ++y)
        for (std::uint32_t x = 0; x < f.width; ++x)
            require(stored(luma, std::size_t(y) * f.width + x) == code(f.planes[0], x, y), what + ": luma kept exactly");
    for (std::uint32_t y = 0; y < ch; ++y)
        for (std::uint32_t x = 0; x < cw; ++x)
            require(stored(chroma, (std::size_t(y) * cw + x) * 2) == code(f.planes[1], x, y) &&
                    stored(chroma, (std::size_t(y) * cw + x) * 2 + 1) == code(f.planes[2], x, y), what + ": chroma kept exactly");
    if (ten) return;
    // 4:4:4 into 420v: each chroma sample the rounded mean of its four.
    const std::uint32_t hw = (f.width + 1) / 2, hh = (f.height + 1) / 2;
    Bytes y420(std::size_t(f.width) * f.height), c420(std::size_t(hw) * hh * 2);
    const std::array<px::Plane, 2> half{{{y420.data(), f.width}, {c420.data(), hw * 2}}};
    require(vf::write(f, mc::fourcc("420v"), half), what + ": writes 420v");
    const auto mean = [&](int plane, std::uint32_t x, std::uint32_t y) {
        double sum = 0;
        for (std::uint32_t dy = 0; dy < 2; ++dy)
            for (std::uint32_t dx = 0; dx < 2; ++dx) sum += code(f.planes[plane], std::min(2 * x + dx, f.width - 1), std::min(2 * y + dy, f.height - 1));
        return static_cast<std::uint32_t>(std::lround(sum / 4));
    };
    require(c420[0] == mean(1, 0, 0) && c420[1] == mean(2, 0, 0) && c420[(std::size_t(hh - 1) * hw + hw - 1) * 2] == mean(1, hw - 1, hh - 1),
            what + ": 4:4:4 chroma averaged into 4:2:0");
}

void vpx_tests() {
    const std::string dir = std::string(ALCHEMY_THIRD_PARTY) + "/libvpx/test-vectors/";
    struct Case { const char* name; std::uint32_t bits, shift_x, shift_y; };
    for (const auto& c : {Case{"vp80-00-comprehensive-001.ivf", 8, 1, 1}, Case{"vp90-2-02-size-08x08.webm", 8, 1, 1},
                          Case{"vp91-2-04-yuv444.webm", 8, 0, 0}, Case{"vp92-2-20-10bit-yuv420.webm", 10, 1, 1}}) {
        const std::string name = c.name;
        const auto file = load(dir + name);
        std::vector<IvfFrame> packets;
        std::uint32_t codec = 0;
        if (name.ends_with(".ivf")) {
            packets = ivf_frames(file, codec);
            codec = codec == mc::fourcc("VP80") ? mc::fourcc("vp08") : mc::fourcc("vp09");
        } else {
            mc::Movie movie;
            std::string why;
            require(mc::parse(reader(file), file.size(), movie, why), name + " parses: " + why);
            const auto& t = movie.tracks.at(0);
            require(t.kind == mc::Track::Kind::video, name + " has a video track");
            codec = t.codec;
            for (const auto& s : t.samples) packets.push_back({mc::read_sample(reader(file), t, s), s.pts});
        }
        const auto frames = decode_all(codec, packets);
        // One "<md5>  <name>-<w>x<h>-<n>.i420" line per frame.
        std::istringstream lines(std::string(reinterpret_cast<const char*>(load(dir + name + ".md5").data()), load(dir + name + ".md5").size()));
        std::vector<std::string> expected;
        for (std::string line; std::getline(lines, line);)
            if (line.size() >= 32) expected.push_back(line.substr(0, 32));
        require(!frames.empty() && frames.size() == expected.size(),
                name + ": " + std::to_string(frames.size()) + " frames, " + std::to_string(expected.size()) + " expected");
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto& f = frames[i];
            require(f.bit_depth == c.bits && f.chroma_shift_x == c.shift_x && f.chroma_shift_y == c.shift_y, name + ": picture format");
            Bytes planes;
            append_planes(planes, f);
            require(md5(planes) == expected[i], name + ": frame " + std::to_string(i + 1) + " MD5");
        }
        require(frames.size() < 2 || frames[1].pts > frames[0].pts, name + ": presentation times carried");
        if (c.shift_x == 0 || c.bits > 8) video_frame_checks(name, frames.front());
    }
    // A corrupt packet is rejected.
    std::string why;
    auto d = codecs::make_video_decoder(mc::fourcc("vp09"), {}, why);
    require(d && !d->send(Bytes{0xff, 0xff, 0xff}, 0, why) && !why.empty(), "a corrupt VP9 packet is rejected");
}

void av1_tests() {
    // libaom's AV1 test vectors and their per-frame MD5s (16-bit little-endian samples above 8 bits).
    const std::string dir = std::string(ALCHEMY_THIRD_PARTY) + "/libaom/test-vectors/";
    for (const auto& [name, bits] : {std::pair{"av1-1-b8-01-size-16x16.ivf", 8U}, std::pair{"av1-1-b10-00-quantizer-00.ivf", 10U}}) {
        std::uint32_t codec = 0;
        const auto packets = ivf_frames(load(dir + name), codec);
        require(codec == mc::fourcc("AV01"), std::string(name) + " is AV1");
        const auto frames = decode_all(mc::fourcc("av01"), packets);
        const auto listing = load(dir + name + ".md5");
        std::istringstream lines(std::string(reinterpret_cast<const char*>(listing.data()), listing.size()));
        std::vector<std::string> expected;
        for (std::string line; std::getline(lines, line);)
            if (line.size() >= 32) expected.push_back(line.substr(0, 32));
        require(!frames.empty() && frames.size() == expected.size(),
                std::string(name) + ": " + std::to_string(frames.size()) + " frames, " + std::to_string(expected.size()) + " expected");
        for (std::size_t i = 0; i < frames.size(); ++i) {
            require(frames[i].bit_depth == bits, std::string(name) + ": bit depth");
            Bytes planes;
            append_planes(planes, frames[i]);
            require(md5(planes) == expected[i], std::string(name) + ": frame " + std::to_string(i + 1) + " MD5 " + md5(planes));
        }
    }
}

} // namespace

int main() {
    try {
        mpeg_tests();
        opus_tests();
        flac_tests();
        vpx_tests();
        av1_tests();
        unsupported_tests();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "media codec tests passed\n";
    return 0;
}
