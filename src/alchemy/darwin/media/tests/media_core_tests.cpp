// Host-only tests of the QLab media group's cores (no guest): CMTime
// arithmetic and comparison, the CoreMIDI object model over a fake backend
// (enumeration, properties, ports, virtual endpoints, scheduling, the
// message splitter).
#include "alchemy/darwin/media/cmtime.hpp"
#include "alchemy/darwin/media/midi_core.hpp"
#include "alchemy/darwin/media/hal_core.hpp"
#include "alchemy/darwin/media/hal_host.hpp"
#include "alchemy/darwin/media/midi_host.hpp"
#include "alchemy/darwin/media/music_core.hpp"
#include "alchemy/darwin/media/audio_file_core.hpp"
#include <filesystem>
#include <fstream>

#include <bit>
#include <cstring>

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace cm = alchemy::gui::completion_kit::media;
namespace midi = alchemy::gui::completion_kit::media::midi;

void require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}
bool same(const cm::CMTime& a, std::int64_t value, std::int32_t timescale, std::uint32_t flags) {
    return a.value == value && a.timescale == timescale && a.flags == flags;
}

void cmtime_tests() {
    using namespace cm::cmtime_flags;
    const auto a = cm::make(1, 3), b = cm::make(1, 2);
    require(same(cm::add(a, b), 5, 6, valid), "1/3 + 1/2 = 5/6 (LCM timescale)");
    require(same(cm::subtract(a, b), -1, 6, valid), "1/3 - 1/2 = -1/6");
    require(same(cm::add(cm::make(3, 600), cm::make(7, 600)), 10, 600, valid), "same timescale adds values");
    require(cm::compare(a, b) == -1 && cm::compare(b, a) == 1 && cm::compare(cm::make(2, 4), b) == 0, "compare rational values");
    require(cm::compare(cm::make(-5, 1), cm::make(-4, 1)) == -1, "compare negatives");
    // Ordering of non-numeric times: -inf < numeric < indefinite < +inf < invalid.
    require(cm::compare(cm::time_negative_infinity, a) == -1, "-inf < numeric");
    require(cm::compare(cm::time_indefinite, a) == 1 && cm::compare(cm::time_indefinite, cm::time_positive_infinity) == -1, "indefinite between");
    require(cm::compare(cm::time_invalid, cm::time_positive_infinity) == 1 && cm::compare(cm::time_invalid, cm::time_invalid) == 0, "invalid greatest");
    require(cm::compare(cm::time_positive_infinity, cm::time_positive_infinity) == 0, "+inf == +inf");
    // Infinities and invalid operands.
    require(cm::add(cm::time_positive_infinity, a).flags == (valid | positive_infinity), "+inf + x");
    require(!cm::is_valid(cm::add(cm::time_positive_infinity, cm::time_negative_infinity)), "+inf + -inf is invalid");
    require(!cm::is_valid(cm::subtract(cm::time_positive_infinity, cm::time_positive_infinity)), "+inf - +inf is invalid");
    require(cm::subtract(a, cm::time_positive_infinity).flags == (valid | negative_infinity), "x - +inf = -inf");
    require(!cm::is_valid(cm::add(cm::time_invalid, a)), "invalid propagates");
    require(cm::add(cm::time_indefinite, a).flags == (valid | indefinite), "indefinite propagates");
    auto epoch = a;
    epoch.epoch = 1;
    require(!cm::is_valid(cm::add(epoch, b)), "different epochs are invalid");
    require(cm::compare(epoch, b) == 1, "later epoch compares greater");
    // Multiplication.
    require(same(cm::multiply(cm::make(3, 10), 4), 12, 10, valid), "multiply keeps the timescale");
    require(same(cm::multiply(cm::make(3, 10), -4), -12, 10, valid), "multiply by a negative");
    const auto big = cm::multiply(cm::make(INT64_MAX / 2, 1000), 4);
    require(cm::is_valid(big) && big.timescale < 1000 && (big.flags & rounded), "overflow halves the timescale and rounds");
    require(std::fabs(cm::seconds(big) - cm::seconds(cm::make(INT64_MAX / 2, 1000)) * 4) / cm::seconds(big) < 1e-9, "halved product keeps its value");
    require(same(cm::multiply_by_float64(cm::make(10, 30), 1.5), 15, 30, valid), "multiply by 1.5");
    require(same(cm::multiply_by_float64(cm::make(1, 30), 0.5), 1, 30, valid | rounded), "half rounds away from zero");
    require(same(cm::multiply_by_float64(cm::make(-1, 30), 0.5), -1, 30, valid | rounded), "negative half rounds away from zero");
    require(cm::multiply(cm::time_positive_infinity, -2).flags == (valid | negative_infinity), "inf * negative flips");
    // Seconds.
    require(same(cm::make_with_seconds(1.5, 600), 900, 600, valid), "1.5 s at 600");
    require(same(cm::make_with_seconds(0.25, 10), 3, 10, valid | rounded), "0.25 s at 10 rounds half away from zero");
    require(cm::make_with_seconds(1e30, 600).flags == (valid | positive_infinity), "huge seconds are +inf");
    require(!cm::is_valid(cm::make_with_seconds(std::nan(""), 600)), "NaN seconds are invalid");
    const auto halved = cm::make_with_seconds(1e16, 1000000000);
    require(cm::is_valid(halved) && halved.timescale < 1000000000 && std::fabs(cm::seconds(halved) - 1e16) < 1e3, "large seconds halve the timescale");
    require(std::isnan(cm::seconds(cm::time_invalid)) && std::isinf(cm::seconds(cm::time_positive_infinity)), "seconds of special times");
    // Ranges and descriptions.
    const auto range = cm::range_make(cm::make(1, 2), cm::make(3, 4));
    require(same(cm::range_end(range), 5, 4, valid), "range end = start + duration");
    require(!cm::is_valid(cm::range_end(cm::range_make(cm::time_invalid, b))), "invalid range has no end");
    require(cm::description(cm::make(3, 2)) == "{3/2 = 1.500}", "description: " + cm::description(cm::make(3, 2)));
    require(cm::description(cm::time_invalid) == "{INVALID}" && cm::description(cm::time_positive_infinity) == "{+INFINITY}", "special descriptions");
    require(same(cm::convert_scale(cm::make(1, 3), 10), 3, 10, valid | rounded), "convert scale rounds");
}

