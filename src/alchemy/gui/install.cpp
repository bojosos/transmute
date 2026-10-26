#include "alchemy/gui/install.hpp"

#include "alchemy/gui/fill_provider.hpp"
#include "alchemy/gui/value_provider.hpp"
#include "alchemy/perf.hpp"
#include "alchemy/registration_audit.hpp"
#include "alchemy/darwin/compression/install.hpp"
#include "alchemy/darwin/avfaudio/install.hpp"
#include "alchemy/darwin/accessibility/install.hpp"
#include "alchemy/darwin/authenticationservices/install.hpp"
#include "alchemy/darwin/corehaptics/install.hpp"
#include "alchemy/darwin/coreml/install.hpp"
#include "alchemy/darwin/coremediaio/install.hpp"
#include "alchemy/darwin/cryptotokenkit/install.hpp"
#include "alchemy/darwin/localauthenticationembeddedui/install.hpp"
#include "alchemy/darwin/mediaplayer/install.hpp"
#include "alchemy/darwin/opengl/install.hpp"
#include "alchemy/darwin/openal/openal.hpp"
#include "alchemy/darwin/safariservices/install.hpp"
#include "alchemy/darwin/usernotifications/install.hpp"
#include "alchemy/darwin/vision/install.hpp"
#include "alchemy/darwin/screencapturekit/install.hpp"
#include "alchemy/darwin/skylight/install.hpp"
#include "alchemy/darwin/quicklookui/install.hpp"
#include "alchemy/darwin/quicklookthumbnailing/install.hpp"
#include "alchemy/darwin/colorsync/profiles.hpp"
#include "alchemy/darwin/storekit/install.hpp"
#include "alchemy/darwin/coredata/install.hpp"
#include "alchemy/darwin/corefoundation/install.hpp"
#include "alchemy/darwin/coreservices/install.hpp"
#include "alchemy/darwin/dispatch/install.hpp"
#include "alchemy/darwin/foundation/install.hpp"
#include "alchemy/darwin/accelerate/install.hpp"
#include "alchemy/darwin/applicationservices/install.hpp"
#include "alchemy/darwin/diskarbitration/install.hpp"
#include "alchemy/darwin/libsystem/install.hpp"
#include "alchemy/darwin/security/install.hpp"
#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

