// fx corpus ARM64 CRT: DLL entry point. atexit in a DLL registers with a
// module-local table that runs at DLL_PROCESS_DETACH.
#include <windows.h>
#include <stdlib.h>

#include "crt_internal.h"

static _onexit_table_t fx_module_atexit_table;

int __cdecl atexit(_PVFV func) {
    return _register_onexit_function(&fx_module_atexit_table, (_onexit_t)func);
}

_onexit_t __cdecl _onexit(_onexit_t func) {
    return _register_onexit_function(&fx_module_atexit_table, func) == 0 ? func : NULL;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved);

BOOL WINAPI fx_default_DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
#pragma comment(linker, "/alternatename:DllMain=fx_default_DllMain")

BOOL WINAPI _DllMainCRTStartup(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        __security_init_cookie();
        __fx_initialize_stdio_options();
        if (_initialize_onexit_table(&fx_module_atexit_table) != 0) return FALSE;
        if (_initterm_e(__xi_a, __xi_z) != 0) return FALSE;
        _initterm(__xc_a, __xc_z);
        __fx_dyn_tls_init_current_thread();
    }
    BOOL result = DllMain(instance, reason, reserved);
    if (reason == DLL_PROCESS_ATTACH && !result) {
        _execute_onexit_table(&fx_module_atexit_table);
    } else if (reason == DLL_PROCESS_DETACH) {
        _execute_onexit_table(&fx_module_atexit_table);
    }
    return result;
}
