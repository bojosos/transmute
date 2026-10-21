// Threads and synchronization: _beginthreadex, CreateThread, std::thread,
// CRITICAL_SECTION, SRWLOCK, CONDITION_VARIABLE, events, semaphores,
// WaitOnAddress, std::mutex/condition_variable/shared_mutex.
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

static unsigned __stdcall range_sum(void* arg) {
    uintptr_t index = reinterpret_cast<uintptr_t>(arg);
    unsigned sum = 0;
    for (unsigned i = 0; i < 100000; i++) sum += (i ^ static_cast<unsigned>(index)) & 0xff;
    return sum;
}

static void test_beginthreadex() {
    HANDLE handles[4];
    for (uintptr_t i = 0; i < 4; i++) {
        handles[i] = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, range_sum, reinterpret_cast<void*>(i), 0, nullptr));
    }
    DWORD wait = WaitForMultipleObjects(4, handles, TRUE, INFINITE);
    printf("_beginthreadex wait=%lu", wait);
    for (int i = 0; i < 4; i++) {
        DWORD code = 0;
        GetExitCodeThread(handles[i], &code);
        printf(" %lu", code);
        CloseHandle(handles[i]);
    }
    printf("\n");
}

struct CsShared {
    CRITICAL_SECTION cs;
    long long counter;
    long long pairs_a, pairs_b;
};

static DWORD WINAPI cs_worker(void* arg) {
    CsShared* s = static_cast<CsShared*>(arg);
    for (int i = 0; i < 100000; i++) {
        EnterCriticalSection(&s->cs);
        s->counter++;
        s->pairs_a++;
        s->pairs_b += 2;
        LeaveCriticalSection(&s->cs);
        if ((i & 1023) == 0 && TryEnterCriticalSection(&s->cs)) {
            s->counter++;
            LeaveCriticalSection(&s->cs);
            EnterCriticalSection(&s->cs);
            s->counter--;
            LeaveCriticalSection(&s->cs);
        }
    }
    return 0;
}

static void test_critical_section() {
    CsShared s{};
    InitializeCriticalSectionAndSpinCount(&s.cs, 1000);
    HANDLE threads[4];
    for (auto& t : threads) t = CreateThread(nullptr, 0, cs_worker, &s, 0, nullptr);
    WaitForMultipleObjects(4, threads, TRUE, INFINITE);
    for (auto t : threads) CloseHandle(t);
    DeleteCriticalSection(&s.cs);
    printf("critical section counter=%lld consistent=%d\n", s.counter, s.pairs_b == 2 * s.pairs_a);
}

struct SrwShared {
    SRWLOCK lock = SRWLOCK_INIT;
    long long a = 0, b = 0;
    std::atomic<long long> inconsistent{0};
};

static void srw_writer(SrwShared* s) {
    for (int i = 0; i < 50000; i++) {
        AcquireSRWLockExclusive(&s->lock);
        s->a++;
        s->b--;
        ReleaseSRWLockExclusive(&s->lock);
    }
}

static void srw_reader(SrwShared* s) {
    for (int i = 0; i < 50000; i++) {
        AcquireSRWLockShared(&s->lock);
        if (s->a + s->b != 0) s->inconsistent++;
        ReleaseSRWLockShared(&s->lock);
    }
}

static void test_srwlock() {
    SrwShared s;
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; i++) threads.emplace_back(srw_writer, &s);
    for (int i = 0; i < 2; i++) threads.emplace_back(srw_reader, &s);
    for (auto& t : threads) t.join();
    BOOLEAN acquired = TryAcquireSRWLockExclusive(&s.lock);
    if (acquired) ReleaseSRWLockExclusive(&s.lock);
    printf("srwlock a=%lld b=%lld inconsistent=%lld try=%d\n", s.a, s.b, s.inconsistent.load(), acquired != 0);
}

// Bounded queue on CONDITION_VARIABLE + CRITICAL_SECTION (and SRW for the
// consumers' result lock).
struct Queue {
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE not_empty, not_full;
    int items[16];
    int head = 0, count = 0;
    bool done = false;
    SRWLOCK result_lock = SRWLOCK_INIT;
    long long consumed_sum = 0, consumed_count = 0;
};

static DWORD WINAPI producer(void* arg) {
    Queue* q = static_cast<Queue*>(arg);
    for (int i = 1; i <= 20000; i++) {
        EnterCriticalSection(&q->cs);
        while (q->count == 16) SleepConditionVariableCS(&q->not_full, &q->cs, INFINITE);
        q->items[(q->head + q->count) % 16] = i;
        q->count++;
        LeaveCriticalSection(&q->cs);
        WakeConditionVariable(&q->not_empty);
    }
    return 0;
}

