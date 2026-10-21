__declspec(dllimport) int guest_leaf_answer(void);
__declspec(dllexport) int guest_bias = 7;
static volatile int* volatile pointer_to_bias = &guest_bias;

int DllMain(void* module, unsigned long reason, void* reserved) {
    (void)module;
    (void)reserved;
    guest_bias = (int)reason - 1;
    return 1;
}

__declspec(dllexport) int guest_answer(void) {
    return guest_leaf_answer() + *pointer_to_bias;
}
