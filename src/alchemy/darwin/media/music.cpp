#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/coremidi.hpp"
#include "alchemy/darwin/media/music_core.hpp"

#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/runloop.hpp"
#include "alchemy/darwin/security/bridge.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <mutex>
#include <thread>

// AudioToolbox MusicSequence, MusicTrack, MusicEventIterator and MusicPlayer
// over the sequence core (music_core.hpp).
//
// The player plays a sequence to its MIDI destination
// (MusicSequenceSetMIDIEndpoint): a player thread (a guest completion thread)
// schedules each 200 ms window of note, channel and raw-data events as
// timestamped CoreMIDI packets (the MIDI server holds them until their host
// time) and calls the sequence's user callback for user events with the
// window as the slice. Tracks honour mute, solo, offset and loop info. There
// is no AUGraph (no music device or audio unit can exist), so a sequence
// without a MIDI endpoint cannot play (kAudioToolboxError_NoTrackDestination),
// and parameter, AU preset and extended-note events, which address audio
// units, are not dispatched. Stopping or moving the play position drops the
// held packets of the destination and ends the notes that are sounding.
namespace alchemy::gui::completion_kit::media {
namespace {

constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
constexpr std::uint32_t midi_file_type = 0x6d696469;   // kMusicSequenceFile_MIDIType 'midi'

struct SequenceObject {
    std::shared_ptr<music::Sequence> sequence = std::make_shared<music::Sequence>();
    Address handle{};
    std::uint32_t endpoint{};
    Address user_callback{}, user_data{};
};

struct TrackRef {
    std::weak_ptr<SequenceObject> owner;
    std::shared_ptr<music::Track> track;
};

struct Iterator {
    Address track_handle{};
    std::shared_ptr<music::Track> track;
    std::size_t index{};
};

struct Sounding { std::uint32_t destination{}; std::array<std::uint8_t, 3> off{}; std::uint64_t on{}, off_time{}; };

struct Player {
    std::mutex mutex;
    std::shared_ptr<SequenceObject> sequence;
    bool playing{};
    double position{};            // beats while stopped; beats at the anchor while playing
    double anchor_seconds{};      // sequence seconds at the anchor
    std::uint64_t anchor_host{};
    double rate{1.0};
    double scheduled_until{};     // beats already handed to CoreMIDI
    std::uint64_t generation{};
    std::shared_ptr<CompletionThread> thread;
    std::vector<Sounding> sounding;

    double beats_at(std::uint64_t host) const {
        const double elapsed = (static_cast<double>(host) - static_cast<double>(anchor_host)) / 1e9 * rate;
        return sequence->sequence->beats_for_seconds(anchor_seconds + elapsed);
    }
    std::uint64_t host_for(double beats) const {
        const double seconds = sequence->sequence->seconds_for_beats(beats) - anchor_seconds;
        const double host = static_cast<double>(anchor_host) + seconds / rate * 1e9;
        return host <= 0 ? 0 : static_cast<std::uint64_t>(host);
    }
};

void status(CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }
double real(const CpuState& s, unsigned index) { return std::bit_cast<double>(s.v[index][0]); }

struct Music {
    Support& support;
    std::shared_ptr<security::Bridge> bridge = security::bridge();
    std::recursive_mutex mutex;
    std::map<Address, std::shared_ptr<SequenceObject>> sequences;
    std::map<Address, TrackRef> tracks;
    std::map<const music::Track*, Address> track_handles;
    std::map<Address, Iterator> iterators;
    std::map<Address, std::shared_ptr<Player>> players;

    explicit Music(Support& owner) : support(owner) {}

