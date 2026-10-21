// Core-type statistics (TRANSMUTE_FX_CORESTATS=<file|1>[,<interval_us>]).
//
// Diagnoses hybrid-CPU scheduling. Every interval a background thread queues
// a special user APC (QueueUserAPC2, delivered even outside alertable waits)
// to each thread of the process that ran for at least a third of the last
// interval; the APC records the logical processor it runs on
// (GetCurrentProcessorNumberEx). GetSystemCpuSetInformation's
// EfficiencyClass tells performance cores (highest class) from efficiency
// cores. The APC only touches its thread's preallocated slot (atomics), so
// it never blocks or allocates on a guest thread. The report (rewritten every
// two seconds and at exit) has per-thread P/E shares, CPU time, priority,
// power-throttling state and description, and a timeline of the busiest
// threads. Guest threads are tagged [guest].
//
// The same file builds a standalone DLL (FX_CORESTATS_STANDALONE) that a
// native process can load (e.g. Blender's Python: ctypes.WinDLL), so native
// and translated runs are measured the same way.
//
// The win32 backend of fx/host/diagnostics.hpp: the runtime passes the guest
// thread ids in (rt::corestats_start_if_requested).

#ifdef FX_CORESTATS_STANDALONE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include "fx/host/diagnostics.hpp"

#include <windows.h>
#endif

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace transmute::fx::host {
namespace {

void (*g_guest_threads)(std::vector<std::uint32_t>& ids) = nullptr;

constexpr unsigned kBucketMs = 250;
constexpr unsigned kBuckets = 2400;   // 10 minutes
constexpr unsigned kMaxProcessors = 64;
constexpr unsigned kMaxSlots = 512;

struct Slot {
    DWORD id = 0;
    HANDLE handle = nullptr;
    bool dead = false;
    bool guest = false;
    ULONG64 last_cycles = 0;
    double final_cpu = -1;
    ULONG context_switches = 0;   // last SystemProcessInformation value
    ULONG wait_reason = 0;
    std::atomic<std::uint32_t> pending{0};
    std::atomic<std::uint64_t> performance{0}, efficiency{0}, migrations{0};
    std::atomic<int> last_processor{-1};
    std::atomic<std::uint32_t> processors[kMaxProcessors] = {};
    std::atomic<std::uint16_t> timeline_p[kBuckets] = {};
    std::atomic<std::uint16_t> timeline_e[kBuckets] = {};
    // Effective core clock probes (sample_apc): sum of MHz and count, by core class.
    std::atomic<std::uint64_t> mhz_sum[2] = {}, mhz_n[2] = {};
};

using NtGetNextThreadFn = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
using QueueUserApc2Fn = BOOL(WINAPI*)(PAPCFUNC, HANDLE, ULONG_PTR, int);
constexpr int kSpecialUserApc = 1;   // QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC

struct CoreStats {
    std::wstring path;
    unsigned interval_us = 1000;
    ULONGLONG start = 0;
    double tsc_hz = 0;
    BYTE classes[kMaxProcessors] = {};
    BYTE top_class = 0;
    std::string topology;
    Slot* slots = nullptr;   // kMaxSlots
    unsigned used = 0;
    DWORD self = 0;
    NtGetNextThreadFn next_thread = nullptr;
    QueueUserApc2Fn queue_apc = nullptr;
    SRWLOCK report_lock = SRWLOCK_INIT;
    bool stopped = false;
    ULONG page_faults = 0;
    std::vector<std::uint8_t> process_info;
};

CoreStats* g_stats = nullptr;

void read_topology(CoreStats& s) {
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<std::uint8_t> buffer(length);
    if (!length || !GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data()), length,
                                               &length, GetCurrentProcess(), 0))
        return;
    char text[64];
    for (ULONG offset = 0; offset < length;) {
        const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (info->Type == CpuSetInformation) {
            const auto& c = info->CpuSet;
            const unsigned index = (c.Group * 64u + c.LogicalProcessorIndex) % kMaxProcessors;
            s.classes[index] = c.EfficiencyClass;
            s.top_class = std::max(s.top_class, c.EfficiencyClass);
            std::snprintf(text, sizeof(text), " %u:%u/c%u", static_cast<unsigned>(c.LogicalProcessorIndex),
                          static_cast<unsigned>(c.EfficiencyClass), static_cast<unsigned>(c.CoreIndex));
            s.topology += text;
        }
        offset += info->Size;
    }
}

