#include "alchemy/darwin/media/media_codecs.hpp"

#include "alchemy/darwin/media/media_container.hpp"
#include "alchemy/host/media_codec.hpp"

#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"
#include "opus_multistream.h"
#include "FLAC/stream_decoder.h"
#include "ALACBitUtilities.h"
#include "ALACDecoder.h"
#include "ALACEncoder.h"
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#include "dav1d/dav1d.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>

namespace alchemy::gui::completion_kit::media::codecs {

namespace {

using container::fourcc;

// ---- MPEG audio (minimp3) ---------------------------------------------------

class MpegDecoder final : public AudioDecoder {
public:
    MpegDecoder(double r, std::uint32_t c) {
        rate = r;
        channels = c ? c : 2;
        max_frames = MINIMP3_MAX_SAMPLES_PER_FRAME / 2;
        mp3dec_init(&dec_);
    }

    bool decode(std::span<const std::uint8_t> packet, std::vector<float>& out, std::string& why) override {
        if (packet.size() < 4) { why = "MPEG audio packet shorter than a header"; return false; }
        // minimp3 syncs on a frame followed by a matching header; a container packet is one frame,
        // so the frame's own header follows it here.
        buffer_.assign(packet.begin(), packet.end());
        buffer_.insert(buffer_.end(), packet.begin(), packet.begin() + 4);
        mp3dec_frame_info_t info{};
        const int frames = mp3dec_decode_frame(&dec_, buffer_.data(), int(buffer_.size()), pcm_, &info);
        if (info.frame_bytes == 0) { why = "no MPEG audio frame in the packet"; return false; }
        if (frames == 0) return true;   // the bit reservoir is still filling (after a seek)
        const std::uint32_t got = std::uint32_t(info.channels);
        const std::size_t base = out.size();
        out.resize(base + std::size_t(frames) * channels);
        for (int f = 0; f < frames; ++f)
            for (std::uint32_t c = 0; c < channels; ++c)
                out[base + std::size_t(f) * channels + c] = pcm_[std::size_t(f) * got + std::min(c, got - 1)];
        return true;
    }

    void reset() override { mp3dec_init(&dec_); }

private:
    mp3dec_t dec_{};
    std::vector<std::uint8_t> buffer_;
    float pcm_[MINIMP3_MAX_SAMPLES_PER_FRAME]{};
};

// ---- Opus (libopus multistream) ---------------------------------------------

struct OpusSetup {
    std::uint32_t channels{}, pre_skip{}, family{}, streams{1}, coupled{};
    std::int32_t gain{};
    std::vector<std::uint8_t> mapping;
};

bool opus_setup(std::span<const std::uint8_t> c, std::uint32_t channels, OpusSetup& s, std::string& why) {
    std::size_t at = 0;
    bool little = false;
    if (c.size() >= 19 && std::memcmp(c.data(), "OpusHead", 8) == 0) { at = 9; little = true; }   // Ogg / Matroska
    else if (c.size() >= 11 && c[0] == 0) at = 1;                                                // dOps (version 0)
    else if (c.empty()) {
        if (channels == 0 || channels > 2) { why = "Opus without a configuration needs one or two channels"; return false; }
        s.channels = channels;
        s.streams = 1;
        s.coupled = channels == 2 ? 1 : 0;
        s.mapping = channels == 2 ? std::vector<std::uint8_t>{0, 1} : std::vector<std::uint8_t>{0};
        return true;
    } else { why = "unrecognised Opus configuration"; return false; }
    auto u16 = [&](std::size_t o) { return little ? std::uint32_t(c[o] | (c[o + 1] << 8)) : std::uint32_t((c[o] << 8) | c[o + 1]); };
    s.channels = c[at];
    s.pre_skip = u16(at + 1);
    s.gain = std::int16_t(u16(at + 7));
    s.family = c[at + 9];
    if (s.channels == 0) { why = "Opus configuration with no channels"; return false; }
    if (s.family == 0) {
        if (s.channels > 2) { why = "Opus mapping family 0 with more than two channels"; return false; }
        s.streams = 1;
        s.coupled = s.channels == 2 ? 1 : 0;
        s.mapping = s.channels == 2 ? std::vector<std::uint8_t>{0, 1} : std::vector<std::uint8_t>{0};
        return true;
    }
    if (c.size() < at + 12 + s.channels) { why = "Opus channel mapping table is truncated"; return false; }
    s.streams = c[at + 10];
    s.coupled = c[at + 11];
    s.mapping.assign(c.begin() + std::ptrdiff_t(at + 12), c.begin() + std::ptrdiff_t(at + 12 + s.channels));
    if (s.streams == 0 || s.coupled > s.streams) { why = "Opus stream counts are inconsistent"; return false; }
    return true;
}

class OpusDecoder final : public AudioDecoder {
public:
    static constexpr std::uint32_t maximum_frames = 5760;   // 120 ms at 48 kHz

