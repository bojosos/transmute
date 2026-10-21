// Run a Windows guest-host process under the native debug API and capture a
// second-chance exception or fail-fast before Windows terminates it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <wct.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace {

std::wstring quote_argument(const wchar_t* argument) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const auto* current = argument; *current != L'\0'; ++current) {
        if (*current == L'\\') {
            ++slashes;
            continue;
        }
        if (*current == L'"') {
            quoted.append(slashes * 2 + 1, L'\\');
        } else {
            quoted.append(slashes, L'\\');
        }
        slashes = 0;
        quoted.push_back(*current);
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

void print_stack(HANDLE process, HANDLE thread, DWORD thread_id) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &context)) {
        std::fprintf(stderr, "GetThreadContext failed: %lu\n", GetLastError());
        return;
    }
    std::fprintf(stderr, "thread %lu rip=0x%llx rsp=0x%llx rbp=0x%llx\n",
                 thread_id, static_cast<unsigned long long>(context.Rip),
                 static_cast<unsigned long long>(context.Rsp),
                 static_cast<unsigned long long>(context.Rbp));
    std::fprintf(stderr,
                 "  rcx=%llx rdx=%llx r8=%llx r9=%llx rbx=%llx rsi=%llx rdi=%llx r12=%llx r13=%llx\n",
                 context.Rcx, context.Rdx, context.R8, context.R9,
                 context.Rbx, context.Rsi, context.Rdi,
                 context.R12, context.R13);

    if (SymInitialize(process, nullptr, TRUE)) {
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rsp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        auto walking = context;
        for (unsigned index = 0; index < 32; ++index) {
            const auto address = frame.AddrPC.Offset;
            struct SymbolStorage {
                SYMBOL_INFO info;
                char extra[MAX_SYM_NAME];
            } storage{};
            storage.info.SizeOfStruct = sizeof(SYMBOL_INFO);
            storage.info.MaxNameLen = MAX_SYM_NAME;
            DWORD64 displacement = 0;
            IMAGEHLP_MODULE64 module{};
            module.SizeOfStruct = sizeof(module);
            const auto has_module = SymGetModuleInfo64(process, address, &module);
            const auto has_symbol = SymFromAddr(process, address, &displacement,
                                                &storage.info);
            std::fprintf(stderr, "  #%u 0x%llx %s%s%s+0x%llx\n", index,
                         static_cast<unsigned long long>(address),
                         has_module ? module.ModuleName : "?",
                         has_symbol ? "!" : "",
                         has_symbol ? storage.info.Name : "",
                         static_cast<unsigned long long>(displacement));
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                             &walking, nullptr, SymFunctionTableAccess64,
                             SymGetModuleBase64, nullptr)) {
                std::fprintf(stderr, "  StackWalk64 stopped: %lu\n",
                             GetLastError());
                break;
            }
            if (frame.AddrPC.Offset == 0 ||
                (index != 0 && frame.AddrPC.Offset == address)) {
                break;
            }
        }
        unsigned long long words[64]{};
        SIZE_T read = 0;
        if (ReadProcessMemory(process,
                              reinterpret_cast<const void*>(context.Rsp),
                              words, sizeof(words), &read)) {
            std::fprintf(stderr, "  raw stack (%zu bytes):\n",
                         static_cast<std::size_t>(read));
            for (std::size_t index = 0; index < read / sizeof(words[0]); ++index) {
                IMAGEHLP_MODULE64 module{};
                module.SizeOfStruct = sizeof(module);
                if (!SymGetModuleInfo64(process, words[index], &module)) continue;
                std::fprintf(stderr, "    rsp+0x%zx 0x%llx %s+0x%llx\n",
                             index * sizeof(words[0]), words[index],
                             module.ModuleName,
                             words[index] - module.BaseOfImage);
            }
        }
        SymCleanup(process);
    } else {
        std::fprintf(stderr, "SymInitialize failed: %lu\n", GetLastError());
    }
}

void write_dump(HANDLE process, DWORD process_id, DWORD thread_id,
                const EXCEPTION_RECORD& exception, HANDLE thread) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &context)) return;
    auto record = exception;
    EXCEPTION_POINTERS pointers{&record, &context};
    MINIDUMP_EXCEPTION_INFORMATION information{thread_id, &pointers, FALSE};
    const auto file = CreateFileW(L"artifacts\\host-crash.dmp", GENERIC_WRITE,
                                  FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "Could not create host-crash.dmp: %lu\n",
                     GetLastError());
        return;
    }
    const auto written = MiniDumpWriteDump(
        process, process_id, file,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo |
                                   MiniDumpWithFullMemoryInfo),
        &information, nullptr, nullptr);
    std::fprintf(stderr, "minidump %s (error %lu)\n",
                 written ? "artifacts/host-crash.dmp" : "failed",
                 written ? 0UL : GetLastError());
    CloseHandle(file);
}

