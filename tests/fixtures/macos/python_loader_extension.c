extern int python_loader_constructor_count;
extern int python_loader_add(int, int);

__attribute__((constructor)) static void initialize(void) {
    ++python_loader_constructor_count;
}

/* This authored fixture tests symbol spelling and loading, not the PyObject ABI. */
unsigned long PyInit_alchemy_loader(void) {
    return (unsigned long)python_loader_add(20, 7);
}

int python_loader_extension(int value, int (*callback)(int, int)) {
    return callback(value, python_loader_constructor_count);
}
