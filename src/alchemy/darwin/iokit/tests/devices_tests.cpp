// devices provider tests: the Qt WebEngine device imports resolve; HID
// managers are real and see no devices; notification ports and matching
// notifications follow the static registry; power assertions are real host
// power requests; power sources reflect the host; Bluetooth, controllers,
// Wi-Fi, location and LocalAuthentication report "none"/"unsupported".
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/engine_adapter.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace {
using namespace fill_test;

#define FRAMEWORK(name) "/System/Library/Frameworks/" name ".framework/Versions/A/" name
constexpr std::string_view iokit = FRAMEWORK("IOKit"), ff = FRAMEWORK("ForceFeedback"), da = FRAMEWORK("DiskArbitration"),
    iob = FRAMEWORK("IOBluetooth"), cb = FRAMEWORK("CoreBluetooth"), gc = FRAMEWORK("GameController"), wlan = FRAMEWORK("CoreWLAN"),
    cl = FRAMEWORK("CoreLocation"), la = FRAMEWORK("LocalAuthentication"), cf = FRAMEWORK("CoreFoundation");
#undef FRAMEWORK
constexpr std::string_view sys_lib = "/usr/lib/libSystem.B.dylib";

void imports(Fixture& f) { imports_resolve(f, {iokit, ff, da, iob, cb, gc, wlan, cl, la}); }

void hid(Fixture& f) {
    const auto manager = f.call(iokit, "_IOHIDManagerCreate", {0, 0});
    require(manager && f.call(cf, "_CFGetTypeID", {manager}) == f.call(iokit, "_IOHIDManagerGetTypeID"), "IOHIDManager");
    static_cast<void>(f.call(iokit, "_IOHIDManagerSetDeviceMatchingMultiple", {manager, f.cf.array({})}));
    static_cast<void>(f.call(iokit, "_IOHIDManagerRegisterDeviceMatchingCallback", {manager, 0x1000, 0}));
    require(f.status(iokit, "_IOHIDManagerOpen", {manager, 0}) == 0 && f.status(iokit, "_IOHIDManagerClose", {manager, 0}) == 0, "open/close");
    require(f.call(iokit, "_IOHIDDeviceCreate", {0, 0x2c03}) == 0, "no HID devices");
    require(f.call(iokit, "_IOHIDValueGetIntegerValue", {0}) == 0, "NULL value");
    bool refused = false;
    try { static_cast<void>(f.call(iokit, "_IOHIDElementGetUsage", {0x1234})); } catch (const std::exception&) { refused = true; }
    require(refused, "HID elements cannot exist");
    // The HID event-system client is unavailable: no client, services or events.
    require(f.call(iokit, "_IOHIDEventSystemClientCreate", {0}) == 0, "no HID event-system client");
    require(f.call(iokit, "_IOHIDEventSystemClientCopyServices", {0}) == 0 && f.call(iokit, "_IOHIDServiceClientCopyEvent", {0, 15, 0, 0}) == 0,
            "NULL client and service answer empty");
    refused = false;
    try { static_cast<void>(f.call(iokit, "_IOHIDServiceClientCopyProperty", {0x1234, 0})); } catch (const std::exception&) { refused = true; }
    require(refused, "HID event-system services cannot exist");
    const auto plugin = f.block(8, 0xff);
    require(f.status(iokit, "_IOCreatePlugInInterfaceForService", {0, 0, 0, plugin, f.block(4)}) == static_cast<std::int32_t>(0xe00002c0U) &&
            f.engine.read64(plugin) == 0, "no plug-in interfaces");
}

void notifications(Fixture& f) {
    const auto port = f.call(iokit, "_IONotificationPortCreate", {0});
    require(port != 0, "IONotificationPortCreate");
    const auto source = f.call(iokit, "_IONotificationPortGetRunLoopSource", {port});
    require(source && source == f.call(iokit, "_IONotificationPortGetRunLoopSource", {port}), "run-loop source");
    // Matching notifications start with the current matches of the registry.
    const auto iterator = f.block(4);
    const auto matching = f.call(iokit, "_IOServiceMatching", {f.text("IOPlatformExpertDevice")});
    require(f.status(iokit, "_IOServiceAddMatchingNotification", {port, f.text("IOServiceFirstMatch"), matching, 0x1000, 0, iterator}) == 0, "first match");
    const auto first = static_cast<std::uint32_t>(f.call(iokit, "_IOIteratorNext", {f.engine.read32(iterator)}));
    require(first != 0, "platform expert matched");
    require(f.call(iokit, "_IOIteratorNext", {f.engine.read32(iterator)}) == 0, "one match");
    const auto terminated = f.block(4);
    require(f.status(iokit, "_IOServiceAddMatchingNotification", {port, f.text("IOServiceTerminate"),
             f.call(iokit, "_IOServiceMatching", {f.text("IOService")}), 0x1000, 0, terminated}) == 0 &&
            f.call(iokit, "_IOIteratorNext", {f.engine.read32(terminated)}) == 0, "nothing terminates");
    const auto interest = f.block(4);
    require(f.status(iokit, "_IOServiceAddInterestNotification", {port, first, f.text("IOGeneralInterest"), 0x1000, 0, interest}) == 0, "interest");
    // Registry entry queries.
    const auto name = f.block(128);
    require(f.status(iokit, "_IORegistryEntryGetName", {first, name}) == 0 && f.read_text(name) == "IOPlatformExpertDevice", "entry name");
    const auto properties = f.block(8);
    require(f.status(iokit, "_IORegistryEntryCreateCFProperties", {first, properties, 0, 0}) == 0, "properties");
    require(f.cf.dictionary_value(f.engine.read64(properties), f.cf.string("IOPlatformUUID")) != 0, "IOPlatformUUID property");
    const auto id = f.block(8);
    require(f.status(iokit, "_IORegistryEntryGetRegistryEntryID", {first, id}) == 0 && f.engine.read64(id) != 0, "entry id");
    static_cast<void>(f.call(iokit, "_IONotificationPortDestroy", {port}));
}

void power(Fixture& f) {
    const auto assertion = f.block(4);
    require(f.status(iokit, "_IOPMAssertionCreateWithName", {f.cf.string("PreventUserIdleSystemSleep"), 255, f.cf.string("alchemy test"), assertion}) == 0,
            "system sleep assertion");
    require(f.status(iokit, "_IOPMAssertionRelease", {f.engine.read32(assertion)}) == 0, "release");
    require(f.status(iokit, "_IOPMAssertionRelease", {f.engine.read32(assertion)}) == static_cast<std::int32_t>(0xe00002c2U), "double release");
    require(f.status(iokit, "_IOPMAssertionCreateWithName", {f.cf.string("UnknownAssertionType"), 255, f.cf.string("x"), assertion}) ==
            static_cast<std::int32_t>(0xe00002c7U), "unknown assertion type");
    const auto info = f.call(iokit, "_IOPSCopyPowerSourcesInfo");
    const auto list = f.call(iokit, "_IOPSCopyPowerSourcesList", {info});
    require(info && list, "power sources");
    for (std::uint64_t i = 0; i < f.cf.length(list); ++i) {
        const auto description = f.call(iokit, "_IOPSGetPowerSourceDescription", {info, f.cf.element(list, i)});
        require(description && f.cf.utf8(f.cf.dictionary_value(description, f.cf.string("Type"))) == "InternalBattery", "battery description");
    }
    // Power-source notifications: a change (the test hook) runs the callback
    // through the source on the run loop it joined.
    static std::atomic<int> changes{};
    static std::atomic<Address> seen_context{};
    const auto changed = f.process.install_function("__devices_test_power_source", [](transmute::CpuState& s, transmute::GuestMemory&) {
        seen_context = s.x[0];
        ++changes;
    }, "devices.test");
    require(f.call(iokit, "_IOPSNotificationCreateRunLoopSource", {0, 0}) == 0, "a power-source source needs a callback");
    const auto source = f.call(iokit, "_IOPSNotificationCreateRunLoopSource", {changed, 0xc0ffee});
    require(source != 0, "IOPSNotificationCreateRunLoopSource");
    const auto loop = f.call(cf, "_CFRunLoopGetCurrent");
    const auto mode = f.engine.read64(f.symbol(cf, "_kCFRunLoopDefaultMode"));
    static_cast<void>(f.call(cf, "_CFRunLoopAddSource", {loop, source, mode}));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // a registration-time host report may arrive
    static_cast<void>(f.call(cf, "_CFRunLoopRunInMode", {mode, 0, 1}));
    const auto before = changes.load();
    const auto inject_source = f.process.resolve("alchemy.internal", "__alchemy_iops_inject")->address;
    static_cast<void>(f.engine.invoke_callback(inject_source, std::array<Address, 0>{}));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (changes.load() == before) {
        require(std::chrono::steady_clock::now() < until, "power-source change delivered");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        static_cast<void>(f.call(cf, "_CFRunLoopRunInMode", {mode, 0, 1}));
    }
    require(seen_context == 0xc0ffee, "power-source callback context");
    static_cast<void>(f.call(cf, "_CFRunLoopRemoveSource", {loop, source, mode}));
    static_cast<void>(f.call(cf, "_CFRunLoopSourceInvalidate", {source}));
    static_cast<void>(f.call(cf, "_CFRelease", {source}));
    // IORegisterForSystemPower without a callback fails (system_power covers the real path).
    const auto notifier = f.block(4, 0xff), port = f.block(8, 0xff);
    require(f.call(iokit, "_IORegisterForSystemPower", {0, port, 0, notifier}) == 0 && f.engine.read32(notifier) == 0, "no callback, no registration");
}

void other_devices(Fixture& f) {
    require(f.status(ff, "_FFIsForceFeedback", {0x2c03}) == static_cast<std::int32_t>(0x80004002U), "no force feedback");
    // Disk Arbitration: completion kit's local arbiter (installed before this
    // fill-only provider) gives sessions; a path that is not a volume has no disk.
    const auto session = f.call(da, "_DASessionCreate", {0});
    require(session != 0, "Disk Arbitration session");
    const auto not_a_volume = f.cf.file_url("/tmp/not-a-volume", true);
    require(f.call(da, "_DADiskCreateFromVolumePath", {0, session, not_a_volume}) == 0, "no disk for a non-volume path");
    require(f.string_constant(da, "_kDADiskDescriptionVolumePathKey") == "DAVolumePath", "DA key");
    const auto watch = f.engine.read64(f.symbol(da, "_kDADiskDescriptionWatchVolumePath"));
    require(f.cf.length(watch) == 1 && f.cf.utf8(f.cf.element(watch, 0)) == "DAVolumePath", "kDADiskDescriptionWatchVolumePath");
    require(f.send(f.cls("IOBluetoothHostController"), "defaultController") == 0, "no Bluetooth controller");
    require(f.cf.length(f.send(f.cls("IOBluetoothDevice"), "pairedDevices")) == 0, "no paired devices");
    const auto central = f.send(f.send(f.cls("CBCentralManager"), "alloc"), "initWithDelegate:queue:", {0, 0});
    require(central && f.send(central, "state") == 2, "CBManagerStateUnsupported");
    const auto uuid = f.send(f.cls("CBUUID"), "UUIDWithString:", {f.cf.string("0000180f-0000-1000-8000-00805f9b34fb")});
    require(f.cf.utf8(f.send(uuid, "UUIDString")) == "180F", "Bluetooth base UUID shortens");
    const auto full = f.send(f.cls("CBUUID"), "UUIDWithString:", {f.cf.string("12345678-9abc-def0-1234-56789abcdef0")});
    require(f.cf.utf8(f.send(full, "UUIDString")) == "12345678-9ABC-DEF0-1234-56789ABCDEF0", "128-bit UUID");
    require(f.send(uuid, "isEqual:", {f.send(f.cls("CBUUID"), "UUIDWithString:", {f.cf.string("180F")})}) == 1, "CBUUID equality");
    require(f.cf.length(f.send(f.cls("GCController"), "controllers")) == 0, "no controllers");
    require(f.send(f.send(f.cls("CWWiFiClient"), "sharedWiFiClient"), "interface") == 0, "no Wi-Fi interface");
    require(f.send(f.cls("CLLocationManager"), "locationServicesEnabled") == 0, "location services disabled");
    require(std::bit_cast<double>(f.engine.read64(f.symbol(cl, "_kCLLocationAccuracyHundredMeters"))) == 100.0, "kCLLocationAccuracyHundredMeters");
    const auto context = f.send(f.send(f.cls("LAContext"), "alloc"), "init");
    const auto error = f.block(8);
    require(f.send(context, "canEvaluatePolicy:error:", {1, error}) == 0, "no biometry");
    require(static_cast<std::int64_t>(f.call(cf, "_CFErrorGetCode", {f.engine.read64(error)})) == -6, "LAErrorBiometryNotAvailable");
}


// A call from a host callback on a guest worker thread, through that thread's engine.
Address here(Fixture& f, std::string_view library, std::string_view name, std::initializer_list<Address> arguments) {
    return alchemy::EngineAdapter(f.process.calling_engine()).call(f.symbol(library, name), std::span(arguments.begin(), arguments.size()));
}
template <class Done> void wait_until(Done done, std::string_view what) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
        require(std::chrono::steady_clock::now() < until, what);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// The registry's traversal API over the static tree.
void registry_walk(Fixture& f) {
    const auto root = static_cast<std::uint32_t>(f.call(iokit, "_IORegistryGetRootEntry", {0}));
    require(root != 0, "IORegistryGetRootEntry");
    const auto name = f.block(512);
    require(f.status(iokit, "_IOObjectGetClass", {root, name}) == 0 && f.read_text(name) == "IOPlatformExpertDevice", "IOObjectGetClass");
    require(f.cf.utf8(f.call(iokit, "_IOObjectCopySuperclassForClass", {f.cf.string("IOPlatformExpertDevice")})) == "IOService", "superclass");
    require(f.status(iokit, "_IORegistryEntryGetPath", {root, f.text("IOService"), name}) == 0 && f.read_text(name) == "IOService:/", "root path");
    // A child's path names it, and the path finds it again.
    const auto child = f.block(4);
    require(f.status(iokit, "_IORegistryEntryGetChildEntry", {root, f.text("IOService"), child}) == 0, "IORegistryEntryGetChildEntry");
    const auto first = f.engine.read32(child);
    require(f.status(iokit, "_IORegistryEntryGetPath", {first, f.text("IOService"), name}) == 0, "child path");
    const auto path = f.read_text(name);
    require(path.starts_with("IOService:/") && path.size() > 11, "child path text");
    const auto again = static_cast<std::uint32_t>(f.call(iokit, "_IORegistryEntryFromPath", {0, f.text(path)}));
    require(again != 0 && f.call(iokit, "_IOObjectIsEqualTo", {again, first}) == 0, "a path finds its entry (a new handle)");
    const auto id_a = f.block(8), id_b = f.block(8);
    require(f.status(iokit, "_IORegistryEntryGetRegistryEntryID", {again, id_a}) == 0 && f.status(iokit, "_IORegistryEntryGetRegistryEntryID", {first, id_b}) == 0 &&
            f.engine.read64(id_a) == f.engine.read64(id_b), "the same entry");
    // Child and parent iterators.
    const auto iterator = f.block(4);
    require(f.status(iokit, "_IORegistryEntryGetChildIterator", {root, f.text("IOService"), iterator}) == 0 &&
            f.call(iokit, "_IOIteratorIsValid", {f.engine.read32(iterator)}) == 1, "child iterator");
    std::size_t children = 0;
    for (Address entry; (entry = f.call(iokit, "_IOIteratorNext", {f.engine.read32(iterator)})) != 0; ++children)
        static_cast<void>(f.call(iokit, "_IOObjectRelease", {entry}));
    require(children >= 1, "the root has children");
    require(f.status(iokit, "_IORegistryEntryGetParentIterator", {first, f.text("IOService"), iterator}) == 0, "parent iterator");
    const auto parent = static_cast<std::uint32_t>(f.call(iokit, "_IOIteratorNext", {f.engine.read32(iterator)}));
    require(f.status(iokit, "_IOObjectGetClass", {parent, name}) == 0 && f.read_text(name) == "IOPlatformExpertDevice", "the parent is the root");
    // IORegistryCreateIterator: the root, then EnterEntry walks its children, ExitEntry returns.
    require(f.status(iokit, "_IORegistryCreateIterator", {0, f.text("IOService"), 0, iterator}) == 0, "IORegistryCreateIterator");
    const auto walker = f.engine.read32(iterator);
    require(f.call(iokit, "_IOIteratorNext", {walker}) != 0 && f.call(iokit, "_IOIteratorNext", {walker}) == 0, "only the root at the top level");
    require(f.status(iokit, "_IORegistryIteratorEnterEntry", {walker}) == static_cast<std::int32_t>(0xe00002c0U), "nothing current to enter");
    static_cast<void>(f.call(iokit, "_IOIteratorReset", {walker}));
    static_cast<void>(f.call(iokit, "_IOIteratorNext", {walker}));
    require(f.status(iokit, "_IORegistryIteratorEnterEntry", {walker}) == 0, "enter the root");
    std::size_t entered = 0;
    while (f.call(iokit, "_IOIteratorNext", {walker}) != 0) ++entered;
    require(entered == children && f.status(iokit, "_IORegistryIteratorExitEntry", {walker}) == 0 &&
            f.status(iokit, "_IORegistryIteratorExitEntry", {walker}) == static_cast<std::int32_t>(0xe00002c0U), "enter/exit");
    // Matching one entry, busy state, the main port, setting properties.
    const auto matches = f.block(4);
    require(f.status(iokit, "_IOServiceMatchPropertyTable", {root, f.call(iokit, "_IOServiceMatching", {f.text("IOService")}), matches}) == 0 &&
            f.engine.read32(matches) == 1, "IOServiceMatchPropertyTable");
    const auto busy = f.block(4, 0xff);
    require(f.status(iokit, "_IOServiceGetBusyState", {root, busy}) == 0 && f.engine.read32(busy) == 0 && f.status(iokit, "_IOKitWaitQuiet", {0, 0}) == 0, "never busy");
    const auto port = f.block(4, 0xff);
    require(f.status(iokit, "_IOMainPort", {0, port}) == 0 && f.engine.read32(port) == 0, "IOMainPort is MACH_PORT_NULL");
    require(f.status(iokit, "_IORegistryEntrySetCFProperty", {root, f.cf.string("x"), f.cf.string("y")}) == static_cast<std::int32_t>(0xe00002c7U),
            "properties cannot be set");
    require(f.call(iokit, "_IOObjectGetRetainCount", {root}) == 1 && f.status(iokit, "_IOObjectRetain", {root}) == 0 &&
            f.call(iokit, "_IOObjectGetRetainCount", {root}) == 2, "retain counts");
}

// HID managers beyond matching: properties, no devices, the dispatch-queue model.
void hid_manager(Fixture& f) {
    const auto manager = f.call(iokit, "_IOHIDManagerCreate", {0, 0});
    require(f.call(iokit, "_IOHIDManagerCopyDevices", {manager}) == 0, "no devices");
    static_cast<void>(f.call(iokit, "_IOHIDManagerSetDeviceMatching", {manager, f.cf.dictionary(std::vector<std::pair<Address, Address>>{})}));
    const auto key = f.cf.string("AlchemyKey"), value = f.cf.string("v");
    require(f.call(iokit, "_IOHIDManagerSetProperty", {manager, key, value}) == 1 && f.call(iokit, "_IOHIDManagerGetProperty", {manager, key}) == value,
            "manager properties");
    require(f.call(iokit, "_IOHIDDeviceGetTypeID") != f.call(iokit, "_IOHIDManagerGetTypeID"), "distinct HID type IDs");
    static std::atomic<int> cancelled{};
    const auto handler = f.process.install_function("__devices_test_hid_cancel", [](transmute::CpuState& s, transmute::GuestMemory&) { ++cancelled; s.x[0] = 0; },
                                                    "devices.test");
    const auto block = f.block(32);
    f.engine.write64(block, f.symbol(sys_lib, "__NSConcreteGlobalBlock"));
    f.engine.write32(block + 8, 1U << 28);
    f.engine.write64(block + 16, handler);
    const auto descriptor = f.block(16);
    f.engine.write64(descriptor + 8, 32);
    f.engine.write64(block + 24, descriptor);
    const auto queue = f.call(sys_lib, "_dispatch_queue_create", {f.text("alchemy.test.hid"), 0});
    static_cast<void>(f.call(iokit, "_IOHIDManagerSetDispatchQueue", {manager, queue}));
    static_cast<void>(f.call(iokit, "_IOHIDManagerSetCancelHandler", {manager, block}));
    static_cast<void>(f.call(iokit, "_IOHIDManagerActivate", {manager}));
    static_cast<void>(f.call(iokit, "_IOHIDManagerCancel", {manager}));
    wait_until([] { return cancelled.load() == 1; }, "the cancel handler runs on the manager's queue");
    bool refused = false;
    try { static_cast<void>(f.call(iokit, "_IOHIDElementGetCookie", {0x1234})); } catch (const std::exception&) { refused = true; }
    require(refused, "HID elements cannot exist");
}

// Assertions from a properties dictionary, their properties, status and counts.
void power_properties(Fixture& f) {
    const std::vector<std::pair<Address, Address>> fields{{f.cf.string("AssertType"), f.cf.string("PreventUserIdleSystemSleep")},
                                                          {f.cf.string("AssertName"), f.cf.string("alchemy properties")}};
    const auto properties = f.cf.dictionary(fields);
    const auto assertion = f.block(4);
    require(f.status(iokit, "_IOPMAssertionCreateWithProperties", {properties, assertion}) == 0, "IOPMAssertionCreateWithProperties");
    const auto id = f.engine.read32(assertion);
    const auto copied = f.call(iokit, "_IOPMAssertionCopyProperties", {id});
    require(f.cf.utf8(f.cf.dictionary_value(copied, f.cf.string("AssertName"))) == "alchemy properties", "IOPMAssertionCopyProperties");
    const auto status_out = f.block(8);
    require(f.status(iokit, "_IOPMCopyAssertionsStatus", {status_out}) == 0, "IOPMCopyAssertionsStatus");
    const auto level = f.cf.dictionary_value(f.engine.read64(status_out), f.cf.string("PreventUserIdleSystemSleep"));
    require(level && f.cf.equal(level, f.cf.integer(1)), "the type is held");
    require(f.status(iokit, "_IOPMAssertionSetProperty", {id, f.cf.string("AssertLevel"), f.cf.integer(0)}) == 0, "level off");
    require(f.status(iokit, "_IOPMCopyAssertionsStatus", {status_out}) == 0 &&
            f.cf.equal(f.cf.dictionary_value(f.engine.read64(status_out), f.cf.string("PreventUserIdleSystemSleep")), f.cf.integer(0)), "no longer held");
    require(f.status(iokit, "_IOPMAssertionRetain", {id}) == 0 && f.status(iokit, "_IOPMAssertionRelease", {id}) == 0 &&
            f.call(iokit, "_IOPMAssertionCopyProperties", {id}) != 0, "a retained assertion survives one release");
    require(f.status(iokit, "_IOPMAssertionRelease", {id}) == 0 && f.call(iokit, "_IOPMAssertionCopyProperties", {id}) == 0, "the last release ends it");
    const auto type = f.cf.utf8(f.call(iokit, "_IOPSGetProvidingPowerSourceType", {0}));
    require(type == "AC Power" || type == "Battery Power", "IOPSGetProvidingPowerSourceType");
}

// Disk Arbitration: scheduled sessions announce the volumes, then idle; requests are answered.
std::atomic<int> appeared{}, idled{};
std::atomic<std::uint32_t> unmount_status{};
void disk_arbitration(Fixture& f) {
    const auto on_appeared = f.process.install_function("__devices_test_da_appeared", [](transmute::CpuState& s, transmute::GuestMemory&) {
        if (s.x[0]) ++appeared;
        s.x[0] = 0;
    }, "devices.test");
    const auto on_idle = f.process.install_function("__devices_test_da_idle", [](transmute::CpuState& s, transmute::GuestMemory&) { ++idled; s.x[0] = 0; },
                                                    "devices.test");
    static Fixture* fixture = &f;
    const auto on_unmount = f.process.install_function("__devices_test_da_unmount", [](transmute::CpuState& s, transmute::GuestMemory&) {
        unmount_status = static_cast<std::uint32_t>(here(*fixture, da, "_DADissenterGetStatus", {s.x[1]}));
        s.x[0] = 0;
    }, "devices.test");
    const auto session = f.call(da, "_DASessionCreate", {0});
    require(f.call(cf, "_CFGetTypeID", {session}) == f.call(da, "_DASessionGetTypeID"), "CFGetTypeID of a session");
    static_cast<void>(f.call(da, "_DARegisterDiskAppearedCallback", {session, f.engine.read64(f.symbol(da, "_kDADiskDescriptionMatchVolumeMountable")),
                                                                     on_appeared, 0}));
    static_cast<void>(f.call(da, "_DARegisterIdleCallback", {session, on_idle, 0}));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(appeared == 0 && idled == 0, "nothing before the session is scheduled");
    const auto queue = f.call(sys_lib, "_dispatch_queue_create", {f.text("alchemy.test.da"), 0});
    static_cast<void>(f.call(da, "_DASessionSetDispatchQueue", {session, queue}));
    wait_until([] { return idled.load() == 1; }, "the session idles after announcing the volumes");
    require(appeared >= 1, "the root volume appears");
    // A volume path gives its volume's disk (without an Alchemy root, "/" names the host drive's
    // /Volumes/<letter>); disk1s1 is always the root volume.
    const auto volume = f.call(da, "_DADiskCreateFromVolumePath", {0, session, f.cf.file_url("/", true)});
    require(volume != 0 && f.read_text(f.call(da, "_DADiskGetBSDName", {volume})).starts_with("disk1s"), "a disk for a volume path");
    const auto disk = f.call(da, "_DADiskCreateFromBSDName", {0, session, f.text("disk1s1")});
    require(disk != 0, "disk1s1");
    const auto description = f.call(da, "_DADiskCopyDescription", {disk});
    require(f.cf.url_path(f.cf.dictionary_value(description, f.cf.string("DAVolumePath"))) == f.cf.url_path(f.cf.file_url("/", true)) &&
            f.cf.utf8(f.cf.dictionary_value(description, f.cf.string("DAVolumeName"))) == "Macintosh HD", "disk1s1 is the root volume");
    require(f.call(cf, "_CFGetTypeID", {disk}) == f.call(da, "_DADiskGetTypeID"), "CFGetTypeID of a disk");
    require(f.status(da, "_DADiskSetOptions", {disk, 1, 1}) == 0 && f.call(da, "_DADiskGetOptions", {disk}) == 1, "disk options");
    static_cast<void>(f.call(da, "_DADiskUnmount", {disk, 0, on_unmount, 0}));
    wait_until([] { return unmount_status.load() != 0; }, "the unmount callback");
    require(unmount_status == 0xf8da0008, "kDAReturnNotPermitted");
    require(f.call(da, "_DADiskCreateFromIOMedia", {0, session, 0}) == 0, "no IOKit media");
}

} // namespace

