#include "alchemy/darwin/media/media.hpp"

// Audio and MIDI gaps: none are left here.
//
// - CoreAudio HAL and CoreMIDI are the QLab media group's
//   (src/alchemy/darwin/kit/completion/media: hal.cpp over WASAPI, coremidi.cpp over
//   winmm), installed earlier by install_completion_kit.
// - AudioToolbox: AudioConverter, AudioQueue (output and input) and AudioFormat properties are
//   audio_converter.cpp's, audio_queue.cpp's and audio_format.cpp's.
// - AudioComponent and the output audio units (AUHAL input included) are audio_units.cpp's.
namespace alchemy::gui::fill {

void install_audio(InstallContext&) {}

} // namespace alchemy::gui::fill
