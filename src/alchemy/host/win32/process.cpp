#include "alchemy/host/process.hpp"

#include "alchemy/host/win32/win32.hpp"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace alchemy::host::process {

std::uint32_t current_pid() noexcept { return GetCurrentProcessId(); }
std::uint64_t current_thread_id() noexcept { return GetCurrentThreadId(); }

Handle standard_handle(Standard which) noexcept {
    const DWORD id = which == Standard::input ? STD_INPUT_HANDLE : which == Standard::output ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE;
    return win32::wrap(GetStdHandle(id));
}

Result<Usage> usage(std::uint32_t host_pid) {
    const bool self = host_pid == 0 || host_pid == GetCurrentProcessId();
    const auto process = self ? GetCurrentProcess() : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, host_pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER ? Error::not_found : win32::last_error();
    Usage value;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        value.user_ns = win32::nanoseconds(user);
        value.system_ns = win32::nanoseconds(kernel);
    }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        value.resident = counters.WorkingSetSize;
        value.resident_peak = counters.PeakWorkingSetSize;
        value.private_bytes = counters.PrivateUsage;
        value.page_faults = counters.PageFaultCount;
    }
    if (self) {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status)) value.virtual_size = status.ullTotalVirtual - status.ullAvailVirtual;
    } else {
        CloseHandle(process);
    }
    return value;
}

Result<ThreadUsage> thread_usage() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return win32::last_error();
    return ThreadUsage{win32::nanoseconds(user), win32::nanoseconds(kernel)};
}

Result<MemoryUsage> memory_usage() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof counters;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters))
        return win32::last_error();
    // The UCRT's malloc allocates from the process heap.
    HEAP_SUMMARY heap{};
    heap.cb = sizeof heap;
    if (!HeapSummary(GetProcessHeap(), 0, &heap)) return win32::last_error();
    return MemoryUsage{counters.PrivateUsage, heap.cbCommitted};
}

Status set_environment(std::string_view name, std::optional<std::string_view> value) {
    const auto wide_value = value ? win32::widen(*value) : std::wstring();
    if (!SetEnvironmentVariableW(win32::widen(name).c_str(), value ? wide_value.c_str() : nullptr)) return win32::last_error();
    return {};
}

std::vector<std::string> environment_block() {
    std::vector<std::string> result;
    if (auto* block = GetEnvironmentStringsW()) {
        for (auto* entry = block; *entry;) {
            const std::wstring_view value(entry);
            result.push_back(win32::narrow(value));
            entry += value.size() + 1;
        }
        FreeEnvironmentStringsW(block);
    }
    return result;
}

Result<std::uint32_t> exit_code(Handle process) {
    if (WaitForSingleObject(win32::native(process), 0) == WAIT_TIMEOUT) return Error::busy;
    DWORD code = 0;
    if (!GetExitCodeProcess(win32::native(process), &code)) return win32::last_error();
    return static_cast<std::uint32_t>(code);
}

Status terminate(Handle process, std::uint32_t exit_code) {
    if (!TerminateProcess(win32::native(process), exit_code)) return win32::last_error();
    return {};
}

Status terminate_pid(std::uint32_t host_pid, std::uint32_t exit_code) {
    const auto process = OpenProcess(PROCESS_TERMINATE, FALSE, host_pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER ? Error::not_found : win32::last_error();
    const bool ended = TerminateProcess(process, exit_code) != 0;
    const auto code = GetLastError();
    CloseHandle(process);
    if (!ended) return win32::error(code);
    return {};
}

void terminate_self(std::uint32_t exit_code) noexcept {
    TerminateProcess(GetCurrentProcess(), exit_code);
    std::abort();
}

void suspend_other_threads() noexcept {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const auto self = GetCurrentThreadId(), process = GetCurrentProcessId();
    for (auto ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        if (const auto thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID)) {
            SuspendThread(thread);
            CloseHandle(thread);
        }
    }
    CloseHandle(snapshot);
}

