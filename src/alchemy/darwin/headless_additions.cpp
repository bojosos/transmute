#include "alchemy/darwin/headless_additions.hpp"

#include "alchemy/darwin/foundation/headless_shells.hpp"
#include "alchemy/placeholder_exports.hpp"
#include "alchemy/process_context.hpp"

namespace alchemy::darwin_headless {

void install_additions(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    forget_placeholder_exports(context);
    install_corefoundation_strings_and_bundles(context, objc, cf);
    install_appkit_color_alpha(context, objc, cf);
    install_foundation_class_shells_and_events(context, objc, cf);
    install_libsystem_crt_and_host_info(context, objc, cf);
    install_applicationservices_process_manager(context, objc, cf);
    foundation_shells::install_authorization_shell(context);   // Security: alchemy.exe only (darwin/security/authorization_headless.cpp)
    install_foundation_geometry(context, objc, cf);
    install_coregraphics_event_source(context, objc, cf);
    install_appkit_carbon_events(context, objc, cf);
    install_printing_no_printers(context, objc, cf);
    install_coreservices_types_and_gestalt(context, objc, cf);
    foundation_shells::install_cf_date_formatter_shell(context);   // alchemy.exe only (r2's headless shells)
    foundation_shells::install_cfsocket_shell(context);
    install_quartzcore_contents_formats(context, objc, cf);
    install_appkit_class_shells(context, objc, cf);
}

} // namespace alchemy::darwin_headless
