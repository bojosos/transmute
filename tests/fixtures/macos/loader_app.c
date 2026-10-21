typedef unsigned long size_t;
extern int printf(const char*, ...);
extern void* dlopen(const char*, int);
extern void* dlsym(void*, const char*);
extern int dlclose(void*);
extern const char* dlerror(void);
struct DlInfo { const char* image; void* base; const char* name; void* symbol; };
extern int dladdr(const void*, struct DlInfo*);
extern int loader_bridge(int);
extern int* loader_buffer(int);
extern void free(void*);

int executable_callback(int value) {
    return value * 2;
}

int main(int argc, char** argv) {
    if (argc != 2 || loader_bridge(5) != 12) return 1;
    int* buffer = loader_buffer(13);
    if (!buffer || buffer[0] != 20 || buffer[1] != 27) return 16;
    free(buffer);
    void* plugin = dlopen(argv[1], 2 | 4);
    if (!plugin) return 2;
    int (*run)(int) = (int (*)(int))dlsym(plugin, "loader_plugin_run");
    if (!run || run(5) != 25) return 3;
    struct DlInfo info;
    if (!dladdr((const void*)run, &info) || !info.base || info.symbol != (void*)run) return 4;
    int* value = (int*)dlsym(plugin, "loader_value");
    if (!value || *value != 7) return 5;
    int* count = (int*)dlsym(plugin, "loader_leaf_initializations");
    if (!count || *count != 1) return 6;
    if (dlsym(plugin, "does_not_exist") || !dlerror() || dlerror()) return 7;
    if (dlclose(plugin)) return 8;
    plugin = dlopen(argv[1], 2 | 4);
    int* initializations = (int*)dlsym(plugin, "loader_plugin_initializations");
    if (!initializations || *initializations != 1) return 9;
    if (dlclose(plugin) || !dlclose(plugin)) return 10;
    plugin = dlopen(argv[1], 2 | 8);
    if (!plugin || !dlsym((void*)-2L, "loader_plugin_run")) return 11;
    if (dlsym((void*)-1L, "loader_plugin_run") || dlsym((void*)-3L, "loader_plugin_run")) return 12;
    if (!dlsym((void*)-1L, "loader_bridge") || !dlsym((void*)-3L, "loader_value")) return 13;
    if (!dlsym((void*)-5L, "executable_callback") || dlsym((void*)-5L, "loader_value")) return 14;
    void* process = dlopen((const char*)0, 2);
    if (!process || !dlsym(process, "loader_plugin_run") || dlclose(process) || dlclose(plugin)) return 15;
    printf("Plugin: %d\n", run(5));
    return 0;
}
