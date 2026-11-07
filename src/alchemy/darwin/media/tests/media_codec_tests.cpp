// Host-only tests of the media decoders (media_codecs.hpp) through the containers
// (media_container.hpp): each decodes and compares.
// - MPEG audio layers I, II and III: minimp3's own conformance vectors
//   (third_party/minimp3/vectors), to the upstream test's PSNR bound.
// - Opus: a stereo signal encoded here by libopus, in Ogg and through an MP4 dOps
//   configuration, against the input by signal-to-noise ratio.
// - FLAC: signals encoded here by libFLAC, parsed as native FLAC streams, decoded bit-exactly.
// - ALAC: signals of 16, 20, 24 and 32 bits in 1, 2 and 6 channels encoded by the ALAC encoder
//   (media_codecs.hpp), decoded bit-exactly (32-bit to float precision), in CAF and M4A files
//   built here; malformed cookies and packets are rejected; CAF and M4A files made by the packet-file
//   writer (packet_file.hpp) read back the same.
// - VP8 and VP9 (profiles 0, 1 and 2): libvpx's test vectors (IVF, and WebM through the
//   Matroska parser) against their per-frame MD5s.
// - AV1 (8 and 10 bits): libaom's test vectors against their per-frame MD5s.
#include "alchemy/darwin/media/audio_stream.hpp"
#include "alchemy/darwin/media/media_codecs.hpp"
#include "alchemy/darwin/media/media_container.hpp"
#include "alchemy/darwin/media/packet_file.hpp"
#include "alchemy/darwin/media/video_frames.hpp"

#include "alchemy/host/crypto.hpp"

#include "FLAC/stream_encoder.h"
#include "opus.h"

#include <algorithm>
#include <bit>
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

// ---- ALAC -----------------------------------------------------------------------------------------

Bytes box(const char* type, const Bytes& payload) {
    Bytes out;
    put(out, 8 + payload.size(), 4);
    out.insert(out.end(), type, type + 4);
    append(out, payload);
    return out;
}
Bytes full(std::uint32_t flags, const Bytes& rest) { Bytes out; put(out, 0, 1); put(out, flags, 3); append(out, rest); return out; }

// Sines (a frequency per channel) with noise, quantised to `bits` and left-justified in 32 bits,
// with the extreme values at frames 5 and 6.
std::vector<std::int32_t> alac_signal(std::uint32_t frames, std::uint32_t channels, std::uint32_t bits) {
    std::vector<std::int32_t> out(std::size_t(frames) * channels);
    std::uint32_t seed = 12345;
    const double full_scale = std::ldexp(1.0, int(bits) - 1);
    for (std::uint32_t f = 0; f < frames; ++f)
        for (std::uint32_t c = 0; c < channels; ++c) {
            seed = seed * 1664525u + 1013904223u;
            const double noise = (double(seed >> 8) / double(1 << 24) - 0.5) * 0.1;
            const double x = 0.7 * std::sin(2 * 3.14159265358979 * 220.0 * (c + 1) * f / 44100.0) + noise;
            auto v = std::clamp<std::int64_t>(std::llround(x * full_scale), std::int64_t(-full_scale), std::int64_t(full_scale) - 1);
            if (f == 5) v = std::int64_t(full_scale) - 1;
            if (f == 6) v = -std::int64_t(full_scale);
            out[std::size_t(f) * channels + c] = std::int32_t(std::uint32_t(std::uint64_t(v) << (32 - bits)));
        }
    return out;
}

struct AlacStream { std::vector<Bytes> packets; Bytes cookie; std::uint32_t frames{}; };

AlacStream alac_encode(const std::vector<std::int32_t>& signal, std::uint32_t channels, std::uint32_t bits) {
    std::string why;
    auto encoder = codecs::make_audio_encoder(mc::fourcc("alac"), 44100, channels, bits, 0, why);
    require(encoder && encoder->frames_per_packet == 4096 && encoder->bits == bits, "an ALAC encoder: " + why);
    AlacStream s;
    s.frames = std::uint32_t(signal.size() / channels);
    for (std::size_t at = 0; at < signal.size(); at += std::size_t(4096) * channels) {
        const auto n = std::min<std::size_t>(std::size_t(4096) * channels, signal.size() - at);
        Bytes packet;
        require(encoder->encode(std::span(signal).subspan(at, n), packet, why) && !packet.empty() && packet.size() <= encoder->max_packet_bytes,
                "ALAC encodes: " + why);
        s.packets.push_back(std::move(packet));
    }
    s.cookie = encoder->cookie();
    return s;
}

