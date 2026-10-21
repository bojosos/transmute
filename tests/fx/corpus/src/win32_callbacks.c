// Win32 APIs that call back into guest code: thread pool work/timers/waits,
// timer queues, QueueUserWorkItem, RegisterWaitForSingleObject, APCs,
// InitOnceExecuteOnce, EnumSystemCodePages-free choices.
#include <windows.h>
#include <stdio.h>

static volatile LONG work_count;
static volatile LONG work_sum;

static void CALLBACK work_callback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work) {
    (void)instance;
    (void)work;
    InterlockedIncrement(&work_count);
    InterlockedExchangeAdd(&work_sum, (LONG)(INT_PTR)context);
}

static void CALLBACK simple_callback(PTP_CALLBACK_INSTANCE instance, PVOID context) {
    (void)instance;
    HANDLE semaphore = (HANDLE)context;
    InterlockedIncrement(&work_count);
    ReleaseSemaphore(semaphore, 1, NULL);
}

typedef struct {
    HANDLE event;
    volatile LONG calls;
    volatile LONG flag;
} Signal;

static void CALLBACK tp_timer_callback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_TIMER timer) {
    (void)instance;
    (void)timer;
    Signal* s = (Signal*)context;
    InterlockedIncrement(&s->calls);
    SetEvent(s->event);
}

static void CALLBACK tp_wait_callback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WAIT wait,
                                      TP_WAIT_RESULT result) {
    (void)instance;
    (void)wait;
    Signal* s = (Signal*)context;
    s->flag = (LONG)result;
    InterlockedIncrement(&s->calls);
    SetEvent(s->event);
}

static void CALLBACK queue_timer_callback(PVOID context, BOOLEAN fired) {
    Signal* s = (Signal*)context;
    s->flag = fired;
    InterlockedIncrement(&s->calls);
    SetEvent(s->event);
}

static DWORD WINAPI work_item(PVOID context) {
    Signal* s = (Signal*)context;
    InterlockedIncrement(&s->calls);
    SetEvent(s->event);
    return 0;
}

static void CALLBACK registered_wait(PVOID context, BOOLEAN timed_out) {
    Signal* s = (Signal*)context;
    s->flag = timed_out;
    InterlockedIncrement(&s->calls);
    SetEvent(s->event);
}

static volatile LONG apc_value;
static void CALLBACK apc_routine(ULONG_PTR data) {
    InterlockedExchangeAdd(&apc_value, (LONG)data);
}

static volatile LONG init_once_calls;
static BOOL CALLBACK init_once_callback(PINIT_ONCE once, PVOID parameter, PVOID* context) {
    (void)once;
    InterlockedIncrement(&init_once_calls);
    // The low INIT_ONCE_CTX_RESERVED_BITS of the context must be zero.
    *context = (PVOID)((INT_PTR)parameter << INIT_ONCE_CTX_RESERVED_BITS);
    return TRUE;
}

static void reset(Signal* s) {
    ResetEvent(s->event);
    s->calls = 0;
    s->flag = -1;
}

