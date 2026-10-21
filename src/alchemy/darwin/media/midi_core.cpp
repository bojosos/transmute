#include "alchemy/darwin/media/midi_core.hpp"

#include <algorithm>
#include <chrono>
#include <random>

namespace alchemy::gui::completion_kit::media::midi {

std::size_t message_length(std::uint8_t status) {
    if (status < 0x80) return 0;
    if (status < 0xc0) return 3;            // note off/on, poly pressure, control change
    if (status < 0xe0) return 2;            // program change, channel pressure
    if (status < 0xf0) return 3;            // pitch bend
    switch (status) {
    case 0xf0: return 0;                    // SysEx: variable
    case 0xf1: case 0xf3: return 2;         // time code quarter frame, song select
    case 0xf2: return 3;                    // song position pointer
    default: return 1;                      // tune request, EOX, undefined, realtime
    }
}

void MessageSplitter::feed(std::span<const std::uint8_t> bytes, const std::function<void(std::vector<std::uint8_t>)>& emit) {
    for (const auto byte : bytes) {
        if (byte >= 0xf8) { emit({byte}); continue; }   // realtime: anywhere, never breaks a message
        if (byte == 0xf0) {
            current_.assign(1, byte);
            sysex_ = true;
            running_ = 0;
            continue;
        }
        if (byte == 0xf7) {
            if (sysex_) {
                current_.push_back(byte);
                emit(std::move(current_));
                current_.clear();
                sysex_ = false;
            }
            continue;
        }
        if (byte & 0x80) {
            // A status byte ends an unterminated SysEx (dropped) and starts a message.
            sysex_ = false;
            current_.assign(1, byte);
            expected_ = message_length(byte);
            running_ = byte < 0xf0 ? byte : 0;   // system common messages cancel running status
            if (current_.size() == expected_) { emit(std::move(current_)); current_.clear(); }
            continue;
        }
        if (sysex_) { current_.push_back(byte); continue; }
        if (current_.empty()) {
            if (!running_) continue;             // data without a status: dropped
            current_.assign(1, running_);
            expected_ = message_length(running_);
        }
        current_.push_back(byte);
        if (current_.size() == expected_) { emit(std::move(current_)); current_.clear(); }
    }
}

Server::Server(std::unique_ptr<Backend> backend, Clock clock) : backend_(std::move(backend)), clock_(std::move(clock)) {
    random_state_ = std::random_device{}();
    random_state_ = (random_state_ << 32) ^ std::random_device{}() ^ 0x9e3779b97f4a7c15ULL;
}

Server::~Server() {
    {
        std::lock_guard lock(schedule_mutex_);
        stopping_ = true;
    }
    schedule_wake_.notify_all();
    if (scheduler_.joinable()) scheduler_.join();
    std::vector<std::size_t> inputs;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [port, open] : open_inputs_) if (open) inputs.push_back(port);
    }
    for (const auto port : inputs) backend_->close_input(port);
}

void Server::set_sink(Sink sink) { std::lock_guard lock(mutex_); sink_ = std::move(sink); }
void Server::set_notifier(Notifier notifier) { std::lock_guard lock(mutex_); notifier_ = std::move(notifier); }

Server::Object* Server::find_locked(ObjectRef ref) {
    const auto found = objects_.find(ref);
    return found == objects_.end() ? nullptr : &found->second;
}

ObjectRef Server::add_locked(ObjectType type, ObjectRef parent) {
    const auto ref = next_ref_++;
    auto& object = objects_[ref];
    object.ref = ref;
    object.type = type;
    object.parent = parent;
    if (parent) if (auto* owner = find_locked(parent)) owner->children.push_back(ref);
    return ref;
}

bool Server::id_taken_locked(std::int32_t id, ObjectRef except) {
    for (const auto& [ref, object] : objects_) {
        if (ref == except) continue;
        const auto found = object.properties.find(property::unique_id);
        if (found != object.properties.end() && std::holds_alternative<std::int32_t>(found->second) && std::get<std::int32_t>(found->second) == id)
            return true;
    }
    return false;
}