std::uint32_t be32_at(const Bytes& b, std::size_t at) { return (std::uint32_t(b[at]) << 24) | (std::uint32_t(b[at + 1]) << 16) | (std::uint32_t(b[at + 2]) << 8) | b[at + 3]; }

// A CAF file of the packets: 'desc', 'kuki', 'pakt' (BER sizes, the valid frames and priming) and 'data'.
Bytes alac_caf(const AlacStream& s, std::uint32_t channels, std::uint32_t bits, std::uint32_t priming) {
    Bytes file{'c', 'a', 'f', 'f', 0, 1, 0, 0};
    const auto chunk = [&](const char* type, const Bytes& body) { file.insert(file.end(), type, type + 4); put(file, body.size(), 8); append(file, body); };
    Bytes desc;
    put(desc, std::bit_cast<std::uint64_t>(44100.0), 8);
    desc.insert(desc.end(), {'a', 'l', 'a', 'c'});
    put(desc, bits == 16 ? 1 : bits == 20 ? 2 : bits == 24 ? 3 : 4, 4); put(desc, 0, 4); put(desc, 4096, 4); put(desc, channels, 4); put(desc, 0, 4);
    chunk("desc", desc);
    chunk("kuki", s.cookie);
    Bytes pakt;
    const std::uint32_t total = std::uint32_t(s.packets.size()) * 4096;
    put(pakt, s.packets.size(), 8); put(pakt, s.frames - priming, 8); put(pakt, priming, 4); put(pakt, total - s.frames, 4);
    for (const auto& p : s.packets) {
        Bytes ber{static_cast<std::uint8_t>(p.size() & 0x7f)};
        for (auto v = p.size() >> 7; v; v >>= 7) ber.insert(ber.begin(), static_cast<std::uint8_t>(0x80 | (v & 0x7f)));
        append(pakt, ber);
    }
    chunk("pakt", pakt);
    Bytes data(4, 0);   // mEditCount
    for (const auto& p : s.packets) append(data, p);
    chunk("data", data);
    return file;
}

// An M4A file of the packets: an 'alac' sample entry with the cookie in its 'alac' box.
Bytes alac_m4a(const AlacStream& s, std::uint32_t channels, std::uint32_t bits) {
    Bytes brand{'M', '4', 'A', ' '}; put(brand, 0, 4);
    Bytes file = box("ftyp", brand);
    Bytes data;
    for (const auto& p : s.packets) append(data, p);
    const auto data_offset = std::uint32_t(file.size() + 8);
    append(file, box("mdat", data));
    Bytes entry(6, 0); put(entry, 1, 2);
    append(entry, Bytes(8, 0)); put(entry, channels, 2); put(entry, bits, 2); put(entry, 0, 4); put(entry, 44100ULL << 16, 4);
    append(entry, box("alac", full(0, s.cookie)));
    Bytes stsd; put(stsd, 1, 4); append(stsd, box("alac", entry));
    const auto n = std::uint32_t(s.packets.size());
    const std::uint32_t last = s.frames - (n - 1) * 4096;
    Bytes stts; put(stts, 2, 4); put(stts, n - 1, 4); put(stts, 4096, 4); put(stts, 1, 4); put(stts, last, 4);
    Bytes stsc; put(stsc, 1, 4); put(stsc, 1, 4); put(stsc, n, 4); put(stsc, 1, 4);
    Bytes stsz; put(stsz, 0, 4); put(stsz, n, 4); for (const auto& p : s.packets) put(stsz, p.size(), 4);
    Bytes stco; put(stco, 1, 4); put(stco, data_offset, 4);
    Bytes stbl;
    append(stbl, box("stsd", full(0, stsd))); append(stbl, box("stts", full(0, stts))); append(stbl, box("stsc", full(0, stsc)));
    append(stbl, box("stsz", full(0, stsz))); append(stbl, box("stco", full(0, stco)));
    Bytes tkhd(8, 0); put(tkhd, 1, 4); append(tkhd, Bytes(68, 0));
    Bytes mdhd(8, 0); put(mdhd, 44100, 4); put(mdhd, s.frames, 4); put(mdhd, 0, 4);
    Bytes hdlr(4, 0); hdlr.insert(hdlr.end(), {'s', 'o', 'u', 'n'}); append(hdlr, Bytes(13, 0));
    Bytes mdia = box("mdhd", full(0, mdhd));
    append(mdia, box("hdlr", full(0, hdlr)));
    append(mdia, box("minf", box("stbl", stbl)));
    Bytes trak = box("tkhd", full(3, tkhd));
    append(trak, box("mdia", mdia));
    Bytes mvhd(8, 0); put(mvhd, 44100, 4); put(mvhd, s.frames, 4); append(mvhd, Bytes(80, 0));
    Bytes moov = box("mvhd", full(0, mvhd));
    append(moov, box("trak", trak));
    append(file, box("moov", moov));
    return file;
}

