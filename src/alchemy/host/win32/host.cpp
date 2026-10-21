// Win32 host backend: handles are Win32 HANDLEs (sockets are SOCKETs, closed
// with closesocket; see net.cpp). Only files under src/alchemy/host/win32/
// include Windows headers.
#include "alchemy/host/host.hpp"
#include "alchemy/host/win32/win32.hpp"

namespace alchemy::host {

std::string_view backend() noexcept { return "win32"; }

void close(Handle handle) noexcept {
    if (handle) static_cast<void>(CloseHandle(win32::native(handle)));
}

Result<Handle> duplicate(Handle handle) {
    HANDLE copy{};
    if (!DuplicateHandle(GetCurrentProcess(), win32::native(handle), GetCurrentProcess(), &copy, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return win32::last_error();
    return win32::wrap(copy);
}

} // namespace alchemy::host