    bool open(std::span<const std::uint8_t> config, std::uint32_t c, std::string& why) {
        OpusSetup s;
        if (!opus_setup(config, c, s, why)) return false;
        int error = 0;
        dec_ = opus_multistream_decoder_create(48000, int(s.channels), int(s.streams), int(s.coupled), s.mapping.data(), &error);
        if (!dec_ || error != OPUS_OK) { why = std::string("Opus decoder: ") + opus_strerror(error); return false; }
        if (s.gain) opus_multistream_decoder_ctl(dec_, OPUS_SET_GAIN(int(s.gain)));
        rate = 48000;
        channels = s.channels;
        max_frames = maximum_frames;
        return true;
    }
    ~OpusDecoder() override { if (dec_) opus_multistream_decoder_destroy(dec_); }

    bool decode(std::span<const std::uint8_t> packet, std::vector<float>& out, std::string& why) override {
        const std::size_t base = out.size();
        out.resize(base + std::size_t(maximum_frames) * channels);
        const int frames = opus_multistream_decode_float(dec_, packet.data(), opus_int32(packet.size()), out.data() + base,
                                                         int(maximum_frames), 0);
        if (frames < 0) {
            out.resize(base);
            why = std::string("Opus packet: ") + opus_strerror(frames);
            return false;
        }
        out.resize(base + std::size_t(frames) * channels);
        return true;
    }

    void reset() override { opus_multistream_decoder_ctl(dec_, OPUS_RESET_STATE); }

private:
    OpusMSDecoder* dec_{};
};

// ---- FLAC (libFLAC stream decoder fed frame by frame) -----------------------

class FlacDecoder final : public AudioDecoder {
public:
    bool open(std::span<const std::uint8_t> config, std::string& why) {
        // STREAMINFO: a bare metadata block, or one after "fLaC" (Matroska) or after the dfLa
        // version and flags (MP4).
        std::size_t at = SIZE_MAX;
        for (std::size_t o : {std::size_t(0), std::size_t(4)}) {
            if (config.size() >= o + 38 && (config[o] & 0x7f) == 0 &&
                ((config[o + 1] << 16) | (config[o + 2] << 8) | config[o + 3]) == 34) { at = o; break; }
        }
        if (at == SIZE_MAX) { why = "no FLAC STREAMINFO in the configuration"; return false; }
        header_.assign({'f', 'L', 'a', 'C', 0x80, 0, 0, 34});
        header_.insert(header_.end(), config.begin() + std::ptrdiff_t(at + 4), config.begin() + std::ptrdiff_t(at + 38));
        const std::uint8_t* si = header_.data() + 8;
        max_frames = std::uint32_t((si[2] << 8) | si[3]);
        rate = double((std::uint32_t(si[10]) << 12) | (std::uint32_t(si[11]) << 4) | (si[12] >> 4));
        channels = ((si[12] >> 1) & 7) + 1;
        bits = (std::uint32_t(si[12] & 1) << 4 | (si[13] >> 4)) + 1;
        if (rate <= 0 || max_frames == 0) { why = "FLAC STREAMINFO has no rate or block size"; return false; }
        dec_ = FLAC__stream_decoder_new();
        if (!dec_) { why = "FLAC decoder could not be created"; return false; }
        if (FLAC__stream_decoder_init_stream(dec_, &FlacDecoder::read, nullptr, nullptr, nullptr, nullptr, &FlacDecoder::write,
                                             nullptr, &FlacDecoder::error, this) != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            why = "FLAC decoder could not be initialised";
            return false;
        }
        return prime(why);
    }
    ~FlacDecoder() override { if (dec_) FLAC__stream_decoder_delete(dec_); }

    bool decode(std::span<const std::uint8_t> packet, std::vector<float>& out, std::string& why) override {
        if (FLAC__stream_decoder_get_state(dec_) == FLAC__STREAM_DECODER_END_OF_STREAM) FLAC__stream_decoder_flush(dec_);
        input_ = packet;
        out_ = &out;
        failed_ = false;
        const bool ok = FLAC__stream_decoder_process_single(dec_);
        out_ = nullptr;
        input_ = {};
        if (!ok || failed_) {
            why = failed_ ? failure_ : "FLAC decoder failed";
            if (!ok) { FLAC__stream_decoder_reset(dec_); std::string ignored; prime(ignored); }
            return false;
        }
        return true;
    }

