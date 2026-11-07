#pragma once

// A deterministic stand-in for the host's video encoders (video_encoder.hpp) for the media tests: a software H.264
// encoder, a hardware H.264 encoder and a software HEVC encoder (none with `none`). Each picture gives one sample:
// a single NAL unit (IDR or not) whose payload is the picture's number (4 bytes, little endian) and its mean luma,
// Cb and Cr. With frame reordering each pair of pictures after a keyframe comes out swapped (the later one first,
// as a P frame before its B frame), with decode times in order. The parameter sets are real ones for the size,
// profile and level asked for (what CoreMedia parses), made after the first keyframe. `last` is the settings of
// the last encoder opened.

#include "alchemy/darwin/media/video_encoder.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace fake_video {

namespace enc = alchemy::gui::completion_kit::media::video_encoding;

inline enc::Settings last;
inline int opened = 0;

// Exp-Golomb bit writing, then emulation prevention after the NAL header.
struct Writer {
    std::vector<std::uint8_t> bytes;
    unsigned bits{};
    void bit(unsigned b) {
        if (bits % 8 == 0) bytes.push_back(0);
        if (b) bytes.back() |= static_cast<std::uint8_t>(0x80 >> (bits % 8));
        ++bits;
    }
    void put(std::uint64_t value, unsigned n) { for (unsigned i = n; i-- > 0;) bit((value >> i) & 1); }
    void ue(std::uint32_t value) {
        const std::uint64_t v = std::uint64_t(value) + 1;
        unsigned length = 0;
        while ((v >> length) > 1) ++length;
        put(0, length);
        put(v, length + 1);
    }
    std::vector<std::uint8_t> nal(std::vector<std::uint8_t> header) {
        bit(1);   // rbsp_stop_one_bit, then zero alignment
        for (std::size_t i = 0, zeros = 0; i < bytes.size(); ++i) {
            if (zeros >= 2 && bytes[i] <= 3) { header.push_back(3); zeros = 0; }
            header.push_back(bytes[i]);
            zeros = bytes[i] == 0 ? zeros + 1 : 0;
        }
        return header;
    }
};

inline std::vector<std::uint8_t> h264_sps(std::uint32_t width, std::uint32_t height, std::uint32_t profile, std::uint32_t level) {
    Writer w;
    w.put(profile, 8);
    w.put(0, 8);   // constraint flags
    w.put(level, 8);
    w.ue(0);       // seq_parameter_set_id
    if (profile == 100) { w.ue(1); w.ue(0); w.ue(0); w.bit(0); w.bit(0); }   // 4:2:0, 8 bits, no scaling matrices
    w.ue(0);       // log2_max_frame_num_minus4
    w.ue(0);       // pic_order_cnt_type
    w.ue(0);       // log2_max_pic_order_cnt_lsb_minus4
    w.ue(2);       // max_num_ref_frames
    w.bit(0);      // gaps_in_frame_num_value_allowed_flag
    const auto mbs_w = (width + 15) / 16, mbs_h = (height + 15) / 16;
    w.ue(mbs_w - 1);
    w.ue(mbs_h - 1);
    w.bit(1);      // frame_mbs_only_flag
    w.bit(1);      // direct_8x8_inference_flag
    const auto crop_right = (mbs_w * 16 - width) / 2, crop_bottom = (mbs_h * 16 - height) / 2;
    w.bit(crop_right || crop_bottom ? 1 : 0);
    if (crop_right || crop_bottom) { w.ue(0); w.ue(crop_right); w.ue(0); w.ue(crop_bottom); }
    w.bit(0);      // vui_parameters_present_flag
    return w.nal({0x67});
}

inline std::vector<std::uint8_t> hevc_sps(std::uint32_t width, std::uint32_t height, std::uint32_t level) {
    Writer w;
    w.put(0, 4);              // sps_video_parameter_set_id
    w.put(0, 3);              // sps_max_sub_layers_minus1
    w.bit(1);                 // sps_temporal_id_nesting_flag
    w.put(0, 2); w.bit(0); w.put(1, 5);   // general profile space, tier, profile_idc 1 (Main)
    w.put(0x60000000, 32);    // general_profile_compatibility_flags (Main, Main 10)
    w.put(0x900000000000ULL, 48);   // progressive source, frame only
    w.put(level, 8);
    w.ue(0);                  // sps_seq_parameter_set_id
    w.ue(1);                  // chroma_format_idc 4:2:0
    const auto aligned_w = (width + 7) / 8 * 8, aligned_h = (height + 7) / 8 * 8;
    w.ue(aligned_w);
    w.ue(aligned_h);
    const auto right = (aligned_w - width) / 2, bottom = (aligned_h - height) / 2;
    w.bit(right || bottom ? 1 : 0);
    if (right || bottom) { w.ue(0); w.ue(right); w.ue(0); w.ue(bottom); }
    w.ue(0); w.ue(0);         // bit depths 8
    return w.nal({0x42, 0x01});
}