static DWORD WINAPI consumer(void* arg) {
    Queue* q = static_cast<Queue*>(arg);
    long long sum = 0, count = 0;
    for (;;) {
        EnterCriticalSection(&q->cs);
        while (q->count == 0 && !q->done) SleepConditionVariableCS(&q->not_empty, &q->cs, INFINITE);
        if (q->count == 0 && q->done) {
            LeaveCriticalSection(&q->cs);
            break;
        }
        int v = q->items[q->head];
        q->head = (q->head + 1) % 16;
        q->count--;
        LeaveCriticalSection(&q->cs);
        WakeConditionVariable(&q->not_full);
        sum += v;
        count++;
    }
    AcquireSRWLockExclusive(&q->result_lock);
    q->consumed_sum += sum;
    q->consumed_count += count;
    ReleaseSRWLockExclusive(&q->result_lock);
    return 0;
}

static void test_condition_variable() {
    Queue q;
    InitializeCriticalSection(&q.cs);
    InitializeConditionVariable(&q.not_empty);
    InitializeConditionVariable(&q.not_full);
    HANDLE producers[2], consumers[3];
    for (auto& h : consumers) h = CreateThread(nullptr, 0, consumer, &q, 0, nullptr);
    for (auto& h : producers) h = CreateThread(nullptr, 0, producer, &q, 0, nullptr);
    WaitForMultipleObjects(2, producers, TRUE, INFINITE);
    EnterCriticalSection(&q.cs);
    q.done = true;
    LeaveCriticalSection(&q.cs);
    WakeAllConditionVariable(&q.not_empty);
    WaitForMultipleObjects(3, consumers, TRUE, INFINITE);
    for (auto h : producers) CloseHandle(h);
    for (auto h : consumers) CloseHandle(h);
    DeleteCriticalSection(&q.cs);
    printf("condition variable consumed=%lld sum=%lld\n", q.consumed_count, q.consumed_sum);
}

static void test_std_sync() {
    std::mutex m;
    std::condition_variable cv;
    std::deque<int> queue;
    bool done = false;
    long long sum = 0, count = 0;
    std::vector<std::thread> consumers;
    for (int c = 0; c < 3; c++) {
        consumers.emplace_back([&] {
            long long local_sum = 0, local_count = 0;
            for (;;) {
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, [&] { return !queue.empty() || done; });
                if (queue.empty()) break;
                local_sum += queue.front();
                local_count++;
                queue.pop_front();
            }
            std::lock_guard<std::mutex> guard(m);
            sum += local_sum;
            count += local_count;
        });
    }
    std::vector<std::thread> producers;
    for (int p = 0; p < 2; p++) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < 20000; i++) {
                {
                    std::lock_guard<std::mutex> lock(m);
                    queue.push_back(i * 2 + p);
                }
                cv.notify_one();
            }
        });
    }
    for (auto& t : producers) t.join();
    {
        std::lock_guard<std::mutex> lock(m);
        done = true;
    }
    cv.notify_all();
    for (auto& t : consumers) t.join();
    printf("std::condition_variable consumed=%lld sum=%lld\n", count, sum);

    std::recursive_mutex rm;
    int depth = 0;
    std::function<void(int)> recurse = [&](int n) {
        std::lock_guard<std::recursive_mutex> lock(rm);
        depth++;
        if (n > 0) recurse(n - 1);
    };
    recurse(10);
    std::timed_mutex tm;
    bool locked = tm.try_lock_for(std::chrono::milliseconds(10));
    if (locked) tm.unlock();
    printf("recursive depth=%d timed try_lock=%d\n", depth, locked);

    std::shared_mutex sm;
    long long value = 0;
    std::atomic<int> torn{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; i++) {
        threads.emplace_back([&] {
            for (int k = 0; k < 20000; k++) {
                std::unique_lock<std::shared_mutex> lock(sm);
                value += 3;
            }
        });
        threads.emplace_back([&] {
            for (int k = 0; k < 20000; k++) {
                std::shared_lock<std::shared_mutex> lock(sm);
                if (value % 3 != 0) torn++;
            }
        });
    }
    for (auto& t : threads) t.join();
    printf("shared_mutex value=%lld torn=%d\n", value, torn.load());

    std::once_flag once;
    std::atomic<int> once_calls{0};
    std::vector<std::thread> once_threads;
    for (int i = 0; i < 4; i++) once_threads.emplace_back([&] { std::call_once(once, [&] { once_calls++; }); });
    for (auto& t : once_threads) t.join();
    printf("call_once calls=%d\n", once_calls.load());

    std::thread::id main_id = std::this_thread::get_id();
    bool different = false;
    std::thread([&] { different = std::this_thread::get_id() != main_id; }).join();
    printf("thread id differs=%d hardware_concurrency>0=%d\n", different, std::thread::hardware_concurrency() > 0);
}