// GUI provider registration for alchemy-fx (ALCHEMY_WITH_GUI). The order is
// part of the contract; see include/alchemy/gui/install.hpp.
namespace alchemy {
namespace {

struct Installer {
    const char* name; // the owner the registration audit reports
    void (*install)(gui::InstallContext&);
};

// One row per installer, in installation order.
constexpr Installer installers[] = {
    {"install_runloop", gui::install_runloop},
    {"install_windowserver", gui::install_windowserver},
    {"install_appkit_gui", gui::install_appkit_gui},
    {"install_quartzcore", gui::install_quartzcore},
    {"install_metal_gui", gui::install_metal_gui},
    {"install_opengl", opengl::install_opengl},      // OpenGL.framework over host::gl (darwin/opengl/)
    {"install_openal_gui", openal::install_openal_gui}, // OpenAL over the default output device (darwin/openal/)
    {"install_system_configuration", gui::install_system_configuration},
    {"install_quartz2d", gui::install_quartz2d},
    {"install_security", gui::install_security},           // Security, then GSS (darwin/security/install.cpp)
    {"install_network", gui::install_network},
    // The former gap-fill group, row by row in its order. Every
    // row fills gaps only (export_fill::Provider never replaces an export).
    // (install_gss is part of install_security now.)
    {"install_zlib", compression::install_zlib},
    {"install_libsystem_gaps", libsystem::install_libsystem_gaps},
    {"install_security_common_cryptor", [](gui::InstallContext& c) {
        export_fill::Provider p(c.process); gui::install_security_common_cryptor(p); }},
    // The former install_framework_gaps (a renderer plug-in's non-drawing framework
    // symbols), one row per framework in its order:
    {"install_applicationservices", applicationservices::install_applicationservices},
    {"install_diskarbitration", diskarbitration::install_diskarbitration},
    {"install_coreservices_carbon_core", gui::install_coreservices_carbon_core},
    {"install_corefoundation_time_zones_and_uuids", gui::install_corefoundation_time_zones_and_uuids},
    {"install_foundation_file_attribute_keys", gui::install_foundation_file_attribute_keys},
    {"install_accelerate_bnns", accelerate::install_accelerate_bnns},
    {"install_iokit_registry", [](gui::InstallContext& c) {   // owner: r3
        export_fill::Provider p(c.process); export_fill::install_iokit_registry(p, c.objc, c.cf); }},
    {"install_corefoundation_additions", [](gui::InstallContext& c) {
        export_fill::Provider p(c.process); gui::install_corefoundation_additions(p, c.objc, c.cf); }},
    {"install_foundation_value_classes", [](gui::InstallContext& c) {
        export_fill::Provider p(c.process); gui::install_foundation_value_classes(p, c.objc, c.cf); }},
    {"install_coreservices_collation_and_types", [](gui::InstallContext& c) {
        export_fill::Provider p(c.process); gui::install_coreservices_collation_and_types(p, c.objc, c.cf); }},
    {"install_libsystem_services", libsystem::install_libsystem_services},
    {"install_dispatch_data", dispatch::install_dispatch_data},
    {"install_support_kit", gui::install_support_kit},     // its gap fillers never shadow another provider
    {"install_completion_kit", gui::install_completion_kit}, // completes classes the providers above declare (NSThread, ...)
    {"install_mach", libsystem::install_mach}, // Mach IPC and notify(3) (the former install_chrome_support)
    // Strict-mode frameworks (fill-only, so they run after every provider above):
    // the identities and exports Qt WebEngine links, so a renderer plug-in's images bind
    // without ALCHEMY_PROBE_MISSING.
    {"fill::install_machsys", gui::fill::install_machsys},
    {"fill::install_fwgaps", gui::fill::install_fwgaps},
    {"fill::install_media", gui::fill::install_media},
    {"fill::install_devices", gui::fill::install_devices},
    {"fill::install_printing", gui::fill::install_printing},
    {"fill::install_libsystem_leftovers", gui::fill::install_libsystem_leftovers},
    // Frameworks with no host service, each answering the documented
    // "unavailable" way (their READMEs; fill-only, through values::Provider):
    {"install_avfaudio", avfaudio::install_avfaudio},
    {"install_accessibility", accessibility::install_accessibility},
    {"install_authenticationservices", authenticationservices::install_authenticationservices},
    {"install_corehaptics", corehaptics::install_corehaptics},
    {"install_coreml", coreml::install_coreml},
    {"install_coremediaio", coremediaio::install_coremediaio},
    {"install_cryptotokenkit", cryptotokenkit::install_cryptotokenkit},
    {"install_localauthenticationembeddedui", localauthenticationembeddedui::install_localauthenticationembeddedui},
    {"install_mediaplayer", mediaplayer::install_mediaplayer},
    {"install_safariservices", safariservices::install_safariservices},
    {"install_usernotifications", usernotifications::install_usernotifications},
    {"install_vision", vision::install_vision},
    {"install_screencapturekit", screencapturekit::install_screencapturekit},
    {"install_skylight", skylight::install_skylight},
    {"install_quicklookui", quicklookui::install_quicklookui},
    {"install_quicklookthumbnailing", quicklookthumbnailing::install_quicklookthumbnailing},
    {"install_colorsync_profiles", colorsync::install_colorsync_profiles},
    {"install_storekit", storekit::install_storekit},
    {"install_coredata", coredata::install_coredata},
    {"install_values_kit", gui::values::install_values_kit}, // value-class frameworks (after the fill-only set)
};

} // namespace

void install_darwin_gui(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    gui::InstallContext install{context, objc, cf};
    for (const auto& installer : installers) {
        registration_audit::Scope scope(installer.name);
        installer.install(install);
        if (perf::enabled()) perf::phase(std::string("providers: ") + installer.name);
    }
}

} // namespace alchemy