// FNV-1a of the seed, positive and nonzero, probed past collisions: hardware
// IDs stay the same from run to run while the port names do.
std::int32_t Server::stable_id_locked(const std::string& seed) {
    std::uint32_t hash = 2166136261U;
    for (const auto c : seed) { hash ^= static_cast<std::uint8_t>(c); hash *= 16777619U; }
    auto id = static_cast<std::int32_t>(hash & 0x7fffffffU);
    while (id == 0 || id_taken_locked(id, 0)) id = static_cast<std::int32_t>((static_cast<std::uint32_t>(id) + 1) & 0x7fffffffU);
    return id;
}

std::int32_t Server::random_id_locked() {
    for (;;) {
        random_state_ ^= random_state_ << 13;
        random_state_ ^= random_state_ >> 7;
        random_state_ ^= random_state_ << 17;
        const auto id = static_cast<std::int32_t>(static_cast<std::uint32_t>(random_state_ >> 16));
        if (id != 0 && !id_taken_locked(id, 0)) return id;
    }
}

void Server::setup_locked() {
    if (setup_) return;
    setup_ = true;
    const auto inputs = backend_->inputs();
    const auto outputs = backend_->outputs();
    hardware_inputs_.assign(inputs.size(), 0);
    hardware_outputs_.assign(outputs.size(), 0);
    std::vector<bool> input_used(inputs.size());
    std::map<std::string, int> seen;
    const auto make_device = [&](const PortInfo& info, std::optional<std::size_t> input, std::optional<std::size_t> output) {
        const auto ordinal = seen[info.name]++;
        const auto seed = info.name + "#" + std::to_string(ordinal);
        const auto device = add_locked(ObjectType::device, 0);
        devices_.push_back(device);
        auto& d = *find_locked(device);
        d.properties[property::name] = info.name;
        if (!info.manufacturer.empty()) d.properties[property::manufacturer] = info.manufacturer;
        if (!info.driver.empty()) d.properties[property::driver_owner] = info.driver;
        d.properties[property::offline] = std::int32_t{0};
        d.properties[property::unique_id] = stable_id_locked("device:" + seed);
        const auto entity = add_locked(ObjectType::entity, device);
        auto& e = *find_locked(entity);
        e.properties[property::name] = info.name;
        e.properties[property::unique_id] = stable_id_locked("entity:" + seed);
        if (input) {
            const auto endpoint = add_locked(ObjectType::source, entity);
            auto& o = *find_locked(endpoint);
            o.backend_port = *input;
            o.properties[property::name] = inputs[*input].name;
            o.properties[property::unique_id] = stable_id_locked("source:" + seed);
            hardware_inputs_[*input] = endpoint;
        }
        if (output) {
            const auto endpoint = add_locked(ObjectType::destination, entity);
            auto& o = *find_locked(endpoint);
            o.backend_port = *output;
            o.properties[property::name] = outputs[*output].name;
            o.properties[property::unique_id] = stable_id_locked("destination:" + seed);
            hardware_outputs_[*output] = endpoint;
        }
    };
    for (std::size_t o = 0; o < outputs.size(); ++o) {
        std::optional<std::size_t> paired;
        for (std::size_t i = 0; i < inputs.size() && !paired; ++i)
            if (!input_used[i] && inputs[i].name == outputs[o].name) { paired = i; input_used[i] = true; }
        make_device(outputs[o], paired, o);
    }
    for (std::size_t i = 0; i < inputs.size(); ++i)
        if (!input_used[i]) make_device(inputs[i], i, std::nullopt);
}

void Server::notify_locked(Notification notification, std::vector<std::pair<ObjectRef, Notification>>& out) {
    for (const auto client : clients_) out.emplace_back(client, notification);
}

void Server::publish(std::vector<std::pair<ObjectRef, Notification>> notifications) {
    Notifier notifier;
    { std::lock_guard lock(mutex_); notifier = notifier_; }
    if (!notifier) return;
    for (auto& [client, notification] : notifications) notifier(client, std::move(notification));
}

ObjectRef Server::create_client(const std::string& name) {
    std::lock_guard lock(mutex_);
    setup_locked();
    const auto ref = add_locked(ObjectType::client, 0);
    find_locked(ref)->properties[property::name] = name;
    clients_.push_back(ref);
    return ref;
}

