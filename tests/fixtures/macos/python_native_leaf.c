/* Authored guest dependency used by the CPython extension workload. */
static int initialized;
__attribute__((constructor)) static void initialize(void) { initialized = 27; }
long alchemy_python_add(long value) { return value + initialized; }
int alchemy_python_initialized(void) { return initialized; }
