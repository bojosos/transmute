// fx corpus ARM64 CRT: declarations shared by the startup objects.
#pragma once

#include <corecrt_startup.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

extern _PIFV __xi_a[];
extern _PIFV __xi_z[];
extern _PVFV __xc_a[];
extern _PVFV __xc_z[];

void __cdecl __security_init_cookie(void);
// Selects standard printf rounding for this module (as the MSVC startup does).
void __fx_initialize_stdio_options(void);
// Runs the thread_local dynamic initializers for the calling thread (the
// loader only calls the TLS callbacks with DLL_THREAD_ATTACH for new threads).
void __fx_dyn_tls_init_current_thread(void);

#ifdef __cplusplus
}
#endif
