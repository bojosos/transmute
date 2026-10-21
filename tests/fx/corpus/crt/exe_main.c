// fx corpus ARM64 CRT: console EXE entry point for main().
#include <stdlib.h>

#include "crt_internal.h"

int main(int argc, char** argv, char** envp);

int mainCRTStartup(void) {
    __security_init_cookie();
    __fx_initialize_stdio_options();
    _set_app_type(_crt_console_app);
    if (_configure_narrow_argv(_crt_argv_unexpanded_arguments) != 0) return 255;
    if (_initialize_narrow_environment() != 0) return 255;
    if (_initterm_e(__xi_a, __xi_z) != 0) return 255;
    _initterm(__xc_a, __xc_z);
    __fx_dyn_tls_init_current_thread();
    exit(main(*__p___argc(), *__p___argv(), _get_initial_narrow_environment()));
}
