#include "alchemy/host/ipc.hpp"

#include "alchemy/host/win32/win32.hpp"

#include <algorithm>

namespace alchemy::host::ipc {
namespace {

std::wstring pipe_name(std::string_view name) { return L"\\\\.\\pipe\\" + win32::widen(name); }

// One overlapped transfer (all I/O on these pipes is overlapped, so a
// connection's reader never blocks its writer).
Result<std::size_t> transfer(Handle channel, bool writing, void* data, std::size_t size) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) return win32::last_error();
    DWORD done = 0;
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1U << 20));
    const auto pipe = win32::native(channel);
    const BOOL started = writing ? WriteFile(pipe, data, chunk, nullptr, &overlapped) : ReadFile(pipe, data, chunk, nullptr, &overlapped);
    const bool ok = (started || GetLastError() == ERROR_IO_PENDING) && GetOverlappedResult(pipe, &overlapped, &done, TRUE);
    const auto code = GetLastError();
    CloseHandle(overlapped.hEvent);
    if (ok) return static_cast<std::size_t>(done);
    // A closed peer or a cancelled transfer ends the channel.
    if (code == ERROR_BROKEN_PIPE || code == ERROR_OPERATION_ABORTED || code == ERROR_PIPE_NOT_CONNECTED || code == ERROR_NO_DATA)
        return std::size_t{0};
    return win32::error(code);
}

} // namespace

Result<Handle> listen(std::string_view name, bool first) {
    const auto pipe = CreateNamedPipeW(pipe_name(name).c_str(),
                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        return first && code == ERROR_ACCESS_DENIED ? Error::exists : win32::error(code);
    }
    return win32::wrap(pipe);
}

Status accept(Handle listener) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) return win32::last_error();
    const auto pipe = win32::native(listener);
    bool connected = ConnectNamedPipe(pipe, &overlapped) != 0;
    auto code = connected ? DWORD{ERROR_SUCCESS} : GetLastError();
    if (!connected) {
        DWORD ignored = 0;
        connected = code == ERROR_PIPE_CONNECTED || (code == ERROR_IO_PENDING && GetOverlappedResult(pipe, &overlapped, &ignored, TRUE));
        if (!connected && code == ERROR_IO_PENDING) code = GetLastError();
    }
    CloseHandle(overlapped.hEvent);
    if (!connected) return win32::error(code);
    return {};
}

Result<Handle> connect(std::string_view name) {
    const auto wide = pipe_name(name);
    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto pipe = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return win32::wrap(pipe);
        const auto code = GetLastError();
        if (code != ERROR_PIPE_BUSY) return win32::error(code);
        WaitNamedPipeW(wide.c_str(), 100);
    }
    return Error::busy;
}

Result<std::size_t> read(Handle channel, Bytes buffer) { return transfer(channel, false, buffer.data(), buffer.size()); }
Result<std::size_t> write(Handle channel, ConstBytes data) { return transfer(channel, true, const_cast<std::byte*>(data.data()), data.size()); }
void cancel(Handle channel) noexcept { CancelIoEx(win32::native(channel), nullptr); }

} // namespace alchemy::host::ipc
