#include "alchemy/darwin/media/video_encoder.hpp"

#include "alchemy/host/media_codec.hpp"

#include <mutex>

namespace alchemy::gui::completion_kit::media::video_encoding {

namespace {

namespace mc = alchemy::host::media_codec;

class HostEncoder final : public Encoder {
public:
    HostEncoder(mc::VideoEncoder* encoder, EncoderInfo info) : encoder_(encoder), info_(std::move(info)) {}
    ~HostEncoder() override { mc::close(encoder_); }
    bool encode(const Picture& p, bool keyframe, std::vector<Packet>& out) override {
        const mc::Picture picture{p.width, p.height, p.luma, p.luma_stride, p.chroma, p.chroma_stride, p.pts, p.duration};
        std::vector<mc::Sample> samples;
        const bool ok = static_cast<bool>(mc::encode(encoder_, picture, keyframe, samples));
        take(samples, out);
        return ok;
    }
    bool drain(std::vector<Packet>& out) override {
        std::vector<mc::Sample> samples;
        const bool ok = static_cast<bool>(mc::drain(encoder_, samples));
        take(samples, out);
        return ok;
    }
    std::vector<std::vector<std::uint8_t>> parameter_sets() const override { return mc::parameter_sets(encoder_); }
    const EncoderInfo& info() const override { return info_; }

private:
    static void take(std::vector<mc::Sample>& samples, std::vector<Packet>& out) {
        for (auto& s : samples) out.push_back({std::move(s.data), s.pts, s.dts, s.duration, s.keyframe});
    }
    mc::VideoEncoder* encoder_;
    EncoderInfo info_;
};

std::mutex mutex;
Backend replacement;

// The host's encoders with their Alchemy IDs, and the host component each stands for.
struct Listed {
    EncoderInfo info;
    std::string component;
};
std::vector<Listed> host_encoders() {
    std::vector<Listed> out;
    for (const auto codec : {Codec::h264, Codec::hevc}) {
        const auto listed = mc::encoders(codec == Codec::h264 ? mc::Codec::h264 : mc::Codec::hevc);
        if (!listed) continue;
        std::size_t n = 0;
        for (const auto& c : *listed)
            out.push_back({{"com.alchemy.mediafoundation." + std::string(codec == Codec::h264 ? "h264." : "hevc.") + std::to_string(n++), c.name, codec,
                            c.hardware},
                           c.id});
    }
    return out;
}

mc::Profile host_profile(Profile p) {
    switch (p) {
    case Profile::h264_baseline: return mc::Profile::h264_baseline;
    case Profile::h264_main: return mc::Profile::h264_main;
    case Profile::h264_high: return mc::Profile::h264_high;
    case Profile::hevc_main: return mc::Profile::hevc_main;
    case Profile::hevc_main10: return mc::Profile::hevc_main10;
    default: return mc::Profile::automatic;
    }
}

} // namespace

void set_backend(Backend backend) {
    std::lock_guard lock(mutex);
    replacement = std::move(backend);
}

std::vector<EncoderInfo> encoders() {
    {
        std::lock_guard lock(mutex);
        if (replacement.encoders) return replacement.encoders();
    }
    std::vector<EncoderInfo> out;
    for (auto& l : host_encoders()) out.push_back(std::move(l.info));
    return out;
}

std::unique_ptr<Encoder> open(const Settings& settings, OpenError& error) {
    error = OpenError::none;
    {
        std::lock_guard lock(mutex);
        if (replacement.open) return replacement.open(settings, error);
    }
    // The candidates: the named encoder, or every encoder of the codec (hardware ones only when required).
    std::vector<Listed> candidates;
    for (auto& l : host_encoders()) {
        if (l.info.codec != settings.codec) continue;
        if (!settings.encoder_id.empty() && l.info.id != settings.encoder_id) continue;
        if ((settings.require_hardware && !l.info.hardware) || (settings.require_software && l.info.hardware)) continue;
        candidates.push_back(std::move(l));
    }
    if (candidates.empty()) {
        error = OpenError::not_found;
        return nullptr;
    }
    mc::VideoSettings s;
    s.codec = settings.codec == Codec::h264 ? mc::Codec::h264 : mc::Codec::hevc;
    s.width = settings.width;
    s.height = settings.height;
    s.frame_rate = settings.frame_rate;
    s.bit_rate = settings.bit_rate;
    s.keyframe_interval = settings.keyframe_interval;
    s.profile = host_profile(settings.profile);
    s.level = settings.level;
    s.real_time = settings.real_time;
    s.frame_reordering = settings.frame_reordering;
    s.require_hardware = settings.require_hardware;
    for (auto& c : candidates) {
        s.component = c.component;
        if (const auto opened = mc::open_video(s)) return std::make_unique<HostEncoder>(*opened, std::move(c.info));
    }
    error = OpenError::invalid_settings;
    return nullptr;
}

} // namespace alchemy::gui::completion_kit::media::video_encoding