    void reset() override { FLAC__stream_decoder_flush(dec_); }

private:
    bool prime(std::string& why) {
        input_ = header_;
        const bool ok = FLAC__stream_decoder_process_until_end_of_metadata(dec_);
        input_ = {};
        if (!ok || failed_) { why = "FLAC STREAMINFO was rejected"; return false; }
        return true;
    }

    static FLAC__StreamDecoderReadStatus read(const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes, void* self) {
        auto& d = *static_cast<FlacDecoder*>(self);
        if (d.input_.empty()) { *bytes = 0; return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM; }
        const std::size_t n = std::min(*bytes, d.input_.size());
        std::memcpy(buffer, d.input_.data(), n);
        d.input_ = d.input_.subspan(n);
        *bytes = n;
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }

    static FLAC__StreamDecoderWriteStatus write(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                                                const FLAC__int32* const buffer[], void* self) {
        auto& d = *static_cast<FlacDecoder*>(self);
        if (!d.out_) return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
        const std::uint32_t n = frame->header.blocksize, got = frame->header.channels;
        const double scale = 1.0 / double(std::uint64_t(1) << (frame->header.bits_per_sample - 1));
        auto& out = *d.out_;
        const std::size_t base = out.size();
        out.resize(base + std::size_t(n) * d.channels);
        for (std::uint32_t i = 0; i < n; ++i)
            for (std::uint32_t c = 0; c < d.channels; ++c)
                out[base + std::size_t(i) * d.channels + c] = float(double(buffer[std::min(c, got - 1)][i]) * scale);
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    static void error(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status, void* self) {
        auto& d = *static_cast<FlacDecoder*>(self);
        d.failed_ = true;
        d.failure_ = std::string("FLAC: ") + FLAC__StreamDecoderErrorStatusString[status];
    }

    FLAC__StreamDecoder* dec_{};
    std::vector<std::uint8_t> header_;
    std::span<const std::uint8_t> input_;
    std::vector<float>* out_{};
    bool failed_{};
    std::string failure_;
};

// ---- Apple Lossless (Apple's open-source ALAC codec) ----------------------------

std::uint32_t alac_be32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }

bool alac_bits_supported(std::uint32_t bits) { return bits == 16 || bits == 20 || bits == 24 || bits == 32; }

// An ALACSpecificConfig the library can use: frameLength 1-16384, compatible version 0, a bit
// depth it decodes, 1-8 channels and an entropy-coder limit (kb) a shift can take.
bool alac_config_valid(std::span<const std::uint8_t> c) {
    if (c.size() < 24) return false;
    const auto frame_length = alac_be32(c.data());
    return frame_length >= 1 && frame_length <= 16384 && c[4] == 0 && alac_bits_supported(c[5]) && c[8] <= 31 && c[9] >= 1 && c[9] <= 8;
}

class AlacDecoder final : public AudioDecoder {
public:
    bool open(std::span<const std::uint8_t> config, double container_rate, std::string& why) {
        // Older magic cookies wrap the config in 'frma' and 'alac' atoms; an MP4 'alac' box payload
        // starts with its version and flags.
        const auto atom = [&](const char* type) { return config.size() >= 12 && std::memcmp(config.data() + 4, type, 4) == 0; };
        if (atom("frma")) config = config.subspan(12);
        if (atom("alac")) config = config.subspan(12);
        if (!alac_config_valid(config) && config.size() >= 28 && alac_be32(config.data()) == 0 && alac_config_valid(config.subspan(4)))
            config = config.subspan(4);
        if (!alac_config_valid(config)) { why = "no usable ALACSpecificConfig in the configuration"; return false; }
        config_.assign(config.begin(), config.begin() + 24);
        frame_length_ = alac_be32(config_.data());
        bits = config_[5];
        channels = config_[9];
        rate = double(alac_be32(config_.data() + 20));
        if (rate <= 0) rate = container_rate;
        max_frames = frame_length_;
        if (decoder_.Init(config_.data(), std::uint32_t(config_.size())) != 0) { why = "the ALAC decoder rejected its configuration"; return false; }
        samples_.resize(std::size_t(frame_length_) * channels * 4);
        return true;
    }

