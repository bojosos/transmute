// Thread-local storage: __declspec(thread), thread_local with dynamic
// initialization and destructors, TlsAlloc (including expansion slots) and
// FlsAlloc callbacks.
#include <windows.h>
#include <stdio.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

__declspec(thread) int tls_counter = 100;
__declspec(thread) char tls_text[32] = "initial";
__declspec(thread) long long tls_zero;

static std::atomic<int> string_inits{0};
static std::atomic<int> trackers_constructed{0};
static std::atomic<int> trackers_destroyed{0};
static std::atomic<int> fls_callbacks{0};

static std::string make_string() {
    string_inits++;
    return std::string("dynamic-") + std::to_string(41 + 1);
}

struct Tracker {
    int value;
    Tracker() : value(5) { trackers_constructed++; }
    ~Tracker() { trackers_destroyed++; }
};

thread_local std::string tls_string = make_string();
thread_local Tracker tls_tracker;
thread_local int tls_plain = 7;

static DWORD tls_index;
static DWORD fls_index;
static DWORD many_indices[100];

static void WINAPI fls_callback(void* data) {
    if (data) fls_callbacks++;
}

struct Result {
    int counter;
    char text[32];
    long long zero;
    std::string str;
    int tracker;
    int plain;
    uintptr_t tls_value;
    int many_ok;
};

static void worker(int id, Result* out) {
    tls_counter += id;
    snprintf(tls_text, sizeof tls_text, "thread-%d", id);
    tls_zero += id * 1000;
    tls_string += "-" + std::to_string(id);
    tls_tracker.value += id;
    tls_plain *= id + 1;
    TlsSetValue(tls_index, reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + id)));
    FlsSetValue(fls_index, reinterpret_cast<void*>(static_cast<uintptr_t>(id + 1)));
    for (int i = 0; i < 100; i++) TlsSetValue(many_indices[i], reinterpret_cast<void*>(static_cast<uintptr_t>(i * 3 + id)));
    Sleep(5);
    int many_ok = 1;
    for (int i = 0; i < 100; i++) {
        if (reinterpret_cast<uintptr_t>(TlsGetValue(many_indices[i])) != static_cast<uintptr_t>(i * 3 + id)) many_ok = 0;
    }
    out->counter = tls_counter;
    snprintf(out->text, sizeof out->text, "%s", tls_text);
    out->zero = tls_zero;
    out->str = tls_string;
    out->tracker = tls_tracker.value;
    out->plain = tls_plain;
    out->tls_value = reinterpret_cast<uintptr_t>(TlsGetValue(tls_index));
    out->many_ok = many_ok;
}

static DWORD WINAPI win32_worker(void* arg) {
    tls_counter = 555;
    return static_cast<DWORD>(tls_counter + reinterpret_cast<uintptr_t>(arg) + (TlsGetValue(tls_index) == nullptr));
}

int main() {
    tls_index = TlsAlloc();
    fls_index = FlsAlloc(fls_callback);
    int distinct = 1;
    for (int i = 0; i < 100; i++) {
        many_indices[i] = TlsAlloc();
        if (many_indices[i] == TLS_OUT_OF_INDEXES) distinct = 0;
    }
    printf("tls indices allocated ok=%d\n", distinct);
    TlsSetValue(tls_index, reinterpret_cast<void*>(0x77));
    FlsSetValue(fls_index, reinterpret_cast<void*>(0x88));

    printf("main initial counter=%d text=%s zero=%lld string=%s tracker=%d plain=%d\n", tls_counter, tls_text, tls_zero,
           tls_string.c_str(), tls_tracker.value, tls_plain);

    std::vector<Result> results(6);
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; i++) threads.emplace_back(worker, i + 1, &results[i]);
    for (auto& t : threads) t.join();
    for (int i = 0; i < 6; i++) {
        const Result& r = results[i];
        printf("thread %d counter=%d text=%s zero=%lld string=%s tracker=%d plain=%d tls=%llx many=%d\n", i + 1,
               r.counter, r.text, r.zero, r.str.c_str(), r.tracker, r.plain, (unsigned long long)r.tls_value,
               r.many_ok);
    }

    HANDLE h = CreateThread(nullptr, 0, win32_worker, reinterpret_cast<void*>(10), 0, nullptr);
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(h, &code);
    CloseHandle(h);
    printf("win32 thread exit=%lu\n", code);

    printf("main after counter=%d text=%s string=%s tls=%llx fls=%llx\n", tls_counter, tls_text, tls_string.c_str(),
           (unsigned long long)reinterpret_cast<uintptr_t>(TlsGetValue(tls_index)),
           (unsigned long long)reinterpret_cast<uintptr_t>(FlsGetValue(fls_index)));
    // Every thread that touched tls_string/tls_tracker initialized them once;
    // worker threads have exited, so their destructors ran.
    printf("string inits=%d trackers constructed=%d destroyed=%d fls callbacks=%d\n", string_inits.load(),
           trackers_constructed.load(), trackers_destroyed.load(), fls_callbacks.load());

    for (int i = 0; i < 100; i++) TlsFree(many_indices[i]);
    printf("TlsFree=%d FlsFree=%d\n", TlsFree(tls_index) != 0, FlsFree(fls_index) != 0);
    printf("fls callbacks after FlsFree=%d\n", fls_callbacks.load());
    fflush(stdout);
    return 0;
}
