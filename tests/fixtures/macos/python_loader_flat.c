/* A Node/Python-style add-on built with -undefined dynamic_lookup: its import
   of a framework function binds through the flat namespace to a provider
   library that only the executable links (AudioUnit here), as dyld searches
   every loaded image. */
extern void* AudioComponentFindNext(void* component, const void* description);

unsigned long python_loader_flat(void) {
    return AudioComponentFindNext((void*)0, (const void*)0) == (void*)0x2a ? 42 : 1;
}
