// fx corpus ARM64 CRT: console EXE entry point for wmain().
#include <stdlib.h>

#include "crt_internal.h"

int wmain(int argc, wchar_t** argv, wchar_t** envp);

int wmainCRTStartup(void) {
    __security_init_cookie();
    __fx_initialize_stdio_options();
    _set_app_type(_crt_console_app);
    if (_configure_wide_argv(_crt_argv_unexpanded_arguments) != 0) return 255;
    if (_initialize_wide_environment() != 0) return 255;
    if (_initterm_e(__xi_a, __xi_z) != 0) return 255;
    _initterm(__xc_a, __xc_z);
    __fx_dyn_tls_init_current_thread();
    exit(wmain(*__p___argc(), *__p___wargv(), _get_initial_wide_environment()));
}
