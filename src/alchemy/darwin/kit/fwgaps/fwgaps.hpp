#pragma once

// Framework gaps (src/alchemy/darwin/kit/fwgaps): exports of frameworks Alchemy already
// provides (CoreFoundation, Foundation, AppKit, CoreGraphics, QuartzCore,
// CoreText, CoreServices, Security, SystemConfiguration, OpenGL, Carbon,
// Accelerate, libobjc) that the Qt WebEngine images bind and no provider had,
// plus the small identities OpenDirectory and SecurityInterface. Installed
// by install_fwgaps (fill-only, after every other GUI installer).

#include "alchemy/gui/fill_provider.hpp"

namespace alchemy::gui::fill {

void install_cf_gaps(InstallContext& context);
void install_character_sets(InstallContext& context);
void install_foundation_gaps(InstallContext& context);
void install_appkit_gaps(InstallContext& context);
void install_graphics_gaps(InstallContext& context);
// The former system_gaps.cpp, one piece per framework, installed in this order:
void install_coreservices_launch_services(InstallContext& context);   // darwin/coreservices/launch_services.cpp
void install_security_cssm_and_tasks(InstallContext& context);   // darwin/security/cssm_and_tasks.cpp
void install_securityinterface(InstallContext& context);   // darwin/securityinterface/install.cpp
void install_systemconfiguration_errors_and_aliases(InstallContext& context);   // darwin/systemconfiguration/errors_and_aliases.cpp
void install_opendirectory_sessions(InstallContext& context);   // darwin/opendirectory/sessions.cpp
void install_objc_exception_types(InstallContext& context);   // objc/exception_types.cpp

} // namespace alchemy::gui::fill
