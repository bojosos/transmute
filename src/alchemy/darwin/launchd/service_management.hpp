#pragma once

// ServiceManagement's job calls over the session launchd (alchemy-fx only):
// SMJobSubmit, SMJobRemove, SMJobCopyDictionary and SMCopyAllJobDictionaries
// in the user domain. The system domain is refused (kSMErrorAuthorizationFailure):
// Alchemy runs no system launchd. SMJobBless and SMAppService stay in
// darwin/servicemanagement/.

#include "alchemy/darwin/kit/completion/completion_kit.hpp"

namespace alchemy::gui::completion_kit {
void install_launchd_jobs(Support& support);
} // namespace alchemy::gui::completion_kit