// IORegisterForSystemPower: real ports, host suspend/resume broadcasts
// (injected) delivered on the notification port's dispatch queue.
std::vector<std::pair<std::uint32_t, std::uint64_t>> power_messages;
std::mutex power_mutex;
void system_power(Fixture& f) {
    const auto callback = f.process.install_function("__devices_test_power", [](transmute::CpuState& s, transmute::GuestMemory&) {
        std::lock_guard lock(power_mutex);
        power_messages.emplace_back(static_cast<std::uint32_t>(s.x[2]), s.x[3]);
    }, "devices.test");
    const auto port_out = f.block(8), notifier = f.block(4);
    const auto root = static_cast<std::uint32_t>(f.call(iokit, "_IORegisterForSystemPower", {0x77, port_out, callback, notifier}));
    const auto port = f.engine.read64(port_out);
    require(root != 0 && port != 0 && f.engine.read32(notifier) != 0, "IORegisterForSystemPower");
    require(f.call(iokit, "_IONotificationPortGetRunLoopSource", {port}) != 0, "run-loop source");
    require(f.call(iokit, "_IONotificationPortGetMachPort", {port}) != 0, "Mach port");
    const auto label = f.text("alchemy.test.power");
    const auto queue = f.call(sys_lib, "_dispatch_queue_create", {label, 0});
    static_cast<void>(f.call(iokit, "_IONotificationPortSetDispatchQueue", {port, queue}));
    const auto inject = f.process.resolve("alchemy.internal", "__alchemy_io_power_inject")->address;
    const auto wait_for = [&](std::size_t count) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            { std::lock_guard lock(power_mutex); if (power_messages.size() >= count) return; }
            require(std::chrono::steady_clock::now() < until, "power messages delivered");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    const std::array<Address, 1> suspend{1}, resume{2};
    static_cast<void>(f.engine.invoke_callback(inject, suspend));
    wait_for(1);
    require(power_messages[0].first == 0xe0000280 && power_messages[0].second != 0, "kIOMessageSystemWillSleep");
    require(f.status(iokit, "_IOAllowPowerChange", {root, power_messages[0].second}) == 0, "IOAllowPowerChange");
    static_cast<void>(f.engine.invoke_callback(inject, resume));
    wait_for(3);
    require(power_messages[1].first == 0xe0000320 && power_messages[2].first == 0xe0000300, "WillPowerOn, HasPoweredOn");
    require(f.status(iokit, "_IODeregisterForSystemPower", {notifier}) == 0 && f.engine.read32(notifier) == 0, "deregister");
    require(f.status(iokit, "_IOServiceClose", {root}) == 0, "root port closed");
    static_cast<void>(f.call(iokit, "_IONotificationPortDestroy", {port}));
}

int main() {
    return run("devices", {{"imports", imports}, {"hid", hid}, {"notifications", notifications}, {"power", power}, {"system power", system_power}, {"other devices", other_devices},
                           {"registry walk", registry_walk}, {"hid manager", hid_manager}, {"power properties", power_properties},
                           {"disk arbitration", disk_arbitration}});
}
