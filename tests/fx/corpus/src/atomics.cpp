// Atomics. Built twice for ARM64: default (LDXR/STXR loops) and
// -march=armv8.1-a (LSE). Both builds must print the same as x64.
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>

#include <atomic>
#include <thread>
#include <vector>

template <typename T>
static void single_thread_ops(const char* name) {
    std::atomic<T> a{static_cast<T>(250)};
    unsigned long long r[12];
    r[0] = a.fetch_add(10);           // wraps for 8-bit
    r[1] = a.load();
    r[2] = a.fetch_sub(static_cast<T>(300));
    r[3] = a.fetch_and(static_cast<T>(0x5a5a5a5a5a5a5a5aull));
    r[4] = a.fetch_or(static_cast<T>(0x8001));
    r[5] = a.fetch_xor(static_cast<T>(0xffff));
    r[6] = a.exchange(static_cast<T>(77));
    T expected = static_cast<T>(76);
    bool ok1 = a.compare_exchange_strong(expected, static_cast<T>(1));
    r[7] = expected;
    bool ok2 = a.compare_exchange_strong(expected, static_cast<T>(99));
    r[8] = a.load(std::memory_order_acquire);
    T e = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(e, static_cast<T>(e * 3 + 1), std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
    r[9] = a.load();
    a.store(static_cast<T>(-1), std::memory_order_release);
    r[10] = a.load(std::memory_order_seq_cst);
    unsigned long long pre_inc = ++a;
    unsigned long long post_inc = a++;
    unsigned long long pre_dec = --a;
    r[11] = pre_inc + post_inc * 3 + pre_dec * 5;
    printf("%s:", name);
    for (auto v : r) printf(" %llx", v);
    printf(" cas=%d,%d lock_free=%d\n", ok1, ok2, a.is_lock_free());
}

static void interlocked_ops() {
    volatile LONG l = 10;
    volatile LONG64 q = 1000;
    volatile SHORT s = 5;
    volatile char c = 3;
    unsigned long long r[16];
    r[0] = (unsigned)InterlockedIncrement(&l);
    r[1] = (unsigned)InterlockedDecrement(&l);
    r[2] = (unsigned)InterlockedExchangeAdd(&l, 32);
    r[3] = (unsigned)InterlockedCompareExchange(&l, 7, 42);
    r[4] = (unsigned)InterlockedExchange(&l, 0x0ff0);
    r[5] = (unsigned)InterlockedAnd(&l, 0x00ff);
    r[6] = (unsigned)InterlockedOr(&l, 0x1000);
    r[7] = (unsigned)InterlockedXor(&l, 0x1111);
    r[8] = (unsigned long long)InterlockedIncrement64(&q);
    r[9] = (unsigned long long)InterlockedExchangeAdd64(&q, -2001);
    r[10] = (unsigned long long)InterlockedCompareExchange64(&q, 5, -1000);
    r[11] = (unsigned short)InterlockedIncrement16(&s);
    r[12] = (unsigned short)InterlockedCompareExchange16(&s, 9, 6);
    r[13] = (unsigned char)_InterlockedExchange8(&c, 8);
    r[14] = (unsigned char)_InterlockedOr8(&c, 0x41);
    r[15] = (unsigned long long)l + (unsigned long long)q + (unsigned)s + (unsigned char)c;
    printf("interlocked:");
    for (auto v : r) printf(" %llx", v);
    printf("\n");

    void* volatile ptr = nullptr;
    void* old = InterlockedCompareExchangePointer(&ptr, (void*)&l, nullptr);
    void* old2 = InterlockedExchangePointer(&ptr, nullptr);
    printf("pointer cas old-null=%d exchanged=%d final-null=%d\n", old == nullptr, old2 == (void*)&l, ptr == nullptr);

    alignas(16) volatile LONG64 pair[2] = {1, 2};
    LONG64 comparand[2] = {1, 2};
    unsigned char ok = InterlockedCompareExchange128(pair, 20, 10, comparand);
    LONG64 comparand2[2] = {1, 2};
    unsigned char ok2 = InterlockedCompareExchange128(pair, 40, 30, comparand2);
    printf("cas128 ok=%d pair=%lld,%lld ok2=%d observed=%lld,%lld\n", ok, pair[0], pair[1], ok2, comparand2[0],
           comparand2[1]);

    LONG bits = 0;
    unsigned char b1 = InterlockedBitTestAndSet(&bits, 5);
    unsigned char b2 = InterlockedBitTestAndSet(&bits, 5);
    unsigned char b3 = InterlockedBitTestAndReset(&bits, 5);
    printf("bittest %d %d %d bits=%ld\n", b1, b2, b3, bits);
}

static void contention() {
    std::atomic<uint64_t> c64{0};
    std::atomic<uint32_t> c32{0};
    std::atomic<uint16_t> c16{0};
    std::atomic<uint8_t> c8{0};
    std::atomic<uint64_t> maxv{0};
    std::atomic<uint64_t> orbits{0};
    volatile LONG legacy = 0;
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    long long guarded = 0;
    const int kThreads = 4, kIters = 100000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kIters; i++) {
                c64.fetch_add(3, std::memory_order_relaxed);
                c32.fetch_add(1);
                c16.fetch_sub(1);
                c8++;
                InterlockedIncrement(&legacy);
                uint64_t candidate = (uint64_t)(i * 7919 + t * 104729) % 1000003;
                uint64_t cur = maxv.load(std::memory_order_relaxed);
                while (candidate > cur && !maxv.compare_exchange_weak(cur, candidate)) {
                }
                if ((i & 63) == 0) orbits.fetch_or(1ull << ((i / 64 + t) & 63));
                if ((i & 7) == 0) {
                    while (flag.test_and_set(std::memory_order_acquire)) {
                    }
                    guarded++;
                    flag.clear(std::memory_order_release);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    printf("contention c64=%llu c32=%u c16=%u c8=%u legacy=%ld max=%llu or=%llx guarded=%lld\n",
           (unsigned long long)c64.load(), c32.load(), (unsigned)c16.load(), (unsigned)c8.load(), legacy,
           (unsigned long long)maxv.load(), (unsigned long long)orbits.load(), guarded);
}

struct Pair {
    uint64_t lo, hi;
};

static void wide_atomic() {
    std::atomic<Pair> p{Pair{0, 0}};
    std::vector<std::thread> threads;
    for (int t = 0; t < 2; t++) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20000; i++) {
                Pair cur = p.load();
                Pair next;
                do {
                    next = Pair{cur.lo + 1, cur.hi + 2};
                } while (!p.compare_exchange_weak(cur, next));
            }
        });
    }
    for (auto& th : threads) th.join();
    Pair v = p.load();
    printf("atomic<16 bytes> lo=%llu hi=%llu\n", (unsigned long long)v.lo, (unsigned long long)v.hi);

    std::atomic<double> d{0.0};
    std::vector<std::thread> adders;
    for (int t = 0; t < 4; t++) {
        adders.emplace_back([&] {
            for (int i = 0; i < 10000; i++) d.fetch_add(0.5);
        });
    }
    for (auto& th : adders) th.join();
    printf("atomic<double> sum=%.1f\n", d.load());
}

