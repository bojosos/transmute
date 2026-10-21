#pragma once

// QLab gap group G5 (media): installers of src/alchemy/darwin/kit/completion/media/ and
// the hooks its tests use. Group-internal header.
//   coremidi.cpp      CoreMIDI over the MIDI core (midi_core.cpp) and host::midi (midi_host.cpp)
//   hal.cpp           CoreAudio HAL devices over host::audio (hal_core.cpp, hal_host.cpp)
//   music.cpp         AudioToolbox MusicSequence/MusicTrack/MusicEventIterator/MusicPlayer (music_core.cpp)
//   audio_file.cpp    AudioToolbox AudioFile and ExtAudioFile (audio_file_core.cpp: WAV/AIFF/CAF PCM)
//   au_listeners.cpp  AUListener/AUEventListener, AUParameterSet/Notify, AudioUnit gaps
//   avfoundation.cpp  AVAsset/AVURLAsset/AVAssetReader, AVAudioFormat/AVAudioChannelLayout,
//                     AVAudioUnit, capture discovery, AVMetadataItem, AUGenericView
//   cmtime.cpp        CMTime arithmetic (core; bound by src/alchemy/darwin/media/coremedia.cpp)

#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <functional>
#include <memory>

namespace alchemy::gui::completion_kit::media {

namespace midi { class Backend; }
namespace hal { class Backend; }

void install_core_midi(Support& support);
void install_hal(Support& support);
void install_music(Support& support);
void install_audio_file(Support& support);
void install_au_listeners(Support& support);
void install_av_foundation(Support& support);

// Host time (mach_absolute_time: nanoseconds of unbiased interrupt time).
std::uint64_t host_time_now();

// Test hooks: the device backends the next process uses (default: the host's,
// host::midi and host::audio; ALCHEMY_MIDI_DEVICES=none / ALCHEMY_AUDIO_DEVICES=none select an
// empty device list).
void set_midi_backend_factory(std::function<std::unique_ptr<midi::Backend>()> factory);
void set_hal_backend_factory(std::function<std::unique_ptr<hal::Backend>()> factory);

} // namespace alchemy::gui::completion_kit::media
