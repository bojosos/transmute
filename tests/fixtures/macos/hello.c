extern int printf(const char *, ...);

int main(void) {
    return printf("%s %d\n", "Hello World from MacOS:", 27) == 27 ? 0 : 11;
}
