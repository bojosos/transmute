#include "alchemy/host/system.hpp"

#include "alchemy/host/win32/win32.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <mutex>

namespace alchemy::host::system {

Result<Memory> memory() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return win32::last_error();
    return Memory{status.ullTotalPhys, status.ullAvailPhys};
}

Result<CpuTimes> cpu_times() {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return win32::last_error();
    // Kernel time includes idle time.
    const auto idle_ns = win32::nanoseconds(idle), kernel_ns = win32::nanoseconds(kernel);
    return CpuTimes{win32::nanoseconds(user), kernel_ns - std::min(kernel_ns, idle_ns), idle_ns};
}

Result<std::vector<CpuTimes>> processor_times() {
    // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION, from NtQuerySystemInformation
    // class SystemProcessorPerformanceInformation (8): one record per processor.
    struct Record { LARGE_INTEGER idle, kernel, user, dpc, interrupt; ULONG interrupts; };
    using Query = LONG(WINAPI*)(int, void*, ULONG, ULONG*);
    static const auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    if (!query) return Error::unsupported;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    std::vector<Record> records(std::max<DWORD>(1, info.dwNumberOfProcessors));
    ULONG returned{};
    if (query(8, records.data(), static_cast<ULONG>(records.size() * sizeof(Record)), &returned) < 0) return Error::io;
    records.resize(returned / sizeof(Record));
    std::vector<CpuTimes> times;
    times.reserve(records.size());
    for (const auto& record : records) {
        const auto idle = static_cast<std::uint64_t>(record.idle.QuadPart) * 100;
        const auto kernel = static_cast<std::uint64_t>(record.kernel.QuadPart) * 100;
        times.push_back({static_cast<std::uint64_t>(record.user.QuadPart) * 100, kernel - std::min(kernel, idle), idle});
    }
    return times;
}

Result<std::array<std::uint8_t, 16>> machine_uuid() {
    // The Windows machine GUID is the stable host identity.
    wchar_t buffer[64]{};
    DWORD size = sizeof(buffer);
    const auto status = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer, &size);
    if (status != ERROR_SUCCESS) return win32::error(static_cast<DWORD>(status));
    std::array<std::uint8_t, 16> bytes{};
    unsigned index = 0;
    for (const wchar_t* at = buffer; *at && index < 32; ++at) {
        const auto c = *at;
        const int digit = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
        if (digit < 0) continue;
        bytes[index / 2] = static_cast<std::uint8_t>(bytes[index / 2] | (digit << ((index & 1) ? 0 : 4)));
        ++index;
    }
    if (index != 32) return Error::io;
    return bytes;
}

Result<Pressure> memory_pressure() {
    static const HANDLE low = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    static const HANDLE high = CreateMemoryResourceNotification(HighMemoryResourceNotification);
    BOOL state{};
    if (low && QueryMemoryResourceNotification(low, &state) && state) return Pressure::critical;
    // Without the high-memory notification, assume plenty.
    if (!high || (QueryMemoryResourceNotification(high, &state) && state)) return Pressure::normal;
    return Pressure::warning;
}

Result<std::string> host_name(bool physical) {
    std::array<wchar_t, 256> name{};
    DWORD size = static_cast<DWORD>(name.size());
    if (!GetComputerNameExW(physical ? ComputerNamePhysicalDnsHostname : ComputerNameDnsHostname, name.data(), &size)) return win32::last_error();
    return win32::narrow(std::wstring_view(name.data(), size));
}

Result<std::string> user_name() {
    std::array<wchar_t, 257> name{};
    DWORD size = static_cast<DWORD>(name.size());
    if (!GetUserNameW(name.data(), &size)) return win32::last_error();
    if (size <= 1) return Error::not_found;
    return win32::narrow(std::wstring_view(name.data(), size - 1));
}

Result<PowerRequest> PowerRequest::create(std::string_view reason, bool system, bool display) {
    auto text = win32::widen(reason);
    REASON_CONTEXT context{};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = text.data();
    const auto handle = PowerCreateRequest(&context);
    if (handle == INVALID_HANDLE_VALUE || !handle) return win32::last_error();
    PowerRequest request;
    request.handle_ = win32::wrap(handle);
    request.system_ = system;
    request.display_ = display;
    if (system) PowerSetRequest(handle, PowerRequestSystemRequired);
    if (display) PowerSetRequest(handle, PowerRequestDisplayRequired);
    return request;
}

