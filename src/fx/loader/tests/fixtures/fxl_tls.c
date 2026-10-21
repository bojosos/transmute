// Static TLS with a template, zero fill, a TLS callback and DllMain. Every
// notification is recorded in g_events as (kind << 8) | reason, kind 1 = TLS
// callback, 2 = DllMain.
#define FXL_TLS_ZERO_FILL 32
#include "fxl_tlssup.h"

__declspec(thread) int tls_value = 1234;
__declspec(thread) int tls_counter;

__declspec(dllexport) volatile unsigned long g_events[64];
__declspec(dllexport) volatile long g_event_count;

static void record(unsigned long kind, unsigned long reason) {
    long i = g_event_count;
    if (i < 64) {
        g_events[i] = (kind << 8) | reason;
        g_event_count = i + 1;
    }
}

static void tls_callback(void* module, unsigned long reason, void* reserved) {
    (void)module;
    (void)reserved;
    record(1, reason);
    tls_counter += 1;
}
FXL_TLS_CALLBACK(tls_callback);

int DllMain(void* module, unsigned long reason, void* reserved) {
    (void)module;
    (void)reserved;
    record(2, reason);
    return 1;
}

__declspec(dllexport) int tls_get_value(void) { return tls_value; }
__declspec(dllexport) void tls_set_value(int value) { tls_value = value; }
__declspec(dllexport) int tls_get_counter(void) { return tls_counter; }
