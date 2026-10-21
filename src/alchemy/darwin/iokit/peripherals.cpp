#include "alchemy/darwin/iokit/devices.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <cctype>
#include <mutex>
#include <vector>

// Wireless, controller, location and authentication frameworks.
//
// - IOBluetooth: no Bluetooth controller exists (no default controller, no
//   paired devices); the channel and SDP classes are shells.
// - CoreBluetooth: managers report CBManagerStateUnsupported (delivered to
//   the delegate's ...DidUpdateState: on the manager's queue, as on a Mac
//   without Bluetooth LE) and authorization "restricted"; scanning and
//   advertising are ignored with Apple's "API MISUSE" notice. CBUUID is real.
// - GameController: no controllers; wireless discovery ends at once.
// - CoreWLAN: no Wi-Fi interface.
// - CoreLocation: location services are disabled; starting updates reports
//   kCLErrorDenied to the delegate.
// - LocalAuthentication: no biometry or device passcode can be evaluated.
namespace alchemy::gui::fill {
namespace {

struct Manager final : ObjectPayload { std::mutex mutex; Address delegate{}; };
struct Uuid final : ObjectPayload { std::vector<std::uint8_t> bytes; };
struct LocationManager final : ObjectPayload {
    std::mutex mutex;
    Address delegate{};
    double accuracy{-1.0}, distance{-1.0};   // kCLLocationAccuracyBest, kCLDistanceFilterNone
};

// The Bluetooth base UUID (0000xxxx-0000-1000-8000-00805F9B34FB).
constexpr std::array<std::uint8_t, 12> bluetooth_base{0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb};

std::optional<std::vector<std::uint8_t>> parse_uuid(const std::string& text) {
    std::string hex;
    for (const auto c : text) if (c != '-') hex.push_back(c);
    if (!(hex.size() == 4 || hex.size() == 8 || (hex.size() == 32 && text.size() == 36))) return std::nullopt;
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const auto value = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : std::isxdigit(static_cast<unsigned char>(c)) ? (std::tolower(c) - 'a' + 10) : -1; };
        const auto high = value(hex[i]), low = value(hex[i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        bytes.push_back(static_cast<std::uint8_t>(high * 16 + low));
    }
    // 128-bit UUIDs on the Bluetooth base shorten to their 16-bit alias.
    if (bytes.size() == 16 && bytes[0] == 0 && bytes[1] == 0 && std::equal(bluetooth_base.begin(), bluetooth_base.end(), bytes.begin() + 4))
        bytes = {bytes[2], bytes[3]};
    return bytes;
}
std::string format_uuid(const std::vector<std::uint8_t>& bytes) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string text;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes.size() == 16 && (i == 4 || i == 6 || i == 8 || i == 10)) text.push_back('-');
        text.push_back(digits[bytes[i] >> 4]);
        text.push_back(digits[bytes[i] & 15]);
    }
    return text;
}

class Peripherals final : public Provider {
public:
    using Provider::Provider;
    std::mutex mutex;
    Address wifi_client{};

    void install_bluetooth();
    void install_core_bluetooth();
    void install_misc();
};

void Peripherals::install_bluetooth() {
    unsupported(io_bluetooth, "_IOBluetoothPreferenceSetControllerPowerState", "no Bluetooth controller exists");
    static_cast<void>(cls(io_bluetooth, "IOBluetoothObject", "NSObject"));
    const auto device = cls(io_bluetooth, "IOBluetoothDevice", "IOBluetoothObject");
    method(device, "pairedDevices", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); }, true);
    method(device, "deviceWithAddressString:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true);
    const auto controller = cls(io_bluetooth, "IOBluetoothHostController", "NSObject");
    method(controller, "defaultController", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true);
    static_cast<void>(cls(io_bluetooth, "IOBluetoothDeviceInquiry", "NSObject"));
    static_cast<void>(cls(io_bluetooth, "IOBluetoothL2CAPChannel", "IOBluetoothObject"));
    static_cast<void>(cls(io_bluetooth, "IOBluetoothRFCOMMChannel", "IOBluetoothObject"));
    static_cast<void>(cls(io_bluetooth, "IOBluetoothSDPServiceRecord", "NSObject"));
    static_cast<void>(cls(io_bluetooth, "IOBluetoothSDPUUID", "NSData"));
}

