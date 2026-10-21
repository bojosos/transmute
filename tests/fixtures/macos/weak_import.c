extern int alchemy_missing_export(void) __attribute__((weak_import));
extern int puts(const char *);
int main(void) {
    if (alchemy_missing_export) return 31;
    puts("weak import null");
    return 0;
}
