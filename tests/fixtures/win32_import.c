__declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);

unsigned long guest_entry(void) {
    volatile unsigned long process_id = GetCurrentProcessId();
    return process_id;
}