void Peripherals::install_core_bluetooth() {
    for (const auto& [symbol, value] : std::array<std::pair<const char*, const char*>, 9>{{
             {"_CBATTErrorDomain", "CBATTErrorDomain"}, {"_CBErrorDomain", "CBErrorDomain"},
             {"_CBAdvertisementDataLocalNameKey", "kCBAdvDataLocalName"},
             {"_CBAdvertisementDataManufacturerDataKey", "kCBAdvDataManufacturerData"},
             {"_CBAdvertisementDataOverflowServiceUUIDsKey", "kCBAdvDataOverflowServiceUUIDs"},
             {"_CBAdvertisementDataServiceDataKey", "kCBAdvDataServiceData"},
             {"_CBAdvertisementDataServiceUUIDsKey", "kCBAdvDataServiceUUIDs"},
             {"_CBAdvertisementDataTxPowerLevelKey", "kCBAdvDataTxPowerLevel"},
             {"_CBCentralManagerScanOptionAllowDuplicatesKey", "kCBScanOptionAllowDuplicates"}}})
        string(core_bluetooth, symbol, value);
    const auto base = cls(core_bluetooth, "CBManager", "NSObject");
    method(base, "authorization", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, true, "q@:");   // CBManagerAuthorizationRestricted
    method(base, "authorization", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, false, "q@:");
    method(base, "state", [](CpuState& s, GuestMemory&) { s.x[0] = 2; }, false, "q@:");          // CBManagerStateUnsupported
    method(base, "delegate", [this](CpuState& s, GuestMemory&) {
        const auto value = objc.host_state<Manager>(s.x[0]);
        std::lock_guard lock(value->mutex);
        s.x[0] = value->delegate;
    });
    method(base, "setDelegate:", [this](CpuState& s, GuestMemory&) {
        const auto value = objc.host_state<Manager>(s.x[0]);
        std::lock_guard lock(value->mutex);
        value->delegate = s.x[2];
    }, false, "v@:@");
    const auto define_manager = [this](const char* name, const char* update) {
        const auto manager = cls(core_bluetooth, name, "CBManager");
        const std::string selector = update;
        const auto initialize = [this, selector](CpuState& s, GuestMemory&) {
            const auto self = s.x[0], delegate = s.x[2], queue = s.x[3] ? s.x[3] : bridge->main_queue();
            const auto value = objc.host_state<Manager>(self);
            { std::lock_guard lock(value->mutex); value->delegate = delegate; }
            // The state (unsupported) is delivered on the manager's queue.
            const auto retained_self = retain(self), retained_delegate = retain(delegate);
            bridge->dispatch_async(queue, [this, retained_self, retained_delegate, selector] {
                if (retained_delegate) {
                    CallRegs registers{};
                    registers.x[2] = retained_self;
                    static_cast<void>(objc.send_if_responds(retained_delegate, objc.selector(selector), registers));
                }
                release(retained_delegate);
                release(retained_self);
            });
        };
        method(manager, "init", [initialize](CpuState& s, GuestMemory& m) { s.x[2] = 0; s.x[3] = 0; initialize(s, m); });
        method(manager, "initWithDelegate:queue:", initialize);
        method(manager, "initWithDelegate:queue:options:", initialize);
        return manager;
    };
    const auto central = define_manager("CBCentralManager", "centralManagerDidUpdateState:");
    const auto misuse = [this](const char* what) {
        return [this, what](CpuState&, GuestMemory&) {
            notice_once(std::string("CoreBluetooth[WARNING] API MISUSE: ") + what + " can only be accepted in the powered on state (Bluetooth is unsupported)");
        };
    };
    method(central, "scanForPeripheralsWithServices:options:", misuse("scanForPeripheralsWithServices:options:"), false, "v@:@@");
    method(central, "stopScan", [](CpuState&, GuestMemory&) {}, false, "v@:");
    method(central, "isScanning", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    for (const auto* selector : {"retrievePeripheralsWithIdentifiers:", "retrieveConnectedPeripheralsWithServices:"})
        method(central, selector, [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); });
    const auto peripheral = define_manager("CBPeripheralManager", "peripheralManagerDidUpdateState:");
    method(peripheral, "startAdvertising:", misuse("startAdvertising:"), false, "v@:@");
    method(peripheral, "stopAdvertising", [](CpuState&, GuestMemory&) {}, false, "v@:");
    method(peripheral, "isAdvertising", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    // CBUUID
    const auto uuid = cls(core_bluetooth, "CBUUID", "NSObject");
    const auto make_uuid = [this, uuid](std::vector<std::uint8_t> bytes) {
        const auto object = objc.allocate(uuid);
        auto value = std::make_shared<Uuid>();
        value->bytes = std::move(bytes);
        static_cast<void>(objc.host_state<Uuid>(object, [&] { return value; }));
        return bridge->autorelease(object);
    };
    const auto bytes_of = [this](Address object) {
        const auto value = object ? objc.find_host_state<Uuid>(object) : nullptr;
        if (!value) throw std::runtime_error("Alchemy: the object is not a CBUUID");
        return value->bytes;
    };
    method(uuid, "UUIDWithString:", [this, make_uuid](CpuState& s, GuestMemory&) {
        const auto text = s.x[2] ? cf.utf8(s.x[2]) : std::string();
        const auto bytes = parse_uuid(text);
        if (!bytes) throw std::runtime_error("CBUUID: invalid UUID string '" + text + "'");
        s.x[0] = make_uuid(*bytes);
    }, true);
    method(uuid, "UUIDWithData:", [this, make_uuid](CpuState& s, GuestMemory&) {
        std::vector<std::uint8_t> bytes;
        if (s.x[2]) { const auto data = bridge->bytes(s.x[2]); bytes.assign(data.begin(), data.end()); }
        if (bytes.size() != 2 && bytes.size() != 4 && bytes.size() != 16) throw std::runtime_error("CBUUID: data must be 2, 4 or 16 bytes");
        if (bytes.size() == 16 && bytes[0] == 0 && bytes[1] == 0 && std::equal(bluetooth_base.begin(), bluetooth_base.end(), bytes.begin() + 4))
            bytes = {bytes[2], bytes[3]};
        s.x[0] = make_uuid(std::move(bytes));
    }, true);
    method(uuid, "UUIDString", [this, bytes_of](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.string(format_uuid(bytes_of(s.x[0])))); });
    method(uuid, "data", [this, bytes_of](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(bridge->data(bytes_of(s.x[0]))); });
    method(uuid, "isEqual:", [this, bytes_of](CpuState& s, GuestMemory&) {
        const auto other = s.x[2] ? objc.find_host_state<Uuid>(s.x[2]) : nullptr;
        s.x[0] = other && other->bytes == bytes_of(s.x[0]);
    }, false, "B@:@");
    method(uuid, "hash", [bytes_of](CpuState& s, GuestMemory&) {
        std::uint64_t h = 1469598103934665603ULL;
        for (const auto b : bytes_of(s.x[0])) h = (h ^ b) * 1099511628211ULL;
        s.x[0] = h;
    }, false, "Q@:");
}

