#include "alchemy/darwin/iokit/devices.hpp"

#include "alchemy/host/system.hpp"

#include "alchemy/darwin_threads.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

// IOKit device and power gaps, ForceFeedback and DiskArbitration.
//
// - HID: the IOKit registry (src/alchemy/darwin/kit/export_fill/iokit_registry.cpp) has no
//   HID devices. IOHIDManager objects are real (matching, callbacks, run-loop
//   scheduling are recorded) and never report a device; IOHIDDevice,
//   IOHIDElement and IOHIDValue objects cannot exist.
// - Notifications: IONotificationPort objects are real; matching
//   notifications return the current matches (the registry never changes,
//   so later arrivals and terminations never happen).
// - Power: power assertions are real host power requests
//   (host::system::PowerRequest): display and system sleep are held off
//   while an assertion lives. Power-source information comes from
//   host::system::power_status (one internal battery when the host has one).
//   IOPSNotificationCreateRunLoopSource is real: the host's AC/DC and
//   battery-percentage notifications (host::system::watch_power_source)
//   signal every such source and wake the
//   run loops it is scheduled on, whose perform calls the callback. Windows
//   also reports each setting once at registration, so the first source can
//   see one change right after it is created.
// - System power (IORegisterForSystemPower): real over the host's
//   suspend/resume broadcasts (PowerRegisterSuspendResumeNotification).
//   Clients get kIOMessageSystemWillSleep on suspend (Windows does not wait
//   for IOAllowPowerChange) and kIOMessageSystemWillPowerOn then
//   kIOMessageSystemHasPoweredOn on resume, on the notification port's
//   run-loop source (the main run loop is woken) or dispatch queue. A guest
//   dispatch worker picks the host events up (polled every 50 ms). The root
//   port and IONotificationPortGetMachPort are real Mach receive rights. IOPMAssertionCreateWithDescription creates the same
//   assertions; IOPMCopyAssertionsByProcess lists this process's assertions
//   (other processes' are not visible). IOObjectIsEqualTo compares handles.
// - ForceFeedback: no force-feedback device exists (FFERR_NOINTERFACE).
// - DiskArbitration: no Disk Arbitration daemon runs (sessions and disks
//   cannot be created).
namespace alchemy::gui::fill {
namespace {

constexpr std::int32_t io_return_success = 0;
constexpr std::int32_t io_return_bad_argument = static_cast<std::int32_t>(0xe00002c2U), io_return_no_device = static_cast<std::int32_t>(0xe00002c0U),
    io_return_unsupported = static_cast<std::int32_t>(0xe00002c7U), io_return_not_found = static_cast<std::int32_t>(0xe00002f0U);
constexpr std::int32_t fferr_no_interface = static_cast<std::int32_t>(0x80004002U), fferr_invalid_param = static_cast<std::int32_t>(0x80070057U);

void impossible(std::uint64_t object, std::string_view what) {
    if (object) throw std::runtime_error("Alchemy: " + std::string(what) + " received an object that cannot exist under Alchemy");
}

struct HidManager final : ObjectPayload {
    std::mutex mutex;
    Address matching{};              // retained
    std::array<std::pair<Address, Address>, 3> callbacks{};   // matching, removal, input value
    bool open{};
};
struct NotificationPort final : ObjectPayload {
    Address source{}, queue{};
    std::uint32_t mach_port{};
    std::deque<std::pair<std::uint32_t, std::uint64_t>> pending;   // (message, argument) for system-power clients
};

// kIOMessageSystem* (IOMessage.h: iokit_common_msg(n) = 0xe0000000 | n).
constexpr std::uint32_t message_will_sleep = 0xe0000280, message_will_power_on = 0xe0000320, message_has_powered_on = 0xe0000300;

// Host suspend/resume broadcasts (any thread), consumed by the guest watcher.
struct HostPowerEvents {
    std::mutex mutex;
    std::deque<std::uint32_t> messages;
    std::atomic<std::uint64_t> generation{0};
    bool sleeping{};
    void push_suspend() {
        std::lock_guard lock(mutex);
        sleeping = true;
        messages.push_back(message_will_sleep);
        ++generation;
    }
    void push_resume() {
        std::lock_guard lock(mutex);
        if (!sleeping) return;   // PBT_APMRESUMEAUTOMATIC and PBT_APMRESUMESUSPEND both arrive
        sleeping = false;
        messages.push_back(message_will_power_on);
        messages.push_back(message_has_powered_on);
        ++generation;
    }
};
HostPowerEvents& host_power_events() { static HostPowerEvents events; return events; }

// Registers once per process (host::system::watch_sleep).
bool register_host_power() {
    static const bool registered = static_cast<bool>(alchemy::host::system::watch_sleep([](bool sleeping) {
        if (sleeping) host_power_events().push_suspend();
        else host_power_events().push_resume();
    }));
    return registered;
}

// Host power-source changes (AC/DC, battery percentage): a generation the
// guest watcher compares.
std::atomic<std::uint64_t>& host_power_source_generation() { static std::atomic<std::uint64_t> generation{0}; return generation; }

// Registers once per process (host::system::watch_power_source: AC/DC and
// battery percentage).
bool register_host_power_sources() {
    static const bool registered =
        static_cast<bool>(alchemy::host::system::watch_power_source([] { ++host_power_source_generation(); }));
    return registered;
}

class IoKit final : public Provider {
public:
    using Provider::Provider;
    Address manager_class{}, port_class{};
    std::mutex mutex;
    std::map<std::uint32_t, alchemy::host::system::PowerRequest> assertions;
    struct AssertionInfo { std::string type, name; std::uint32_t level{}; };
    std::map<std::uint32_t, AssertionInfo> assertion_info;
    std::uint32_t next_assertion{1};
    // IORegisterForSystemPower clients: notifier handle -> client.
    struct PowerClient { Address port{}; Address callback{}, refcon{}; std::uint32_t root{}; };
    std::map<std::uint32_t, PowerClient> power_clients;
    std::set<std::uint32_t> power_roots;   // open root ports (until IOServiceClose)
    std::uint32_t next_notifier{0x50000001};
    std::uint64_t next_notification{1};
    bool power_watcher{};
    std::uint64_t power_seen{};
    // IOPSNotificationCreateRunLoopSource sources: info (an id) -> client.
    struct PowerSourceClient { Address callback{}, context{}, source{}; std::vector<Address> loops; };
    std::map<Address, PowerSourceClient> power_source_clients;
    Address next_power_source{1};
    bool power_source_watcher{};

