#include "alchemy/darwin/media/aac_encoder.hpp"

#include "alchemy/host/media_codec.hpp"

#include <algorithm>
#include <mutex>
#include <optional>

namespace alchemy::gui::completion_kit::media::aac {

namespace {

namespace mc = alchemy::host::media_codec;

class HostEncoder final : public Encoder {
public:
    explicit HostEncoder(mc::AudioEncoder* encoder) : encoder_(encoder) {}
    ~HostEncoder() override { mc::close(encoder_); }
    bool encode(std::span<const std::int16_t> samples, std::vector<std::vector<std::uint8_t>>& out) override {
        std::vector<mc::Sample> packets;
        const bool ok = static_cast<bool>(mc::encode(encoder_, samples, packets));
        for (auto& p : packets) out.push_back(std::move(p.data));
        return ok;
    }
    bool drain(std::vector<std::vector<std::uint8_t>>& out) override {
        std::vector<mc::Sample> packets;
        const bool ok = static_cast<bool>(mc::drain(encoder_, packets));
        for (auto& p : packets) out.push_back(std::move(p.data));
        return ok;
    }
    std::vector<std::uint8_t> audio_specific_config() const override { return mc::configuration(encoder_); }
    std::uint32_t bit_rate() const override { return mc::bit_rate(encoder_); }
    // The component's recorded delay (measured by the host layer; encoders whose delay is not known are not used).
    std::uint32_t priming() const override { return mc::component(encoder_).delay; }

private:
    mc::AudioEncoder* encoder_;
};

std::mutex mutex;
Backend replacement;
std::optional<Capabilities> host_capabilities;

Capabilities host_capabilities_now() {
    std::lock_guard lock(mutex);
    if (!host_capabilities) {
        Capabilities c;
        if (const auto listed = mc::encoders(mc::Codec::aac)) {
            const auto merge = [](std::vector<std::uint32_t>& into, const std::vector<std::uint32_t>& from) {
                for (const auto v : from)
                    if (std::find(into.begin(), into.end(), v) == into.end()) into.push_back(v);
                std::sort(into.begin(), into.end());
            };
            for (const auto& component : *listed) {
                if (!component.delay) continue;   // without a known delay its streams could not be trimmed
                merge(c.sample_rates, component.sample_rates);
                merge(c.channel_counts, component.channel_counts);
                merge(c.bit_rates, component.bit_rates);
            }
        }
        host_capabilities = std::move(c);
    }
    return *host_capabilities;
}

// An expandable-size descriptor (ISO/IEC 14496-1, 8.3.3): the tag, the size in 7-bit groups, the body.
void descriptor(std::vector<std::uint8_t>& out, std::uint8_t tag, const std::vector<std::uint8_t>& body) {
    out.push_back(tag);
    const auto size = static_cast<std::uint32_t>(body.size());
    for (int shift = 21; shift > 0; shift -= 7) out.push_back(static_cast<std::uint8_t>(0x80 | ((size >> shift) & 0x7f)));
    out.push_back(static_cast<std::uint8_t>(size & 0x7f));
    out.insert(out.end(), body.begin(), body.end());
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

} // namespace

void set_backend(Backend backend) {
    std::lock_guard lock(mutex);
    replacement = std::move(backend);
}

Capabilities capabilities() {
    {
        std::lock_guard lock(mutex);
        if (replacement.capabilities) return replacement.capabilities();
    }
    return host_capabilities_now();
}

bool available() { return !capabilities().empty(); }

std::unique_ptr<Encoder> open(std::uint32_t rate, std::uint32_t channels, std::uint32_t bit_rate, std::string& why) {
    {
        std::lock_guard lock(mutex);
        if (replacement.open) return replacement.open(rate, channels, bit_rate, why);
    }
    // The first encoder with a known delay that takes the rate and channels.
    alchemy::host::Result<mc::AudioEncoder*> opened = alchemy::host::Error::not_found;
    if (const auto listed = mc::encoders(mc::Codec::aac))
        for (const auto& component : *listed) {
            if (!component.delay) continue;
            opened = mc::open_audio({mc::Codec::aac, rate, channels, bit_rate, component.id});
            if (opened) break;
        }
    if (!opened) {
        why = opened.error() == alchemy::host::Error::not_found || opened.error() == alchemy::host::Error::unsupported ? "no AAC encoder exists on this host"
                                                                                                    : "no AAC encoder takes this sample rate and channel count";
        return nullptr;
    }
    return std::make_unique<HostEncoder>(*opened);
}

std::vector<std::uint8_t> es_descriptor(std::span<const std::uint8_t> audio_specific_config, std::uint32_t average_bit_rate,
                                        std::uint32_t maximum_bit_rate, std::uint32_t buffer_bytes) {
    // DecoderConfigDescriptor: objectTypeIndication 0x40 (ISO/IEC 14496-3 audio), streamType 5 (audio) with
    // upStream 0 and the reserved bit, the buffer size, the maximum and average bit rates, and the
    // DecoderSpecificInfo (the AudioSpecificConfig).
    std::vector<std::uint8_t> config{0x40, (0x05 << 2) | 1, static_cast<std::uint8_t>(buffer_bytes >> 16),
                                     static_cast<std::uint8_t>(buffer_bytes >> 8), static_cast<std::uint8_t>(buffer_bytes)};
    put32(config, maximum_bit_rate);
    put32(config, average_bit_rate);
    descriptor(config, 0x05, std::vector<std::uint8_t>(audio_specific_config.begin(), audio_specific_config.end()));
    // ES_Descriptor: ES_ID 0, no stream dependence, URL or OCR stream; then the SLConfigDescriptor (predefined 2: MP4).
    std::vector<std::uint8_t> es{0, 0, 0};
    descriptor(es, 0x04, config);
    descriptor(es, 0x06, {0x02});
    std::vector<std::uint8_t> out;
    descriptor(out, 0x03, es);
    return out;
}

} // namespace alchemy::gui::completion_kit::media::aac
