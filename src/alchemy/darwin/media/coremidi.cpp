#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/midi_core.hpp"
#include "alchemy/darwin/media/midi_ump.hpp"
#include "alchemy/darwin/media/midi_host.hpp"
#include "alchemy/darwin/media/coremidi.hpp"

#include "alchemy/gui/runloop.hpp"
#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/time.hpp"

#include <array>
#include <cstdlib>
#include <map>
#include <mutex>

// CoreMIDI over the MIDI core (midi_core.hpp): clients, ports, connections,
// virtual sources and destinations, devices/entities/endpoints from Windows
// MIDI, typed properties, scheduled sends, SysEx requests and the packet-list
// helpers. Read procs and blocks run on one CoreMIDI guest thread (a completion
// thread, "com.apple.coremidi.client"); notify procs run on the main queue when
// the client was created on the main thread (the run loop CoreMIDI uses), on the
// CoreMIDI thread otherwise.
// MIDI 2.0 event lists (MIDIEventList of Universal MIDI Packets) are translated
// to and from MIDI 1.0 at the API (midi_ump.hpp): the MIDI core and the host's
// ports carry MIDI 1.0 bytes. Receive blocks get event lists in the protocol
// their port or destination asked for.
namespace alchemy::gui::completion_kit::media {

namespace {

constexpr std::string_view core_midi = "/System/Library/Frameworks/CoreMIDI.framework/Versions/A/CoreMIDI";

std::function<std::unique_ptr<midi::Backend>()>& midi_factory() {
    static std::function<std::unique_ptr<midi::Backend>()> factory;
    return factory;
}

class NoDevices final : public midi::Backend {
public:
    std::vector<midi::PortInfo> inputs() override { return {}; }
    std::vector<midi::PortInfo> outputs() override { return {}; }
    bool open_input(std::size_t, Input) override { return false; }
    void close_input(std::size_t) override {}
    bool send(std::size_t, std::span<const std::uint8_t>) override { return false; }
};

std::unique_ptr<midi::Backend> make_backend() {
    if (midi_factory()) return midi_factory()();
    if (const char* value = std::getenv("ALCHEMY_MIDI_DEVICES"); value && std::string_view(value) == "none") return std::make_unique<NoDevices>();
    return midi::make_host_backend();
}

// How a port or destination receives: a read proc (MIDIPacketList, refCon), a read block
// (MIDIPacketList) or a receive block (MIDIEventList in `protocol`). Blocks are _Block_copy'd.
struct Proc { Address function{}, refcon{}, block{}; bool event_list{}; std::uint32_t protocol{}; };
struct Client { Address notify{}, refcon{}; bool main_thread{}; };

struct Midi {
    Support& support;
    std::shared_ptr<security::Bridge> bridge;
    std::uint64_t main_thread{};
    std::mutex mutex;
    std::unique_ptr<midi::Server> server;
    std::shared_ptr<CompletionThread> thread;
    std::map<midi::ObjectRef, Proc> read_procs;   // input ports and virtual destinations
    std::map<midi::ObjectRef, Client> clients;

    explicit Midi(Support& owner) : support(owner), bridge(security::bridge()) {}

    midi::Server& core() {
        std::lock_guard lock(mutex);
        if (!server) {
            server = std::make_unique<midi::Server>(make_backend(), host_time_now);
            server->set_sink([this](midi::Delivery delivery) { deliver(std::move(delivery)); });
            server->set_notifier([this](midi::ObjectRef client, midi::Notification notification) { notify(client, std::move(notification)); });
        }
        return *server;
    }
    // On a guest thread inside a call: the CoreMIDI thread exists from the first client on.
    void ensure_thread() {
        std::lock_guard lock(mutex);
        if (!thread) thread = start_completion_thread(support.process, "com.apple.coremidi.client");
    }

