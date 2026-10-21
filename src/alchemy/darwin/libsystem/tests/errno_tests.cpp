// libsystem::darwin_errno (darwin/libsystem/errno.hpp): every host::Error has a
// Darwin errno, and the socket and link kinds keep the values the WinSock and
// Win32 translations of the libSystem services returned before the host layer.
#include "alchemy/darwin/libsystem/errno.hpp"
#if defined(ALCHEMY_HOST_WIN32)
#include "alchemy/host/win32/win32.hpp"
#endif

#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using alchemy::host::Error;
using alchemy::libsystem::darwin_errno;

static_assert(darwin_errno(Error::none) == 0);
static_assert(darwin_errno(Error::not_found) == 2 && darwin_errno(Error::bad_handle) == 9);
static_assert(darwin_errno(Error::would_block) == 35 && darwin_errno(Error::in_progress) == 36);
static_assert(darwin_errno(Error::destination_required) == 39 && darwin_errno(Error::protocol_type) == 41);
static_assert(darwin_errno(Error::protocol_option) == 42 && darwin_errno(Error::protocol_not_supported) == 43);
static_assert(darwin_errno(Error::socket_type) == 44 && darwin_errno(Error::protocol_family) == 46);
static_assert(darwin_errno(Error::network_reset) == 52 && darwin_errno(Error::shut_down) == 58);
static_assert(darwin_errno(Error::loop) == 62 && darwin_errno(Error::host_down) == 64);
static_assert(darwin_errno(Error::too_many_links) == 31 && darwin_errno(Error::busy) == 16);
static_assert(darwin_errno(Error::unsupported) == 45 && darwin_errno(Error::denied_by_policy) == 51);
static_assert(darwin_errno(Error::other) == 5);

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}


// The tables of the replaced Win32 code, kind by kind.
using alchemy::libsystem::darwin_crt_errno;
using alchemy::libsystem::darwin_file_errno;
using alchemy::libsystem::darwin_socket_errno;
static_assert(darwin_file_errno(Error::unsupported) == 78 && darwin_file_errno(Error::busy) == 16 && darwin_file_errno(Error::read_only) == 30);
static_assert(darwin_file_errno(Error::cross_device) == 18 && darwin_file_errno(Error::name_too_long) == 63);
static_assert(darwin_file_errno(Error::broken_pipe) == 5 && darwin_file_errno(Error::timed_out) == 5 && darwin_file_errno(Error::other) == 5);
static_assert(darwin_crt_errno(Error::busy) == 13 && darwin_crt_errno(Error::read_only) == 13 && darwin_crt_errno(Error::name_too_long) == 2);
static_assert(darwin_crt_errno(Error::cross_device) == 5 && darwin_crt_errno(Error::broken_pipe) == 32 && darwin_crt_errno(Error::io) == 13);
static_assert(darwin_crt_errno(Error::unsupported) == 22 && darwin_crt_errno(Error::other) == 22 && darwin_crt_errno(Error::permission) == 22);
static_assert(darwin_socket_errno(Error::unsupported) == 45 && darwin_socket_errno(Error::would_block) == 35);
static_assert(darwin_socket_errno(Error::no_memory) == 5 && darwin_socket_errno(Error::not_found) == 5 && darwin_socket_errno(Error::other) == 5);