double calibrate_tsc() {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    const auto t0 = __rdtsc();
    Sleep(50);
    const auto t1 = __rdtsc();
    QueryPerformanceCounter(&b);
    return static_cast<double>(t1 - t0) * static_cast<double>(f.QuadPart) / static_cast<double>(b.QuadPart - a.QuadPart);
}

// Effective clock of the calling core: a chain of dependent 64-bit multiplies
// (3-cycle latency on both Golden Cove and Gracemont) timed with the TSC.
// About 2 us at 3 GHz; samples interrupted by the scheduler are dropped.
volatile std::uint64_t g_probe_multiplier = 0x9E3779B97F4A7C15ull;
volatile std::uint64_t g_probe_sink;
constexpr unsigned kProbeMultiplies = 2048;
double probe_mhz(double tsc_hz) {
    std::uint64_t x = g_probe_multiplier | 1;
    const std::uint64_t k = g_probe_multiplier;
    _mm_lfence();
    const auto t0 = __rdtsc();
    _mm_lfence();
    for (unsigned i = 0; i < kProbeMultiplies; ++i) x *= k;
    _mm_lfence();
    const auto t1 = __rdtsc();
    g_probe_sink = x;
    if (t1 <= t0) return 0;
    return 3.0 * kProbeMultiplies * tsc_hz / static_cast<double>(t1 - t0) / 1e6;
}

void NTAPI sample_apc(ULONG_PTR parameter) {
    CoreStats& s = *g_stats;
    Slot& slot = *reinterpret_cast<Slot*>(parameter);
    PROCESSOR_NUMBER number{};
    GetCurrentProcessorNumberEx(&number);
    const unsigned index = (number.Group * 64u + number.Number) % kMaxProcessors;
    const bool performance = s.classes[index] == s.top_class;
    if (s.tsc_hz > 0) {
        const double mhz = probe_mhz(s.tsc_hz);
        if (mhz > 200 && mhz < 8000) {
            slot.mhz_sum[performance].fetch_add(static_cast<std::uint64_t>(mhz), std::memory_order_relaxed);
            slot.mhz_n[performance].fetch_add(1, std::memory_order_relaxed);
        }
    }
    (performance ? slot.performance : slot.efficiency).fetch_add(1, std::memory_order_relaxed);
    const int previous = slot.last_processor.exchange(static_cast<int>(index), std::memory_order_relaxed);
    if (previous >= 0 && previous != static_cast<int>(index)) slot.migrations.fetch_add(1, std::memory_order_relaxed);
    slot.processors[index].fetch_add(1, std::memory_order_relaxed);
    const auto bucket = static_cast<std::size_t>((GetTickCount64() - s.start) / kBucketMs);
    if (bucket < kBuckets) (performance ? slot.timeline_p : slot.timeline_e)[bucket].fetch_add(1, std::memory_order_relaxed);
    slot.pending.store(0, std::memory_order_release);
}

// Same thread id and creation time (thread ids are reused).
bool same_thread(HANDLE a, HANDLE b) {
    FILETIME ca{}, cb{}, x, k, u;
    GetThreadTimes(a, &ca, &x, &k, &u);
    GetThreadTimes(b, &cb, &x, &k, &u);
    return ca.dwLowDateTime == cb.dwLowDateTime && ca.dwHighDateTime == cb.dwHighDateTime;
}

