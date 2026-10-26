// Host-only tests of the QLab media group's cores (no guest): CMTime
// arithmetic and comparison, the CoreMIDI object model over a fake backend
// (enumeration, properties, ports, virtual endpoints, scheduling, the
// message splitter).
#include "alchemy/darwin/media/cmtime.hpp"
#include "alchemy/darwin/media/midi_core.hpp"
#include "alchemy/darwin/media/midi_ump.hpp"
#include "alchemy/darwin/media/media_container.hpp"
#include "alchemy/darwin/media/hal_core.hpp"
#include "alchemy/darwin/media/hal_host.hpp"
#include "alchemy/darwin/media/midi_host.hpp"
#include "alchemy/darwin/media/music_core.hpp"
#include "alchemy/darwin/media/audio_file_core.hpp"
#include "alchemy/darwin/media/resampler.hpp"
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
#include <memory>
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

// The resampler: a 1 kHz sine from 44.1 kHz to 48 kHz keeps its amplitude and frequency, a
// 15 kHz tone from 48 kHz to 22.05 kHz is removed (above the new Nyquist frequency), and equal
// rates copy.
void resampler_tests() {
    const auto tone = [](double frequency, double rate) {
        auto phase = std::make_shared<double>(0);
        return [frequency, rate, phase](double* out, std::size_t frames) {
            for (std::size_t i = 0; i < frames; ++i) {
                out[i] = std::sin(2 * 3.14159265358979323846 * frequency * *phase / rate);
                *phase += 1;
            }
        };
    };
    cm::Resampler up(44100, 48000, 1);
    std::vector<double> out(4800);
    const auto pull = tone(1000, 44100);
    for (int block = 0; block < 4; ++block) up.render(out.data() + block * 1200, 1200, pull);
    double peak = 0;
    int crossings = 0;
    for (std::size_t i = 600; i < out.size(); ++i) {
        peak = std::max(peak, std::abs(out[i]));
        if ((out[i - 1] < 0) != (out[i] < 0)) ++crossings;
    }
    require(std::abs(peak - 1) < 0.01, "1 kHz keeps its amplitude");
    require(std::abs(crossings - 175) <= 2, "1 kHz keeps its frequency (4200 frames at 48 kHz: 175 zero crossings)");
    cm::Resampler down(48000, 22050, 1);
    std::vector<double> low(2205);
    down.render(low.data(), low.size(), tone(15000, 48000));
    double residue = 0;
    for (std::size_t i = 200; i < low.size(); ++i) residue = std::max(residue, std::abs(low[i]));
    require(residue < 0.01, "a tone above the new Nyquist frequency is removed");
    cm::Resampler same_rate(48000, 48000, 2);
    std::vector<double> copied(8);
    same_rate.render(copied.data(), 4, [](double* in, std::size_t frames) { for (std::size_t i = 0; i < frames * 2; ++i) in[i] = static_cast<double>(i); });
    require(copied[7] == 7, "equal rates copy");
}


