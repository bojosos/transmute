#include "alchemy/darwin/media/music_core.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>

namespace alchemy::gui::completion_kit::media::music {
namespace {

constexpr double epsilon = 1e-9;

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
std::uint32_t get32(const std::vector<std::uint8_t>& in, std::size_t at) {
    std::uint32_t value{};
    if (at + 4 <= in.size()) std::memcpy(&value, in.data() + at, 4);
    return value;
}
float get_float(const std::vector<std::uint8_t>& in, std::size_t at) { return std::bit_cast<float>(get32(in, at)); }
double get_double(const std::vector<std::uint8_t>& in, std::size_t at) {
    std::uint64_t value{};
    if (at + 8 <= in.size()) std::memcpy(&value, in.data() + at, 8);
    return std::bit_cast<double>(value);
}

// Big-endian SMF reader.
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    bool failed{};
    std::uint8_t u8() { if (at >= bytes.size()) { failed = true; return 0; } return bytes[at++]; }
    std::uint32_t be(unsigned count) { std::uint32_t value = 0; while (count--) value = (value << 8) | u8(); return value; }
    std::uint32_t vlq() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const auto byte = u8();
            value = (value << 7) | (byte & 0x7f);
            if (!(byte & 0x80)) return value;
        }
        failed = true;
        return 0;
    }
    std::vector<std::uint8_t> take(std::size_t count) {
        if (at + count > bytes.size()) { failed = true; return {}; }
        std::vector<std::uint8_t> out(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + count));
        at += count;
        return out;
    }
};

void put_vlq(std::vector<std::uint8_t>& out, std::uint32_t value) {
    std::uint8_t buffer[5];
    int count = 0;
    buffer[count++] = value & 0x7f;
    while (value >>= 7) buffer[count++] = static_cast<std::uint8_t>((value & 0x7f) | 0x80);
    while (count) out.push_back(buffer[--count]);
}
void put_be(std::vector<std::uint8_t>& out, std::uint32_t value, unsigned count) {
    while (count--) out.push_back(static_cast<std::uint8_t>(value >> (8 * count)));
}

// Conductor meta events: tempo, SMPTE offset, time and key signature.
bool conductor_meta(std::uint8_t type) { return type == 0x51 || type == 0x54 || type == 0x58 || type == 0x59; }

} // namespace

