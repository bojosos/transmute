#include "alchemy/darwin/kit/fwgaps/fwgaps.hpp"
#include "alchemy/darwin/accelerate/install.hpp"
#include "alchemy/darwin/corefoundation/install.hpp"
#include "alchemy/darwin/foundation/install.hpp"

namespace alchemy::gui::fill {

void install_fwgaps(InstallContext& context) {
    gui::install_corefoundation_sets_and_charsets(context);
    gui::install_foundation_property_list_serialization(context);
    install_appkit_gaps(context);
    install_graphics_gaps(context);
    install_coreservices_launch_services(context);
    install_security_cssm_and_tasks(context);
    install_securityinterface(context);
    install_systemconfiguration_errors_and_aliases(context);
    install_opendirectory_sessions(context);
    install_objc_exception_types(context);
    install_accelerate(context);   // darwin/accelerate/vdsp.cpp
}

} // namespace alchemy::gui::fill