struct PingPong {
    HANDLE ping, pong;
    int rounds;
};

static DWORD WINAPI pong_thread(void* arg) {
    PingPong* p = static_cast<PingPong*>(arg);
    for (int i = 0; i < 2000; i++) {
        WaitForSingleObject(p->ping, INFINITE);
        p->rounds++;
        SetEvent(p->pong);
    }
    return 0;
}

static void test_events_semaphores() {
    PingPong p{CreateEventW(nullptr, FALSE, FALSE, nullptr), CreateEventW(nullptr, FALSE, FALSE, nullptr), 0};
    HANDLE t = CreateThread(nullptr, 0, pong_thread, &p, 0, nullptr);
    for (int i = 0; i < 2000; i++) {
        SetEvent(p.ping);
        WaitForSingleObject(p.pong, INFINITE);
    }
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    printf("event ping-pong rounds=%d\n", p.rounds);

    HANDLE manual = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    printf("manual event before=%lu", WaitForSingleObject(manual, 0));
    SetEvent(manual);
    printf(" after=%lu again=%lu", WaitForSingleObject(manual, 0), WaitForSingleObject(manual, 0));
    ResetEvent(manual);
    printf(" reset=%lu\n", WaitForSingleObject(manual, 0));
    CloseHandle(manual);

    HANDLE sem = CreateSemaphoreW(nullptr, 2, 5, nullptr);
    DWORD r1 = WaitForSingleObject(sem, 0), r2 = WaitForSingleObject(sem, 0), r3 = WaitForSingleObject(sem, 0);
    LONG previous = -1;
    ReleaseSemaphore(sem, 3, &previous);
    BOOL over = ReleaseSemaphore(sem, 5, nullptr);
    printf("semaphore waits=%lu,%lu,%lu previous=%ld over-release=%d error=%lu\n", r1, r2, r3, previous, over != 0,
           GetLastError());
    CloseHandle(sem);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, nullptr);
    DWORD recursive = WaitForSingleObject(mutex, 0);
    ReleaseMutex(mutex);
    ReleaseMutex(mutex);
    BOOL extra = ReleaseMutex(mutex);
    printf("mutex recursive=%lu extra-release=%d error=%lu\n", recursive, extra != 0, GetLastError());
    CloseHandle(mutex);
}

static volatile LONG address_value = 0;
static DWORD WINAPI address_waker(void*) {
    for (int i = 1; i <= 100; i++) {
        InterlockedExchange(&address_value, i);
        WakeByAddressSingle((PVOID)&address_value);
        while (InterlockedCompareExchange(&address_value, 0, 0) == i) {
            Sleep(0);
        }
    }
    return 0;
}

static void test_wait_on_address() {
    HANDLE t = CreateThread(nullptr, 0, address_waker, nullptr, 0, nullptr);
    long long seen = 0;
    for (int i = 1; i <= 100; i++) {
        LONG undesired = 0;
        while (InterlockedCompareExchange(&address_value, 0, 0) == 0) {
            WaitOnAddress(&address_value, &undesired, sizeof undesired, 10);
        }
        seen += address_value;
        InterlockedExchange(&address_value, 0);
    }
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    printf("WaitOnAddress seen=%lld\n", seen);
}

static void test_thread_states() {
    HANDLE t = CreateThread(nullptr, 0, [](void*) -> DWORD { return 1234; }, nullptr, CREATE_SUSPENDED, nullptr);
    DWORD before = 0;
    GetExitCodeThread(t, &before);
    DWORD suspended_wait = WaitForSingleObject(t, 20);
    DWORD resume = ResumeThread(t);
    WaitForSingleObject(t, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    CloseHandle(t);
    printf("suspended still-active=%d wait=%lu resume=%lu exit=%lu\n", before == STILL_ACTIVE, suspended_wait, resume,
           code);

    std::atomic<int> sleepers{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; i++) {
        threads.emplace_back([&, i] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1 + i));
            std::this_thread::yield();
            SwitchToThread();
            sleepers++;
        });
    }
    for (auto& th : threads) th.join();
    printf("sleepers=%d\n", sleepers.load());
}

int main() {
    test_beginthreadex();
    test_critical_section();
    test_srwlock();
    test_condition_variable();
    test_std_sync();
    test_events_semaphores();
    test_wait_on_address();
    test_thread_states();
    fflush(stdout);
    return 0;
}