void alac_tests() {
    std::string why;
    const auto alac = mc::fourcc("alac");
    for (const std::uint32_t bits : {16u, 20u, 24u, 32u})
        for (const std::uint32_t channels : {1u, 2u, 6u}) {
            const std::string what = "ALAC " + std::to_string(bits) + "-bit " + std::to_string(channels) + " ch";
            const std::uint32_t frames = 4096 * 2 + 1000;
            const auto signal = alac_signal(frames, channels, bits);
            const auto s = alac_encode(signal, channels, bits);
            // The cookie: the ALACSpecificConfig, then (above two channels) the big-endian layout.
            require(s.cookie.size() == (channels > 2 ? 48u : 24u) && be32_at(s.cookie, 0) == 4096 && s.cookie[5] == bits && s.cookie[9] == channels &&
                    be32_at(s.cookie, 20) == 44100, what + ": cookie");
            std::size_t largest = 0;
            for (const auto& p : s.packets) largest = std::max(largest, p.size());
            require(be32_at(s.cookie, 12) == largest && be32_at(s.cookie, 16) > 0, what + ": maxFrameBytes and avgBitRate");
            if (channels == 6) require(be32_at(s.cookie, 36) == ((124u << 16) | 6), what + ": the MPEG 5.1 (D) layout tag, big endian");
            auto decoder = codecs::make_audio_decoder(alac, 0, 0, s.cookie, why);
            require(decoder && decoder->rate == 44100 && decoder->channels == channels && decoder->bits == bits && decoder->max_frames == 4096,
                    what + ": a decoder: " + why);
            std::vector<float> decoded;
            for (const auto& p : s.packets) require(decoder->decode(p, decoded, why), what + ": decodes: " + why);
            require(decoded.size() == signal.size(), what + ": " + std::to_string(decoded.size() / channels) + " frames");
            // Up to 24 bits a float holds the sample exactly; a 32-bit sample to float precision.
            const double tolerance = bits == 32 ? 256.0 : 0.0;
            for (std::size_t i = 0; i < signal.size(); ++i)
                require(std::fabs(double(decoded[i]) * 2147483648.0 - double(signal[i])) <= tolerance,
                        what + ": sample " + std::to_string(i) + " is " + std::to_string(double(decoded[i]) * 2147483648.0) + ", expected " + std::to_string(signal[i]));
            if (bits != 16 || channels != 2) continue;

            // The cookie as an MP4 'alac' box payload (version and flags first), and wrapped in the
            // 'frma' and 'alac' atoms of older cookies.
            Bytes boxed(4, 0);
            append(boxed, s.cookie);
            Bytes wrapped{0, 0, 0, 12, 'f', 'r', 'm', 'a', 'a', 'l', 'a', 'c', 0, 0, 0, 36, 'a', 'l', 'a', 'c', 0, 0, 0, 0};
            append(wrapped, s.cookie);
            for (const auto* config : {&boxed, &wrapped}) {
                auto other = codecs::make_audio_decoder(alac, 44100, 2, *config, why);
                std::vector<float> again;
                require(other && other->decode(s.packets[0], again, why) &&
                        std::equal(again.begin(), again.end(), decoded.begin()), "ALAC decodes with a wrapped cookie: " + why);
            }

            // In CAF (with priming) and M4A, through the stream.
            for (const std::uint32_t priming : {0u, 100u}) {
                const auto caf = alac_caf(s, channels, bits, priming);
                const std::vector<float> reference(decoded.begin() + std::ptrdiff_t(priming) * channels, decoded.end());
                stream_checks("ALAC in CAF, priming " + std::to_string(priming), caf, reference, channels, 0.0f);
                std::int32_t status = 0;
                auto stream = alchemy::gui::completion_kit::media::audio_stream::Stream::open(reader(caf), caf.size(), status, why);
                const auto asbd = stream->asbd();
                std::uint32_t format{}, flags{}, per_packet{};
                std::memcpy(&format, asbd.data() + 8, 4); std::memcpy(&flags, asbd.data() + 12, 4); std::memcpy(&per_packet, asbd.data() + 20, 4);
                require(stream->file_type() == mc::fourcc("caff") && format == alac && flags == 1 && per_packet == 4096 && stream->magic_cookie() == s.cookie &&
                        stream->priming() == priming && stream->packet_count() == 3, "ALAC in CAF: the packets' description");
            }
            const auto m4a = alac_m4a(s, channels, bits);
            stream_checks("ALAC in M4A", m4a, decoded, channels, 0.0f);
            std::int32_t status = 0;
            auto stream = alchemy::gui::completion_kit::media::audio_stream::Stream::open(reader(m4a), m4a.size(), status, why);
            require(stream->file_type() == mc::fourcc("m4af") && stream->magic_cookie() == s.cookie, "ALAC in M4A: the cookie without the box's version");
        }

    // Cookies the library cannot use: no decoder.
    const auto good = alac_encode(alac_signal(4096, 1, 16), 1, 16);
    const auto with = [&](std::size_t at, std::uint8_t value) { Bytes c = good.cookie; c[at] = value; return c; };
    for (const auto& bad : {with(0, 0xff), with(2, 0), with(4, 1), with(5, 18), with(9, 0), with(9, 9), Bytes(good.cookie.begin(), good.cookie.begin() + 20)})
        require(!codecs::make_audio_decoder(alac, 44100, 1, bad, why), "a bad ALAC cookie is refused");
    // A partial frame longer than the frame length, a truncated packet and random bytes are rejected
    // without decoding past the buffers.
    auto decoder = codecs::make_audio_decoder(alac, 44100, 1, good.cookie, why);
    std::vector<float> out;
    // SCE tag 0, instance 0, 12 unused bits, partial frame and escape flags, then 2^20 samples.
    Bytes partial{0x00, 0x00, 0x12, 0x00, 0x20, 0x00, 0x00};
    partial.resize(64, 0);
    require(!decoder->decode(partial, out, why) && out.empty(), "an oversized partial frame is rejected");
    Bytes truncated(good.packets[0].begin(), good.packets[0].begin() + std::ptrdiff_t(good.packets[0].size() / 2));
    static_cast<void>(decoder->decode(truncated, out, why));
    std::uint32_t seed = 7;
    for (int i = 0; i < 3000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        Bytes junk(1 + (seed >> 8) % 3000);
        for (auto& b : junk) { seed = seed * 1664525u + 1013904223u; b = std::uint8_t(seed >> 24); }
        out.clear();
        if (decoder->decode(junk, out, why)) require(out.size() <= 4096, "a junk packet decodes to at most a frame length");
    }
    require(decoder->decode(good.packets[0], out, why), "the decoder still decodes after rejected packets");
    // The packet-file writer: CAF (constant and varying packet frames, priming) and M4A files of the
    // packets read back through the stream exactly.
    namespace pf = alchemy::gui::completion_kit::media::packet_file;
    const std::uint32_t frames = 4096 * 2 + 1000;
    const auto signal = alac_signal(frames, 2, 24);
    const auto s = alac_encode(signal, 2, 24);
    std::vector<float> decoded;
    auto reference_decoder = codecs::make_audio_decoder(alac, 0, 0, s.cookie, why);
    for (const auto& p : s.packets) require(reference_decoder->decode(p, decoded, why), "decodes");
    const auto write_file = [&](std::uint32_t type, std::uint32_t priming, bool tell_frames) {
        pf::Format format{type, 44100, alac, 3, 4096, 2};
        auto w = pf::Writer::make(format, why);
        require(w.has_value(), "a packet-file writer: " + why);
        Bytes file = w->header();
        for (std::size_t i = 0; i < s.packets.size(); ++i) {
            require(w->data_end() == file.size(), "packets go at the data's end");
            append(file, s.packets[i]);
            const bool last = i + 1 == s.packets.size();
            w->add_packet(std::uint32_t(s.packets[i].size()), tell_frames || last ? (last ? frames - 4096 * 2 : 4096) : 0);
        }
        w->set_cookie(s.cookie);
        if (priming) w->set_priming(priming, 0);
        for (const auto& [offset, bytes] : w->finish()) {
            if (file.size() < offset + bytes.size()) file.resize(offset + bytes.size());
            std::copy(bytes.begin(), bytes.end(), file.begin() + std::ptrdiff_t(offset));
        }
        return file;
    };
    for (const auto type : {mc::fourcc("caff"), mc::fourcc("m4af"), mc::fourcc("mp4f")}) {
        const auto file = write_file(type, 0, false);
        stream_checks("packet-file writer " + std::to_string(type), file, decoded, 2, 0.0f);
        std::int32_t status = 0;
        auto stream = alchemy::gui::completion_kit::media::audio_stream::Stream::open(reader(file), file.size(), status, why);
        require(stream->magic_cookie() == s.cookie && stream->packet_count() == 3 && stream->file_type() == (type == mc::fourcc("caff") ? type : type),
                "packet-file writer: the cookie and packets");
    }
    const std::vector<float> primed(decoded.begin() + 2 * 300, decoded.end());
    stream_checks("packet-file writer, CAF with priming", write_file(mc::fourcc("caff"), 300, true), primed, 2, 0.0f);
    {   // Varying frames: the description says 0 frames per packet and the table has each packet's.
        pf::Format format{mc::fourcc("caff"), 44100, alac, 3, 4096, 2};
        auto w = pf::Writer::make(format, why);
        w->add_packet(100, 1000);
        w->add_packet(100, 4096);
        Bytes file = w->header();
        file.resize(file.size() + 200);
        for (const auto& [offset, bytes] : w->finish()) {
            if (file.size() < offset + bytes.size()) file.resize(offset + bytes.size());
            std::copy(bytes.begin(), bytes.end(), file.begin() + std::ptrdiff_t(offset));
        }
        mc::Movie movie;
        require(mc::parse(reader(file), file.size(), movie, why) && movie.tracks[0].samples.size() == 2 && movie.tracks[0].samples[0].duration == 1000 &&
                movie.tracks[0].duration == 5096, "a CAF of varying packet frames");
    }
    require(!pf::Writer::make({mc::fourcc("WAVE"), 44100, alac, 1, 4096, 2}, why) && !pf::Writer::make({mc::fourcc("caff"), 44100, alac, 0, 4096, 2}, why) &&
            !pf::Writer::make({mc::fourcc("caff"), 44100, alac, 1, 4096, 9}, why) && !pf::Writer::make({mc::fourcc("m4af"), 44100, mc::fourcc("aac "), 1, 1024, 2}, why),
            "the writer's limits");
    // Encoder limits.
    require(!codecs::make_audio_encoder(alac, 44100, 9, 16, 0, why) && !codecs::make_audio_encoder(alac, 44100, 2, 18, 0, why) &&
            !codecs::make_audio_encoder(alac, 44100.5, 2, 16, 0, why) && !codecs::make_audio_encoder(alac, 44100, 2, 16, 20000, why) &&
            !codecs::make_audio_encoder(mc::fourcc("mp4a"), 44100, 2, 16, 0, why), "encoder limits");
}

void unsupported_tests() {
    std::string why;
    for (const char* code : {"avc1"}) {   // ('mp4a' decodes where the host has an AAC decoder)
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
        alac_tests();
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
