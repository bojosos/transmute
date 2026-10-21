#include "alchemy/darwin_providers.hpp"

#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_mach_vm.hpp"
#include "alchemy/darwin_native.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/perf.hpp"
#include "alchemy/process_context.hpp"
#include "alchemy/registration_audit.hpp"

namespace alchemy {
namespace {

// The perf phases (ALCHEMY_PERF, with ALCHEMY_PERF_MEMORY the memory at each)
// split the installation's time and memory by provider.
bool install_core(ProcessContext& context) {
    perf::phase("providers: libSystem services");
    install_darwin_mach_vm(context);
    install_darwin_native(context);
    perf::phase("providers: Mach VM and libSystem natives");
    install_darwin_numeric(context);
    perf::phase("providers: numeric");
    install_darwin_dispatch(context);
    perf::phase("providers: libdispatch");
    return true;
}

bool install_frameworks(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    perf::phase("providers: threads, libobjc, CoreFoundation, Foundation bridge");
    install_darwin_frameworks(context, objc, cf);
    perf::phase("providers: headless frameworks");
#ifdef ALCHEMY_WITH_GUI
    // alchemy-fx GUI providers (include/alchemy/gui/install.hpp).
    install_darwin_gui(context, objc, cf);
#endif
    registration_audit::report(context);
    return true;
}

} // namespace

DarwinProviders::DarwinProviders(ProcessContext& context)
    : services(context),
      core_installed_(install_core(context)),
      threads(context),
      objc(context),
      cf(context),
      foundation(context, objc, cf),
      frameworks_installed_(install_frameworks(context, objc, cf)) {}

} // namespace alchemy
