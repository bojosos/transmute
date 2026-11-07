/* Hello world for the toolchain checks. No SDK headers: declarations are written here. */
int printf(const char *format, ...);
int main(int argc, char **argv) {
    printf("hello from clang, argc=%d, argv[0]=%s\n", argc, argv[0]);
    return 0;
}
