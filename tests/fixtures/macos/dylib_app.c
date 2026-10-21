extern int helper_value(void);
extern int printf(const char *, ...);

int main(void) {
    const int value = helper_value();
    printf("dylib %d\n", value);
    return value == 42 ? 0 : 12;
}
