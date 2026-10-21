#include "alchemy/host/win32/win32.hpp"

namespace alchemy::host::win32 {

Error error(DWORD code) noexcept {
    switch (code) {
    case ERROR_SUCCESS: return Error::none;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: case ERROR_BAD_NETPATH:
    case ERROR_BAD_PATHNAME: case ERROR_MOD_NOT_FOUND: case ERROR_PROC_NOT_FOUND: case ERROR_BAD_NET_NAME:
    case ERROR_NO_MORE_FILES:
        return Error::not_found;
    case ERROR_ACCESS_DENIED: case ERROR_NETWORK_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION:
        return code == ERROR_SHARING_VIOLATION ? Error::busy : Error::access_denied;
    case ERROR_PRIVILEGE_NOT_HELD: return Error::permission;
    case ERROR_INVALID_HANDLE: case ERROR_INVALID_TARGET_HANDLE: case ERROR_DIRECT_ACCESS_HANDLE: case WSAEBADF:
        return Error::bad_handle;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return Error::exists;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return Error::no_space;
    case ERROR_INVALID_PARAMETER: case ERROR_INVALID_NAME: case ERROR_NEGATIVE_SEEK: case WSAEINVAL:
        return Error::invalid_argument;
    case ERROR_DIR_NOT_EMPTY: return Error::not_empty;
    case ERROR_DIRECTORY: return Error::not_directory;
    case ERROR_NOT_SUPPORTED: case ERROR_CALL_NOT_IMPLEMENTED: case WSAEOPNOTSUPP: return Error::unsupported;
    case ERROR_TOO_MANY_OPEN_FILES: case WSAEMFILE: return Error::too_many_open;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: case ERROR_COMMITMENT_LIMIT: case ERROR_ARENA_TRASHED:
    case ERROR_INVALID_BLOCK: case ERROR_NOT_ENOUGH_QUOTA:
        return Error::no_memory;
    case ERROR_WRITE_PROTECT: return Error::read_only;
    case ERROR_NOT_SAME_DEVICE: return Error::cross_device;
    case ERROR_FILENAME_EXCED_RANGE: case WSAENAMETOOLONG: return Error::name_too_long;
    case ERROR_BROKEN_PIPE: case ERROR_NO_DATA: case ERROR_PIPE_NOT_CONNECTED: return Error::broken_pipe;
    case ERROR_BUSY: case ERROR_PIPE_BUSY: return Error::busy;
    case ERROR_TIMEOUT: case WAIT_TIMEOUT: case ERROR_SEM_TIMEOUT: case WSAETIMEDOUT: return Error::timed_out;
    case ERROR_IO_PENDING: case WSAEWOULDBLOCK: return Error::would_block;
    case ERROR_OPERATION_ABORTED: case WSAEINTR: return Error::interrupted;
    case ERROR_NOACCESS: case WSAEFAULT: return Error::fault;
    case ERROR_ARITHMETIC_OVERFLOW: case ERROR_INSUFFICIENT_BUFFER: return Error::out_of_range;
    case ERROR_CANT_RESOLVE_FILENAME: case ERROR_STOPPED_ON_SYMLINK: case WSAELOOP: return Error::loop;
    case ERROR_TOO_MANY_LINKS: return Error::too_many_links;
    // Device and media failures.
    case ERROR_NOT_READY: case ERROR_CRC: case ERROR_SECTOR_NOT_FOUND: case ERROR_WRITE_FAULT: case ERROR_READ_FAULT:
    case ERROR_GEN_FAILURE: case ERROR_BAD_UNIT: case ERROR_BAD_COMMAND: case ERROR_BAD_LENGTH: case ERROR_SEEK:
    case ERROR_NOT_DOS_DISK: case ERROR_OUT_OF_PAPER: case ERROR_WRONG_DISK: case 35 /* ERROR_FCB_UNAVAILABLE */:
    case ERROR_SHARING_BUFFER_EXCEEDED: case ERROR_CURRENT_DIRECTORY: case ERROR_CANNOT_MAKE: case ERROR_FAIL_I24:
    case ERROR_DRIVE_LOCKED: case ERROR_SEEK_ON_DEVICE: case ERROR_NOT_LOCKED: case ERROR_LOCK_FAILED:
        return Error::io;
    case WSAEACCES: return Error::access_denied;
    case WSAEINPROGRESS: return Error::in_progress;
    case WSAEALREADY: return Error::already;
    case WSAENOTSOCK: return Error::not_socket;
    case WSAEMSGSIZE: return Error::message_too_long;
    case WSAEDESTADDRREQ: return Error::destination_required;
    case WSAEPROTOTYPE: return Error::protocol_type;
    case WSAENOPROTOOPT: return Error::protocol_option;
    case WSAEPROTONOSUPPORT: return Error::protocol_not_supported;
    case WSAESOCKTNOSUPPORT: return Error::socket_type;
    case WSAEPFNOSUPPORT: return Error::protocol_family;
    case WSAEAFNOSUPPORT: return Error::address_family;
    case WSAEADDRINUSE: return Error::address_in_use;
    case WSAEADDRNOTAVAIL: return Error::address_not_available;
    case WSAENETDOWN: case WSANOTINITIALISED: return Error::network_down;
    case WSAENETUNREACH: return Error::network_unreachable;
    case WSAECONNABORTED: return Error::connection_aborted;
    case WSAECONNRESET: return Error::connection_reset;
    case WSAENETRESET: return Error::network_reset;
    case WSAENOBUFS: return Error::no_buffers;
    case WSAEISCONN: return Error::already_connected;
    case WSAENOTCONN: return Error::not_connected;
    case WSAESHUTDOWN: return Error::shut_down;
    case WSAECONNREFUSED: return Error::connection_refused;
    case WSAEHOSTUNREACH: return Error::host_unreachable;
    case WSAEHOSTDOWN: return Error::host_down;
    default: return Error::other;
    }
}

std::wstring local_name(std::string_view name) {
    if (name.empty()) return {};
    return L"Local\\" + widen(name);
}

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

} // namespace alchemy::host::win32