    bool decode(std::span<const std::uint8_t> packet, std::vector<float>& out, std::string& why) override {
        // The bit reader does not stop at the packet's end: it reads from a copy followed by more
        // zeros than one packet's elements can consume (frame length <= 16384 samples of <= 33 bits,
        // two channels per element).
        constexpr std::size_t pad = 256 * 1024;
        input_.assign(packet.begin(), packet.end());
        input_.resize(packet.size() + pad, 0);
        BitBuffer bits_in{};
        BitBufferInit(&bits_in, input_.data(), std::uint32_t(packet.size()));
        std::uint32_t frames = 0;
        if (packet.empty() || decoder_.Decode(&bits_in, samples_.data(), frame_length_, channels, &frames) != 0 || frames > frame_length_) {
            why = "the ALAC decoder rejected the packet";
            return false;
        }
        const std::size_t base = out.size(), count = std::size_t(frames) * channels;
        out.resize(base + count);
        const std::uint8_t* s = samples_.data();
        for (std::size_t i = 0; i < count; ++i) {
            float v{};
            switch (bits) {
            case 16: v = float(std::int16_t(std::uint16_t(s[2 * i] | (s[2 * i + 1] << 8)))) / 32768.0f; break;
            case 20: case 24: {   // three bytes, little endian; 20-bit samples are left-aligned in them
                const std::int32_t x = std::int32_t((std::uint32_t(s[3 * i]) << 8) | (std::uint32_t(s[3 * i + 1]) << 16) | (std::uint32_t(s[3 * i + 2]) << 24)) >> 8;
                v = float(double(x) / 8388608.0);
                break;
            }
            default: {
                std::int32_t x;
                std::memcpy(&x, s + 4 * i, 4);
                v = float(double(x) / 2147483648.0);
                break;
            }
            }
            out[base + i] = v;
        }
        return true;
    }

    void reset() override {}   // ALAC packets are independent

private:
    ALACDecoder decoder_;
    std::vector<std::uint8_t> config_, input_, samples_;
    std::uint32_t frame_length_{};
};

class AlacEncoder final : public AudioEncoder {
public:
    bool open(double sample_rate, std::uint32_t channel_count, std::uint32_t bit_depth, std::uint32_t frames, std::string& why) {
        if (!alac_bits_supported(bit_depth)) { why = "ALAC encodes 16, 20, 24 or 32 bits"; return false; }
        if (channel_count < 1 || channel_count > 8) { why = "ALAC encodes 1-8 channels"; return false; }
        if (!(sample_rate >= 1 && sample_rate <= 4294967295.0) || sample_rate != std::floor(sample_rate)) { why = "ALAC needs an integer sample rate"; return false; }
        if (frames == 0) frames = kALACDefaultFramesPerPacket;
        if (frames > 16384) { why = "ALAC packets hold at most 16384 frames"; return false; }
        rate = sample_rate;
        channels = channel_count;
        bits = bit_depth;
        frames_per_packet = frames;
        bytes_per_sample_ = (bits + 7) / 8;   // 20-bit samples take three bytes, left-aligned
        input_format_ = {rate, kALACFormatLinearPCM, kALACFormatFlagIsSignedInteger | kALACFormatFlagIsPacked, bytes_per_sample_ * channels, 1,
                         bytes_per_sample_ * channels, channels, bits, 0};
        output_format_ = {rate, kALACFormatAppleLossless, bits == 16 ? 1u : bits == 20 ? 2u : bits == 24 ? 3u : 4u, 0, frames, 0, channels, 0, 0};
        encoder_.SetFrameSize(frames);
        if (encoder_.InitializeEncoder(output_format_) != 0) { why = "the ALAC encoder could not be initialised"; return false; }
        max_packet_bytes = frames * channels * ((10 + 32) / 8) + 1;   // the encoder's own bound
        input_.resize(std::size_t(frames) * channels * bytes_per_sample_);
        output_.resize(max_packet_bytes + kALACMaxEscapeHeaderBytes);
        return true;
    }

    bool encode(std::span<const std::int32_t> samples, std::vector<std::uint8_t>& packet, std::string& why) override {
        const std::size_t frames = samples.size() / channels;
        if (frames == 0 || frames > frames_per_packet || samples.size() % channels) { why = "an ALAC packet takes 1 to frames_per_packet whole frames"; return false; }
        std::uint8_t* p = input_.data();
        for (const std::int32_t sample : samples) {
            const auto u = std::uint32_t(sample);
            switch (bits) {
            case 16: p[0] = std::uint8_t(u >> 16); p[1] = std::uint8_t(u >> 24); p += 2; break;
            case 20: p[0] = std::uint8_t((u >> 8) & 0xf0); p[1] = std::uint8_t(u >> 16); p[2] = std::uint8_t(u >> 24); p += 3; break;
            case 24: p[0] = std::uint8_t(u >> 8); p[1] = std::uint8_t(u >> 16); p[2] = std::uint8_t(u >> 24); p += 3; break;
            default: std::memcpy(p, &u, 4); p += 4; break;
            }
        }
        auto bytes = std::int32_t(frames * channels * bytes_per_sample_);
        if (encoder_.Encode(input_format_, input_format_, input_.data(), output_.data(), &bytes) != 0 || bytes <= 0 ||
            std::uint32_t(bytes) > output_.size()) {
            why = "the ALAC encoder failed";
            return false;
        }
        packet.assign(output_.begin(), output_.begin() + bytes);
        total_bytes_ += std::uint64_t(bytes);
        total_frames_ += frames;
        return true;
    }

