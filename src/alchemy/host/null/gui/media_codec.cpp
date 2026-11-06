// Null host backend, area "media_codec": no encoders; every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/media_codec.hpp"

namespace alchemy::host::media_codec {

namespace {
const Component none{};
} // namespace

Result<std::vector<Component>> encoders(Codec) { return Error::unsupported; }

Result<VideoEncoder*> open_video(const VideoSettings&) { return Error::unsupported; }
const Component& component(const VideoEncoder*) noexcept { return none; }
Status encode(VideoEncoder*, const Picture&, bool, std::vector<Sample>&) { return Error::unsupported; }
Status drain(VideoEncoder*, std::vector<Sample>&) { return Error::unsupported; }
std::vector<std::vector<std::uint8_t>> parameter_sets(const VideoEncoder*) { return {}; }
void close(VideoEncoder*) noexcept {}

Result<AudioEncoder*> open_audio(const AudioSettings&) { return Error::unsupported; }
const Component& component(const AudioEncoder*) noexcept { return none; }
std::uint32_t bit_rate(const AudioEncoder*) noexcept { return 0; }
std::uint32_t frames_per_packet(const AudioEncoder*) noexcept { return 0; }
std::vector<std::uint8_t> configuration(const AudioEncoder*) { return {}; }
Status encode(AudioEncoder*, std::span<const std::int16_t>, std::vector<Sample>&) { return Error::unsupported; }
Status drain(AudioEncoder*, std::vector<Sample>&) { return Error::unsupported; }
void close(AudioEncoder*) noexcept {}
Result<std::uint32_t> measure_delay(const AudioSettings&) { return Error::unsupported; }

} // namespace alchemy::host::media_codec