// Universal MIDI Packets <-> MIDI 1.0 bytes.
void ump_tests() {
    namespace ump = midi::ump;
    require(ump::scale(127, 7, 16) == 0xffff && ump::scale(64, 7, 16) == 0x8000 && ump::scale(0, 7, 16) == 0, "7 to 16 bits");
    require(ump::scale(127, 7, 32) == 0xffffffffU && ump::scale(0x2000, 14, 32) == 0x80000000U && ump::scale(0xffffffffU, 32, 7) == 127, "scaling");
    // Protocol 1.0: channel voice as type 2, real time as type 1, SysEx as 7-bit data packets.
    const std::vector<std::uint8_t> bytes{0x90, 60, 100, 0xf8, 0xb3, 7, 64, 0xf0, 1, 2, 3, 4, 5, 6, 7, 0xf7, 0xc1, 5};
    const auto words = ump::from_bytes(bytes, ump::protocol_1_0);
    require(words.size() == 1 + 1 + 1 + 4 + 1 && words[0] == 0x20903c64 && words[1] == 0x10f80000 && words[2] == 0x20b30740 &&
            words[3] == 0x30160102 && words[4] == 0x03040506 && words[5] == 0x30310700 && words[7] == 0x20c10500, "MIDI 1.0 packets");
    require(ump::to_bytes(words) == bytes, "and back");
    // Protocol 2.0: MIDI 2.0 channel voice; a note on of velocity 0 is a note off.
    const std::vector<std::uint8_t> voice{0x91, 60, 127, 0x91, 60, 0, 0xe0, 0, 0x40};
    const auto two = ump::from_bytes(voice, ump::protocol_2_0);
    require(two.size() == 6 && two[0] == 0x40913c00 && two[1] == 0xffff0000U && two[2] == 0x40813c00 && two[3] == 0x80000000U &&
            two[4] == 0x40e00000 && two[5] == 0x80000000U, "MIDI 2.0 packets");
    require(ump::to_bytes(two) == std::vector<std::uint8_t>{0x91, 60, 127, 0x81, 60, 64, 0xe0, 0, 0x40}, "MIDI 2.0 down to 1.0");
    // A program change with its bank: bank select, then the program; per-note messages are dropped.
    const std::vector<std::uint32_t> program{0x40c20001, 0x05000302, 0x40020000, 0x12345678};
    require(ump::to_bytes(program) == std::vector<std::uint8_t>{0xb2, 0, 3, 0xb2, 32, 2, 0xc2, 5}, "bank and program");
    require(ump::word_count(0x50000000) == 4 && ump::word_count(0x40000000) == 2 && ump::word_count(0x20000000) == 1, "word counts");
}

// Every container Alchemy writes reads back with its format.
void audio_file_writing_tests() {
    namespace af = cm::audio_file;
    struct Case { std::uint32_t type; af::Pcm pcm; };
    std::vector<Case> cases;
    af::Pcm s16; s16.rate = 44100; s16.channels = 2; s16.bits = 16; s16.container = 2;
    cases.push_back({af::four("WAVE"), s16});
    af::Pcm f32 = s16; f32.is_float = true; f32.bits = 32; f32.container = 4; f32.channels = 6;
    cases.push_back({af::four("WAVE"), f32});
    af::Pcm be24 = s16; be24.big_endian = true; be24.bits = 24; be24.container = 3;
    cases.push_back({af::four("AIFF"), be24});
    af::Pcm bef = f32; bef.big_endian = true; bef.channels = 1;
    cases.push_back({af::four("AIFC"), bef});
    af::Pcm sowt = s16;
    cases.push_back({af::four("AIFC"), sowt});
    cases.push_back({af::four("caff"), f32});
    for (const auto& c : cases) {
        std::vector<std::uint8_t> file;
        std::string why;
        require(af::write_header(c.type, c.pcm, 4 * c.pcm.frame_bytes(), {}, file, why) == 0, "write_header: " + why);
        const auto header = file.size();
        file.resize(header + 4 * c.pcm.frame_bytes());
        af::Container container;
        const auto read = [&file](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
            if (offset >= file.size()) return 0;
            const auto n = std::min<std::size_t>(out.size(), file.size() - static_cast<std::size_t>(offset));
            std::memcpy(out.data(), file.data() + offset, n);
            return n;
        };
        require(af::parse(read, file.size(), container, why) == 0, "parse: " + why);
        const auto& f = container.format;
        require(container.file_type == c.type && container.data_offset == header && container.data_bytes == 4 * c.pcm.frame_bytes() &&
                f.rate == c.pcm.rate && f.channels == c.pcm.channels && f.bits == c.pcm.bits && f.is_float == c.pcm.is_float &&
                f.big_endian == c.pcm.big_endian, "a written header reads back");
    }
    af::Pcm le32 = s16; le32.bits = 32; le32.container = 4;
    std::vector<std::uint8_t> out;
    std::string why;
    require(af::write_header(af::four("AIFF"), le32, 0, {}, out, why) == af::error::unsupported_data_format, "AIFF holds big-endian only");
    require(af::write_header(af::four("MPG3"), s16, 0, {}, out, why) == af::error::unsupported_file_type, "no MP3 container");
}