    // A new Mach receive right (c4's in-process Mach IPC).
    std::uint32_t receive_right() {
        const auto task = memory().read32(symbol(libsystem, "_mach_task_self_"));
        const auto slot = process.allocate_guest(4);
        memory().write32(slot, 0);
        const auto kr = call(libsystem, "_mach_port_allocate", {task, 1 /* MACH_PORT_RIGHT_RECEIVE */, slot});
        const auto name = memory().read32(slot);
        process.release_guest(slot);
        return kr == 0 ? name : 0;
    }
    void drop_receive_right(std::uint32_t name) {
        if (!name) return;
        const auto task = memory().read32(symbol(libsystem, "_mach_task_self_"));
        static_cast<void>(call(libsystem, "_mach_port_mod_refs", {task, name, 1, static_cast<Address>(static_cast<std::int64_t>(-1))}));
    }
    // Runs the pending system-power messages of `port` (a guest thread: its
    // run-loop source or dispatch queue).
    void deliver_power(Address port) {
        const auto value = objc.find_host_state<NotificationPort>(port);
        if (!value) return;
        for (;;) {
            std::pair<std::uint32_t, std::uint64_t> message;
            std::vector<PowerClient> clients;
            {
                std::lock_guard lock(mutex);
                if (value->pending.empty()) return;
                message = value->pending.front();
                value->pending.pop_front();
                for (const auto& [notifier, client] : power_clients) if (client.port == port) clients.push_back(client);
            }
            // IOServiceInterestCallback(refcon, service (the root port), messageType, messageArgument)
            for (const auto& client : clients) {
                const std::array<Address, 4> arguments{client.refcon, client.root, message.first, message.second};
                static_cast<void>(bridge->call_address(client.callback, arguments));
            }
        }
    }
    // Queues `message` on every power port and wakes its delivery.
    void post_power(std::uint32_t message) {
        std::vector<Address> ports;
        {
            std::lock_guard lock(mutex);
            for (const auto& [notifier, client] : power_clients) if (std::find(ports.begin(), ports.end(), client.port) == ports.end()) ports.push_back(client.port);
        }
        for (const auto port : ports) {
            const auto value = objc.find_host_state<NotificationPort>(port);
            if (!value) continue;
            Address queue{}, source{};
            {
                std::lock_guard lock(mutex);
                value->pending.emplace_back(message, message == message_will_sleep ? next_notification++ : 0);
                queue = value->queue;
                source = value->source;
            }
            if (queue) {
                static_cast<void>(retain(port));
                bridge->dispatch_async(queue, [this, port] { deliver_power(port); release(port); });
            } else if (source) {
                static_cast<void>(call(core_foundation, "_CFRunLoopSourceSignal", {source}));
                static_cast<void>(call(core_foundation, "_CFRunLoopWakeUp", {call(core_foundation, "_CFRunLoopGetMain", {})}));
            }
        }
    }
    // The watcher (a guest dispatch worker) that turns host broadcasts into messages.
    void watch_power() {
        auto& events = host_power_events();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (guest_worker_must_stop(process)) throw GuestThreadStop{};
            if (events.generation.load() == power_seen) continue;
            std::deque<std::uint32_t> messages;
            {
                std::lock_guard lock(events.mutex);
                messages.swap(events.messages);
                power_seen = events.generation.load();
            }
            for (const auto message : messages) post_power(message);
        }
    }
    // The watcher (a guest dispatch worker) that signals the power-source
    // run-loop sources when the host generation changes.
    void watch_power_sources() {
        auto seen = host_power_source_generation().load();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (guest_worker_must_stop(process)) throw GuestThreadStop{};
            const auto now = host_power_source_generation().load();
            if (now == seen) continue;
            seen = now;
            std::vector<std::pair<Address, std::vector<Address>>> targets;
            {
                std::lock_guard lock(mutex);
                for (const auto& [id, client] : power_source_clients)
                    if (client.source && !client.loops.empty()) targets.emplace_back(client.source, client.loops);
            }
            for (const auto& [source, loops] : targets) {
                static_cast<void>(call(core_foundation, "_CFRunLoopSourceSignal", {source}));
                for (const auto loop : loops) static_cast<void>(call(core_foundation, "_CFRunLoopWakeUp", {loop}));
            }
        }
    }

