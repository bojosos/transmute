#pragma once

// The value-class provider (gui::values::Provider): fill::Provider plus
// helpers for frameworks whose classes are plain values (object, integer,
// real and weak properties kept in a host side table, singletons, completion
// handlers that fail) and for adding selectors to classes other providers
// implement. Fill-only like the strict-mode set (fill_provider.hpp): every
// export is added only when no provider has it, and says honestly what it does
// (real behaviour, a documented failure value, or an explicit stop).
//
// The frameworks' entry points that share one instance are installed in order
// by install_values_kit (value_provider.cpp; one installer-table row). They are
// Provider members defined in each framework's directory:
//   network           darwin/network/network_framework.cpp
//   appkit            darwin/appkit/appkit_gaps.cpp
//   services          darwin/coreservices/services.cpp (and the pieces it calls
//                     in place: printing, security, libsystem/misc/sandbox.cpp,
//                     opendirectory, systemconfiguration, iokit,
//                     servicemanagement, diskarbitration)
//   media             darwin/media/media_gaps.cpp
//   foundation        darwin/corefoundation/cf_string_clusters.cpp
//   selectors, menus, windows, spelling   darwin/appkit/
// The frameworks with no host service that use these helpers are their own
// providers and rows (darwin/<framework>/install.cpp: AVFAudio, Accessibility,
// AuthenticationServices, CoreHaptics, CoreML, CoreMediaIO, CryptoTokenKit,
// LocalAuthenticationEmbeddedUI, MediaPlayer, SafariServices,
// UserNotifications, Vision, ScreenCaptureKit, SkyLight, QuickLookUI,
// StoreKit, CoreData).
// Chrome 155's image tree was the first program to need them
// (docs/ALCHEMY_CHROME_AUDIT.md, stage 1 item (c)).

#include "alchemy/gui/install.hpp"
#include "alchemy/gui/fill_provider.hpp"

#include <map>
#include <set>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::values {

using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

#define ALCHEMY_VALUES_FRAMEWORK(name, version) "/System/Library/Frameworks/" name ".framework/Versions/" version "/" name
inline constexpr std::string_view network = ALCHEMY_VALUES_FRAMEWORK("Network", "A");
inline constexpr std::string_view cf_network = ALCHEMY_VALUES_FRAMEWORK("CFNetwork", "A");
inline constexpr std::string_view metal = ALCHEMY_VALUES_FRAMEWORK("Metal", "A");
inline constexpr std::string_view service_management = ALCHEMY_VALUES_FRAMEWORK("ServiceManagement", "A");
inline constexpr std::string_view uniform_type_identifiers = ALCHEMY_VALUES_FRAMEWORK("UniformTypeIdentifiers", "A");
#undef ALCHEMY_VALUES_FRAMEWORK
inline constexpr std::string_view libsandbox = "/usr/lib/libsandbox.1.dylib";
inline constexpr std::string_view libbz2 = "/usr/lib/libbz2.1.0.dylib";

// fill::Provider plus helpers for plain value classes.
class Provider : public fill::Provider {
public:
    using fill::Provider::Provider;

    // Object properties of a value class: `-name` and `-setName:` (retained
    // copies of the stored object, nil when unset), kept in host state.
    void object_properties(Address target, std::initializer_list<std::string_view> names);
    // Integer (NSInteger / BOOL / enum) properties, 0 when unset.
    void integer_properties(Address target, std::initializer_list<std::string_view> names);
    // Unretained object properties (delegates, `weak`/`assign`).
    void weak_properties(Address target, std::initializer_list<std::string_view> names);
    // Host access to the same storage (`store` retains `value`).
    void store(Address object, std::string_view key, Address value);
    void store_integer(Address object, std::string_view key, std::int64_t value);
    [[nodiscard]] Address stored(Address object, std::string_view key);
    [[nodiscard]] std::int64_t stored_integer(Address object, std::string_view key);
    // [[cls alloc] init] (+1).
    Address create(Address cls);
    // Double (NSTimeInterval / CGFloat / float stored as double) properties.
    void real_properties(Address target, std::initializer_list<std::string_view> names, bool single_precision = false);
    // `+new` / `-init` exist on NSObject; this adds an immortal `+selector`
    // singleton of `target` (sharedX / defaultX / currentX).
    void singleton(Address target, std::string_view selector);
    // `float` / `double` data constants.
    void float_constant(std::string_view library, std::string_view name, float value);
    void double_constant(std::string_view library, std::string_view name, double value);
    // A completion handler `block(nil, error)` invoked on the main queue.
    void complete_with_error(Address block, std::string_view domain, std::int64_t code, std::string_view description,
                             unsigned leading_nils = 1);

    // The value classes' storage, keyed by instance (a side table: instances
    // of NSView or NSButton subclasses already carry their class's host state).
    std::shared_ptr<struct Properties> properties(Address object, bool make = true);
    void cleanup_on_dealloc(Address target);

    std::mutex mutex;
    std::map<std::pair<Address, std::string>, Address> singletons;
    std::mutex table_mutex;
    std::map<Address, std::shared_ptr<struct Properties>> table;
    std::set<Address> cleaned;

    void install_network();
    void install_appkit();
    void install_services();
    // install_services' pieces, each in its framework's directory (called in place).
    void install_print_core();            // darwin/printing/print_core_gaps.cpp
    void install_legacy_keychain();       // darwin/security/legacy_keychain.cpp
    void install_sandbox();               // darwin/libsystem/misc/sandbox.cpp
    void install_open_directory();        // darwin/opendirectory/constants.cpp
    void install_network_interfaces();    // darwin/systemconfiguration/network_interfaces.cpp
    void install_iokit_gaps();            // darwin/iokit/iokit_gaps.cpp
    void install_service_management();    // darwin/servicemanagement/job_bless.cpp
    void install_disk_claims();           // darwin/diskarbitration/claims.cpp
    void install_fsevents();              // darwin/coreservices/fsevents.cpp
    void install_media();
    void install_foundation();
    void install_selectors();
    void install_menus();
    void install_windows();
    void install_spelling();

    // Adds `selector` to the existing class `name` unless it (or a superclass)
    // implements it already.
    void extend(std::string_view name, std::string_view selector, transmute::GuestCallHandler handler,
                bool class_method = false, std::string types = "@@:");
    // Side-table storage (properties()) on instances of a class another
    // provider implements: a dealloc that clears it, then runs the class's own.
    void track_dealloc(std::string_view name);
    // The side table follows -copyWithZone: of that class: the copy gets the
    // same values (objects retained).
    void track_copy(std::string_view name);
    // The same for AppKit's NSWindow, NSMenu, NSMenuItem and NSBitmapImageRep,
    // whose own -dealloc / -copyWithZone: tell their observers
    // (AppKit::observe_object_lifetimes). Once per process.
    void observe_appkit_lifetimes();
    void drop_properties(Address object);
    void copy_properties(Address source, Address copy);
    bool appkit_lifetimes_observed{};
};

// Storage of the value classes (object_properties and friends).
struct Properties {
    std::mutex mutex;
    std::map<std::string, Address, std::less<>> objects;   // retained
    std::map<std::string, std::int64_t, std::less<>> integers;
    std::map<std::string, double, std::less<>> reals;
};

// "title" -> "setTitle:".
std::string setter_name(std::string_view property);

// The installer-table row: the entry points above, in order.
void install_values_kit(InstallContext& context);

} // namespace alchemy::gui::values