// ---- containers: files built by small muxers here, parsed back -------------------------------------
namespace mc = cm::container;
using Bytes = std::vector<std::uint8_t>;
void put(Bytes& out, std::uint64_t value, int bytes) { for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
void put_le(Bytes& out, std::uint64_t value, int bytes) { for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
void append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }
Bytes box(const char* type, const Bytes& payload) {
    Bytes out;
    put(out, 8 + payload.size(), 4);
    out.insert(out.end(), type, type + 4);
    append(out, payload);
    return out;
}
Bytes full(int version, std::uint32_t flags, const Bytes& rest) { Bytes out; put(out, static_cast<std::uint64_t>(version), 1); put(out, flags, 3); append(out, rest); return out; }
mc::ReadAt reader(const Bytes& file) {
    return [&file](std::uint64_t offset, std::span<std::uint8_t> out) -> std::size_t {
        if (offset >= file.size()) return 0;
        const auto n = std::min<std::size_t>(out.size(), file.size() - static_cast<std::size_t>(offset));
        std::memcpy(out.data(), file.data() + offset, n);
        return n;
    };
}

// A video track (av01, 3 samples, the second not a sync sample) and an Opus track (2 samples).
Bytes iso_file(bool fragmented) {
    const auto visual = [] {
        Bytes e(6, 0); put(e, 1, 2);            // reserved, data_reference_index
        append(e, Bytes(16, 0));                 // pre_defined/reserved
        put(e, 320, 2); put(e, 240, 2);          // width, height
        put(e, 0x00480000, 4); put(e, 0x00480000, 4); put(e, 0, 4); put(e, 1, 2);
        append(e, Bytes(32, 0)); put(e, 24, 2); put(e, 0xffff, 2);
        append(e, box("av1C", Bytes{0x81, 0x00, 0x0c, 0x00}));
        return box("av01", e);
    }();
    const auto audio = [] {
        Bytes e(6, 0); put(e, 1, 2);
        append(e, Bytes(8, 0)); put(e, 2, 2); put(e, 16, 2); put(e, 0, 4); put(e, 48000ULL << 16, 4);
        Bytes dops{0x00, 0x02}; put(dops, 312, 2); put(dops, 48000, 4); put(dops, 0, 2); put(dops, 0, 1);
        append(e, box("dOps", dops));
        return box("Opus", e);
    }();
    const auto trak = [&](std::uint32_t id, const char* handler, const Bytes& entry, std::uint32_t timescale, std::vector<std::uint32_t> sizes,
                          std::uint32_t delta, std::uint32_t chunk_offset, bool sync_table) {
        Bytes tkhd(8, 0); put(tkhd, id, 4); append(tkhd, Bytes(68, 0));
        Bytes mdhd(8, 0); put(mdhd, timescale, 4); put(mdhd, 0, 4); put(mdhd, 0, 4);
        Bytes hdlr(4, 0); hdlr.insert(hdlr.end(), handler, handler + 4); append(hdlr, Bytes(13, 0));
        Bytes stsd; put(stsd, 1, 4); append(stsd, entry);
        Bytes stts, stsc, stsz, stco, stss;
        const auto n = static_cast<std::uint32_t>(fragmented ? 0 : sizes.size());
        put(stts, n ? 1 : 0, 4); if (n) { put(stts, n, 4); put(stts, delta, 4); }
        put(stsc, n ? 1 : 0, 4); if (n) { put(stsc, 1, 4); put(stsc, n, 4); put(stsc, 1, 4); }
        put(stsz, 0, 4); put(stsz, n, 4); for (std::uint32_t i = 0; i < n; ++i) put(stsz, sizes[i], 4);
        put(stco, n ? 1 : 0, 4); if (n) put(stco, chunk_offset, 4);
        put(stss, 2, 4); put(stss, 1, 4); put(stss, 3, 4);
        Bytes stbl;
        append(stbl, box("stsd", full(0, 0, stsd))); append(stbl, box("stts", full(0, 0, stts))); append(stbl, box("stsc", full(0, 0, stsc)));
        append(stbl, box("stsz", full(0, 0, stsz))); append(stbl, box("stco", full(0, 0, stco)));
        if (sync_table && !fragmented) append(stbl, box("stss", full(0, 0, stss)));
        Bytes elst; put(elst, 1, 4); put(elst, 0, 4); put(elst, id == 2 ? 312 : 0, 4); put(elst, 0x10000, 4);
        Bytes trak_payload = box("tkhd", full(0, 3, tkhd));
        append(trak_payload, box("edts", box("elst", full(0, 0, elst))));
        Bytes mdia = box("mdhd", full(0, 0, mdhd));
        append(mdia, box("hdlr", full(0, 0, hdlr)));
        append(mdia, box("minf", box("stbl", stbl)));
        append(trak_payload, box("mdia", mdia));
        return box("trak", trak_payload);
    };
    Bytes brand{'i', 's', 'o', 'm'}; put(brand, 0, 4);
    Bytes file = box("ftyp", brand);
    // mdat first: video samples of 10, 20, 30 bytes, then audio of 5 and 7.
    Bytes data;
    for (int i = 0; i < 60; ++i) data.push_back(static_cast<std::uint8_t>(i));
    for (int i = 0; i < 12; ++i) data.push_back(static_cast<std::uint8_t>(0x80 + i));
    const auto mdat_payload_offset = static_cast<std::uint32_t>(file.size() + 8);
    append(file, box("mdat", data));
    Bytes mvhd(8, 0); put(mvhd, 1000, 4); put(mvhd, 100, 4); append(mvhd, Bytes(80, 0));
    Bytes moov = box("mvhd", full(0, 0, mvhd));
    append(moov, trak(1, "vide", visual, 90000, {10, 20, 30}, 3000, mdat_payload_offset, true));
    append(moov, trak(2, "soun", audio, 48000, {5, 7}, 960, mdat_payload_offset + 60, false));
    if (fragmented) {
        Bytes mvex;
        for (std::uint32_t id : {1U, 2U}) {
            Bytes trex; put(trex, id, 4); put(trex, 1, 4); put(trex, id == 1 ? 3000 : 960, 4); put(trex, 0, 4); put(trex, 0x10000, 4);
            append(mvex, box("trex", full(0, 0, trex)));
        }
        append(moov, box("mvex", mvex));
    }
    append(file, box("moov", moov));
    if (fragmented) {
        // One moof: the video samples (the first a sync sample through first-sample-flags), then audio, both
        // with data offsets from the moof's start back into the mdat above.
        const auto moof_offset = static_cast<std::int64_t>(file.size());
        const auto traf = [&](std::uint32_t id, std::vector<std::uint32_t> sizes, std::int64_t data_start, bool first_flags) {
            Bytes tfhd; put(tfhd, id, 4);
            Bytes tfdt; put(tfdt, id == 1 ? 6000 : 0, 4);
            Bytes trun; put(trun, sizes.size(), 4); put(trun, static_cast<std::uint32_t>(data_start - moof_offset), 4);
            if (first_flags) put(trun, 0, 4);
            for (const auto size : sizes) put(trun, size, 4);
            Bytes t = box("tfhd", full(0, 0x20000, tfhd));
            append(t, box("tfdt", full(0, 0, tfdt)));
            append(t, box("trun", full(0, 0x1 | 0x200 | (first_flags ? 0x4 : 0), trun)));
            return box("traf", t);
        };
        Bytes moof = box("mfhd", full(0, 0, Bytes{0, 0, 0, 1}));
        append(moof, traf(1, {10, 20, 30}, mdat_payload_offset, true));
        append(moof, traf(2, {5, 7}, mdat_payload_offset + 60, false));
        append(file, box("moof", moof));
    }
    return file;
}

// An EBML element with an 8-byte size (or the unknown size).
Bytes ebml(std::uint32_t id, const Bytes& payload, bool unknown = false) {
    Bytes out;
    bool started = false;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const auto b = static_cast<std::uint8_t>(id >> shift);
        if (!started && !b) continue;
        started = true;
        out.push_back(b);
    }
    if (unknown) out.push_back(0xff);
    else { out.push_back(0x01); put(out, payload.size(), 7); }
    append(out, payload);
    return out;
}
Bytes ebml_uint(std::uint32_t id, std::uint64_t value) { Bytes v; put(v, value, 4); return ebml(id, v); }