#if defined(ALCHEMY_HOST_WIN32)
// Code by code: every Win32 and WinSock code the replaced tables named keeps
// its Darwin value through the host backend's mapping.
struct Legacy { unsigned long code; std::uint32_t value; };
// windows_error() of the libSystem services.
constexpr Legacy file_table[] = {
    {ERROR_SUCCESS, 0}, {ERROR_FILE_NOT_FOUND, 2}, {ERROR_PATH_NOT_FOUND, 2}, {ERROR_ACCESS_DENIED, 13},
    {ERROR_NETWORK_ACCESS_DENIED, 13}, {ERROR_PRIVILEGE_NOT_HELD, 1}, {ERROR_INVALID_HANDLE, 9}, {ERROR_ALREADY_EXISTS, 17},
    {ERROR_FILE_EXISTS, 17}, {ERROR_SHARING_VIOLATION, 16}, {ERROR_DISK_FULL, 28}, {ERROR_HANDLE_DISK_FULL, 28},
    {ERROR_INVALID_PARAMETER, 22}, {ERROR_INVALID_NAME, 22}, {ERROR_DIR_NOT_EMPTY, 66}, {ERROR_DIRECTORY, 20},
    {ERROR_NOT_SUPPORTED, 78}, {ERROR_TOO_MANY_OPEN_FILES, 24}, {ERROR_NOT_ENOUGH_MEMORY, 12}, {ERROR_OUTOFMEMORY, 12},
    {ERROR_WRITE_PROTECT, 30}, {ERROR_NOT_SAME_DEVICE, 18}, {ERROR_FILENAME_EXCED_RANGE, 63},
    // not named there: EIO
    {ERROR_BROKEN_PIPE, 5}, {ERROR_NO_DATA, 5}, {ERROR_IO_PENDING, 5}, {ERROR_USER_MAPPED_FILE, 5}, {ERROR_GEN_FAILURE, 5},
    {ERROR_SEM_TIMEOUT, 5}, {ERROR_OPERATION_ABORTED, 5}, {ERROR_DISK_CORRUPT, 5}, {ERROR_BAD_UNIT, 5}, {ERROR_DRIVE_LOCKED, 5},
    {ERROR_SEEK_ON_DEVICE, 5}, {ERROR_NOT_LOCKED, 5}, {ERROR_LOCK_FAILED, 5}, {ERROR_CANNOT_MAKE, 5},
};
// The C runtime's Win32-to-errno table (read, write, lseek, dup, _commit,
// _chsize_s, _wchmod, _waccess), then host_error().
constexpr Legacy crt_table[] = {
    {ERROR_FILE_NOT_FOUND, 2}, {ERROR_PATH_NOT_FOUND, 2}, {ERROR_TOO_MANY_OPEN_FILES, 24}, {ERROR_ACCESS_DENIED, 13},
    {ERROR_INVALID_HANDLE, 9}, {ERROR_NOT_ENOUGH_MEMORY, 12}, {ERROR_INVALID_PARAMETER, 22}, {ERROR_NOT_SAME_DEVICE, 5},
    {ERROR_LOCK_VIOLATION, 13}, {ERROR_NETWORK_ACCESS_DENIED, 13}, {ERROR_FILE_EXISTS, 17}, {ERROR_ALREADY_EXISTS, 17},
    {ERROR_BROKEN_PIPE, 32}, {ERROR_DISK_FULL, 28}, {ERROR_NEGATIVE_SEEK, 22}, {ERROR_DIR_NOT_EMPTY, 66},
    {ERROR_FILENAME_EXCED_RANGE, 2}, {ERROR_WRITE_PROTECT, 13}, {ERROR_SHARING_VIOLATION, 13}, {ERROR_NOT_READY, 13},
    {ERROR_CRC, 13}, {ERROR_SECTOR_NOT_FOUND, 13}, {ERROR_WRITE_FAULT, 13}, {ERROR_READ_FAULT, 13}, {ERROR_GEN_FAILURE, 13},
    {ERROR_BAD_UNIT, 13}, {ERROR_BAD_COMMAND, 13}, {ERROR_BAD_LENGTH, 13}, {ERROR_SEEK, 13}, {ERROR_NOT_DOS_DISK, 13},
    {ERROR_OUT_OF_PAPER, 13}, {ERROR_WRONG_DISK, 13}, {35, 13}, {ERROR_SHARING_BUFFER_EXCEEDED, 13}, {ERROR_CURRENT_DIRECTORY, 13},
    {ERROR_CANNOT_MAKE, 13}, {ERROR_FAIL_I24, 13}, {ERROR_DRIVE_LOCKED, 13}, {ERROR_SEEK_ON_DEVICE, 13}, {ERROR_NOT_LOCKED, 13},
    {ERROR_LOCK_FAILED, 13},
    // not named there: EINVAL
    {ERROR_NOT_SUPPORTED, 22}, {ERROR_PRIVILEGE_NOT_HELD, 22}, {ERROR_IO_PENDING, 22}, {ERROR_OPERATION_ABORTED, 22},
};
// windows_errno() of process/launch.cpp (child process creation).
constexpr Legacy spawn_table[] = {
    {ERROR_FILE_NOT_FOUND, 2}, {ERROR_PATH_NOT_FOUND, 2}, {ERROR_INVALID_DRIVE, 2}, {ERROR_ACCESS_DENIED, 13},
    {ERROR_NOT_ENOUGH_MEMORY, 12}, {ERROR_OUTOFMEMORY, 12}, {ERROR_COMMITMENT_LIMIT, 12}, {ERROR_TOO_MANY_OPEN_FILES, 24},
    {ERROR_INVALID_HANDLE, 9}, {ERROR_FILENAME_EXCED_RANGE, 63}, {ERROR_DIRECTORY, 20},
    // not named there: EIO
    {ERROR_SHARING_VIOLATION, 5}, {ERROR_INVALID_PARAMETER, 5}, {ERROR_NOT_SUPPORTED, 5}, {ERROR_BAD_EXE_FORMAT, 5},
};
// network_error() of the libSystem sockets.
constexpr Legacy socket_table[] = {
    {WSAEINTR, 4}, {WSAEBADF, 9}, {WSAEACCES, 13}, {WSAEFAULT, 14}, {WSAEINVAL, 22}, {WSAEMFILE, 24},
    {WSAEWOULDBLOCK, 35}, {WSAEINPROGRESS, 36}, {WSAEALREADY, 37}, {WSAENOTSOCK, 38}, {WSAEDESTADDRREQ, 39},
    {WSAEMSGSIZE, 40}, {WSAEPROTOTYPE, 41}, {WSAENOPROTOOPT, 42}, {WSAEPROTONOSUPPORT, 43}, {WSAESOCKTNOSUPPORT, 44},
    {WSAEOPNOTSUPP, 45}, {WSAEPFNOSUPPORT, 46}, {WSAEAFNOSUPPORT, 47}, {WSAEADDRINUSE, 48}, {WSAEADDRNOTAVAIL, 49},
    {WSAENETDOWN, 50}, {WSAENETUNREACH, 51}, {WSAENETRESET, 52}, {WSAECONNABORTED, 53}, {WSAECONNRESET, 54},
    {WSAENOBUFS, 55}, {WSAEISCONN, 56}, {WSAENOTCONN, 57}, {WSAESHUTDOWN, 58}, {WSAETIMEDOUT, 60},
    {WSAECONNREFUSED, 61}, {WSAELOOP, 62}, {WSAENAMETOOLONG, 63}, {WSAEHOSTDOWN, 64}, {WSAEHOSTUNREACH, 65},
    {WSANOTINITIALISED, 50},
    // not named there: EIO
    {WSA_NOT_ENOUGH_MEMORY, 5}, {WSAEPROCLIM, 5}, {WSAEDISCON, 5}, {WSAEUSERS, 5}, {WSAEDQUOT, 5}, {WSAESTALE, 5},
};