void Peripherals::install_misc() {
    // GameController: no controllers.
    const auto controller = cls(game_controller, "GCController", "NSObject");
    method(controller, "controllers", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); }, true);
    method(controller, "current", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true);
    method(controller, "startWirelessControllerDiscoveryWithCompletionHandler:", [this](CpuState& s, GuestMemory&) {
        if (s.x[2]) static_cast<void>(bridge->invoke_block(s.x[2], {}));   // discovery ends at once: nothing to find
    }, true, "v@:@?");
    method(controller, "stopWirelessControllerDiscovery", [](CpuState&, GuestMemory&) {}, true, "v@:");
    // CoreWLAN: no Wi-Fi interface.
    const auto wifi = cls(core_wlan, "CWWiFiClient", "NSObject");
    method(wifi, "sharedWiFiClient", [this, wifi](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        if (!wifi_client) {
            wifi_client = objc.allocate(wifi);
            if (const auto record = process.objects().find(wifi_client)) record->immortal = true;
        }
        s.x[0] = wifi_client;
    }, true);
    method(wifi, "interfaceNames", [this](CpuState& s, GuestMemory&) {
        const auto set = objc.find_class("NSSet");
        s.x[0] = set ? objc.send(set, "set").x0 : 0;
    }, true);
    method(wifi, "interface", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    method(wifi, "interfaces", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); });
    // CoreLocation: location services are disabled.
    const std::array<double, 2> accuracies{-1.0, 100.0};
    for (std::size_t i = 0; i < accuracies.size(); ++i) {
        std::array<std::uint8_t, 8> bytes{};
        std::memcpy(bytes.data(), &accuracies[i], 8);
        static_cast<void>(data(core_location, i == 0 ? "_kCLLocationAccuracyBest" : "_kCLLocationAccuracyHundredMeters", bytes, 8));
    }
    const auto location = cls(core_location, "CLLocationManager", "NSObject");
    method(location, "locationServicesEnabled", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true, "B@:");
    method(location, "authorizationStatus", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, true, "i@:");   // kCLAuthorizationStatusRestricted
    method(location, "authorizationStatus", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, false, "i@:");
    method(location, "delegate", [this](CpuState& s, GuestMemory&) {
        const auto value = objc.host_state<LocationManager>(s.x[0]);
        std::lock_guard lock(value->mutex);
        s.x[0] = value->delegate;
    });
    method(location, "setDelegate:", [this](CpuState& s, GuestMemory&) {
        const auto value = objc.host_state<LocationManager>(s.x[0]);
        std::lock_guard lock(value->mutex);
        value->delegate = s.x[2];
    }, false, "v@:@");
    method(location, "desiredAccuracy", [this](CpuState& s, GuestMemory&) {
        const auto value = objc.host_state<LocationManager>(s.x[0]);
        std::lock_guard lock(value->mutex);
        return_real(s, value->accuracy);
    }, false, "d@:");
    method(location, "setDesiredAccuracy:", [this](CpuState& s, GuestMemory&) {
        const auto value = objc.host_state<LocationManager>(s.x[0]);
        std::lock_guard lock(value->mutex);
        value->accuracy = fill::real(s, 0);
    }, false, "v@:d");
    method(location, "distanceFilter", [this](CpuState& s, GuestMemory&) {
        const auto value = objc.host_state<LocationManager>(s.x[0]);
        std::lock_guard lock(value->mutex);
        return_real(s, value->distance);
    }, false, "d@:");
    method(location, "setDistanceFilter:", [this](CpuState& s, GuestMemory&) {
        const auto value = objc.host_state<LocationManager>(s.x[0]);
        std::lock_guard lock(value->mutex);
        value->distance = fill::real(s, 0);
    }, false, "v@:d");
    method(location, "location", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    for (const auto* selector : {"requestWhenInUseAuthorization", "requestAlwaysAuthorization", "stopUpdatingLocation"})
        method(location, selector, [](CpuState&, GuestMemory&) {}, false, "v@:");   // restricted: nothing to request or stop
    method(location, "startUpdatingLocation", [this](CpuState& s, GuestMemory&) {
        const auto value = objc.host_state<LocationManager>(s.x[0]);
        Address delegate{};
        { std::lock_guard lock(value->mutex); delegate = value->delegate; }
        if (!delegate) return;
        const auto self = retain(s.x[0]), target = retain(delegate);
        bridge->dispatch_async(bridge->main_queue(), [this, self, target] {
            const auto error = bridge->error("kCLErrorDomain", 1, "Location services are disabled");   // kCLErrorDenied
            CallRegs registers{};
            registers.x[2] = self;
            registers.x[3] = error;
            static_cast<void>(objc.send_if_responds(target, objc.selector("locationManager:didFailWithError:"), registers));
            release(error);
            release(target);
            release(self);
        });
    }, false, "v@:");
    // LocalAuthentication: nothing can be evaluated.
    const auto context = cls(local_authentication, "LAContext", "NSObject");
    const auto unavailable = [this](std::int64_t policy) {
        // LAPolicyDeviceOwnerAuthenticationWithBiometrics (1): LAErrorBiometryNotAvailable;
        // otherwise LAErrorPasscodeNotSet.
        return policy == 1 ? bridge->error("com.apple.LocalAuthentication", -6, "Biometry is not available")
                           : bridge->error("com.apple.LocalAuthentication", -5, "No device passcode is set");
    };
    method(context, "canEvaluatePolicy:error:", [this, unavailable](CpuState& s, GuestMemory& m) {
        if (s.x[3]) m.write64(s.x[3], bridge->autorelease(unavailable(static_cast<std::int64_t>(s.x[2]))));
        s.x[0] = 0;
    }, false, "B@:q^@");
    method(context, "evaluatePolicy:localizedReason:reply:", [this, unavailable](CpuState& s, GuestMemory&) {
        const auto error = unavailable(static_cast<std::int64_t>(s.x[2]));
        if (s.x[4]) static_cast<void>(bridge->invoke_block(s.x[4], {0, error}));
        release(error);
    }, false, "v@:q@@?");
    method(context, "invalidate", [](CpuState&, GuestMemory&) {}, false, "v@:");
}

} // namespace

void install_peripherals(InstallContext& context) {
    const auto provider = std::make_shared<Peripherals>(context);
    provider->install_bluetooth();
    provider->install_core_bluetooth();
    provider->install_misc();
}

} // namespace alchemy::gui::fill