    // An assertion of `type` (IOPMAssertionCreateWithName's types); 0 or an IOReturn (x0 image).
    Address create_assertion(const std::string& type, std::uint32_t level, const std::string& name, Address out, GuestMemory& m) {
        bool display = false, system = false;
        if (type == "PreventUserIdleDisplaySleep" || type == "NoDisplaySleepAssertion") display = system = true;
        else if (type == "PreventUserIdleSystemSleep" || type == "NoIdleSleepAssertion" || type == "PreventSystemSleep") system = true;
        else return status(io_return_unsupported);
        // Level 0 (kIOPMAssertionLevelOff) holds nothing until it is raised.
        auto request = alchemy::host::system::PowerRequest::create(name, system && level != 0, display && level != 0);
        if (!request) return status(io_return_unsupported);
        std::lock_guard lock(mutex);
        const auto id = next_assertion++;
        assertions[id] = std::move(*request);
        assertion_info[id] = {type, name, level};
        m.write32(out, id);
        return 0;   // kIOReturnSuccess
    }

    Address native_class(const char* name) {
        auto value = objc.find_class(name);
        if (!value) value = objc.register_native_class(name, objc.find_class("NSObject"), 8);
        return value;
    }
    std::shared_ptr<HidManager> manager(Address object) {
        const auto value = object ? objc.find_host_state<HidManager>(object) : nullptr;
        if (!value) throw std::runtime_error("Alchemy: the object is not an IOHIDManager");
        return value;
    }
    // An iterator that yields nothing (a matching dictionary no entry satisfies).
    std::int32_t empty_iterator(Address out) {
        const auto name = cstring("AlchemyNoSuchClass");
        const auto matching = call(iokit, "_IOServiceMatching", {name});
        return static_cast<std::int32_t>(call(iokit, "_IOServiceGetMatchingServices", {0, matching, out}));
    }
    void install_hid();
    void install_notifications();
    void install_power();
    void install_force_feedback();
    void install_disk_arbitration();
};

