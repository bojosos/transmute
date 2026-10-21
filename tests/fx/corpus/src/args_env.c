// argv, environment and command line. The harness passes fixed arguments and
// sets FX_CORPUS_VAR / FX_CORPUS_OTHER. Exits with 7.
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_strings(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static const char* base_name(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    return base;
}

// Skips argv[0] in the raw command line the way the CRT does.
static const char* command_tail(const char* line) {
    if (*line == '"') {
        ++line;
        while (*line && *line != '"') ++line;
        if (*line) ++line;
    } else {
        while (*line && *line != ' ' && *line != '\t') ++line;
    }
    while (*line == ' ' || *line == '\t') ++line;
    return line;
}

int main(int argc, char** argv, char** envp) {
    printf("argc=%d\n", argc);
    char name[MAX_PATH];
    strncpy(name, base_name(argv[0]), sizeof name - 1);
    name[sizeof name - 1] = 0;
    for (char* p = name; *p; ++p) *p = (char)tolower((unsigned char)*p);
    printf("argv[0] base=%s\n", name);
    for (int i = 1; i < argc; i++) printf("argv[%d]=[%s] len=%zu\n", i, argv[i], strlen(argv[i]));
    printf("argv[argc]=%s\n", argv[argc] ? "non-null" : "null");
    printf("__argc=%d same-argv=%d\n", __argc, __argv == argv);
    printf("tail=[%s]\n", command_tail(GetCommandLineA()));

    const char* var = getenv("FX_CORPUS_VAR");
    printf("getenv FX_CORPUS_VAR=%s\n", var ? var : "(null)");
    var = getenv("FX_CORPUS_MISSING");
    printf("getenv FX_CORPUS_MISSING=%s\n", var ? var : "(null)");

    const char* found[64];
    int count = 0;
    for (char** e = envp; *e && count < 64; ++e) {
        if (strncmp(*e, "FX_CORPUS_", 10) == 0) found[count++] = *e;
    }
    qsort(found, (size_t)count, sizeof found[0], compare_strings);
    printf("envp FX_CORPUS_* count=%d\n", count);
    for (int i = 0; i < count; i++) printf("  %s\n", found[i]);

    char value[256];
    DWORD len = GetEnvironmentVariableA("FX_CORPUS_OTHER", value, sizeof value);
    printf("GetEnvironmentVariableA FX_CORPUS_OTHER=%s len=%lu\n", len ? value : "(none)", len);
    len = GetEnvironmentVariableA("FX_CORPUS_MISSING", value, sizeof value);
    printf("GetEnvironmentVariableA missing len=%lu error=%lu\n", len, GetLastError());

    _putenv_s("FX_CORPUS_SET", "set-by-guest");
    var = getenv("FX_CORPUS_SET");
    printf("after _putenv_s getenv=%s\n", var ? var : "(null)");
    len = GetEnvironmentVariableA("FX_CORPUS_SET", value, sizeof value);
    printf("after _putenv_s GetEnvironmentVariableA=%s\n", len ? value : "(none)");

    SetEnvironmentVariableA("FX_CORPUS_OS", "os-level");
    len = GetEnvironmentVariableA("FX_CORPUS_OS", value, sizeof value);
    printf("SetEnvironmentVariableA then Get=%s\n", len ? value : "(none)");

    char expanded[256];
    ExpandEnvironmentStringsA("[%FX_CORPUS_VAR%]", expanded, sizeof expanded);
    printf("ExpandEnvironmentStringsA=%s\n", expanded);
    fflush(stdout);
    return 7;
}