class Encoder final : public enc::Encoder {
public:
    Encoder(const enc::Settings& s, enc::EncoderInfo info) : settings_(s), info_(std::move(info)) {}
    bool encode(const enc::Picture& p, bool keyframe, std::vector<enc::Packet>& out) override {
        if (p.width != settings_.width || p.height != settings_.height || !p.luma || !p.chroma) return false;
        const bool key = keyframe || count_ == 0 || (settings_.keyframe_interval && count_ - last_key_ >= settings_.keyframe_interval);
        if (key) last_key_ = count_;
        auto packet = make(p, key);
        ++count_;
        input_.push_back(p.pts);
        if (!settings_.frame_reordering || key) {
            if (held_) emit(std::move(*std::exchange(held_, std::nullopt)), out);
            emit(std::move(packet), out);
        } else if (held_) {
            emit(std::move(packet), out);   // the later picture first (P), then the held one (B)
            emit(std::move(*std::exchange(held_, std::nullopt)), out);
        } else {
            held_ = std::move(packet);
        }
        if (key && sets_.empty()) sets_ = parameter_sets_now();
        return true;
    }
    bool drain(std::vector<enc::Packet>& out) override {
        if (held_) emit(std::move(*std::exchange(held_, std::nullopt)), out);
        return true;
    }
    std::vector<std::vector<std::uint8_t>> parameter_sets() const override { return sets_; }
    const enc::EncoderInfo& info() const override { return info_; }

private:
    enc::Packet make(const enc::Picture& p, bool key) const {
        double y = 0, cb = 0, cr = 0;
        for (std::uint32_t row = 0; row < p.height; ++row)
            for (std::uint32_t x = 0; x < p.width; ++x) y += p.luma[row * p.luma_stride + x];
        for (std::uint32_t row = 0; row < p.height / 2; ++row)
            for (std::uint32_t x = 0; x < p.width / 2; ++x) { cb += p.chroma[row * p.chroma_stride + 2 * x]; cr += p.chroma[row * p.chroma_stride + 2 * x + 1]; }
        const double n = double(p.width) * p.height, c = n / 4;
        std::vector<std::uint8_t> nal;
        if (settings_.codec == enc::Codec::h264) nal = {static_cast<std::uint8_t>(key ? 0x65 : 0x41)};
        else nal = {static_cast<std::uint8_t>(key ? 19 << 1 : 1 << 1), 0x01};
        for (int i = 0; i < 4; ++i) nal.push_back(static_cast<std::uint8_t>(count_ >> (8 * i)));
        nal.push_back(static_cast<std::uint8_t>(y / n + 0.5));
        nal.push_back(static_cast<std::uint8_t>(cb / c + 0.5));
        nal.push_back(static_cast<std::uint8_t>(cr / c + 0.5));
        enc::Packet packet;
        const auto size = nal.size();
        packet.data = {0, 0, static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
        packet.data.insert(packet.data.end(), nal.begin(), nal.end());
        packet.pts = p.pts;
        packet.duration = p.duration;
        packet.keyframe = key;
        return packet;
    }
    // Decode times: the input times in order, one frame earlier when frames are reordered.
    void emit(enc::Packet packet, std::vector<enc::Packet>& out) {
        const auto shift = settings_.frame_reordering ? static_cast<std::int64_t>(1e7 / settings_.frame_rate) : 0;
        packet.dts = input_.at(emitted_++) - shift;
        out.push_back(std::move(packet));
    }
    std::vector<std::vector<std::uint8_t>> parameter_sets_now() const {
        if (settings_.codec == enc::Codec::h264) {
            const std::uint32_t profile = settings_.profile == enc::Profile::h264_baseline ? 66 : settings_.profile == enc::Profile::h264_high ? 100 : 77;
            return {h264_sps(settings_.width, settings_.height, profile, settings_.level ? settings_.level : 30), {0x68, 0xce, 0x38, 0x80}};
        }
        return {{0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60}, hevc_sps(settings_.width, settings_.height, settings_.level ? settings_.level : 93),
                {0x44, 0x01, 0xc1, 0x72, 0xb4, 0x62, 0x40}};
    }
    enc::Settings settings_;
    enc::EncoderInfo info_;
    std::uint32_t count_{}, last_key_{};
    std::size_t emitted_{};
    std::vector<std::int64_t> input_;
    std::optional<enc::Packet> held_;
    std::vector<std::vector<std::uint8_t>> sets_;
};

inline std::vector<enc::EncoderInfo> listed(bool none) {
    if (none) return {};
    return {{"com.alchemy.mediafoundation.h264.0", "Fake H.264 encoder", enc::Codec::h264, false},
            {"com.alchemy.mediafoundation.h264.1", "Fake H.264 hardware encoder", enc::Codec::h264, true},
            {"com.alchemy.mediafoundation.hevc.0", "Fake HEVC encoder", enc::Codec::hevc, false}};
}

inline void install(bool none = false) {
    enc::Backend backend;
    backend.encoders = [none] { return listed(none); };
    backend.open = [none](const enc::Settings& s, enc::OpenError& error) -> std::unique_ptr<enc::Encoder> {
        for (auto& info : listed(none)) {
            if (info.codec != s.codec || (!s.encoder_id.empty() && info.id != s.encoder_id) || (s.require_hardware && !info.hardware)) continue;
            if (!s.width || !s.height || (s.width & 1) || (s.height & 1) || s.width > 4096 || s.height > 4096) { error = enc::OpenError::invalid_settings; return nullptr; }
            last = s;
            ++opened;
            return std::make_unique<Encoder>(s, std::move(info));
        }
        error = enc::OpenError::not_found;
        return nullptr;
    };
    enc::set_backend(std::move(backend));
}

} // namespace fake_video
