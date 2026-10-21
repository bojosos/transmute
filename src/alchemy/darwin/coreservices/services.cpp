#include "alchemy/gui/value_provider.hpp"

#include <array>
#include <cstdio>

// System-service gaps of Chrome's framework and helpers:
// - ApplicationServices, accessibility client (AXUIElement / AXObserver):
//   AXValue, AXTextMarker and AXTextMarkerRange are real values;
//   AXUIElementCreateApplication makes a real element, but no accessibility
//   server answers, so every message to another process fails with
//   kAXErrorCannotComplete and observers cannot be created.
//   UAZoomChangeFocus has no Zoom to inform (noErr: Zoom is not enabled).
// - CoreServices / Carbon: Apple Event descriptors are real (AECreateDesc,
//   AEPutParamDesc, AEGetParamDesc, AEDisposeDesc, AECreateAppleEvent);
//   handlers are recorded but no event ever arrives (no Apple Event server);
//   AESendMessage fails with connectionInvalid; LaunchServices registration
//   and LSOpenFromURLSpec fail (kLSServerCommunicationErr); Spotlight
//   attributes cannot be set. The process is not a trusted AX client.
//   The Carbon event loop is empty (ReceiveNextEvent: eventLoopTimedOutErr).
// - UniformTypeIdentifiers constants.
// The other frameworks' pieces live in their own directories and are called
// from install_services in their old order: PrintCore (printing/), legacy
// Security (security/), libsandbox (libsystem/misc/), OpenDirectory
// (opendirectory/), ServiceManagement (servicemanagement/), DiskArbitration
// (diskarbitration/), SystemConfiguration
// interfaces (systemconfiguration/) and IOKit (iokit/).
namespace alchemy::gui::values {
namespace {

using fill::application_services;
using fill::carbon;
using fill::core_foundation;
using fill::core_services;
using fill::status;

constexpr std::int32_t ax_cannot_complete = -25204, ax_illegal_argument = -25201;
constexpr std::int32_t ls_server_communication = -10822;
constexpr std::int32_t ae_connection_invalid = -609, ae_param = -50, ae_desc_not_found = -1701;
constexpr std::int32_t event_loop_timed_out = -9875, event_not_handled = -9874;
constexpr std::uint32_t type_null = fill::fourcc("null");

// <HIServices/AXValue.h> kAXValueType*: CGPoint 1, CGSize 2, CGRect 3, CFRange 4, AXError 5.
std::size_t ax_value_size(std::uint64_t type) {
    switch (type) {
    case 1: case 2: case 4: return 16;
    case 3: return 32;
    case 5: return 4;
    default: return 0;
    }
}

struct AxValue final : ObjectPayload { std::uint64_t type{}; std::vector<std::uint8_t> bytes; };
struct AxElement final : ObjectPayload { std::int64_t pid{}; };
struct TextMarker final : ObjectPayload { std::vector<std::uint8_t> bytes; };
struct TextMarkerRange final : ObjectPayload { Address start{}, end{}; };   // retained markers

// An Apple Event descriptor's data: AEDesc.dataHandle points at a guest
// block {size (8 bytes), bytes}. Records and events keep their parameters
// host-side, keyed by the dataHandle.
struct AeRecord { std::map<std::uint32_t, std::pair<std::uint32_t, std::vector<std::uint8_t>>> parameters; };

} // namespace

void Provider::install_services() {
    // ---------------------------------------------------------------- AX client
    const auto value_class = cls(application_services, "__AXValue", "NSObject");
    const auto element_class = cls(application_services, "__AXUIElement", "NSObject");
    const auto marker_class = cls(application_services, "__AXTextMarker", "NSObject");
    const auto range_class = cls(application_services, "__AXTextMarkerRange", "NSObject");
    // CFGetTypeID values for the HIServices types (Chrome range 0x700).
    register_type(value_class, static_cast<fill::TypeId>(0x700));
    register_type(element_class, static_cast<fill::TypeId>(0x701));
    register_type(marker_class, static_cast<fill::TypeId>(0x702));
    register_type(range_class, static_cast<fill::TypeId>(0x703));
    constant(application_services, "_AXValueGetTypeID", 0x700);
    constant(application_services, "_AXUIElementGetTypeID", 0x701);
    constant(application_services, "_AXTextMarkerGetTypeID", 0x702);
    constant(application_services, "_AXTextMarkerRangeGetTypeID", 0x703);
    raw(application_services, "_AXValueCreate", [this, value_class](CpuState& s, GuestMemory& m) {
        const auto size = ax_value_size(s.x[0]);
        if (!size || !s.x[1]) { s.x[0] = 0; return; }
        auto value = std::make_shared<AxValue>();
        value->type = s.x[0];
        value->bytes.resize(size);
        m.read_bytes(s.x[1], value->bytes);
        const auto object = objc.allocate(value_class);
        static_cast<void>(objc.host_state<AxValue>(object, [&] { return value; }));
        s.x[0] = object;
    });
    raw(application_services, "_AXValueGetType", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? objc.find_host_state<AxValue>(s.x[0]) : nullptr;
        s.x[0] = value ? value->type : 0;   // kAXValueTypeIllegal
    });
    raw(application_services, "_AXValueGetValue", [this](CpuState& s, GuestMemory& m) {
        const auto value = s.x[0] ? objc.find_host_state<AxValue>(s.x[0]) : nullptr;
        if (!value || value->type != s.x[1] || !s.x[2]) { s.x[0] = 0; return; }
        m.write_bytes(s.x[2], value->bytes);
        s.x[0] = 1;
    });
    raw(application_services, "_AXUIElementCreateApplication", [this, element_class](CpuState& s, GuestMemory&) {
        const auto object = objc.allocate(element_class);
        objc.host_state<AxElement>(object)->pid = static_cast<std::int32_t>(s.x[0]);
        s.x[0] = object;
    });
    // Messages to an element: no accessibility server answers.
    failure(application_services, "_AXUIElementCopyAttributeValue", ax_cannot_complete, {2});
    failure(application_services, "_AXUIElementCopyAttributeNames", ax_cannot_complete, {1});
    failure(application_services, "_AXUIElementCopyActionNames", ax_cannot_complete, {1});
    failure(application_services, "_AXUIElementCopyParameterizedAttributeNames", ax_cannot_complete, {1});
    failure(application_services, "_AXUIElementCopyParameterizedAttributeValue", ax_cannot_complete, {3});
    failure(application_services, "_AXUIElementSetAttributeValue", ax_cannot_complete);
    failure(application_services, "_AXUIElementPerformAction", ax_cannot_complete);
    // AXObserverCreateWithInfoCallback(pid, callback, AXObserverRef *): no
    // notification can ever be delivered, so no observer is created.
    failure(application_services, "_AXObserverCreateWithInfoCallback", ax_cannot_complete, {2});
    failure(application_services, "_AXObserverCreate", ax_cannot_complete, {2});
    failure(application_services, "_AXObserverAddNotification", ax_illegal_argument);   // only a NULL observer can exist
    failure(application_services, "_AXObserverRemoveNotification", ax_illegal_argument);
    constant(application_services, "_AXObserverGetRunLoopSource", 0);
    // The process is not a trusted accessibility client (no accessibility
    // server exists to trust it); kAXTrustedCheckOptionPrompt shows nothing.
    constant(application_services, "_AXIsProcessTrusted", 0);
    constant(application_services, "_AXIsProcessTrustedWithOptions", 0);
    string(application_services, "_kAXTrustedCheckOptionPrompt", "AXTrustedCheckOptionPrompt");
    raw(application_services, "_AXTextMarkerGetLength", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? objc.find_host_state<TextMarker>(s.x[0]) : nullptr;
        s.x[0] = value ? value->bytes.size() : 0;
    });
    std::shared_ptr<std::map<Address, Address>> marker_bytes = std::make_shared<std::map<Address, Address>>();
    raw(application_services, "_AXTextMarkerCreate", [this, marker_class, marker_bytes](CpuState& s, GuestMemory& m) {
        auto value = std::make_shared<TextMarker>();
        value->bytes.resize(static_cast<std::size_t>(s.x[2]));
        if (!value->bytes.empty()) m.read_bytes(s.x[1], value->bytes);
        const auto object = objc.allocate(marker_class);
        static_cast<void>(objc.host_state<TextMarker>(object, [&] { return value; }));
        const auto copy = process.allocate_guest(std::max<std::size_t>(value->bytes.size(), 1));
        if (!value->bytes.empty()) m.write_bytes(copy, value->bytes);
        std::lock_guard lock(mutex);
        (*marker_bytes)[object] = copy;
        s.x[0] = object;
    });
    raw(application_services, "_AXTextMarkerGetBytePtr", [this, marker_bytes](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        const auto found = marker_bytes->find(s.x[0]);
        s.x[0] = found == marker_bytes->end() ? 0 : found->second;
    });
    method(marker_class, "dealloc", [this, marker_class, marker_bytes](CpuState& s, GuestMemory&) {
        Address copy{};
        {
            std::lock_guard lock(mutex);
            if (const auto found = marker_bytes->find(s.x[0]); found != marker_bytes->end()) {
                copy = found->second;
                marker_bytes->erase(found);
            }
        }
        if (copy) process.release_guest(copy);
        static_cast<void>(objc.send_super(s.x[0], marker_class, objc.selector("dealloc")));
    }, false, "v@:");
    raw(application_services, "_AXTextMarkerRangeCreate", [this, range_class](CpuState& s, GuestMemory&) {
        // (allocator, startMarker, endMarker)
        if (!s.x[1] || !s.x[2]) { s.x[0] = 0; return; }
        const auto object = objc.allocate(range_class);
        auto value = std::make_shared<TextMarkerRange>();
        value->start = retain(s.x[1]);
        value->end = retain(s.x[2]);
        static_cast<void>(objc.host_state<TextMarkerRange>(object, [&] { return value; }));
        s.x[0] = object;
    });
    raw(application_services, "_AXTextMarkerRangeCopyStartMarker", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? objc.find_host_state<TextMarkerRange>(s.x[0]) : nullptr;
        s.x[0] = value ? retain(value->start) : 0;
    });
    raw(application_services, "_AXTextMarkerRangeCopyEndMarker", [this](CpuState& s, GuestMemory&) {
        const auto value = s.x[0] ? objc.find_host_state<TextMarkerRange>(s.x[0]) : nullptr;
        s.x[0] = value ? retain(value->end) : 0;
    });
    method(range_class, "dealloc", [this, range_class](CpuState& s, GuestMemory&) {
        if (const auto value = objc.find_host_state<TextMarkerRange>(s.x[0])) {
            release(std::exchange(value->start, 0));
            release(std::exchange(value->end, 0));
        }
        static_cast<void>(objc.send_super(s.x[0], range_class, objc.selector("dealloc")));
    }, false, "v@:");
    constant(application_services, "_UAZoomChangeFocus", 0);   // Zoom is not enabled (UAZoomEnabled)

    install_print_core();            // darwin/printing/print_core_gaps.cpp

    // ---------------------------------------------------------------- Apple Events
    // AEDesc { DescType descriptorType; AEDataStorage dataHandle; } (16 bytes).
    auto records = std::make_shared<std::map<Address, AeRecord>>();
    const auto make_handle = [this](std::span<const std::uint8_t> bytes) {
        const auto handle = malloc(8 + bytes.size());
        memory().write64(handle, bytes.size());
        if (!bytes.empty()) memory().write_bytes(handle + 8, bytes);
        return handle;
    };
    const auto handle_bytes = [](GuestMemory& m, Address handle) {
        std::vector<std::uint8_t> bytes;
        if (!handle) return bytes;
        bytes.resize(static_cast<std::size_t>(m.read64(handle)));
        if (!bytes.empty()) m.read_bytes(handle + 8, bytes);
        return bytes;
    };
    // OSErr AECreateDesc(DescType, const void *dataPtr, Size, AEDesc *result)
    raw(core_services, "_AECreateDesc", [this, make_handle](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) { s.x[0] = status(ae_param); return; }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(s.x[2]));
        if (!bytes.empty()) {
            if (!s.x[1]) { s.x[0] = status(ae_param); return; }
            m.read_bytes(s.x[1], bytes);
        }
        m.write32(s.x[3], static_cast<std::uint32_t>(s.x[0]));
        m.write64(s.x[3] + 8, make_handle(bytes));
        s.x[0] = 0;
    });
    // OSErr AEDisposeDesc(AEDesc *): frees the data and leaves a null descriptor.
    raw(core_services, "_AEDisposeDesc", [this, records](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = status(ae_param); return; }
        const auto handle = m.read64(s.x[0] + 8);
        if (handle) {
            {
                std::lock_guard lock(mutex);
                records->erase(handle);
            }
            free(handle);
        }
        m.write32(s.x[0], type_null);
        m.write64(s.x[0] + 8, 0);
        s.x[0] = 0;
    });
    // OSErr AECreateAppleEvent(AEEventClass, AEEventID, const AEAddressDesc *target,
    //     AEReturnID, AETransactionID, AppleEvent *result): an event record
    //     ('aevt') whose class, ID and target are parameters.
    raw(core_services, "_AECreateAppleEvent", [this, records, make_handle, handle_bytes](CpuState& s, GuestMemory& m) {
        if (!s.x[5]) { s.x[0] = status(ae_param); return; }
        const auto handle = make_handle({});
        AeRecord record;
        auto four = [](std::uint32_t value) {
            return std::vector<std::uint8_t>{static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
                                             static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
        };
        record.parameters[fill::fourcc("evcl")] = {fill::fourcc("type"), four(static_cast<std::uint32_t>(s.x[0]))};
        record.parameters[fill::fourcc("evid")] = {fill::fourcc("type"), four(static_cast<std::uint32_t>(s.x[1]))};
        if (s.x[2]) record.parameters[fill::fourcc("addr")] = {m.read32(s.x[2]), handle_bytes(m, m.read64(s.x[2] + 8))};
        {
            std::lock_guard lock(mutex);
            (*records)[handle] = std::move(record);
        }
        m.write32(s.x[5], fill::fourcc("aevt"));
        m.write64(s.x[5] + 8, handle);
        s.x[0] = 0;
    });
    // OSErr AEPutParamDesc(AppleEvent *, AEKeyword, const AEDesc *)
    raw(core_services, "_AEPutParamDesc", [this, records, handle_bytes](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[2]) { s.x[0] = status(ae_param); return; }
        const auto handle = m.read64(s.x[0] + 8);
        std::lock_guard lock(mutex);
        const auto found = records->find(handle);
        if (found == records->end()) { s.x[0] = status(-1700); return; }   // errAECoercionFail: not a record
        found->second.parameters[static_cast<std::uint32_t>(s.x[1])] = {m.read32(s.x[2]), handle_bytes(m, m.read64(s.x[2] + 8))};
        s.x[0] = 0;
    });
    // OSErr AEGetParamDesc(const AppleEvent *, AEKeyword, DescType desiredType, AEDesc *result)
    raw(core_services, "_AEGetParamDesc", [this, records, make_handle](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[3]) { s.x[0] = status(ae_param); return; }
        m.write32(s.x[3], type_null);
        m.write64(s.x[3] + 8, 0);
        std::pair<std::uint32_t, std::vector<std::uint8_t>> parameter;
        {
            std::lock_guard lock(mutex);
            const auto found = records->find(m.read64(s.x[0] + 8));
            if (found == records->end()) { s.x[0] = status(ae_desc_not_found); return; }
            const auto entry = found->second.parameters.find(static_cast<std::uint32_t>(s.x[1]));
            if (entry == found->second.parameters.end()) { s.x[0] = status(ae_desc_not_found); return; }
            parameter = entry->second;
        }
        // Only typeWildCard ('****') or the stored type: no coercion handlers exist.
        const auto desired = static_cast<std::uint32_t>(s.x[2]);
        if (desired != fill::fourcc("****") && desired != parameter.first) { s.x[0] = status(-1700); return; }
        m.write32(s.x[3], parameter.first);
        m.write64(s.x[3] + 8, make_handle(parameter.second));
        s.x[0] = 0;
    });
    // Handlers are recorded; no Apple Event is ever delivered to this process.
    auto handlers = std::make_shared<std::set<std::pair<std::uint32_t, std::uint32_t>>>();
    raw(core_services, "_AEInstallEventHandler", [this, handlers](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        handlers->insert({static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1])});
        s.x[0] = 0;
    });
    raw(core_services, "_AERemoveEventHandler", [this, handlers](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        s.x[0] = handlers->erase({static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1])}) ? 0 : status(-1717);   // errAEHandlerNotFound
    });
    // AESendMessage(event, reply, mode, timeout): no process can receive it.
    raw(core_services, "_AESendMessage", [](CpuState& s, GuestMemory& m) {
        if (s.x[1]) { m.write32(s.x[1], type_null); m.write64(s.x[1] + 8, 0); }
        s.x[0] = status(ae_connection_invalid);
    });
    failure(carbon, "_AEProcessEvent", event_not_handled);
    failure(core_services, "_LSRegisterURL", ls_server_communication);
    // LSOpenFromURLSpec(const LSLaunchURLSpec*, CFURLRef* outLaunchedURL):
    // no Launch Services server opens anything (NSWorkspace opens files).
    raw(core_services, "_LSOpenFromURLSpec", [](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write64(s.x[1], 0);
        s.x[0] = status(ls_server_communication);
    });
    constant(core_services, "_MDItemSetAttribute", 0);   // Boolean: Spotlight is not available
    string(core_services, "__kLSOpenOptionBackgroundLaunchKey", "LSBackgroundLaunch");   // (unverified)
    // Carbon event loop: nothing is ever queued.
    raw(carbon, "_ReceiveNextEvent", [](CpuState& s, GuestMemory& m) {
        // (numTypes, list, timeout d0, pullEvent, EventRef *outEvent)
        if (s.x[3]) m.write64(s.x[3], 0);
        s.x[0] = status(event_loop_timed_out);
    });
    failure(carbon, "_CallNextEventHandler", event_not_handled);
    constant(carbon, "_GetEventKind", 0);
    raw(carbon, "_ReleaseEvent", [](CpuState&, GuestMemory&) {});   // only NULL events can exist
    constant(carbon, "_RemoveEventHandler", 0);

    install_legacy_keychain();       // darwin/security/legacy_keychain.cpp
    install_sandbox();               // darwin/libsystem/misc/sandbox.cpp
    install_open_directory();        // darwin/opendirectory/constants.cpp

    // ---------------------------------------------------------------- UTI
    string(uniform_type_identifiers, "_UTTagClassFilenameExtension", "public.filename-extension");
    string(uniform_type_identifiers, "_UTTagClassMIMEType", "public.mime-type");
    if (!has(uniform_type_identifiers, "_UTTypeInternetShortcut")) {
        if (const auto type_class = objc.find_class("UTType")) {
            // The Windows .url internet shortcut (identifier unverified).
            CallRegs registers{};
            registers.x[2] = cf.string("com.microsoft.internet-shortcut");
            const auto type = objc.send(type_class, "importedTypeWithIdentifier:", registers).x0;
            release(registers.x[2]);
            if (type) value(uniform_type_identifiers, "_UTTypeInternetShortcut", type);
        }
    }

    install_network_interfaces();    // darwin/systemconfiguration/network_interfaces.cpp
    install_iokit_gaps();            // darwin/iokit/iokit_gaps.cpp
    install_service_management();    // darwin/servicemanagement/job_bless.cpp
    install_disk_claims();           // darwin/diskarbitration/claims.cpp
    install_fsevents();              // darwin/coreservices/fsevents.cpp
}

} // namespace alchemy::gui::values