// ---- MIDI -------------------------------------------------------------------------------
struct Fake final : midi::Backend {
    struct Shared {
        std::mutex mutex;
        std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>> sent;
        std::map<std::size_t, Input> inputs;
        std::vector<std::size_t> closed;
    };
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    std::vector<midi::PortInfo> ins{{"Interface", "Maker", "drv", false}, {"Keyboard", "", "drv", false}};
    std::vector<midi::PortInfo> outs{{"Interface", "Maker", "drv", false}, {"Synth", "Maker", "drv", true}};
    std::vector<midi::PortInfo> inputs() override { return ins; }
    std::vector<midi::PortInfo> outputs() override { return outs; }
    bool open_input(std::size_t index, Input deliver) override {
        std::lock_guard lock(shared->mutex);
        shared->inputs[index] = std::move(deliver);
        return true;
    }
    void close_input(std::size_t index) override {
        std::lock_guard lock(shared->mutex);
        shared->inputs.erase(index);
        shared->closed.push_back(index);
    }
    bool send(std::size_t index, std::span<const std::uint8_t> message) override {
        std::lock_guard lock(shared->mutex);
        shared->sent.emplace_back(index, std::vector<std::uint8_t>(message.begin(), message.end()));
        return true;
    }
};

std::string text(const midi::Value& value) { return std::get<std::string>(value); }

void splitter_tests() {
    midi::MessageSplitter splitter;
    std::vector<std::vector<std::uint8_t>> out;
    const auto emit = [&](std::vector<std::uint8_t> message) { out.push_back(std::move(message)); };
    const std::vector<std::uint8_t> stream{0x90, 60, 100, 62, 90, 0xf8, 0xc0, 5, 0xf0, 1, 2};
    splitter.feed(stream, emit);
    require(out.size() == 4, "running status note, realtime, program change");
    require((out[0] == std::vector<std::uint8_t>{0x90, 60, 100}) && (out[1] == std::vector<std::uint8_t>{0x90, 62, 90}), "running status expands");
    require((out[2] == std::vector<std::uint8_t>{0xf8}) && (out[3] == std::vector<std::uint8_t>{0xc0, 5}), "realtime and program change");
    const std::vector<std::uint8_t> rest{3, 0xfe, 0xf7, 0xf6};
    splitter.feed(rest, emit);
    require(out.size() == 7 && (out[4] == std::vector<std::uint8_t>{0xfe}) && (out[5] == std::vector<std::uint8_t>{0xf0, 1, 2, 3, 0xf7}) &&
            (out[6] == std::vector<std::uint8_t>{0xf6}), "SysEx spans feeds; realtime inside SysEx; tune request");
}

