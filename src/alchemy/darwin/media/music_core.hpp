#pragma once

// AudioToolbox MusicSequence core (host-only, QLab gap group G5): tracks of
// timed events, the tempo map, bar/beat conversion, and Standard MIDI File
// reading and writing. music.cpp binds it (and the MusicPlayer) to the guest.
//
// Semantics follow <AudioToolbox/MusicPlayer.h>:
// - times are MusicTimeStamp beats (quarter notes); events keep time order,
//   events at the same time keep insertion order;
// - the tempo track holds ExtendedTempo events (and the time-signature and
//   other conductor meta events); the tempo is 120 BPM before the first
//   tempo event; the time signature is 4/4 before the first one;
// - event payloads are the C structures' bytes (MIDINoteMessage,
//   MIDIChannelMessage, MIDIRawData, MIDIMetaEvent, ExtendedNoteOnEvent,
//   ExtendedTempoEvent, MusicEventUserData, ParameterEvent, AUPresetEvent);
// - SMF loading: format 0 and 1 files with PPQ division. Note on/off pairs
//   become MIDINoteMessages (duration in beats, release velocity); other
//   channel messages MIDIChannelMessages; SysEx MIDIRawData (with its F0);
//   tempo meta events ExtendedTempo events in the tempo track; other meta
//   events MIDIMetaEvents (time signature, key signature, SMPTE offset and
//   markers to the tempo track for format 1's first track). The end-of-track
//   time sets the track length. kMusicSequenceLoadSMF_ChannelsToTracks splits
//   channel events into one track per channel (in channel order) with meta
//   and SysEx events in the last track, as Apple documents.
// - SMF writing: format 1 at the given resolution (480 when 0): the tempo
//   track first, then each track. User, parameter, AU preset and extended
//   note events have no SMF form and are left out.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::media::music {

// MusicEventType
enum class EventType : std::uint32_t {
    null = 0, extended_note = 1, extended_tempo = 3, user = 4, meta = 5, midi_note = 6, midi_channel = 7,
    midi_raw = 8, parameter = 9, au_preset = 10,
};

namespace error {
inline constexpr std::int32_t param = -50, invalid_sequence_type = -10846, track_index = -10859, track_not_found = -10858,
    end_of_track = -10857, start_of_track = -10856, illegal_track_destination = -10855, no_sequence = -10854, invalid_event_type = -10853,
    invalid_player_state = -10852, cannot_do_in_current_context = -10863, no_track_destination = -66720,
    invalid_file = static_cast<std::int32_t>(0x64746121);   // kAudioFileInvalidFileError 'dta!'
}

struct Event {
    double time{};
    EventType type{};
    std::vector<std::uint8_t> data;   // the C structure's bytes
    std::uint64_t guest{};            // guest copy of `data` (managed by the binding)
};

struct Track {
    std::vector<std::shared_ptr<Event>> events;   // time order
    double loop_duration{};
    std::int32_t loop_count{};
    double offset{};
    bool mute{}, solo{};
    std::uint32_t automated_parameters{};
    double length{};                              // kSequenceTrackProperty_TrackLength
    bool length_set{};

    void insert(std::shared_ptr<Event> event);
    [[nodiscard]] double end() const;              // the length property, or the end of the last event
};

struct TimeSignature { double beat{}; std::uint32_t numerator{4}, denominator{4}; };

struct BarBeatTime { std::int32_t bar{1}; std::uint16_t beat{1}, subbeat{}, subbeat_divisor{}; };

class Sequence {
public:
    Sequence();
    std::shared_ptr<Track> tempo;
    std::vector<std::shared_ptr<Track>> tracks;
    std::uint32_t type{0x62656174};      // MusicSequenceType: 'beat' (also 'secs', 'samp')
    std::int16_t resolution{480};        // ticks per quarter of the loaded file

    [[nodiscard]] double seconds_for_beats(double beats) const;
    [[nodiscard]] double beats_for_seconds(double seconds) const;
    [[nodiscard]] std::vector<TimeSignature> time_signatures() const;
    [[nodiscard]] BarBeatTime to_bar_beat(double beats, std::uint16_t subbeat_divisor) const;
    [[nodiscard]] double from_bar_beat(const BarBeatTime& time) const;

    // Replaces the content with a Standard MIDI File; false when malformed.
    bool load_smf(std::span<const std::uint8_t> bytes, std::uint32_t flags, std::string& why);
    [[nodiscard]] std::vector<std::uint8_t> write_smf(std::int16_t resolution) const;

private:
    struct Segment { double beat, seconds, bpm; };
    [[nodiscard]] std::vector<Segment> tempo_map() const;
};

// Payload helpers (little-endian C structures).
std::vector<std::uint8_t> note_payload(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity, std::uint8_t release, float duration);
std::vector<std::uint8_t> channel_payload(std::uint8_t status, std::uint8_t data1, std::uint8_t data2);
std::vector<std::uint8_t> raw_payload(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> meta_payload(std::uint8_t type, std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> tempo_payload(double bpm);

// Flags of MusicSequenceFileLoad.
inline constexpr std::uint32_t load_channels_to_tracks = 1;

} // namespace alchemy::gui::completion_kit::media::music
