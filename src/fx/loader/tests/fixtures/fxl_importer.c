// Imports by name, by ordinal, data, forwarded (guest/host/ordinal/chained),
// circular DLLs, a TLS DLL, host kernel32, a missing function, a missing DLL
// and a delay-loaded DLL. The EXE has its own TLS and a TLS callback.
#include "fxl_tlssup.h"

__declspec(dllimport) int named_one(void);
__declspec(dllimport) int by_ordinal(void);
__declspec(dllimport) extern int data_value;
__declspec(dllimport) int fwd_leaf(void);
__declspec(dllimport) unsigned long fwd_host(void);
__declspec(dllimport) int fwd_ord(void);
__declspec(dllimport) int fwd_chain(void);
__declspec(dllimport) int cycle_a_calls_b(void);
__declspec(dllimport) int tls_get_value(void);
__declspec(dllimport) unsigned long GetCurrentProcessId(void);
__declspec(dllimport) unsigned long TransmuteFxMissingFunction(void);
__declspec(dllimport) int fxl_missing_function(void);
__declspec(dllimport) void* ResolveDelayLoadedAPI(void* parent, const void* descriptor,
                                                  void* failure_dll_hook, void* failure_system_hook,
                                                  void* thunk, unsigned long flags);
__declspec(dllimport) int delay_value(void);

extern char __ImageBase;

// The delay-load helper the linker's delay thunks call.
void* __delayLoadHelper2(const void* descriptor, void** iat_slot) {
    return ResolveDelayLoadedAPI(&__ImageBase, descriptor, 0, 0, iat_slot, 0);
}

__declspec(thread) int exe_tls = 77;

__declspec(dllexport) volatile unsigned long exe_events[16];
__declspec(dllexport) volatile long exe_event_count;

static void exe_tls_callback(void* module, unsigned long reason, void* reserved) {
    (void)module;
    (void)reserved;
    long i = exe_event_count;
    if (i < 16) {
        exe_events[i] = reason;
        exe_event_count = i + 1;
    }
}
FXL_TLS_CALLBACK(exe_tls_callback);

__declspec(dllexport) int fxl_sum(void) {
    return named_one() + by_ordinal() + data_value + fwd_leaf() + fwd_ord() + fwd_chain() +
           cycle_a_calls_b() + exe_tls;
}

__declspec(dllexport) unsigned long fxl_host_pid(void) { return GetCurrentProcessId() + fwd_host(); }

__declspec(dllexport) unsigned long fxl_call_missing(void) {
    return TransmuteFxMissingFunction() + (unsigned long)fxl_missing_function();
}

__declspec(dllexport) int fxl_tls_value(void) { return tls_get_value(); }

__declspec(dllexport) int fxl_call_delay(void) { return delay_value(); }

int fxl_entry(void) { return fxl_sum(); }
