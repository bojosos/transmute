#pragma once

// In-process access to the CoreMIDI server of coremidi.cpp (QLab group G5):
// the MusicPlayer sends its sequence's events to a MIDI destination through
// an internal client and output port, as CoreMIDI clients do.

#include "alchemy/darwin/media/midi_core.hpp"

namespace alchemy::gui::completion_kit::media {

// MIDISend to `destination` from the internal "MusicPlayer" port (packets
// with future host times are held until due). An OSStatus.
std::int32_t midi_send_internal(midi::ObjectRef destination, std::vector<midi::Packet> packets);
// MIDIFlushOutput(destination).
std::int32_t midi_flush_internal(midi::ObjectRef destination);
// Whether `object` is a MIDI destination endpoint.
bool midi_is_destination(midi::ObjectRef object);

} // namespace alchemy::gui::completion_kit::media