Bytes webm_file() {
    Bytes header = ebml(0x1A45DFA3, ebml(0x4282, Bytes{'w', 'e', 'b', 'm'}));
    Bytes info_payload = ebml_uint(0x2AD7B1, 1000000);
    Bytes duration; put(duration, std::bit_cast<std::uint32_t>(80.0f), 4);
    append(info_payload, ebml(0x4489, duration));
    Bytes info = ebml(0x1549A966, info_payload);
    Bytes opus_head{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2}; put_le(opus_head, 312, 2); put_le(opus_head, 48000, 4); put_le(opus_head, 0, 2); opus_head.push_back(0);
    Bytes video_entry = ebml_uint(0xD7, 1);
    append(video_entry, ebml_uint(0x83, 1));
    append(video_entry, ebml(0x86, Bytes{'V', '_', 'V', 'P', '9'}));
    Bytes video = ebml_uint(0xB0, 64); append(video, ebml_uint(0xBA, 48));
    append(video_entry, ebml(0xE0, video));
    Bytes audio_entry = ebml_uint(0xD7, 2);
    append(audio_entry, ebml_uint(0x83, 2));
    append(audio_entry, ebml(0x86, Bytes{'A', '_', 'O', 'P', 'U', 'S'}));
    append(audio_entry, ebml(0x63A2, opus_head));
    append(audio_entry, ebml_uint(0x23E383, 20000000));   // 20 ms frames
    Bytes audio = ebml_uint(0x9F, 2);
    Bytes rate; put(rate, std::bit_cast<std::uint64_t>(48000.0), 8);
    append(audio, ebml(0xB5, rate));
    append(audio_entry, ebml(0xE1, audio));
    Bytes entries = ebml(0xAE, video_entry); append(entries, ebml(0xAE, audio_entry));
    Bytes tracks = ebml(0x1654AE6B, entries);
    // A cluster of unknown size: a video keyframe at 0, an audio block of three Xiph-laced frames
    // (3, 4, 5 bytes) at 0, a video frame at 40 in a BlockGroup with a reference.
    Bytes cluster = ebml_uint(0xE7, 0);
    append(cluster, ebml(0xA3, Bytes{0x81, 0x00, 0x00, 0x80, 'K', 'E', 'Y'}));
    Bytes laced{0x82, 0x00, 0x00, 0x02, 0x02, 3, 4};
    for (int i = 0; i < 12; ++i) laced.push_back(static_cast<std::uint8_t>(i));
    append(cluster, ebml(0xA3, laced));
    Bytes group = ebml(0xA1, Bytes{0x81, 0x00, 0x28, 0x00, 'P'});
    append(group, ebml(0xFB, Bytes{0xd8}));
    append(cluster, ebml(0xA0, group));
    Bytes segment = info; append(segment, tracks); append(segment, ebml(0x1F43B675, cluster, true));
    append(header, ebml(0x18538067, segment, true));
    return header;
}

