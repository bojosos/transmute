#include "alchemy/darwin/libsystem/install.hpp"

#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

namespace alchemy::libsystem {

void install_libsystem_gaps(gui::InstallContext& context) {
    export_fill::Provider p(context.process);
    export_fill::install_libsystem_net(p);
    export_fill::install_libsystem_sysv(p);
    export_fill::install_libsystem_ipc(p);
    export_fill::install_libsystem_process(p);
    export_fill::install_libsystem_runtime(p);
    export_fill::install_libsystem_misc(p);
    export_fill::install_libsystem_stdlib(p);
    export_fill::install_libsystem_ndbm(p);
    export_fill::install_libsystem_rpc_nis(p);
    export_fill::install_libedit_readline(p);
    export_fill::install_ncurses(p);
    export_fill::install_libpanel(p);
    export_fill::install_libsystem_threads_extra(p);
    export_fill::install_libsystem_bsd_extra(p);
    export_fill::install_libsystem_netdb_extra(p);
    export_fill::install_libsystem_mach_extra(p);
}

void install_libsystem_services(gui::InstallContext& context) {
    export_fill::Provider p(context.process);
    export_fill::install_libsystem_xpc(p, context.objc);
    export_fill::install_libsystem_services(p, context.objc);
}

void install_mach(gui::InstallContext& context) {
    install_mach_ipc(context);
    install_notify(context);
}

} // namespace alchemy::libsystem
