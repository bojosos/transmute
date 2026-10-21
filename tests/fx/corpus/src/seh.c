// Structured exception handling: __try/__except/__finally, hardware faults,
// RaiseException, unwinding through frames, vectored handlers.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char trace[512];
static void mark(const char* s) {
    strncat(trace, s, sizeof trace - strlen(trace) - 1);
}

static volatile int* volatile null_ptr = 0;
static volatile int sink;

typedef struct {
    DWORD code;
    DWORD flags;
    DWORD params;
    ULONG_PTR info[4];
} Captured;

static int capture(EXCEPTION_POINTERS* ep, Captured* out, int result) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    out->code = r->ExceptionCode;
    // Newer Windows versions also set EXCEPTION_SOFTWARE_ORIGINATE (0x80).
    out->flags = r->ExceptionFlags & EXCEPTION_NONCONTINUABLE;
    out->params = r->NumberParameters;
    for (DWORD i = 0; i < 4; i++) out->info[i] = i < r->NumberParameters ? r->ExceptionInformation[i] : 0;
    return result;
}

static void test_access_violation(void) {
    Captured c;
    memset(&c, 0, sizeof c);
    __try {
        sink = *null_ptr;
        printf("not reached\n");
    } __except (capture(GetExceptionInformation(), &c, EXCEPTION_EXECUTE_HANDLER)) {
        printf("read AV code=%08lx params=%lu write=%llu addr=%llx\n", c.code, c.params,
               (unsigned long long)c.info[0], (unsigned long long)c.info[1]);
    }
    __try {
        *(volatile int*)(uintptr_t)0x10 = 5;
    } __except (capture(GetExceptionInformation(), &c, EXCEPTION_EXECUTE_HANDLER)) {
        printf("write AV code=%08lx write=%llu addr=%llx\n", c.code, (unsigned long long)c.info[0],
               (unsigned long long)c.info[1]);
    }

    char* page = (char*)VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    page[100] = 1;
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_READONLY, &old);
    __try {
        page[200] = 2;
    } __except (capture(GetExceptionInformation(), &c, EXCEPTION_EXECUTE_HANDLER)) {
        printf("readonly write code=%08lx write=%llu offset=%lld value-intact=%d\n", c.code,
               (unsigned long long)c.info[0], (long long)(c.info[1] - (ULONG_PTR)page), page[200] == 0);
    }
    VirtualProtect(page, 4096, PAGE_READWRITE | PAGE_GUARD, &old);
    __try {
        sink = page[300];
    } __except (capture(GetExceptionInformation(), &c, EXCEPTION_EXECUTE_HANDLER)) {
        printf("guard page code=%08lx offset=%lld\n", c.code, (long long)(c.info[1] - (ULONG_PTR)page));
    }
    page[300] = 3; // guard is one-shot
    printf("after guard write=%d\n", page[300]);
    VirtualProtect(page + 4096, 4096, PAGE_NOACCESS, &old);
    __try {
        sink = page[4096 + 8];
    } __except (capture(GetExceptionInformation(), &c, EXCEPTION_EXECUTE_HANDLER)) {
        printf("noaccess code=%08lx offset=%lld\n", c.code, (long long)(c.info[1] - (ULONG_PTR)page));
    }
    VirtualFree(page, 0, MEM_RELEASE);
}

static void test_raise(void) {
    Captured c;
    ULONG_PTR args[3] = {0x1111, 0x2222, 0x3333};
    __try {
        RaiseException(0xE0000001, 0, 3, args);
    } __except (capture(GetExceptionInformation(), &c, EXCEPTION_EXECUTE_HANDLER)) {
        printf("raise code=%08lx flags=%lu params=%lu %llx %llx %llx\n", c.code, c.flags, c.params,
               (unsigned long long)c.info[0], (unsigned long long)c.info[1], (unsigned long long)c.info[2]);
    }
    __try {
        RaiseException(0xE0000002, EXCEPTION_NONCONTINUABLE, 0, NULL);
    } __except (capture(GetExceptionInformation(), &c, EXCEPTION_EXECUTE_HANDLER)) {
        printf("noncontinuable flags=%lu\n", c.flags);
    }
    // A filter returning EXCEPTION_CONTINUE_EXECUTION resumes after RaiseException.
    int resumed = 0;
    __try {
        RaiseException(0xE0000003, 0, 0, NULL);
        resumed = 1;
    } __except (EXCEPTION_CONTINUE_EXECUTION) {
        resumed = -1;
    }
    printf("continue execution resumed=%d\n", resumed);
    __try {
        __debugbreak();
    } __except (capture(GetExceptionInformation(), &c, EXCEPTION_EXECUTE_HANDLER)) {
        printf("breakpoint code=%08lx\n", c.code);
    }
}