int main(void) {
    PTP_WORK work = CreateThreadpoolWork(work_callback, (PVOID)(INT_PTR)7, NULL);
    for (int i = 0; i < 10; i++) SubmitThreadpoolWork(work);
    WaitForThreadpoolWorkCallbacks(work, FALSE);
    CloseThreadpoolWork(work);
    printf("threadpool work count=%ld sum=%ld\n", work_count, work_sum);

    work_count = 0;
    HANDLE semaphore = CreateSemaphoreW(NULL, 0, 100, NULL);
    for (int i = 0; i < 5; i++) TrySubmitThreadpoolCallback(simple_callback, semaphore, NULL);
    for (int i = 0; i < 5; i++) WaitForSingleObject(semaphore, INFINITE);
    printf("TrySubmitThreadpoolCallback count=%ld\n", work_count);
    CloseHandle(semaphore);

    Signal s;
    s.event = CreateEventW(NULL, TRUE, FALSE, NULL);

    reset(&s);
    PTP_TIMER timer = CreateThreadpoolTimer(tp_timer_callback, &s, NULL);
    FILETIME due;
    ULARGE_INTEGER relative;
    relative.QuadPart = (ULONGLONG)(-(LONGLONG)(10 * 10000)); // 10 ms
    due.dwLowDateTime = relative.LowPart;
    due.dwHighDateTime = relative.HighPart;
    SetThreadpoolTimer(timer, &due, 0, 0);
    DWORD wait = WaitForSingleObject(s.event, 10000);
    WaitForThreadpoolTimerCallbacks(timer, TRUE);
    CloseThreadpoolTimer(timer);
    printf("threadpool timer wait=%lu calls=%ld\n", wait, s.calls);

    reset(&s);
    HANDLE trigger = CreateEventW(NULL, FALSE, FALSE, NULL);
    PTP_WAIT tp_wait = CreateThreadpoolWait(tp_wait_callback, &s, NULL);
    SetThreadpoolWait(tp_wait, trigger, NULL);
    SetEvent(trigger);
    wait = WaitForSingleObject(s.event, 10000);
    WaitForThreadpoolWaitCallbacks(tp_wait, FALSE);
    CloseThreadpoolWait(tp_wait);
    printf("threadpool wait wait=%lu calls=%ld result=%ld\n", wait, s.calls, s.flag);

    reset(&s);
    HANDLE queue_timer = NULL;
    CreateTimerQueueTimer(&queue_timer, NULL, queue_timer_callback, &s, 10, 0, WT_EXECUTEONLYONCE);
    wait = WaitForSingleObject(s.event, 10000);
    DeleteTimerQueueTimer(NULL, queue_timer, INVALID_HANDLE_VALUE);
    printf("timer queue wait=%lu calls=%ld fired=%ld\n", wait, s.calls, s.flag);

    reset(&s);
    HANDLE timer_queue = CreateTimerQueue();
    HANDLE periodic = NULL;
    CreateTimerQueueTimer(&periodic, timer_queue, queue_timer_callback, &s, 5, 5, WT_EXECUTEDEFAULT);
    while (s.calls < 3) Sleep(1);
    DeleteTimerQueueTimer(timer_queue, periodic, INVALID_HANDLE_VALUE);
    LONG periodic_calls = s.calls;
    Sleep(30);
    printf("periodic timer at-least-3=%d stopped=%d\n", periodic_calls >= 3, s.calls == periodic_calls);
    DeleteTimerQueueEx(timer_queue, INVALID_HANDLE_VALUE);

    reset(&s);
    QueueUserWorkItem(work_item, &s, WT_EXECUTEDEFAULT);
    wait = WaitForSingleObject(s.event, 10000);
    printf("QueueUserWorkItem wait=%lu calls=%ld\n", wait, s.calls);

    reset(&s);
    HANDLE registered = NULL;
    RegisterWaitForSingleObject(&registered, trigger, registered_wait, &s, INFINITE, WT_EXECUTEONLYONCE);
    SetEvent(trigger);
    wait = WaitForSingleObject(s.event, 10000);
    UnregisterWaitEx(registered, INVALID_HANDLE_VALUE);
    printf("RegisterWaitForSingleObject wait=%lu calls=%ld timed_out=%ld\n", wait, s.calls, s.flag);

    reset(&s);
    RegisterWaitForSingleObject(&registered, trigger, registered_wait, &s, 15, WT_EXECUTEONLYONCE);
    wait = WaitForSingleObject(s.event, 10000);
    UnregisterWaitEx(registered, INVALID_HANDLE_VALUE);
    printf("RegisterWaitForSingleObject timeout wait=%lu calls=%ld timed_out=%ld\n", wait, s.calls, s.flag);

    QueueUserAPC(apc_routine, GetCurrentThread(), 40);
    QueueUserAPC(apc_routine, GetCurrentThread(), 2);
    DWORD sleep_result = SleepEx(0, TRUE);
    printf("APC value=%ld SleepEx=%lu\n", apc_value, sleep_result);

    INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    PVOID context1 = NULL, context2 = NULL;
    BOOL ok1 = InitOnceExecuteOnce(&once, init_once_callback, (PVOID)21, &context1);
    BOOL ok2 = InitOnceExecuteOnce(&once, init_once_callback, (PVOID)99, &context2);
    printf("InitOnce calls=%ld ok=%d,%d context=%lld,%lld\n", init_once_calls, ok1, ok2, (long long)(INT_PTR)context1,
           (long long)(INT_PTR)context2);

    CloseHandle(trigger);
    CloseHandle(s.event);
    fflush(stdout);
    return 0;
}
