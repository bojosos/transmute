// LoadLibrary/GetProcAddress: system DLL entry points called through
// pointers (including varargs and callbacks), a guest DLL loaded and unloaded
// at run time, exports of the EXE itself, and the failure paths.
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "fxcorpus_dll.h"

__declspec(dllexport) int fx_exe_export(int v) {
    return v * 3 + 1;
}

static const char* base_name(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    return base;
}

static int __cdecl compare_desc(const void* a, const void* b) {
    return *(const int*)b - *(const int*)a;
}

static int twice(int value, void* context) {
    (void)context;
    return value * 2;
}

int main(void) {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    printf("kernel32 handle=%d\n", k32 != NULL);
    typedef DWORD(WINAPI * PidFn)(void);
    PidFn pid = (PidFn)GetProcAddress(k32, "GetCurrentProcessId");
    printf("GetCurrentProcessId matches=%d\n", pid && pid() == GetCurrentProcessId());
    typedef int(WINAPI * LenFn)(LPCSTR);
    LenFn len = (LenFn)GetProcAddress(k32, "lstrlenA");
    printf("lstrlenA=%d\n", len ? len("dynamic") : -1);
    typedef int(WINAPI * MulDivFn)(int, int, int);
    MulDivFn muldiv = (MulDivFn)GetProcAddress(k32, "MulDiv");
    printf("MulDiv=%d\n", muldiv ? muldiv(100, 3, 7) : -1);

    HMODULE user32 = LoadLibraryA("user32.dll");
    typedef int(WINAPIV * WsprintfFn)(LPSTR, LPCSTR, ...);
    WsprintfFn wsp = (WsprintfFn)GetProcAddress(user32, "wsprintfA");
    char buffer[128];
    int n = wsp ? wsp(buffer, "%d-%s-%x-%c-%lu", 42, "str", 255, 'z', 4000000000ul) : -1;
    printf("wsprintfA=%d [%s]\n", n, n > 0 ? buffer : "");
    typedef LPSTR(WINAPI * CharUpperFn)(LPSTR);
    CharUpperFn upper = (CharUpperFn)GetProcAddress(user32, "CharUpperA");
    strcpy(buffer, "mixed Case");
    if (upper) upper(buffer);
    printf("CharUpperA=%s\n", buffer);

    HMODULE crt = LoadLibraryA("api-ms-win-crt-utility-l1-1-0.dll");
    typedef void(__cdecl * QsortFn)(void*, size_t, size_t, int(__cdecl*)(const void*, const void*));
    QsortFn qs = (QsortFn)GetProcAddress(crt, "qsort");
    int values[8] = {5, 1, 7, 3, 8, 2, 6, 4};
    if (qs) qs(values, 8, sizeof values[0], compare_desc);
    printf("dynamic qsort found=%d:", qs != NULL);
    for (int i = 0; i < 8; i++) printf(" %d", values[i]);
    printf("\n");

    SetLastError(0);
    HMODULE missing = LoadLibraryA("fx_no_such_library.dll");
    printf("missing dll null=%d error=%lu\n", missing == NULL, GetLastError());
    FARPROC missing_proc = GetProcAddress(k32, "FxNoSuchExport");
    printf("missing export null=%d error=%lu\n", missing_proc == NULL, GetLastError());

    printf("guest dll loaded before=%d\n", GetModuleHandleA("fxcorpus_dll.dll") != NULL);
    HMODULE dll = LoadLibraryA("fxcorpus_dll.dll");
    printf("LoadLibrary guest dll=%d\n", dll != NULL);
    if (dll) {
        char path[MAX_PATH];
        GetModuleFileNameA(dll, path, MAX_PATH);
        printf("module file=%s\n", base_name(path));
        typedef int (*AddFn)(int, int);
        typedef int (*ApplyFn)(FxCallback, void*, int);
        typedef int (*VsumFn)(int, ...);
        typedef const char* (*NameFn)(void);
        AddFn add = (AddFn)GetProcAddress(dll, "fxdll_add");
        ApplyFn apply = (ApplyFn)GetProcAddress(dll, "fxdll_apply");
        VsumFn vsum = (VsumFn)GetProcAddress(dll, "fxdll_vsum");
        NameFn name = (NameFn)GetProcAddress(dll, MAKEINTRESOURCEA(8));
        int* counter = (int*)GetProcAddress(dll, "fxdll_counter");
        long* attach = (long*)GetProcAddress(dll, "fxdll_process_attach");
        int added = add(20, 22);
        int applied = apply(twice, NULL, 5);
        int summed = vsum(4, 1, 1, 1, 1);
        printf("exports add=%d apply=%d vsum=%d name=%s counter=%d attach=%ld\n", added, applied, summed, name(),
               *counter, *attach);
        HMODULE again = LoadLibraryA("fxcorpus_dll.dll");
        printf("second LoadLibrary same=%d\n", again == dll);
        FreeLibrary(again);
        printf("still loaded=%d\n", GetModuleHandleA("fxcorpus_dll.dll") != NULL);
        FreeLibrary(dll);
        printf("unloaded=%d\n", GetModuleHandleA("fxcorpus_dll.dll") == NULL);
        dll = LoadLibraryExA("fxcorpus_dll.dll", NULL, 0);
        counter = (int*)GetProcAddress(dll, "fxdll_counter");
        printf("reloaded counter reset=%d\n", counter && *counter == 1000);
        FreeLibrary(dll);
    }

    HMODULE self = GetModuleHandleA(NULL);
    typedef int (*SelfFn)(int);
    SelfFn self_fn = (SelfFn)GetProcAddress(self, "fx_exe_export");
    printf("exe export=%d same=%d\n", self_fn ? self_fn(5) : -1, self_fn == fx_exe_export);
    HMODULE from_address = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void*)main, &from_address);
    printf("module from address is exe=%d\n", from_address == self);
    fflush(stdout);
    return 0;
}