    // A guest MIDIPacketList of `packets` (packets longer than 65535 bytes are split).
    Address packet_list(const std::vector<midi::Packet>& packets, std::uint64_t& size) {
        std::vector<std::pair<std::uint64_t, std::span<const std::uint8_t>>> parts;
        for (const auto& packet : packets)
            for (std::size_t at = 0; at < packet.data.size() || (at == 0 && packet.data.empty()); at += 65535) {
                const auto length = std::min<std::size_t>(packet.data.size() - at, 65535);
                parts.emplace_back(packet.time, std::span<const std::uint8_t>(packet.data).subspan(at, length));
                if (packet.data.empty()) break;
            }
        size = 4;
        for (const auto& [time, bytes] : parts) size += (10 + bytes.size() + 3) & ~std::size_t{3};
        const auto list = support.process.allocate_guest(size, 8);
        auto& m = support.memory();
        m.write32(list, static_cast<std::uint32_t>(parts.size()));
        auto at = list + 4;
        for (const auto& [time, bytes] : parts) {
            m.write64(at, time);
            m.write8(at + 8, static_cast<std::uint8_t>(bytes.size()));
            m.write8(at + 9, static_cast<std::uint8_t>(bytes.size() >> 8));
            if (!bytes.empty()) m.write_bytes(at + 10, std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
            at += (10 + bytes.size() + 3) & ~std::uint64_t{3};
        }
        return list;
    }

    void deliver(midi::Delivery delivery) {
        std::shared_ptr<CompletionThread> worker;
        { std::lock_guard lock(mutex); worker = thread; }
        if (!worker) return;   // no client exists, so nothing can read
        worker->post([this, delivery = std::move(delivery)] {
            Proc proc;
            {
                std::lock_guard lock(mutex);
                const auto found = read_procs.find(delivery.target);
                if (found == read_procs.end()) return;   // disposed meanwhile
                proc = found->second;
            }
            const auto connection = delivery.kind == midi::Delivery::Kind::input_port ? delivery.connection_refcon : 0;
            if (proc.event_list && proc.block) {
                const auto list = event_list(delivery.packets, proc.protocol);
                static_cast<void>(bridge->invoke_block(proc.block, {list, connection}));
                support.process.release_guest(list);
                return;
            }
            if (!proc.function && !proc.block) return;
            std::uint64_t size{};
            const auto list = packet_list(delivery.packets, size);
            if (proc.block) {
                static_cast<void>(bridge->invoke_block(proc.block, {list, connection}));
            } else {
                const std::array<Address, 3> arguments{list, proc.refcon, connection};
                static_cast<void>(bridge->call_address(proc.function, arguments));
            }
            support.process.release_guest(list);
        });
    }

    // MIDINotification records: {SInt32 messageID; UInt32 messageSize; ...}.
    void notify(midi::ObjectRef client_ref, midi::Notification notification) {
        Client client;
        std::shared_ptr<CompletionThread> worker;
        {
            std::lock_guard lock(mutex);
            const auto found = clients.find(client_ref);
            if (found == clients.end() || !found->second.notify) return;
            client = found->second;
            worker = thread;
        }
        auto work = [this, client, client_ref, notification = std::move(notification)] {
            {
                std::lock_guard lock(mutex);
                if (!clients.contains(client_ref)) return;   // disposed meanwhile
            }
            auto& m = support.memory();
            const auto record = support.process.allocate_guest(24, 8);
            m.fill_bytes(record, 0, 24);
            m.write32(record, static_cast<std::uint32_t>(notification.kind));
            Address name{};
            switch (notification.kind) {
            case midi::Notification::Kind::setup_changed: m.write32(record + 4, 8); break;
            case midi::Notification::Kind::object_added:
            case midi::Notification::Kind::object_removed:
                m.write32(record + 4, 24);
                m.write32(record + 8, notification.parent);
                m.write32(record + 12, static_cast<std::uint32_t>(notification.parent_type));
                m.write32(record + 16, notification.child);
                m.write32(record + 20, static_cast<std::uint32_t>(notification.child_type));
                break;
            case midi::Notification::Kind::property_changed:
                m.write32(record + 4, 24);
                m.write32(record + 8, notification.child);
                m.write32(record + 12, static_cast<std::uint32_t>(notification.child_type));
                name = support.cf.string(notification.property);
                m.write64(record + 16, name);
                break;
            }
            const std::array<Address, 2> arguments{record, client.refcon};
            static_cast<void>(bridge->call_address(client.notify, arguments));
            if (name) support.release(name);
            support.process.release_guest(record);
        };
        if (client.main_thread) bridge->dispatch_async(bridge->main_queue(), std::move(work));
        else if (worker) worker->post(std::move(work));
    }

    // A guest MIDIEventList of `packets` translated to `protocol` (#pragma pack(4): {MIDIProtocolID
    // protocol; UInt32 numPackets; MIDIEventPacket packet[]}, MIDIEventPacket {MIDITimeStamp
    // timeStamp; UInt32 wordCount; UInt32 words[]}, the next packet right after the words; at most
    // 64 words a packet).
    Address event_list(const std::vector<midi::Packet>& packets, std::uint32_t protocol) {
        std::vector<std::pair<std::uint64_t, std::vector<std::uint32_t>>> parts;
        for (const auto& packet : packets) {
            const auto words = midi::ump::from_bytes(packet.data, protocol);
            std::vector<std::uint32_t> current;
            for (std::size_t i = 0; i < words.size();) {
                const auto n = std::min(midi::ump::word_count(words[i]), words.size() - i);
                if (current.size() + n > 64) parts.emplace_back(packet.time, std::exchange(current, {}));
                current.insert(current.end(), words.begin() + static_cast<std::ptrdiff_t>(i), words.begin() + static_cast<std::ptrdiff_t>(i + n));
                i += n;
            }
            if (!current.empty()) parts.emplace_back(packet.time, std::move(current));
        }
        std::uint64_t size = 8;
        for (const auto& [time, words] : parts) size += 12 + 4 * words.size();
        const auto list = support.process.allocate_guest(size, 8);
        auto& m = support.memory();
        m.write32(list, protocol);
        m.write32(list + 4, static_cast<std::uint32_t>(parts.size()));
        auto at = list + 8;
        for (const auto& [time, words] : parts) {
            m.write64(at, time);
            m.write32(at + 8, static_cast<std::uint32_t>(words.size()));
            for (std::size_t i = 0; i < words.size(); ++i) m.write32(at + 12 + 4 * i, words[i]);
            at += 12 + 4 * words.size();
        }
        return list;
    }
    // A guest MIDIEventList as MIDI 1.0 packets.
    std::vector<midi::Packet> read_event_list(GuestMemory& m, Address list) {
        std::vector<midi::Packet> packets;
        if (!list) return packets;
        const auto count = m.read32(list + 4);
        auto at = list + 8;
        for (std::uint32_t i = 0; i < count; ++i) {
            midi::Packet packet;
            packet.time = m.read64(at);
            const auto words = std::min<std::uint32_t>(m.read32(at + 8), 64);
            std::vector<std::uint32_t> data(words);
            for (std::uint32_t k = 0; k < words; ++k) data[k] = m.read32(at + 12 + 4ULL * k);
            packet.data = midi::ump::to_bytes(data);
            if (!packet.data.empty()) packets.push_back(std::move(packet));
            at += 12 + 4ULL * words;
        }
        return packets;
    }

    std::vector<midi::Packet> read_packet_list(GuestMemory& m, Address list) {
        std::vector<midi::Packet> packets;
        if (!list) return packets;
        const auto count = m.read32(list);
        auto at = list + 4;
        for (std::uint32_t i = 0; i < count; ++i) {
            midi::Packet packet;
            packet.time = m.read64(at);
            const auto length = static_cast<std::size_t>(m.read8(at + 8) | (m.read8(at + 9) << 8));
            packet.data.resize(length);
            if (length) m.read_bytes(at + 10, packet.data);
            packets.push_back(std::move(packet));
            at += (10 + length + 3) & ~std::uint64_t{3};
        }
        return packets;
    }
};

void status(CpuState& s, std::int32_t value) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

std::weak_ptr<Midi>& process_midi() {
    static std::weak_ptr<Midi> value;
    return value;
}

} // namespace

std::int32_t midi_send_internal(midi::ObjectRef destination, std::vector<midi::Packet> packets) {
    const auto state = process_midi().lock();
    if (!state) return midi::error::unknown_endpoint;
    static std::mutex mutex;
    static midi::ObjectRef port{};
    {
        std::lock_guard lock(mutex);
        if (!port) {
            const auto client = state->core().create_client("MusicPlayer");
            if (const auto result = state->core().create_port(client, "MusicPlayer", false, port)) return result;
        }
    }
    return state->core().send(port, destination, std::move(packets));
}

std::int32_t midi_flush_internal(midi::ObjectRef destination) {
    const auto state = process_midi().lock();
    return state ? state->core().flush(destination) : midi::error::unknown_endpoint;
}

bool midi_is_destination(midi::ObjectRef object) {
    const auto state = process_midi().lock();
    return state && state->core().type_of(object) == midi::ObjectType::destination;
}

std::uint64_t host_time_now() { return host::time::monotonic_ns(); }

void set_midi_backend_factory(std::function<std::unique_ptr<midi::Backend>()> factory) { midi_factory() = std::move(factory); }

void install_core_midi(Support& support) {
    auto state = std::make_shared<Midi>(support);
    state->main_thread = host::process::current_thread_id();   // installers run on the main thread
    process_midi() = state;
    auto* sp = &support;
    const auto text = [sp](Address string) { return string ? sp->cf.utf8(string) : std::string(); };
    using midi::ObjectType;

    // ---- property names --------------------------------------------------------
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_kMIDIPropertyName", midi::property::name}, {"_kMIDIPropertyManufacturer", midi::property::manufacturer},
             {"_kMIDIPropertyModel", midi::property::model}, {"_kMIDIPropertyUniqueID", midi::property::unique_id},
             {"_kMIDIPropertyDeviceID", midi::property::device_id}, {"_kMIDIPropertyReceiveChannels", midi::property::receive_channels},
             {"_kMIDIPropertyTransmitChannels", midi::property::transmit_channels}, {"_kMIDIPropertyMaxSysExSpeed", midi::property::max_sysex_speed},
             {"_kMIDIPropertyAdvanceScheduleTimeMuSec", midi::property::advance_schedule_time}, {"_kMIDIPropertyIsEmbeddedEntity", midi::property::embedded},
             {"_kMIDIPropertyIsBroadcast", midi::property::broadcast}, {"_kMIDIPropertySingleRealtimeEntity", midi::property::single_realtime_entity},
             {"_kMIDIPropertyConnectionUniqueID", midi::property::connection_unique_id}, {"_kMIDIPropertyOffline", midi::property::offline},
             {"_kMIDIPropertyPrivate", midi::property::private_}, {"_kMIDIPropertyDriverOwner", midi::property::driver_owner},
             {"_kMIDIPropertyImage", midi::property::image}, {"_kMIDIPropertyDriverVersion", midi::property::driver_version},
             {"_kMIDIPropertyDisplayName", midi::property::display_name}, {"_kMIDIPropertySupportsGeneralMIDI", midi::property::supports_general_midi}})
        support.string_constant(core_midi, symbol, value);

