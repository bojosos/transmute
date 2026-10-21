extern int loader_bridge(int);
extern int executable_callback(int);
extern void* dlopen(const char*, int);
extern int dlclose(void*);
volatile int loader_plugin_initializations;
volatile int loader_plugin_nested_error;

__attribute__((constructor)) static void initialize_plugin(void) {
    ++loader_plugin_initializations;
    void* self = dlopen("@loader_path/loader_plugin.bundle", 2 | 4);
    if (!self || dlclose(self)) loader_plugin_nested_error = 1;
}

int loader_plugin_run(int value) {
    return executable_callback(loader_bridge(value)) + loader_plugin_initializations + loader_plugin_nested_error;
}
