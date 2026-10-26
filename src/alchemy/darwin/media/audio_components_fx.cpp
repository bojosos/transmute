#include "alchemy/darwin/headless_additions.hpp"

// alchemy-fx: AudioComponent and the AudioUnit calls are the output audio
// units' (audio_units.cpp); the headless answer (audio_components.cpp) is
// alchemy.exe's only.
namespace alchemy::darwin_headless {

void install_media_audio_components(ProcessContext&, ObjcRuntime&, CoreFoundation&) {}

} // namespace alchemy::darwin_headless