// Sampler thread only.
void discover_threads(CoreStats& s) {
    HANDLE current = nullptr;
    for (;;) {
        HANDLE next = nullptr;
        if (s.next_thread(GetCurrentProcess(), current, THREAD_QUERY_INFORMATION | THREAD_SET_CONTEXT | SYNCHRONIZE, 0, 0,
                          &next) < 0)
            break;
        if (current) CloseHandle(current);
        current = next;
        const DWORD id = GetThreadId(next);
        if (id == s.self) continue;
        bool known = false;
        for (unsigned i = 0; i < s.used; ++i)
            if (s.slots[i].id == id && (!s.slots[i].dead || same_thread(s.slots[i].handle, next))) known = true;
        if (known || s.used == kMaxSlots) continue;
        HANDLE copy = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), next, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
            continue;
        Slot& slot = s.slots[s.used];
        slot.id = id;
        slot.handle = copy;
        QueryThreadCycleTime(copy, &slot.last_cycles);
        s.used++;
    }
    if (current) CloseHandle(current);
    if (g_guest_threads) {
        std::vector<std::uint32_t> guests;
        g_guest_threads(guests);
        for (const std::uint32_t id : guests)
            for (unsigned i = 0; i < s.used; ++i)
                if (s.slots[i].id == id && !s.slots[i].dead) s.slots[i].guest = true;
    }
}

double cpu_seconds(HANDLE h) {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(h, &created, &exited, &kernel, &user)) return 0;
    const auto v = [](const FILETIME& t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return static_cast<double>(v(kernel) + v(user)) * 1e-7;
}

// Per-thread context switches and wait reasons, and the process page-fault
// count, from NtQuerySystemInformation(SystemProcessInformation).
using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
void refresh_switches(CoreStats& s) {
    static const auto query = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query) return;
    auto& buffer = s.process_info;
    if (buffer.size() < (1u << 20)) buffer.resize(1u << 20);
    ULONG needed = 0;
    LONG status;
    while ((status = query(5, buffer.data(), static_cast<ULONG>(buffer.size()), &needed)) == static_cast<LONG>(0xC0000004))
        buffer.resize(std::max<std::size_t>(buffer.size() * 2, needed + 65536));
    if (status < 0) return;
    const auto pid = static_cast<std::uintptr_t>(GetCurrentProcessId());
    for (std::size_t offset = 0;;) {
        const std::uint8_t* p = buffer.data() + offset;
        const ULONG next = *reinterpret_cast<const ULONG*>(p);
        const ULONG threads = *reinterpret_cast<const ULONG*>(p + 4);
        if (*reinterpret_cast<const std::uintptr_t*>(p + 0x50) == pid) {
            s.page_faults = *reinterpret_cast<const ULONG*>(p + 0x80);   // PageFaultCount
            for (ULONG i = 0; i < threads; ++i) {
                const std::uint8_t* t = p + 0x100 + i * 0x50;
                const auto id = static_cast<DWORD>(*reinterpret_cast<const std::uintptr_t*>(t + 0x30));
                for (unsigned k = 0; k < s.used; ++k)
                    if (s.slots[k].id == id && !s.slots[k].dead) {
                        s.slots[k].context_switches = *reinterpret_cast<const ULONG*>(t + 0x40);
                        s.slots[k].wait_reason = *reinterpret_cast<const ULONG*>(t + 0x48);
                    }
            }
            return;
        }
        if (!next) return;
        offset += next;
    }
}