void IoKit::install_hid() {
    manager_class = native_class("__AlchemyIOHIDManager");
    register_type(manager_class, TypeId::hid_manager);
    if (!has(iokit, "_IOHIDManagerCreate")) {
        static_cast<void>(objc.add_native_method(manager_class, "dealloc", [self = shared_from_this(), this](CpuState& s, GuestMemory&) {
            if (const auto value = objc.find_host_state<HidManager>(s.x[0])) release(std::exchange(value->matching, 0));
            static_cast<void>(objc.send_super(s.x[0], manager_class, objc.selector("dealloc")));
        }, false, "v@:"));
    }
    constant(iokit, "_IOHIDManagerGetTypeID", static_cast<Address>(TypeId::hid_manager));
    raw(iokit, "_IOHIDManagerCreate", [this](CpuState& s, GuestMemory&) {
        const auto object = objc.allocate(manager_class);
        static_cast<void>(objc.host_state<HidManager>(object));
        s.x[0] = object;
    });
    raw(iokit, "_IOHIDManagerOpen", [this](CpuState& s, GuestMemory&) {
        const auto value = manager(s.x[0]);
        std::lock_guard lock(value->mutex);
        value->open = true;
        s.x[0] = io_return_success;
    });
    raw(iokit, "_IOHIDManagerClose", [this](CpuState& s, GuestMemory&) {
        const auto value = manager(s.x[0]);
        std::lock_guard lock(value->mutex);
        value->open = false;
        s.x[0] = io_return_success;
    });
    raw(iokit, "_IOHIDManagerSetDeviceMatchingMultiple", [this](CpuState& s, GuestMemory&) {
        const auto value = manager(s.x[0]);
        const auto matching = retain(s.x[1]);
        Address previous{};
        { std::lock_guard lock(value->mutex); previous = std::exchange(value->matching, matching); }
        release(previous);
    });
    const std::array<const char*, 3> callbacks{"_IOHIDManagerRegisterDeviceMatchingCallback", "_IOHIDManagerRegisterDeviceRemovalCallback",
                                               "_IOHIDManagerRegisterInputValueCallback"};
    for (std::size_t index = 0; index < callbacks.size(); ++index) {
        raw(iokit, callbacks[index], [this, index](CpuState& s, GuestMemory&) {
            const auto value = manager(s.x[0]);
            std::lock_guard lock(value->mutex);
            value->callbacks[index] = {s.x[1], s.x[2]};
        });
    }
    // Scheduling: no device event source exists, so nothing is added to the run loop.
    for (const auto* name : {"_IOHIDManagerScheduleWithRunLoop", "_IOHIDManagerUnscheduleFromRunLoop"})
        raw(iokit, name, [this](CpuState& s, GuestMemory&) { static_cast<void>(manager(s.x[0])); });
    // Devices, elements and values cannot exist.
    raw(iokit, "_IOHIDDeviceCreate", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    for (const auto* name : {"_IOHIDDeviceOpen", "_IOHIDDeviceClose", "_IOHIDDeviceGetReport", "_IOHIDDeviceSetReport"})
        failure(iokit, name, io_return_bad_argument);
    for (const auto* name : {"_IOHIDDeviceGetProperty", "_IOHIDDeviceCopyMatchingElements", "_IOHIDDeviceGetService", "_IOHIDElementGetDevice",
                             "_IOHIDElementGetLogicalMax", "_IOHIDElementGetLogicalMin", "_IOHIDElementGetParent", "_IOHIDElementGetReportID",
                             "_IOHIDElementGetReportSize", "_IOHIDElementGetType", "_IOHIDElementGetUsage", "_IOHIDElementGetUsagePage",
                             "_IOHIDValueGetBytePtr", "_IOHIDValueGetElement", "_IOHIDValueGetIntegerValue", "_IOHIDValueGetLength"}) {
        const std::string what(name + 1);
        raw(iokit, name, [what](CpuState& s, GuestMemory&) { impossible(s.x[0], what); s.x[0] = 0; });
    }
    for (const auto* name : {"_IOHIDDeviceRegisterInputReportCallback", "_IOHIDDeviceScheduleWithRunLoop", "_IOHIDDeviceUnscheduleFromRunLoop"}) {
        const std::string what(name + 1);
        raw(iokit, name, [what](CpuState& s, GuestMemory&) { impossible(s.x[0], what); });
    }
    // IOCreatePlugInInterfaceForService(service, pluginType, interfaceType, IOCFPlugInInterface ***, SInt32 *score)
    raw(iokit, "_IOCreatePlugInInterfaceForService", [](CpuState& s, GuestMemory& m) {
        if (s.x[3]) m.write64(s.x[3], 0);
        if (s.x[4]) m.write32(s.x[4], 0);
        s.x[0] = status(s.x[0] ? io_return_unsupported : io_return_no_device);
    });
}

void IoKit::install_notifications() {
    port_class = native_class("__AlchemyIONotificationPort");
    register_type(port_class, TypeId::notification_port);
    // The run-loop source of a notification port (info = the port): it fires
    // only for system-power messages (the IOKit registry never changes).
    static_cast<void>(process.install_function("__alchemy_io_notification_perform", [this](CpuState& s, GuestMemory&) {
        deliver_power(s.x[0]);
    }, "alchemy.internal"));
    // Test hook: a host suspend (1) or resume (2) broadcast.
    static_cast<void>(process.install_function("__alchemy_io_power_inject", [](CpuState& s, GuestMemory&) {
        if (s.x[0] == 1) host_power_events().push_suspend();
        else host_power_events().push_resume();
    }, "alchemy.internal"));
    raw(iokit, "_IONotificationPortCreate", [this](CpuState& s, GuestMemory&) {
        const auto object = objc.allocate(port_class);
        static_cast<void>(objc.host_state<NotificationPort>(object));
        s.x[0] = object;
    });
    raw(iokit, "_IONotificationPortDestroy", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0]) return;
        if (const auto value = objc.find_host_state<NotificationPort>(s.x[0])) {
            release(std::exchange(value->source, 0));
            if (const auto queue = std::exchange(value->queue, 0)) static_cast<void>(call(libsystem, "_dispatch_release", {queue}));
            drop_receive_right(std::exchange(value->mach_port, 0));
        }
        release(s.x[0]);
    });
    raw(iokit, "_IONotificationPortGetRunLoopSource", [this](CpuState& s, GuestMemory& m) {
        const auto value = s.x[0] ? objc.find_host_state<NotificationPort>(s.x[0]) : nullptr;
        if (!value) throw std::runtime_error("Alchemy: IONotificationPortGetRunLoopSource needs an IONotificationPort");
        std::lock_guard lock(mutex);
        if (!value->source) {
            const auto perform = process.resolve("alchemy.internal", "__alchemy_io_notification_perform");
            const auto context = process.allocate_guest(80, 16);
            m.fill_bytes(context, 0, 80);
            m.write64(context + 8, s.x[0]);              // info: the port
            m.write64(context + 72, perform->address);   // CFRunLoopSourceContext version 0: perform is the last field
            value->source = call(core_foundation, "_CFRunLoopSourceCreate", {0, 0, context});
            process.release_guest(context);
        }
        s.x[0] = value->source;
    });
    // mach_port_t IONotificationPortGetMachPort(port): a real receive right.
    raw(iokit, "_IONotificationPortGetMachPort", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? objc.find_host_state<NotificationPort>(s.x[0]) : nullptr;
        if (!value) { s.x[0] = 0; return; }
        std::lock_guard lock(mutex);
        if (!value->mach_port) value->mach_port = receive_right();
        s.x[0] = value->mach_port;
    });
    raw(iokit, "_IONotificationPortSetDispatchQueue", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? objc.find_host_state<NotificationPort>(s.x[0]) : nullptr;
        if (!value) throw std::runtime_error("Alchemy: IONotificationPortSetDispatchQueue needs an IONotificationPort");
        if (s.x[1]) static_cast<void>(call(libsystem, "_dispatch_retain", {s.x[1]}));
        Address previous{};
        { std::lock_guard lock(mutex); previous = std::exchange(value->queue, s.x[1]); }
        if (previous) static_cast<void>(call(libsystem, "_dispatch_release", {previous}));
    });
    // IOServiceAddMatchingNotification(port, type, matching (consumed), callback, refCon, io_iterator_t *):
    // first-match/publish/matched notifications start with the current matches;
    // termination notifications start empty (nothing is ever removed).
    raw(iokit, "_IOServiceAddMatchingNotification", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[5]) { release(s.x[2]); s.x[0] = status(io_return_bad_argument); return; }
        const auto type = read_cstring(m, s.x[1], 128);
        if (type == "IOServiceTerminate") {
            release(s.x[2]);
            s.x[0] = status(empty_iterator(s.x[5]));
            return;
        }
        s.x[0] = call(iokit, "_IOServiceGetMatchingServices", {0, s.x[2], s.x[5]});
    });
    // IOServiceAddInterestNotification(port, service, interestType, callback, refCon, io_object_t *):
    // the registry is static, so no interest notification is ever delivered.
    raw(iokit, "_IOServiceAddInterestNotification", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[5] || !s.x[1]) { s.x[0] = status(io_return_bad_argument); return; }
        const auto name = cstring("IOService");
        if (!(call(iokit, "_IOObjectConformsTo", {s.x[1], name}) & 0xff)) { m.write32(s.x[5], 0); s.x[0] = status(io_return_bad_argument); return; }
        s.x[0] = status(empty_iterator(s.x[5]));
    });
}

