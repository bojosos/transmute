#pragma once

// CoreMIDI's object model as a host-only core (QLab gap group G5): the MIDI
// "server" of one Alchemy process. CoreMIDI.cpp binds it to the guest API.
//
// - Devices, entities and endpoints come from a Backend (Windows MIDI through
//   host::midi in alchemy-fx, a fake in tests). An input and an output port with the
//   same name form one device with one entity (one source, one destination),
//   as a USB MIDI interface appears on a Mac; other ports get a device each.
//   The list is read once, when a client first asks (CoreMIDI's setup);
//   devices that appear later are not added (no hot-plug).
// - Clients, input/output ports, connections and virtual sources/destinations
//   are real. Packets sent to a virtual destination, received from a virtual
//   source or read from a hardware input are delivered through the Sink
//   (the guest layer calls the read procs on CoreMIDI's thread).
// - Packets whose timestamp lies in the future are held by a scheduler thread
//   until their host time; MIDIFlushOutput drops held packets.
// - Properties are typed (integer, string, data); entities and endpoints
//   inherit their device's properties (not the unique ID), as CoreMIDI
//   documents. Unique IDs are stable across runs for hardware (derived from
//   the port names) and random for virtual endpoints; setting a duplicate
//   unique ID fails with kMIDIIDNotUnique.
// - Object changes made by this process produce the CoreMIDI notifications
//   (object added/removed, property changed, then setup changed).

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace alchemy::gui::completion_kit::media::midi {

using ObjectRef = std::uint32_t;

// MIDIObjectType
enum class ObjectType : std::int32_t { other = -1, device = 0, entity = 1, source = 2, destination = 3, client = 100, input_port = 101, output_port = 102 };

// OSStatus values of <CoreMIDI/MIDIServices.h>.
namespace error {
inline constexpr std::int32_t invalid_client = -10830, invalid_port = -10831, wrong_endpoint_type = -10832, no_connection = -10833,
    unknown_endpoint = -10834, unknown_property = -10835, wrong_property_type = -10836, message_send = -10838,
    object_not_found = -10842, id_not_unique = -10843, not_permitted = -10844, param = -50;
}

// Property names (the CFString values of the kMIDIProperty* constants).
namespace property {
inline constexpr const char *name = "name", *manufacturer = "manufacturer", *model = "model", *unique_id = "uniqueID",
    *device_id = "deviceID", *receive_channels = "receiveChannels", *transmit_channels = "transmitChannels",
    *max_sysex_speed = "maxSysExSpeed", *advance_schedule_time = "scheduleAheadMuSec", *embedded = "embedded",
    *broadcast = "broadcast", *single_realtime_entity = "singleRealtimeEntity", *connection_unique_id = "connUniqueID",
    *offline = "offline", *private_ = "private", *driver_owner = "driver", *image = "image", *driver_version = "driverVersion",
    *display_name = "displayName", *supports_general_midi = "supportsGeneralMIDI";
}

struct Packet {
    std::uint64_t time{};              // host time (nanoseconds); 0 = now
    std::vector<std::uint8_t> data;
};

using Value = std::variant<std::int32_t, std::string, std::vector<std::uint8_t>>;

// One backend port (a host MIDI input or output).
struct PortInfo {
    std::string name, manufacturer, driver;
    bool synthesizer{};                // a software or hardware synth (not a MIDI port)
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::vector<PortInfo> inputs() = 0;
    virtual std::vector<PortInfo> outputs() = 0;
    // Starts input from port `index`; `deliver` gets complete MIDI messages
    // with their host time, on any thread. False when the port cannot open.
    using Input = std::function<void(std::vector<std::uint8_t> message, std::uint64_t time)>;
    virtual bool open_input(std::size_t index, Input deliver) = 0;
    virtual void close_input(std::size_t index) = 0;
    // Sends one complete message (a channel/system message or F0..F7 SysEx).
    virtual bool send(std::size_t index, std::span<const std::uint8_t> message) = 0;
};

// Splits a MIDI byte stream into complete messages: running status, realtime
// bytes inside other messages and SysEx spanning several feeds.
class MessageSplitter {
public:
    void feed(std::span<const std::uint8_t> bytes, const std::function<void(std::vector<std::uint8_t>)>& emit);
private:
    std::vector<std::uint8_t> current_;
    std::uint8_t running_{};
    std::size_t expected_{};
    bool sysex_{};
};
std::size_t message_length(std::uint8_t status);   // bytes of a channel/system message with this status (0 = variable/SysEx)

// What the guest layer must deliver (on CoreMIDI's thread).
struct Delivery {
    enum class Kind { input_port, destination } kind{};
    ObjectRef target{};                // the input port or the virtual destination
    std::uint64_t connection_refcon{}; // input ports: the connection's refCon
    ObjectRef source{};                // input ports: the source endpoint
    std::vector<Packet> packets;
};

struct Notification {
    enum class Kind : std::int32_t { setup_changed = 1, object_added = 2, object_removed = 3, property_changed = 4 } kind{};
    ObjectRef parent{}, child{};       // added/removed; property changed: child = object
    ObjectType parent_type{ObjectType::other}, child_type{ObjectType::other};
    std::string property;              // property changed
};

class Server {
public:
    using Clock = std::function<std::uint64_t()>;
    using Sink = std::function<void(Delivery)>;
    using Notifier = std::function<void(ObjectRef client, Notification)>;

    Server(std::unique_ptr<Backend> backend, Clock clock);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void set_sink(Sink sink);
    void set_notifier(Notifier notifier);

    // Clients and ports.
    ObjectRef create_client(const std::string& name);
    std::int32_t dispose_client(ObjectRef client);
    std::int32_t create_port(ObjectRef client, const std::string& name, bool input, ObjectRef& out);
    std::int32_t dispose_port(ObjectRef port);
    std::int32_t connect(ObjectRef port, ObjectRef source, std::uint64_t refcon);
    std::int32_t disconnect(ObjectRef port, ObjectRef source);

    // Virtual endpoints.
    std::int32_t create_virtual(ObjectRef client, const std::string& name, bool source, ObjectRef& out);
    std::int32_t dispose_endpoint(ObjectRef endpoint);

    // Enumeration (online objects only for sources/destinations).
    std::size_t device_count();
    ObjectRef device(std::size_t index);
    std::size_t source_count();
    ObjectRef source(std::size_t index);
    std::size_t destination_count();
    ObjectRef destination(std::size_t index);
    std::size_t child_count(ObjectRef parent, ObjectType type);   // entities of a device, sources/destinations of an entity
    ObjectRef child(ObjectRef parent, ObjectType type, std::size_t index);
    std::int32_t parent(ObjectRef object, ObjectType type, ObjectRef& out);   // entity of an endpoint, device of an entity
    std::optional<ObjectType> type_of(ObjectRef object);
    std::int32_t find_unique_id(std::int32_t id, ObjectRef& object, ObjectType& type);
    bool owns_virtual(ObjectRef client, ObjectRef endpoint);

    // Properties.
    std::int32_t get(ObjectRef object, const std::string& key, Value& out);   // with inheritance
    std::int32_t set(ObjectRef object, const std::string& key, Value value);
    std::int32_t remove(ObjectRef object, const std::string& key);
    std::int32_t properties(ObjectRef object, std::map<std::string, Value>& out);   // own and inherited

    // I/O.
    std::int32_t send(ObjectRef port, ObjectRef destination, std::vector<Packet> packets);
    std::int32_t send_sysex(ObjectRef destination, std::vector<std::uint8_t> bytes);
    std::int32_t received(ObjectRef source, std::vector<Packet> packets);
    std::int32_t flush(ObjectRef destination);   // 0 = every destination
    std::size_t pending();                       // packets held by the scheduler

private:
    struct Object {
        ObjectRef ref{};
        ObjectType type{};
        ObjectRef parent{};
        std::vector<ObjectRef> children;
        std::map<std::string, Value> properties;
        ObjectRef client{};                       // owner client (ports, virtual endpoints)
        std::optional<std::size_t> backend_port;  // hardware endpoints
        bool is_virtual{};
        std::map<ObjectRef, std::uint64_t> connections;   // input ports: source -> refcon
    };
    struct Held { std::uint64_t time{}; std::uint64_t order{}; ObjectRef destination{}; Packet packet; };

    void setup_locked();
    ObjectRef add_locked(ObjectType type, ObjectRef parent);
    Object* find_locked(ObjectRef ref);
    std::int32_t stable_id_locked(const std::string& seed);
    std::int32_t random_id_locked();
    bool id_taken_locked(std::int32_t id, ObjectRef except);
    void notify_locked(Notification notification, std::vector<std::pair<ObjectRef, Notification>>& out);
    void publish(std::vector<std::pair<ObjectRef, Notification>> notifications);
    void deliver_now(ObjectRef destination, std::vector<Packet> packets);
    void input_from_backend(std::size_t port, std::vector<std::uint8_t> message, std::uint64_t time);
    void schedule(ObjectRef destination, std::vector<Packet> packets);
    void scheduler_loop();

    std::unique_ptr<Backend> backend_;
    Clock clock_;
    std::mutex mutex_;
    std::mutex output_mutex_;                         // serializes backend sends
    Sink sink_;
    Notifier notifier_;
    bool setup_{};
    ObjectRef next_ref_{0x1000};
    std::map<ObjectRef, Object> objects_;
    std::vector<ObjectRef> devices_, clients_;
    std::vector<ObjectRef> hardware_inputs_;          // backend input index -> source endpoint
    std::vector<ObjectRef> hardware_outputs_;         // backend output index -> destination endpoint
    std::map<std::size_t, bool> open_inputs_;
    std::map<ObjectRef, MessageSplitter> splitters_;  // per hardware destination
    std::uint64_t random_state_{};

    std::mutex schedule_mutex_;
    std::condition_variable schedule_wake_;
    std::vector<Held> held_;
    std::uint64_t held_order_{};
    bool stopping_{};
    std::thread scheduler_;
};

} // namespace alchemy::gui::completion_kit::media::midi