std::int32_t Server::dispose_client(ObjectRef client) {
    std::vector<ObjectRef> owned;
    {
        std::lock_guard lock(mutex_);
        const auto* object = find_locked(client);
        if (!object || object->type != ObjectType::client) return error::invalid_client;
        for (const auto& [ref, item] : objects_) if (item.client == client) owned.push_back(ref);
    }
    for (const auto ref : owned) {
        ObjectType type{};
        { std::lock_guard lock(mutex_); const auto* item = find_locked(ref); if (!item) continue; type = item->type; }
        if (type == ObjectType::input_port || type == ObjectType::output_port) static_cast<void>(dispose_port(ref));
        else static_cast<void>(dispose_endpoint(ref));
    }
    std::lock_guard lock(mutex_);
    objects_.erase(client);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
    return 0;
}

std::int32_t Server::create_port(ObjectRef client, const std::string& name, bool input, ObjectRef& out) {
    std::lock_guard lock(mutex_);
    const auto* owner = find_locked(client);
    if (!owner || owner->type != ObjectType::client) return error::invalid_client;
    out = add_locked(input ? ObjectType::input_port : ObjectType::output_port, 0);
    auto& port = *find_locked(out);
    port.client = client;
    port.properties[property::name] = name;
    return 0;
}

std::int32_t Server::dispose_port(ObjectRef port) {
    std::vector<ObjectRef> sources;
    {
        std::lock_guard lock(mutex_);
        const auto* object = find_locked(port);
        if (!object || (object->type != ObjectType::input_port && object->type != ObjectType::output_port)) return error::invalid_port;
        for (const auto& [source, refcon] : object->connections) sources.push_back(source);
    }
    for (const auto source : sources) static_cast<void>(disconnect(port, source));
    std::lock_guard lock(mutex_);
    objects_.erase(port);
    return 0;
}

std::int32_t Server::connect(ObjectRef port, ObjectRef source, std::uint64_t refcon) {
    std::optional<std::size_t> open;
    {
        std::lock_guard lock(mutex_);
        auto* object = find_locked(port);
        if (!object || object->type != ObjectType::input_port) return error::invalid_port;
        const auto* endpoint = find_locked(source);
        if (!endpoint) return error::unknown_endpoint;
        if (endpoint->type != ObjectType::source) return error::wrong_endpoint_type;
        if (endpoint->backend_port && !open_inputs_[*endpoint->backend_port]) open = endpoint->backend_port;
        object->connections[source] = refcon;
    }
    if (open) {
        const auto index = *open;
        const bool opened = backend_->open_input(index, [this, index](std::vector<std::uint8_t> message, std::uint64_t time) {
            input_from_backend(index, std::move(message), time);
        });
        std::lock_guard lock(mutex_);
        if (!opened) {
            if (auto* object = find_locked(port)) object->connections.erase(source);
            return -10845;   // kMIDIUnknownError: the Windows MIDI input could not be opened
        }
        open_inputs_[index] = true;
    }
    return 0;
}

std::int32_t Server::disconnect(ObjectRef port, ObjectRef source) {
    std::optional<std::size_t> close;
    {
        std::lock_guard lock(mutex_);
        auto* object = find_locked(port);
        if (!object || object->type != ObjectType::input_port) return error::invalid_port;
        if (!object->connections.erase(source)) return error::no_connection;
        const auto* endpoint = find_locked(source);
        if (endpoint && endpoint->backend_port) {
            bool used = false;
            for (const auto& [ref, item] : objects_) used = used || item.connections.contains(source);
            if (!used && open_inputs_[*endpoint->backend_port]) { open_inputs_[*endpoint->backend_port] = false; close = endpoint->backend_port; }
        }
    }
    if (close) backend_->close_input(*close);
    return 0;
}

std::int32_t Server::create_virtual(ObjectRef client, const std::string& name, bool source, ObjectRef& out) {
    std::vector<std::pair<ObjectRef, Notification>> notifications;
    {
        std::lock_guard lock(mutex_);
        const auto* owner = find_locked(client);
        if (!owner || owner->type != ObjectType::client) return error::invalid_client;
        out = add_locked(source ? ObjectType::source : ObjectType::destination, 0);
        auto& endpoint = *find_locked(out);
        endpoint.client = client;
        endpoint.is_virtual = true;
        endpoint.properties[property::name] = name;
        endpoint.properties[property::unique_id] = random_id_locked();
        Notification added{Notification::Kind::object_added, 0, out, ObjectType::other, endpoint.type, {}};
        notify_locked(added, notifications);
        notify_locked({Notification::Kind::setup_changed}, notifications);
    }
    publish(std::move(notifications));
    return 0;
}

