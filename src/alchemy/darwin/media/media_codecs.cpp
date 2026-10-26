#include "alchemy/darwin/media/media_codecs.hpp"

#include "alchemy/darwin/media/media_container.hpp"

#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"
#include "opus_multistream.h"
#include "FLAC/stream_decoder.h"
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#include "dav1d/dav1d.h"

#include <algorithm>
#include <cerrno>
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
    return codec == fourcc(".mp1") || codec == fourcc(".mp2") || codec == fourcc(".mp3") || codec == fourcc("opus") ||
           codec == fourcc("flac");
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
    why = "no decoder for this audio format";
    return nullptr;
}

} // namespace alchemy::gui::completion_kit::media::codecs
