static volatile int initialized;

__attribute__((constructor)) static void initialize(void) { initialized = 31; }

int helper_value(void) { return initialized == 31 ? 42 : -1; }