std::int32_t Server::dispose_endpoint(ObjectRef endpoint) {
    std::vector<std::pair<ObjectRef, Notification>> notifications;
    {
        std::lock_guard lock(mutex_);
        const auto* object = find_locked(endpoint);
        if (!object || (object->type != ObjectType::source && object->type != ObjectType::destination)) return error::unknown_endpoint;
        if (!object->is_virtual) return error::not_permitted;
        const auto type = object->type;
        for (auto& [ref, item] : objects_) item.connections.erase(endpoint);
        objects_.erase(endpoint);
        notify_locked({Notification::Kind::object_removed, 0, endpoint, ObjectType::other, type, {}}, notifications);
        notify_locked({Notification::Kind::setup_changed}, notifications);
    }
    { std::lock_guard lock(schedule_mutex_); std::erase_if(held_, [&](const Held& held) { return held.destination == endpoint; }); }
    publish(std::move(notifications));
    return 0;
}

std::size_t Server::device_count() { std::lock_guard lock(mutex_); setup_locked(); return devices_.size(); }
ObjectRef Server::device(std::size_t index) {
    std::lock_guard lock(mutex_);
    setup_locked();
    return index < devices_.size() ? devices_[index] : 0;
}

namespace {
template <class Map, class Predicate>
std::vector<ObjectRef> select(const Map& objects, Predicate predicate) {
    std::vector<ObjectRef> refs;
    for (const auto& [ref, object] : objects) if (predicate(object)) refs.push_back(ref);
    return refs;   // map order = creation order (refs increase)
}
} // namespace

std::size_t Server::source_count() {
    std::lock_guard lock(mutex_);
    setup_locked();
    return select(objects_, [](const Object& o) { return o.type == ObjectType::source; }).size();
}
ObjectRef Server::source(std::size_t index) {
    std::lock_guard lock(mutex_);
    setup_locked();
    const auto all = select(objects_, [](const Object& o) { return o.type == ObjectType::source; });
    return index < all.size() ? all[index] : 0;
}
std::size_t Server::destination_count() {
    std::lock_guard lock(mutex_);
    setup_locked();
    return select(objects_, [](const Object& o) { return o.type == ObjectType::destination; }).size();
}
ObjectRef Server::destination(std::size_t index) {
    std::lock_guard lock(mutex_);
    setup_locked();
    const auto all = select(objects_, [](const Object& o) { return o.type == ObjectType::destination; });
    return index < all.size() ? all[index] : 0;
}

std::size_t Server::child_count(ObjectRef parent, ObjectType type) {
    std::lock_guard lock(mutex_);
    const auto* object = find_locked(parent);
    if (!object) return 0;
    std::size_t count = 0;
    for (const auto child : object->children) if (const auto* c = find_locked(child); c && c->type == type) ++count;
    return count;
}

ObjectRef Server::child(ObjectRef parent, ObjectType type, std::size_t index) {
    std::lock_guard lock(mutex_);
    const auto* object = find_locked(parent);
    if (!object) return 0;
    for (const auto ref : object->children)
        if (const auto* c = find_locked(ref); c && c->type == type && index-- == 0) return ref;
    return 0;
}

std::int32_t Server::parent(ObjectRef object, ObjectType type, ObjectRef& out) {
    std::lock_guard lock(mutex_);
    out = 0;
    const auto* item = find_locked(object);
    if (!item) return error::object_not_found;
    const auto* owner = item->parent ? find_locked(item->parent) : nullptr;
    if (!owner || owner->type != type) return error::object_not_found;   // virtual endpoints have no entity
    out = owner->ref;
    return 0;
}

std::optional<ObjectType> Server::type_of(ObjectRef object) {
    std::lock_guard lock(mutex_);
    const auto* item = find_locked(object);
    return item ? std::optional<ObjectType>(item->type) : std::nullopt;
}

std::int32_t Server::find_unique_id(std::int32_t id, ObjectRef& out, ObjectType& type) {
    std::lock_guard lock(mutex_);
    setup_locked();
    for (const auto& [ref, object] : objects_) {
        if (object.type == ObjectType::client || object.type == ObjectType::input_port || object.type == ObjectType::output_port) continue;
        const auto found = object.properties.find(property::unique_id);
        if (found != object.properties.end() && std::holds_alternative<std::int32_t>(found->second) && std::get<std::int32_t>(found->second) == id) {
            out = ref;
            type = object.type;
            return 0;
        }
    }
    out = 0;
    type = ObjectType::other;
    return error::object_not_found;
}