void IoKit::install_power() {
    // io_connect_t IORegisterForSystemPower(void *refcon, IONotificationPortRef *thePortRef,
    //     IOServiceInterestCallback callback, io_object_t *notifier): a new
    // notification port, a root port (a Mach receive right) and a notifier.
    raw(iokit, "_IORegisterForSystemPower", [this](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write64(s.x[1], 0);
        if (s.x[3]) m.write32(s.x[3], 0);
        if (!s.x[1] || !s.x[2] || !s.x[3]) { s.x[0] = 0; return; }   // MACH_PORT_NULL
        const auto port = call(iokit, "_IONotificationPortCreate", {0});
        const auto root = receive_right();
        if (!port || !root) { release(port); s.x[0] = 0; return; }
        const bool host = register_host_power();
        std::uint32_t notifier{};
        bool start = false;
        {
            std::lock_guard lock(mutex);
            notifier = next_notifier++;
            power_clients[notifier] = {port, s.x[2], s.x[0], root};
            power_roots.insert(root);
            if (host && !power_watcher) power_watcher = start = true;
        }
        if (start) {
            power_seen = host_power_events().generation.load();
            const auto worker = call(libsystem, "_dispatch_get_global_queue", {0, 0});
            bridge->dispatch_async(worker, [self = std::static_pointer_cast<IoKit>(shared_from_this())] { self->watch_power(); });
        }
        m.write64(s.x[1], port);
        m.write32(s.x[3], notifier);
        s.x[0] = root;
    });
    // IOReturn IODeregisterForSystemPower(io_object_t *notifier)
    raw(iokit, "_IODeregisterForSystemPower", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = status(io_return_bad_argument); return; }
        std::lock_guard lock(mutex);
        const auto found = power_clients.find(m.read32(s.x[0]));
        if (found == power_clients.end()) { s.x[0] = status(io_return_bad_argument); return; }
        power_clients.erase(found);
        m.write32(s.x[0], 0);
        s.x[0] = io_return_success;
    });
    // IOAllowPowerChange(root, notificationID): Windows does not wait for an answer.
    raw(iokit, "_IOAllowPowerChange", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        s.x[0] = power_roots.contains(static_cast<std::uint32_t>(s.x[0])) ? io_return_success : status(io_return_bad_argument);
    });
    failure(iokit, "_IOCancelPowerChange", io_return_unsupported);   // a host suspend cannot be vetoed
    // IOServiceClose of a root port drops its receive right. The registry
    // (iokit_registry.cpp) never hands out a connection, so any other
    // argument is invalid.
    bridge->function(iokit, "_IOServiceClose", [this](CpuState& s, GuestMemory&) {
        bool root = false;
        {
            std::lock_guard lock(mutex);
            root = power_roots.erase(static_cast<std::uint32_t>(s.x[0])) != 0;
        }
        if (root) { drop_receive_right(static_cast<std::uint32_t>(s.x[0])); s.x[0] = io_return_success; return; }
        s.x[0] = status(io_return_bad_argument);
    });
    failure(iokit, "_IOPMCopyCPUPowerStatus", io_return_not_found, {0});
    // IOPMAssertionCreateWithName(type, level, name, IOPMAssertionID *)
    raw(iokit, "_IOPMAssertionCreateWithName", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[3]) { s.x[0] = status(io_return_bad_argument); return; }
        s.x[0] = create_assertion(cf.utf8(s.x[0]), static_cast<std::uint32_t>(s.x[1]), s.x[2] ? cf.utf8(s.x[2]) : std::string("Alchemy"), s.x[3], m);
    });
    // IOPMAssertionCreateWithDescription(type, name, details, humanReadableReason,
    //     localizationBundlePath, timeout (d0), timeoutAction, IOPMAssertionID * (x6)): an
    // assertion at kIOPMAssertionLevelOn; a timeout needs a release timer and
    // fails explicitly.
    raw(iokit, "_IOPMAssertionCreateWithDescription", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1] || !s.x[6]) { s.x[0] = status(io_return_bad_argument); return; }
        if (std::bit_cast<double>(s.v[0][0]) > 0)
            throw std::runtime_error("unsupported: IOPMAssertionCreateWithDescription with a timeout");
        s.x[0] = create_assertion(cf.utf8(s.x[0]), 255, cf.utf8(s.x[1]), s.x[6], m);
    });
    // IOReturn IOPMCopyAssertionsByProcess(CFDictionaryRef *): {pid: [assertion]}.
    raw(iokit, "_IOPMCopyAssertionsByProcess", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = status(io_return_bad_argument); return; }
        std::vector<AssertionInfo> list;
        { std::lock_guard lock(mutex); for (const auto& [id, info] : assertion_info) { static_cast<void>(id); list.push_back(info); } }
        std::vector<Address> owned, entries;
        for (const auto& info : list) {
            const std::array<std::pair<const char*, Address>, 3> fields{{
                {"AssertType", cf.string(info.type)}, {"AssertName", cf.string(info.name)}, {"AssertLevel", cf.integer(info.level, 4)}}};
            std::vector<std::pair<Address, Address>> pairs;
            for (const auto& [key, value] : fields) {
                owned.push_back(cf.string(key));
                owned.push_back(value);
                pairs.emplace_back(owned[owned.size() - 2], value);
            }
            entries.push_back(cf.dictionary(pairs));
            owned.push_back(entries.back());
        }
        Address result;
        if (entries.empty()) result = cf.dictionary({});
        else {
            const auto pid = cf.integer(static_cast<std::int64_t>(call(libsystem, "_getpid", {})), 4);
            const auto assertions_array = cf.array(entries);
            const std::array<std::pair<Address, Address>, 1> top{{{pid, assertions_array}}};
            result = cf.dictionary(top);
            owned.push_back(pid);
            owned.push_back(assertions_array);
        }
        for (const auto value : owned) process.objects().release(value);
        m.write64(s.x[0], result);
        s.x[0] = io_return_success;
    });
    // boolean_t IOObjectIsEqualTo(io_object_t, io_object_t): handles name objects.
    raw(iokit, "_IOObjectIsEqualTo", [](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint32_t>(s.x[0]) == static_cast<std::uint32_t>(s.x[1]);
    });
    raw(iokit, "_IOPMAssertionRelease", [this](CpuState& s, GuestMemory&) {
        alchemy::host::system::PowerRequest request;
        {
            std::lock_guard lock(mutex);
            const auto found = assertions.find(static_cast<std::uint32_t>(s.x[0]));
            if (found == assertions.end()) { s.x[0] = status(io_return_bad_argument); return; }
            request = std::move(found->second);
            assertions.erase(found);
            assertion_info.erase(static_cast<std::uint32_t>(s.x[0]));
        }
        request.release();
        s.x[0] = io_return_success;
    });
    // Power sources: the blob is a CFArray of source description dictionaries;
    // each description is its own power-source reference.
    raw(iokit, "_IOPSCopyPowerSourcesInfo", [this](CpuState& s, GuestMemory&) {
        std::vector<Address> sources;
        if (const auto power = alchemy::host::system::power_status(); power && power->battery) {
            std::vector<std::pair<Address, Address>> entries;
            std::vector<Address> owned;
            const auto add = [&](const char* key, Address value) {
                const auto name = cf.string(key);
                owned.push_back(name);
                owned.push_back(value);
                entries.emplace_back(name, value);
            };
            add("Name", cf.string("InternalBattery-0"));
            add("Type", cf.string("InternalBattery"));
            add("Transport Type", cf.string("Internal"));
            add("Power Source State", cf.string(power->external_power ? "AC Power" : "Battery Power"));
            add("Current Capacity", cf.integer(power->percent.value_or(0)));
            add("Max Capacity", cf.integer(100));
            add("Is Present", retain(cf.boolean(true)));
            add("Is Charging", retain(cf.boolean(power->charging)));
            add("Time to Empty", cf.integer(power->seconds_to_empty ? static_cast<std::int64_t>(*power->seconds_to_empty / 60) : -1));
            add("Time to Full Charge", cf.integer(-1));
            sources.push_back(cf.dictionary(entries));
            for (const auto object : owned) release(object);
        }
        s.x[0] = cf.array(sources);
        for (const auto source : sources) release(source);
    });
    raw(iokit, "_IOPSCopyPowerSourcesList", [this](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? cf.copy(s.x[0]) : 0; });
    raw(iokit, "_IOPSGetPowerSourceDescription", [this](CpuState& s, GuestMemory&) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
        const auto values = cf.array_values(s.x[0]);
        s.x[0] = std::find(values.begin(), values.end(), s.x[1]) != values.end() ? s.x[1] : 0;
    });
    // CFRunLoopSourceRef IOPSNotificationCreateRunLoopSource(IOPowerSourceCallbackType, void *context):
    // a version 0 source (info = a client id) whose perform calls callback(context).
    const auto schedule = process.install_function("__alchemy_iops_schedule", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);   // schedule(info, CFRunLoopRef, mode)
        if (const auto found = power_source_clients.find(s.x[0]); found != power_source_clients.end())
            if (std::find(found->second.loops.begin(), found->second.loops.end(), s.x[1]) == found->second.loops.end()) found->second.loops.push_back(s.x[1]);
    }, "alchemy.internal");
    const auto cancel = process.install_function("__alchemy_iops_cancel", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        if (const auto found = power_source_clients.find(s.x[0]); found != power_source_clients.end()) {
            const auto at = std::find(found->second.loops.begin(), found->second.loops.end(), s.x[1]);
            if (at != found->second.loops.end()) found->second.loops.erase(at);
        }
    }, "alchemy.internal");
    const auto perform = process.install_function("__alchemy_iops_perform", [this](CpuState& s, GuestMemory&) {
        Address callback{}, context{};
        {
            std::lock_guard lock(mutex);
            const auto found = power_source_clients.find(s.x[0]);
            if (found == power_source_clients.end()) return;
            callback = found->second.callback; context = found->second.context;
        }
        if (callback) static_cast<void>(bridge->call_address(callback, std::array<Address, 1>{context}));
    }, "alchemy.internal");
    const auto forget = process.install_function("__alchemy_iops_release", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);   // the source's release(info): it is gone
        power_source_clients.erase(s.x[0]);
    }, "alchemy.internal");
    // Test hook: a host power-source change.
    static_cast<void>(process.install_function("__alchemy_iops_inject", [](CpuState&, GuestMemory&) { ++host_power_source_generation(); },
                                               "alchemy.internal"));
    raw(iokit, "_IOPSNotificationCreateRunLoopSource", [this, schedule, cancel, perform, forget](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = 0; return; }
        Address id{};
        {
            std::lock_guard lock(mutex);
            id = next_power_source++;
            power_source_clients[id] = {s.x[0], s.x[1], 0, {}};
        }
        // CFRunLoopSourceContext: version, info, retain, release, copyDescription, equal, hash, schedule, cancel, perform.
        const auto context = process.allocate_guest(80, 16);
        m.fill_bytes(context, 0, 80);
        m.write64(context + 8, id);
        m.write64(context + 24, forget);
        m.write64(context + 56, schedule);
        m.write64(context + 64, cancel);
        m.write64(context + 72, perform);
        const auto source = call(core_foundation, "_CFRunLoopSourceCreate", {0, 0, context});
        process.release_guest(context);
        if (!source) {
            std::lock_guard lock(mutex);
            power_source_clients.erase(id);
            s.x[0] = 0;
            return;
        }
        bool start{};
        {
            std::lock_guard lock(mutex);
            if (const auto found = power_source_clients.find(id); found != power_source_clients.end()) found->second.source = source;
            start = !std::exchange(power_source_watcher, true);
        }
        static_cast<void>(register_host_power_sources());   // without it, only the test hook changes anything
        if (start) {
            const auto worker = call(libsystem, "_dispatch_get_global_queue", {0, 0});
            bridge->dispatch_async(worker, [this] { watch_power_sources(); });
        }
        s.x[0] = source;
    });
}

