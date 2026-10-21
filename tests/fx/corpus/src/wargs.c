// wmain entry: wide argv/environment, printed as UTF-16 code units so the
// output stays ASCII.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static void print_wide(const wchar_t* s) {
    for (; *s; ++s) {
        if (*s >= 0x20 && *s < 0x7f) {
            putchar((char)*s);
        } else {
            printf("\\u%04x", (unsigned)*s);
        }
    }
}

int wmain(int argc, wchar_t** argv, wchar_t** envp) {
    printf("argc=%d\n", argc);
    for (int i = 1; i < argc; i++) {
        printf("argv[%d]=[", i);
        print_wide(argv[i]);
        printf("] len=%zu\n", wcslen(argv[i]));
    }
    const wchar_t* var = _wgetenv(L"FX_CORPUS_WIDE");
    printf("_wgetenv FX_CORPUS_WIDE=[");
    print_wide(var ? var : L"(null)");
    printf("]\n");
    int count = 0;
    for (wchar_t** e = envp; *e; ++e) {
        if (wcsncmp(*e, L"FX_CORPUS_WIDE=", 15) == 0) count++;
    }
    printf("envp FX_CORPUS_WIDE entries=%d\n", count);
    char utf8[64];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, argc > 2 ? argv[2] : L"", -1, utf8, sizeof utf8, NULL, NULL);
    printf("utf8 bytes=%d:", bytes);
    for (int i = 0; i + 1 < bytes; i++) printf(" %02x", (unsigned char)utf8[i]);
    printf("\n");
    wchar_t buffer[64];
    swprintf(buffer, 64, L"%d-%ls-%.2f", 12, L"w", 0.5);
    printf("swprintf=[");
    print_wide(buffer);
    printf("]\n");
    fflush(stdout);
    return argc;
}