bool Server::owns_virtual(ObjectRef client, ObjectRef endpoint) {
    std::lock_guard lock(mutex_);
    const auto* object = find_locked(endpoint);
    return object && object->is_virtual && object->client == client;
}

namespace {
bool has_properties(ObjectType type) {
    return type == ObjectType::device || type == ObjectType::entity || type == ObjectType::source || type == ObjectType::destination;
}
} // namespace

std::int32_t Server::get(ObjectRef object, const std::string& key, Value& out) {
    std::lock_guard lock(mutex_);
    const auto* item = find_locked(object);
    if (!item || !has_properties(item->type)) return error::object_not_found;
    for (const auto* current = item; current; current = current->parent ? find_locked(current->parent) : nullptr) {
        if (current != item && key == property::unique_id) break;   // unique IDs are never inherited
        const auto found = current->properties.find(key);
        if (found != current->properties.end()) { out = found->second; return 0; }
    }
    return error::unknown_property;
}

std::int32_t Server::set(ObjectRef object, const std::string& key, Value value) {
    std::vector<std::pair<ObjectRef, Notification>> notifications;
    {
        std::lock_guard lock(mutex_);
        auto* item = find_locked(object);
        if (!item || !has_properties(item->type)) return error::object_not_found;
        if (key == property::unique_id) {
            if (!std::holds_alternative<std::int32_t>(value)) return error::wrong_property_type;
            if (id_taken_locked(std::get<std::int32_t>(value), object)) return error::id_not_unique;
        }
        item->properties[key] = std::move(value);
        notify_locked({Notification::Kind::property_changed, 0, object, ObjectType::other, item->type, key}, notifications);
    }
    publish(std::move(notifications));
    return 0;
}

std::int32_t Server::remove(ObjectRef object, const std::string& key) {
    std::vector<std::pair<ObjectRef, Notification>> notifications;
    {
        std::lock_guard lock(mutex_);
        auto* item = find_locked(object);
        if (!item || !has_properties(item->type)) return error::object_not_found;
        if (!item->properties.erase(key)) return error::unknown_property;
        notify_locked({Notification::Kind::property_changed, 0, object, ObjectType::other, item->type, key}, notifications);
    }
    publish(std::move(notifications));
    return 0;
}

std::int32_t Server::properties(ObjectRef object, std::map<std::string, Value>& out) {
    std::lock_guard lock(mutex_);
    const auto* item = find_locked(object);
    if (!item || !has_properties(item->type)) return error::object_not_found;
    out.clear();
    for (const auto* current = item; current; current = current->parent ? find_locked(current->parent) : nullptr)
        for (const auto& [key, value] : current->properties)
            if (current == item || key != property::unique_id) out.emplace(key, value);   // nearest definition wins
    return 0;
}

void Server::deliver_now(ObjectRef destination, std::vector<Packet> packets) {
    Sink sink;
    std::optional<std::size_t> port;
    std::vector<std::vector<std::uint8_t>> messages;
    {
        std::lock_guard lock(mutex_);
        const auto* object = find_locked(destination);
        if (!object) return;   // disposed while held
        if (object->is_virtual) sink = sink_;
        else if (object->backend_port) {
            port = object->backend_port;
            auto& splitter = splitters_[destination];
            for (const auto& packet : packets) splitter.feed(packet.data, [&](std::vector<std::uint8_t> message) { messages.push_back(std::move(message)); });
        }
    }
    if (sink) { sink({Delivery::Kind::destination, destination, 0, 0, std::move(packets)}); return; }
    if (port) {
        std::lock_guard lock(output_mutex_);
        for (const auto& message : messages) static_cast<void>(backend_->send(*port, message));
    }
}

std::int32_t Server::send(ObjectRef port, ObjectRef destination, std::vector<Packet> packets) {
    {
        std::lock_guard lock(mutex_);
        const auto* object = find_locked(port);
        if (!object || object->type != ObjectType::output_port) return error::invalid_port;
        const auto* endpoint = find_locked(destination);
        if (!endpoint) return error::unknown_endpoint;
        if (endpoint->type != ObjectType::destination) return error::wrong_endpoint_type;
    }
    const auto now = clock_();
    std::vector<Packet> immediate, later;
    for (auto& packet : packets) (packet.time == 0 || packet.time <= now ? immediate : later).push_back(std::move(packet));
    if (!immediate.empty()) deliver_now(destination, std::move(immediate));
    if (!later.empty()) schedule(destination, std::move(later));
    return 0;
}

