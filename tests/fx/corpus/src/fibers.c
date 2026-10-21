// Fibers: ConvertThreadToFiber, CreateFiber, SwitchToFiber ping-pong with
// stack-resident state, fiber data, FLS per fiber, DeleteFiber.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static LPVOID main_fiber;
static LPVOID workers[2];
static char trace[512];
static DWORD fls_index;
static volatile LONG fls_callbacks;

static void mark(const char* s) {
    strncat(trace, s, sizeof trace - strlen(trace) - 1);
}

static void WINAPI fls_cleanup(void* data) {
    if (data) InterlockedIncrement(&fls_callbacks);
}

static void WINAPI fiber_proc(void* param) {
    int id = (int)(INT_PTR)param;
    volatile long long local = id * 100; // lives on this fiber's stack
    FlsSetValue(fls_index, (void*)(INT_PTR)(id + 1));
    for (int round = 0; round < 3; round++) {
        char buf[48];
        snprintf(buf, sizeof buf, "f%d:%lld:%d;", id, local, (int)(INT_PTR)GetFiberData());
        mark(buf);
        local += 1;
        SwitchToFiber(id == 0 ? workers[1] : main_fiber);
    }
    char buf[32];
    snprintf(buf, sizeof buf, "f%d-done:%d;", id, (int)(INT_PTR)FlsGetValue(fls_index) - 1);
    mark(buf);
    SwitchToFiber(main_fiber);
}

int main(void) {
    fls_index = FlsAlloc(fls_cleanup);
    main_fiber = ConvertThreadToFiber((void*)(INT_PTR)99);
    printf("converted=%d is_fiber=%d data=%d\n", main_fiber != NULL, IsThreadAFiber(),
           (int)(INT_PTR)GetFiberData());
    FlsSetValue(fls_index, (void*)(INT_PTR)50);
    workers[0] = CreateFiber(64 * 1024, fiber_proc, (void*)(INT_PTR)0);
    workers[1] = CreateFiber(0, fiber_proc, (void*)(INT_PTR)1);
    for (int round = 0; round < 3; round++) {
        mark("m;");
        SwitchToFiber(workers[0]);
    }
    mark("m-final;");
    SwitchToFiber(workers[0]);
    SwitchToFiber(workers[1]);
    printf("trace=%s\n", trace);
    printf("main fls=%d current-is-main=%d\n", (int)(INT_PTR)FlsGetValue(fls_index), GetCurrentFiber() == main_fiber);
    DeleteFiber(workers[0]);
    DeleteFiber(workers[1]);
    printf("fls callbacks after DeleteFiber=%ld\n", fls_callbacks);
    BOOL converted_back = ConvertFiberToThread();
    printf("ConvertFiberToThread=%d is_fiber=%d\n", converted_back != 0, IsThreadAFiber());
    fflush(stdout);
    return 0;
}
