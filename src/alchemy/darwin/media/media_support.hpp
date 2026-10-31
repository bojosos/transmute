#pragma once

// QLab gap group G5 (media): installers of src/alchemy/darwin/kit/completion/media/ and
// the hooks its tests use. Group-internal header.
//   coremidi.cpp      CoreMIDI over the MIDI core (midi_core.cpp) and host::midi (midi_host.cpp)
//   hal.cpp           CoreAudio HAL devices over host::audio (hal_core.cpp, hal_host.cpp)
//   music.cpp         AudioToolbox MusicSequence/MusicTrack/MusicEventIterator/MusicPlayer (music_core.cpp)
//   audio_file.cpp    AudioToolbox AudioFile and ExtAudioFile (audio_file_core.cpp: WAV/AIFF/CAF PCM)
//   au_listeners.cpp  AUListener/AUEventListener, AUParameterSet/Notify, AudioUnit gaps
//   avfoundation.cpp  AVAsset/AVURLAsset/AVAssetReader, AVAudioFormat/AVAudioChannelLayout,
//                     AVAudioUnit, AVMetadataItem, AUGenericView
//   av_capture.cpp    AVFoundation capture over host::camera (capture_host.cpp) and the HAL's inputs
//   avaudio_*.cpp     AVFAudio: AVAudioTime/PCMBuffer/File, AVAudioPlayer, AVAudioEngine (avaudio.hpp)
//   cmtime.cpp        CMTime arithmetic (core; bound by src/alchemy/darwin/media/coremedia.cpp)

#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::media {

namespace midi { class Backend; }
namespace hal { class Backend; class InputStream; }
namespace capture { class Backend; }
namespace hal_io { class DeviceIo; }

void install_core_midi(Support& support);
void install_hal(Support& support);
void install_audio_units(Support& support);   // audio_units.cpp: AudioComponent and the output units
void install_audio_converter(Support& support);   // audio_converter.cpp: AudioConverter (linear PCM)
void install_audio_queue(Support& support);   // audio_queue.cpp: AudioQueue output
void install_audio_format(Support& support);   // audio_format.cpp: AudioFormatGetProperty
void install_music(Support& support);
void install_audio_file(Support& support);
void install_au_listeners(Support& support);
void install_av_foundation(Support& support);
// av_player.cpp: AVPlayer, AVPlayerItem, AVPlayerItemVideoOutput, AVPlayerLayer (from install_av_foundation).
void install_av_player(Support& support);
// av_sample_buffer.cpp: AVSampleBufferDisplayLayer, AVSampleBufferAudioRenderer, AVSampleBufferRenderSynchronizer.
void install_av_sample_buffer_renderers(Support& support);
// av_capture.cpp: AVCaptureDevice, discovery, sessions, device inputs and the video and audio data
// outputs over the capture backend (capture_core.hpp) and the HAL's input devices (from install_av_foundation).
void install_av_capture(Support& support);

// Host time (mach_absolute_time: nanoseconds of unbiased interrupt time).
std::uint64_t host_time_now();

// The CoreAudio HAL of the process (hal.cpp), for the output audio units: an output device's
// I/O (nullptr for an input device or none), the default output device (0: none) and the
// device's I/O buffer frame size (kAudioDevicePropertyBufferFrameSize).
std::shared_ptr<hal_io::DeviceIo> hal_device_io(std::uint32_t device);
std::uint32_t hal_default_output();
std::uint32_t hal_buffer_frames(std::uint32_t device);
std::int32_t hal_set_buffer_frames(std::uint32_t device, std::uint32_t frames);
// The input devices (in the HAL's order), the default input device (0: none) and an input stream on
// one, capturing interleaved float frames in its rate and channels (nullptr: none, or refused).
struct HalInputDevice { std::uint32_t id{}; std::string uid, name; std::uint32_t channels{}; double rate{}; };
std::vector<HalInputDevice> hal_input_devices();
std::uint32_t hal_default_input();
std::unique_ptr<hal::InputStream> hal_open_input(std::uint32_t device,
                                                 std::function<void(const float* samples, std::uint32_t frames, std::uint64_t time_ns)> capture);

// Test hooks: the device backends the next process uses (default: the host's,
// host::midi and host::audio; ALCHEMY_MIDI_DEVICES=none / ALCHEMY_AUDIO_DEVICES=none select an
// empty device list).
void set_midi_backend_factory(std::function<std::unique_ptr<midi::Backend>()> factory);
void set_hal_backend_factory(std::function<std::unique_ptr<hal::Backend>()> factory);
// The cameras the next process uses (default: the host's, host::camera).
void set_capture_backend_factory(std::function<std::unique_ptr<capture::Backend>()> factory);

} // namespace alchemy::gui::completion_kit::media