    // ---- clients and ports -----------------------------------------------------
    // MIDIClientCreate(CFStringRef name, MIDINotifyProc notifyProc, void *notifyRefCon, MIDIClientRef *outClient)
    support.function(core_midi, "_MIDIClientCreate", [state, text](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) return status(s, midi::error::param);
        state->ensure_thread();
        const auto client = state->core().create_client(text(s.x[0]));
        {
            std::lock_guard lock(state->mutex);
            state->clients[client] = {s.x[1], s.x[2], host::process::current_thread_id() == state->main_thread};
        }
        m.write32(s.x[3], client);
        status(s, 0);
    });
    support.function(core_midi, "_MIDIClientDispose", [state](CpuState& s, GuestMemory&) {
        const auto client = static_cast<midi::ObjectRef>(s.x[0]);
        const auto result = state->core().dispose_client(client);
        if (!result) { std::lock_guard lock(state->mutex); state->clients.erase(client); }
        status(s, result);
    });
    // MIDIInputPortCreate(client, portName, MIDIReadProc readProc, void *refCon, MIDIPortRef *outPort)
    support.function(core_midi, "_MIDIInputPortCreate", [state, text](CpuState& s, GuestMemory& m) {
        if (!s.x[4] || !s.x[2]) return status(s, midi::error::param);
        midi::ObjectRef port{};
        const auto result = state->core().create_port(static_cast<midi::ObjectRef>(s.x[0]), text(s.x[1]), true, port);
        if (!result) {
            { std::lock_guard lock(state->mutex); state->read_procs[port] = {s.x[2], s.x[3]}; }
            m.write32(s.x[4], port);
        }
        status(s, result);
    });
    // MIDIInputPortCreateWithBlock(client, portName, MIDIPortRef *outPort, MIDIReadBlock): MIDIPacketLists.
    // MIDIInputPortCreateWithProtocol(client, portName, MIDIProtocolID, MIDIPortRef *outPort, MIDIReceiveBlock):
    // MIDIEventLists in that protocol.
    const auto input_port = [state, text](CpuState& s, GuestMemory& m, Address name, Address out, Address block, bool event_list, std::uint32_t protocol) {
        if (!out || !block) return status(s, midi::error::param);
        if (event_list && protocol != midi::ump::protocol_1_0 && protocol != midi::ump::protocol_2_0) return status(s, midi::error::param);
        midi::ObjectRef port{};
        const auto result = state->core().create_port(static_cast<midi::ObjectRef>(s.x[0]), text(name), true, port);
        if (!result) {
            { std::lock_guard lock(state->mutex); state->read_procs[port] = {0, 0, state->bridge->block_copy(block), event_list, protocol}; }
            m.write32(out, port);
        }
        status(s, result);
    };
    support.function(core_midi, "_MIDIInputPortCreateWithBlock", [input_port](CpuState& s, GuestMemory& m) {
        input_port(s, m, s.x[1], s.x[2], s.x[3], false, 0);
    });
    support.function(core_midi, "_MIDIInputPortCreateWithProtocol", [input_port](CpuState& s, GuestMemory& m) {
        input_port(s, m, s.x[1], s.x[3], s.x[4], true, static_cast<std::uint32_t>(s.x[2]));
    });
    support.function(core_midi, "_MIDIOutputPortCreate", [state, text](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, midi::error::param);
        midi::ObjectRef port{};
        const auto result = state->core().create_port(static_cast<midi::ObjectRef>(s.x[0]), text(s.x[1]), false, port);
        if (!result) m.write32(s.x[2], port);
        status(s, result);
    });
    // The receiving side of a disposed port or endpoint goes (its block released).
    const auto forget = [state](midi::ObjectRef object) {
        Address block{};
        {
            std::lock_guard lock(state->mutex);
            const auto found = state->read_procs.find(object);
            if (found == state->read_procs.end()) return;
            block = found->second.block;
            state->read_procs.erase(found);
        }
        if (block) state->bridge->block_release(block);
    };
    support.function(core_midi, "_MIDIPortDispose", [state, forget](CpuState& s, GuestMemory&) {
        const auto port = static_cast<midi::ObjectRef>(s.x[0]);
        const auto result = state->core().dispose_port(port);
        if (!result) forget(port);
        status(s, result);
    });
    support.function(core_midi, "_MIDIPortConnectSource", [state](CpuState& s, GuestMemory&) {
        status(s, state->core().connect(static_cast<midi::ObjectRef>(s.x[0]), static_cast<midi::ObjectRef>(s.x[1]), s.x[2]));
    });
    support.function(core_midi, "_MIDIPortDisconnectSource", [state](CpuState& s, GuestMemory&) {
        status(s, state->core().disconnect(static_cast<midi::ObjectRef>(s.x[0]), static_cast<midi::ObjectRef>(s.x[1])));
    });

    // ---- virtual endpoints -------------------------------------------------------
    // MIDISourceCreate(client, name, MIDIEndpointRef *outSrc)
    support.function(core_midi, "_MIDISourceCreate", [state, text](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, midi::error::param);
        midi::ObjectRef endpoint{};
        const auto result = state->core().create_virtual(static_cast<midi::ObjectRef>(s.x[0]), text(s.x[1]), true, endpoint);
        if (!result) m.write32(s.x[2], endpoint);
        status(s, result);
    });
    // MIDIDestinationCreate(client, name, MIDIReadProc readProc, void *refCon, MIDIEndpointRef *outDest)
    support.function(core_midi, "_MIDIDestinationCreate", [state, text](CpuState& s, GuestMemory& m) {
        if (!s.x[4] || !s.x[2]) return status(s, midi::error::param);
        midi::ObjectRef endpoint{};
        // Registered before the endpoint exists for other clients, so no packet finds it without a proc.
        const auto result = state->core().create_virtual(static_cast<midi::ObjectRef>(s.x[0]), text(s.x[1]), false, endpoint);
        if (!result) {
            { std::lock_guard lock(state->mutex); state->read_procs[endpoint] = {s.x[2], s.x[3]}; }
            m.write32(s.x[4], endpoint);
        }
        status(s, result);
    });
    // MIDIDestinationCreateWithBlock(client, name, MIDIEndpointRef *outDest, MIDIReadBlock)
    // MIDIDestinationCreateWithProtocol(client, name, MIDIProtocolID, MIDIEndpointRef *outDest, MIDIReceiveBlock)
    const auto destination = [state, text](CpuState& s, GuestMemory& m, Address name, Address out, Address block, bool event_list, std::uint32_t protocol) {
        if (!out || !block) return status(s, midi::error::param);
        if (event_list && protocol != midi::ump::protocol_1_0 && protocol != midi::ump::protocol_2_0) return status(s, midi::error::param);
        midi::ObjectRef endpoint{};
        const auto result = state->core().create_virtual(static_cast<midi::ObjectRef>(s.x[0]), text(name), false, endpoint);
        if (!result) {
            { std::lock_guard lock(state->mutex); state->read_procs[endpoint] = {0, 0, state->bridge->block_copy(block), event_list, protocol}; }
            m.write32(out, endpoint);
        }
        status(s, result);
    };
    support.function(core_midi, "_MIDIDestinationCreateWithBlock", [destination](CpuState& s, GuestMemory& m) {
        destination(s, m, s.x[1], s.x[2], s.x[3], false, 0);
    });
    support.function(core_midi, "_MIDIDestinationCreateWithProtocol", [destination](CpuState& s, GuestMemory& m) {
        destination(s, m, s.x[1], s.x[3], s.x[4], true, static_cast<std::uint32_t>(s.x[2]));
    });
    // MIDISourceCreateWithProtocol(client, name, MIDIProtocolID, MIDIEndpointRef *outSrc)
    support.function(core_midi, "_MIDISourceCreateWithProtocol", [state, text](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) return status(s, midi::error::param);
        const auto protocol = static_cast<std::uint32_t>(s.x[2]);
        if (protocol != midi::ump::protocol_1_0 && protocol != midi::ump::protocol_2_0) return status(s, midi::error::param);
        midi::ObjectRef endpoint{};
        const auto result = state->core().create_virtual(static_cast<midi::ObjectRef>(s.x[0]), text(s.x[1]), true, endpoint);
        if (!result) m.write32(s.x[3], endpoint);
        status(s, result);
    });
    support.function(core_midi, "_MIDIEndpointDispose", [state, forget](CpuState& s, GuestMemory&) {
        const auto endpoint = static_cast<midi::ObjectRef>(s.x[0]);
        const auto result = state->core().dispose_endpoint(endpoint);
        if (!result) forget(endpoint);
        status(s, result);
    });

    // ---- enumeration -------------------------------------------------------------
    support.function(core_midi, "_MIDIGetNumberOfDevices", [state](CpuState& s, GuestMemory&) { s.x[0] = state->core().device_count(); });
    support.function(core_midi, "_MIDIGetDevice", [state](CpuState& s, GuestMemory&) { s.x[0] = state->core().device(s.x[0]); });
    support.function(core_midi, "_MIDIGetNumberOfSources", [state](CpuState& s, GuestMemory&) { s.x[0] = state->core().source_count(); });
    support.function(core_midi, "_MIDIGetSource", [state](CpuState& s, GuestMemory&) { s.x[0] = state->core().source(s.x[0]); });
    support.function(core_midi, "_MIDIGetNumberOfDestinations", [state](CpuState& s, GuestMemory&) { s.x[0] = state->core().destination_count(); });
    support.function(core_midi, "_MIDIGetDestination", [state](CpuState& s, GuestMemory&) { s.x[0] = state->core().destination(s.x[0]); });
    // External devices are the user's studio setup (Audio MIDI Setup): none is configured.
    support.function(core_midi, "_MIDIGetNumberOfExternalDevices", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    support.function(core_midi, "_MIDIGetExternalDevice", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    support.function(core_midi, "_MIDIDeviceGetNumberOfEntities", [state](CpuState& s, GuestMemory&) {
        s.x[0] = state->core().child_count(static_cast<midi::ObjectRef>(s.x[0]), ObjectType::entity);
    });
    support.function(core_midi, "_MIDIDeviceGetEntity", [state](CpuState& s, GuestMemory&) {
        s.x[0] = state->core().child(static_cast<midi::ObjectRef>(s.x[0]), ObjectType::entity, s.x[1]);
    });
    for (const auto& [count_name, get_name, type] : std::initializer_list<std::tuple<const char*, const char*, ObjectType>>{
             {"_MIDIEntityGetNumberOfSources", "_MIDIEntityGetSource", ObjectType::source},
             {"_MIDIEntityGetNumberOfDestinations", "_MIDIEntityGetDestination", ObjectType::destination}}) {
        const auto endpoint_type = type;
        support.function(core_midi, count_name, [state, endpoint_type](CpuState& s, GuestMemory&) {
            s.x[0] = state->core().child_count(static_cast<midi::ObjectRef>(s.x[0]), endpoint_type);
        });
        support.function(core_midi, get_name, [state, endpoint_type](CpuState& s, GuestMemory&) {
            s.x[0] = state->core().child(static_cast<midi::ObjectRef>(s.x[0]), endpoint_type, s.x[1]);
        });
    }
    // MIDIEntityGetDevice(entity, MIDIDeviceRef *out), MIDIEndpointGetEntity(endpoint, MIDIEntityRef *out)
    for (const auto& [name, parent_type] : std::initializer_list<std::pair<const char*, ObjectType>>{
             {"_MIDIEntityGetDevice", ObjectType::device}, {"_MIDIEndpointGetEntity", ObjectType::entity}}) {
        const auto wanted = parent_type;
        support.function(core_midi, name, [state, wanted](CpuState& s, GuestMemory& m) {
            midi::ObjectRef parent{};
            const auto result = state->core().parent(static_cast<midi::ObjectRef>(s.x[0]), wanted, parent);
            if (s.x[1]) m.write32(s.x[1], parent);
            status(s, result);
        });
    }
    // MIDIObjectFindByUniqueID(MIDIUniqueID, MIDIObjectRef *outObject, MIDIObjectType *outType)
    support.function(core_midi, "_MIDIObjectFindByUniqueID", [state](CpuState& s, GuestMemory& m) {
        midi::ObjectRef object{};
        midi::ObjectType type{};
        const auto result = state->core().find_unique_id(static_cast<std::int32_t>(s.x[0]), object, type);
        if (s.x[1]) m.write32(s.x[1], object);
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(type));
        status(s, result);
    });

    // ---- properties ------------------------------------------------------------------
    support.function(core_midi, "_MIDIObjectGetIntegerProperty", [state, text](CpuState& s, GuestMemory& m) {
        midi::Value value;
        auto result = state->core().get(static_cast<midi::ObjectRef>(s.x[0]), text(s.x[1]), value);
        if (!result && !std::holds_alternative<std::int32_t>(value)) result = midi::error::wrong_property_type;
        if (!result && s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(std::get<std::int32_t>(value)));
        status(s, result);
    });
    support.function(core_midi, "_MIDIObjectGetStringProperty", [state, text, sp](CpuState& s, GuestMemory& m) {
        midi::Value value;
        auto result = state->core().get(static_cast<midi::ObjectRef>(s.x[0]), text(s.x[1]), value);
        if (!result && !std::holds_alternative<std::string>(value)) result = midi::error::wrong_property_type;
        if (s.x[2]) m.write64(s.x[2], result ? 0 : sp->cf.string(std::get<std::string>(value)));   // +1
        status(s, result);
    });
    support.function(core_midi, "_MIDIObjectGetDataProperty", [state, text](CpuState& s, GuestMemory& m) {
        midi::Value value;
        auto result = state->core().get(static_cast<midi::ObjectRef>(s.x[0]), text(s.x[1]), value);
        if (!result && !std::holds_alternative<std::vector<std::uint8_t>>(value)) result = midi::error::wrong_property_type;
        if (s.x[2]) m.write64(s.x[2], result ? 0 : state->bridge->data(std::get<std::vector<std::uint8_t>>(value)));   // +1
        status(s, result);
    });
    support.function(core_midi, "_MIDIObjectSetIntegerProperty", [state, text](CpuState& s, GuestMemory&) {
        status(s, state->core().set(static_cast<midi::ObjectRef>(s.x[0]), text(s.x[1]), static_cast<std::int32_t>(s.x[2])));
    });
    support.function(core_midi, "_MIDIObjectSetStringProperty", [state, text](CpuState& s, GuestMemory&) {
        const auto value = state->bridge->maybe_text(s.x[2]);
        if (!value) return status(s, midi::error::param);
        status(s, state->core().set(static_cast<midi::ObjectRef>(s.x[0]), text(s.x[1]), *value));
    });
    support.function(core_midi, "_MIDIObjectSetDataProperty", [state, text](CpuState& s, GuestMemory&) {
        if (!s.x[2] || !state->bridge->is_data(s.x[2])) return status(s, midi::error::param);
        status(s, state->core().set(static_cast<midi::ObjectRef>(s.x[0]), text(s.x[1]), state->bridge->bytes(s.x[2])));
    });
    support.function(core_midi, "_MIDIObjectRemoveProperty", [state, text](CpuState& s, GuestMemory&) {
        status(s, state->core().remove(static_cast<midi::ObjectRef>(s.x[0]), text(s.x[1])));
    });
    // MIDIObjectGetProperties(object, CFPropertyListRef *outProperties, Boolean deep): a +1
    // dictionary; deep adds the children ("entities", "sources", "destinations").
    auto dictionary_of = std::make_shared<std::function<Address(midi::ObjectRef, bool)>>();
    *dictionary_of = [state, sp, weak = std::weak_ptr(dictionary_of)](midi::ObjectRef object, bool deep) -> Address {
        std::map<std::string, midi::Value> values;
        if (state->core().properties(object, values)) return 0;
        std::vector<std::pair<Address, Address>> entries;
        for (const auto& [key, value] : values) {
            Address item{};
            if (const auto* number = std::get_if<std::int32_t>(&value)) item = sp->cf.integer(*number, 3);   // kCFNumberSInt32Type
            else if (const auto* string = std::get_if<std::string>(&value)) item = sp->cf.string(*string);
            else item = state->bridge->data(std::get<std::vector<std::uint8_t>>(value));
            entries.emplace_back(sp->cf.string(key), item);
        }
        if (deep) {
            const auto self = weak.lock();
            const auto type = state->core().type_of(object);
            const auto children = [&](const char* key, ObjectType child_type) {
                std::vector<Address> items;
                const auto count = state->core().child_count(object, child_type);
                for (std::size_t i = 0; i < count; ++i)
                    if (const auto made = (*self)(state->core().child(object, child_type, i), true)) items.push_back(made);
                entries.emplace_back(sp->cf.string(key), state->bridge->array(items));
                state->bridge->release_all(items);
            };
            if (type == ObjectType::device) children("entities", ObjectType::entity);
            if (type == ObjectType::entity) { children("sources", ObjectType::source); children("destinations", ObjectType::destination); }
        }
        const auto dictionary = state->bridge->dictionary(entries);
        for (const auto& [key, item] : entries) { sp->release(key); sp->release(item); }
        return dictionary;
    };
    support.function(core_midi, "_MIDIObjectGetProperties", [state, dictionary_of](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) return status(s, midi::error::param);
        const auto object = static_cast<midi::ObjectRef>(s.x[0]);
        const auto dictionary = (*dictionary_of)(object, (s.x[2] & 0xff) != 0);
        m.write64(s.x[1], dictionary);
        status(s, dictionary ? 0 : midi::error::object_not_found);
    });

    // ---- I/O ---------------------------------------------------------------------------
    // MIDISend(MIDIPortRef port, MIDIEndpointRef dest, const MIDIPacketList *pktlist)
    support.function(core_midi, "_MIDISend", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, midi::error::param);
        status(s, state->core().send(static_cast<midi::ObjectRef>(s.x[0]), static_cast<midi::ObjectRef>(s.x[1]), state->read_packet_list(m, s.x[2])));
    });
    // MIDISendEventList(port, dest, const MIDIEventList *), MIDIReceivedEventList(src, const MIDIEventList *):
    // the event list's messages as MIDI 1.0.
    support.function(core_midi, "_MIDISendEventList", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[2]) return status(s, midi::error::param);
        status(s, state->core().send(static_cast<midi::ObjectRef>(s.x[0]), static_cast<midi::ObjectRef>(s.x[1]), state->read_event_list(m, s.x[2])));
    });
    support.function(core_midi, "_MIDIReceivedEventList", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) return status(s, midi::error::param);
        status(s, state->core().received(static_cast<midi::ObjectRef>(s.x[0]), state->read_event_list(m, s.x[1])));
    });
    // MIDIReceived(MIDIEndpointRef src, const MIDIPacketList *pktlist): a virtual source's output.
    support.function(core_midi, "_MIDIReceived", [state](CpuState& s, GuestMemory& m) {
        if (!s.x[1]) return status(s, midi::error::param);
        status(s, state->core().received(static_cast<midi::ObjectRef>(s.x[0]), state->read_packet_list(m, s.x[1])));
    });
    // MIDISendSysex(MIDISysexSendRequest *request): {MIDIEndpointRef destination; const Byte *data;
    // UInt32 bytesToSend; Boolean complete; Byte reserved[3]; MIDICompletionProc completionProc;
    // void *completionRefCon}. Sent at once; the completion proc runs on the CoreMIDI thread.
    support.function(core_midi, "_MIDISendSysex", [state](CpuState& s, GuestMemory& m) {
        const auto request = s.x[0];
        if (!request) return status(s, midi::error::param);
        const auto destination = m.read32(request);
        const auto data = m.read64(request + 8);
        const auto count = m.read32(request + 16);
        if (count && !data) return status(s, midi::error::param);
        std::vector<std::uint8_t> bytes(count);
        if (count) m.read_bytes(data, bytes);
        const auto result = state->core().send_sysex(destination, std::move(bytes));
        if (result) return status(s, result);
        m.write64(request + 8, data + count);
        m.write32(request + 16, 0);
        m.write8(request + 20, 1);
        const auto completion = m.read64(request + 24);
        if (completion) {
            std::shared_ptr<CompletionThread> worker;
            { std::lock_guard lock(state->mutex); worker = state->thread; }
            if (!worker) state->ensure_thread();
            { std::lock_guard lock(state->mutex); worker = state->thread; }
            worker->post([state, completion, request] {
                const std::array<Address, 1> arguments{request};
                static_cast<void>(state->bridge->call_address(completion, arguments));
            });
        }
        status(s, 0);
    });
    support.function(core_midi, "_MIDIFlushOutput", [state](CpuState& s, GuestMemory&) {
        status(s, state->core().flush(static_cast<midi::ObjectRef>(s.x[0])));
    });

    // ---- packet lists ------------------------------------------------------------------
    // MIDIPacketList (#pragma pack(4)): {UInt32 numPackets; MIDIPacket packet[]};
    // MIDIPacket {UInt64 timeStamp; UInt16 length; Byte data[]}; on arm64 the
    // next packet starts at the following 4-byte boundary.
    support.function(core_midi, "_MIDIPacketListInit", [](CpuState& s, GuestMemory& m) {
        m.write32(s.x[0], 0);
        s.x[0] = s.x[0] + 4;
    });
    // MIDIEventPacket *MIDIEventListInit(MIDIEventList *, MIDIProtocolID)
    support.function(core_midi, "_MIDIEventListInit", [](CpuState& s, GuestMemory& m) {
        m.write32(s.x[0], static_cast<std::uint32_t>(s.x[1]));
        m.write32(s.x[0] + 4, 0);
        s.x[0] = s.x[0] + 8;
    });
    // MIDIEventPacket *MIDIEventListAdd(list, ByteCount listSize, MIDIEventPacket *cur, MIDITimeStamp time,
    //     ByteCount wordCount, const UInt32 *words): appended to `cur` when the time is the same and it fits.
    support.function(core_midi, "_MIDIEventListAdd", [](CpuState& s, GuestMemory& m) {
        const auto list = s.x[0], size = s.x[1], current = s.x[2], time = s.x[3], count = s.x[4], words = s.x[5];
        if (!list || !current) { s.x[0] = 0; return; }
        const auto packets = m.read32(list + 4);
        const auto end = list + size;
        if (packets && m.read64(current) == time) {
            const auto used = m.read32(current + 8);
            if (used + count <= 64 && current + 12 + (used + count) * 4 <= end) {
                for (std::uint64_t i = 0; i < count; ++i) m.write32(current + 12 + (used + i) * 4, m.read32(words + i * 4));
                m.write32(current + 8, static_cast<std::uint32_t>(used + count));
                s.x[0] = current;
                return;
            }
        }
        const auto packet = packets ? current + 12 + m.read32(current + 8) * 4ULL : current;
        if (count > 64 || packet + 12 + count * 4 > end) { s.x[0] = 0; return; }   // no room
        m.write64(packet, time);
        m.write32(packet + 8, static_cast<std::uint32_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) m.write32(packet + 12 + i * 4, m.read32(words + i * 4));
        m.write32(list + 4, packets + 1);
        s.x[0] = packet;
    });
    // MIDIPacket *MIDIPacketListAdd(list, listSize, curPacket, time, nData, data)
    support.function(core_midi, "_MIDIPacketListAdd", [](CpuState& s, GuestMemory& m) {
        const auto list = s.x[0], size = s.x[1], current = s.x[2], time = s.x[3], count = s.x[4], bytes = s.x[5];
        if (!list || !current || count > 65535) { s.x[0] = 0; return; }
        const auto read16 = [&m](Address at) { return static_cast<std::uint64_t>(m.read8(at) | (m.read8(at + 1) << 8)); };
        const auto write16 = [&m](Address at, std::uint64_t value) { m.write8(at, static_cast<std::uint8_t>(value)); m.write8(at + 1, static_cast<std::uint8_t>(value >> 8)); };
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(count));
        if (count) m.read_bytes(bytes, payload);
        const auto packets = m.read32(list);
        const auto end = list + size;
        if (packets) {
            const auto length = read16(current + 8);
            const auto first = length ? m.read8(current + 10) : 0;
            // Same timestamp, neither side SysEx, still within a 256-byte packet: append.
            if (m.read64(current) == time && count && payload[0] != 0xf0 && first != 0xf0 && length + count <= 256 &&
                current + 10 + length + count <= end) {
                m.write_bytes(current + 10 + length, payload);
                write16(current + 8, length + count);
                s.x[0] = current;
                return;
            }
        }
        const auto next = packets ? ((current + 10 + read16(current + 8) + 3) & ~Address{3}) : current;
        if (next + 10 + count > end) { s.x[0] = 0; return; }
        m.write64(next, time);
        write16(next + 8, count);
        if (count) m.write_bytes(next + 10, payload);
        m.write32(list, packets + 1);
        s.x[0] = next;
    });
}

} // namespace alchemy::gui::completion_kit::media