void write_report(CoreStats& s) {
    FILE* f = s.path.empty() ? stderr : _wfopen(s.path.c_str(), L"w");
    if (!f) return;
    refresh_switches(s);
    std::fprintf(f, "fx core stats: interval %u us, %.1f s since start, tsc %.0f MHz\ntopology (logical:class/core):%s\n",
                 s.interval_us, (GetTickCount64() - s.start) / 1000.0, s.tsc_hz / 1e6, s.topology.c_str());
    PROCESS_POWER_THROTTLING_STATE process{};
    process.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &process, sizeof(process));
    std::fprintf(f, "process: priority class 0x%lx, power throttling control 0x%lx state 0x%lx, page faults %lu\n",
                 GetPriorityClass(GetCurrentProcess()), process.ControlMask, process.StateMask, s.page_faults);
    {
        std::uint64_t sum[2] = {}, n[2] = {};
        for (unsigned i = 0; i < s.used; ++i)
            for (int c = 0; c < 2; ++c) {
                sum[c] += s.slots[i].mhz_sum[c].load(std::memory_order_relaxed);
                n[c] += s.slots[i].mhz_n[c].load(std::memory_order_relaxed);
            }
        std::fprintf(f, "clock probes: P-core %.0f MHz (%llu), E-core %.0f MHz (%llu)\n", n[1] ? double(sum[1]) / n[1] : 0.0,
                     static_cast<unsigned long long>(n[1]), n[0] ? double(sum[0]) / n[0] : 0.0,
                     static_cast<unsigned long long>(n[0]));
    }
    std::fprintf(f, "\n== threads: id samples P%% migrations cpu-s cswitches wait priority throttle(control/state) P-MHz E-MHz description processors\n");
    std::vector<unsigned> order;
    for (unsigned i = 0; i < s.used; ++i) order.push_back(i);
    const auto samples = [&](unsigned i) {
        return s.slots[i].performance.load(std::memory_order_relaxed) + s.slots[i].efficiency.load(std::memory_order_relaxed);
    };
    std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) { return samples(a) > samples(b); });
    for (unsigned i : order) {
        Slot& t = s.slots[i];
        const std::uint64_t n = samples(i);
        const double cpu = t.dead ? t.final_cpu : cpu_seconds(t.handle);
        if (n == 0 && cpu < 0.05) continue;
        THREAD_POWER_THROTTLING_STATE state{};
        state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        int priority = 0;
        std::string name;
        if (!t.dead) {
            GetThreadInformation(t.handle, ThreadPowerThrottling, &state, sizeof(state));
            priority = GetThreadPriority(t.handle);
            PWSTR description = nullptr;
            if (SUCCEEDED(GetThreadDescription(t.handle, &description)) && description) {
                for (const wchar_t* c = description; *c; ++c) name.push_back(*c < 128 ? static_cast<char>(*c) : '?');
                LocalFree(description);
            }
        } else {
            name = "(exited)";
        }
        if (t.guest) name += " [guest]";
        const auto mhz = [&](int c) {
            const auto k = t.mhz_n[c].load(std::memory_order_relaxed);
            return k ? static_cast<double>(t.mhz_sum[c].load(std::memory_order_relaxed)) / k : 0.0;
        };
        std::fprintf(f, "  %6lu %7llu %5.1f %6llu %7.2f %8lu %2lu %3d %lx/%lx %5.0f %5.0f %s |", t.id,
                     static_cast<unsigned long long>(n), n ? 100.0 * t.performance.load() / n : 0.0,
                     static_cast<unsigned long long>(t.migrations.load()), cpu, t.context_switches, t.wait_reason,
                     priority, state.ControlMask, state.StateMask, mhz(1), mhz(0), name.c_str());
        for (unsigned p = 0; p < kMaxProcessors; ++p)
            if (const auto c = t.processors[p].load(std::memory_order_relaxed)) std::fprintf(f, " %u:%u", p, c);
        std::fprintf(f, "\n");
    }
    const unsigned shown = static_cast<unsigned>(std::min<std::size_t>(3, order.size()));
    if (shown) {
        const auto elapsed = static_cast<unsigned>(std::min<ULONGLONG>(kBuckets, (GetTickCount64() - s.start) / kBucketMs + 1));
        std::fprintf(f, "\n== timeline (%u ms buckets), P/E samples of threads", kBucketMs);
        for (unsigned k = 0; k < shown; ++k) std::fprintf(f, " %lu", s.slots[order[k]].id);
        std::fprintf(f, "\n");
        for (unsigned b = 0; b < elapsed; ++b) {
            std::fprintf(f, "%7.2f s", b * kBucketMs / 1000.0);
            for (unsigned k = 0; k < shown; ++k)
                std::fprintf(f, "  %4u %4u", s.slots[order[k]].timeline_p[b].load(), s.slots[order[k]].timeline_e[b].load());
            std::fprintf(f, "\n");
        }
    }
    if (f != stderr) std::fclose(f);
}

