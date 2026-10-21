// win32 backend of fx/host/sync.hpp: the out-of-line part.

#include "fx/host/sync.hpp"

#include <windows.h>

namespace transmute::fx::host {

AutoResetEvent::AutoResetEvent() noexcept : handle_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

AutoResetEvent::~AutoResetEvent() {
    if (handle_) CloseHandle(handle_);
}

void AutoResetEvent::set() noexcept { SetEvent(handle_); }

bool AutoResetEvent::wait_for(unsigned long milliseconds) noexcept {
    return WaitForSingleObject(handle_, milliseconds) == WAIT_OBJECT_0;
}

} // namespace transmute::fx::host