std::int32_t Server::send_sysex(ObjectRef destination, std::vector<std::uint8_t> bytes) {
    {
        std::lock_guard lock(mutex_);
        const auto* endpoint = find_locked(destination);
        if (!endpoint) return error::unknown_endpoint;
        if (endpoint->type != ObjectType::destination) return error::wrong_endpoint_type;
    }
    std::vector<Packet> packets;
    for (std::size_t at = 0; at < bytes.size(); at += 65535) {
        const auto end = std::min(bytes.size(), at + 65535);
        packets.push_back({0, std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(end))});
    }
    deliver_now(destination, std::move(packets));
    return 0;
}

std::int32_t Server::received(ObjectRef source, std::vector<Packet> packets) {
    Sink sink;
    std::vector<Delivery> deliveries;
    {
        std::lock_guard lock(mutex_);
        const auto* endpoint = find_locked(source);
        if (!endpoint) return error::unknown_endpoint;
        if (endpoint->type != ObjectType::source || !endpoint->is_virtual) return error::wrong_endpoint_type;
        sink = sink_;
        for (const auto& [ref, object] : objects_) {
            const auto found = object.connections.find(source);
            if (object.type == ObjectType::input_port && found != object.connections.end())
                deliveries.push_back({Delivery::Kind::input_port, ref, found->second, source, packets});
        }
    }
    if (sink) for (auto& delivery : deliveries) sink(std::move(delivery));
    return 0;
}

void Server::input_from_backend(std::size_t port, std::vector<std::uint8_t> message, std::uint64_t time) {
    Sink sink;
    std::vector<Delivery> deliveries;
    {
        std::lock_guard lock(mutex_);
        if (port >= hardware_inputs_.size()) return;
        const auto source = hardware_inputs_[port];
        sink = sink_;
        for (const auto& [ref, object] : objects_) {
            const auto found = object.connections.find(source);
            if (object.type == ObjectType::input_port && found != object.connections.end())
                deliveries.push_back({Delivery::Kind::input_port, ref, found->second, source, {Packet{time, message}}});
        }
    }
    if (sink) for (auto& delivery : deliveries) sink(std::move(delivery));
}

std::int32_t Server::flush(ObjectRef destination) {
    if (destination) {
        std::lock_guard lock(mutex_);
        const auto* endpoint = find_locked(destination);
        if (!endpoint) return error::unknown_endpoint;
        if (endpoint->type != ObjectType::destination) return error::wrong_endpoint_type;
    }
    std::lock_guard lock(schedule_mutex_);
    std::erase_if(held_, [&](const Held& held) { return !destination || held.destination == destination; });
    schedule_wake_.notify_all();
    return 0;
}

std::size_t Server::pending() {
    std::lock_guard lock(schedule_mutex_);
    return held_.size();
}

void Server::schedule(ObjectRef destination, std::vector<Packet> packets) {
    std::lock_guard lock(schedule_mutex_);
    for (auto& packet : packets) held_.push_back({packet.time, held_order_++, destination, std::move(packet)});
    if (!scheduler_.joinable()) scheduler_ = std::thread([this] { scheduler_loop(); });
    schedule_wake_.notify_all();
}

void Server::scheduler_loop() {
    std::unique_lock lock(schedule_mutex_);
    while (!stopping_) {
        if (held_.empty()) { schedule_wake_.wait(lock); continue; }
        const auto earliest = std::min_element(held_.begin(), held_.end(), [](const Held& a, const Held& b) {
            return a.time != b.time ? a.time < b.time : a.order < b.order;
        });
        const auto now = clock_();
        if (earliest->time > now) {
            schedule_wake_.wait_for(lock, std::chrono::nanoseconds(std::min<std::uint64_t>(earliest->time - now, 50'000'000)));
            continue;
        }
        auto held = std::move(*earliest);
        held_.erase(earliest);
        lock.unlock();
        std::vector<Packet> packets;
        packets.push_back(std::move(held.packet));
        deliver_now(held.destination, std::move(packets));
        lock.lock();
    }
}

} // namespace alchemy::gui::completion_kit::media::midi