    std::vector<std::uint8_t> cookie() override {
        std::uint32_t size = encoder_.GetMagicCookieSize(channels);
        std::vector<std::uint8_t> c(size);
        encoder_.GetMagicCookie(c.data(), &size);
        c.resize(size);
        if (c.size() < 24) return c;
        // The library leaves the average bit rate at 0: bits per second of what was encoded.
        const auto rate_bps = total_frames_ ? std::uint32_t(std::min(4294967295.0, double(total_bytes_) * 8 * rate / double(total_frames_))) : 0u;
        for (int i = 0; i < 4; ++i) c[16 + i] = std::uint8_t(rate_bps >> (24 - 8 * i));
        // The channel layout info is big endian (ALACMagicCookieDescription.txt); the library writes
        // its tag in host order.
        if (c.size() >= 24 + 12 + 12) {
            std::uint8_t* tag = c.data() + 24 + 12;
            std::reverse(tag, tag + 4);
        }
        return c;
    }

private:
    ALACEncoder encoder_;
    AudioFormatDescription input_format_{}, output_format_{};
    std::uint32_t bytes_per_sample_{};
    std::vector<std::uint8_t> input_, output_;
    std::uint64_t total_bytes_{}, total_frames_{};
};

// ---- AAC (the host's decoder) -----------------------------------------------------

// The AudioSpecificConfig in an AAC configuration: an MP4 esds payload (version and flags, then the
// ES_Descriptor), an ES_Descriptor (tag 3), or the AudioSpecificConfig itself.
std::vector<std::uint8_t> aac_audio_specific_config(std::span<const std::uint8_t> c) {
    std::size_t at = 0;
    if (c.size() > 4 && c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0 && c[4] == 0x03) at = 4;
    else if (c.empty() || c[0] != 0x03) return {c.begin(), c.end()};
    const auto descriptor = [&](std::uint8_t& tag, std::size_t& length) {
        if (at >= c.size()) return false;
        tag = c[at++];
        length = 0;
        for (int i = 0; i < 4 && at < c.size(); ++i) {
            const auto b = c[at++];
            length = (length << 7) | (b & 0x7f);
            if (!(b & 0x80)) break;
        }
        return true;
    };
    std::uint8_t tag{};
    std::size_t length{};
    if (!descriptor(tag, length) || tag != 3 || at + 3 > c.size()) return {};
    const auto flags = c[at + 2];
    at += 3;
    if (flags & 0x80) at += 2;
    if (flags & 0x40) { if (at >= c.size()) return {}; at += 1 + c[at]; }
    if (flags & 0x20) at += 2;
    if (!descriptor(tag, length) || tag != 4 || at + 13 > c.size()) return {};
    at += 13;   // objectTypeIndication, streamType, bufferSizeDB, maxBitrate, avgBitrate
    if (!descriptor(tag, length) || tag != 5 || at + length > c.size()) return {};
    return {c.begin() + static_cast<std::ptrdiff_t>(at), c.begin() + static_cast<std::ptrdiff_t>(at + length)};
}

bool aac_decoder_exists() {
    static const bool exists = [] {
        const auto listed = alchemy::host::media_codec::decoders(alchemy::host::media_codec::Codec::aac);
        return listed && !listed->empty();
    }();
    return exists;
}

class AacDecoder final : public AudioDecoder {
public:
    ~AacDecoder() override { alchemy::host::media_codec::close(decoder_); }
    bool open(std::span<const std::uint8_t> config, double container_rate, std::uint32_t container_channels, std::string& why) {
        namespace mc = alchemy::host::media_codec;
        const auto asc = aac_audio_specific_config(config);
        if (asc.size() < 2) { why = "no AudioSpecificConfig in the AAC configuration"; return false; }
        // The sampling frequency and channel configuration of the AudioSpecificConfig (ISO/IEC 14496-3, 1.6.2.1).
        static constexpr std::uint32_t rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
        const std::uint32_t index = ((asc[0] & 7) << 1) | (asc[1] >> 7);
        const std::uint32_t channel_config = (asc[1] >> 3) & 15;
        rate = index < 13 ? rates[index] : container_rate;
        channels = channel_config >= 1 && channel_config <= 6 ? channel_config : channel_config == 7 ? 8 : container_channels;
        if (!(rate > 0) || !channels) { why = "the AAC configuration names no rate or channels"; return false; }
        const auto opened = mc::open_audio_decoder({mc::Codec::aac, static_cast<std::uint32_t>(rate), channels, asc, {}});
        if (!opened) {
            why = opened.error() == alchemy::host::Error::not_found || opened.error() == alchemy::host::Error::unsupported ? "no AAC decoder exists on this host"
                                                                                                                         : "the host's AAC decoder refused the stream";
            return false;
        }
        decoder_ = *opened;
        held_ = mc::component(decoder_).delay;
        max_frames = 1024;
        return true;
    }