static int filter_code(DWORD code, DWORD wanted, const char* tag) {
    mark(tag);
    return code == wanted ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

__declspec(noinline) static void inner_raise(int depth) {
    __try {
        if (depth == 0) {
            mark("raise;");
            RaiseException(0xE0000010, 0, 0, NULL);
        }
        inner_raise(depth - 1);
    } __finally {
        char buf[32];
        snprintf(buf, sizeof buf, "fin%d%s;", depth, AbnormalTermination() ? "!" : "");
        mark(buf);
    }
}

static void test_unwind(void) {
    trace[0] = 0;
    __try {
        __try {
            inner_raise(3);
        } __except (filter_code(GetExceptionCode(), 0xE0000099, "f-inner;")) {
            mark("h-inner;");
        }
    } __except (filter_code(GetExceptionCode(), 0xE0000010, "f-outer;")) {
        mark("h-outer;");
    }
    printf("unwind trace=%s\n", trace);

    trace[0] = 0;
    __try {
        mark("body;");
    } __finally {
        mark(AbnormalTermination() ? "fin-abnormal;" : "fin-normal;");
    }
    __try {
        mark("before-leave;");
        __leave;
        mark("not-reached;");
    } __finally {
        mark(AbnormalTermination() ? "leave-abnormal;" : "leave-normal;");
    }
    printf("finally trace=%s\n", trace);

    // Exception raised inside a handler is caught by an outer handler.
    trace[0] = 0;
    __try {
        __try {
            sink = *null_ptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mark("handler1;");
            RaiseException(0xE0000020, 0, 0, NULL);
        }
    } __except (filter_code(GetExceptionCode(), 0xE0000020, "f2;")) {
        mark("handler2;");
    }
    printf("nested trace=%s\n", trace);

    int count = 0;
    for (int i = 0; i < 1000; i++) {
        __try {
            if (i % 3 == 0) RaiseException(0xE0000030, 0, 0, NULL);
            if (i % 3 == 1) sink = *null_ptr;
            count += 100;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            count += GetExceptionCode() == 0xE0000030 ? 1 : 2;
        }
    }
    printf("loop count=%d\n", count);
}

static volatile LONG veh_hits;
static volatile LONG veh_other;
static LONG CALLBACK vectored(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == 0xE0000040) {
        InterlockedIncrement(&veh_hits);
    } else {
        InterlockedIncrement(&veh_other);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG CALLBACK vectored_resume(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == 0xE0000041) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

static void test_vectored(void) {
    PVOID h = AddVectoredExceptionHandler(1, vectored);
    for (int i = 0; i < 5; i++) {
        __try {
            RaiseException(0xE0000040, 0, 0, NULL);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    printf("vectored hits=%ld remove=%lu\n", veh_hits, RemoveVectoredExceptionHandler(h));
    __try {
        RaiseException(0xE0000040, 0, 0, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    printf("vectored hits after remove=%ld\n", veh_hits);

    PVOID r = AddVectoredExceptionHandler(0, vectored_resume);
    int reached = 0;
    __try {
        RaiseException(0xE0000041, 0, 0, NULL);
        reached = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        reached = -1;
    }
    RemoveVectoredExceptionHandler(r);
    printf("vectored resume reached=%d\n", reached);
}

static LONG WINAPI top_level(EXCEPTION_POINTERS* ep) {
    (void)ep;
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(void) {
    test_access_violation();
    test_raise();
    test_unwind();
    test_vectored();
    LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(top_level);
    LPTOP_LEVEL_EXCEPTION_FILTER mine = SetUnhandledExceptionFilter(previous);
    printf("unhandled filter roundtrip=%d\n", mine == top_level);
    fflush(stdout);
    return 0;
}
