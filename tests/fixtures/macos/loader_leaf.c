int loader_value = 7;
volatile int loader_leaf_initializations;
extern void* malloc(unsigned long);

__attribute__((constructor)) static void initialize_leaf(void) {
    ++loader_leaf_initializations;
}

int loader_leaf(int value) {
    return value + loader_value;
}

int* loader_allocate(int value) {
    int* buffer = (int*)malloc(2 * sizeof(int));
    if (buffer) { buffer[0] = value; buffer[1] = 0; }
    return buffer;
}