Result<std::uint64_t> start_time(std::uint32_t host_pid) {
    const bool self = host_pid == 0 || host_pid == GetCurrentProcessId();
    const auto process = self ? GetCurrentProcess() : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, host_pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER ? Error::not_found : win32::last_error();
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool known = GetProcessTimes(process, &created, &exited, &kernel, &user) != 0;
    const auto code = GetLastError();
    if (!self) CloseHandle(process);
    if (!known) return win32::error(code);
    // FILETIME counts 100 ns from 1601-01-01.
    const auto ticks = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    return ticks > 116444736000000000ULL ? (ticks - 116444736000000000ULL) * 100 : 0;
}

Result<Handle> open(std::uint32_t host_pid) {
    const auto process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, host_pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER ? Error::not_found : win32::last_error();
    return win32::wrap(process);
}

Result<bool> is_running(std::uint32_t host_pid) {
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, host_pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER ? Error::not_found : win32::last_error();
    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

Result<std::string> image_path(std::uint32_t host_pid) {
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, host_pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER ? Error::not_found : win32::last_error();
    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    const bool found = QueryFullProcessImageNameW(process, 0, buffer.data(), &size) != 0;
    const auto code = GetLastError();
    CloseHandle(process);
    if (!found) return win32::error(code);
    buffer.resize(size);
    return win32::narrow(buffer);
}

namespace {
// One argument of a CreateProcess command line, CommandLineToArgvW- (and C
// runtime-) compatible: quoted when it has blanks or quotes, backslashes
// doubled before a quote and at the end.
std::wstring quote(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
    std::wstring result = L"\"";
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') { ++it; ++backslashes; }
        if (it == argument.end()) { result.append(backslashes * 2, L'\\'); break; }
        if (*it == L'\"') { result.append(backslashes * 2 + 1, L'\\'); result.push_back(L'\"'); }
        else { result.append(backslashes, L'\\'); result.push_back(*it); }
    }
    result.push_back(L'\"');
    return result;
}
// The job that ends every child started with end_with_parent when this
// process ends (kill on close).
HANDLE children_job() {
    static HANDLE job = [] {
        const auto handle = CreateJobObjectW(nullptr, nullptr);
        if (!handle) return HANDLE{};
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(handle);
            return HANDLE{};
        }
        return handle;
    }();
    return job;
}
} // namespace

Result<Child> spawn(const Spawn& options) {
    std::wstring command = quote(win32::widen(options.program));
    for (const auto& argument : options.arguments) command += L" " + quote(win32::widen(argument));
    if (command.size() > 32000) return Error::out_of_range;
    std::vector<HANDLE> list;
    for (const auto handle : {options.standard_input, options.standard_output, options.standard_error})
        if (handle && std::find(list.begin(), list.end(), win32::native(handle)) == list.end()) list.push_back(win32::native(handle));
    for (const auto handle : options.inherit)
        if (handle && std::find(list.begin(), list.end(), win32::native(handle)) == list.end()) list.push_back(win32::native(handle));
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<std::uint8_t> attribute_storage(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) return win32::last_error();
    struct AttributeList { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeList() { DeleteProcThreadAttributeList(value); } } attribute_guard{attributes};
    // An empty list would inherit nothing but is refused: one entry at least.
    if (!list.empty() &&
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list.data(), list.size() * sizeof(HANDLE), nullptr, nullptr))
        return win32::last_error();
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = win32::native(options.standard_input);
    startup.StartupInfo.hStdOutput = win32::native(options.standard_output);
    startup.StartupInfo.hStdError = win32::native(options.standard_error);
    startup.lpAttributeList = attributes;
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    if (options.suspended) flags |= CREATE_SUSPENDED;
    if (!GetConsoleWindow()) flags |= CREATE_NO_WINDOW;   // never open a console window
    if (options.new_group) flags |= CREATE_NEW_PROCESS_GROUP;
    const auto directory = win32::widen(options.working_directory);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, !list.empty(), flags, nullptr,
                        directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &process))
        return win32::last_error();
    if (options.end_with_parent) {
        if (const auto job = children_job(); !job || !AssignProcessToJobObject(job, process.hProcess)) {
            const auto error = win32::last_error();
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return error == Error::none ? Error::other : error;
        }
    }
    return Child{win32::wrap(process.hProcess), win32::wrap(process.hThread), process.dwProcessId};
}

