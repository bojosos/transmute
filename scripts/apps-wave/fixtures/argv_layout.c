/* Prints where the kernel-provided argv/envp strings live, to check Darwin's contiguous ascending layout. */
int printf(const char *format, ...);
int main(int argc, char **argv, char **envp) {
    for (int i = 0; i < argc; i++) printf("argv[%d] %p\n", i, (void *)argv[i]);
    printf("envp[0] %p\n", (void *)envp[0]);
    return 0;
}
