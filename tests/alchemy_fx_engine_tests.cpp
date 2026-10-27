// alchemy-fx engine tests: run Mach-O fixtures in separate alchemy-fx.exe
// processes (one fx runtime per host process) and check exit status, output
// and wall time. Covers process exit with CPU-bound and blocked workers,
// fatal signals (status 128 + signal with a guest backtrace), many threads
// with mixed stack sizes, and ARMv8.5 FRINTTS / FHM instructions.
//
// Usage: alchemy_fx_engine_tests <alchemy-fx.exe> <fx_threads> <fx_frint_probe> [threads fixture]

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Outcome {
    DWORD status{};
    std::string output;   // stdout and stderr, interleaved
    double seconds{};
};

std::wstring wide(const std::string& text) {
    std::wstring result(text.size(), L'\0');
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                            result.data(), static_cast<int>(result.size()));
    result.resize(static_cast<std::size_t>(length));
    return result;
}

std::wstring quote(const std::string& argument) {
    if (argument.find_first_of(" \t\"") == std::string::npos) return wide(argument);
    std::wstring text = L"\"";
    for (const auto ch : wide(argument)) {
        if (ch == L'"') text += L'\\';
        text += ch;
    }
    return text + L"\"";
}

Outcome run(const std::vector<std::string>& command, DWORD timeout_ms = 60'000) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) throw std::runtime_error("CreatePipe failed");
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    std::wstring line;
    for (auto argument : command) {
        if (line.empty()) std::replace(argument.begin(), argument.end(), '/', '\\');
        line += (line.empty() ? L"" : L" ") + quote(argument);
    }
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write;
    startup.hStdError = write;
    PROCESS_INFORMATION process{};
    const auto begin = std::chrono::steady_clock::now();
    const BOOL created = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                        &startup, &process);
    CloseHandle(write);
    if (!created) {
        CloseHandle(read);
        throw std::runtime_error("cannot start alchemy-fx");
    }
    Outcome outcome;
    // The pipe closes when the child ends; read it beside the timed wait.
    std::thread reader([&] {
        char buffer[4096];
        DWORD count = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &count, nullptr) && count) outcome.output.append(buffer, count);
    });
    const bool timed_out = WaitForSingleObject(process.hProcess, timeout_ms) != WAIT_OBJECT_0;
    if (timed_out) {
        TerminateProcess(process.hProcess, 0xdead);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    reader.join();
    CloseHandle(read);
    if (timed_out) outcome.output += "\n[test: timed out]";
    GetExitCodeProcess(process.hProcess, &outcome.status);
    outcome.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return outcome;
}

int failures = 0;

