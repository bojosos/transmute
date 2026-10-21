// Alchemy process model (docs/ALCHEMY_PROCESS.md): runs the proc_model guest
// fixture in separate Alchemy processes, at the default and a rebased load
// address, and checks each test's "ok" line and status. Every run happens in a
// job object of this driver; after the top-level process ends, no Alchemy
// child may remain (children die with their parent).
//
// Usage: alchemy_process_model_tests <alchemy(.exe|-fx.exe)> <proc_model> [options] [tests...]
//
// Options for alchemy-fx (rebased runs only, docs/ALCHEMY_REFACTOR_FOLLOWUPS.md, item 1):
//   --hold-check    the executable's load range is held from process start: a
//                   host reservation in it after the engine starts
//                   (ALCHEMY_FX_TEST_TAKE_LOAD_RANGE) is refused and the
//                   program still loads, rebased and at the default base;
//   --loop=R,P      P drivers run every test rebased R times at once (load).
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <cstdio>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Outcome { DWORD status{}; std::string output; double seconds{}; DWORD leftover{}; };

std::wstring wide(const std::string& text) {
    std::wstring result(text.size(), L'\0');
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), static_cast<int>(result.size()));
    result.resize(static_cast<std::size_t>(length));
    return result;
}

std::wstring quote(const std::string& argument) {
    if (!argument.empty() && argument.find_first_of(" \t\"") == std::string::npos) return wide(argument);
    std::wstring text = L"\"";
    for (const auto ch : wide(argument)) { if (ch == L'"') text += L'\\'; text += ch; }
    return text + L"\"";
}

Outcome run(const std::vector<std::string>& command, DWORD timeout_ms = 180'000) {
    const auto job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) throw std::runtime_error("CreatePipe failed");
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    std::wstring line;
    for (auto argument : command) {
        if (line.empty()) for (auto& c : argument) if (c == '/') c = '\\';
        line += (line.empty() ? L"" : L" ") + quote(argument);
    }
    // A defined standard input (the fixtures check the inherited descriptor set).
    const auto input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = write;
    startup.hStdError = write;
    PROCESS_INFORMATION process{};
    const auto begin = std::chrono::steady_clock::now();
    const BOOL created = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
    CloseHandle(write);
    CloseHandle(input);
    if (!created) { CloseHandle(read); CloseHandle(job); throw std::runtime_error("cannot start the Alchemy process"); }
    AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    Outcome outcome;
    std::thread reader([&] {
        char buffer[4096]; DWORD count = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &count, nullptr) && count) outcome.output.append(buffer, count);
    });
    const bool timed_out = WaitForSingleObject(process.hProcess, timeout_ms) != WAIT_OBJECT_0;
    if (timed_out) { TerminateProcess(process.hProcess, 0xdead); WaitForSingleObject(process.hProcess, INFINITE); }
    GetExitCodeProcess(process.hProcess, &outcome.status);
    outcome.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    // The parent's children end with it (its kill-on-close job): give them a moment.
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    for (int attempt = 0; attempt < 100; ++attempt) {
        QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr);
        if (!accounting.ActiveProcesses) break;
        Sleep(20);
    }
    outcome.leftover = accounting.ActiveProcesses;
    CloseHandle(job);   // ends anything left
    reader.join();
    CloseHandle(read);
    if (timed_out) outcome.output += "\n[test: timed out]";
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return outcome;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

bool passed(const Outcome& outcome) {
    return outcome.status == 0 && outcome.leftover == 0 && contains(outcome.output, "ok ") && !contains(outcome.output, "FAIL");
}

std::vector<std::string> rebased(const std::string& alchemy, const std::string& fixture, const std::string& test) {
    return {alchemy, "--load-base", "0x120000000", fixture, test};
}

// A host reservation taken in the load range right after the engine starts
// (as a thread stack placed bottom-up would be) is refused, and the program
// loads: rebased, and at the default base.
int hold_check(const std::string& alchemy, const std::string& fixture, const std::string& test) {
    int failures = 0;
    // `before`: an allocation already in the range when main starts
    // (ALCHEMY_FX_TEST_OCCUPY_BEFORE_HOLD), away from the image: the rest of
    // the range is still held.
    const struct { std::vector<std::string> command; const wchar_t* address; const char* label; const wchar_t* before; } cases[] = {
        {rebased(alchemy, fixture, test), L"0x120000000", "0x120000000", nullptr},
        {rebased(alchemy, fixture, test), L"0x1a0000000", "0x1a0000000", nullptr},
        {{alchemy, fixture, test}, L"0x100000000", "0x100000000", nullptr},
        {rebased(alchemy, fixture, test), L"0x120000000", "0x120000000 with 0x1f0000000 taken before main", L"0x1f0000000"},
    };
    for (const auto& [command, address, label, before] : cases) {
        SetEnvironmentVariableW(L"ALCHEMY_FX_TEST_TAKE_LOAD_RANGE", address);
        SetEnvironmentVariableW(L"ALCHEMY_FX_TEST_OCCUPY_BEFORE_HOLD", before);
        const auto outcome = run(command);
        SetEnvironmentVariableW(L"ALCHEMY_FX_TEST_TAKE_LOAD_RANGE", nullptr);
        SetEnvironmentVariableW(L"ALCHEMY_FX_TEST_OCCUPY_BEFORE_HOLD", nullptr);
        const bool ok = passed(outcome) && contains(outcome.output, "refused (held)");
        std::cout << (ok ? "  ok   " : "  FAIL ") << "load range held: " << test << (command.size() > 3 ? " (rebased)" : "")
                  << ", host reservation at " << label << "\n";
        if (!ok) {
            ++failures;
            std::cout << "    status " << outcome.status << "\n" << outcome.output << "\n";
        }
    }
    // Host memory placed before main inside the executable's own range (a
    // loader thread's stack or a heap segment, bottom-up from a random base):
    // the position-independent executable slides to a held part of the range,
    // as on Darwin, instead of failing with "requested segment address is
    // unavailable" (intermittent in child processes).
    {
        SetEnvironmentVariableW(L"ALCHEMY_FX_TEST_OCCUPY_BEFORE_HOLD", L"0x100000000");
        const auto outcome = run({alchemy, fixture, test});
        SetEnvironmentVariableW(L"ALCHEMY_FX_TEST_OCCUPY_BEFORE_HOLD", nullptr);
        const bool ok = passed(outcome) && contains(outcome.output, "is taken by host memory; it loads at 0x");
        std::cout << (ok ? "  ok   " : "  FAIL ") << "executable slides past host memory in its range: " << test << "\n";
        if (!ok) {
            ++failures;
            std::cout << "    status " << outcome.status << "\n" << outcome.output << "\n";
        }
    }
    return failures;
}