void IoKit::install_force_feedback() {
    failure(force_feedback, "_FFIsForceFeedback", fferr_no_interface);
    failure(force_feedback, "_FFCreateDevice", fferr_no_interface, {1});
    failure(force_feedback, "_FFDeviceCreateEffect", fferr_invalid_param, {3});
    for (const auto* name : {"_FFDeviceGetForceFeedbackCapabilities", "_FFDeviceReleaseEffect", "_FFEffectSetParameters", "_FFEffectStart",
                             "_FFEffectStop", "_FFReleaseDevice"})
        failure(force_feedback, name, fferr_invalid_param);
}

void IoKit::install_disk_arbitration() {
    // The sessions, callbacks and disk copies are the
    // DiskArbitration provider's (darwin/diskarbitration/sessions.cpp,
    // installed before this fill-only set); these are the names it leaves.
    // kDADiskDescriptionMatchVolumeMountable = {DAVolumeMountable: true}; kDADiskDescriptionWatchVolumePath = [DAVolumePath].
    if (!has(disk_arbitration, "_kDADiskDescriptionMatchVolumeMountable")) {
        const auto key = cf.constant_string("DAVolumeMountable");
        const auto dictionary = cf.dictionary(std::vector<std::pair<Address, Address>>{{key, cf.boolean(true)}});
        if (const auto record = process.objects().find(dictionary)) record->immortal = true;
        value(disk_arbitration, "_kDADiskDescriptionMatchVolumeMountable", dictionary);
    }
    if (!has(disk_arbitration, "_kDADiskDescriptionWatchVolumePath")) {
        const auto array = cf.array(std::vector<Address>{cf.constant_string("DAVolumePath")});
        if (const auto record = process.objects().find(array)) record->immortal = true;
        value(disk_arbitration, "_kDADiskDescriptionWatchVolumePath", array);
    }
    for (const auto* name : {"_DADiskEject", "_DADiskUnmount"}) {
        const std::string what(name + 1);
        raw(disk_arbitration, name, [what](CpuState& s, GuestMemory&) { impossible(s.x[0], what); });
    }
}

} // namespace

void install_iokit_devices(InstallContext& context) {
    const auto provider = std::make_shared<IoKit>(context);
    provider->install_hid();
    provider->install_notifications();
    provider->install_power();
    provider->install_force_feedback();
    provider->install_disk_arbitration();
}

} // namespace alchemy::gui::fill