    bool decode(std::span<const std::uint8_t> packet, std::vector<float>& out, std::string& why) override {
        // The block the host's decoder holds back (its own delay) comes first, as silence: the first block a
        // decoder that hides nothing gives is that delay, and the container's priming trims it.
        std::vector<std::int16_t> pcm;
        if (fresh_) {
            pcm.assign(std::size_t(held_) * channels, 0);
            fresh_ = false;
        }
        if (!alchemy::host::media_codec::decode(decoder_, packet, pcm)) {
            why = "the host's AAC decoder rejected the packet";
            return false;
        }
        const auto base = out.size();
        out.resize(base + pcm.size());
        for (std::size_t i = 0; i < pcm.size(); ++i) out[base + i] = float(pcm[i]) / 32768.0f;
        return true;
    }

    void reset() override {
        alchemy::host::media_codec::flush(decoder_);
        fresh_ = true;
    }

private:
    alchemy::host::media_codec::AudioDecoder* decoder_{};
    std::uint32_t held_{};
    bool fresh_{true};
};

// ---- video: copies of the decoders' pictures ----------------------------------

void copy_plane(VideoFrame::Plane& plane, const std::uint8_t* data, std::ptrdiff_t stride, std::uint32_t width,
                std::uint32_t height, std::uint32_t bytes_per_sample) {
    plane.width = width;
    plane.height = height;
    plane.stride = width * bytes_per_sample;
    plane.data.resize(std::size_t(plane.stride) * height);
    for (std::uint32_t y = 0; y < height; ++y)
        std::memcpy(plane.data.data() + std::size_t(y) * plane.stride, data + std::ptrdiff_t(y) * stride, plane.stride);
}

// ---- VP8 and VP9 (libvpx) ---------------------------------------------------

class VpxDecoder final : public VideoDecoder {
public:
    bool open(bool vp9, std::string& why) {
        vp9_ = vp9;
        vpx_codec_dec_cfg_t cfg{};
        cfg.threads = 1;
        if (vpx_codec_dec_init(&ctx_, vp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx(), &cfg, 0) != VPX_CODEC_OK) {
            why = std::string("libvpx: ") + vpx_codec_error(&ctx_);
            return false;
        }
        open_ = true;
        return true;
    }
    ~VpxDecoder() override { if (open_) vpx_codec_destroy(&ctx_); }

    bool send(std::span<const std::uint8_t> packet, std::int64_t pts, std::string& why) override {
        // libvpx hands each image back with the user data of the packet that produced it.
        times_.push_back(pts);
        auto* tag = reinterpret_cast<void*>(std::uintptr_t(++serial_));
        if (vpx_codec_decode(&ctx_, packet.data(), unsigned(packet.size()), tag, 0) != VPX_CODEC_OK) {
            why = std::string("libvpx: ") + vpx_codec_error(&ctx_);
            if (const char* detail = vpx_codec_error_detail(&ctx_)) why += std::string(" (") + detail + ")";
            times_.pop_back();
            --serial_;
            return false;
        }
        collect();
        return true;
    }

    bool receive(VideoFrame& out) override {
        if (ready_.empty()) return false;
        out = std::move(ready_.front());
        ready_.pop_front();
        return true;
    }

    void drain() override {
        vpx_codec_decode(&ctx_, nullptr, 0, nullptr, 0);
        collect();
    }