// `parallel` drivers each run every test rebased `rounds` times.
int loop(const std::string& alchemy, const std::string& fixture, const std::vector<std::string>& tests, unsigned rounds,
         unsigned parallel) {
    std::atomic<int> failures{0}, runs{0};
    std::mutex report;
    std::vector<std::thread> drivers;
    for (unsigned driver = 0; driver < parallel; ++driver)
        drivers.emplace_back([&] {
            for (unsigned round = 0; round < rounds; ++round)
                for (const auto& test : tests) {
                    const auto outcome = run(rebased(alchemy, fixture, test));
                    ++runs;
                    if (passed(outcome)) continue;
                    if (++failures <= 3) {
                        std::lock_guard lock(report);
                        std::cout << "  FAIL " << test << " (rebased, loop): status " << outcome.status << "\n" << outcome.output << "\n";
                    }
                }
        });
    for (auto& driver : drivers) driver.join();
    std::cout << "  " << (failures ? "FAIL " : "ok   ") << "rebased loop: " << runs.load() << " runs, " << failures.load()
              << " failure(s)\n";
    return failures.load();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: alchemy_process_model_tests <alchemy.exe> <proc_model> [tests...]\n";
        return 2;
    }
    const std::string alchemy = argv[1], fixture = argv[2];
    bool check_hold = false;
    unsigned rounds = 0, parallel = 0;
    int index = 3;
    for (; index < argc && std::string_view(argv[index]).starts_with("--"); ++index) {
        const std::string_view option(argv[index]);
        if (option == "--hold-check") check_hold = true;
        else if (option.starts_with("--loop=") && std::sscanf(argv[index] + 7, "%u,%u", &rounds, &parallel) == 2 && rounds && parallel) {}
        else { std::cerr << "unknown option " << option << "\n"; return 2; }
    }
    std::vector<std::string> tests;
    for (; index < argc; ++index) tests.emplace_back(argv[index]);
    if (check_hold || rounds) {
        if (tests.empty()) { std::cerr << "--hold-check and --loop need tests\n"; return 2; }
        int failures = check_hold ? hold_check(alchemy, fixture, tests.front()) : 0;
        if (rounds) failures += loop(alchemy, fixture, tests, rounds, parallel);
        std::cout << (failures ? "FAILED: " : "passed: ") << failures << " failure(s)\n";
        return failures ? 1 : 0;
    }
    if (tests.empty())
        tests = {"pipe", "spawn", "boost", "forkdup", "forktls", "wait", "kqproc", "stackguard", "pty", "popen", "cloexec", "kill", "violation", "shell", "shellsock", "installer",
                 "superuser", "exec"};
    int failures = 0;
    for (const bool rebased : {false, true}) {
        for (const auto& test : tests) {
            std::vector<std::string> command{alchemy};
            if (rebased) { command.push_back("--load-base"); command.push_back("0x120000000"); }
            command.push_back(fixture);
            command.push_back(test);
            // "superuser" runs as the sudo-equivalent root identity.
            if (test == "superuser") SetEnvironmentVariableW(L"ALCHEMY_SUPERUSER", L"1");
            const auto outcome = run(command);
            if (test == "superuser") SetEnvironmentVariableW(L"ALCHEMY_SUPERUSER", nullptr);
            const auto name = test == "exec" ? std::string("exec_in_place") : test;
            const auto expected_status = test == "exec" ? 42U : 0U;
            const bool passed = outcome.status == expected_status && outcome.leftover == 0 &&
                (contains(outcome.output, "ok ") && !contains(outcome.output, "FAIL"));
            std::cout << (passed ? "  ok   " : "  FAIL ") << test << (rebased ? " (rebased)" : "") << " (" << outcome.seconds << " s)\n";
            if (!passed) {
                ++failures;
                std::cout << "    status " << outcome.status << ", leftover processes " << outcome.leftover << "\n" << outcome.output << "\n";
            }
        }
    }
    std::cout << (failures ? "FAILED: " : "passed: ") << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
