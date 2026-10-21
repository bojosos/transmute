extern int loader_leaf(int);
extern int loader_value;
extern int* loader_allocate(int);

int loader_bridge(int value) {
    return loader_leaf(value);
}

int* loader_buffer(int value) {
    int* buffer = loader_allocate(value);
    if (buffer) {
        buffer[0] += loader_value;
        buffer[1] = loader_leaf(buffer[0]);
    }
    return buffer;
}