unsigned long long thread_cpu(HANDLE thread) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0;
    return (static_cast<unsigned long long>(kernel.dwHighDateTime) << 32) +
           kernel.dwLowDateTime +
           (static_cast<unsigned long long>(user.dwHighDateTime) << 32) +
           user.dwLowDateTime;
}

int sample_cpu(DWORD process_id, unsigned attempts, unsigned interval) {
    const auto process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                     SYNCHRONIZE, FALSE, process_id);
    if (process == nullptr) {
        std::fprintf(stderr, "OpenProcess failed: %lu\n", GetLastError());
        return 2;
    }
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    HANDLE selected = nullptr;
    DWORD selected_id = 0;
    unsigned long long highest_delta = 0;
    unsigned long long selected_cpu = 0;
    struct Candidate {
        HANDLE thread;
        DWORD id;
        unsigned long long cpu;
    };
    std::vector<Candidate> candidates;
    const auto selection_started = std::chrono::steady_clock::now();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != process_id) continue;
            const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                           THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE,
                                           entry.th32ThreadID);
            if (thread == nullptr) continue;
            candidates.push_back({thread, entry.th32ThreadID, thread_cpu(thread)});
        } while (Thread32Next(snapshot, &entry));
    }
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    constexpr DWORD selection_interval = 200;
    const auto target_running = WaitForSingleObject(process, selection_interval) == WAIT_TIMEOUT;
    if (target_running) {
        for (const auto& candidate : candidates) {
            if (WaitForSingleObject(candidate.thread, 0) != WAIT_TIMEOUT) continue;
            const auto cpu = thread_cpu(candidate.thread);
            const auto delta = cpu >= candidate.cpu ? cpu - candidate.cpu : 0;
            if (selected == nullptr || delta > highest_delta) {
                selected = candidate.thread;
                selected_id = candidate.id;
                highest_delta = delta;
                selected_cpu = cpu;
            }
        }
    }
    for (const auto& candidate : candidates) {
        if (candidate.thread != selected) CloseHandle(candidate.thread);
    }
    const auto selection_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - selection_started).count();
    if (!target_running) {
        std::fprintf(stderr, "[transmute-profile] target exited during %lums thread selection\n",
                     selection_interval);
        CloseHandle(process);
        return 1;
    }
    if (selected == nullptr || !SymInitialize(process, nullptr, TRUE)) {
        std::fprintf(stderr, "Could not initialize sampling: %lu\n", GetLastError());
        if (selected != nullptr) CloseHandle(selected);
        CloseHandle(process);
        return 2;
    }
    std::fprintf(stderr,
                 "[transmute-profile] sampling busiest recent thread %lu (selector %lums, observed %.3fs, CPU %.3fs)\n",
                 selected_id, selection_interval, selection_elapsed,
                 highest_delta / 10000000.0);
    std::fflush(stderr);
    std::map<DWORD64, std::pair<unsigned, unsigned>> counts;
    unsigned captured = 0;
    auto previous_cpu = selected_cpu;
    const auto started = std::chrono::steady_clock::now();
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        Sleep(interval);
        if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT) break;
        const auto current_cpu = thread_cpu(selected);
        if (current_cpu == previous_cpu) continue;
        previous_cpu = current_cpu;
        if (SuspendThread(selected) == static_cast<DWORD>(-1)) break;
        std::array<DWORD64, 16> stack{};
        std::size_t depth = 0;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(selected, &context)) {
            stack[depth++] = context.Rip;
            STACKFRAME64 frame{};
            frame.AddrPC = {context.Rip, 0, AddrModeFlat};
            frame.AddrFrame = {context.Rsp, 0, AddrModeFlat};
            frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
            for (unsigned walk = 0; walk < stack.size() && depth < stack.size(); ++walk) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, selected, &frame,
                                 &context, nullptr, SymFunctionTableAccess64,
                                 SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
                    break;
                if (walk == 0 && frame.AddrPC.Offset == stack[depth - 1]) continue;
                if (frame.AddrPC.Offset == stack[depth - 1]) break;
                stack[depth++] = frame.AddrPC.Offset;
            }
        }
        ResumeThread(selected);
        // Allocate and aggregate only after resuming the target.
        if (depth == 0) continue;
        ++captured;
        ++counts[stack[0]].first;
        for (std::size_t index = 0; index < depth; ++index) {
            if (std::find(stack.begin(), stack.begin() + index, stack[index]) ==
                stack.begin() + index) ++counts[stack[index]].second;
        }
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::map<std::string, std::pair<unsigned, unsigned>> symbols;
    for (const auto& [address, count] : counts) {
        struct Storage { SYMBOL_INFO info; char extra[MAX_SYM_NAME]; } storage{};
        storage.info.SizeOfStruct = sizeof(SYMBOL_INFO);
        storage.info.MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        const bool known_module = SymGetModuleInfo64(process, address, &module);
        bool known_symbol = SymFromAddr(process, address, &displacement, &storage.info);
        if (known_symbol && storage.info.Size != 0) {
            known_symbol = address >= storage.info.Address &&
                address - storage.info.Address < storage.info.Size;
        } else if (known_symbol && known_module) {
            // Public-only PDBs can return an unrelated preceding symbol for a
            // private function. Use unwind bounds to reject that attribution.
            const auto* function = static_cast<const RUNTIME_FUNCTION*>(
                SymFunctionTableAccess64(process, address));
            known_symbol = function != nullptr &&
                storage.info.Address == module.BaseOfImage + function->BeginAddress;
        }
        char location[1024]{};
        if (known_symbol) {
            std::snprintf(location, sizeof(location), "%s!%s",
                          known_module ? module.ModuleName : "?", storage.info.Name);
        } else if (known_module) {
            std::snprintf(location, sizeof(location), "%s+0x%llx", module.ModuleName,
                          static_cast<unsigned long long>(address - module.BaseOfImage));
        } else {
            std::snprintf(location, sizeof(location), "anonymous code");
        }
        auto& total = symbols[location];
        total.first += count.first;
        total.second += count.second;
        if (known_symbol &&
            (std::strstr(storage.info.Name, "floating_multiply_add_bits") != nullptr ||
             std::strstr(storage.info.Name, "ReferenceEngine::execute") != nullptr)) {
            IMAGEHLP_LINE64 source{};
            source.SizeOfStruct = sizeof(source);
            DWORD line_displacement = 0;
            if (SymGetLineFromAddr64(process, address - 1, &line_displacement, &source)) {
                std::fprintf(stderr,
                    "[transmute-profile-source] leaf %u inclusive %u %s+0x%llx %s:%lu\n",
                    count.first, count.second, location,
                    static_cast<unsigned long long>(displacement),
                    source.FileName, source.LineNumber);
            }
        }
    }
    std::vector<std::pair<std::string, std::pair<unsigned, unsigned>>> ranked(
        symbols.begin(), symbols.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        return left.second.first != right.second.first
            ? left.second.first > right.second.first
            : left.second.second > right.second.second;
    });
    std::fprintf(stderr, "[transmute-profile] %u samples in %.3fs, thread CPU %.3fs\n",
                 captured, elapsed, (previous_cpu - selected_cpu) / 10000000.0);
    for (const auto& [name, count] : ranked) {
        std::fprintf(stderr, "  leaf %u inclusive %u %s\n",
                     count.first, count.second, name.c_str());
    }
    SymCleanup(process);
    CloseHandle(selected);
    CloseHandle(process);
    return captured == 0 ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wcscmp(argv[1], L"--sample") == 0) {
        const auto process_id = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
        const auto attempts = static_cast<unsigned>(std::wcstoul(argv[3], nullptr, 10));
        const auto interval = static_cast<unsigned>(std::wcstoul(argv[4], nullptr, 10));
        if (process_id == 0 || attempts == 0 || attempts > 100000 ||
            interval == 0 || interval > 1000) return 2;
        return sample_cpu(process_id, attempts, interval);
    }
    if (argc == 3 && std::wcscmp(argv[1], L"--attach") == 0) {
        const auto process_id = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
        const auto process = OpenProcess(PROCESS_QUERY_INFORMATION |
                                         PROCESS_VM_READ, FALSE, process_id);
        if (process == nullptr) {
            std::fprintf(stderr, "OpenProcess failed: %lu\n", GetLastError());
            return 2;
        }
        const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "CreateToolhelp32Snapshot failed: %lu\n",
                         GetLastError());
            CloseHandle(process);
            return 2;
        }
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const auto wait_chain = OpenThreadWaitChainSession(0, nullptr);
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != process_id) continue;
                if (wait_chain != nullptr) {
                    WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT]{};
                    DWORD count = WCT_MAX_NODE_COUNT;
                    BOOL cycle = FALSE;
                    if (GetThreadWaitChain(wait_chain, 0, 0,
                                           entry.th32ThreadID, &count,
                                           nodes, &cycle)) {
                        std::fprintf(stderr, "wait chain thread %lu cycle=%d:",
                                     entry.th32ThreadID, cycle);
                        for (DWORD index = 0; index < count; ++index) {
                            const auto& node = nodes[index];
                            std::fprintf(stderr, " [%u status=%u type=%u",
                                         index, node.ObjectStatus,
                                         node.ObjectType);
                            if (node.ObjectType == WctThreadType) {
                                std::fprintf(stderr, " tid=%lu",
                                             node.ThreadObject.ThreadId);
                            } else {
                                std::fprintf(stderr, " name=%ls",
                                             node.LockObject.ObjectName);
                            }
                            std::fprintf(stderr, "]");
                        }
                        std::fprintf(stderr, "\n");
                    } else {
                        std::fprintf(stderr,
                                     "wait chain thread %lu failed: %lu\n",
                                     entry.th32ThreadID, GetLastError());
                    }
                }
                const auto thread = OpenThread(THREAD_SUSPEND_RESUME |
                                               THREAD_GET_CONTEXT |
                                               THREAD_QUERY_INFORMATION,
                                               FALSE, entry.th32ThreadID);
                if (thread == nullptr) continue;
                if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                    print_stack(process, thread, entry.th32ThreadID);
                    ResumeThread(thread);
                }
                CloseHandle(thread);
            } while (Thread32Next(snapshot, &entry));
        }
        if (wait_chain != nullptr) CloseThreadWaitChainSession(wait_chain);
        CloseHandle(snapshot);
        CloseHandle(process);
        return 0;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: debug_host_crash.exe program [arguments...] | --attach PID | --sample PID ATTEMPTS INTERVAL_MS\n");
        return 2;
    }
    std::wstring command_line;
    for (int index = 1; index < argc; ++index) {
        if (!command_line.empty()) command_line.push_back(L' ');
        command_line += quote_argument(argv[index]);
    }
    std::vector<wchar_t> writable(command_line.begin(), command_line.end());
    writable.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(argv[1], writable.data(), nullptr, nullptr, FALSE,
                        DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr,
                        &startup, &child)) {
        std::fprintf(stderr, "CreateProcessW failed: %lu\n", GetLastError());
        return 2;
    }
    bool captured = false;
    for (;;) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, INFINITE)) {
            std::fprintf(stderr, "WaitForDebugEvent failed: %lu\n",
                         GetLastError());
            break;
        }
        DWORD continuation = DBG_CONTINUE;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            if (event.u.CreateProcessInfo.hFile != nullptr) {
                CloseHandle(event.u.CreateProcessInfo.hFile);
            }
        } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            if (event.u.LoadDll.hFile != nullptr) CloseHandle(event.u.LoadDll.hFile);
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const auto& detail = event.u.Exception;
            const auto& record = detail.ExceptionRecord;
            const bool fail_fast = record.ExceptionCode == 0xc0000409UL;
            const bool second_chance = detail.dwFirstChance == 0;
            if ((fail_fast || second_chance) && !captured) {
                captured = true;
                std::fprintf(stderr,
                    "exception code=0x%08lx address=%p first_chance=%lu "
                    "parameters=%lu\n", record.ExceptionCode,
                    record.ExceptionAddress, detail.dwFirstChance,
                    record.NumberParameters);
                for (DWORD index = 0; index < record.NumberParameters; ++index) {
                    std::fprintf(stderr, "  parameter[%lu]=0x%llx\n", index,
                        static_cast<unsigned long long>(
                            record.ExceptionInformation[index]));
                }
                const auto thread = OpenThread(
                    THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                    event.dwThreadId);
                if (thread != nullptr) {
                    print_stack(child.hProcess, thread, event.dwThreadId);
                    write_dump(child.hProcess, child.dwProcessId,
                               event.dwThreadId, record, thread);
                    CloseHandle(thread);
                }
            }
            continuation = record.ExceptionCode == EXCEPTION_BREAKPOINT
                ? DBG_CONTINUE : DBG_EXCEPTION_NOT_HANDLED;
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            std::fprintf(stderr, "debuggee exited: 0x%08lx\n",
                         event.u.ExitProcess.dwExitCode);
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId,
                               continuation);
            break;
        }
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId,
                                continuation)) {
            std::fprintf(stderr, "ContinueDebugEvent failed: %lu\n",
                         GetLastError());
            break;
        }
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(child.hProcess, &exit_code);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return static_cast<int>(exit_code);
}
