// Linux host backend (not written yet), area "media_codec": no encoders or decoders; every call fails with Error::unsupported
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
Result<std::vector<Component>> decoders(Codec) { return Error::unsupported; }
Result<VideoDecoder*> open_video_decoder(const VideoDecoderSettings&) { return Error::unsupported; }
const Component& component(const VideoDecoder*) noexcept { return none; }
Status decode(VideoDecoder*, const Sample&, std::vector<DecodedPicture>&) { return Error::unsupported; }
Status drain(VideoDecoder*, std::vector<DecodedPicture>&) { return Error::unsupported; }
void flush(VideoDecoder*) noexcept {}
void close(VideoDecoder*) noexcept {}
Result<AudioDecoder*> open_audio_decoder(const AudioDecoderSettings&) { return Error::unsupported; }
const Component& component(const AudioDecoder*) noexcept { return none; }
Status decode(AudioDecoder*, std::span<const std::uint8_t>, std::vector<std::int16_t>&) { return Error::unsupported; }
Status drain(AudioDecoder*, std::vector<std::int16_t>&) { return Error::unsupported; }
void flush(AudioDecoder*) noexcept {}
void close(AudioDecoder*) noexcept {}

} // namespace alchemy::host::media_codec