void midi_tests() {
    auto fake = std::make_unique<Fake>();
    auto shared = fake->shared;
    std::atomic<std::uint64_t> clock{1000};
    midi::Server server(std::move(fake), [&] { return clock.load(); });
    std::mutex mutex;
    std::vector<midi::Delivery> deliveries;
    std::vector<std::pair<midi::ObjectRef, midi::Notification>> notes;
    server.set_sink([&](midi::Delivery delivery) { std::lock_guard lock(mutex); deliveries.push_back(std::move(delivery)); });
    server.set_notifier([&](midi::ObjectRef client, midi::Notification note) { std::lock_guard lock(mutex); notes.emplace_back(client, std::move(note)); });

    // Enumeration: "Interface" in/out pair into one device; "Synth" and "Keyboard" get their own.
    require(server.device_count() == 3, "three devices");
    const auto interface = server.device(0);
    midi::Value value;
    require(server.get(interface, midi::property::name, value) == 0 && text(value) == "Interface", "device name");
    require(server.child_count(interface, midi::ObjectType::entity) == 1, "one entity");
    const auto entity = server.child(interface, midi::ObjectType::entity, 0);
    require(server.child_count(entity, midi::ObjectType::source) == 1 && server.child_count(entity, midi::ObjectType::destination) == 1, "paired endpoints");
    const auto source = server.child(entity, midi::ObjectType::source, 0), destination = server.child(entity, midi::ObjectType::destination, 0);
    midi::ObjectRef parent{};
    require(server.parent(source, midi::ObjectType::entity, parent) == 0 && parent == entity, "endpoint entity");
    require(server.parent(entity, midi::ObjectType::device, parent) == 0 && parent == interface, "entity device");
    require(server.source_count() == 2 && server.destination_count() == 2, "two hardware sources and destinations");
    // Inheritance: the endpoint inherits the manufacturer, not the unique ID.
    require(server.get(source, midi::property::manufacturer, value) == 0 && text(value) == "Maker", "inherited manufacturer");
    midi::Value device_id, source_id;
    require(server.get(interface, midi::property::unique_id, device_id) == 0 && server.get(source, midi::property::unique_id, source_id) == 0 &&
            std::get<std::int32_t>(device_id) != std::get<std::int32_t>(source_id), "own unique IDs");
    require(server.get(source, midi::property::private_, value) == midi::error::unknown_property, "unset property");
    midi::ObjectRef found{};
    midi::ObjectType type{};
    require(server.find_unique_id(std::get<std::int32_t>(source_id), found, type) == 0 && found == source && type == midi::ObjectType::source, "find by unique ID");
    require(server.find_unique_id(0x7abcdef0, found, type) == midi::error::object_not_found && found == 0, "unknown unique ID");
    // Stable IDs: a second server over the same ports agrees.
    {
        midi::Server again(std::make_unique<Fake>(), [] { return std::uint64_t{0}; });
        midi::Value other;
        require(again.get(again.child(again.child(again.device(0), midi::ObjectType::entity, 0), midi::ObjectType::source, 0), midi::property::unique_id, other) == 0 &&
                std::get<std::int32_t>(other) == std::get<std::int32_t>(source_id), "hardware unique IDs are stable");
    }

    // Clients, ports, sends to hardware (split into messages).
    const auto client = server.create_client("test");
    midi::ObjectRef output{}, input{};
    require(server.create_port(client, "out", false, output) == 0 && server.create_port(client, "in", true, input) == 0, "ports");
    midi::ObjectRef bogus{};
    require(server.create_port(12345, "x", true, bogus) == midi::error::invalid_client, "bad client");
    require(server.send(input, destination, {}) == midi::error::invalid_port, "send needs an output port");
    require(server.send(output, source, {}) == midi::error::wrong_endpoint_type, "send needs a destination");
    require(server.send(output, destination, {{0, {0x90, 60, 100, 0x80, 60, 0}}}) == 0, "send");
    {
        std::lock_guard lock(shared->mutex);
        require(shared->sent.size() == 2 && shared->sent[0].first == 0 && shared->sent[1].second == std::vector<std::uint8_t>({0x80, 60, 0}), "two messages to output 0");
    }
    // Future timestamps are held until due; flush drops them.
    require(server.send(output, destination, {{5000, {0xb0, 7, 64}}}) == 0 && server.pending() == 1, "held packet");
    require(server.flush(destination) == 0 && server.pending() == 0, "flush drops it");
    require(server.send(output, destination, {{3000, {0xb0, 7, 1}}}) == 0, "held again");
    clock = 4000;
    for (int i = 0; i < 200 && server.pending(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    {
        std::lock_guard lock(shared->mutex);
        require(server.pending() == 0 && shared->sent.size() == 3 && shared->sent[2].second == std::vector<std::uint8_t>({0xb0, 7, 1}), "due packet sent by the scheduler");
    }
    // Hardware input: connecting opens the backend input; messages reach the port.
    require(server.connect(input, destination, 1) == midi::error::wrong_endpoint_type, "connect needs a source");
    require(server.connect(input, source, 0x55) == 0, "connect");
    {
        std::lock_guard lock(shared->mutex);
        require(shared->inputs.contains(0), "backend input opened");
        shared->inputs[0]({0x90, 1, 2}, 777);
    }
    {
        std::lock_guard lock(mutex);
        require(deliveries.size() == 1 && deliveries[0].target == input && deliveries[0].connection_refcon == 0x55 &&
                deliveries[0].packets[0].time == 777 && deliveries[0].packets[0].data == std::vector<std::uint8_t>({0x90, 1, 2}), "hardware input delivered");
    }
    require(server.disconnect(input, source) == 0 && server.disconnect(input, source) == midi::error::no_connection, "disconnect");
    { std::lock_guard lock(shared->mutex); require(!shared->closed.empty() && shared->closed.back() == 0, "backend input closed"); }

    // Virtual endpoints: notifications, routing, unique IDs.
    { std::lock_guard lock(mutex); notes.clear(); deliveries.clear(); }
    midi::ObjectRef virtual_source{}, virtual_destination{};
    require(server.create_virtual(client, "Out", true, virtual_source) == 0 && server.create_virtual(client, "In", false, virtual_destination) == 0, "virtual endpoints");
    require(server.source_count() == 3 && server.destination_count() == 3, "virtual endpoints are listed");
    {
        std::lock_guard lock(mutex);
        require(notes.size() == 4 && notes[0].second.kind == midi::Notification::Kind::object_added && notes[0].second.child == virtual_source &&
                notes[1].second.kind == midi::Notification::Kind::setup_changed, "added + setup changed");
    }
    require(server.parent(virtual_source, midi::ObjectType::entity, parent) == midi::error::object_not_found && parent == 0, "virtual endpoints have no entity");
    require(server.connect(input, virtual_source, 9) == 0 && server.received(virtual_source, {{1, {0xf8}}}) == 0, "MIDIReceived");
    require(server.received(source, {{1, {0xf8}}}) == midi::error::wrong_endpoint_type, "MIDIReceived needs a virtual source");
    require(server.send(output, virtual_destination, {{0, {0xc0, 1}}}) == 0, "send to a virtual destination");
    {
        std::lock_guard lock(mutex);
        require(deliveries.size() == 2 && deliveries[0].kind == midi::Delivery::Kind::input_port && deliveries[0].connection_refcon == 9 &&
                deliveries[1].kind == midi::Delivery::Kind::destination && deliveries[1].target == virtual_destination, "virtual routing");
    }
    require(server.set(virtual_source, midi::property::unique_id, std::get<std::int32_t>(source_id)) == midi::error::id_not_unique, "duplicate unique ID");
    require(server.set(virtual_source, midi::property::unique_id, std::int32_t{424242}) == 0, "set unique ID");
    require(server.set(virtual_source, midi::property::unique_id, std::string("x")) == midi::error::wrong_property_type, "unique ID is an integer");
    require(server.find_unique_id(424242, found, type) == 0 && found == virtual_source, "found by the new ID");
    std::map<std::string, midi::Value> all;
    require(server.properties(source, all) == 0 && all.contains("name") && all.contains("manufacturer") && all.contains("uniqueID"), "properties with inheritance");
    require(server.dispose_endpoint(source) == midi::error::not_permitted, "hardware endpoints cannot be disposed");
    require(server.dispose_endpoint(virtual_source) == 0 && server.source_count() == 2, "dispose virtual source");
    require(server.dispose_client(client) == 0 && server.destination_count() == 2, "client disposal removes its endpoints");
    require(server.create_port(client, "late", false, output) == midi::error::invalid_client, "disposed client");
}


// ---- CoreAudio HAL model ---------------------------------------------------------------
std::uint32_t u32(const alchemy::gui::completion_kit::media::hal::Value& value, std::size_t index = 0) {
    std::uint32_t out{};
    std::memcpy(&out, value.bytes.data() + index * 4, 4);
    return out;
}

void hal_tests() {
    namespace hal = alchemy::gui::completion_kit::media::hal;
    using hal::four;
    hal::Snapshot snapshot;
    snapshot.devices.push_back({"{out-1}", "Speakers", "", false, 2, 48000, 0x3, 1});
    snapshot.devices.push_back({"{in-1}", "Microphone", "Maker", true, 1, 44100, 0x4, 4});
    snapshot.devices.push_back({"{out-2}", "Surround", "", false, 6, 96000, 0x3f, 2});
    snapshot.default_output = "{out-1}";
    snapshot.default_input = "{in-1}";
    hal::Model model(snapshot);
    hal::Value v;
    require(model.get(hal::system_object, {four("dev#")}, std::nullopt, v) == 0 && v.bytes.size() == 12 && v.element_size == 4, "three devices");
    const auto speakers = u32(v, 0), microphone = u32(v, 1), surround = u32(v, 2);
    require(model.get(hal::system_object, {four("dOut")}, std::nullopt, v) == 0 && u32(v) == speakers, "default output");
    require(model.get(hal::system_object, {four("dIn ")}, std::nullopt, v) == 0 && u32(v) == microphone, "default input");
    require(model.get(hal::system_object, {four("uidd")}, std::string("{out-2}"), v) == 0 && u32(v) == surround, "UID to device");
    require(model.get(hal::system_object, {four("uidd")}, std::string("nope"), v) == 0 && u32(v) == 0, "unknown UID");
    require(model.get(speakers, {four("lnam")}, std::nullopt, v) == 0 && v.string == "Speakers", "name");
    require(model.get(speakers, {four("lmak")}, std::nullopt, v) == hal::error::unknown_property, "no manufacturer");
    require(model.get(microphone, {four("lmak")}, std::nullopt, v) == 0 && v.string == "Maker", "manufacturer");
    require(model.get(speakers, {four("uid ")}, std::nullopt, v) == 0 && v.string == "{out-1}", "UID");
    double rate{};
    require(model.get(microphone, {four("nsrt")}, std::nullopt, v) == 0 && (std::memcpy(&rate, v.bytes.data(), 8), rate == 44100), "nominal rate");
    // Streams by scope; stream configuration as an AudioBufferList.
    require(model.get(speakers, {four("stm#"), hal::scope_output}, std::nullopt, v) == 0 && v.bytes.size() == 4, "output stream");
    const auto stream = u32(v);
    require(model.get(speakers, {four("stm#"), hal::scope_input}, std::nullopt, v) == 0 && v.bytes.empty(), "no input stream");
    require(model.get(surround, {four("slay"), hal::scope_output}, std::nullopt, v) == 0 && v.bytes.size() == 24 && u32(v) == 1 && u32(v, 2) == 6, "6-channel layout");
    require(model.get(surround, {four("slay"), hal::scope_input}, std::nullopt, v) == 0 && v.bytes.size() == 8 && u32(v) == 0, "no input buffers");
    require(model.get(surround, {four("srnd"), hal::scope_output}, std::nullopt, v) == 0 && u32(v, 2) == 6 && u32(v, 3) == 1 && u32(v, 3 + 5 * 3) == 4, "speaker labels");
    require(model.get(speakers, {four("dflt"), hal::scope_output}, std::nullopt, v) == 0 && u32(v) == 1, "can be default output");
    require(model.get(speakers, {four("dflt"), hal::scope_input}, std::nullopt, v) == 0 && u32(v) == 0, "cannot be default input");
    require(model.kind(stream) == hal::Kind::stream, "stream object");
    require(model.get(stream, {four("sfmt")}, std::nullopt, v) == 0 && v.bytes.size() == 40 && u32(v, 2) == four("lpcm") && u32(v, 7) == 2, "virtual format");
    require(model.get(stream, {four("term")}, std::nullopt, v) == 0 && u32(v) == four("spkr"), "terminal type");
    require(model.get(stream, {four("stdv")}, std::nullopt, v) == 0 && u32(v) == speakers, "stream owner");
    // Errors and settable properties.
    require(model.get(0x7777, {four("lnam")}, std::nullopt, v) == hal::error::bad_object, "bad object");
    require(model.get(speakers, {four("zzzz")}, std::nullopt, v) == hal::error::unknown_property, "unknown property");
    bool settable{};
    require(model.settable(speakers, {four("fsiz")}, settable) == 0 && settable, "buffer size settable");
    require(model.settable(speakers, {four("nsrt")}, settable) == 0 && !settable, "rate not settable");
    std::vector<std::uint8_t> frames(4);
    const std::uint32_t wanted = 256;
    std::memcpy(frames.data(), &wanted, 4);
    require(model.set(speakers, {four("fsiz")}, frames) == 0 && model.get(speakers, {four("fsiz")}, std::nullopt, v) == 0 && u32(v) == 256, "buffer size kept");
    std::vector<std::uint8_t> id(4);
    std::memcpy(id.data(), &surround, 4);
    require(model.set(hal::system_object, {four("dOut")}, id) == hal::error::unsupported, "default device cannot change");
    std::memcpy(id.data(), &speakers, 4);
    require(model.set(hal::system_object, {four("dOut")}, id) == 0, "setting the current default");
    const double other_rate = 22050;
    std::vector<std::uint8_t> rate_bytes(8);
    std::memcpy(rate_bytes.data(), &other_rate, 8);
    require(model.set(speakers, {four("nsrt")}, rate_bytes) == hal::error::unsupported, "rate change unsupported");
    // An empty system.
    hal::Model empty(hal::Snapshot{});
    require(empty.get(hal::system_object, {four("dev#")}, std::nullopt, v) == 0 && v.bytes.empty(), "no devices");
    require(empty.get(hal::system_object, {four("dOut")}, std::nullopt, v) == 0 && u32(v) == 0, "no default");
}

} // namespace

// ---- MusicSequence (project-authored SMF fixture) ------------------------------------------
std::vector<std::uint8_t> fixture(const char* name) {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
    std::ifstream in(path, std::ios::binary);
    require(in.good(), "fixture missing: " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void music_tests() {
    namespace music = alchemy::gui::completion_kit::media::music;
    const auto bytes = fixture("two_tracks.mid");
    music::Sequence sequence;
    std::string why;
    require(sequence.load_smf(bytes, 0, why), "load: " + why);
    require(sequence.tracks.size() == 1 && sequence.resolution == 96, "one track at 96 PPQ");
    std::size_t tempos = 0;
    for (const auto& e : sequence.tempo->events) tempos += e->type == music::EventType::extended_tempo;
    require(tempos == 2, "two tempo events in the tempo track");
    require(std::fabs(sequence.seconds_for_beats(2) - 1.0) < 1e-12 && std::fabs(sequence.seconds_for_beats(5) - 3.0) < 1e-12, "seconds for beats across a tempo change");
    require(std::fabs(sequence.beats_for_seconds(3.0) - 5.0) < 1e-12, "beats for seconds");
    const auto& events = sequence.tracks[0]->events;
    require(events.size() == 6, "name, two notes, control, program, SysEx: " + std::to_string(events.size()));
    const auto note = [&](std::size_t i, std::uint8_t key, std::uint8_t velocity, std::uint8_t release, float duration) {
        const auto& e = *events[i];
        float got{};
        std::memcpy(&got, e.data.data() + 4, 4);
        return e.type == music::EventType::midi_note && e.data[1] == key && e.data[2] == velocity && e.data[3] == release && got == duration;
    };
    require(events[0]->type == music::EventType::meta && events[0]->data[0] == 0x03, "track name first (insertion order at time 0)");
    require(note(1, 60, 100, 64, 1.0f) && note(2, 64, 90, 0, 2.0f), "note durations and release velocities");
    require(events[3]->type == music::EventType::midi_channel && events[3]->time == 3.0 && events[3]->data[0] == 0xb0 && events[3]->data[2] == 0x50, "control change");
    require(events[4]->type == music::EventType::midi_channel && events[4]->data[0] == 0xc0 && events[4]->data[1] == 5, "program change");
    require(events[5]->type == music::EventType::midi_raw && events[5]->time == 4.0 && events[5]->data[0] == 6 && events[5]->data[4] == 0xf0 && events[5]->data[9] == 0xf7, "SysEx as raw data with F0");
    require(sequence.tracks[0]->end() == 5.0 && sequence.tempo->end() == 8.0, "track lengths from end of track");
    // Bars and beats in 3/4: beat 4 is bar 2, beat 2.
    const auto bbt = sequence.to_bar_beat(4.5, 4);
    require(bbt.bar == 2 && bbt.beat == 2 && bbt.subbeat == 2 && bbt.subbeat_divisor == 4, "beats to bar/beat");
    require(std::fabs(sequence.from_bar_beat(bbt) - 4.5) < 1e-12 && sequence.from_bar_beat({1, 1, 0, 0}) == 0, "bar/beat to beats");
    // Round trip through MusicSequenceFileCreateData's writer.
    const auto written = sequence.write_smf(240);
    music::Sequence again;
    require(again.load_smf(written, 0, why), "reload: " + why);
    require(again.resolution == 240 && again.tracks.size() == 1 && again.tracks[0]->events.size() == 6, "same events after a round trip");
    require(std::fabs(again.seconds_for_beats(5) - 3.0) < 1e-6 && again.tracks[0]->end() == 5.0, "tempo map and length survive");
    const auto& copy = again.tracks[0]->events;
    require(copy[5]->type == music::EventType::midi_raw && copy[5]->data == events[5]->data, "SysEx survives");
    // Channels to tracks: channel 0, then the meta/SysEx track.
    music::Sequence split;
    require(split.load_smf(bytes, music::load_channels_to_tracks, why) && split.tracks.size() == 2 &&
            split.tracks[0]->events.size() == 4 && split.tracks[1]->events.size() == 2, "ChannelsToTracks");
    // Malformed input.
    require(!split.load_smf(std::span(bytes).first(30), 0, why) && !why.empty(), "truncated file fails");
    const std::vector<std::uint8_t> junk{'R', 'I', 'F', 'F'};
    require(!split.load_smf(junk, 0, why), "not a MIDI file");
    // Insertion order and tempo-less defaults.
    music::Sequence empty;
    require(empty.seconds_for_beats(4) == 2.0 && empty.to_bar_beat(4, 0).bar == 2, "120 BPM, 4/4 by default");
}

// ---- AudioFile / ExtAudioFile core -------------------------------------------------------------
namespace wav {
void le(std::vector<std::uint8_t>& out, std::uint64_t value, int bytes) { for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
void be(std::vector<std::uint8_t>& out, std::uint64_t value, int bytes) { for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
void tag(std::vector<std::uint8_t>& out, const char* text) { out.insert(out.end(), text, text + 4); }
// A WAVE file: `format_tag` 1 (PCM) or 3 (float), or 0xfffe (extensible with `sub`).
std::vector<std::uint8_t> make(std::uint16_t format_tag, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits,
                               const std::vector<std::uint8_t>& samples, std::uint16_t sub = 1, std::uint32_t mask = 0) {
    std::vector<std::uint8_t> fmt;
    const std::uint16_t align = static_cast<std::uint16_t>(channels * bits / 8);
    le(fmt, format_tag, 2); le(fmt, channels, 2); le(fmt, rate, 4); le(fmt, rate * align, 4); le(fmt, align, 2); le(fmt, bits, 2);
    if (format_tag == 0xfffe) {
        le(fmt, 22, 2); le(fmt, bits, 2); le(fmt, mask, 4); le(fmt, sub, 2);
        const std::uint8_t guid_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
        fmt.insert(fmt.end(), guid_tail, guid_tail + 14);
    }
    std::vector<std::uint8_t> out;
    tag(out, "RIFF"); le(out, 4 + 8 + fmt.size() + 8 + 4 + 8 + samples.size(), 4); tag(out, "WAVE");
    tag(out, "fmt "); le(out, fmt.size(), 4); out.insert(out.end(), fmt.begin(), fmt.end());
    tag(out, "LIST"); le(out, 4, 4); tag(out, "INFO");   // a chunk to skip
    tag(out, "data"); le(out, samples.size(), 4); out.insert(out.end(), samples.begin(), samples.end());
    return out;
}
} // namespace wav

void audio_file_tests() {
    namespace af = alchemy::gui::completion_kit::media::audio_file;
    const auto parse = [](const std::vector<std::uint8_t>& file, af::Container& out) {
        std::string why;
        return af::parse([&](std::uint64_t offset, std::span<std::uint8_t> bytes) {
            const auto count = offset >= file.size() ? 0 : std::min<std::size_t>(bytes.size(), file.size() - offset);
            std::memcpy(bytes.data(), file.data() + offset, count);
            return count;
        }, file.size(), out, why);
    };
    const auto data_of = [](const std::vector<std::uint8_t>& file, const af::Container& c) {
        return std::span<const std::uint8_t>(file.data() + c.data_offset, static_cast<std::size_t>(c.data_bytes));
    };
    // 16-bit stereo PCM: full scale, zero, negative full scale.
    std::vector<std::uint8_t> samples;
    for (const std::int16_t v : {std::int16_t{16384}, std::int16_t{-16384}, std::int16_t{0}, std::int16_t{32767}, std::int16_t{-32768}, std::int16_t{8192}}) wav::le(samples, static_cast<std::uint16_t>(v), 2);
    const auto pcm16 = wav::make(1, 2, 44100, 16, samples);
    af::Container c;
    require(parse(pcm16, c) == 0 && c.file_type == af::four("WAVE") && c.format.channels == 2 && c.format.bits == 16 && c.data_bytes == 12, "16-bit WAVE");
    require(c.format.flags() == (af::flag::signed_integer | af::flag::packed) && c.format.frame_bytes() == 4, "WAVE flags");
    auto decoded = af::decode(c.format, {data_of(pcm16, c)}, 3);
    require(decoded[0] == 0.5 && decoded[1] == -0.5 && decoded[4] == -1.0 && std::fabs(decoded[3] - 32767.0 / 32768) < 1e-12, "16-bit decode");
    // To non-interleaved float32: two buffers.
    af::Pcm client = c.format;
    client.is_float = true; client.bits = 32; client.container = 4; client.non_interleaved = true;
    std::vector<std::uint8_t> left(12), right(12);
    af::encode(client, decoded, 2, 3, {std::span<std::uint8_t>(left), std::span<std::uint8_t>(right)});
    float l0{}, r2{};
    std::memcpy(&l0, left.data(), 4);
    std::memcpy(&r2, right.data() + 8, 4);
    require(l0 == 0.5f && r2 == 0.25f, "float32 non-interleaved encode");
    // ASBD round trip.
    std::string why;
    const auto asbd = af::asbd_from_pcm(client);
    const auto back = af::pcm_from_asbd(asbd, why);
    require(back && back->is_float && back->non_interleaved && back->container == 4 && back->channels == 2, "ASBD round trip");
    require(af::convertible(c.format, client) && !af::convertible(c.format, [&] { auto other = client; other.rate = 48000; return other; }()), "no sample-rate conversion");
    // Stereo to mono averages; integer output rounds and clips.
    af::Pcm mono16 = c.format;
    mono16.channels = 1;
    std::vector<std::uint8_t> mono(6);
    af::encode(mono16, decoded, 2, 3, {std::span<std::uint8_t>(mono)});
    require(mono[0] == 0 && mono[1] == 0, "stereo average (0.5 + -0.5)");
    // 24-bit extensible mono with a speaker mask, 8-bit unsigned, float32.
    std::vector<std::uint8_t> s24;
    wav::le(s24, 0x400000, 3); wav::le(s24, 0xc00000, 3);
    const auto ext24 = wav::make(0xfffe, 1, 48000, 24, s24, 1, 0x4);
    require(parse(ext24, c) == 0 && c.format.bits == 24 && c.format.container == 3 && c.channel_mask == 4, "24-bit extensible");
    decoded = af::decode(c.format, {data_of(ext24, c)}, 2);
    require(decoded[0] == 0.5 && decoded[1] == -0.5, "24-bit decode");
    require(af::layout_for_mask(4, 1).size() == 12 + 20, "channel layout from the mask");
    const auto u8 = wav::make(1, 1, 8000, 8, {0x80, 0xc0, 0x00});
    require(parse(u8, c) == 0 && !c.format.is_signed, "8-bit WAVE is unsigned");
    decoded = af::decode(c.format, {data_of(u8, c)}, 3);
    require(decoded[0] == 0 && decoded[1] == 0.5 && decoded[2] == -1, "unsigned decode");
    std::vector<std::uint8_t> f32;
    wav::le(f32, std::bit_cast<std::uint32_t>(0.25f), 4);
    const auto float_wave = wav::make(3, 1, 96000, 32, f32);
    require(parse(float_wave, c) == 0 && c.format.is_float && af::decode(c.format, {data_of(float_wave, c)}, 1)[0] == 0.25, "float WAVE");
    require(parse(wav::make(0x55, 2, 44100, 0, {}), c) == af::error::unsupported_data_format, "MP3-in-WAVE is unsupported");
    // AIFF (big-endian, 80-bit rate) and AIFC 'sowt'.
    const auto aiff = [](const char* kind, const char* compression, std::vector<std::uint8_t> sound) {
        std::vector<std::uint8_t> comm;
        wav::be(comm, 1, 2); wav::be(comm, sound.size() / 2, 4); wav::be(comm, 16, 2);
        const std::uint8_t rate44100[10] = {0x40, 0x0e, 0xac, 0x44, 0, 0, 0, 0, 0, 0};
        comm.insert(comm.end(), rate44100, rate44100 + 10);
        if (compression) { wav::tag(comm, compression); comm.push_back(0); comm.push_back(0); }
        std::vector<std::uint8_t> out;
        wav::tag(out, "FORM"); wav::be(out, 4 + 8 + comm.size() + 8 + 8 + sound.size(), 4); wav::tag(out, kind);
        wav::tag(out, "COMM"); wav::be(out, comm.size(), 4); out.insert(out.end(), comm.begin(), comm.end());
        wav::tag(out, "SSND"); wav::be(out, 8 + sound.size(), 4); wav::be(out, 0, 4); wav::be(out, 0, 4);
        out.insert(out.end(), sound.begin(), sound.end());
        return out;
    };
    const auto big = aiff("AIFF", nullptr, {0x40, 0x00, 0xc0, 0x00});
    require(parse(big, c) == 0 && c.file_type == af::four("AIFF") && c.format.big_endian && c.format.rate == 44100, "AIFF header");
    decoded = af::decode(c.format, {data_of(big, c)}, 2);
    require(decoded[0] == 0.5 && decoded[1] == -0.5, "AIFF decode");
    const auto sowt = aiff("AIFC", "sowt", {0x00, 0x40, 0x00, 0xc0});
    require(parse(sowt, c) == 0 && c.file_type == af::four("AIFC") && !c.format.big_endian && af::decode(c.format, {data_of(sowt, c)}, 2)[0] == 0.5, "AIFC sowt");
    require(parse(aiff("AIFC", "ima4", {0, 0}), c) == af::error::unsupported_data_format, "IMA4 is compressed");
    // CAF, little-endian float32 stereo.
    std::vector<std::uint8_t> caf;
    wav::tag(caf, "caff"); wav::be(caf, 1, 2); wav::be(caf, 0, 2);
    wav::tag(caf, "desc"); wav::be(caf, 32, 8);
    wav::be(caf, std::bit_cast<std::uint64_t>(48000.0), 8); wav::tag(caf, "lpcm"); wav::be(caf, 3, 4); wav::be(caf, 8, 4); wav::be(caf, 1, 4); wav::be(caf, 2, 4); wav::be(caf, 32, 4);
    wav::tag(caf, "data"); wav::be(caf, 4 + 8, 8); wav::be(caf, 0, 4);
    wav::le(caf, std::bit_cast<std::uint32_t>(-0.75f), 4); wav::le(caf, std::bit_cast<std::uint32_t>(0.125f), 4);
    require(parse(caf, c) == 0 && c.file_type == af::four("caff") && c.format.is_float && !c.format.big_endian && c.data_bytes == 8, "CAF");
    decoded = af::decode(c.format, {data_of(caf, c)}, 1);
    require(decoded[0] == -0.75 && decoded[1] == 0.125, "CAF decode");
    require(parse({'O', 'g', 'g', 'S', 0, 0, 0, 0, 0, 0, 0, 0}, c) == af::error::unsupported_file_type, "unknown container");
}

// ALCHEMY_MEDIA_LIST_HOST_DEVICES=1: list this machine's audio endpoints and MIDI ports
// (read-only: no stream or port is opened).
void list_host_devices() {
    const auto snapshot = alchemy::gui::completion_kit::media::hal::make_host_backend()->snapshot();
    for (const auto& d : snapshot.devices)
        std::cout << (d.input ? "audio in : " : "audio out: ") << d.name << " [" << d.channels << " ch, " << d.sample_rate << " Hz, form " << d.form_factor << "] " << d.uid << "\n";
    std::cout << "default output " << snapshot.default_output << "\ndefault input " << snapshot.default_input << "\n";
    auto midi_backend = alchemy::gui::completion_kit::media::midi::make_host_backend();
    for (const auto& port : midi_backend->inputs()) std::cout << "midi in  : " << port.name << "\n";
    for (const auto& port : midi_backend->outputs()) std::cout << "midi out : " << port.name << (port.synthesizer ? " (synth)" : "") << "\n";
}

int main() {
    try {
        if (std::getenv("ALCHEMY_MEDIA_LIST_HOST_DEVICES")) list_host_devices();
        cmtime_tests();
        splitter_tests();
        midi_tests();
        hal_tests();
        music_tests();
        audio_file_tests();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "qlab media core tests passed\n";
    return 0;
}