std::vector<std::uint8_t> note_payload(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity, std::uint8_t release, float duration) {
    std::vector<std::uint8_t> out{channel, note, velocity, release};
    put32(out, std::bit_cast<std::uint32_t>(duration));
    return out;
}
std::vector<std::uint8_t> channel_payload(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) { return {status, data1, data2, 0}; }
std::vector<std::uint8_t> raw_payload(std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> out;
    put32(out, static_cast<std::uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
    if (bytes.empty()) out.push_back(0);   // the structure holds at least data[1]
    return out;
}
std::vector<std::uint8_t> meta_payload(std::uint8_t type, std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> out{type, 0, 0, 0};
    put32(out, static_cast<std::uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
    if (bytes.empty()) out.push_back(0);
    return out;
}
std::vector<std::uint8_t> tempo_payload(double bpm) {
    std::vector<std::uint8_t> out(8);
    std::memcpy(out.data(), &bpm, 8);
    return out;
}

void Track::insert(std::shared_ptr<Event> event) {
    const auto at = std::upper_bound(events.begin(), events.end(), event->time, [](double time, const std::shared_ptr<Event>& e) { return time < e->time; });
    events.insert(at, std::move(event));
}

double Track::end() const {
    if (length_set) return length;
    double last = 0;
    for (const auto& e : events) {
        double end = e->time;
        if (e->type == EventType::midi_note) end += get_float(e->data, 4);
        else if (e->type == EventType::extended_note) end += get_float(e->data, 8);
        last = std::max(last, end);
    }
    return last;
}

Sequence::Sequence() : tempo(std::make_shared<Track>()) {}

std::vector<Sequence::Segment> Sequence::tempo_map() const {
    std::vector<Segment> map{{0, 0, 120}};
    for (const auto& e : tempo->events) {
        if (e->type != EventType::extended_tempo) continue;
        const double bpm = get_double(e->data, 0);
        if (!(bpm > 0)) continue;
        const auto& last = map.back();
        const double beat = std::max(e->time, 0.0);
        if (beat <= last.beat + epsilon) { map.back().bpm = bpm; continue; }
        map.push_back({beat, last.seconds + (beat - last.beat) * 60.0 / last.bpm, bpm});
    }
    return map;
}

double Sequence::seconds_for_beats(double beats) const {
    const auto map = tempo_map();
    const Segment* segment = &map.front();
    for (const auto& s : map) if (s.beat <= beats) segment = &s;
    return segment->seconds + (beats - segment->beat) * 60.0 / segment->bpm;
}

double Sequence::beats_for_seconds(double seconds) const {
    const auto map = tempo_map();
    const Segment* segment = &map.front();
    for (const auto& s : map) if (s.seconds <= seconds) segment = &s;
    return segment->beat + (seconds - segment->seconds) * segment->bpm / 60.0;
}

std::vector<TimeSignature> Sequence::time_signatures() const {
    std::vector<TimeSignature> signatures{{0, 4, 4}};
    for (const auto& e : tempo->events) {
        if (e->type != EventType::meta || e->data.size() < 10 || e->data[0] != 0x58) continue;
        const std::uint32_t numerator = e->data[8], power = e->data[9];
        if (!numerator || power > 6) continue;
        const TimeSignature signature{std::max(e->time, 0.0), numerator, 1U << power};
        if (signature.beat <= signatures.back().beat + epsilon) signatures.back() = signature;
        else signatures.push_back(signature);
    }
    return signatures;
}

BarBeatTime Sequence::to_bar_beat(double beats, std::uint16_t subbeat_divisor) const {
    const auto signatures = time_signatures();
    double cursor = 0;
    std::int64_t bar = 0;
    auto current = signatures.front();
    for (std::size_t i = 1; i < signatures.size() && signatures[i].beat <= beats + epsilon; ++i) {
        const double per_bar = current.numerator * 4.0 / current.denominator;
        bar += static_cast<std::int64_t>(std::ceil((signatures[i].beat - cursor) / per_bar - epsilon));   // a change starts a bar
        cursor = signatures[i].beat;
        current = signatures[i];
    }
    const double per_bar = current.numerator * 4.0 / current.denominator;
    const double relative = beats - cursor;
    const auto bars = static_cast<std::int64_t>(std::floor(relative / per_bar + epsilon));
    const double position = (relative - static_cast<double>(bars) * per_bar) * current.denominator / 4.0;   // in beat units
    const double whole = std::floor(position + epsilon);
    BarBeatTime out;
    out.bar = static_cast<std::int32_t>(bar + bars + 1);
    out.beat = static_cast<std::uint16_t>(std::max(0.0, whole) + 1);
    out.subbeat_divisor = subbeat_divisor;
    out.subbeat = subbeat_divisor ? static_cast<std::uint16_t>(std::floor(std::max(0.0, position - whole) * subbeat_divisor + epsilon)) : 0;
    return out;
}

double Sequence::from_bar_beat(const BarBeatTime& time) const {
    const auto signatures = time_signatures();
    double cursor = 0;
    std::int64_t bar = 0;   // 0-based bar at `cursor`
    auto current = signatures.front();
    const std::int64_t wanted = static_cast<std::int64_t>(time.bar) - 1;
    for (std::size_t i = 1; i < signatures.size(); ++i) {
        const double per_bar = current.numerator * 4.0 / current.denominator;
        const auto bars = static_cast<std::int64_t>(std::ceil((signatures[i].beat - cursor) / per_bar - epsilon));
        if (bar + bars > wanted) break;
        bar += bars;
        cursor = signatures[i].beat;
        current = signatures[i];
    }
    const double per_bar = current.numerator * 4.0 / current.denominator;
    double units = static_cast<double>(time.beat) - 1;
    if (time.subbeat_divisor) units += static_cast<double>(time.subbeat) / time.subbeat_divisor;
    return cursor + static_cast<double>(wanted - bar) * per_bar + units * 4.0 / current.denominator;
}

bool Sequence::load_smf(std::span<const std::uint8_t> bytes, std::uint32_t flags, std::string& why) {
    Reader r{bytes};
    if (r.take(4) != std::vector<std::uint8_t>{'M', 'T', 'h', 'd'}) { why = "no MThd header"; return false; }
    const auto header_length = r.be(4);
    const auto format = r.be(2), track_count = r.be(2), division = r.be(2);
    if (r.failed || header_length < 6) { why = "short MThd"; return false; }
    r.at += header_length - 6;
    if (format > 2) { why = "SMF format " + std::to_string(format); return false; }
    if (division & 0x8000) { why = "SMPTE time division is unsupported"; return false; }
    if (division == 0) { why = "zero division"; return false; }
    const double ppq = division;

    auto new_tempo = std::make_shared<Track>();
    std::vector<std::shared_ptr<Track>> new_tracks;
    std::map<int, std::shared_ptr<Track>> channel_tracks;   // ChannelsToTracks
    auto shared_track = std::make_shared<Track>();           // ChannelsToTracks: meta and SysEx
    double last_end = 0;

    for (std::uint32_t index = 0; index < track_count && r.at + 8 <= bytes.size(); ++index) {
        const auto id = r.take(4);
        const auto length = r.be(4);
        if (r.failed || r.at + length > bytes.size()) { why = "truncated track chunk"; return false; }
        if (id != std::vector<std::uint8_t>{'M', 'T', 'r', 'k'}) { r.at += length; continue; }   // unknown chunks are skipped
        Reader t{bytes.subspan(r.at, length)};
        r.at += length;
        const bool conductor = format == 1 && index == 0;
        auto track = std::make_shared<Track>();
        std::uint64_t tick = 0, end_tick = 0;
        bool ended = false;
        std::uint8_t running = 0;
        std::map<int, std::deque<std::pair<std::uint64_t, std::shared_ptr<Event>>>> open;   // (channel << 8 | note)
        const auto destination = [&](int channel) -> std::shared_ptr<Track> {
            if (!(flags & load_channels_to_tracks)) return track;
            if (channel < 0) return shared_track;
            auto& slot = channel_tracks[channel];
            if (!slot) slot = std::make_shared<Track>();
            return slot;
        };
        const auto add = [&](const std::shared_ptr<Track>& to, EventType type, std::vector<std::uint8_t> data) {
            auto event = std::make_shared<Event>();
            event->time = static_cast<double>(tick) / ppq;
            event->type = type;
            event->data = std::move(data);
            to->insert(event);
            return event;
        };
        while (t.at < t.bytes.size() && !ended) {
            tick += t.vlq();
            auto status = t.u8();
            if (t.failed) break;
            if (status == 0xff) {
                const auto type = t.u8();
                const auto data = t.take(t.vlq());
                if (t.failed) break;
                if (type == 0x2f) { ended = true; end_tick = tick; break; }
                if (type == 0x51 && data.size() == 3) {
                    const auto microseconds = (std::uint32_t(data[0]) << 16) | (std::uint32_t(data[1]) << 8) | data[2];
                    if (microseconds) add(new_tempo, EventType::extended_tempo, tempo_payload(60e6 / microseconds));
                } else if (conductor || conductor_meta(type)) {
                    add(new_tempo, EventType::meta, meta_payload(type, data));
                } else {
                    add(destination(-1), EventType::meta, meta_payload(type, data));
                }
                continue;
            }
            if (status == 0xf0 || status == 0xf7) {
                auto data = t.take(t.vlq());
                if (t.failed) break;
                if (status == 0xf0) data.insert(data.begin(), 0xf0);
                add(destination(-1), EventType::midi_raw, raw_payload(data));
                running = 0;
                continue;
            }
            std::uint8_t first{};
            if (status & 0x80) { running = status; first = t.u8(); }
            else if (running) { first = status; status = running; }
            else { why = "data byte without running status"; return false; }
            const auto kind = status & 0xf0;
            const int channel = status & 0x0f;
            const std::uint8_t second = (kind == 0xc0 || kind == 0xd0) ? 0 : t.u8();
            if (t.failed) break;
            const auto key = (channel << 8) | first;
            if (kind == 0x90 && second > 0) {
                auto event = add(destination(channel), EventType::midi_note, note_payload(static_cast<std::uint8_t>(channel), first, second, 0, 0));
                open[key].emplace_back(tick, event);
            } else if (kind == 0x80 || kind == 0x90) {
                auto& pending = open[key];
                if (pending.empty()) continue;   // a note-off without its note-on
                const auto [on_tick, event] = pending.front();
                pending.pop_front();
                event->data = note_payload(static_cast<std::uint8_t>(channel), first, event->data[2], kind == 0x80 ? second : 0,
                                           static_cast<float>(static_cast<double>(tick - on_tick) / ppq));
            } else {
                add(destination(channel), EventType::midi_channel, channel_payload(status, first, second));
            }
        }
        if (t.failed) { why = "truncated event data"; return false; }
        if (!ended) end_tick = tick;
        for (auto& [key, pending] : open)
            for (auto& [on_tick, event] : pending)
                event->data = note_payload(event->data[0], event->data[1], event->data[2], 0, static_cast<float>(static_cast<double>(end_tick - on_tick) / ppq));
        const double end = static_cast<double>(end_tick) / ppq;
        last_end = std::max(last_end, end);
        if (!(flags & load_channels_to_tracks) && (!track->events.empty() || !conductor)) {
            track->length = std::max(end, track->end());
            track->length_set = true;
            if (!track->events.empty() || (flags & 2)) new_tracks.push_back(track);   // kMusicSequenceLoadSMF_PreserveTracks
        }
    }
    if (r.failed) { why = "truncated file"; return false; }
    if (flags & load_channels_to_tracks) {
        for (auto& [channel, track] : channel_tracks) new_tracks.push_back(track);
        new_tracks.push_back(shared_track);
        for (auto& track : new_tracks) { track->length = std::max(last_end, track->end()); track->length_set = true; }
    }
    new_tempo->length = last_end;
    new_tempo->length_set = true;
    tempo = new_tempo;
    tracks = std::move(new_tracks);
    resolution = static_cast<std::int16_t>(division);
    return true;
}

std::vector<std::uint8_t> Sequence::write_smf(std::int16_t requested) const {
    const std::uint32_t res = requested > 0 ? static_cast<std::uint32_t>(requested) : 480;
    const auto ticks = [res](double beats) { return static_cast<std::uint64_t>(std::llround(std::max(beats, 0.0) * res)); };
    std::vector<std::uint8_t> out{'M', 'T', 'h', 'd'};
    put_be(out, 6, 4);
    put_be(out, 1, 2);
    put_be(out, static_cast<std::uint32_t>(tracks.size() + 1), 2);
    put_be(out, res, 2);
    const auto chunk = [&](const Track& track) {
        struct Item { std::uint64_t tick; int order; std::uint64_t sequence; std::vector<std::uint8_t> bytes; };
        std::vector<Item> items;
        std::uint64_t sequence = 0;
        for (const auto& e : track.events) {
            const auto at = ticks(e->time);
            const auto& d = e->data;
            switch (e->type) {
            case EventType::midi_note: {
                if (d.size() < 8) break;
                const std::uint8_t channel = d[0] & 0x0f;
                items.push_back({at, 1, sequence++, {static_cast<std::uint8_t>(0x90 | channel), static_cast<std::uint8_t>(d[1] & 0x7f), static_cast<std::uint8_t>(std::max<int>(d[2] & 0x7f, 1))}});
                items.push_back({ticks(e->time + get_float(d, 4)), 0, sequence++, {static_cast<std::uint8_t>(0x80 | channel), static_cast<std::uint8_t>(d[1] & 0x7f), static_cast<std::uint8_t>(d[3] & 0x7f)}});
                break;
            }
            case EventType::midi_channel: {
                if (d.size() < 3 || !(d[0] & 0x80) || d[0] >= 0xf0) break;
                std::vector<std::uint8_t> bytes{d[0], static_cast<std::uint8_t>(d[1] & 0x7f)};
                if ((d[0] & 0xf0) != 0xc0 && (d[0] & 0xf0) != 0xd0) bytes.push_back(d[2] & 0x7f);
                items.push_back({at, 1, sequence++, bytes});
                break;
            }
            case EventType::midi_raw: {
                const auto length = get32(d, 0);
                if (!length || d.size() < 4 + length) break;
                std::vector<std::uint8_t> bytes;
                if (d[4] == 0xf0) { bytes.push_back(0xf0); put_vlq(bytes, length - 1); bytes.insert(bytes.end(), d.begin() + 5, d.begin() + 4 + length); }
                else { bytes.push_back(0xf7); put_vlq(bytes, length); bytes.insert(bytes.end(), d.begin() + 4, d.begin() + 4 + length); }
                items.push_back({at, 1, sequence++, bytes});
                break;
            }
            case EventType::meta: {
                const auto length = get32(d, 4);
                if (d.size() < 8 + length || d[0] == 0x2f) break;
                std::vector<std::uint8_t> bytes{0xff, d[0]};
                put_vlq(bytes, length);
                bytes.insert(bytes.end(), d.begin() + 8, d.begin() + 8 + length);
                items.push_back({at, 1, sequence++, bytes});
                break;
            }
            case EventType::extended_tempo: {
                const double bpm = get_double(d, 0);
                if (!(bpm > 0)) break;
                const auto microseconds = static_cast<std::uint32_t>(std::clamp(std::llround(60e6 / bpm), 1LL, 0xffffffLL));
                items.push_back({at, 1, sequence++, {0xff, 0x51, 0x03, static_cast<std::uint8_t>(microseconds >> 16),
                                                     static_cast<std::uint8_t>(microseconds >> 8), static_cast<std::uint8_t>(microseconds)}});
                break;
            }
            default: break;   // user, parameter, AU preset and extended note events have no SMF form
            }
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            return a.tick != b.tick ? a.tick < b.tick : a.order != b.order ? a.order < b.order : a.sequence < b.sequence;
        });
        std::vector<std::uint8_t> body;
        std::uint64_t now = 0;
        for (const auto& item : items) {
            put_vlq(body, static_cast<std::uint32_t>(item.tick - now));
            now = item.tick;
            body.insert(body.end(), item.bytes.begin(), item.bytes.end());
        }
        const auto end = std::max(now, ticks(track.end()));
        put_vlq(body, static_cast<std::uint32_t>(end - now));
        body.insert(body.end(), {0xff, 0x2f, 0x00});
        out.insert(out.end(), {'M', 'T', 'r', 'k'});
        put_be(out, static_cast<std::uint32_t>(body.size()), 4);
        out.insert(out.end(), body.begin(), body.end());
    };
    chunk(*tempo);
    for (const auto& track : tracks) chunk(*track);
    return out;
}

} // namespace alchemy::gui::completion_kit::media::music