// The rows of r1-errno-fidelity.md, the codes one host kind groups
// together, each with the value chosen by Darwin's semantics
// (docs/ALCHEMY_REFACTOR_FOLLOWUPS.md, "errno residuals").
constexpr Legacy file_residuals[] = {
    // a missing drive, share, path or module: ENOENT
    {ERROR_INVALID_DRIVE, 2}, {ERROR_BAD_NETPATH, 2}, {ERROR_BAD_PATHNAME, 2}, {ERROR_MOD_NOT_FOUND, 2}, {ERROR_PROC_NOT_FOUND, 2},
    {ERROR_BAD_NET_NAME, 2}, {ERROR_NO_MORE_FILES, 2},
    {ERROR_LOCK_VIOLATION, 13}, {ERROR_CALL_NOT_IMPLEMENTED, 78}, {ERROR_COMMITMENT_LIMIT, 12},
    {ERROR_BUSY, 16}, {ERROR_PIPE_BUSY, 16},
    // exhausted memory or quota: ENOMEM; handles: EBADF
    {ERROR_ARENA_TRASHED, 12}, {ERROR_INVALID_BLOCK, 12}, {ERROR_NOT_ENOUGH_QUOTA, 12},
    {ERROR_INVALID_TARGET_HANDLE, 9}, {ERROR_DIRECT_ACCESS_HANDLE, 9},
};
constexpr Legacy crt_residuals[] = {
    {ERROR_ARENA_TRASHED, 12}, {ERROR_INVALID_BLOCK, 12}, {ERROR_NOT_ENOUGH_QUOTA, 12},
    {ERROR_NO_MORE_FILES, 2}, {ERROR_BAD_NET_NAME, 2},
    {ERROR_INVALID_TARGET_HANDLE, 9}, {ERROR_DIRECT_ACCESS_HANDLE, 9},
    // Kept: no descriptor call (read, write, lseek, dup, fsync, ftruncate,
    // chmod, access) fails with an exec-format or process-slot code, and
    // ERROR_BUSY shares the kind of a sharing violation (EACCES at these sites).
    {ERROR_INVALID_STARTING_CODESEG, 22}, {ERROR_INFLOOP_IN_RELOC_CHAIN, 22}, {ERROR_NO_PROC_SLOTS, 22}, {ERROR_MAX_THRDS_REACHED, 22},
    {ERROR_BUSY, 13},
};
constexpr Legacy socket_residuals[] = {
    {WSA_INVALID_HANDLE, 9}, {WSA_INVALID_PARAMETER, 22}, {WSA_OPERATION_ABORTED, 4}, {WSA_IO_PENDING, 35},
};
constexpr Legacy spawn_residuals[] = {
    {ERROR_NOT_ENOUGH_QUOTA, 12}, {ERROR_BAD_NET_NAME, 2}, {ERROR_INVALID_TARGET_HANDLE, 9},
};