PowerRequest& PowerRequest::operator=(PowerRequest&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, {});
        system_ = other.system_;
        display_ = other.display_;
    }
    return *this;
}

Result<PowerStatus> power_status() {
    SYSTEM_POWER_STATUS power{};
    if (!GetSystemPowerStatus(&power)) return win32::last_error();
    PowerStatus status;
    status.battery = power.BatteryFlag != 128 && power.BatteryFlag != 255;   // no system battery / unknown
    status.external_power = power.ACLineStatus == 1;
    status.charging = (power.BatteryFlag & 8) != 0;
    if (power.BatteryLifePercent <= 100) status.percent = power.BatteryLifePercent;
    if (power.BatteryLifeTime != static_cast<DWORD>(-1)) status.seconds_to_empty = power.BatteryLifeTime;
    return status;
}

namespace {

// powrprof.dll's notification registrations, loaded on demand (DEVICE_NOTIFY_CALLBACK).
struct NotifyParameters { PVOID callback; PVOID context; };
constexpr DWORD device_notify_callback = 2;

HMODULE powrprof() {
    static const HMODULE library = LoadLibraryW(L"powrprof.dll");
    return library;
}

std::function<void(bool)>& sleep_watcher() { static std::function<void(bool)> watcher; return watcher; }
std::function<void()>& power_source_watcher() { static std::function<void()> watcher; return watcher; }

// PBT_APMSUSPEND 4, PBT_APMRESUMESUSPEND 7, PBT_APMRESUMEAUTOMATIC 0x12.
ULONG CALLBACK sleep_callback(PVOID, ULONG type, PVOID) {
    if (type == 4) sleep_watcher()(true);
    else if (type == 7 || type == 0x12) sleep_watcher()(false);
    return 0;
}

ULONG CALLBACK power_source_callback(PVOID, ULONG, PVOID) {
    power_source_watcher()();
    return 0;
}

} // namespace

Status watch_sleep(std::function<void(bool sleeping)> changed) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    if (sleep_watcher()) return Error::already;
    using Register = DWORD(WINAPI*)(DWORD, HANDLE, PHANDLE);
    const auto library = powrprof();
    if (!library) return win32::last_error();
    const auto function = reinterpret_cast<Register>(reinterpret_cast<void*>(GetProcAddress(library, "PowerRegisterSuspendResumeNotification")));
    if (!function) return Error::unsupported;
    sleep_watcher() = std::move(changed);
    static NotifyParameters parameters{reinterpret_cast<PVOID>(&sleep_callback), nullptr};
    HANDLE handle{};
    if (const auto status = function(device_notify_callback, &parameters, &handle); status != ERROR_SUCCESS) {
        sleep_watcher() = {};
        return win32::error(status);
    }
    return {};
}

// GUID_ACDC_POWER_SOURCE and GUID_BATTERY_PERCENTAGE_REMAINING: either registering is enough.
Status watch_power_source(std::function<void()> changed) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    if (power_source_watcher()) return Error::already;
    using Register = DWORD(WINAPI*)(LPCGUID, DWORD, HANDLE, PVOID*);
    const auto library = powrprof();
    if (!library) return win32::last_error();
    const auto function = reinterpret_cast<Register>(reinterpret_cast<void*>(GetProcAddress(library, "PowerSettingRegisterNotification")));
    if (!function) return Error::unsupported;
    power_source_watcher() = std::move(changed);
    static NotifyParameters parameters{reinterpret_cast<PVOID>(&power_source_callback), nullptr};
    static const GUID acdc{0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}};
    static const GUID percentage{0xa7ad8041, 0xb45a, 0x4cae, {0x87, 0xa3, 0xee, 0xcb, 0xb4, 0x68, 0xa9, 0xe1}};
    DWORD last = ERROR_SUCCESS;
    bool any = false;
    for (const auto* setting : {&acdc, &percentage}) {
        PVOID handle{};
        if (const auto status = function(setting, device_notify_callback, &parameters, &handle); status == ERROR_SUCCESS) any = true;
        else last = status;
    }
    if (!any) {
        power_source_watcher() = {};
        return win32::error(last);
    }
    return {};
}

void PowerRequest::release() noexcept {
    if (!handle_) return;
    const auto handle = win32::native(handle_);
    if (system_) PowerClearRequest(handle, PowerRequestSystemRequired);
    if (display_) PowerClearRequest(handle, PowerRequestDisplayRequired);
    CloseHandle(handle);
    handle_ = {};
}

} // namespace alchemy::host::system