    void reset() override {
        ready_.clear();
        times_.clear();
        if (open_) vpx_codec_destroy(&ctx_);
        open_ = false;
        std::string ignored;
        open(vp9_, ignored);
    }

private:
    void collect() {
        vpx_codec_iter_t iter = nullptr;
        while (const vpx_image_t* img = vpx_codec_get_frame(&ctx_, &iter)) {
            VideoFrame f;
            const auto serial = std::uint64_t(reinterpret_cast<std::uintptr_t>(img->user_priv));
            // Times of packets that produced no picture (VP9 hidden frames) are dropped.
            const std::size_t back = std::size_t(serial_ - serial);
            f.pts = back < times_.size() ? times_[times_.size() - 1 - back] : 0;
            const bool high = (img->fmt & VPX_IMG_FMT_HIGHBITDEPTH) != 0;
            f.width = img->d_w;
            f.height = img->d_h;
            f.bit_depth = high ? img->bit_depth : 8;
            f.chroma_shift_x = img->x_chroma_shift;
            f.chroma_shift_y = img->y_chroma_shift;
            f.full_range = img->range == VPX_CR_FULL_RANGE;
            switch (img->cs) {
            case VPX_CS_BT_601: case VPX_CS_SMPTE_170: f.matrix = 6; f.primaries = 6; f.transfer = 6; break;
            case VPX_CS_BT_709: f.matrix = 1; f.primaries = 1; f.transfer = 1; break;
            case VPX_CS_SMPTE_240: f.matrix = 7; f.primaries = 7; f.transfer = 7; break;
            case VPX_CS_BT_2020: f.matrix = 9; f.primaries = 9; break;
            case VPX_CS_SRGB: f.matrix = 0; f.primaries = 1; f.transfer = 13; break;
            default: break;
            }
            const std::uint32_t bytes = high ? 2 : 1;
            for (int p = 0; p < 3; ++p) {
                const std::uint32_t w = p ? (img->d_w + img->x_chroma_shift) >> img->x_chroma_shift : img->d_w;
                const std::uint32_t h = p ? (img->d_h + img->y_chroma_shift) >> img->y_chroma_shift : img->d_h;
                copy_plane(f.planes[p], img->planes[p], img->stride[p], w, h, bytes);
            }
            ready_.push_back(std::move(f));
            while (times_.size() > 64) times_.pop_front();
        }
    }

    vpx_codec_ctx_t ctx_{};
    bool open_{}, vp9_{};
    std::uint64_t serial_{};
    std::deque<std::int64_t> times_;
    std::deque<VideoFrame> ready_;
};

// ---- AV1 (dav1d) ------------------------------------------------------------

class Dav1dDecoder final : public VideoDecoder {
public:
    bool open(std::span<const std::uint8_t> config, std::string& why) {
        // av1C: four bytes, then configuration OBUs (a sequence header), fed ahead of the first packet.
        if (config.size() > 4) prefix_.assign(config.begin() + 4, config.end());
        Dav1dSettings s;
        dav1d_default_settings(&s);
        s.n_threads = 1;
        s.max_frame_delay = 1;
        if (dav1d_open(&ctx_, &s) < 0) { why = "dav1d could not be opened"; return false; }
        pending_prefix_ = !prefix_.empty();
        return true;
    }
    ~Dav1dDecoder() override { if (ctx_) dav1d_close(&ctx_); }

    bool send(std::span<const std::uint8_t> packet, std::int64_t pts, std::string& why) override {
        std::vector<std::uint8_t> bytes;
        if (pending_prefix_) {
            bytes = prefix_;
            pending_prefix_ = false;
        }
        bytes.insert(bytes.end(), packet.begin(), packet.end());
        Dav1dData data{};
        std::uint8_t* buffer = dav1d_data_create(&data, bytes.size());
        if (!buffer) { why = "dav1d: out of memory"; return false; }
        std::memcpy(buffer, bytes.data(), bytes.size());
        data.m.timestamp = pts;
        while (data.sz > 0) {
            const int r = dav1d_send_data(ctx_, &data);
            if (r == 0) continue;
            if (r != DAV1D_ERR(EAGAIN)) {
                dav1d_data_unref(&data);
                why = "dav1d rejected the packet (" + std::to_string(r) + ")";
                return false;
            }
            if (!pull()) {   // the decoder wants its output taken first, and has none
                dav1d_data_unref(&data);
                why = "dav1d could not accept the packet";
                return false;
            }
        }
        while (pull()) {}
        return true;
    }

    bool receive(VideoFrame& out) override {
        if (ready_.empty()) return false;
        out = std::move(ready_.front());
        ready_.pop_front();
        return true;
    }

    void drain() override {
        while (pull()) {}
    }

