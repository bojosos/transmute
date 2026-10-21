#pragma once

// Host area "ipc" (owner: r1). The rules are in include/alchemy/host/host.hpp. The byte pipes and
// handle passing under the cross-process Mach transport, and pipes between Alchemy processes.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace alchemy::host::ipc {

// Local byte channels between the processes of this login session (named
// pipes on Windows, Unix-domain sockets on Linux), named by `name`. A server
// makes one listener per client: listen() (with `first`, failing with
// Error::exists when another process already serves the name), then
// accept() waits for a client and the listener becomes that connection.
// A connection reads and writes from two threads at once; cancel() makes
// both return. Close every handle with host::close.
[[nodiscard]] Result<Handle> listen(std::string_view name, bool first);
[[nodiscard]] Status accept(Handle listener);
// A connection to a server; Error::not_found when none listens, Error::busy
// when every listener stayed taken.
[[nodiscard]] Result<Handle> connect(std::string_view name);
// Bytes transferred (0: the peer closed the channel or the I/O was cancelled).
[[nodiscard]] Result<std::size_t> read(Handle channel, Bytes buffer);
[[nodiscard]] Result<std::size_t> write(Handle channel, ConstBytes data);
// Ends the pending and later I/O of `channel` (another thread's read, write or accept).
void cancel(Handle channel) noexcept;

} // namespace alchemy::host::ipc