void expect(bool condition, const std::string& name, const Outcome& outcome, const std::string& why) {
    if (condition) {
        std::cout << "  ok   " << name << " (" << outcome.seconds << " s)\n";
        return;
    }
    ++failures;
    std::cout << "  FAIL " << name << ": " << why << " (status " << outcome.status << ", " << outcome.seconds
              << " s)\n" << outcome.output << '\n';
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: alchemy_fx_engine_tests <alchemy-fx.exe> <fx_threads> <fx_frint_probe>\n";
        return 2;
    }
    const std::string fx = argv[1], threads = argv[2], frint = argv[3];
    // Optional: --vm-remap=<fixture>, --vm-reserve=<fixture>, --jit-flush=<fixture>, --vm-lazy=<fixture>,
    // then the threaded fixture.
    std::string vm_remap, vm_reserve, jit_flush, vm_lazy, threaded;
    for (int i = 4; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.rfind("--vm-remap=", 0) == 0) vm_remap = argument.substr(11);
        else if (argument.rfind("--vm-reserve=", 0) == 0) vm_reserve = argument.substr(13);
        else if (argument.rfind("--jit-flush=", 0) == 0) jit_flush = argument.substr(12);
        else if (argument.rfind("--vm-lazy=", 0) == 0) vm_lazy = argument.substr(10);
        else threaded = argument;
    }
    try {
        {
            const auto outcome = run({fx, "--jit-stats", frint});
            expect(outcome.status == 0 && contains(outcome.output, "fallback instructions 0"),
                   "frint32x/frint32z/frint64x/fmlal/fmlal2/fmlsl exact and translated", outcome,
                   "failure mask " + std::to_string(outcome.status) +
                       " (bit 0 frint32x 4s, 1 frint32z 4s, 2 frint64x 2d, 3 frint32x 2d, 4 frint32x s RP, "
                       "5 fmlal, 6 fmlal2, 7 fmlsl)");
        }
        {
            // A synthetic (provider) call gets its native host-callback entry at
            // its first call: a process registers thousands and calls a few.
            // With ALCHEMY_FX_SVC_CALLS=1 every call stays on the SVC path.
            const auto counts = [](const std::string& output, unsigned long long& registered, unsigned long long& native) {
                const auto at = output.find("alchemy-fx synthetic calls: registered ");
                return at != std::string::npos &&
                       std::sscanf(output.c_str() + at, "alchemy-fx synthetic calls: registered %llu, native entries %llu",
                                   &registered, &native) == 2;
            };
            SetEnvironmentVariableA("ALCHEMY_FX_STATS", "1");
            const auto lazy = run({fx, frint});
            SetEnvironmentVariableA("ALCHEMY_FX_SVC_CALLS", "1");
            const auto svc = run({fx, frint});
            SetEnvironmentVariableA("ALCHEMY_FX_SVC_CALLS", nullptr);
            SetEnvironmentVariableA("ALCHEMY_FX_STATS", nullptr);
            unsigned long long registered = 0, native = 0, svc_registered = 0, svc_native = 0;
            expect(lazy.status == 0 && counts(lazy.output, registered, native) && native > 0 && native * 10 < registered,
                   "synthetic calls get native entries at their first call only", lazy,
                   "registered " + std::to_string(registered) + ", native entries " + std::to_string(native));
            expect(svc.status == 0 && counts(svc.output, svc_registered, svc_native) && svc_native == 0 &&
                       svc_registered == registered,
                   "ALCHEMY_FX_SVC_CALLS=1 keeps every synthetic call on the SVC path", svc,
                   "registered " + std::to_string(svc_registered) + ", native entries " + std::to_string(svc_native));
        }
        {
            // Translated LSE atomics (the system family must be registered:
            // the interpreter's read-modify-write is not atomic across threads).
            const auto outcome = run({fx, "--jit-stats", threads, "atomics", "16"});
            expect(outcome.status == 0 && contains(outcome.output, "fx threads: atomics 16 threads ok") &&
                       contains(outcome.output, "fallback instructions 0"),
                   "ldaddal under 16-thread contention, no interpreter fallback", outcome, "atomics failed");
        }
        {
            const auto outcome = run({fx, threads, "stress", "96"}, 180'000);
            expect(outcome.status == 0 && contains(outcome.output, "fx threads: 96 workers ok"),
                   "96 threads, 256 KiB to 16 MiB stacks, nested creation", outcome, "stress failed");
        }
        {
            const auto outcome = run({fx, threads, "exit-busy"});
            expect(outcome.status == 7 && outcome.seconds < 20 && !contains(outcome.output, "alchemy:"),
                   "exit() with spinning, mutex, condition and semaphore waiters", outcome,
                   "expected prompt status 7 without diagnostics");
        }
        {
            // _exit() ends workers blocked in kevent64, poll and usleep without
            // their calls returning an error (Chrome's message pump CHECKs
            // kevent64's result: ECANCELED there was a SIGTRAP at shutdown).
            // Whether a woken worker's guest code runs before the process is
            // gone is a race, so the traced calls show it: a wait that returns
            // at exit prints its result (errno 89 = ECANCELED).
            SetEnvironmentVariableA("ALCHEMY_TRACE_CALLS", "_kevent64,_poll,_usleep");
            const auto outcome = run({fx, threads, "exit-wakes"});
            SetEnvironmentVariableA("ALCHEMY_TRACE_CALLS", nullptr);
            expect(outcome.status == 0 && outcome.seconds < 20 && contains(outcome.output, "exit with waiting workers") &&
                       contains(outcome.output, "_kevent64(") && !contains(outcome.output, "(errno 89)") &&
                       !contains(outcome.output, "failed during exit"),
                   "_exit() with kevent64, poll and usleep waiters", outcome, "expected status 0 and no wait failing at exit");
        }
        {
            const auto outcome = run({fx, threads, "return-busy"});
            expect(outcome.status == 9 && outcome.seconds < 20 && !contains(outcome.output, "alchemy:"),
                   "return from main with busy workers", outcome, "expected prompt status 9");
        }
        {
            const auto outcome = run({fx, threads, "worker-exit"});
            expect(outcome.status == 5 && outcome.seconds < 20 && !contains(outcome.output, "alchemy:"),
                   "worker exit() while main is CPU-bound", outcome, "expected prompt status 5");
        }
        {
            // ALCHEMY_HANG_DUMP: touching the file makes the process print every
            // guest thread (the provider call it waits in, its backtrace).
            char directory[MAX_PATH]{};
            GetTempPathA(MAX_PATH, directory);
            const std::string trigger = std::string(directory) + "alchemy_hang_dump_" + std::to_string(GetCurrentProcessId());
            DeleteFileA(trigger.c_str());
            SetEnvironmentVariableA("ALCHEMY_HANG_DUMP", trigger.c_str());
            std::thread toucher([&] {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                if (FILE* file = std::fopen(trigger.c_str(), "w")) { std::fputs("dump", file); std::fclose(file); }
            });
            const auto outcome = run({fx, threads, "wait", "40"});
            toucher.join();
            SetEnvironmentVariableA("ALCHEMY_HANG_DUMP", nullptr);
            DeleteFileA(trigger.c_str());
            // The host stacks go to hang-dump-<pid>.txt next to the trigger.
            std::string host_stacks;
            if (const auto at = outcome.output.find("alchemy: hang dump (pid "); at != std::string::npos) {
                const auto pid = std::strtoul(outcome.output.c_str() + at + 24, nullptr, 10);
                const std::string file = std::string(directory) + "hang-dump-" + std::to_string(pid) + ".txt";
                if (std::ifstream in{file}) host_stacks.assign(std::istreambuf_iterator<char>(in), {});
                DeleteFileA(file.c_str());
            }
            expect(outcome.status == 0 && contains(outcome.output, "alchemy: hang dump (pid ") &&
                       contains(outcome.output, ") fx_threads: ") && contains(outcome.output, " (host ") &&
                       contains(outcome.output, " receive right(s) with ") &&
                       contains(host_stacks, " host thread ") && contains(host_stacks, "host #0 ") &&
                       contains(host_stacks, "alchemy-fx.exe+0x") &&
                       contains(outcome.output, "in _pthread_cond_wait") && contains(outcome.output, "in _usleep") &&
                       contains(outcome.output, "fx_threads+0x"),
                   "ALCHEMY_HANG_DUMP prints the waiting threads", outcome, "expected a dump with both waits");
        }
        {
            const auto outcome = run({fx, threads, "fault"});
            expect(outcome.status == 139 && contains(outcome.output, "SIGSEGV") &&
                       contains(outcome.output, "write of 0x10") && contains(outcome.output, "fx_threads+0x"),
                   "null-page write -> SIGSEGV with guest backtrace", outcome, "expected status 139");
        }
        {
            const auto outcome = run({fx, threads, "worker-fault"});
            expect(outcome.status == 139 && contains(outcome.output, "SIGSEGV") &&
                       contains(outcome.output, "[worker thread]"),
                   "worker fault while main joins", outcome, "expected status 139 from the worker");
        }
        {
            const auto outcome = run({fx, threads, "trap"});
            expect(outcome.status == 133 && contains(outcome.output, "SIGTRAP") && contains(outcome.output, "brk #0x1"),
                   "__builtin_trap -> SIGTRAP", outcome, "expected status 133");
        }
        {
            // abort() raises SIGABRT once libSystem's _abort throws
            // GuestSignalTermination; before that it is an error (status 1).
            const auto outcome = run({fx, threads, "abort"});
            expect(outcome.status == 134 || (outcome.status == 1 && contains(outcome.output, "abort")),
                   "abort() -> SIGABRT", outcome, "expected status 134");
            if (outcome.status != 134) std::cout << "       note: _abort does not raise SIGABRT yet (status 1)\n";
        }
        if (!vm_remap.empty()) {
            // Mach vm_remap of vm_allocate'd (shared) memory: a real alias, also
            // for code run through a read/execute alias and rewritten through
            // the writable one (fx shared guest memory).
            const auto outcome = run({fx, vm_remap});
            expect(outcome.status == 0 && contains(outcome.output, "fx vm_remap: ok"),
                   "vm_remap shared alias (data, code, backing after deallocate)", outcome,
                   "failed step " + std::to_string(outcome.status));
        }
        if (!vm_reserve.empty()) {
            // Darwin VM range semantics PartitionAlloc and V8 use: hints,
            // MAP_FIXED inside own reservations, partial munmap, MAP_JIT, a
            // V8-sized reservation (or a clean ENOMEM), madvise, translations
            // dropped across unmap/replace, region_recurse, purgeable memory.
            const auto outcome = run({fx, vm_reserve});
            expect(outcome.status == 0 && contains(outcome.output, "fx vm reserve: ok") &&
                       contains(outcome.output, "large reservation: "),
                   "Darwin VM ranges (hint, MAP_FIXED, partial munmap, MAP_JIT, madvise, region_recurse, purgeable)",
                   outcome, "failed step " + std::to_string(outcome.status));
            if (outcome.status == 0) std::cout << outcome.output;
        }
        if (!jit_flush.empty()) {
            // JIT code rewritten in place and announced with sys_icache_invalidate
            // (V8's pattern), code changed by a path fx cannot see (pwrite under a
            // MAP_SHARED view), and a thread leaving a stale translated loop.
            SetEnvironmentVariableA("ALCHEMY_FX_STATS", "1");
            const auto outcome = run({fx, jit_flush});
            SetEnvironmentVariableA("ALCHEMY_FX_STATS", nullptr);
            expect(outcome.status == 0 && contains(outcome.output, "fx jit flush: ok") &&
                       contains(outcome.output, "alchemy-fx invalidation: ") &&
                       !contains(outcome.output, "icache flushes 0 "),
                   "sys_icache_invalidate drops rewritten JIT code (in place, unseen write, spinning thread)", outcome,
                   "failed step " + std::to_string(outcome.status));
            if (outcome.status == 0) {
                for (const char* line : {"unseen write:", "alchemy-fx invalidation:"}) {
                    const auto at = outcome.output.find(line);
                    if (at != std::string::npos)
                        std::cout << "  " << outcome.output.substr(at, outcome.output.find('\n', at) - at) << '\n';
                }
            }
        }
        if (!vm_lazy.empty()) {
            // Darwin commits anonymous memory on first touch: a 1 GiB RWX
            // MAP_JIT range commits only the pages guest code, providers and
            // racing threads touch (resident pages from region_recurse), runs
            // code written there, gives MADV_FREE pages back; vm_remap of image
            // text over part of a code range (V8's embedded builtins).
            const auto outcome = run({fx, vm_lazy});
            expect(outcome.status == 0 && contains(outcome.output, "fx vm lazy: ok"),
                   "demand commit (1 GiB RWX range, resident pages, providers, threads, MADV_FREE, builtins remap)",
                   outcome, "failed step " + std::to_string(outcome.status));
        }
        if (!threaded.empty()) {
            const auto outcome = run({fx, threaded});
            expect(outcome.status == 0 && contains(outcome.output, "Darwin threads: 27 42"), "threaded fixture",
                   outcome, "threaded fixture failed");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures) {
        std::cout << failures << " alchemy-fx engine test(s) failed\n";
        return 1;
    }
    std::cout << "Alchemy fx engine tests passed\n";
    return 0;
}
