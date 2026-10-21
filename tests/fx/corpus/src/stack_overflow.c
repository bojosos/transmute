// Stack overflow caught with __except on a thread with a small stack, twice
// (the guard page is restored with _resetstkoflw), plus large frames that go
// through __chkstk.
#include <windows.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

static volatile int sink;

__declspec(noinline) static int recurse(int depth) {
    volatile char frame[512];
    frame[0] = (char)depth;
    frame[511] = (char)(depth >> 8);
    sink = frame[0];
    return recurse(depth + 1) + frame[511];
}

__declspec(noinline) static int big_frame(int seed) {
    volatile char buffer[64 * 1024];
    for (int i = 0; i < (int)sizeof buffer; i += 4096) buffer[i] = (char)(seed + i);
    int sum = 0;
    for (int i = 0; i < (int)sizeof buffer; i += 4096) sum += buffer[i];
    return sum;
}

static DWORD WINAPI overflow_thread(void* arg) {
    (void)arg;
    int caught = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        __try {
            recurse(0);
        } __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER
                                                                   : EXCEPTION_CONTINUE_SEARCH) {
            caught++;
            int reset = _resetstkoflw();
            printf("attempt %d caught stack overflow reset=%d\n", attempt, reset != 0);
        }
    }
    printf("big frame sum=%d\n", big_frame(3));
    char* dynamic = (char*)_alloca(20000);
    memset(dynamic, 1, 20000);
    printf("alloca sum=%d\n", dynamic[0] + dynamic[19999]);
    return (DWORD)caught;
}

int main(void) {
    HANDLE t = CreateThread(NULL, 256 * 1024, overflow_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    WaitForSingleObject(t, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    printf("thread exit=%lu\n", code);
    printf("main big frame sum=%d\n", big_frame(5));
    fflush(stdout);
    return 0;
}
