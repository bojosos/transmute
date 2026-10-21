extern int puts(const char *);
extern int printf(const char *, ...);

static int equal(const char *left, const char *right) {
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}

int main(int argc, char **argv, char **envp, char **apple) {
    if (argc != 3 || !argv[0] || !equal(argv[1], "first") ||
        !equal(argv[2], "second") || argv[3]) return 21;
    if (!envp || !envp[0] || !equal(envp[0], "ALCHEMY_TEST=present") || envp[1]) return 22;
    if (!apple || !apple[0] || apple[1]) return 23;
    const char *key = "executable_path=";
    const char *value = apple[0];
    while (*key && *key == *value) { ++key; ++value; }
    if (*key || !equal(value, argv[0])) return 24;
    puts(argv[1]);
    printf("argc %d %% %s\n", argc, argv[2]);
    return 7;
}
