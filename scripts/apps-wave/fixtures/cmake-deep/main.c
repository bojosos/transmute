int printf(const char *format, ...);
int atoi(const char *text);
int util_sum_squares(int count);
int main(int argc, char **argv) {
    if (argc < 2) return 1;
    printf("sum=%d\n", util_sum_squares(atoi(argv[1])));
    return 0;
}