DWORD WINAPI corestats_main(void*) {
    CoreStats& s = *g_stats;
    SetThreadDescription(GetCurrentThread(), L"fx corestats");
    s.self = GetCurrentThreadId();
    s.tsc_hz = calibrate_tsc();
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ULONGLONG last_report = GetTickCount64(), last_discover = 0;
    const auto min_cycles = static_cast<ULONG64>(s.tsc_hz * s.interval_us * 1e-6 / 3);
    for (;;) {
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(s.interval_us) * 10;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
        AcquireSRWLockExclusive(&s.report_lock);
        if (s.stopped) {
            ReleaseSRWLockExclusive(&s.report_lock);
            return 0;
        }
        const ULONGLONG now = GetTickCount64();
        if (now - last_discover >= 100) {
            discover_threads(s);
            last_discover = now;
        }
        for (unsigned i = 0; i < s.used; ++i) {
            Slot& t = s.slots[i];
            if (t.dead) continue;
            if (WaitForSingleObject(t.handle, 0) == WAIT_OBJECT_0) {
                refresh_switches(s);
                t.final_cpu = cpu_seconds(t.handle);
                t.dead = true;
                continue;
            }
            ULONG64 cycles = 0;
            QueryThreadCycleTime(t.handle, &cycles);
            const ULONG64 delta = cycles - t.last_cycles;
            t.last_cycles = cycles;
            if (delta < min_cycles || t.pending.load(std::memory_order_acquire)) continue;
            t.pending.store(1, std::memory_order_relaxed);
            if (!s.queue_apc(&sample_apc, t.handle, reinterpret_cast<ULONG_PTR>(&t), kSpecialUserApc))
                t.pending.store(0, std::memory_order_relaxed);
        }
        if (now - last_report >= 2000) {
            write_report(s);
            last_report = GetTickCount64();
        }
        ReleaseSRWLockExclusive(&s.report_lock);
    }
}

void start(const wchar_t* value) {
    auto* s = new CoreStats();
    s->slots = new Slot[kMaxSlots];
    std::wstring text(value);
    const auto comma = text.find(L',');
    if (comma != std::wstring::npos) {
        s->interval_us = std::max(250u, static_cast<unsigned>(_wtoi(text.c_str() + comma + 1)));
        text.resize(comma);
    }
    if (text != L"1") s->path = text;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    s->next_thread = reinterpret_cast<NtGetNextThreadFn>(GetProcAddress(ntdll, "NtGetNextThread"));
    s->queue_apc = reinterpret_cast<QueueUserApc2Fn>(kernelbase ? GetProcAddress(kernelbase, "QueueUserAPC2") : nullptr);
    if (!s->queue_apc)
        s->queue_apc = reinterpret_cast<QueueUserApc2Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueueUserAPC2"));
    if (!s->next_thread || !s->queue_apc) {
        std::fprintf(stderr, "fx corestats: NtGetNextThread/QueueUserAPC2 unavailable\n");
        return;
    }
    s->start = GetTickCount64();
    read_topology(*s);
    g_stats = s;
    if (HANDLE thread = CreateThread(nullptr, 0, &corestats_main, nullptr, 0, nullptr)) CloseHandle(thread);
}

} // namespace

void corestats_flush() {
    if (!g_stats) return;
    // Never waits: the sampler may have been terminated holding the lock.
    if (!TryAcquireSRWLockExclusive(&g_stats->report_lock)) return;
    if (!g_stats->stopped) {
        g_stats->stopped = true;
        write_report(*g_stats);
    }
    ReleaseSRWLockExclusive(&g_stats->report_lock);
}

void corestats_start_if_requested(void (*guest_threads)(std::vector<std::uint32_t>& ids)) {
    g_guest_threads = guest_threads;
    wchar_t value[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"TRANSMUTE_FX_CORESTATS", value, MAX_PATH);
    if (n == 0 || n >= MAX_PATH || value[0] == L'0') return;
    start(value);
}

} // namespace transmute::fx::host

#ifdef FX_CORESTATS_STANDALONE
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) transmute::fx::host::corestats_start_if_requested(nullptr);
    if (reason == DLL_PROCESS_DETACH && reserved) transmute::fx::host::corestats_flush();
    return TRUE;
}
#endif
