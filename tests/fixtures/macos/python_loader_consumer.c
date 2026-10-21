extern int python_loader_add(int, int);
extern int python_loader_extension(int, int (*)(int, int));

int python_loader_consumer(void) {
    return python_loader_extension(40, python_loader_add);
}