    Address new_handle() { return support.process.allocate_guest(16, 16); }
    std::shared_ptr<SequenceObject> sequence(Address handle) {
        const auto found = sequences.find(handle);
        return found == sequences.end() ? nullptr : found->second;
    }
    TrackRef* track(Address handle) {
        const auto found = tracks.find(handle);
        return found == tracks.end() || found->second.owner.expired() ? nullptr : &found->second;
    }
    Address handle_for(const std::shared_ptr<SequenceObject>& owner, const std::shared_ptr<music::Track>& value) {
        const auto found = track_handles.find(value.get());
        if (found != track_handles.end()) return found->second;
        const auto handle = new_handle();
        tracks[handle] = {owner, value};
        track_handles[value.get()] = handle;
        return handle;
    }
    // Frees the guest copies of a track's events (and AU preset retains) and forgets its handle.
    void drop_track(const std::shared_ptr<music::Track>& value) {
        for (auto& event : value->events) release_event(*event);
        const auto found = track_handles.find(value.get());
        if (found == track_handles.end()) return;
        tracks.erase(found->second);
        support.process.release_guest(found->second);
        track_handles.erase(found);
    }
    void release_event(music::Event& event) {
        if (event.type == music::EventType::au_preset && event.data.size() >= 16) {
            std::uint64_t preset{};
            std::memcpy(&preset, event.data.data() + 8, 8);
            support.release(preset);
        }
        if (event.guest) support.process.release_guest(std::exchange(event.guest, 0));
    }
    Address guest_copy(music::Event& event) {
        if (!event.guest) {
            event.guest = support.process.allocate_guest(std::max<std::size_t>(event.data.size(), 16), 8);
            support.memory().write_bytes(event.guest, event.data);
        }
        return event.guest;
    }
    // The bytes of the guest file at `path` (through the guest libSystem), or nullopt.
    std::optional<std::vector<std::uint8_t>> read_file(const std::string& path) {
        const auto text = support.process.allocate_guest(path.size() + 1, 1);
        std::vector<std::uint8_t> name(path.begin(), path.end());
        name.push_back(0);
        support.memory().write_bytes(text, name);
        const auto fd = static_cast<std::int64_t>(support.call(libsystem, "_open", {text, 0}));
        support.process.release_guest(text);
        if (fd < 0) return std::nullopt;
        std::vector<std::uint8_t> bytes;
        const auto chunk = support.process.allocate_guest(65536);
        for (;;) {
            const auto got = static_cast<std::int64_t>(support.call(libsystem, "_read", {static_cast<Address>(fd), chunk, 65536}));
            if (got <= 0) { if (got < 0) bytes.clear(); break; }
            const auto at = bytes.size();
            bytes.resize(at + static_cast<std::size_t>(got));
            support.memory().read_bytes(chunk, std::span(bytes.data() + at, static_cast<std::size_t>(got)));
            if (bytes.size() > (64u << 20)) { bytes.clear(); break; }   // not a MIDI file
        }
        support.process.release_guest(chunk);
        static_cast<void>(support.call(libsystem, "_close", {static_cast<Address>(fd)}));
        return bytes;
    }
    std::int32_t load(const std::shared_ptr<SequenceObject>& owner, std::span<const std::uint8_t> bytes, std::uint32_t type, std::uint32_t flags) {
        if (type != 0 && type != midi_file_type) return music::error::invalid_sequence_type;   // iMelody and others are not read
        auto fresh = std::make_shared<music::Sequence>();
        std::string why;
        if (!fresh->load_smf(bytes, flags, why)) return music::error::invalid_file;
        for (const auto& track : owner->sequence->tracks) drop_track(track);
        drop_track(owner->sequence->tempo);
        owner->sequence->tempo = fresh->tempo;
        owner->sequence->tracks = fresh->tracks;
        owner->sequence->resolution = fresh->resolution;
        return 0;
    }
    std::int32_t add_event(Address track_handle, double time, music::EventType type, std::vector<std::uint8_t> data) {
        auto* ref = track(track_handle);
        if (!ref) return music::error::track_not_found;
        if (!(time >= 0) || !std::isfinite(time)) return music::error::param;
        const auto owner = ref->owner.lock();
        const bool tempo_track = owner && owner->sequence->tempo == ref->track;
        if ((type == music::EventType::extended_tempo) != tempo_track && type != music::EventType::meta)
            return music::error::invalid_event_type;   // tempo events live in the tempo track only, and only they do (with meta events)
        auto event = std::make_shared<music::Event>();
        event->time = time;
        event->type = type;
        event->data = std::move(data);
        ref->track->insert(std::move(event));
        return 0;
    }

