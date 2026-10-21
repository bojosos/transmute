__declspec(dllexport) int guest_failure_probe(void) {
    return 17;
}

int DllMain(void* module, unsigned long reason, void* reserved) {
    (void)module;
    (void)reserved;
    return reason != 1; // Reject DLL_PROCESS_ATTACH.
}