Bytes ogg_page(std::uint32_t serial, std::uint32_t sequence, std::uint8_t type, std::int64_t granule, const std::vector<Bytes>& packets, bool last_continues = false) {
    Bytes lacing, body;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        auto n = packets[i].size();
        while (n >= 255) { lacing.push_back(255); n -= 255; }
        if (!(last_continues && i + 1 == packets.size())) lacing.push_back(static_cast<std::uint8_t>(n));
        append(body, packets[i]);
    }
    Bytes page{'O', 'g', 'g', 'S', 0, type};
    put_le(page, static_cast<std::uint64_t>(granule), 8); put_le(page, serial, 4); put_le(page, sequence, 4); put_le(page, 0, 4);
    page.push_back(static_cast<std::uint8_t>(lacing.size()));
    append(page, lacing); append(page, body);
    return page;
}

std::uint8_t header_crc8(const Bytes& b) {
    std::uint8_t c = 0;
    for (const auto x : b) { c ^= x; for (int i = 0; i < 8; ++i) c = static_cast<std::uint8_t>((c & 0x80) ? (c << 1) ^ 7 : c << 1); }
    return c;
}

void container_tests() {
    for (const bool fragmented : {false, true}) {
        const auto file = iso_file(fragmented);
        mc::Movie movie;
        std::string why;
        require(mc::parse(reader(file), file.size(), movie, why), "MP4 parses: " + why);
        require(movie.brand == mc::fourcc("isom") && movie.tracks.size() == 2, "two MP4 tracks");
        const auto& v = movie.tracks[0];
        require(v.kind == mc::Track::Kind::video && v.codec == mc::fourcc("av01") && v.width == 320 && v.height == 240 && v.timescale == 90000 &&
                v.config.size() == 4, "the AV1 track");
        require(v.samples.size() == 3 && v.samples[1].size == 20 && v.samples[2].dts == (fragmented ? 12000 : 6000) && v.samples[0].keyframe &&
                v.samples[1].keyframe == false, "video samples");
        require(mc::read_sample(reader(file), v, v.samples[1])[0] == 10, "the second video sample's bytes");
        const auto& a = movie.tracks[1];
        require(a.codec == mc::fourcc("opus") && a.channels == 2 && a.sample_rate == 48000 && a.priming == 312 && a.samples.size() == 2 &&
                a.samples[1].size == 7 && mc::read_sample(reader(file), a, a.samples[0])[0] == 0x80, "the Opus track");
    }
    {
        const auto file = webm_file();
        mc::Movie movie;
        std::string why;
        require(mc::parse(reader(file), file.size(), movie, why), "WebM parses: " + why);
        require(movie.brand == mc::fourcc("webm") && movie.timescale == 1000 && movie.tracks.size() == 2, "two WebM tracks");
        const auto& v = movie.tracks[0];
        require(v.codec == mc::fourcc("vp09") && v.width == 64 && v.height == 48 && v.samples.size() == 2 && v.samples[0].keyframe &&
                !v.samples[1].keyframe && v.samples[1].pts == 40, "the VP9 track");
        require(mc::read_sample(reader(file), v, v.samples[0]) == Bytes{'K', 'E', 'Y'}, "the keyframe bytes");
        const auto& a = movie.tracks[1];
        require(a.codec == mc::fourcc("opus") && a.priming == 312 && a.samples.size() == 3 && a.samples[0].size == 3 && a.samples[2].size == 5 &&
                a.samples[1].pts == 20 && mc::read_sample(reader(file), a, a.samples[2])[0] == 7, "Xiph-laced Opus frames");
    }
    {
        // Ogg Opus: the header packets, then a 300-byte packet that spans two pages and a 1-byte packet.
        Bytes head{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1}; put_le(head, 120, 2); put_le(head, 48000, 4); put_le(head, 0, 2); head.push_back(0);
        Bytes tags{'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 0, 0, 0, 0, 0, 0, 0, 0};
        Bytes big(300, 0xfc);   // TOC 0xfc: CELT 20 ms, one frame
        Bytes first_half(big.begin(), big.begin() + 255), second_half(big.begin() + 255, big.end());
        Bytes file = ogg_page(7, 0, 2, 0, {head});
        append(file, ogg_page(7, 1, 0, 0, {tags}));
        append(file, ogg_page(7, 2, 0, -1, {first_half}, true));
        append(file, ogg_page(7, 3, 1, 1920, {second_half, Bytes{0xfc}}));
        mc::Movie movie;
        std::string why;
        require(mc::parse(reader(file), file.size(), movie, why), "Ogg parses: " + why);
        const auto& t = movie.tracks.at(0);
        require(t.codec == mc::fourcc("opus") && t.priming == 120 && t.samples.size() == 2 && t.samples[0].size == 300 && t.samples[0].pieces == 2 &&
                t.samples[0].duration == 960 && t.samples[1].pts == 960, "Ogg Opus packets");
        require(mc::read_sample(reader(file), t, t.samples[0]) == big, "a packet across pages reads whole");
    }
    {
        // FLAC: STREAMINFO (44.1 kHz, stereo, 16 bits, 8192 samples), two fixed-block-size frames of 4096.
        Bytes file{'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
        Bytes info{0x10, 0x00, 0x10, 0x00, 0, 0, 0, 0, 0, 0};
        info.push_back(0x0a); info.push_back(0xc4); info.push_back(0x42); info.push_back(0xf0);
        put(info, 8192, 4); append(info, Bytes(16, 0));
        append(file, info);
        for (std::uint8_t number = 0; number < 2; ++number) {
            Bytes frame{0xff, 0xf8, 0xc9, 0x18, number};   // block 4096 (code 12), 44.1 kHz, stereo, 16 bits, frame number
            frame.push_back(header_crc8(frame));
            append(frame, Bytes(20, 0x55));
            append(file, frame);
        }
        mc::Movie movie;
        std::string why;
        require(mc::parse(reader(file), file.size(), movie, why), "FLAC parses: " + why);
        const auto& t = movie.tracks.at(0);
        require(t.codec == mc::fourcc("flac") && t.sample_rate == 44100 && t.channels == 2 && t.bits == 16 && t.duration == 8192 &&
                t.samples.size() == 2 && t.samples[1].pts == 4096 && t.samples[0].size == 26 && t.config.size() == 38, "FLAC frames");
    }
    {
        // MP3: 128 kb/s, 44.1 kHz stereo frames of 417 bytes; the first an Info/LAME frame (delay 576, padding 1000).
        const auto frame = [](bool padding) {
            Bytes f{0xff, 0xfb, static_cast<std::uint8_t>(0x90 | (padding ? 2 : 0)), 0x00};
            f.resize(padding ? 418 : 417, 0);
            return f;
        };
        Bytes file{'I', 'D', '3', 3, 0, 0, 0, 0, 0, 5, 1, 2, 3, 4, 5};
        auto info = frame(false);
        std::memcpy(info.data() + 36, "Info", 4);
        info[43] = 0x0f;   // frames, bytes, TOC, quality
        const std::size_t tag = 36 + 8 + 4 + 4 + 100 + 4;
        info[tag + 21] = 0x24; info[tag + 22] = 0x03; info[tag + 23] = 0xe8;   // delay 576, padding 1000
        append(file, info);
        for (int i = 0; i < 4; ++i) append(file, frame(i == 1));
        mc::Movie movie;
        std::string why;
        require(mc::parse(reader(file), file.size(), movie, why), "MP3 parses: " + why);
        const auto& t = movie.tracks.at(0);
        require(t.codec == mc::fourcc(".mp3") && t.sample_rate == 44100 && t.channels == 2 && t.samples.size() == 4 && t.samples[1].size == 418 &&
                t.priming == 576 + 529 && t.duration == 4 * 1152 - 576 - 529 - 1000, "MP3 frames and the LAME delay");
    }
}

int main() {
    try {
        resampler_tests();
        if (std::getenv("ALCHEMY_MEDIA_LIST_HOST_DEVICES")) list_host_devices();
        cmtime_tests();
        splitter_tests();
        midi_tests();
        hal_tests();
        music_tests();
        audio_file_tests();
        audio_file_writing_tests();
        ump_tests();
        container_tests();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "qlab media core tests passed\n";
    return 0;
}