    // ---- player ----------------------------------------------------------------------
    void send_offs(Player& player, std::uint64_t now) {
        std::map<std::uint32_t, std::vector<midi::Packet>> offs;
        for (const auto& note : player.sounding)
            if (note.on <= now && note.off_time > now) offs[note.destination].push_back({0, {note.off.begin(), note.off.end()}});
        player.sounding.clear();
        for (auto& [destination, packets] : offs) static_cast<void>(midi_send_internal(destination, std::move(packets)));
    }
    void halt(Player& player) {
        const auto now = host_time_now();
        if (player.sequence && player.sequence->endpoint) static_cast<void>(midi_flush_internal(player.sequence->endpoint));
        send_offs(player, now);
    }
    // Hands the events in [player.scheduled_until, until) to CoreMIDI; returns user events to call.
    struct UserCall { Address track, data; double time, start, end; };
    std::vector<UserCall> schedule(Player& player, double until) {
        std::vector<UserCall> calls;
        const auto owner = player.sequence;
        const double from = player.scheduled_until;
        if (until <= from) return calls;
        player.scheduled_until = until;
        bool solo = false;
        for (const auto& track : owner->sequence->tracks) solo = solo || track->solo;
        std::vector<midi::Packet> packets;
        const auto now = host_time_now();
        std::erase_if(player.sounding, [now](const Sounding& note) { return note.off_time <= now; });
        for (const auto& track : owner->sequence->tracks) {
            if (track->mute || (solo && !track->solo)) continue;
            const double loop = track->loop_duration;
            const auto emit = [&](const music::Event& event, double time) {
                if (time < from || time >= until) return;
                const auto& d = event.data;
                switch (event.type) {
                case music::EventType::midi_note: {
                    if (d.size() < 8) return;
                    float duration{};
                    std::memcpy(&duration, d.data() + 4, 4);
                    const std::uint8_t channel = d[0] & 0x0f;
                    const auto on = player.host_for(time), off = player.host_for(time + std::max(0.0f, duration));
                    packets.push_back({std::max<std::uint64_t>(on, 1), {static_cast<std::uint8_t>(0x90 | channel), static_cast<std::uint8_t>(d[1] & 0x7f), static_cast<std::uint8_t>(d[2] & 0x7f)}});
                    const std::array<std::uint8_t, 3> note_off{static_cast<std::uint8_t>(0x80 | channel), static_cast<std::uint8_t>(d[1] & 0x7f), static_cast<std::uint8_t>(d[3] & 0x7f)};
                    packets.push_back({std::max<std::uint64_t>(off, 1), {note_off.begin(), note_off.end()}});
                    player.sounding.push_back({owner->endpoint, note_off, on, off});
                    return;
                }
                case music::EventType::midi_channel: {
                    if (d.size() < 3 || !(d[0] & 0x80)) return;
                    const auto length = std::max<std::size_t>(midi::message_length(d[0]), 1);
                    packets.push_back({std::max<std::uint64_t>(player.host_for(time), 1), {d.begin(), d.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(length, 3))}});
                    return;
                }
                case music::EventType::midi_raw: {
                    std::uint32_t length{};
                    std::memcpy(&length, d.data(), 4);
                    if (!length || d.size() < 4 + length) return;
                    packets.push_back({std::max<std::uint64_t>(player.host_for(time), 1), {d.begin() + 4, d.begin() + 4 + length}});
                    return;
                }
                case music::EventType::user:
                    if (owner->user_callback) calls.push_back({handle_for(owner, track), guest_copy(const_cast<music::Event&>(event)), time, from, until});
                    return;
                default: return;   // tempo and meta: conductor data; parameter/AU preset/extended notes: audio units
                }
            };
            for (const auto& event : track->events) {
                if (loop > 0 && event->time >= loop) break;
                if (loop <= 0) { emit(*event, event->time + track->offset); continue; }
                const auto first = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor((from - track->offset - event->time) / loop)));
                for (auto k = first;; ++k) {
                    if (track->loop_count > 0 && k >= track->loop_count) break;
                    const double time = event->time + track->offset + static_cast<double>(k) * loop;
                    if (time >= until) break;
                    emit(*event, time);
                }
            }
        }
        if (!packets.empty() && owner->endpoint) {
            std::stable_sort(packets.begin(), packets.end(), [](const midi::Packet& a, const midi::Packet& b) { return a.time < b.time; });
            static_cast<void>(midi_send_internal(owner->endpoint, std::move(packets)));
        }
        return calls;
    }
    // The player thread's work item: one per Start; returns when the player stops.
    void run(std::shared_ptr<Player> player, std::uint64_t generation) {
        auto& process = support.process;
        for (;;) {
            if (process.requested_exit().has_value() || engine_stop_requested(process.calling_engine())) return;
            std::vector<UserCall> calls;
            Address callback{}, data{}, sequence_handle{};
            {
                std::lock_guard lock(mutex);
                std::lock_guard player_lock(player->mutex);
                if (!player->playing || player->generation != generation) return;
                calls = schedule(*player, player->beats_at(host_time_now() + 200'000'000));
                callback = player->sequence->user_callback;
                data = player->sequence->user_data;
                sequence_handle = player->sequence->handle;
            }
            // MusicSequenceUserCallback(clientData, sequence, track, eventTime, const MusicEventUserData *, startSliceBeat, endSliceBeat)
            for (const auto& call : calls) {
                CallRegs registers{};
                registers.x[0] = data;
                registers.x[1] = sequence_handle;
                registers.x[2] = call.track;
                registers.x[3] = call.data;
                registers.set_d(0, call.time);
                registers.set_d(1, call.start);
                registers.set_d(2, call.end);
                static_cast<void>(EngineAdapter(process.calling_engine()).call_regs(callback, registers));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
};

} // namespace

void install_music(Support& support) {
    auto state = std::make_shared<Music>(support);
    auto* sp = &support;
    const auto lib = audio_toolbox;

    // ---- sequences ---------------------------------------------------------------------
    support.function(lib, "_NewMusicSequence", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return status(s, music::error::param);
        std::lock_guard lock(state->mutex);
        auto owner = std::make_shared<SequenceObject>();
        owner->handle = state->new_handle();
        state->sequences[owner->handle] = owner;
        m.write64(s.x[0], owner->handle);
        status(s, 0);
    });
    support.function(lib, "_DisposeMusicSequence", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner) return status(s, music::error::param);
        for (auto& [handle, player] : state->players) {
            std::lock_guard player_lock(player->mutex);
            if (player->sequence != owner) continue;
            if (player->playing) return status(s, music::error::cannot_do_in_current_context);
            player->sequence.reset();
        }
        for (const auto& track : owner->sequence->tracks) state->drop_track(track);
        state->drop_track(owner->sequence->tempo);
        state->sequences.erase(s.x[0]);
        state->support.process.release_guest(s.x[0]);
        status(s, 0);
    });
    support.function(lib, "_MusicSequenceNewTrack", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner || !s.x[1]) return status(s, music::error::param);
        auto track = std::make_shared<music::Track>();
        owner->sequence->tracks.push_back(track);
        m.write64(s.x[1], state->handle_for(owner, track));
        status(s, 0);
    });
    support.function(lib, "_MusicSequenceDisposeTrack", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        auto* ref = state->track(s.x[1]);
        if (!owner) return status(s, music::error::param);
        if (!ref || ref->owner.lock() != owner || ref->track == owner->sequence->tempo) return status(s, music::error::track_not_found);
        auto& list = owner->sequence->tracks;
        const auto value = ref->track;
        list.erase(std::remove(list.begin(), list.end(), value), list.end());
        state->drop_track(value);
        status(s, 0);
    });
    support.function(lib, "_MusicSequenceGetTrackCount", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner || !s.x[1]) return status(s, music::error::param);
        m.write32(s.x[1], static_cast<std::uint32_t>(owner->sequence->tracks.size()));
        status(s, 0);
    });
    support.function(lib, "_MusicSequenceGetIndTrack", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner || !s.x[2]) return status(s, music::error::param);
        const auto index = static_cast<std::uint32_t>(s.x[1]);
        if (index >= owner->sequence->tracks.size()) return status(s, music::error::track_index);
        m.write64(s.x[2], state->handle_for(owner, owner->sequence->tracks[index]));
        status(s, 0);
    });
    support.function(lib, "_MusicSequenceGetTrackIndex", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        auto* ref = state->track(s.x[1]);
        if (!owner || !s.x[2]) return status(s, music::error::param);
        const auto& list = owner->sequence->tracks;
        const auto found = ref ? std::find(list.begin(), list.end(), ref->track) : list.end();
        if (found == list.end()) return status(s, music::error::track_not_found);
        m.write32(s.x[2], static_cast<std::uint32_t>(found - list.begin()));
        status(s, 0);
    });
    support.function(lib, "_MusicSequenceGetTempoTrack", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner || !s.x[1]) return status(s, music::error::param);
        m.write64(s.x[1], state->handle_for(owner, owner->sequence->tempo));
        status(s, 0);
    });
    support.function(lib, "_MusicSequenceGetSequenceType", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner || !s.x[1]) return status(s, music::error::param);
        m.write32(s.x[1], owner->sequence->type);
        status(s, 0);
    });
    // MusicSequenceSetMIDIEndpoint(sequence, MIDIEndpointRef): every track plays to the destination.
    support.function(lib, "_MusicSequenceSetMIDIEndpoint", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner) return status(s, music::error::param);
        const auto endpoint = static_cast<std::uint32_t>(s.x[1]);
        if (!midi_is_destination(endpoint)) return status(s, music::error::illegal_track_destination);
        owner->endpoint = endpoint;
        status(s, 0);
    });
    support.function(lib, "_MusicSequenceSetUserCallback", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner) return status(s, music::error::param);
        owner->user_callback = s.x[1];
        owner->user_data = s.x[2];
        status(s, 0);
    });
    // (sequence, MusicTimeStamp beats [d0], Float64 *outSeconds) and the inverse.
    support.function(lib, "_MusicSequenceGetSecondsForBeats", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner || !s.x[1]) return status(s, music::error::param);
        m.write64(s.x[1], std::bit_cast<std::uint64_t>(owner->sequence->seconds_for_beats(real(s, 0))));
        status(s, 0);
    });
    support.function(lib, "_MusicSequenceGetBeatsForSeconds", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner || !s.x[1]) return status(s, music::error::param);
        m.write64(s.x[1], std::bit_cast<std::uint64_t>(owner->sequence->beats_for_seconds(real(s, 0))));
        status(s, 0);
    });
    // CABarBeatTime {SInt32 bar; UInt16 beat; UInt16 subbeat; UInt16 subbeatDivisor; UInt16 reserved}.
    // MusicSequenceBeatsToBarBeatTime(sequence, MusicTimeStamp beats [d0], UInt32 subbeatDivisor, CABarBeatTime *out)
    support.function(lib, "_MusicSequenceBeatsToBarBeatTime", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner || !s.x[2] || s.x[1] > 0xffff) return status(s, music::error::param);
        const auto time = owner->sequence->to_bar_beat(real(s, 0), static_cast<std::uint16_t>(s.x[1]));
        m.write32(s.x[2], static_cast<std::uint32_t>(time.bar));
        m.write8(s.x[2] + 4, static_cast<std::uint8_t>(time.beat)); m.write8(s.x[2] + 5, static_cast<std::uint8_t>(time.beat >> 8));
        m.write8(s.x[2] + 6, static_cast<std::uint8_t>(time.subbeat)); m.write8(s.x[2] + 7, static_cast<std::uint8_t>(time.subbeat >> 8));
        m.write8(s.x[2] + 8, static_cast<std::uint8_t>(time.subbeat_divisor)); m.write8(s.x[2] + 9, static_cast<std::uint8_t>(time.subbeat_divisor >> 8));
        m.write8(s.x[2] + 10, 0); m.write8(s.x[2] + 11, 0);
        status(s, 0);
    });
    // MusicSequenceBarBeatTimeToBeats(sequence, const CABarBeatTime *, MusicTimeStamp *outBeats)
    support.function(lib, "_MusicSequenceBarBeatTimeToBeats", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner || !s.x[1] || !s.x[2]) return status(s, music::error::param);
        const auto u16 = [&m](Address at) { return static_cast<std::uint16_t>(m.read8(at) | (m.read8(at + 1) << 8)); };
        music::BarBeatTime time;
        time.bar = static_cast<std::int32_t>(m.read32(s.x[1]));
        time.beat = u16(s.x[1] + 4);
        time.subbeat = u16(s.x[1] + 6);
        time.subbeat_divisor = u16(s.x[1] + 8);
        m.write64(s.x[2], std::bit_cast<std::uint64_t>(owner->sequence->from_bar_beat(time)));
        status(s, 0);
    });
    // MusicSequenceFileLoad(sequence, CFURLRef, MusicSequenceFileTypeID, MusicSequenceLoadFlags)
    support.function(lib, "_MusicSequenceFileLoad", [state, sp](CpuState& s, GuestMemory&) {
        if (!s.x[1]) return status(s, music::error::param);
        const auto path = sp->cf.url_path(s.x[1]);
        const auto bytes = state->read_file(path);
        if (!bytes) return status(s, -43);   // fnfErr
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner) return status(s, music::error::param);
        status(s, state->load(owner, *bytes, static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3])));
    });
    // MusicSequenceFileLoadData(sequence, CFDataRef, MusicSequenceFileTypeID, MusicSequenceLoadFlags)
    support.function(lib, "_MusicSequenceFileLoadData", [state](CpuState& s, GuestMemory&) {
        if (!s.x[1] || !state->bridge->is_data(s.x[1])) return status(s, music::error::param);
        const auto bytes = state->bridge->bytes(s.x[1]);
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner) return status(s, music::error::param);
        status(s, state->load(owner, bytes, static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3])));
    });
    // MusicSequenceFileCreateData(sequence, MusicSequenceFileTypeID, MusicSequenceFileFlags, SInt16 resolution, CFDataRef *out)
    support.function(lib, "_MusicSequenceFileCreateData", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto owner = state->sequence(s.x[0]);
        if (!owner || !s.x[4]) return status(s, music::error::param);
        m.write64(s.x[4], 0);
        if (static_cast<std::uint32_t>(s.x[1]) != midi_file_type) return status(s, music::error::invalid_sequence_type);
        m.write64(s.x[4], state->bridge->data(owner->sequence->write_smf(static_cast<std::int16_t>(s.x[3]))));
        status(s, 0);
    });

    // ---- tracks -----------------------------------------------------------------------------
    support.function(lib, "_MusicTrackGetSequence", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* ref = state->track(s.x[0]);
        if (!ref || !s.x[1]) return status(s, music::error::param);
        m.write64(s.x[1], ref->owner.lock()->handle);
        status(s, 0);
    });
    // MusicTrackGetProperty(track, UInt32 propertyID, void *outData, UInt32 *ioLength): with a NULL
    // outData only the length is returned.
    support.function(lib, "_MusicTrackGetProperty", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* ref = state->track(s.x[0]);
        if (!ref || !s.x[3]) return status(s, music::error::param);
        const auto& t = *ref->track;
        const auto owner = ref->owner.lock();
        std::vector<std::uint8_t> value;
        const auto put = [&value](const void* data, std::size_t size) { const auto* p = static_cast<const std::uint8_t*>(data); value.assign(p, p + size); };
        switch (static_cast<std::uint32_t>(s.x[1])) {
        case 0: {   // kSequenceTrackProperty_LoopInfo: MusicTrackLoopInfo {MusicTimeStamp loopDuration; SInt32 numberOfLoops;}
            std::array<std::uint8_t, 16> info{};
            std::memcpy(info.data(), &t.loop_duration, 8);
            std::memcpy(info.data() + 8, &t.loop_count, 4);
            put(info.data(), 16);
            break;
        }
        case 1: put(&t.offset, 8); break;                                 // OffsetTime
        case 2: { const std::uint8_t v = t.mute; put(&v, 1); break; }     // MuteStatus (Boolean)
        case 3: { const std::uint8_t v = t.solo; put(&v, 1); break; }     // SoloStatus
        case 4: put(&t.automated_parameters, 4); break;                   // AutomatedParameters
        case 5: { const double end = t.end(); put(&end, 8); break; }      // TrackLength
        case 6:                                                           // TimeResolution (tempo track only)
            if (!owner || owner->sequence->tempo != ref->track) return status(s, music::error::param);
            put(&owner->sequence->resolution, 2);
            break;
        default: return status(s, music::error::param);
        }
        if (s.x[2]) {
            if (m.read32(s.x[3]) < value.size()) return status(s, music::error::param);
            m.write_bytes(s.x[2], value);
        }
        m.write32(s.x[3], static_cast<std::uint32_t>(value.size()));
        status(s, 0);
    });
    // MusicTrackSetProperty(track, UInt32 propertyID, void *data, UInt32 length)
    support.function(lib, "_MusicTrackSetProperty", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* ref = state->track(s.x[0]);
        if (!ref || !s.x[2]) return status(s, music::error::param);
        auto& t = *ref->track;
        const auto length = static_cast<std::uint32_t>(s.x[3]);
        const auto real_at = [&m](Address at) { return std::bit_cast<double>(m.read64(at)); };
        switch (static_cast<std::uint32_t>(s.x[1])) {
        case 0:
            if (length < 12) return status(s, music::error::param);
            t.loop_duration = real_at(s.x[2]);
            t.loop_count = static_cast<std::int32_t>(m.read32(s.x[2] + 8));
            break;
        case 1: if (length < 8) return status(s, music::error::param); t.offset = real_at(s.x[2]); break;
        case 2: if (length < 1) return status(s, music::error::param); t.mute = m.read8(s.x[2]) != 0; break;
        case 3: if (length < 1) return status(s, music::error::param); t.solo = m.read8(s.x[2]) != 0; break;
        case 4: if (length < 4) return status(s, music::error::param); t.automated_parameters = m.read32(s.x[2]); break;
        case 5: if (length < 8) return status(s, music::error::param); t.length = real_at(s.x[2]); t.length_set = true; break;
        default: return status(s, music::error::param);   // TimeResolution is read-only
        }
        status(s, 0);
    });
    // MusicTrackNew*Event(track, MusicTimeStamp [d0], const <payload> *): copies of the C structures.
    const auto payload = [](GuestMemory& m, Address at, std::size_t fixed, std::optional<std::size_t> length_at, std::size_t length_base) {
        std::size_t size = fixed;
        if (length_at) size = length_base + m.read32(at + *length_at);
        if (size > (16u << 20)) throw std::runtime_error("Alchemy: MusicTrack event payload larger than 16 MB");
        std::vector<std::uint8_t> bytes(size);
        if (size) m.read_bytes(at, bytes);
        return bytes;
    };
    struct Kind { const char* name; music::EventType type; std::size_t fixed; std::optional<std::size_t> length_at; std::size_t length_base; };
    for (const auto& kind : std::initializer_list<Kind>{
             {"_MusicTrackNewMIDINoteEvent", music::EventType::midi_note, 8, std::nullopt, 0},
             {"_MusicTrackNewMIDIChannelEvent", music::EventType::midi_channel, 4, std::nullopt, 0},
             {"_MusicTrackNewMIDIRawDataEvent", music::EventType::midi_raw, 0, 0, 4},
             {"_MusicTrackNewMetaEvent", music::EventType::meta, 0, 4, 8},
             {"_MusicTrackNewUserEvent", music::EventType::user, 0, 0, 4},
             {"_MusicTrackNewParameterEvent", music::EventType::parameter, 16, std::nullopt, 0}}) {
        const auto k = kind;
        support.function(lib, k.name, [state, payload, k](CpuState& s, GuestMemory& m) {
            if (!s.x[1]) return status(s, music::error::param);
            auto bytes = payload(m, s.x[1], k.fixed, k.length_at, k.length_base);
            std::lock_guard lock(state->mutex);
            status(s, state->add_event(s.x[0], real(s, 0), k.type, std::move(bytes)));
        });
    }
    // ExtendedNoteOnEvent {instrumentID; groupID; Float32 duration; MusicDeviceNoteParams {UInt32 argCount;
    // Float32 mPitch; Float32 mVelocity; NoteParamsControlValue mControls[argCount - 2]}}.
    support.function(lib, "_MusicTrackNewExtendedNoteEvent", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) return status(s, music::error::param);
        const auto count = m.read32(s.x[1] + 12);
        if (count > 4096) return status(s, music::error::param);
        std::vector<std::uint8_t> bytes(24 + 8 * static_cast<std::size_t>(count > 2 ? count - 2 : 0));
        m.read_bytes(s.x[1], bytes);
        std::lock_guard lock(state->mutex);
        status(s, state->add_event(s.x[0], real(s, 0), music::EventType::extended_note, std::move(bytes)));
    });
    // MusicTrackNewExtendedTempoEvent(track, MusicTimeStamp [d0], Float64 bpm [d1]): tempo track only.
    support.function(lib, "_MusicTrackNewExtendedTempoEvent", [state](CpuState& s, GuestMemory&) {
        const double bpm = real(s, 1);
        if (!(bpm > 0) || !std::isfinite(bpm)) return status(s, music::error::param);
        std::lock_guard lock(state->mutex);
        status(s, state->add_event(s.x[0], real(s, 0), music::EventType::extended_tempo, music::tempo_payload(bpm)));
    });
    // AUPresetEvent {AudioUnitScope scope; AudioUnitElement element; CFPropertyListRef preset} (retained).
    support.function(lib, "_MusicTrackNewAUPresetEvent", [state, sp](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) return status(s, music::error::param);
        std::vector<std::uint8_t> bytes(16);
        m.read_bytes(s.x[1], bytes);
        std::uint64_t preset{};
        std::memcpy(&preset, bytes.data() + 8, 8);
        std::lock_guard lock(state->mutex);
        const auto result = state->add_event(s.x[0], real(s, 0), music::EventType::au_preset, std::move(bytes));
        if (!result) sp->retain(preset);
        status(s, result);
    });

    // ---- iterators -------------------------------------------------------------------------
    support.function(lib, "_NewMusicEventIterator", [state](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* ref = state->track(s.x[0]);
        if (!ref || !s.x[1]) return status(s, music::error::param);
        const auto handle = state->new_handle();
        state->iterators[handle] = {s.x[0], ref->track, 0};
        m.write64(s.x[1], handle);
        status(s, 0);
    });
    support.function(lib, "_DisposeMusicEventIterator", [state](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        if (!state->iterators.erase(s.x[0])) return status(s, music::error::param);
        state->support.process.release_guest(s.x[0]);
        status(s, 0);
    });
    const auto iterator_of = [state](Address handle) -> Iterator* {
        const auto found = state->iterators.find(handle);
        return found == state->iterators.end() ? nullptr : &found->second;
    };
    // HasNextEvent answers whether the iterator is on an event (CoreAudio's long-standing
    // behaviour, kept by MusicEventIteratorHasCurrentEvent).
    for (const auto* name : {"_MusicEventIteratorHasNextEvent", "_MusicEventIteratorHasCurrentEvent"})
        support.function(lib, name, [state, iterator_of](CpuState& s, GuestMemory& m) {
            std::lock_guard lock(state->mutex);
            auto* it = iterator_of(s.x[0]);
            if (!it || !s.x[1]) return status(s, music::error::param);
            m.write8(s.x[1], it->index < it->track->events.size() ? 1 : 0);
            status(s, 0);
        });
    support.function(lib, "_MusicEventIteratorHasPreviousEvent", [state, iterator_of](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* it = iterator_of(s.x[0]);
        if (!it || !s.x[1]) return status(s, music::error::param);
        m.write8(s.x[1], it->index > 0 && !it->track->events.empty() ? 1 : 0);
        status(s, 0);
    });
    support.function(lib, "_MusicEventIteratorNextEvent", [state, iterator_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        auto* it = iterator_of(s.x[0]);
        if (!it) return status(s, music::error::param);
        if (it->index >= it->track->events.size()) return status(s, music::error::end_of_track);
        ++it->index;
        status(s, 0);
    });
    support.function(lib, "_MusicEventIteratorPreviousEvent", [state, iterator_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        auto* it = iterator_of(s.x[0]);
        if (!it) return status(s, music::error::param);
        if (it->index == 0) return status(s, music::error::start_of_track);
        it->index = std::min(it->index, it->track->events.size()) - 1;
        status(s, 0);
    });
    // MusicEventIteratorSeek(iterator, MusicTimeStamp [d0]): the first event at or after the time.
    support.function(lib, "_MusicEventIteratorSeek", [state, iterator_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        auto* it = iterator_of(s.x[0]);
        if (!it) return status(s, music::error::param);
        const double time = real(s, 0);
        const auto& events = it->track->events;
        it->index = static_cast<std::size_t>(std::lower_bound(events.begin(), events.end(), time,
            [](const std::shared_ptr<music::Event>& e, double t) { return e->time < t; }) - events.begin());
        status(s, 0);
    });
    // MusicEventIteratorGetEventInfo(iterator, MusicTimeStamp *, MusicEventType *, const void **data, UInt32 *size)
    support.function(lib, "_MusicEventIteratorGetEventInfo", [state, iterator_of](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        auto* it = iterator_of(s.x[0]);
        if (!it) return status(s, music::error::param);
        if (it->index >= it->track->events.size()) return status(s, music::error::end_of_track);
        auto& event = *it->track->events[it->index];
        if (s.x[1]) m.write64(s.x[1], std::bit_cast<std::uint64_t>(event.time));
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(event.type));
        if (s.x[3]) m.write64(s.x[3], state->guest_copy(event));
        if (s.x[4]) m.write32(s.x[4], static_cast<std::uint32_t>(event.data.size()));
        status(s, 0);
    });
    support.function(lib, "_MusicEventIteratorDeleteEvent", [state, iterator_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        auto* it = iterator_of(s.x[0]);
        if (!it) return status(s, music::error::param);
        auto& events = it->track->events;
        if (it->index >= events.size()) return status(s, music::error::end_of_track);
        state->release_event(*events[it->index]);
        events.erase(events.begin() + static_cast<std::ptrdiff_t>(it->index));   // the iterator moves to the next event
        status(s, 0);
    });

    // ---- players -----------------------------------------------------------------------------
    support.function(lib, "_NewMusicPlayer", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) return status(s, music::error::param);
        std::lock_guard lock(state->mutex);
        const auto handle = state->new_handle();
        state->players[handle] = std::make_shared<Player>();
        m.write64(s.x[0], handle);
        status(s, 0);
    });
    const auto player_of = [state](Address handle) -> std::shared_ptr<Player> {
        const auto found = state->players.find(handle);
        return found == state->players.end() ? nullptr : found->second;
    };
    support.function(lib, "_DisposeMusicPlayer", [state, player_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        const auto player = player_of(s.x[0]);
        if (!player) return status(s, music::error::param);
        {
            std::lock_guard player_lock(player->mutex);
            if (player->playing) { player->playing = false; ++player->generation; state->halt(*player); }
        }
        state->players.erase(s.x[0]);
        state->support.process.release_guest(s.x[0]);
        status(s, 0);
    });
    support.function(lib, "_MusicPlayerSetSequence", [state, player_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        const auto player = player_of(s.x[0]);
        if (!player) return status(s, music::error::param);
        const auto owner = s.x[1] ? state->sequence(s.x[1]) : nullptr;
        if (s.x[1] && !owner) return status(s, music::error::param);
        std::lock_guard player_lock(player->mutex);
        if (player->playing) { player->playing = false; ++player->generation; state->halt(*player); }
        player->sequence = owner;
        player->position = 0;
        status(s, 0);
    });
    support.function(lib, "_MusicPlayerGetSequence", [state, player_of](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto player = player_of(s.x[0]);
        if (!player || !s.x[1]) return status(s, music::error::param);
        std::lock_guard player_lock(player->mutex);
        if (!player->sequence) return status(s, music::error::no_sequence);
        m.write64(s.x[1], player->sequence->handle);
        status(s, 0);
    });
    support.function(lib, "_MusicPlayerPreroll", [state, player_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        const auto player = player_of(s.x[0]);
        if (!player) return status(s, music::error::param);
        std::lock_guard player_lock(player->mutex);
        if (!player->sequence) return status(s, music::error::no_sequence);
        status(s, player->sequence->endpoint ? 0 : music::error::no_track_destination);
    });
    support.function(lib, "_MusicPlayerStart", [state, player_of](CpuState& s, GuestMemory&) {
        std::shared_ptr<Player> player;
        std::uint64_t generation{};
        std::shared_ptr<CompletionThread> thread;
        bool need_thread{};
        {
            std::lock_guard lock(state->mutex);
            player = player_of(s.x[0]);
            if (!player) return status(s, music::error::param);
            std::lock_guard player_lock(player->mutex);
            if (!player->sequence) return status(s, music::error::no_sequence);
            if (!player->sequence->endpoint) return status(s, music::error::no_track_destination);   // no AUGraph exists
            if (player->playing) return status(s, 0);
            player->playing = true;
            player->anchor_host = host_time_now();
            player->anchor_seconds = player->sequence->sequence->seconds_for_beats(player->position);
            player->scheduled_until = player->position;
            generation = ++player->generation;
            need_thread = !player->thread;
            thread = player->thread;
        }
        if (need_thread) {   // started outside the locks (it creates a guest pthread)
            auto started = start_completion_thread(state->support.process, "com.apple.audio.MusicPlayer");
            std::lock_guard player_lock(player->mutex);
            if (!player->thread) player->thread = std::move(started);
            thread = player->thread;
        }
        thread->post([state, player, generation] { state->run(player, generation); });
        status(s, 0);
    });
    support.function(lib, "_MusicPlayerStop", [state, player_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        const auto player = player_of(s.x[0]);
        if (!player) return status(s, music::error::param);
        std::lock_guard player_lock(player->mutex);
        if (!player->playing) return status(s, 0);
        player->position = player->beats_at(host_time_now());
        player->playing = false;
        ++player->generation;
        state->halt(*player);
        status(s, 0);
    });
    support.function(lib, "_MusicPlayerIsPlaying", [state, player_of](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto player = player_of(s.x[0]);
        if (!player || !s.x[1]) return status(s, music::error::param);
        std::lock_guard player_lock(player->mutex);
        m.write8(s.x[1], player->playing ? 1 : 0);
        status(s, 0);
    });
    // MusicPlayerSetTime(player, MusicTimeStamp [d0])
    support.function(lib, "_MusicPlayerSetTime", [state, player_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        const auto player = player_of(s.x[0]);
        if (!player) return status(s, music::error::param);
        std::lock_guard player_lock(player->mutex);
        const double time = real(s, 0);
        if (!std::isfinite(time)) return status(s, music::error::param);
        player->position = time;
        if (player->playing) {
            if (!player->sequence) return status(s, music::error::no_sequence);
            state->halt(*player);
            player->anchor_host = host_time_now();
            player->anchor_seconds = player->sequence->sequence->seconds_for_beats(time);
            player->scheduled_until = time;
        }
        status(s, 0);
    });
    support.function(lib, "_MusicPlayerGetTime", [state, player_of](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto player = player_of(s.x[0]);
        if (!player || !s.x[1]) return status(s, music::error::param);
        std::lock_guard player_lock(player->mutex);
        if (!player->sequence) return status(s, music::error::no_sequence);
        const double time = player->playing ? player->beats_at(host_time_now()) : player->position;
        m.write64(s.x[1], std::bit_cast<std::uint64_t>(time));
        status(s, 0);
    });
    // MusicPlayerSetPlayRateScalar(player, Float64 [d0]): > 0.
    support.function(lib, "_MusicPlayerSetPlayRateScalar", [state, player_of](CpuState& s, GuestMemory&) {
        std::lock_guard lock(state->mutex);
        const auto player = player_of(s.x[0]);
        const double rate = real(s, 0);
        if (!player || !(rate > 0) || !std::isfinite(rate)) return status(s, music::error::param);
        std::lock_guard player_lock(player->mutex);
        if (player->playing && player->sequence) {   // re-anchor at the current position
            const auto now = host_time_now();
            const double beats = player->beats_at(now);
            player->anchor_host = now;
            player->anchor_seconds = player->sequence->sequence->seconds_for_beats(beats);
        }
        player->rate = rate;
        status(s, 0);
    });
    support.function(lib, "_MusicPlayerGetPlayRateScalar", [state, player_of](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto player = player_of(s.x[0]);
        if (!player || !s.x[1]) return status(s, music::error::param);
        std::lock_guard player_lock(player->mutex);
        m.write64(s.x[1], std::bit_cast<std::uint64_t>(player->rate));
        status(s, 0);
    });
    // MusicPlayerGetHostTimeForBeats(player, MusicTimeStamp [d0], UInt64 *outHostTime): playing only.
    support.function(lib, "_MusicPlayerGetHostTimeForBeats", [state, player_of](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto player = player_of(s.x[0]);
        if (!player || !s.x[1]) return status(s, music::error::param);
        std::lock_guard player_lock(player->mutex);
        if (!player->playing || !player->sequence) return status(s, music::error::invalid_player_state);
        m.write64(s.x[1], player->host_for(real(s, 0)));
        status(s, 0);
    });
    // MusicPlayerGetBeatsForHostTime(player, UInt64 hostTime, MusicTimeStamp *outBeats): playing only.
    support.function(lib, "_MusicPlayerGetBeatsForHostTime", [state, player_of](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(state->mutex);
        const auto player = player_of(s.x[0]);
        if (!player || !s.x[2]) return status(s, music::error::param);
        std::lock_guard player_lock(player->mutex);
        if (!player->playing || !player->sequence) return status(s, music::error::invalid_player_state);
        m.write64(s.x[2], std::bit_cast<std::uint64_t>(player->beats_at(s.x[1])));
        status(s, 0);
    });
}

} // namespace alchemy::gui::completion_kit::media