// Message passing with release/acquire, and store->load ordering with
// seq_cst (Dekker). x64 TSO needs a full barrier for the latter.
static void ordering() {
    const int kRounds = 20000;
    std::atomic<int> flag{0};
    int payload[4] = {0, 0, 0, 0};
    long long bad_mp = 0;
    std::thread producer([&] {
        for (int r = 1; r <= kRounds; r++) {
            while (flag.load(std::memory_order_acquire) != 0) {
            }
            payload[0] = r;
            payload[1] = r * 2;
            payload[2] = r * 3;
            payload[3] = r * 4;
            flag.store(r, std::memory_order_release);
        }
    });
    for (int r = 1; r <= kRounds; r++) {
        int seen;
        while ((seen = flag.load(std::memory_order_acquire)) == 0) {
        }
        if (seen != r || payload[0] != r || payload[1] != 2 * r || payload[2] != 3 * r || payload[3] != 4 * r) bad_mp++;
        flag.store(0, std::memory_order_release);
    }
    producer.join();
    printf("message passing rounds=%d bad=%lld\n", kRounds, bad_mp);

    std::atomic<int> x{0}, y{0};
    std::atomic<int> go{0}, arrived{0};
    int r1 = 0, r2 = 0;
    long long both_zero = 0;
    const int kDekker = 20000;
    std::thread other([&] {
        for (int i = 1; i <= kDekker; i++) {
            while (go.load(std::memory_order_acquire) != i) {
            }
            y.store(1, std::memory_order_seq_cst);
            r2 = x.load(std::memory_order_seq_cst);
            arrived.fetch_add(1, std::memory_order_acq_rel);
        }
    });
    for (int i = 1; i <= kDekker; i++) {
        x.store(0);
        y.store(0);
        go.store(i, std::memory_order_release);
        x.store(1, std::memory_order_seq_cst);
        r1 = y.load(std::memory_order_seq_cst);
        while (arrived.load(std::memory_order_acquire) != i) {
        }
        if (r1 == 0 && r2 == 0) both_zero++;
    }
    other.join();
    printf("dekker rounds=%d both-zero=%lld\n", kDekker, both_zero);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

int main() {
    single_thread_ops<uint8_t>("u8");
    single_thread_ops<uint16_t>("u16");
    single_thread_ops<uint32_t>("u32");
    single_thread_ops<uint64_t>("u64");
    interlocked_ops();
    contention();
    wide_atomic();
    ordering();
    fflush(stdout);
    return 0;
}
