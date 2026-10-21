#pragma once

#include "alchemy/darwin/media/midi_core.hpp"

namespace alchemy::gui::completion_kit::media::midi {

// The host's MIDI devices (host::midi).
std::unique_ptr<Backend> make_host_backend();

} // namespace alchemy::gui::completion_kit::media::midi