    void reset() override {
        dav1d_flush(ctx_);
        ready_.clear();
        pending_prefix_ = !prefix_.empty();
    }

private:
    // Takes one picture; false when none is ready.
    bool pull() {
        Dav1dPicture pic{};
        if (dav1d_get_picture(ctx_, &pic) < 0) return false;
        VideoFrame f;
        f.pts = pic.m.timestamp;
        f.width = std::uint32_t(pic.p.w);
        f.height = std::uint32_t(pic.p.h);
        f.bit_depth = std::uint32_t(pic.p.bpc);
        f.monochrome = pic.p.layout == DAV1D_PIXEL_LAYOUT_I400;
        f.chroma_shift_x = pic.p.layout == DAV1D_PIXEL_LAYOUT_I420 || pic.p.layout == DAV1D_PIXEL_LAYOUT_I422;
        f.chroma_shift_y = pic.p.layout == DAV1D_PIXEL_LAYOUT_I420;
        if (pic.seq_hdr) {
            f.full_range = pic.seq_hdr->color_range != 0;
            f.primaries = std::uint8_t(pic.seq_hdr->pri);
            f.transfer = std::uint8_t(pic.seq_hdr->trc);
            f.matrix = std::uint8_t(pic.seq_hdr->mtrx);
        }
        const std::uint32_t bytes = pic.p.bpc > 8 ? 2 : 1;
        copy_plane(f.planes[0], static_cast<const std::uint8_t*>(pic.data[0]), pic.stride[0], f.width, f.height, bytes);
        if (!f.monochrome) {
            const std::uint32_t w = (f.width + f.chroma_shift_x) >> f.chroma_shift_x;
            const std::uint32_t h = (f.height + f.chroma_shift_y) >> f.chroma_shift_y;
            for (int p = 1; p < 3; ++p)
                copy_plane(f.planes[p], static_cast<const std::uint8_t*>(pic.data[p]), pic.stride[1], w, h, bytes);
        }
        dav1d_picture_unref(&pic);
        ready_.push_back(std::move(f));
        return true;
    }

    Dav1dContext* ctx_{};
    std::vector<std::uint8_t> prefix_;
    bool pending_prefix_{};
    std::deque<VideoFrame> ready_;
};

} // namespace

bool video_decoder_available(std::uint32_t codec) {
    return codec == fourcc("vp08") || codec == fourcc("vp09") || codec == fourcc("av01");
}

std::unique_ptr<VideoDecoder> make_video_decoder(std::uint32_t codec, std::span<const std::uint8_t> config, std::string& why) {
    if (codec == fourcc("vp08") || codec == fourcc("vp09")) {
        auto d = std::make_unique<VpxDecoder>();
        if (!d->open(codec == fourcc("vp09"), why)) return nullptr;
        return d;
    }
    if (codec == fourcc("av01")) {
        auto d = std::make_unique<Dav1dDecoder>();
        if (!d->open(config, why)) return nullptr;
        return d;
    }
    why = "no decoder for this video format";
    return nullptr;
}

bool audio_decoder_available(std::uint32_t codec) {
    if (codec == fourcc("mp4a") || codec == fourcc("aac ")) return aac_decoder_exists();
    return codec == fourcc(".mp1") || codec == fourcc(".mp2") || codec == fourcc(".mp3") || codec == fourcc("opus") ||
           codec == fourcc("flac") || codec == fourcc("alac");
}

bool audio_encoder_available(std::uint32_t codec) { return codec == fourcc("alac"); }

std::unique_ptr<AudioEncoder> make_audio_encoder(std::uint32_t codec, double rate, std::uint32_t channels, std::uint32_t bits,
                                                 std::uint32_t frames_per_packet, std::string& why) {
    if (codec == fourcc("alac")) {
        auto e = std::make_unique<AlacEncoder>();
        if (!e->open(rate, channels, bits, frames_per_packet, why)) return nullptr;
        return e;
    }
    why = "no encoder for this audio format";
    return nullptr;
}

std::unique_ptr<AudioDecoder> make_audio_decoder(std::uint32_t codec, double rate, std::uint32_t channels,
                                                 std::span<const std::uint8_t> config, std::string& why) {
    if (codec == fourcc(".mp1") || codec == fourcc(".mp2") || codec == fourcc(".mp3"))
        return std::make_unique<MpegDecoder>(rate, channels);
    if (codec == fourcc("opus")) {
        auto d = std::make_unique<OpusDecoder>();
        if (!d->open(config, channels, why)) return nullptr;
        return d;
    }
    if (codec == fourcc("flac")) {
        auto d = std::make_unique<FlacDecoder>();
        if (!d->open(config, why)) return nullptr;
        return d;
    }
    if (codec == fourcc("alac")) {
        auto d = std::make_unique<AlacDecoder>();
        if (!d->open(config, rate, why)) return nullptr;
        return d;
    }
    if (codec == fourcc("mp4a") || codec == fourcc("aac ")) {
        auto d = std::make_unique<AacDecoder>();
        if (!d->open(config, rate, channels, why)) return nullptr;
        return d;
    }
    why = "no decoder for this audio format";
    return nullptr;
}

} // namespace alchemy::gui::completion_kit::media::codecs