Status resume(Handle thread) {
    if (ResumeThread(win32::native(thread)) == static_cast<DWORD>(-1)) return win32::last_error();
    return {};
}

Result<Handle> inheritable(Handle handle) {
    HANDLE copy{};
    if (!DuplicateHandle(GetCurrentProcess(), win32::native(handle), GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return win32::last_error();
    return win32::wrap(copy);
}

Result<Piped> spawn_piped(std::string_view program, std::span<const std::string> arguments) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE in_read{}, in_write{}, out_read{}, out_write{}, err_read{}, err_write{};
    const auto close_all = [&] {
        for (auto* handle : {in_read, in_write, out_read, out_write, err_read, err_write})
            if (handle) CloseHandle(handle);
    };
    if (!CreatePipe(&in_read, &in_write, &security, 0) || !CreatePipe(&out_read, &out_write, &security, 0) ||
        !CreatePipe(&err_read, &err_write, &security, 0)) {
        const auto error = win32::last_error();
        close_all();
        return error;
    }
    // This process's ends are never inherited.
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = quote(win32::widen(program));
    for (const auto& argument : arguments) command += L" " + quote(win32::widen(argument));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = in_read;
    startup.StartupInfo.hStdOutput = out_write;
    startup.StartupInfo.hStdError = err_write;
    // The child inherits only its own three pipe ends: with every inheritable
    // handle of the process (other children's pipes on other threads), another
    // child's output would stay open until this one exits.
    HANDLE inherited[] = {in_read, out_write, err_write};
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<unsigned char> attribute_storage(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    DWORD flags = CREATE_NO_WINDOW;
    if (InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) &&
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
        startup.lpAttributeList = attributes;
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    PROCESS_INFORMATION information{};
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                                        &startup.StartupInfo, &information);
    const auto create_error = GetLastError();
    if (startup.lpAttributeList) DeleteProcThreadAttributeList(startup.lpAttributeList);
    // The child's ends belong to the child now.
    CloseHandle(in_read);
    CloseHandle(out_write);
    CloseHandle(err_write);
    in_read = out_write = err_write = nullptr;
    if (!created) {
        close_all();
        return win32::error(create_error);
    }
    CloseHandle(information.hThread);
    return Piped{win32::wrap(information.hProcess), information.dwProcessId, win32::wrap(in_write), win32::wrap(out_read), win32::wrap(err_read)};
}

Result<std::uint64_t> duplicate_into(Handle handle, std::uint32_t target_pid) {
    const bool self = target_pid == 0 || target_pid == GetCurrentProcessId();
    const auto target = self ? GetCurrentProcess() : OpenProcess(PROCESS_DUP_HANDLE, FALSE, target_pid);
    if (!target) return GetLastError() == ERROR_INVALID_PARAMETER ? Error::not_found : win32::last_error();
    HANDLE copy = nullptr;
    const bool duplicated = DuplicateHandle(GetCurrentProcess(), win32::native(handle), target, &copy, 0, FALSE, DUPLICATE_SAME_ACCESS) != 0;
    const auto code = GetLastError();
    if (!self) CloseHandle(target);
    if (!duplicated) return win32::error(code);
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(copy));
}

Result<std::vector<Entry>> list() {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return win32::last_error();
    std::vector<Entry> entries;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (auto ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        entries.push_back({entry.th32ProcessID, entry.th32ParentProcessID});
    CloseHandle(snapshot);
    return entries;
}

std::optional<std::string> environment(std::string_view name) {
    const auto wide = win32::widen(name);
    const auto size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (size == 0) return std::nullopt;
    std::wstring value(size, L'\0');
    const auto written = GetEnvironmentVariableW(wide.c_str(), value.data(), size);
    if (written == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) return std::nullopt;
    value.resize(written);
    return win32::narrow(value);
}

} // namespace alchemy::host::process