void residuals() {
    namespace win32 = alchemy::host::win32;
    for (const auto& entry : file_residuals)
        require(darwin_file_errno(win32::error(entry.code)) == entry.value, "file residual: Win32 " + std::to_string(entry.code));
    for (const auto& entry : crt_residuals)
        require(darwin_crt_errno(win32::error(entry.code)) == entry.value, "C runtime residual: Win32 " + std::to_string(entry.code));
    for (const auto& entry : socket_residuals)
        require(darwin_socket_errno(win32::error(entry.code)) == entry.value, "socket residual: WinSock " + std::to_string(entry.code));
    for (const auto& entry : spawn_residuals)
        require(alchemy::libsystem::darwin_spawn_errno(win32::error(entry.code)) == entry.value, "spawn residual: Win32 " + std::to_string(entry.code));
    // utimes: a file that exists but whose times cannot be written is EIO,
    // never ENOENT (the site uses the file table).
    require(darwin_file_errno(Error::io) == 5 && darwin_file_errno(Error::other) == 5, "utimes after the open: EIO");
}

void legacy_tables() {
    namespace win32 = alchemy::host::win32;
    for (const auto& entry : file_table)
        require(darwin_file_errno(win32::error(entry.code)) == entry.value, "file table: Win32 " + std::to_string(entry.code));
    for (const auto& entry : crt_table)
        require(darwin_crt_errno(win32::error(entry.code)) == entry.value, "C runtime table: Win32 " + std::to_string(entry.code));
    for (const auto& entry : spawn_table)
        require(alchemy::libsystem::darwin_spawn_errno(win32::error(entry.code)) == entry.value, "spawn table: Win32 " + std::to_string(entry.code));
    for (const auto& entry : socket_table)
        require(darwin_socket_errno(win32::error(entry.code)) == entry.value, "socket table: WinSock " + std::to_string(entry.code));
}
#endif

} // namespace

int main() {
    try {
        // Every kind but none maps to a nonzero errno, and the kinds other than
        // the ones that share a Darwin value on purpose map to distinct values.
        std::set<std::uint32_t> seen;
        for (auto value = static_cast<unsigned>(Error::unsupported); value <= static_cast<unsigned>(Error::other); ++value) {
            const auto error = static_cast<Error>(value);
            const auto code = darwin_errno(error);
            require(code != 0, "no errno for " + std::string(alchemy::host::name(error)));
            const bool shared = error == Error::denied_by_policy || error == Error::other;
            if (!shared) require(seen.insert(code).second, "two kinds share errno " + std::to_string(code));
        }
#if defined(ALCHEMY_HOST_WIN32)
        legacy_tables();
        residuals();
#endif
        std::cout << "libSystem errno tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
